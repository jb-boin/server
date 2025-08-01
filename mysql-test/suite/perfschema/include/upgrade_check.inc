# Routine to be called by pfs_upgrade.test
# $out_file and $err_file must be set within pfs_upgrade.test.
#

--exec $MYSQL_UPGRADE --skip-verbose --force > $MYSQLTEST_VARDIR/tmp/out_file 2> $MYSQLTEST_VARDIR/tmp/err_file

# Verify that mysql_upgrade does not complain about the performance_schema

--replace_regex /at line [0-9]+/at line ###/
--cat_file $MYSQLTEST_VARDIR/tmp/err_file
--remove_file $MYSQLTEST_VARDIR/tmp/out_file
--remove_file $MYSQLTEST_VARDIR/tmp/err_file
--remove_file $MYSQLD_DATADIR/mariadb_upgrade_info
