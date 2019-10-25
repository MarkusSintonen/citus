/*-------------------------------------------------------------------------
 *
 * shard_pruning.c
 *   Shard pruning related code.
 *
 * The goal of shard pruning is to find a minimal (super)set of shards that
 * need to be queried to find rows matching the expression in a query.
 *
 * In PruneShards, we first compute a simplified disjunctive normal form (DNF)
 * of the expression as a list of pruning instances. Each pruning instance
 * contains all AND-ed constraints on the partition column. An OR expression
 * will result in two or more new pruning instances being added for the
 * subexpressions. The "parent" instance is marked isPartial and ignored
 * during pruning.
 *
 * We use the distributive property for constraints of the form P AND (Q OR R)
 * to rewrite it to (P AND Q) OR (P AND R) by copying constraints from parent
 * to "child" pruning instances. However, we do not distribute nested
 * expressions. While (P OR Q) AND (R OR S) is logically equivalent to (P AND
 * R) OR (P AND S) OR (Q AND R) OR (Q AND S), in our implementation it becomes
 * P OR Q OR R OR S. This is acceptable since this will always result in a
 * superset of shards.  If this proves to be a issue in practice, a more
 * complete algorithm could be implemented.
 *
 * We then evaluate each non-partial pruning instance in the disjunction
 * through the following, increasingly expensive, steps:
 *
 * 1) If there is a constant equality constraint on the partition column, and
 *    no overlapping shards exist, find the shard interval in which the
 *    constant falls
 *
 * 2) If there is a hash range constraint on the partition column, find the
 *    shard interval matching the range
 *
 * 3) If there are range constraints (e.g. (a > 0 AND a < 10)) on the
 *    partition column, find the shard intervals that overlap with the range
 *
 * 4) If there are overlapping shards, exhaustively search all shards that are
 *    not excluded by constraints
 *
 * Finally, the union of the shards found by each pruning instance is
 * returned.
 *
 * Copyright (c) 2014-2017, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "distributed/shard_pruning.h"

#include "access/nbtree.h"
#include "catalog/pg_am.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "distributed/metadata_cache.h"
#include "distributed/distributed_planner.h"
#include "distributed/multi_join_order.h"
#include "distributed/multi_physical_planner.h"
#include "distributed/shardinterval_utils.h"
#include "distributed/pg_dist_partition.h"
#include "distributed/worker_protocol.h"
#include "nodes/nodeFuncs.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "utils/arrayaccess.h"
#include "utils/catcache.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

/*
 * A pruning instance is a set of ANDed constraints on a partition key.
 */
typedef struct PruningInstance
{
	/* Does this instance contain any prunable expressions? */
	bool hasValidConstraint;

	/*
	 * This constraint never evaluates to true, i.e. pruning does not have to
	 * be performed.
	 */
	bool evaluatesToFalse;

	/*
	 * Constraints on the partition column value.  If multiple values are
	 * found the more restrictive one should be stored here.  Even in case of
	 * a hash-partitioned table, actual column-values are stored here, *not*
	 * hashed values.
	 */
	Const *lessConsts;
	Const *lessEqualConsts;
	Const *equalConsts;
	Const *greaterEqualConsts;
	Const *greaterConsts;
	List *saoEqualConsts;

	/*
	 * Constraint using a pre-hashed column value. The constant will store the
	 * hashed value, not the original value of the restriction.
	 */
	Const *hashedEqualConsts;

	/*
	 * Types of constraints not understood.  We could theoretically try more
	 * expensive methods of pruning if any such restrictions are found.
	 *
	 * TODO: any actual use for this? Right now there seems little point.
	 */
	List *otherRestrictions;

	/*
	 * Has this PruningInstance been added to
	 * ClauseWalkerContext->pruningInstances? This is not done immediately,
	 * but the first time a constraint (independent of us being able to handle
	 * that constraint) is found.
	 */
	bool addedToPruningInstances;

	/*
	 * When OR clauses are found, the non-ORed part (think of a < 3 AND (a > 5
	 * OR a > 7)) of the expression is stored in one PruningInstance which is
	 * then copied for the ORed expressions.  The original is marked as
	 * isPartial, to avoid it being used for pruning.
	 */
	bool isPartial;
} PruningInstance;

typedef struct PruneNode
{
	BoolExprType boolOp;
	List *bools;
	List *conditions;
} PruneNode;

typedef struct ConditionWrapper
{
	List *validConstraints;
} ConditionWrapper;

/*
 * Partial instances that need to be finished building.  This is used to
 * collect all ANDed restrictions, before looking into ORed expressions.
 */
typedef struct PendingPruningInstance
{
	PruningInstance *instance;
	Node *continueAt;
	PruneNode *continueAtPruneNode;
} PendingPruningInstance;


/*
 * Data necessary to perform a single PruneShards().
 */
typedef struct ClauseWalkerContext
{
	Var *partitionColumn;
	char partitionMethod;

	/* ORed list of pruning targets */
	List *pruningInstances;

	/*
	 * Partially built PruningInstances, that need to be completed by doing a
	 * separate PrunableExpressionsWalker() pass.
	 */
	List *pendingInstances;

	/* PruningInstance currently being built, all elegible constraints are added here */
	PruningInstance *currentPruningInstance;

	/*
	 * Information about function calls we need to perform. Re-using the same
	 * FunctionCallInfoData, instead of using FunctionCall2Coll, is often
	 * cheaper.
	 */
	FunctionCallInfoData compareValueFunctionCall;
	FunctionCallInfoData compareIntervalFunctionCall;
} ClauseWalkerContext;

typedef struct PruneNodeContext
{
	Var *partitionColumn;
	PruneNode *pruneNode;
} PruneNodeContext;

bool LogShardPruning = false; /* print shard pruning information as a debugging aid */

static void PullUpBooleanOps(PruneNode *node, PruneNode *parent);
static void SeparateOrConditionsFromBools(PruneNode *node);
static void PrunableExpressions(Node *originalNode, ClauseWalkerContext *context);
static bool PrunableExpressionsWalker(Node *node, ClauseWalkerContext *context);
static void PrunableExpressionsWalker2(PruneNode *node, ClauseWalkerContext *context);
static void HandleConditionNode(Node *node, ClauseWalkerContext *context);
static void AddPartitionKeyRestrictionToInstance(ClauseWalkerContext *context,
												 OpExpr *opClause, Var *varClause,
												 Const *constantClause);
static bool IsValidConditionNode(Node *node, Var *partitionColumn);
static bool IsValidPartitionKeyRestriction(OpExpr *opClause);
static bool IsValidHashRestriction(OpExpr *opClause);
static bool IsValidSAOPartitionKeyRestriction(Var *partitionColumn,
												ScalarArrayOpExpr *arrayOperatorExpression);
static void AddSAOPartitionKeyRestrictionToInstance(ClauseWalkerContext *context,
													ScalarArrayOpExpr *
													arrayOperatorExpression);
static void AddSAOPartitionKeyRestrictionToInstance2(ClauseWalkerContext *context,
													ScalarArrayOpExpr *arrayOperatorExpression);
static void AddHashRestrictionToInstance(ClauseWalkerContext *context, OpExpr *opClause,
										 Var *varClause, Const *constantClause);
static void AddNewConjuction(ClauseWalkerContext *context, OpExpr *op);
static void AddNewConjuction2(ClauseWalkerContext *context, PruneNode *pruneNode);
static PruningInstance * CopyPartialPruningInstance(PruningInstance *sourceInstance);
static List * ShardArrayToList(ShardInterval **shardArray, int length);
static List * DeepCopyShardIntervalList(List *originalShardIntervalList);
static int PerformValueCompare(FunctionCallInfoData *compareFunctionCall, Datum a,
							   Datum b);
static int PerformCompare(FunctionCallInfoData *compareFunctionCall);

static List * PruneOne(DistTableCacheEntry *cacheEntry, ClauseWalkerContext *context,
					   PruningInstance *prune);
static List * PruneWithBoundaries(DistTableCacheEntry *cacheEntry,
								  ClauseWalkerContext *context,
								  PruningInstance *prune);
static List * ExhaustivePrune(DistTableCacheEntry *cacheEntry,
							  ClauseWalkerContext *context,
							  PruningInstance *prune);
static bool ExhaustivePruneOne(ShardInterval *curInterval,
							   ClauseWalkerContext *context,
							   PruningInstance *prune);
static int UpperShardBoundary(Datum partitionColumnValue,
							  ShardInterval **shardIntervalCache,
							  int shardCount, FunctionCallInfoData *compareFunction,
							  bool includeMin);
static int LowerShardBoundary(Datum partitionColumnValue,
							  ShardInterval **shardIntervalCache,
							  int shardCount, FunctionCallInfoData *compareFunction,
							  bool includeMax);

static bool BuildPruneTree(Node *node, PruneNodeContext *pruneNode);
static PruneNode* BooleanDistributeToORofANDs(PruneNode *pruneNode);
static void BooleanPrint(PruneNode *pruneNode);

