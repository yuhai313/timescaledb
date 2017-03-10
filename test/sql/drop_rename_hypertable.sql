\set ON_ERROR_STOP 1
\o /dev/null
\ir include/insert_two_partitions.sql
\o
\set ECHO ALL

\c single
\d+ "_timescaledb_internal".*

-- Test that renaming hypertable is blocked
\set ON_ERROR_STOP 0
ALTER TABLE "testNs" RENAME TO "newname";
\set ON_ERROR_STOP 1

-- Test that renaming ordinary table works
CREATE TABLE renametable (foo int);
ALTER TABLE "renametable" RENAME TO "newname";
SELECT * FROM "newname";

SELECT * FROM _timescaledb_catalog.hypertable;
DROP TABLE "testNs";

SELECT * FROM _timescaledb_catalog.hypertable;
\dt  "public".*
\dt  "_timescaledb_catalog".*
\dt+ "_timescaledb_internal".*
