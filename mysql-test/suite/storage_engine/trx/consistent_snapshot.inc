#
# TRANSACTION WITH CONSISTENT SNAPSHOT
#

--source ../have_engine.inc

connect (con1,localhost,root,,);
connect (con2,localhost,root,,);

connection con1;

let $create_definition = a $int_col;
--source ../create_table.inc

eval SET SESSION TRANSACTION ISOLATION LEVEL $trx_isolation;

# While a consistent snapshot transaction is executed,
# no external inserts should be visible to the transaction.
# But it should only work this way for REPEATABLE-READ and SERIALIZABLE

START TRANSACTION WITH CONSISTENT SNAPSHOT;

connection con2;
INSERT INTO t1 (a) VALUES (1);
if ($mysql_errname)
{
  --echo # INSERT finished with error $mysql_errname
}

connection con1;
--echo # If consistent read works on this isolation level ($trx_isolation), the following SELECT should not return the value we inserted (1)
SELECT a FROM t1; 
COMMIT;

connection default;
disconnect con1;
disconnect con2;
DROP TABLE t1;