#define DebugLog(errmsg) if (LogShardPruning) { ereport(DEBUG2, (errmsg)); }

/*
 * PruneShards returns all shards from a distributed table that cannot be
 * proven to be eliminated by whereClauseList.
 *
 * For reference tables, the function simply returns the single shard that the
 * table has.
 *
 * When there is a single <partition column> = <constant> filter in the where
 * clause list, the constant is written to the partitionValueConst pointer.
 */
List *
PruneShards(Oid relationId, Index rangeTableId, List *whereClauseList,
			Const **partitionValueConst)
{
	DistTableCacheEntry *cacheEntry = DistributedTableCacheEntry(relationId);
	int shardCount = cacheEntry->shardIntervalArrayLength;
	char partitionMethod = cacheEntry->partitionMethod;
	ClauseWalkerContext context = { 0 };
	ListCell *pruneCell;
	List *prunedList = NIL;
	bool foundRestriction = false;
	bool foundPartitionColumnValue = false;
	Const *singlePartitionValueConst = NULL;

	/* there are no shards to return */
	if (shardCount == 0)
	{
		return NIL;
	}

	/* always return empty result if WHERE clause is of the form: false (AND ..) */
	if (ContainsFalseClause(whereClauseList))
	{
		return NIL;
	}

	/* short circuit for reference tables */
	if (partitionMethod == DISTRIBUTE_BY_NONE)
	{
		prunedList = ShardArrayToList(cacheEntry->sortedShardIntervalArray,
									  cacheEntry->shardIntervalArrayLength);
		return DeepCopyShardIntervalList(prunedList);
	}


	context.partitionMethod = partitionMethod;
	context.partitionColumn = PartitionColumn(relationId, rangeTableId);
	context.currentPruningInstance = palloc0(sizeof(PruningInstance));

	if (cacheEntry->shardIntervalCompareFunction)
	{
		/* initiate function call info once (allows comparators to cache metadata) */
		InitFunctionCallInfoData(context.compareIntervalFunctionCall,
								 cacheEntry->shardIntervalCompareFunction,
								 2, DEFAULT_COLLATION_OID, NULL, NULL);
	}
	else
	{
		ereport(ERROR, (errmsg("shard pruning not possible without "
							   "a shard interval comparator")));
	}

	if (cacheEntry->shardColumnCompareFunction)
	{
		/* initiate function call info once (allows comparators to cache metadata) */
		InitFunctionCallInfoData(context.compareValueFunctionCall,
								 cacheEntry->shardColumnCompareFunction,
								 2, DEFAULT_COLLATION_OID, NULL, NULL);
	}
	else
	{
		ereport(ERROR, (errmsg("shard pruning not possible without "
							   "a partition column comparator")));
	}

	/* Build prune tree */
	PruneNode pruneNode = { 0 };
	pruneNode.boolOp = AND_EXPR;
	PruneNodeContext pruneNodeContext = { 0 };
	pruneNodeContext.partitionColumn = context.partitionColumn;
	pruneNodeContext.pruneNode = &pruneNode;
	DebugLog(errmsg("================"));
	BuildPruneTree((Node *) whereClauseList, &pruneNodeContext);
	BooleanPrint(&pruneNode);
	DebugLog(errmsg("--AFTER PULL UP--"));
	PullUpBooleanOps(&pruneNode, (PruneNode*)NULL);
	BooleanPrint(&pruneNode);
	DebugLog(errmsg("--AFTER SEPARATE OR--"));
	SeparateOrConditionsFromBools(&pruneNode);
	BooleanPrint(&pruneNode);
	DebugLog(errmsg("--AFTER DISTRIBUTION--"));
	PruneNode* pruneNode3 = BooleanDistributeToORofANDs(&pruneNode);
	BooleanPrint(pruneNode3);
	DebugLog(errmsg("================"));

	/* Figure out what we can prune on */
	//PrunableExpressions((Node *) whereClauseList, &context);

	/* Figure out what we can prune on */
	PrunableExpressionsWalker2(pruneNode3, &context);

	/*
	 * Prune using each of the PrunableInstances we found, and OR results
	 * together.
	 */
	foreach(pruneCell, context.pruningInstances)
	{
		PruningInstance *prune = (PruningInstance *) lfirst(pruneCell);
		List *pruneOneList;

		/*
		 * If this is a partial instance, a fully built one has also been
		 * added. Skip.
		 */
		if (prune->isPartial)
		{
			continue;
		}

		/*
		 * If the current instance has no prunable expressions, we'll have to
		 * return all shards.  No point in continuing pruning in that case.
		 */
		if (!prune->hasValidConstraint)
		{
			foundRestriction = false;
			break;
		}

		if (context.partitionMethod == DISTRIBUTE_BY_HASH)
		{
			if (!prune->evaluatesToFalse && !prune->equalConsts && !prune->saoEqualConsts &&
				!prune->hashedEqualConsts)
			{
				/* if hash-partitioned and no equals constraints, return all shards */
				foundRestriction = false;
				break;
			}
			else if (partitionValueConst != NULL && prune->equalConsts != NULL)
			{
				if (!foundPartitionColumnValue)
				{
					/* remember the partition column value */
					singlePartitionValueConst = prune->equalConsts;
					foundPartitionColumnValue = true;
				}
				else if (singlePartitionValueConst == NULL)
				{
					/* already found multiple partition column values */
				}
				else if (!equal(prune->equalConsts, singlePartitionValueConst))
				{
					/* found multiple partition column values */
					singlePartitionValueConst = NULL;
				}
			}
			else if (partitionValueConst != NULL && prune->saoEqualConsts != NULL)
			{
				ListCell *constCell = NULL;

				foreach(constCell, prune->saoEqualConsts)
				{
					Const *saoConst = (Const*)lfirst(constCell);

					if (!foundPartitionColumnValue)
					{
						/* remember the partition column value */
						singlePartitionValueConst = saoConst;
						foundPartitionColumnValue = true;
					}
					else if (singlePartitionValueConst == NULL)
					{
						/* already found multiple partition column values */
					}
					else if (!equal(saoConst, singlePartitionValueConst))
					{
						/* found multiple partition column values */
						singlePartitionValueConst = NULL;
					}
				}
			}
		}

		pruneOneList = PruneOne(cacheEntry, &context, prune);

		if (prunedList)
		{
			/*
			 * We can use list_union_ptr, which is a lot faster than doing
			 * comparing shards by value, because all the ShardIntervals are
			 * guaranteed to be from
			 * DistTableCacheEntry->sortedShardIntervalArray (thus having the
			 * same pointer values).
			 */
			prunedList = list_union_ptr(prunedList, pruneOneList);
		}
		else
		{
			prunedList = pruneOneList;
		}
		foundRestriction = true;
	}

	/* found no valid restriction, build list of all shards */
	if (!foundRestriction)
	{
		prunedList = ShardArrayToList(cacheEntry->sortedShardIntervalArray,
									  cacheEntry->shardIntervalArrayLength);
	}

	/* if requested, copy the partition value constant */
	if (partitionValueConst != NULL)
	{
		if (singlePartitionValueConst != NULL)
		{
			*partitionValueConst = copyObject(singlePartitionValueConst);
		}
		else
		{
			*partitionValueConst = NULL;
		}
	}

	/*
	 * Deep copy list, so it's independent of the DistTableCacheEntry
	 * contents.
	 */
	return DeepCopyShardIntervalList(prunedList);
}


/*
 * ContainsFalseClause returns whether the flattened where clause list
 * contains false as a clause.
 */
bool
ContainsFalseClause(List *whereClauseList)
{
	bool containsFalseClause = false;
	ListCell *clauseCell = NULL;

	foreach(clauseCell, whereClauseList)
	{
		Node *clause = (Node *) lfirst(clauseCell);

		if (IsA(clause, Const))
		{
			Const *constant = (Const *) clause;
			if (constant->consttype == BOOLOID && !DatumGetBool(constant->constvalue))
			{
				containsFalseClause = true;
				break;
			}
		}
	}

	return containsFalseClause;
}

/*
 * PrunableExpressions builds a list of all prunable expressions in node,
 * storing them in context->pruningInstances.
 */
static void
PrunableExpressions(Node *node, ClauseWalkerContext *context)
{
	/*
	 * Build initial list of prunable expressions.  As long as only,
	 * implicitly or explicitly, ANDed expressions are found, this perform a
	 * depth-first search.  When an ORed expression is found, the current
	 * PruningInstance is added to context->pruningInstances (once for each
	 * ORed expression), then the tree-traversal is continued without
	 * recursing.  Once at the top-level again, we'll process all pending
	 * expressions - that allows us to find all ANDed expressions, before
	 * recursing into an ORed expression.
	 */
	PrunableExpressionsWalker(node, context);

	/*
	 * Process all pending instances.  While processing, new ones might be
	 * added to the list, so don't use foreach().
	 *
	 * Check the places in PruningInstanceWalker that push onto
	 * context->pendingInstances why construction of the PruningInstance might
	 * be pending.
	 *
	 * We copy the partial PruningInstance, and continue adding information by
	 * calling PrunableExpressionsWalker() on the copy, continuing at the the
	 * node stored in PendingPruningInstance->continueAt.
	 */
	while (context->pendingInstances != NIL)
	{
		PendingPruningInstance *instance =
			(PendingPruningInstance *) linitial(context->pendingInstances);
		PruningInstance *newPrune = CopyPartialPruningInstance(instance->instance);

		context->pendingInstances = list_delete_first(context->pendingInstances);

		context->currentPruningInstance = newPrune;
		PrunableExpressionsWalker(instance->continueAt, context);
		context->currentPruningInstance = NULL;
	}
}

