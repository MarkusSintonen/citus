/* citus--7.2-3--7.3-1 */

DROP FUNCTION IF EXISTS master_expire_table_cache(regclass);
DROP FUNCTION IF EXISTS pg_catalog.worker_fetch_regular_table(text, bigint, text[], integer[]);
DROP FUNCTION IF EXISTS pg_catalog.worker_fetch_foreign_file(text, bigint, text[], integer[]);

/* bump version to 7.3-1 */

