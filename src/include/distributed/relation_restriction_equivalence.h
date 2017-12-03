/*
 * relation_restriction_equivalence.h
 *
 * This file contains functions helper functions for planning
 * queries with colocated tables and subqueries.
 *
 * Copyright (c) 2017-2017, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef RELATION_RESTRICTION_EQUIVALENCE_H
#define RELATION_RESTRICTION_EQUIVALENCE_H

#include "distributed/distributed_planner.h"


extern bool ContainsUnionSubquery(Query *queryTree);
extern bool RestrictionEquivalenceForPartitionKeys(PlannerRestrictionContext *
												   plannerRestrictionContext);
extern uint32 ReferenceRelationCount(RelationRestrictionContext *restrictionContext);
extern bool SafeToPushdownUnionSubquery(
	PlannerRestrictionContext *plannerRestrictionContext);
extern List * RelationIdList(Query *query);


/* TODO: move definitions to relation restriction */
RelationRestrictionContext * FilterRelationRestrictionContext(
	RelationRestrictionContext *relationRestrictionContext,
	Relids
	queryRteIdentities);
JoinRestrictionContext * FilterJoinRestrictionContext(
	JoinRestrictionContext *joinRestrictionContext, Relids
	queryRteIdentities);
List * GenerateAttributeEquivalencesForRelationRestrictions(
	RelationRestrictionContext *restrictionContext);
List * GenerateAttributeEquivalencesForJoinRestrictions(JoinRestrictionContext
														*joinRestrictionContext);

bool EquivalenceListContainsRelationsEquality(List *attributeEquivalenceList,
											  RelationRestrictionContext *
											  restrictionContext);

Relids QueryRteIdentities(Query *queryTree);

#endif /* RELATION_RESTRICTION_EQUIVALENCE_H */