static ConditionWrapper* FindValidConditionWrapper(List *conditions)
{
	ListCell *cell = NULL;

	foreach(cell, conditions)
	{
		ConditionWrapper *conditionWrapper = lfirst(cell);
		if (conditionWrapper->validConstraints)
		{
			return conditionWrapper;
		}
	}

	return NULL;
}

static ConditionWrapper* FindInvalidConditionWrapper(List *conditions)
{
	ListCell *cell = NULL;

	foreach(cell, conditions)
	{
		ConditionWrapper *conditionWrapper = lfirst(cell);
		if (!conditionWrapper->validConstraints)
		{
			return conditionWrapper;
		}
	}

	return NULL;
}

static List* AppendConstraint(List *conditions, Node *validConstraint)
{
	ConditionWrapper *invalidConditionWrapper = FindInvalidConditionWrapper(conditions);
	ConditionWrapper *validConditionWrapper = FindValidConditionWrapper(conditions);

	if (invalidConditionWrapper && validConditionWrapper)
	{
		if (validConstraint)
		{
			validConditionWrapper->validConstraints = lappend(validConditionWrapper->validConstraints, validConstraint);
		}

		return conditions;
	}
	else if (invalidConditionWrapper)
	{
		if (validConstraint)
		{
			ConditionWrapper *conditionWrapper = palloc0(sizeof(ConditionWrapper));
			conditionWrapper->validConstraints = lappend(conditionWrapper->validConstraints, validConstraint);
			conditions = lappend(conditions, conditionWrapper);
		}

		return conditions;
	}
	else if (validConditionWrapper)
	{
		if (validConstraint)
		{
			validConditionWrapper->validConstraints = lappend(validConditionWrapper->validConstraints, validConstraint);
		}
		else
		{
			ConditionWrapper *conditionWrapper = palloc0(sizeof(ConditionWrapper));
			conditionWrapper->validConstraints = NULL;
			conditions = lappend(conditions, conditionWrapper);
		}

		return conditions;
	}
	else
	{
		ConditionWrapper *conditionWrapper = palloc0(sizeof(ConditionWrapper));

		if (validConstraint)
		{
			conditionWrapper->validConstraints = lappend(conditionWrapper->validConstraints, validConstraint);
		}
		else
		{
			conditionWrapper->validConstraints = NULL;
		}

		return lappend(conditions, conditionWrapper);
	}
}

static bool BuildPruneTree(Node *node, PruneNodeContext *context)
{
	if (node == NULL)
	{
		return false;
	}

	/*
	 * Check for boolean expressions and their args.
	 */
	if (IsA(node, List))
	{
		/* at the top of quals we'll frequently see lists, those are to be treated as ANDs */
	}
	else if (IsA(node, BoolExpr))
	{
		BoolExpr *boolExpr = (BoolExpr *) node;

		if (boolExpr->boolop == NOT_EXPR)
		{
			return false;
		}
		else if (boolExpr->boolop != context->pruneNode->boolOp)
		{
			PruneNode *newNode = palloc0(sizeof(PruneNode));
			newNode->boolOp = boolExpr->boolop;

			context->pruneNode->bools = lappend(context->pruneNode->bools, newNode);

			PruneNodeContext newContext = { 0 };
			newContext.partitionColumn = context->partitionColumn;
			newContext.pruneNode = newNode;

			return expression_tree_walker((Node *) boolExpr->args,
					BuildPruneTree, &newContext);
		}
	}
	else if (IsA(node, OpExpr) || IsA(node, ScalarArrayOpExpr))
	{
		if (context->pruneNode->boolOp == AND_EXPR)
		{
			if (IsValidConditionNode(node, context->partitionColumn))
			{
				context->pruneNode->conditions = AppendConstraint(context->pruneNode->conditions, node);
			}
			else
			{
				context->pruneNode->conditions = AppendConstraint(context->pruneNode->conditions, NULL);
			}
		}
		else
		{
			if (IsValidConditionNode(node, context->partitionColumn))
			{
				ConditionWrapper *conditionWrapper = palloc0(sizeof(ConditionWrapper));
				conditionWrapper->validConstraints = lappend(conditionWrapper->validConstraints, node);

				context->pruneNode->conditions = lappend(context->pruneNode->conditions, conditionWrapper);
			}
			else
			{
				context->pruneNode->conditions = AppendConstraint(context->pruneNode->conditions, NULL);
			}
		}

		return false;
	}
	else
	{
		return false;
	}

	return expression_tree_walker(node, BuildPruneTree, context);
}

static bool IsPullUpBooleanOp(PruneNode *node)
{
	if (!node)
	{
		return false;
	}

	return (list_length(node->bools) + list_length(node->conditions)) < 2;
}

static void PullUpBooleanOps(PruneNode *node, PruneNode *parent)
{
	ListCell *cell = NULL;

	if (node->bools)
	{
		foreach(cell, node->bools)
		{
			PullUpBooleanOps((PruneNode *)lfirst(cell), node);
		}
	}

	if (IsPullUpBooleanOp(parent))
	{
		parent->boolOp = node->boolOp;
		parent->bools = node->bools;
		parent->conditions = node->conditions;
	}
}

static void SeparateOrConditionsFromBools(PruneNode *node)
{
	ListCell *cell = NULL;

	if (node->bools)
	{
		foreach(cell, node->bools)
		{
			SeparateOrConditionsFromBools((PruneNode *)lfirst(cell));
		}
	}

	if (node->boolOp == OR_EXPR && node->conditions)
	{
		foreach(cell, node->conditions)
		{
			PruneNode *newAnd = palloc0(sizeof(PruneNode));
			newAnd->boolOp = AND_EXPR;
			newAnd->conditions = lappend(newAnd->conditions, lfirst(cell));

			node->bools = lappend(node->bools, newAnd);
		}

		node->conditions = NULL;
	}
}

static List* MakeORofAnds(List *otherOrs, List *orConds, List *andConds)
{
	if (!otherOrs && !andConds)
	{
		return NULL;
	}

	List *bools = NULL;
	ListCell *cell = NULL;
	ListCell *cell2 = NULL;
	ListCell *cell3 = NULL;

	if (!otherOrs)
	{
		foreach(cell, orConds)
		{
			ConditionWrapper *cond = lfirst(cell);

			PruneNode *newAnd = palloc0(sizeof(PruneNode));
			newAnd->boolOp = AND_EXPR;

			newAnd->conditions = list_copy(andConds);
			newAnd->conditions = lappend(newAnd->conditions, cond);

			bools = lappend(bools, newAnd);
		}

		return bools;
	}

	foreach(cell, orConds)
	{
		ConditionWrapper *cond = lfirst(cell);

		foreach(cell2, otherOrs)
		{
			PruneNode *otherOr = (PruneNode *)lfirst(cell2);

			foreach(cell3, otherOr->conditions)
			{
				ConditionWrapper *cond2 = lfirst(cell3);

				PruneNode *newAnd = palloc0(sizeof(PruneNode));
				newAnd->boolOp = AND_EXPR;

				if (andConds)
				{
					newAnd->conditions = list_copy(andConds);
				}
				newAnd->conditions = lappend(newAnd->conditions, cond);
				newAnd->conditions = lappend(newAnd->conditions, cond2);

				bools = lappend(bools, newAnd);
			}
		}
	}

	return bools;
}

static bool ConstainsORLeaf(PruneNode *node)
{
	if (node->boolOp == OR_EXPR && !node->bools)
	{
		return true;
	}

	ListCell *cell = NULL;

	foreach(cell, node->bools)
	{
		PruneNode *child = lfirst(cell);

		if (ConstainsORLeaf(child))
		{
			return true;
		}
	}

	return false;
}

static PruneNode* OneBooleanDistributeToORofANDs(PruneNode *node)
{
	ListCell *cell = NULL;

	PruneNode *andNode = node->boolOp == AND_EXPR ? node : NULL;
	PruneNode *orNode = node->boolOp == OR_EXPR ? node : NULL;

	if (orNode)
	{
		Assert(!orNode->conditions);

		if (orNode->bools)
		{
			List *asd = NULL;

			foreach(cell, orNode->bools)
			{
				andNode = lfirst(cell);

				asd = list_concat(asd, OneBooleanDistributeToORofANDs(andNode)->bools);
			}

			orNode->bools = asd;

			return orNode;
		}
		else
		{
			return orNode;
		}
	}

	PruneNode *newOr = palloc0(sizeof(PruneNode));
	newOr->boolOp = OR_EXPR;

	if (andNode->bools)
	{
		/* Iterate OR nodes under the AND node distributing them */

		foreach(cell, andNode->bools)
		{
			PruneNode *orNode = (PruneNode*)lfirst(cell);

			newOr->bools = MakeORofAnds(list_delete_ptr(andNode->bools, orNode), orNode->conditions, andNode->conditions);
			newOr->bools = list_concat(newOr->bools, orNode->bools);
		}

		return newOr;
	}
	else if (andNode->conditions)
	{
		newOr->bools = lappend(newOr->bools, andNode);

		return newOr;
	}
	else
	{
		return newOr;
	}
}

static PruneNode* BooleanDistributeToORofANDs(PruneNode *node)
{
	bool needsContinue = false;
	do
	{
		node = OneBooleanDistributeToORofANDs(node);
		needsContinue = node->boolOp == AND_EXPR && ConstainsORLeaf(node);
	} while(false);
	return node;
}

static void BooleanPrint2(PruneNode *node, int depth)
{
	if (!node)
	{
		return;
	}

	int numValid = 0;
	int numInvalid = 0;
	ListCell *cell = NULL;

	foreach(cell, node->conditions)
	{
		ConditionWrapper *conditionWrapper = lfirst(cell);
		if (conditionWrapper->validConstraints)
		{
			numValid += list_length(conditionWrapper->validConstraints);
		}
		else
		{
			numInvalid++;
		}
	}

	char *op = node->boolOp == AND_EXPR ? "AND" : "OR ";
	char *str = palloc0(100);

	sprintf(str, "%*s %s valid_conds=%d invalid_conds=%s", depth, "", op, numValid, numInvalid > 0 ? "TRUE" : "FALSE");
	DebugLog(errmsg(str));

	foreach(cell, node->bools)
	{
		PruneNode *boolNode = lfirst(cell);
		BooleanPrint2(boolNode, depth + 1);
	}
}

static void BooleanPrint(PruneNode *pruneNode)
{
	BooleanPrint2(pruneNode, 0);
}

static void
PrunableExpressionsWalker2(PruneNode *node, ClauseWalkerContext *context)
{
	if (node == NULL)
	{
		return;
	}

	ListCell *cell = NULL;
	ListCell *cell2 = NULL;

	if (node->boolOp == AND_EXPR)
	{
		foreach(cell, node->bools)
		{
			PruneNode *boolNode = lfirst(cell);
			PrunableExpressionsWalker2(boolNode, context);
		}

		ConditionWrapper *validCondWrapper = FindValidConditionWrapper(node->conditions);
		ConditionWrapper *invalidCondWrapper = FindInvalidConditionWrapper(node->conditions);

		if (invalidCondWrapper && !validCondWrapper)
		{
			PruningInstance *newInstance = palloc0(sizeof(PruningInstance));
			newInstance->addedToPruningInstances = true;
			newInstance->isPartial = false;
			newInstance->hasValidConstraint = false;

			context->pruningInstances = lappend(context->pruningInstances, newInstance);
		}
		else if (validCondWrapper)
		{
			PruningInstance *newInstance = palloc0(sizeof(PruningInstance));
			newInstance->addedToPruningInstances = false;
			newInstance->isPartial = false;

			context->currentPruningInstance = newInstance;

			foreach(cell, node->conditions)
			{
				ConditionWrapper *conditionWrapper = lfirst(cell);

				foreach(cell2, conditionWrapper->validConstraints)
				{
					HandleConditionNode((Node*)lfirst(cell2), context);
				}
			}
		}
	}
	else
	{
		Assert(node->boolOp == OR_EXPR);
		Assert(!node->conditions);

		foreach(cell, node->bools)
		{
			PruneNode *boolNode = lfirst(cell);
			PrunableExpressionsWalker2(boolNode, context);
		}
	}
}

static bool
IsValidConditionNode(Node *node, Var *partitionColumn)
{
	if (IsA(node, OpExpr))
	{
		OpExpr *opClause = (OpExpr *) node;
		Node *leftOperand = NULL;
		Node *rightOperand = NULL;
		Const *constantClause = NULL;
		Var *varClause = NULL;

		if (list_length(opClause->args) == 2)
		{
			leftOperand = get_leftop((Expr *) opClause);
			rightOperand = get_rightop((Expr *) opClause);

			leftOperand = strip_implicit_coercions(leftOperand);
			rightOperand = strip_implicit_coercions(rightOperand);

			if (IsA(rightOperand, Const) && IsA(leftOperand, Var))
			{
				constantClause = (Const *) rightOperand;
				varClause = (Var *) leftOperand;
			}
			else if (IsA(leftOperand, Const) && IsA(rightOperand, Var))
			{
				constantClause = (Const *) leftOperand;
				varClause = (Var *) rightOperand;
			}
		}

		if (constantClause && varClause && equal(varClause, partitionColumn))
		{
			return IsValidPartitionKeyRestriction(opClause);
		}
		else if (constantClause && varClause &&
				 varClause->varattno == RESERVED_HASHED_COLUMN_ID)
		{
			return IsValidHashRestriction(opClause);
		}

		return false;
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *arrayOperatorExpression = (ScalarArrayOpExpr *) node;
		return IsValidSAOPartitionKeyRestriction(partitionColumn, arrayOperatorExpression);
	}
	else
	{
		return false;
	}
}

static void
HandleConditionNode(Node *node, ClauseWalkerContext *context)
{
	if (IsA(node, OpExpr))
	{
		OpExpr *opClause = (OpExpr *) node;
		PruningInstance *prune = context->currentPruningInstance;
		Node *leftOperand = NULL;
		Node *rightOperand = NULL;
		Const *constantClause = NULL;
		Var *varClause = NULL;

		if (!prune->addedToPruningInstances)
		{
			context->pruningInstances = lappend(context->pruningInstances,
												prune);
			prune->addedToPruningInstances = true;
		}

		if (list_length(opClause->args) == 2)
		{
			leftOperand = get_leftop((Expr *) opClause);
			rightOperand = get_rightop((Expr *) opClause);

			leftOperand = strip_implicit_coercions(leftOperand);
			rightOperand = strip_implicit_coercions(rightOperand);

			if (IsA(rightOperand, Const) && IsA(leftOperand, Var))
			{
				constantClause = (Const *) rightOperand;
				varClause = (Var *) leftOperand;
			}
			else if (IsA(leftOperand, Const) && IsA(rightOperand, Var))
			{
				constantClause = (Const *) leftOperand;
				varClause = (Var *) rightOperand;
			}
		}

		if (constantClause && varClause && equal(varClause, context->partitionColumn))
		{
			/*
			 * Found a restriction on the partition column itself. Update the
			 * current constraint with the new information.
			 */
			AddPartitionKeyRestrictionToInstance(context,
												 opClause, varClause, constantClause);
		}
		else if (constantClause && varClause &&
				 varClause->varattno == RESERVED_HASHED_COLUMN_ID)
		{
			/*
			 * Found restriction that directly specifies the boundaries of a
			 * hashed column.
			 */
			AddHashRestrictionToInstance(context, opClause, varClause, constantClause);
		}
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		PruningInstance *prune = context->currentPruningInstance;

		if (!prune->addedToPruningInstances)
		{
			context->pruningInstances = lappend(context->pruningInstances,
												prune);
			prune->addedToPruningInstances = true;
		}

		ScalarArrayOpExpr *arrayOperatorExpression = (ScalarArrayOpExpr *) node;
		AddSAOPartitionKeyRestrictionToInstance2(context, arrayOperatorExpression);
	}
	else
	{
		PruningInstance *prune = context->currentPruningInstance;

		/*
		 * Mark expression as added, so we'll fail pruning if there's no ANDed
		 * restrictions that we know how to deal with.
		 */
		if (!prune->addedToPruningInstances)
		{
			context->pruningInstances = lappend(context->pruningInstances,
												prune);
			prune->addedToPruningInstances = true;
		}
	}
}

/*
 * PrunableExpressionsWalker() is the main work horse for
 * PrunableExpressions().
 */
static bool
PrunableExpressionsWalker(Node *node, ClauseWalkerContext *context)
{
	if (node == NULL)
	{
		return false;
	}

	/*
	 * Check for expressions understood by this routine.
	 */
	if (IsA(node, List))
	{
		/* at the top of quals we'll frequently see lists, those are to be treated as ANDs */
	}
	else if (IsA(node, BoolExpr))
	{
		BoolExpr *boolExpr = (BoolExpr *) node;

		if (boolExpr->boolop == NOT_EXPR)
		{
			return false;
		}
		else if (boolExpr->boolop == AND_EXPR)
		{
			return expression_tree_walker((Node *) boolExpr->args,
										  PrunableExpressionsWalker, context);
		}
		else if (boolExpr->boolop == OR_EXPR)
		{
			ListCell *opCell = NULL;

			/*
			 * "Queue" partial pruning instances.  This is used to convert
			 * expressions like (A AND (B OR C) AND D) into (A AND B AND D),
			 * (A AND C AND D), with A, B, C, D being restrictions.  When the
			 * OR is encountered, a reference to the partially built
			 * PruningInstance (containing A at this point), is added to
			 * context->pendingInstances once for B and once for C.  Once a
			 * full tree-walk completed, PrunableExpressions() will complete
			 * the pending instances, which'll now also know about restriction
			 * D, by calling PrunableExpressionsWalker() once for B and once
			 * for C.
			 */
			foreach(opCell, boolExpr->args)
			{
				AddNewConjuction(context, lfirst(opCell));
			}

			return false;
		}
	}
	else if (IsA(node, OpExpr))
	{
		OpExpr *opClause = (OpExpr *) node;
		PruningInstance *prune = context->currentPruningInstance;
		Node *leftOperand = NULL;
		Node *rightOperand = NULL;
		Const *constantClause = NULL;
		Var *varClause = NULL;

		if (!prune->addedToPruningInstances)
		{
			context->pruningInstances = lappend(context->pruningInstances,
												prune);
			prune->addedToPruningInstances = true;
		}

		if (list_length(opClause->args) == 2)
		{
			leftOperand = get_leftop((Expr *) opClause);
			rightOperand = get_rightop((Expr *) opClause);

			leftOperand = strip_implicit_coercions(leftOperand);
			rightOperand = strip_implicit_coercions(rightOperand);

			if (IsA(rightOperand, Const) && IsA(leftOperand, Var))
			{
				constantClause = (Const *) rightOperand;
				varClause = (Var *) leftOperand;
			}
			else if (IsA(leftOperand, Const) && IsA(rightOperand, Var))
			{
				constantClause = (Const *) leftOperand;
				varClause = (Var *) rightOperand;
			}
		}

		if (constantClause && varClause && equal(varClause, context->partitionColumn))
		{
			/*
			 * Found a restriction on the partition column itself. Update the
			 * current constraint with the new information.
			 */
			AddPartitionKeyRestrictionToInstance(context,
												 opClause, varClause, constantClause);
		}
		else if (constantClause && varClause &&
				 varClause->varattno == RESERVED_HASHED_COLUMN_ID)
		{
			/*
			 * Found restriction that directly specifies the boundaries of a
			 * hashed column.
			 */
			AddHashRestrictionToInstance(context, opClause, varClause, constantClause);
		}

		return false;
	}
	else if (IsA(node, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *arrayOperatorExpression = (ScalarArrayOpExpr *) node;
		AddSAOPartitionKeyRestrictionToInstance(context, arrayOperatorExpression);

		return false;
	}
	else
	{
		PruningInstance *prune = context->currentPruningInstance;

		/*
		 * Mark expression as added, so we'll fail pruning if there's no ANDed
		 * restrictions that we know how to deal with.
		 */
		if (!prune->addedToPruningInstances)
		{
			context->pruningInstances = lappend(context->pruningInstances,
												prune);
			prune->addedToPruningInstances = true;
		}

		return false;
	}

	return expression_tree_walker(node, PrunableExpressionsWalker, context);
}


static bool
IsValidSAOPartitionKeyRestriction(Var *partitionColumn, ScalarArrayOpExpr *arrayOperatorExpression)
{
	Node *leftOpExpression = linitial(arrayOperatorExpression->args);
	Node *strippedLeftOpExpression = strip_implicit_coercions(leftOpExpression);
	bool usingEqualityOperator = OperatorImplementsEquality(
		arrayOperatorExpression->opno);
	Expr *arrayArgument = (Expr *) lsecond(arrayOperatorExpression->args);

	/* checking for partcol = ANY(const, value, s); or partcol IN (const,b,c); */
	if (usingEqualityOperator && strippedLeftOpExpression != NULL &&
		equal(strippedLeftOpExpression, partitionColumn) &&
		IsA(arrayArgument, Const))
	{
		Datum inArray = ((Const *) arrayArgument)->constvalue;

		/* check for the NULL right-hand expression*/
		if (inArray == 0)
		{
			return false;
		}

		return true;
	}

	return false;
}


static void
AddSAOPartitionKeyRestrictionToInstance2(ClauseWalkerContext *context,
										ScalarArrayOpExpr *arrayOperatorExpression)
{
	PruningInstance *prune = context->currentPruningInstance;
	Node *leftOpExpression = linitial(arrayOperatorExpression->args);
	Node *strippedLeftOpExpression = strip_implicit_coercions(leftOpExpression);
	bool usingEqualityOperator = OperatorImplementsEquality(
		arrayOperatorExpression->opno);
	Expr *arrayArgument = (Expr *) lsecond(arrayOperatorExpression->args);

	/* checking for partcol = ANY(const, value, s); or partcol IN (const,b,c); */
	if (usingEqualityOperator && strippedLeftOpExpression != NULL &&
		equal(strippedLeftOpExpression, context->partitionColumn) &&
		IsA(arrayArgument, Const))
	{
		ArrayType *array = NULL;
		int16 typlen = 0;
		bool typbyval = false;
		char typalign = '\0';
		Oid elementType = 0;
		ArrayIterator arrayIterator = NULL;
		Datum arrayElement = 0;
		Datum inArray = ((Const *) arrayArgument)->constvalue;
		bool isNull = false;

		/* check for the NULL right-hand expression*/
		if (inArray == 0)
		{
			return;
		}

		array = DatumGetArrayTypeP(((Const *) arrayArgument)->constvalue);

		/* get the necessary information from array type to iterate over it */
		elementType = ARR_ELEMTYPE(array);
		get_typlenbyvalalign(elementType,
							 &typlen,
							 &typbyval,
							 &typalign);

		/* Iterate over the righthand array of expression */
		arrayIterator = array_create_iterator(array, 0, NULL);
		while (array_iterate(arrayIterator, &arrayElement, &isNull))
		{
			Const *constElement = makeConst(elementType, -1,
											DEFAULT_COLLATION_OID, typlen, arrayElement,
											isNull, typbyval);
			prune->saoEqualConsts = lappend(prune->saoEqualConsts, constElement);

			prune->hasValidConstraint = true;
		}
	}

	/* Since we could not deal with the constraint, add the pruning instance to
	 * pruning instance list and labeled it as added.
	 */
	else if (!prune->addedToPruningInstances)
	{
		context->pruningInstances = lappend(context->pruningInstances, prune);
		prune->addedToPruningInstances = true;
	}
}


/*
 * AddSAOPartitionKeyRestrictionToInstance adds partcol = arrayelem operator
 * restriction to the current pruning instance for each element of the array. These
 * restrictions are added to pruning instance to prune shards based on IN/=ANY
 * constraints.
 */
static void
AddSAOPartitionKeyRestrictionToInstance(ClauseWalkerContext *context,
										ScalarArrayOpExpr *arrayOperatorExpression)
{
	PruningInstance *prune = context->currentPruningInstance;
	Node *leftOpExpression = linitial(arrayOperatorExpression->args);
	Node *strippedLeftOpExpression = strip_implicit_coercions(leftOpExpression);
	bool usingEqualityOperator = OperatorImplementsEquality(
		arrayOperatorExpression->opno);
	Expr *arrayArgument = (Expr *) lsecond(arrayOperatorExpression->args);

	/* checking for partcol = ANY(const, value, s); or partcol IN (const,b,c); */
	if (usingEqualityOperator && strippedLeftOpExpression != NULL &&
		equal(strippedLeftOpExpression, context->partitionColumn) &&
		IsA(arrayArgument, Const))
	{
		ArrayType *array = NULL;
		int16 typlen = 0;
		bool typbyval = false;
		char typalign = '\0';
		Oid elementType = 0;
		ArrayIterator arrayIterator = NULL;
		Datum arrayElement = 0;
		Datum inArray = ((Const *) arrayArgument)->constvalue;
		bool isNull = false;

		/* check for the NULL right-hand expression*/
		if (inArray == 0)
		{
			return;
		}

		array = DatumGetArrayTypeP(((Const *) arrayArgument)->constvalue);

		/* get the necessary information from array type to iterate over it */
		elementType = ARR_ELEMTYPE(array);
		get_typlenbyvalalign(elementType,
							 &typlen,
							 &typbyval,
							 &typalign);

		/* Iterate over the righthand array of expression */
		arrayIterator = array_create_iterator(array, 0, NULL);
		while (array_iterate(arrayIterator, &arrayElement, &isNull))
		{
			OpExpr *arrayEqualityOp = NULL;
			Const *constElement = makeConst(elementType, -1,
											DEFAULT_COLLATION_OID, typlen, arrayElement,
											isNull, typbyval);

			/* build partcol = arrayelem operator */
			arrayEqualityOp = makeNode(OpExpr);
			arrayEqualityOp->opno = arrayOperatorExpression->opno;
			arrayEqualityOp->opfuncid = arrayOperatorExpression->opfuncid;
			arrayEqualityOp->inputcollid = arrayOperatorExpression->inputcollid;
			arrayEqualityOp->opresulttype = get_func_rettype(
				arrayOperatorExpression->opfuncid);
			arrayEqualityOp->opcollid = DEFAULT_COLLATION_OID;
			arrayEqualityOp->location = -1;
			arrayEqualityOp->args = list_make2(strippedLeftOpExpression, constElement);

			AddNewConjuction(context, arrayEqualityOp);
		}
	}

	/* Since we could not deal with the constraint, add the pruning instance to
	 * pruning instance list and labeled it as added.
	 */
	else if (!prune->addedToPruningInstances)
	{
		context->pruningInstances = lappend(context->pruningInstances, prune);
		prune->addedToPruningInstances = true;
	}
}


/*
 * AddNewConjuction adds the OpExpr to pending instance list of context
 * as conjunction as partial instance.
 */
static void
AddNewConjuction(ClauseWalkerContext *context, OpExpr *op)
{
	PendingPruningInstance *instance = palloc0(sizeof(PendingPruningInstance));

	instance->instance = context->currentPruningInstance;
	instance->continueAt = (Node *) op;

	/*
	 * Signal that this instance is not to be used for pruning on
	 * its own.  Once the pending instance is processed, it'll be
	 * used.
	 */
	instance->instance->isPartial = true;
	context->pendingInstances = lappend(context->pendingInstances, instance);
}

static void
AddNewConjuction2(ClauseWalkerContext *context, PruneNode *pruneNode)
{
	PendingPruningInstance *instance = palloc0(sizeof(PendingPruningInstance));

	instance->instance = context->currentPruningInstance;
	instance->continueAtPruneNode = pruneNode;

	/*
	 * Signal that this instance is not to be used for pruning on
	 * its own.  Once the pending instance is processed, it'll be
	 * used.
	 */
	instance->instance->isPartial = true;
	context->pendingInstances = lappend(context->pendingInstances, instance);
}


static bool
IsValidPartitionKeyRestriction(OpExpr *opClause)
{
	List *btreeInterpretationList = NULL;
	ListCell *btreeInterpretationCell = NULL;
	bool matchedOp = false;

	btreeInterpretationList =
		get_op_btree_interpretation(opClause->opno);
	foreach(btreeInterpretationCell, btreeInterpretationList)
	{
		OpBtreeInterpretation *btreeInterpretation =
			(OpBtreeInterpretation *) lfirst(btreeInterpretationCell);

		if (btreeInterpretation->strategy == ROWCOMPARE_NE)
		{
			return false;
		}

		matchedOp = true;
	}

	return matchedOp;
}


/*
 * AddPartitionKeyRestrictionToInstance adds information about a PartitionKey
 * $op Const restriction to the current pruning instance.
 */
static void
AddPartitionKeyRestrictionToInstance(ClauseWalkerContext *context, OpExpr *opClause,
									 Var *varClause, Const *constantClause)
{
	PruningInstance *prune = context->currentPruningInstance;
	List *btreeInterpretationList = NULL;
	ListCell *btreeInterpretationCell = NULL;
	bool matchedOp = false;

	btreeInterpretationList =
		get_op_btree_interpretation(opClause->opno);
	foreach(btreeInterpretationCell, btreeInterpretationList)
	{
		OpBtreeInterpretation *btreeInterpretation =
			(OpBtreeInterpretation *) lfirst(btreeInterpretationCell);

		switch (btreeInterpretation->strategy)
		{
			case BTLessStrategyNumber:
			{
				if (!prune->lessConsts ||
					PerformValueCompare(&context->compareValueFunctionCall,
										constantClause->constvalue,
										prune->lessConsts->constvalue) < 0)
				{
					prune->lessConsts = constantClause;
				}
				matchedOp = true;
				break;
			}

			case BTLessEqualStrategyNumber:
			{
				if (!prune->lessEqualConsts ||
					PerformValueCompare(&context->compareValueFunctionCall,
										constantClause->constvalue,
										prune->lessEqualConsts->constvalue) < 0)
				{
					prune->lessEqualConsts = constantClause;
				}
				matchedOp = true;
				break;
			}

			case BTEqualStrategyNumber:
			{
				if (!prune->equalConsts)
				{
					prune->equalConsts = constantClause;
				}
				else if (PerformValueCompare(&context->compareValueFunctionCall,
											 constantClause->constvalue,
											 prune->equalConsts->constvalue) != 0)
				{
					/* key can't be equal to two values */
					prune->evaluatesToFalse = true;
				}
				matchedOp = true;
				break;
			}

			case BTGreaterEqualStrategyNumber:
			{
				if (!prune->greaterEqualConsts ||
					PerformValueCompare(&context->compareValueFunctionCall,
										constantClause->constvalue,
										prune->greaterEqualConsts->constvalue) > 0
					)
				{
					prune->greaterEqualConsts = constantClause;
				}
				matchedOp = true;
				break;
			}

			case BTGreaterStrategyNumber:
			{
				if (!prune->greaterConsts ||
					PerformValueCompare(&context->compareValueFunctionCall,
										constantClause->constvalue,
										prune->greaterConsts->constvalue) > 0)
				{
					prune->greaterConsts = constantClause;
				}
				matchedOp = true;
				break;
			}

			case ROWCOMPARE_NE:
			{
				/* TODO: could add support for this, if we feel like it */
				matchedOp = false;
				break;
			}

			default:
				Assert(false);
		}
	}

	if (!matchedOp)
	{
		prune->otherRestrictions =
			lappend(prune->otherRestrictions, opClause);
	}
	else
	{
		prune->hasValidConstraint = true;
	}
}


static bool
IsValidHashRestriction(OpExpr *opClause)
{
	List *btreeInterpretationList = NULL;
	ListCell *btreeInterpretationCell = NULL;

	btreeInterpretationList =
		get_op_btree_interpretation(opClause->opno);
	foreach(btreeInterpretationCell, btreeInterpretationList)
	{
		OpBtreeInterpretation *btreeInterpretation =
			(OpBtreeInterpretation *) lfirst(btreeInterpretationCell);

		if (btreeInterpretation->strategy == BTGreaterEqualStrategyNumber)
		{
			return true;
		}
	}

	return false;
}


/*
 * AddHashRestrictionToInstance adds information about a
 * RESERVED_HASHED_COLUMN_ID = Const restriction to the current pruning
 * instance.
 */
static void
AddHashRestrictionToInstance(ClauseWalkerContext *context, OpExpr *opClause,
							 Var *varClause, Const *constantClause)
{
	PruningInstance *prune = context->currentPruningInstance;
	List *btreeInterpretationList = NULL;
	ListCell *btreeInterpretationCell = NULL;

	btreeInterpretationList =
		get_op_btree_interpretation(opClause->opno);
	foreach(btreeInterpretationCell, btreeInterpretationList)
	{
		OpBtreeInterpretation *btreeInterpretation =
			(OpBtreeInterpretation *) lfirst(btreeInterpretationCell);

		/*
		 * Ladidadida, dirty hackety hack.  We only add such
		 * constraints (in ShardIntervalOpExpressions()) to select a
		 * shard based on its exact boundaries.  For efficient binary
		 * search it's better to simply use one representative value
		 * to look up the shard.  In practice, this is sufficient for
		 * now.
		 */
		if (btreeInterpretation->strategy == BTGreaterEqualStrategyNumber)
		{
			Assert(!prune->hashedEqualConsts);
			prune->hashedEqualConsts = constantClause;
			prune->hasValidConstraint = true;
		}
	}
}


/*
 * CopyPartialPruningInstance copies a partial PruningInstance, so it can be
 * completed.
 */
static PruningInstance *
CopyPartialPruningInstance(PruningInstance *sourceInstance)
{
	PruningInstance *newInstance = palloc(sizeof(PruningInstance));

	Assert(sourceInstance->isPartial);

	/*
	 * To make the new PruningInstance useful for pruning, we have to reset it
	 * being partial - if necessary it'll be marked so again by
	 * PrunableExpressionsWalker().
	 */
	memcpy(newInstance, sourceInstance, sizeof(PruningInstance));
	newInstance->addedToPruningInstances = false;
	newInstance->isPartial = false;

	return newInstance;
}


/*
 * ShardArrayToList builds a list of out the array of ShardInterval*.
 */
static List *
ShardArrayToList(ShardInterval **shardArray, int length)
{
	List *shardIntervalList = NIL;
	int shardIndex;

	for (shardIndex = 0; shardIndex < length; shardIndex++)
	{
		ShardInterval *shardInterval =
			shardArray[shardIndex];
		shardIntervalList = lappend(shardIntervalList, shardInterval);
	}

	return shardIntervalList;
}


/*
 * DeepCopyShardIntervalList copies originalShardIntervalList and the
 * contained ShardIntervals, into a new list.
 */
static List *
DeepCopyShardIntervalList(List *originalShardIntervalList)
{
	List *copiedShardIntervalList = NIL;
	ListCell *shardIntervalCell = NULL;

	foreach(shardIntervalCell, originalShardIntervalList)
	{
		ShardInterval *originalShardInterval =
			(ShardInterval *) lfirst(shardIntervalCell);
		ShardInterval *copiedShardInterval =
			(ShardInterval *) palloc0(sizeof(ShardInterval));

		CopyShardInterval(originalShardInterval, copiedShardInterval);
		copiedShardIntervalList = lappend(copiedShardIntervalList, copiedShardInterval);
	}

	return copiedShardIntervalList;
}


/*
 * PruneOne returns all shards in the table that match a single
 * PruningInstance.
 */
static List *
PruneOne(DistTableCacheEntry *cacheEntry, ClauseWalkerContext *context,
		 PruningInstance *prune)
{
	ShardInterval *shardInterval = NULL;
	List *shardIntervals = NULL;

	/* Well, if life always were this easy... */
	if (prune->evaluatesToFalse)
	{
		return NIL;
	}

	/*
	 * For an equal constraints, if there's no overlapping shards (always the
	 * case for hash and range partitioning, sometimes for append), can
	 * perform binary search for the right interval.  That's usually the
	 * fastest, so try that first.
	 */
	if (prune->equalConsts &&
		!cacheEntry->hasOverlappingShardInterval)
	{
		shardInterval = FindShardInterval(prune->equalConsts->constvalue, cacheEntry);

		/*
		 * If pruned down to nothing, we're done. Otherwise see if other
		 * methods prune down further / to nothing.
		 */
		if (!shardInterval)
		{
			return NIL;
		}

		shardIntervals = lappend(shardIntervals, shardInterval);
	}

	if (prune->saoEqualConsts &&
		!cacheEntry->hasOverlappingShardInterval)
	{
		ListCell *cell = NULL;

		foreach(cell, prune->saoEqualConsts)
		{
			Const *equalConsts = (Const *)lfirst(cell);

			shardInterval = FindShardInterval(equalConsts->constvalue, cacheEntry);

			if (shardInterval)
			{
				shardIntervals = list_append_unique_ptr(shardIntervals, shardInterval);
			}
		}

		if (!shardIntervals)
		{
			return NIL;
		}
	}

	/*
	 * If the hash value we're looking for is known, we can search for the
	 * interval directly.  That's fast and should only ever be the case for a
	 * hash-partitioned table.
	 */
	if (prune->hashedEqualConsts)
	{
		int shardIndex = INVALID_SHARD_INDEX;
		ShardInterval **sortedShardIntervalArray = cacheEntry->sortedShardIntervalArray;

		Assert(context->partitionMethod == DISTRIBUTE_BY_HASH);

		shardIndex = FindShardIntervalIndex(prune->hashedEqualConsts->constvalue,
											cacheEntry);

		if (shardIndex == INVALID_SHARD_INDEX)
		{
			return NIL;
		}

		if (!shardIntervals)
		{
			return list_make1(sortedShardIntervalArray[shardIndex]);
		}

		List *foundIntervals = NULL;
		ListCell *cell = NULL;

		foreach(cell, shardIntervals)
		{
			shardInterval = (ShardInterval*)lfirst(cell);

			/*
			 * equalConst based pruning might yielded a different shard than
			 * pruning based on pre-hashed equality.  This is useful in case
			 * of INSERT ... SELECT, where both can occur together (one via
			 * join/colocation, the other via a plain equality restriction).
			 */
			if (sortedShardIntervalArray[shardIndex]->shardId == shardInterval->shardId)
			{
				foundIntervals = lappend(foundIntervals, sortedShardIntervalArray[shardIndex]);
			}
		}

		return foundIntervals;
	}

	/*
	 * If previous pruning method yielded a single shard, and the table is not
	 * hash partitioned, attempt range based pruning to exclude it further.
	 *
	 * That's particularly important in particular for subquery pushdown,
	 * where it's very common to have a user specified equality restriction,
	 * and a range based restriction for shard boundaries, added by the
	 * subquery machinery.
	 */
	if (shardIntervals)
	{
		ListCell *cell = NULL;

		foreach(cell, shardIntervals)
		{
			ShardInterval *shardInterval = lfirst(cell);

			if (context->partitionMethod != DISTRIBUTE_BY_HASH &&
				ExhaustivePruneOne(shardInterval, context, prune))
			{
				return NIL;
			}
		}

		/* no chance to prune further, return */
		return shardIntervals;
	}

	/*
	 * Should never get here for hashing, we've filtered down to either zero
	 * or one shard, and returned.
	 */
	Assert(context->partitionMethod != DISTRIBUTE_BY_HASH);

	/*
	 * Next method: binary search with fuzzy boundaries. Can't trivially do so
	 * if shards have overlapping boundaries.
	 *
	 * TODO: If we kept shard intervals separately sorted by both upper and
	 * lower boundaries, this should be possible?
	 */
	if (!cacheEntry->hasOverlappingShardInterval && (
			prune->greaterConsts || prune->greaterEqualConsts ||
			prune->lessConsts || prune->lessEqualConsts))
	{
		return PruneWithBoundaries(cacheEntry, context, prune);
	}

	/*
	 * Brute force: Check each shard.
	 */
	return ExhaustivePrune(cacheEntry, context, prune);
}


/*
 * PerformCompare invokes comparator with prepared values, check for
 * unexpected NULL returns.
 */
static int
PerformCompare(FunctionCallInfoData *compareFunctionCall)
{
	Datum result = FunctionCallInvoke(compareFunctionCall);

	if (compareFunctionCall->isnull)
	{
		elog(ERROR, "function %u returned NULL", compareFunctionCall->flinfo->fn_oid);
	}

	return DatumGetInt32(result);
}


/*
 * PerformValueCompare invokes comparator with a/b, and checks for unexpected
 * NULL returns.
 */
static int
PerformValueCompare(FunctionCallInfoData *compareFunctionCall, Datum a, Datum b)
{
	compareFunctionCall->arg[0] = a;
	compareFunctionCall->argnull[0] = false;
	compareFunctionCall->arg[1] = b;
	compareFunctionCall->argnull[1] = false;

	return PerformCompare(compareFunctionCall);
}


/*
 * LowerShardBoundary returns the index of the first ShardInterval that's >=
 * (if includeMax) or > partitionColumnValue.
 */
static int
LowerShardBoundary(Datum partitionColumnValue, ShardInterval **shardIntervalCache,
				   int shardCount, FunctionCallInfoData *compareFunction, bool includeMax)
{
	int lowerBoundIndex = 0;
	int upperBoundIndex = shardCount;

	Assert(shardCount != 0);

	/* setup partitionColumnValue argument once */
	compareFunction->arg[0] = partitionColumnValue;
	compareFunction->argnull[0] = false;

	while (lowerBoundIndex < upperBoundIndex)
	{
		int middleIndex = lowerBoundIndex + ((upperBoundIndex - lowerBoundIndex) / 2);
		int maxValueComparison = 0;
		int minValueComparison = 0;

		/* setup minValue as argument */
		compareFunction->arg[1] = shardIntervalCache[middleIndex]->minValue;
		compareFunction->argnull[1] = false;

		/* execute cmp(partitionValue, lowerBound) */
		minValueComparison = PerformCompare(compareFunction);

		/* and evaluate results */
		if (minValueComparison < 0)
		{
			/* value smaller than entire range */
			upperBoundIndex = middleIndex;
			continue;
		}

		/* setup maxValue as argument */
		compareFunction->arg[1] = shardIntervalCache[middleIndex]->maxValue;
		compareFunction->argnull[1] = false;

		/* execute cmp(partitionValue, upperBound) */
		maxValueComparison = PerformCompare(compareFunction);

		if ((maxValueComparison == 0 && !includeMax) ||
			maxValueComparison > 0)
		{
			/* value bigger than entire range */
			lowerBoundIndex = middleIndex + 1;
			continue;
		}

		/* found interval containing partitionValue */
		return middleIndex;
	}

	Assert(lowerBoundIndex == upperBoundIndex);

	/*
	 * If we get here, none of the ShardIntervals exactly contain the value
	 * (we'd have hit the return middleIndex; case otherwise). Figure out
	 * whether there's possibly any interval containing a value that's bigger
	 * than the partition key one.
	 */
	if (lowerBoundIndex == 0)
	{
		/* all intervals are bigger, thus return 0 */
		return 0;
	}
	else if (lowerBoundIndex == shardCount)
	{
		/* partition value is bigger than all partition values */
		return INVALID_SHARD_INDEX;
	}

	/* value falls inbetween intervals */
	return lowerBoundIndex + 1;
}


/*
 * UpperShardBoundary returns the index of the last ShardInterval that's <=
 * (if includeMin) or < partitionColumnValue.
 */
static int
UpperShardBoundary(Datum partitionColumnValue, ShardInterval **shardIntervalCache,
				   int shardCount, FunctionCallInfoData *compareFunction, bool includeMin)
{
	int lowerBoundIndex = 0;
	int upperBoundIndex = shardCount;

	Assert(shardCount != 0);

	/* setup partitionColumnValue argument once */
	compareFunction->arg[0] = partitionColumnValue;
	compareFunction->argnull[0] = false;

	while (lowerBoundIndex < upperBoundIndex)
	{
		int middleIndex = lowerBoundIndex + ((upperBoundIndex - lowerBoundIndex) / 2);
		int maxValueComparison = 0;
		int minValueComparison = 0;

		/* setup minValue as argument */
		compareFunction->arg[1] = shardIntervalCache[middleIndex]->minValue;
		compareFunction->argnull[1] = false;

		/* execute cmp(partitionValue, lowerBound) */
		minValueComparison = PerformCompare(compareFunction);

		/* and evaluate results */
		if ((minValueComparison == 0 && !includeMin) ||
			minValueComparison < 0)
		{
			/* value smaller than entire range */
			upperBoundIndex = middleIndex;
			continue;
		}

		/* setup maxValue as argument */
		compareFunction->arg[1] = shardIntervalCache[middleIndex]->maxValue;
		compareFunction->argnull[1] = false;

		/* execute cmp(partitionValue, upperBound) */
		maxValueComparison = PerformCompare(compareFunction);

		if (maxValueComparison > 0)
		{
			/* value bigger than entire range */
			lowerBoundIndex = middleIndex + 1;
			continue;
		}

		/* found interval containing partitionValue */
		return middleIndex;
	}

	Assert(lowerBoundIndex == upperBoundIndex);

	/*
	 * If we get here, none of the ShardIntervals exactly contain the value
	 * (we'd have hit the return middleIndex; case otherwise). Figure out
	 * whether there's possibly any interval containing a value that's smaller
	 * than the partition key one.
	 */
	if (upperBoundIndex == shardCount)
	{
		/* all intervals are smaller, thus return 0 */
		return shardCount - 1;
	}
	else if (upperBoundIndex == 0)
	{
		/* partition value is smaller than all partition values */
		return INVALID_SHARD_INDEX;
	}

	/* value falls inbetween intervals, return the inverval one smaller as bound */
	return upperBoundIndex - 1;
}


/*
 * PruneWithBoundaries searches for shards that match inequality constraints,
 * using binary search on both the upper and lower boundary, and returns a
 * list of surviving shards.
 */
static List *
PruneWithBoundaries(DistTableCacheEntry *cacheEntry, ClauseWalkerContext *context,
					PruningInstance *prune)
{
	List *remainingShardList = NIL;
	int shardCount = cacheEntry->shardIntervalArrayLength;
	ShardInterval **sortedShardIntervalArray = cacheEntry->sortedShardIntervalArray;
	bool hasLowerBound = false;
	bool hasUpperBound = false;
	Datum lowerBound = 0;
	Datum upperBound = 0;
	bool lowerBoundInclusive = false;
	bool upperBoundInclusive = false;
	int lowerBoundIdx = -1;
	int upperBoundIdx = -1;
	int curIdx = 0;
	FunctionCallInfo compareFunctionCall = &context->compareIntervalFunctionCall;

	if (prune->greaterEqualConsts)
	{
		lowerBound = prune->greaterEqualConsts->constvalue;
		lowerBoundInclusive = true;
		hasLowerBound = true;
	}
	if (prune->greaterConsts)
	{
		/*
		 * Use the more restrictive one, if both greater and greaterEqual
		 * constraints are specified.
		 */
		if (!hasLowerBound ||
			PerformValueCompare(compareFunctionCall,
								prune->greaterConsts->constvalue,
								lowerBound) >= 0)
		{
			lowerBound = prune->greaterConsts->constvalue;
			lowerBoundInclusive = false;
			hasLowerBound = true;
		}
	}
	if (prune->lessEqualConsts)
	{
		upperBound = prune->lessEqualConsts->constvalue;
		upperBoundInclusive = true;
		hasUpperBound = true;
	}
	if (prune->lessConsts)
	{
		/*
		 * Use the more restrictive one, if both less and lessEqual
		 * constraints are specified.
		 */
		if (!hasUpperBound ||
			PerformValueCompare(compareFunctionCall,
								prune->lessConsts->constvalue,
								upperBound) <= 0)
		{
			upperBound = prune->lessConsts->constvalue;
			upperBoundInclusive = false;
			hasUpperBound = true;
		}
	}

	Assert(hasLowerBound || hasUpperBound);

	/* find lower bound */
	if (hasLowerBound)
	{
		lowerBoundIdx = LowerShardBoundary(lowerBound, sortedShardIntervalArray,
										   shardCount, compareFunctionCall,
										   lowerBoundInclusive);
	}
	else
	{
		lowerBoundIdx = 0;
	}

	/* find upper bound */
	if (hasUpperBound)
	{
		upperBoundIdx = UpperShardBoundary(upperBound, sortedShardIntervalArray,
										   shardCount, compareFunctionCall,
										   upperBoundInclusive);
	}
	else
	{
		upperBoundIdx = shardCount - 1;
	}

	if (lowerBoundIdx == INVALID_SHARD_INDEX)
	{
		return NIL;
	}
	else if (upperBoundIdx == INVALID_SHARD_INDEX)
	{
		return NIL;
	}

	/*
	 * Build list of all shards that are in the range of shards (possibly 0).
	 */
	for (curIdx = lowerBoundIdx; curIdx <= upperBoundIdx; curIdx++)
	{
		remainingShardList = lappend(remainingShardList,
									 sortedShardIntervalArray[curIdx]);
	}

	return remainingShardList;
}


/*
 * ExhaustivePrune returns a list of shards matching PruningInstances
 * constraints, by simply checking them for each individual shard.
 */
static List *
ExhaustivePrune(DistTableCacheEntry *cacheEntry, ClauseWalkerContext *context,
				PruningInstance *prune)
{
	List *remainingShardList = NIL;
	int shardCount = cacheEntry->shardIntervalArrayLength;
	ShardInterval **sortedShardIntervalArray = cacheEntry->sortedShardIntervalArray;
	int curIdx = 0;

	for (curIdx = 0; curIdx < shardCount; curIdx++)
	{
		ShardInterval *curInterval = sortedShardIntervalArray[curIdx];

		if (!ExhaustivePruneOne(curInterval, context, prune))
		{
			remainingShardList = lappend(remainingShardList, curInterval);
		}
	}

	return remainingShardList;
}


/*
 * ExhaustivePruneOne returns true if curInterval is pruned away, false
 * otherwise.
 */
static bool
ExhaustivePruneOne(ShardInterval *curInterval,
				   ClauseWalkerContext *context,
				   PruningInstance *prune)
{
	FunctionCallInfo compareFunctionCall = &context->compareIntervalFunctionCall;
	Datum compareWith = 0;

	/* NULL boundaries can't be compared to */
	if (!curInterval->minValueExists || !curInterval->maxValueExists)
	{
		return false;
	}

	if (prune->equalConsts)
	{
		compareWith = prune->equalConsts->constvalue;

		if (PerformValueCompare(compareFunctionCall,
								compareWith,
								curInterval->minValue) < 0)
		{
			return true;
		}

		if (PerformValueCompare(compareFunctionCall,
								compareWith,
								curInterval->maxValue) > 0)
		{
			return true;
		}
	}
	if (prune->saoEqualConsts)
	{
		ListCell *cell = NULL;

		foreach(cell, prune->saoEqualConsts)
		{
			compareWith = ((Const *)lfirst(cell))->constvalue;

			if (PerformValueCompare(compareFunctionCall,
									compareWith,
									curInterval->minValue) < 0)
			{
				return true;
			}

			if (PerformValueCompare(compareFunctionCall,
									compareWith,
									curInterval->maxValue) > 0)
			{
				return true;
			}
		}
	}
	if (prune->greaterEqualConsts)
	{
		compareWith = prune->greaterEqualConsts->constvalue;

		if (PerformValueCompare(compareFunctionCall,
								curInterval->maxValue,
								compareWith) < 0)
		{
			return true;
		}
	}
	if (prune->greaterConsts)
	{
		compareWith = prune->greaterConsts->constvalue;

		if (PerformValueCompare(compareFunctionCall,
								curInterval->maxValue,
								compareWith) <= 0)
		{
			return true;
		}
	}
	if (prune->lessEqualConsts)
	{
		compareWith = prune->lessEqualConsts->constvalue;

		if (PerformValueCompare(compareFunctionCall,
								curInterval->minValue,
								compareWith) > 0)
		{
			return true;
		}
	}
	if (prune->lessConsts)
	{
		compareWith = prune->lessConsts->constvalue;

		if (PerformValueCompare(compareFunctionCall,
								curInterval->minValue,
								compareWith) >= 0)
		{
			return true;
		}
	}

	return false;
}
