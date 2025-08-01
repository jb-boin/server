/* Copyright (c) 2010, 2015, Oracle and/or its affiliates.
   Copyright (c) 2011, 2021, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "mariadb.h"
#include "sql_class.h"                       // THD
#include "keycaches.h"                       // get_key_cache
#include "sql_base.h"                        // Open_table_context
#include "lock.h"                            // MYSQL_OPEN_*
#include "sql_handler.h"                     // mysql_ha_rm_tables
#include "partition_element.h"               // PART_ADMIN
#include "sql_partition.h"                   // set_part_state
#include "transaction.h"                     // trans_rollback_stmt
#include "sql_view.h"                        // view_checksum
#include "sql_table.h"                       // mysql_recreate_table
#include "debug_sync.h"                      // DEBUG_SYNC
#include "sp.h"                              // Sroutine_hash_entry
#include "sql_parse.h"                       // check_table_access
#include "strfunc.h"
#include "sql_admin.h"
#include "sql_statistics.h"
#include "wsrep_mysqld.h"
#ifdef WITH_WSREP
#include "wsrep_trans_observer.h"
#endif

const LEX_CSTRING msg_status= {STRING_WITH_LEN("status")};
const LEX_CSTRING msg_repair= { STRING_WITH_LEN("repair") };
const LEX_CSTRING msg_assign_to_keycache=
{ STRING_WITH_LEN("assign_to_keycache") };
const LEX_CSTRING msg_analyze= { STRING_WITH_LEN("analyze") };
const LEX_CSTRING msg_check= { STRING_WITH_LEN("check") };
const LEX_CSTRING msg_preload_keys= { STRING_WITH_LEN("preload_keys") };
const LEX_CSTRING msg_optimize= { STRING_WITH_LEN("optimize") };

/* Prepare, run and cleanup for mysql_recreate_table() */

static bool admin_recreate_table(THD *thd, TABLE_LIST *table_list,
                                 Recreate_info *recreate_info,
                                 bool table_copy)
{
  bool result_code;
  TABLE_LIST *save_next_global;
  DBUG_ENTER("admin_recreate_table");

  trans_rollback_stmt(thd);
  trans_rollback(thd);
  thd->tmp_table_binlog_handled= 1;
  close_thread_tables(thd);
  thd->release_transactional_locks();

  /*
    table_list->table has been closed and freed. Do not reference
    uninitialized data. open_tables() could fail.
  */
  table_list->table= NULL;
  /* Same applies to MDL ticket. */
  table_list->mdl_request.ticket= NULL;

  DEBUG_SYNC(thd, "ha_admin_try_alter");
  tmp_disable_binlog(thd); // binlogging is done by caller if wanted
  /* Ignore if there is more than one table in the list */
  save_next_global= table_list->next_global;
  table_list->next_global= 0;
  result_code= (thd->open_temporary_tables(table_list) ||
                mysql_recreate_table(thd, table_list, recreate_info,
                                     table_copy));
  table_list->next_global= save_next_global;
  reenable_binlog(thd);
  /*
    mysql_recreate_table() can push OK or ERROR.
    Clear 'OK' status. If there is an error, keep it:
    we will store the error message in a result set row 
    and then clear.
  */
  if (thd->get_stmt_da()->is_ok())
    thd->get_stmt_da()->reset_diagnostics_area();
  table_list->table= NULL;
  DBUG_RETURN(result_code);
}


static int send_check_errmsg(THD *thd, TABLE_LIST* table,
			     const LEX_CSTRING *operator_name,
                             const char* errmsg)

{
  Protocol *protocol= thd->protocol;
  protocol->prepare_for_resend();
  protocol->store(table->alias.str, table->alias.length, system_charset_info);
  protocol->store(operator_name, system_charset_info);
  protocol->store(&error_clex_str, system_charset_info);
  protocol->store(errmsg, strlen(errmsg), system_charset_info);
  thd->clear_error();
  if (protocol->write())
    return -1;
  return 1;
}


static int prepare_for_repair(THD *thd, TABLE_LIST *table_list,
			      HA_CHECK_OPT *check_opt)
{
  int error= 0, create_error= 0;
  TABLE tmp_table, *table;
  TABLE_LIST *pos_in_locked_tables= 0;
  TABLE_SHARE *share= 0;
  bool has_mdl_lock= FALSE;
  char from[FN_REFLEN],tmp[FN_REFLEN+32];
  const char **ext;
  MY_STAT stat_info;
  Open_table_context ot_ctx(thd, (MYSQL_OPEN_IGNORE_FLUSH |
                                  MYSQL_OPEN_HAS_MDL_LOCK |
                                  MYSQL_LOCK_IGNORE_TIMEOUT));
  DBUG_ENTER("prepare_for_repair");

  if (!(check_opt->sql_flags & TT_USEFRM))
    DBUG_RETURN(0);

  if (!(table= table_list->table))
  {
    /*
      If the table didn't exist, we have a shared metadata lock
      on it that is left from mysql_admin_table()'s attempt to 
      open it. Release the shared metadata lock before trying to
      acquire the exclusive lock to satisfy MDL asserts and avoid
      deadlocks.
    */
    thd->release_transactional_locks();
    /*
      Attempt to do full-blown table open in mysql_admin_table() has failed.
      Let us try to open at least a .FRM for this table.
    */

    MDL_REQUEST_INIT(&table_list->mdl_request, MDL_key::TABLE,
                     table_list->db.str, table_list->table_name.str,
                     MDL_EXCLUSIVE, MDL_TRANSACTION);

    if (lock_table_names(thd, table_list, table_list->next_global,
                         thd->variables.lock_wait_timeout, 0))
      DBUG_RETURN(0);
    has_mdl_lock= TRUE;

    share= tdc_acquire_share(thd, table_list, GTS_TABLE);
    if (share == NULL)
      DBUG_RETURN(0);				// Can't open frm file

    if (open_table_from_share(thd, share, &empty_clex_str, 0, 0, 0,
                              &tmp_table, FALSE))
    {
      tdc_release_share(share);
      DBUG_RETURN(0);                           // Out of memory
    }
    table= &tmp_table;
  }

  /*
    REPAIR TABLE ... USE_FRM for temporary tables makes little sense.
  */
  if (table->s->tmp_table)
  {
    error= send_check_errmsg(thd, table_list, &msg_repair,
			     "Cannot repair temporary table from .frm file");
    goto end;
  }

  /*
    User gave us USE_FRM which means that the header in the index file is
    trashed.
    In this case we will try to fix the table the following way:
    - Rename the data file to a temporary name
    - Truncate the table
    - Replace the new data file with the old one
    - Run a normal repair using the new index file and the old data file
  */

  if (table->s->frm_version < FRM_VER_TRUE_VARCHAR &&
      table->s->varchar_fields)
  {
    error= send_check_errmsg(thd, table_list, &msg_repair,
                             "Failed repairing a very old .frm file as the "
                             "data file format has changed between versions. "
                             "Please dump the table in your old system with "
                             "mysqldump and read it into this system with "
                             "mysql or mysqlimport");
    goto end;
  }

  /*
    Check if this is a table type that stores index and data separately,
    like ISAM or MyISAM. We assume fixed order of engine file name
    extensions array. First element of engine file name extensions array
    is meta/index file extention. Second element - data file extention. 
  */
  ext= table->file->bas_ext();
  if (!ext[0] || !ext[1])
    goto end;					// No data file

  /* A MERGE table must not come here. */
  DBUG_ASSERT(table->file->ht->db_type != DB_TYPE_MRG_MYISAM);

  // Name of data file
  strxmov(from, table->s->normalized_path.str, ext[1], NullS);
  if (!mysql_file_stat(key_file_misc, from, &stat_info, MYF(0)))
    goto end;				// Can't use USE_FRM flag

  my_snprintf(tmp, sizeof(tmp), "%s-%lx_%llx",
	      from, current_pid, thd->thread_id);

  if (table_list->table)
  {
    /*
      Table was successfully open in mysql_admin_table(). Now we need
      to close it, but leave it protected by exclusive metadata lock.
    */
    pos_in_locked_tables= table->pos_in_locked_tables;
    if (wait_while_table_is_used(thd, table, HA_EXTRA_PREPARE_FOR_FORCED_CLOSE))
      goto end;
    /* Close table but don't remove from locked list */
    close_all_tables_for_name(thd, table_list->table->s,
                              HA_EXTRA_NOT_USED, NULL);
    table_list->table= 0;
  }
  else
  {
    /*
      Table open failed, maybe because we run out of memory.
      Close all open tables and release all MDL locks
    */
    tdc_release_share(share);
    share->tdc->flush(thd, true);
    share= 0;
  }

  /*
    After this point we have an exclusive metadata lock on our table
    in both cases when table was successfully open in mysql_admin_table()
    and when it was open in prepare_for_repair().
  */

  if (my_rename(from, tmp, MYF(MY_WME)))
  {
    error= send_check_errmsg(thd, table_list, &msg_repair,
			     "Failed renaming data file");
    goto end;
  }
  if (dd_recreate_table(thd, table_list->db.str, table_list->table_name.str))
    create_error= send_check_errmsg(thd, table_list, &msg_repair,
                                    "Failed generating table from .frm file");
  /*
    'FALSE' for 'using_transactions' means don't postpone
    invalidation till the end of a transaction, but do it
    immediately.
  */
  query_cache_invalidate3(thd, table_list, FALSE);
  if (mysql_file_rename(key_file_misc, tmp, from, MYF(MY_WME)))
  {
    error= send_check_errmsg(thd, table_list, &msg_repair,
			     "Failed restoring .MYD file");
    goto end;
  }
  if (create_error)
    goto end;

  if (thd->locked_tables_list.locked_tables())
  {
    if (thd->locked_tables_list.reopen_tables(thd, false))
      goto end;
    /* Restore the table in the table list with the new opened table */
    table_list->table= pos_in_locked_tables->table;
  }
  else
  {
    /*
      Now we should be able to open the partially repaired table
      to finish the repair in the handler later on.
    */
    if (open_table(thd, table_list, &ot_ctx))
    {
      error= send_check_errmsg(thd, table_list, &msg_repair,
                               "Failed to open partially repaired table");
      goto end;
    }
  }
  /*
    We have now fixed the table. However the frm file is still of old format.
    We cannot update the frm_file to FRM_VER_TRUE_VARCHAR as the new format
    are not compatible with the data.
  */

end:
  thd->locked_tables_list.unlink_all_closed_tables(thd, NULL, 0);
  if (table == &tmp_table)
  {
    closefrm(table);
    if (share)
      tdc_release_share(share);
  }
  /* In case of a temporary table there will be no metadata lock. */
  if (unlikely(error) && has_mdl_lock)
    thd->release_transactional_locks();

  DBUG_RETURN(error);
}


/**
  Check if a given error is something that could occur during
  open_and_lock_tables() that does not indicate table corruption.

  @param  sql_errno  Error number to check.

  @retval TRUE       Error does not indicate table corruption.
  @retval FALSE      Error could indicate table corruption.
*/

static inline bool table_not_corrupt_error(uint sql_errno)
{
  return (sql_errno == ER_NO_SUCH_TABLE ||
          sql_errno == ER_NO_SUCH_TABLE_IN_ENGINE ||
          sql_errno == ER_FILE_NOT_FOUND ||
          sql_errno == ER_LOCK_WAIT_TIMEOUT ||
          sql_errno == ER_LOCK_DEADLOCK ||
          sql_errno == ER_CANT_LOCK_LOG_TABLE ||
          sql_errno == ER_OPEN_AS_READONLY ||
          sql_errno == ER_WRONG_OBJECT);
}

#ifndef DBUG_OFF
// It is counter for debugging fail on second call of open_only_one_table
static int debug_fail_counter= 0;
#endif

static bool open_only_one_table(THD* thd, TABLE_LIST* table,
                                bool repair_table_use_frm,
                                bool is_view_operator_func)
{
  LEX *lex= thd->lex;
  SELECT_LEX *select= lex->first_select_lex();
  TABLE_LIST *save_next_global, *save_next_local;
  bool open_error;
  save_next_global= table->next_global;
  table->next_global= 0;
  save_next_local= table->next_local;
  table->next_local= 0;
  select->table_list.first= table;
  /*
    Time zone tables and SP tables can be add to lex->query_tables list,
    so it have to be prepared.
    TODO: Investigate if we can put extra tables into argument instead of
    using lex->query_tables
  */
  lex->query_tables= table;
  lex->query_tables_last= &table->next_global;
  lex->query_tables_own_last= 0;

  DBUG_EXECUTE_IF("fail_2call_open_only_one_table", {
                  if (debug_fail_counter)
                  {
                    open_error= TRUE;
                    goto dbug_err;
                  }
                  else
                    debug_fail_counter++;
                  });

  /*
    CHECK TABLE command is allowed for views as well. Check on alter flags
    to differentiate from ALTER TABLE...CHECK PARTITION on which view is not
    allowed.
  */
  if (lex->alter_info.partition_flags & ALTER_PARTITION_ADMIN ||
      !is_view_operator_func)
  {
    table->required_type= TABLE_TYPE_NORMAL;
    DBUG_ASSERT(lex->table_type != TABLE_TYPE_VIEW);
  }
  else if (lex->table_type == TABLE_TYPE_VIEW)
  {
    table->required_type= lex->table_type;
  }
  else if ((lex->table_type != TABLE_TYPE_VIEW) &&
           lex->sql_command == SQLCOM_REPAIR)
  {
    table->required_type= TABLE_TYPE_NORMAL;
  }

  if (lex->sql_command == SQLCOM_CHECK ||
      lex->sql_command == SQLCOM_REPAIR ||
      lex->sql_command == SQLCOM_ANALYZE ||
      lex->sql_command == SQLCOM_OPTIMIZE)
    thd->prepare_derived_at_open= TRUE;
  if (!thd->locked_tables_mode && repair_table_use_frm)
  {
    /*
      If we're not under LOCK TABLES and we're executing REPAIR TABLE
      USE_FRM, we need to ignore errors from open_and_lock_tables().
      REPAIR TABLE USE_FRM is a heavy weapon used when a table is
      critically damaged, so open_and_lock_tables() will most likely
      report errors. Those errors are not interesting for the user
      because it's already known that the table is badly damaged.
    */

    Diagnostics_area *da= thd->get_stmt_da();
    Warning_info tmp_wi(thd->query_id, false, true);

    da->push_warning_info(&tmp_wi);

    open_error= (thd->open_temporary_tables(table) ||
                 open_and_lock_tables(thd, table, TRUE, 0));

    da->pop_warning_info();
  }
  else
  {
    /*
      It's assumed that even if it is REPAIR TABLE USE_FRM, the table
      can be opened if we're under LOCK TABLES (otherwise LOCK TABLES
      would fail). Thus, the only errors we could have from
      open_and_lock_tables() are logical ones, like incorrect locking
      mode. It does make sense for the user to see such errors.
    */

    open_error= (thd->open_temporary_tables(table) ||
                 open_and_lock_tables(thd, table, TRUE, 0));
  }

#ifndef DBUG_OFF
dbug_err:
#endif

  thd->prepare_derived_at_open= FALSE;

  /*
    MERGE engine may adjust table->next_global chain, thus we have to
    append save_next_global after merge children.
  */
  if (save_next_global)
  {
    TABLE_LIST *table_list_iterator= table;
    while (table_list_iterator->next_global)
      table_list_iterator= table_list_iterator->next_global;
    table_list_iterator->next_global= save_next_global;
    save_next_global->prev_global= &table_list_iterator->next_global;
  }

  table->next_local= save_next_local;

  return open_error;
}

#ifdef WITH_WSREP
/** RAII class for temporarily disable wsrep_on in the connection. */
class Disable_wsrep_on_guard
{
 public:
  /**
    @param thd     - pointer to the context of connection in which
                     wsrep_on mode needs to be disabled.
    @param disable - true if wsrep_on should be disabled
  */
  explicit Disable_wsrep_on_guard(THD *thd, bool disable)
    : m_thd(thd), m_orig_wsrep_on(thd->variables.wsrep_on)
  {
    if (disable)
      thd->variables.wsrep_on= false;
  }

  ~Disable_wsrep_on_guard()
  {
    m_thd->variables.wsrep_on= m_orig_wsrep_on;
  }
 private:
  THD* m_thd;
  bool m_orig_wsrep_on;
};
#endif /* WITH_WSREP */


static void send_read_only_warning(THD *thd, const LEX_CSTRING *msg_status,
                                   const LEX_CSTRING *table_name)
{
  Protocol *protocol= thd->protocol;
  char buf[MYSQL_ERRMSG_SIZE];
  size_t length;
  length= my_snprintf(buf, sizeof(buf),
                      ER_THD(thd, ER_OPEN_AS_READONLY),
                      table_name->str);
  protocol->store(msg_status, system_charset_info);
  protocol->store(buf, length, system_charset_info);
}


/**
  Collect field names of result set that will be sent to a client

  @param      thd     Thread data object
  @param[out] fields  List of fields whose metadata should be collected for
                      sending to client
*/

void fill_check_table_metadata_fields(THD *thd, List<Item>* fields)
{
  Item *item;

  item= new (thd->mem_root) Item_empty_string(thd, "Table", NAME_CHAR_LEN * 2);
  item->set_maybe_null();
  fields->push_back(item, thd->mem_root);

  item= new (thd->mem_root) Item_empty_string(thd, "Op", 10);
  item->set_maybe_null();
  fields->push_back(item, thd->mem_root);

  item= new (thd->mem_root) Item_empty_string(thd, "Msg_type", 10);
  item->set_maybe_null();
  fields->push_back(item, thd->mem_root);

  item= new (thd->mem_root) Item_empty_string(thd, "Msg_text",
                                              SQL_ADMIN_MSG_TEXT_SIZE);
  item->set_maybe_null();
  fields->push_back(item, thd->mem_root);
}


/*
  RETURN VALUES
    FALSE Message sent to net (admin operation went ok)
    TRUE  Message should be sent by caller 
          (admin operation or network communication failed)
*/
static bool mysql_admin_table(THD* thd, TABLE_LIST* tables,
                              HA_CHECK_OPT* check_opt,
                              const LEX_CSTRING *operator_name,
                              thr_lock_type lock_type,
                              bool org_open_for_modify,
                              bool no_errors_from_open,
                              uint extra_open_options,
                              int (*prepare_func)(THD *, TABLE_LIST *,
                                                  HA_CHECK_OPT *),
                              int (handler::*operator_func)(THD *,
                                                            HA_CHECK_OPT *),
                              int (view_operator_func)(THD *, TABLE_LIST*,
                                                       HA_CHECK_OPT *),
                              bool is_cmd_replicated)
{
  TABLE_LIST *table;
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  LEX *lex= thd->lex;
  bool need_repair_or_alter= 0;
  wait_for_commit* suspended_wfc;
  bool is_table_modified= false;
  LEX_CUSTRING tabledef_version;
  DBUG_ENTER("mysql_admin_table");
  DBUG_PRINT("enter", ("extra_open_options: %u", extra_open_options));

#ifdef WITH_WSREP
  /*
    CACHE INDEX and LOAD INDEX INTO CACHE statements are
    local operations. Do not replicate them with Galera
  */
  const bool disable_wsrep_on= (WSREP(thd) &&
    (lex->sql_command == SQLCOM_ASSIGN_TO_KEYCACHE ||
     lex->sql_command == SQLCOM_PRELOAD_KEYS));

  Disable_wsrep_on_guard wsrep_on_guard(thd, disable_wsrep_on);
#endif /* WITH_WSREP */

  fill_check_table_metadata_fields(thd, &field_list);

  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  /*
    This function calls trans_commit() during its operation, but that does not
    imply that the operation is complete or binlogged. So we have to suspend
    temporarily the wakeup_subsequent_commits() calls (if used).
  */
  suspended_wfc= thd->suspend_subsequent_commits();

  mysql_ha_rm_tables(thd, tables);

  /*
    Close all temporary tables which were pre-open to simplify
    privilege checking. Clear all references to closed tables.
  */
  close_thread_tables(thd);
  for (table= tables; table; table= table->next_local)
    table->table= NULL;

  for (table= tables; table; table= table->next_local)
  {
    char table_name_buff[SAFE_NAME_LEN*2+2];
    LEX_CSTRING table_name= { table_name_buff, 0};
    char storage_engine_name[NAME_LEN];
    bool storage_engine_partitioned= 0;
    uchar tabledef_version_buff[MY_UUID_SIZE];
    const char *db= table->db.str;
    bool fatal_error=0;
    bool open_error= 0, recreate_used= 0;
    bool require_data_conversion= 0, require_alter_table= 0;
    bool collect_eis=  FALSE;
    bool open_for_modify= org_open_for_modify;
    Recreate_info recreate_info;
    int compl_result_code, result_code;

    compl_result_code= result_code= HA_ADMIN_FAILED;
    storage_engine_name[0]= 0;                  // Marker that's not used

    DBUG_PRINT("admin", ("table: '%s'.'%s'", db, table->table_name.str));
    DEBUG_SYNC(thd, "admin_command_kill_before_modify");

    table_name.length= strxmov(table_name_buff, db, ".", table->table_name.str,
                               NullS) - table_name_buff;
    thd->open_options|= extra_open_options;
    table->lock_type= lock_type;
    /*
      To make code safe for re-execution we need to reset type of MDL
      request as code below may change it.
      To allow concurrent execution of read-only operations we acquire
      weak metadata lock for them.
    */
    table->mdl_request.set_type(lex->sql_command == SQLCOM_REPAIR
                                ? MDL_SHARED_NO_READ_WRITE
                                : lock_type >= TL_FIRST_WRITE
                                ? MDL_SHARED_WRITE : MDL_SHARED_READ);
    if (thd->check_killed())
    {
      open_error= false;
      fatal_error= true;
      result_code= HA_ADMIN_FAILED;
      goto send_result;
    }

    /* open only one table from local list of command */
    while (1)
    {
      open_error= open_only_one_table(thd, table,
                                      no_errors_from_open,
                                      (view_operator_func != NULL));
      thd->open_options&= ~extra_open_options;

      /*
        If open_and_lock_tables() failed, close_thread_tables() will close
        the table and table->table can therefore be invalid.
      */
      if (unlikely(open_error))
        table->table= NULL;

      /*
        Under locked tables, we know that the table can be opened,
        so any errors opening the table are logical errors.
        In these cases it does not make sense to try to repair.
      */
      if (unlikely(open_error) && thd->locked_tables_mode)
      {
        result_code= HA_ADMIN_FAILED;
        goto send_result;
      }

      if (!table->table || table->mdl_request.type != MDL_SHARED_WRITE ||
          table->table->file->ha_table_flags() & HA_CONCURRENT_OPTIMIZE)
        break;

      trans_rollback_stmt(thd);
      trans_rollback(thd);
      close_thread_tables(thd);
      table->table= NULL;
      thd->release_transactional_locks();
      MDL_REQUEST_INIT(&table->mdl_request, MDL_key::TABLE, table->db.str,
                       table->table_name.str, MDL_SHARED_NO_READ_WRITE,
                       MDL_TRANSACTION);
    }

#ifdef WITH_PARTITION_STORAGE_ENGINE
      if (table->table)
      {
        /*
          Set up which partitions that should be processed
          if ALTER TABLE t ANALYZE/CHECK/OPTIMIZE/REPAIR PARTITION ..
          CACHE INDEX/LOAD INDEX for specified partitions
        */
        Alter_info *alter_info= &lex->alter_info;

        if (alter_info->partition_flags & ALTER_PARTITION_ADMIN)
        {
          if (!table->table->part_info)
          {
            my_error(ER_PARTITION_MGMT_ON_NONPARTITIONED, MYF(0));
            thd->resume_subsequent_commits(suspended_wfc);
            DBUG_RETURN(TRUE);
          }
          if (set_part_state(alter_info, table->table->part_info, PART_ADMIN))
          {
            char buff[FN_REFLEN + MYSQL_ERRMSG_SIZE];
            size_t length;
            DBUG_PRINT("admin", ("sending non existent partition error"));
            protocol->prepare_for_resend();
            protocol->store(&table_name, system_charset_info);
            protocol->store(operator_name, system_charset_info);
            protocol->store(&error_clex_str, system_charset_info);
            length= my_snprintf(buff, sizeof(buff), "%s",
                                ER_THD(thd, ER_PARTITION_DOES_NOT_EXIST));
            protocol->store(buff, length, system_charset_info);
            if(protocol->write())
              goto err;
            my_eof(thd);
            goto err;
          }
        }
      }
#endif
    DBUG_PRINT("admin", ("table: %p", table->table));

    if (table->schema_table)
    {
      result_code= HA_ADMIN_NOT_IMPLEMENTED;
      goto send_result;
    }

    if (prepare_func)
    {
      DBUG_PRINT("admin", ("calling prepare_func"));
      switch ((*prepare_func)(thd, table, check_opt)) {
      case  1:           // error, message written to net
        trans_rollback_stmt(thd);
        trans_rollback(thd);
        close_thread_tables(thd);
        thd->release_transactional_locks();
        DBUG_PRINT("admin", ("simple error, admin next table"));
        continue;
      case -1:           // error, message could be written to net
        /* purecov: begin inspected */
        DBUG_PRINT("admin", ("severe error, stop"));
        goto err;
        /* purecov: end */
      default:           // should be 0 otherwise
        DBUG_PRINT("admin", ("prepare_func succeeded"));
        ;
      }
    }

    /*
      CHECK/REPAIR TABLE command is only command where VIEW allowed here and
      this command use only temporary table method for VIEWs resolving =>
      there can't be VIEW tree substitution of join view => if opening table
      succeed then table->table will have real TABLE pointer as value (in
      case of join view substitution table->table can be 0, but here it is
      impossible)
    */
    if (!table->table)
    {
      DBUG_PRINT("admin", ("open table failed"));
      if (thd->get_stmt_da()->is_warning_info_empty())
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                     ER_CHECK_NO_SUCH_TABLE,
                     ER_THD(thd, ER_CHECK_NO_SUCH_TABLE));
      /* if it was a view will check md5 sum */
      if (table->view &&
          view_check(thd, table, check_opt) == HA_ADMIN_WRONG_CHECKSUM)
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                     ER_VIEW_CHECKSUM, ER_THD(thd, ER_VIEW_CHECKSUM));
      if (thd->get_stmt_da()->is_error() &&
          table_not_corrupt_error(thd->get_stmt_da()->sql_errno()))
        result_code= HA_ADMIN_FAILED;
      else
        /* Default failure code is corrupt table */
        result_code= HA_ADMIN_CORRUPT;
      goto send_result;
    }

    if (table->view)
    {
      DBUG_PRINT("admin", ("calling view_operator_func"));
      result_code= (*view_operator_func)(thd, table, check_opt);
      goto send_result;
    }

    if ((table->table->db_stat & HA_READ_ONLY) && open_for_modify &&
        operator_func != &handler::ha_analyze)
    {
      /* purecov: begin inspected */
      enum_sql_command save_sql_command= lex->sql_command;
      DBUG_PRINT("admin", ("sending error message"));
      protocol->prepare_for_resend();
      protocol->store(&table_name, system_charset_info);
      protocol->store(operator_name, system_charset_info);
      send_read_only_warning(thd, &error_clex_str, &table_name);
      trans_commit_stmt(thd);
      trans_commit(thd);
      close_thread_tables(thd);
      thd->release_transactional_locks();
      lex->reset_query_tables_list(FALSE);
      /*
        Restore Query_tables_list::sql_command value to make statement
        safe for re-execution.
      */
      lex->sql_command= save_sql_command;
      table->table=0;				// For query cache
      if (protocol->write())
	goto err;
      thd->get_stmt_da()->reset_diagnostics_area();
      continue;
      /* purecov: end */
    }

    /*
      This has to be tested separately from the following test as
      optimizer table takes a MDL_SHARED_WRITE lock but we want to
      log this to the ddl log.
    */

    if (lock_type == TL_WRITE && table->mdl_request.type >= MDL_SHARED_WRITE)
    {
      /* Store information about table for ddl log */
      storage_engine_partitioned= table->table->file->partition_engine();
      strmake(storage_engine_name, table->table->file->real_table_type(),
              sizeof(storage_engine_name)-1);
      tabledef_version.str= tabledef_version_buff;
      if ((tabledef_version.length= table->table->s->tabledef_version.length))
        memcpy((char*) tabledef_version.str,
               table->table->s->tabledef_version.str,
               MY_UUID_SIZE);
    }

    /*
      Close all instances of the table to allow MyISAM "repair"
      (which is internally also used from "optimize") to rename files.
      @todo: This code does not close all instances of the table.
      It only closes instances in other connections, but if this
      connection has LOCK TABLE t1 a READ, t1 b WRITE,
      both t1 instances will be kept open.

      Note that this code is only executed for engines that request
      MDL_SHARED_NO_READ_WRITE lock (MDL_SHARED_WRITE cannot be upgraded)
      by *not* having HA_CONCURRENT_OPTIMIZE table_flag.
    */
    if (lock_type == TL_WRITE && table->mdl_request.type > MDL_SHARED_WRITE)
    {
      if (table->table->s->tmp_table)
        thd->close_unused_temporary_table_instances(table);
      else
      {
        if (wait_while_table_is_used(thd, table->table, HA_EXTRA_NOT_USED))
          goto err;
        DEBUG_SYNC(thd, "after_admin_flush");
        /* Flush entries in the query cache involving this table. */
        query_cache_invalidate3(thd, table->table, 0);
        /*
          XXX: hack: switch off open_for_modify to skip the
          flush that is made later in the execution flow.
        */
        open_for_modify= 0;
      }
    }

    if (table->table->s->crashed && operator_func == &handler::ha_check)
    {
      /* purecov: begin inspected */
      DBUG_PRINT("admin", ("sending crashed warning"));
      protocol->prepare_for_resend();
      protocol->store(&table_name, system_charset_info);
      protocol->store(operator_name, system_charset_info);
      protocol->store(STRING_WITH_LEN("warning"), system_charset_info);
      protocol->store(STRING_WITH_LEN("Table is marked as crashed"),
                      system_charset_info);
      if (protocol->write())
        goto err;
      /* purecov: end */
    }

    if (operator_func == &handler::ha_repair)
    {
      handler *file= table->table->file;
      int check_for_upgrade= file->ha_check_for_upgrade(check_opt);
      require_data_conversion=
         check_for_upgrade == HA_ADMIN_NEEDS_DATA_CONVERSION;
      require_alter_table= check_for_upgrade == HA_ADMIN_NEEDS_ALTER;

      if (!(check_opt->sql_flags & (TT_USEFRM | TT_FORCE)))
      {
        if (require_data_conversion || require_alter_table)
        {
          /* We use extra_open_options to be able to open crashed tables */
          thd->open_options|= extra_open_options;
          result_code= (admin_recreate_table(thd, table, &recreate_info, 1) ?
                        HA_ADMIN_FAILED : HA_ADMIN_OK);
          recreate_used= 1;
          thd->open_options&= ~extra_open_options;
          goto send_result;
        }
        if (check_for_upgrade ||
            !(table->table->file->ha_table_flags() & HA_CAN_REPAIR))
        {
          /*
            If data upgrade is needed or repair is not implemented for the
            engine, run ALTER TABLE FORCE
          */
          need_repair_or_alter= 1;
        }
      }
    }
    result_code= compl_result_code= HA_ADMIN_OK;

    if (operator_func == &handler::ha_analyze)
    {
      TABLE *tab= table->table;

      if (lex->with_persistent_for_clause &&
          tab->s->table_category != TABLE_CATEGORY_USER)
      {
        compl_result_code= result_code= HA_ADMIN_INVALID;
      }

      /*
        The check for ALTER_PARTITION_ADMIN implements this logic:
        do not collect EITS STATS for this syntax:
          ALTER TABLE ... ANALYZE PARTITION p
        EITS statistics is global (not per-partition). Collecting global stats
        is much more expensive processing just one partition, so the most
        appropriate action is to just not collect EITS stats for this command.
      */
      collect_eis=
        (tab->s->table_category == TABLE_CATEGORY_USER && !tab->s->sequence &&
        !(lex->alter_info.partition_flags & ALTER_PARTITION_ADMIN) &&
         (check_eits_collection_allowed(thd) ||
          lex->with_persistent_for_clause));
    }

    if (result_code == HA_ADMIN_OK)
    {    
      DBUG_PRINT("admin", ("calling operator_func '%s'", operator_name->str));
      THD_STAGE_INFO(thd, stage_executing);
      result_code = (table->table->file->*operator_func)(thd, check_opt);
      THD_STAGE_INFO(thd, stage_sending_data);
      DBUG_PRINT("admin", ("operator_func returned: %d", result_code));
#ifdef WITH_PARTITION_STORAGE_ENGINE
      if (lex->alter_info.partition_flags & ALTER_PARTITION_ADMIN)
        set_part_state(&lex->alter_info, table->table->part_info, PART_NORMAL);
#endif
    }

    /* Note: compl_result_code can be different from result_code here */
    if (compl_result_code == HA_ADMIN_OK && collect_eis)
    {
      if (result_code == HA_ERR_TABLE_READONLY)
      {
        protocol->prepare_for_resend();
        protocol->store(&table_name, system_charset_info);
        protocol->store(operator_name, system_charset_info);
        send_read_only_warning(thd, &msg_status, &table_name);
        (void) protocol->write();
        result_code= HA_ADMIN_OK;
      }
      /*
        Here we close and reopen table in read mode because operation of
        collecting statistics is long and it will be better do not block
        the table completely.
        InnoDB will allow read/write and MyISAM read/insert.
      */
      trans_commit_stmt(thd);
      trans_commit(thd);
      thd->open_options|= extra_open_options;
      close_thread_tables(thd);
      table->table= NULL;
      thd->release_transactional_locks();
      MDL_REQUEST_INIT(&table->mdl_request, MDL_key::TABLE, table->db.str,
                       table->table_name.str, MDL_SHARED_NO_READ_WRITE,
                       MDL_TRANSACTION);
      table->mdl_request.set_type(MDL_SHARED_READ);

      table->lock_type= TL_READ;
      DBUG_ASSERT(view_operator_func == NULL);
      open_error= open_only_one_table(thd, table,
                                      no_errors_from_open, FALSE);
      thd->open_options&= ~extra_open_options;

      if (unlikely(!open_error))
      {
        TABLE *tab= table->table;
        Field **field_ptr= tab->field;
        MEM_ROOT_SAVEPOINT memroot_sv;

        if (!lex->column_list)
        {
          /* Fields we have to read from the engine */
          bitmap_clear_all(tab->read_set);
          /* Fields we want to have statistics for */
          bitmap_clear_all(&tab->has_value_set);

          for (uint fields= 0; *field_ptr; field_ptr++, fields++)
          {
            Field *field= *field_ptr;
            if (field->flags & LONG_UNIQUE_HASH_FIELD)
            {
              /*
                No point in doing statistic for hash fields that should be
                unique
              */
              continue;
            }
            /*
              Note that type() always return MYSQL_TYPE_BLOB for
              all blob types. Another function needs to be added
              if we in the future want to distinguish between blob
              types here.
            */
            enum enum_field_types type= field->type();
            if ((type < MYSQL_TYPE_TINY_BLOB ||
                 type > MYSQL_TYPE_BLOB) &&
                field->field_length <= thd->variables.analyze_max_length)
            {
              field->register_field_in_read_map();
              bitmap_set_bit(&tab->has_value_set, field->field_index);
            }
            else
              push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                                  ER_NO_EIS_FOR_FIELD,
                                  ER_THD(thd, ER_NO_EIS_FOR_FIELD),
                                  (*field_ptr)->field_name.str);
          }
        }
        else
        {
          int pos;
          LEX_STRING *column_name;
          List_iterator_fast<LEX_STRING> it(*lex->column_list);

          /* Fields we have to read from the engine */
          bitmap_clear_all(tab->read_set);
          /* Fields we want to have statistics for */
          bitmap_clear_all(&tab->has_value_set);

          while ((column_name= it++))
          {
            Field *field;
            enum enum_field_types type;
            if (tab->s->fieldnames.type_names == 0 ||
                (pos= find_type(&tab->s->fieldnames, column_name->str,
                                column_name->length, 1)) <= 0)
            {
              compl_result_code= result_code= HA_ADMIN_INVALID;
              break;
            }
            pos--;
            field= tab->field[pos];
            type= field->type();
            if (!(field->flags & LONG_UNIQUE_HASH_FIELD) &&
                (type < MYSQL_TYPE_TINY_BLOB ||
                 type > MYSQL_TYPE_BLOB))
            {
              field->register_field_in_read_map();
              bitmap_set_bit(&tab->has_value_set, field->field_index);
            }
            else
              push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                                  ER_NO_EIS_FOR_FIELD,
                                  ER_THD(thd, ER_NO_EIS_FOR_FIELD),
                                  column_name->str);
          }
          tab->file->column_bitmaps_signal();
        }
        if (!lex->index_list)
          tab->keys_in_use_for_query.init(tab->s->keys);
        else
        {
          int pos;
          LEX_STRING *index_name;
          List_iterator_fast<LEX_STRING> it(*lex->index_list);
          tab->keys_in_use_for_query.clear_all();
          while ((index_name= it++))
          {
            if (tab->s->keynames.type_names == 0 ||
                (pos= find_type(&tab->s->keynames, index_name->str,
                                index_name->length, 1)) <= 0)
            {
              compl_result_code= result_code= HA_ADMIN_INVALID;
              break;
            }
            tab->keys_in_use_for_query.set_bit(--pos);
          }
        }
        /* Ensure that number of records are updated */
        tab->file->info(HA_STATUS_VARIABLE);
        root_make_savepoint(thd->mem_root, &memroot_sv);
        if (!(compl_result_code=
              alloc_statistics_for_table(thd, tab,
                                         &tab->has_value_set)) &&
            !(compl_result_code=
              collect_statistics_for_table(thd, tab)))
          compl_result_code= update_statistics_for_table(thd, tab);
        free_statistics_for_table(tab);
        root_free_to_savepoint(&memroot_sv);
      }
      else
        compl_result_code= HA_ADMIN_FAILED;

      if (compl_result_code)
        result_code= HA_ADMIN_FAILED;
      else
      {
        protocol->prepare_for_resend();
        protocol->store(&table_name, system_charset_info);
        protocol->store(operator_name, system_charset_info);
        protocol->store(&msg_status, system_charset_info);
	protocol->store(STRING_WITH_LEN("Engine-independent statistics collected"), 
                        system_charset_info);
        if (protocol->write())
          goto err;
      }
    }

    if (result_code == HA_ADMIN_NOT_IMPLEMENTED && need_repair_or_alter)
    {
      /*
        repair was not implemented and we need to upgrade the table
        to a new version so we recreate the table with ALTER TABLE
      */
      result_code= admin_recreate_table(thd, table, &recreate_info, 1);
      recreate_used= 1;
    }

send_result:
    lex->cleanup_after_one_table_open();
    thd->clear_error();  // these errors shouldn't get client

    if (recreate_info.records_duplicate())
    {
      protocol->prepare_for_resend();
      protocol->store(&table_name, system_charset_info);
      protocol->store(operator_name, system_charset_info);
      protocol->store(warning_level_names[Sql_condition::WARN_LEVEL_WARN].str,
                      warning_level_names[Sql_condition::WARN_LEVEL_WARN].length,
                      system_charset_info);
      char buf[80];
      size_t length= my_snprintf(buf, sizeof(buf),
                                 "Number of rows changed from %u to %u",
                                 (uint) recreate_info.records_processed(),
                                 (uint) recreate_info.records_copied());
      protocol->store(buf, length, system_charset_info);
      if (protocol->write())
        goto err;
    }

    {
      Diagnostics_area::Sql_condition_iterator it=
        thd->get_stmt_da()->sql_conditions();
      const Sql_condition *err;
      while ((err= it++))
      {
        const char *err_msg= err->get_message_text();
        protocol->prepare_for_resend();
        protocol->store(&table_name, system_charset_info);
        protocol->store(operator_name, system_charset_info);
        protocol->store(warning_level_names[err->get_level()].str,
                        warning_level_names[err->get_level()].length,
                        system_charset_info);
        protocol->store(err_msg, strlen(err_msg), system_charset_info);
        if (protocol->write())
          goto err;
      }
      thd->get_stmt_da()->clear_warning_info(thd->query_id);
    }

    /*
      Give a warning if REPAIR TABLE was used but table still needs an
      ALTER TABLE. This can only happen for old type tables where REPAIR
      was using FORCE to recover old data.
    */
    if (operator_func == &handler::ha_repair && ! recreate_used &&
        (require_data_conversion || require_alter_table))
    {
      char buf[MYSQL_ERRMSG_SIZE];
      if (result_code == HA_ADMIN_OK)
      {
        protocol->prepare_for_resend();
        protocol->store(&table_name, system_charset_info);
        protocol->store(operator_name, system_charset_info);
	protocol->store(STRING_WITH_LEN("note"), system_charset_info);
        protocol->store(STRING_WITH_LEN("Table data recovered"),system_charset_info);
        if (protocol->write())
          goto err;
      }
      my_snprintf(buf, sizeof(buf),
                  ER_THD(thd, ER_TABLE_NEEDS_REBUILD),
                  table_name.str);
      if (send_check_errmsg(thd, table, operator_name, buf) < 0)
        goto err;
      result_code= HA_ADMIN_FAILED;
    }

    protocol->prepare_for_resend();
    protocol->store(&table_name, system_charset_info);
    protocol->store(operator_name, system_charset_info);

send_result_message:

    DBUG_PRINT("info", ("result_code: %d", result_code));
    switch (result_code) {
    case HA_ADMIN_NOT_IMPLEMENTED:
      {
       char buf[MYSQL_ERRMSG_SIZE];
       size_t length=my_snprintf(buf, sizeof(buf),
                                 ER_THD(thd, ER_CHECK_NOT_IMPLEMENTED),
                                 operator_name->str);
	protocol->store(STRING_WITH_LEN("note"), system_charset_info);
	protocol->store(buf, length, system_charset_info);
      }
      break;

    case HA_ADMIN_NOT_BASE_TABLE:
      {
        char buf[MYSQL_ERRMSG_SIZE];
        size_t length= my_snprintf(buf, sizeof(buf),
                                   ER_THD(thd, ER_BAD_TABLE_ERROR),
                                   table_name.str);
        protocol->store(STRING_WITH_LEN("note"), system_charset_info);
        protocol->store(buf, length, system_charset_info);
      }
      break;

    case HA_ADMIN_OK:
      protocol->store(&msg_status, system_charset_info);
      protocol->store(STRING_WITH_LEN("OK"), system_charset_info);
      break;

    case HA_ADMIN_FAILED:
      protocol->store(&msg_status, system_charset_info);
      protocol->store(STRING_WITH_LEN("Operation failed"),
                      system_charset_info);
      break;

    case HA_ADMIN_REJECT:
      protocol->store(&msg_status, system_charset_info);
      protocol->store(STRING_WITH_LEN("Operation need committed state"),
                      system_charset_info);
      open_for_modify= FALSE;
      break;

    case HA_ADMIN_ALREADY_DONE:
      protocol->store(&msg_status, system_charset_info);
      protocol->store(STRING_WITH_LEN("Table is already up to date"),
                      system_charset_info);
      break;

    case HA_ADMIN_CORRUPT:
      protocol->store(&error_clex_str, system_charset_info);
      protocol->store(STRING_WITH_LEN("Corrupt"), system_charset_info);
      fatal_error=1;
      break;

    case HA_ADMIN_INVALID:
      protocol->store(&error_clex_str, system_charset_info);
      protocol->store(STRING_WITH_LEN("Invalid argument"),
                      system_charset_info);
      break;

    case HA_ADMIN_TRY_ALTER:
    {
      Alter_info *alter_info= &lex->alter_info;

      protocol->store(STRING_WITH_LEN("note"), system_charset_info);
      if (alter_info->partition_flags & ALTER_PARTITION_ADMIN)
      {
        protocol->store(STRING_WITH_LEN(
        "Table does not support optimize on partitions. All partitions "
        "will be rebuilt and analyzed."),system_charset_info);
      }
      else
      {
        protocol->store(STRING_WITH_LEN(
        "Table does not support optimize, doing recreate + analyze instead"),
        system_charset_info);
      }
      if (protocol->write())
        goto err;
      THD_STAGE_INFO(thd, stage_recreating_table);
      DBUG_PRINT("info", ("HA_ADMIN_TRY_ALTER, trying analyze..."));
      TABLE_LIST *save_next_local= table->next_local,
                 *save_next_global= table->next_global;
      table->next_local= table->next_global= 0;

      result_code= admin_recreate_table(thd, table, &recreate_info, 0);
      recreate_used= 1;
      trans_commit_stmt(thd);
      trans_commit(thd);
      thd->tmp_table_binlog_handled= 1;
      close_thread_tables(thd);
      thd->release_transactional_locks();
      /* Clear references to TABLE and MDL_ticket after releasing them. */
      table->mdl_request.ticket= NULL;

      if (!result_code) // recreation went ok
      {
        /* Clear the ticket released above. */
        table->mdl_request.ticket= NULL;
        DEBUG_SYNC(thd, "ha_admin_open_ltable");
        table->mdl_request.set_type(MDL_SHARED_WRITE);
        if (!thd->open_temporary_tables(table) &&
            (table->table= open_ltable(thd, table, lock_type, 0)))
        {
          ulonglong save_flags;
          /* Store the original value of alter_info->flags */
          save_flags= alter_info->flags;

          /*
           Reset the ALTER_PARTITION_ADMIN bit in alter_info->flags
           to force analyze on all partitions.
          */
          alter_info->partition_flags &= ~(ALTER_PARTITION_ADMIN);
          result_code= table->table->file->ha_analyze(thd, check_opt);
          if (result_code == HA_ADMIN_ALREADY_DONE)
            result_code= HA_ADMIN_OK;
          else if (result_code)  // analyze failed
            table->table->file->print_error(result_code, MYF(0));
          alter_info->flags= save_flags;
        }
        else
          result_code= -1; // open failed
      }
      /* Start a new row for the final status row */
      protocol->prepare_for_resend();
      protocol->store(&table_name, system_charset_info);
      protocol->store(operator_name, system_charset_info);
      if (result_code) // either mysql_recreate_table or analyze failed
      {
        DBUG_ASSERT(thd->is_error());
        if (thd->is_error())
        {
          const char *err_msg= thd->get_stmt_da()->message();
          if (!thd->vio_ok())
          {
            sql_print_error("%s", err_msg);
          }
          else
          {
            /* Hijack the row already in-progress. */
            protocol->store(&error_clex_str, system_charset_info);
            protocol->store(err_msg, strlen(err_msg), system_charset_info);
            if (protocol->write())
              goto err;
            /* Start off another row for HA_ADMIN_FAILED */
            protocol->prepare_for_resend();
            protocol->store(&table_name, system_charset_info);
            protocol->store(operator_name, system_charset_info);
          }
          thd->clear_error();
        }
        /* Make sure this table instance is not reused after the operation. */
        if (table->table)
          table->table->mark_table_for_reopen();
      }
      result_code= result_code ? HA_ADMIN_FAILED : HA_ADMIN_OK;
      table->next_local= save_next_local;
      table->next_global= save_next_global;
      goto send_result_message;
    }
    case HA_ADMIN_WRONG_CHECKSUM:
    {
      protocol->store(STRING_WITH_LEN("note"), system_charset_info);
      protocol->store(ER_THD(thd, ER_VIEW_CHECKSUM),
                      strlen(ER_THD(thd, ER_VIEW_CHECKSUM)),
                      system_charset_info);
      break;
    }

    case HA_ADMIN_NEEDS_UPGRADE:
    case HA_ADMIN_NEEDS_DATA_CONVERSION:
    case HA_ADMIN_NEEDS_ALTER:
    {
      char buf[MYSQL_ERRMSG_SIZE];
      size_t length;
      const char *what_to_upgrade= table->view ? "VIEW" :
          table->table->file->ha_table_flags() & HA_CAN_REPAIR ? "TABLE" : 0;

      protocol->store(&error_clex_str, system_charset_info);
      if (what_to_upgrade && result_code == HA_ADMIN_NEEDS_UPGRADE)
        length= my_snprintf(buf, sizeof(buf),
                            ER_THD(thd, ER_TABLE_NEEDS_UPGRADE),
                            what_to_upgrade, table->table_name.str);
      else
        length= my_snprintf(buf, sizeof(buf),
                            ER_THD(thd, ER_TABLE_NEEDS_REBUILD),
                            table->table_name.str);
      protocol->store(buf, length, system_charset_info);
      fatal_error=1;
      break;
    }
    case HA_ERR_TABLE_READONLY:
    {
      send_read_only_warning(thd, &msg_status, &table_name);
      break;
    }
    default:				// Probably HA_ADMIN_INTERNAL_ERROR
      {
        char buf[MYSQL_ERRMSG_SIZE];
        size_t length=my_snprintf(buf, sizeof(buf),
                                "Unknown - internal error %d during operation",
                                result_code);
        protocol->store(&error_clex_str, system_charset_info);
        protocol->store(buf, length, system_charset_info);
        fatal_error=1;
        break;
      }
    }
    /*
      Admin commands acquire table locks and these locks are not detected by
      parallel replication deadlock detection-and-handling mechanism. Hence
      they must be marked as DDL so that they are not scheduled in parallel
      with conflicting DMLs resulting in deadlock.
    */
    thd->transaction->stmt.mark_executed_table_admin_cmd();

    if (table->table && !table->view)
    {
      /* Skip FLUSH TABLES if we are doing analyze */
      const bool skip_flush= (operator_func == &handler::ha_analyze);
      if (table->table->s->tmp_table)
      {
        /*
          If the table was not opened successfully, do not try to get
          status information. (Bug#47633)
        */
        if (open_for_modify && !open_error)
          table->table->file->info(HA_STATUS_CONST);
      }
      else if ((!skip_flush && open_for_modify) || fatal_error)
      {
        table->table->s->tdc->flush_unused(true);
        /*
          May be something modified. Consequently, we have to
          invalidate the query cache.
        */
        table->table= 0;                        // For query cache
        query_cache_invalidate3(thd, table, 0);
      }
      else if (collect_eis && skip_flush && compl_result_code == HA_ADMIN_OK)
      {
        TABLE_LIST *save_next_global= table->next_global;
        table->next_global= 0;
        read_statistics_for_tables(thd, table, true /* force_reload */);
        table->next_global= save_next_global;
      }
    }
    /* Error path, a admin command failed. */
    if (thd->transaction_rollback_request || fatal_error)
    {
      /*
        Unlikely, but transaction rollback was requested by one of storage
        engines (e.g. due to deadlock). Perform it.
      */
      if (trans_rollback_stmt(thd) || trans_rollback_implicit(thd))
        goto err;
    }
    else
    {
      if (trans_commit_stmt(thd))
        goto err;
      is_table_modified= true;
    }
    thd->tmp_table_binlog_handled= 1;
    close_thread_tables(thd);

    if (storage_engine_name[0])
    {
      /* Table was changed (repair, optimize or something similar) */
      backup_log_info ddl_log;
      bzero(&ddl_log, sizeof(ddl_log));
      lex_string_set(&ddl_log.org_storage_engine_name,
                     storage_engine_name);
      ddl_log.query=            *operator_name;
      ddl_log.org_partitioned=  storage_engine_partitioned;
      ddl_log.org_database=     table->db;
      ddl_log.org_table=        table->table_name;
      ddl_log.org_table_id=     tabledef_version;
      if (recreate_used)
      {
        LEX_CUSTRING tabledef_version=
          { recreate_info.tabledef_version, MY_UUID_SIZE };
        ddl_log.new_database=     table->db;
        ddl_log.new_table=        table->table_name;
        ddl_log.new_table_id=     tabledef_version;
      }
      backup_log_ddl(&ddl_log);
    }

    thd->release_transactional_locks();

    /*
      If it is CHECK TABLE v1, v2, v3, and v1, v2, v3 are views, we will run
      separate open_tables() for each CHECK TABLE argument.
      Right now we do not have a separate method to reset the prelocking
      state in the lex to the state after parsing, so each open will pollute
      this state: add elements to lex->srotuines_list, TABLE_LISTs to
      lex->query_tables. Below is a lame attempt to recover from this
      pollution.
      @todo: have a method to reset a prelocking context, or use separate
      contexts for each open.
    */
    for (Sroutine_hash_entry *rt=
           (Sroutine_hash_entry*)thd->lex->sroutines_list.first;
         rt; rt= rt->next)
      rt->mdl_request.ticket= NULL;

    if (protocol->write())
      goto err;
    DEBUG_SYNC(thd, "admin_command_kill_after_modify");
  }
  thd->resume_subsequent_commits(suspended_wfc);
  DBUG_EXECUTE_IF("inject_analyze_table_sleep", my_sleep(500000););
  if (is_table_modified && is_cmd_replicated &&
      (!opt_readonly || thd->slave_thread) && !thd->lex->no_write_to_binlog)
  {
    thd->get_stmt_da()->set_overwrite_status(true);
    auto res= write_bin_log(thd, true, thd->query(), thd->query_length());
    thd->get_stmt_da()->set_overwrite_status(false);
    if (res)
      goto err;
  }
  else
  {
    /*
      We decided to not log the query to binlog.
      We mark the query as logged to ensure that temporary tables are not
      marked with 'mark_as_not_binlogged()' on close.
    */
    thd->tmp_table_binlog_handled= 1;
  }

  my_eof(thd);

  DBUG_RETURN(FALSE);

err:
  /* Make sure this table instance is not reused after the failure. */
  trans_rollback_stmt(thd);
  if (stmt_causes_implicit_commit(thd, CF_IMPLICIT_COMMIT_END))
    trans_rollback(thd);
  if (table && table->table)
  {
    table->table->mark_table_for_reopen();
    table->table->mark_as_not_binlogged();
    table->table= 0;
  }
  close_thread_tables(thd);			// Shouldn't be needed
  thd->release_transactional_locks();
  thd->resume_subsequent_commits(suspended_wfc);
  DBUG_RETURN(TRUE);
}


/*
  Assigned specified indexes for a table into key cache

  SYNOPSIS
    mysql_assign_to_keycache()
    thd		Thread object
    tables	Table list (one table only)

  RETURN VALUES
   FALSE ok
   TRUE  error
*/

bool mysql_assign_to_keycache(THD* thd, TABLE_LIST* tables,
			     const LEX_CSTRING *key_cache_name)
{
  HA_CHECK_OPT check_opt;
  KEY_CACHE *key_cache;
  DBUG_ENTER("mysql_assign_to_keycache");

  THD_STAGE_INFO(thd, stage_finding_key_cache);
  check_opt.init();
  mysql_mutex_lock(&LOCK_global_system_variables);
  if (!(key_cache= get_key_cache(key_cache_name)))
  {
    mysql_mutex_unlock(&LOCK_global_system_variables);
    my_error(ER_UNKNOWN_KEY_CACHE, MYF(0), key_cache_name->str);
    DBUG_RETURN(TRUE);
  }
  mysql_mutex_unlock(&LOCK_global_system_variables);
  if (!key_cache->key_cache_inited)
  {
    my_error(ER_UNKNOWN_KEY_CACHE, MYF(0), key_cache_name->str);
    DBUG_RETURN(true);
  }
  check_opt.key_cache= key_cache;
  DBUG_RETURN(mysql_admin_table(thd, tables, &check_opt,
                                &msg_assign_to_keycache, TL_READ_NO_INSERT, 0,
                                0, 0, 0,
                                &handler::assign_to_keycache, 0, false));
}


/*
  Preload specified indexes for a table into key cache

  SYNOPSIS
    mysql_preload_keys()
    thd		Thread object
    tables	Table list (one table only)

  RETURN VALUES
    FALSE ok
    TRUE  error
*/

bool mysql_preload_keys(THD* thd, TABLE_LIST* tables)
{
  DBUG_ENTER("mysql_preload_keys");
  /*
    We cannot allow concurrent inserts. The storage engine reads
    directly from the index file, bypassing the cache. It could read
    outdated information if parallel inserts into cache blocks happen.
  */
  DBUG_RETURN(mysql_admin_table(thd, tables, 0,
                                &msg_preload_keys, TL_READ_NO_INSERT,
                                0, 0, 0, 0,
                                &handler::preload_keys, 0, false));
}


bool Sql_cmd_analyze_table::execute(THD *thd)
{
  LEX *m_lex= thd->lex;
  TABLE_LIST *first_table= m_lex->first_select_lex()->table_list.first;
  bool res= TRUE;
  thr_lock_type lock_type = TL_READ_NO_INSERT;
  DBUG_ENTER("Sql_cmd_analyze_table::execute");

  if (check_table_access(thd, SELECT_ACL | INSERT_ACL, first_table,
                         FALSE, UINT_MAX, FALSE))
    goto error;
  if (thd->has_read_only_protection())
    goto error;

  WSREP_TO_ISOLATION_BEGIN_WRTCHK(NULL, NULL, first_table);
  res= mysql_admin_table(thd, first_table, &m_lex->check_opt,
                         &msg_analyze, lock_type, 1, 0, 0, 0,
                         &handler::ha_analyze, 0, true);
  m_lex->first_select_lex()->table_list.first= first_table;
  m_lex->query_tables= first_table;

#ifdef WITH_WSREP
 wsrep_error_label:
#endif /* WITH_WSREP */
error:
  DBUG_RETURN(res);
}


bool Sql_cmd_check_table::execute(THD *thd)
{
  LEX *m_lex= thd->lex;
  TABLE_LIST *first_table= m_lex->first_select_lex()->table_list.first;
  thr_lock_type lock_type = TL_READ_NO_INSERT;
  bool res= TRUE;
  DBUG_ENTER("Sql_cmd_check_table::execute");

  if (check_table_access(thd, SELECT_ACL, first_table,
                         TRUE, UINT_MAX, FALSE))
    goto error; /* purecov: inspected */

  res= mysql_admin_table(thd, first_table, &m_lex->check_opt, &msg_check,
                         lock_type, 0, 0, HA_OPEN_FOR_REPAIR, 0,
                         &handler::ha_check, &view_check, false);

  m_lex->first_select_lex()->table_list.first= first_table;
  m_lex->query_tables= first_table;

error:
  DBUG_RETURN(res);
}


bool Sql_cmd_optimize_table::execute(THD *thd)
{
  LEX *m_lex= thd->lex;
  TABLE_LIST *first_table= m_lex->first_select_lex()->table_list.first;
  bool res= TRUE;
  Recreate_info recreate_info;
  DBUG_ENTER("Sql_cmd_optimize_table::execute");

  if (check_table_access(thd, SELECT_ACL | INSERT_ACL, first_table,
                         FALSE, UINT_MAX, FALSE))
    goto error; /* purecov: inspected */

  WSREP_TO_ISOLATION_BEGIN_WRTCHK(NULL, NULL, first_table);
  res= (specialflag & SPECIAL_NO_NEW_FUNC) ?
    mysql_recreate_table(thd, first_table, &recreate_info,
                         false) :
    mysql_admin_table(thd, first_table, &m_lex->check_opt,
                      &msg_optimize, TL_WRITE, 1, 0, 0, 0,
                      &handler::ha_optimize, 0, true);
  m_lex->first_select_lex()->table_list.first= first_table;
  m_lex->query_tables= first_table;

#ifdef WITH_WSREP
wsrep_error_label:
#endif /* WITH_WSREP */
error:
  DBUG_RETURN(res);
}


bool Sql_cmd_repair_table::execute(THD *thd)
{
  LEX *m_lex= thd->lex;
  TABLE_LIST *first_table= m_lex->first_select_lex()->table_list.first;
  bool res= TRUE;
  DBUG_ENTER("Sql_cmd_repair_table::execute");

  if (check_table_access(thd, SELECT_ACL | INSERT_ACL, first_table,
                         FALSE, UINT_MAX, FALSE))
    goto error; /* purecov: inspected */
  WSREP_TO_ISOLATION_BEGIN_WRTCHK(NULL, NULL, first_table);
  res= mysql_admin_table(thd, first_table, &m_lex->check_opt, &msg_repair,
                         TL_WRITE, 1,
                         MY_TEST(m_lex->check_opt.sql_flags &
                                 (TT_USEFRM | TT_FORCE)),
                         HA_OPEN_FOR_REPAIR, &prepare_for_repair,
                         &handler::ha_repair, &view_repair, true);

  m_lex->first_select_lex()->table_list.first= first_table;
  m_lex->query_tables= first_table;

#ifdef WITH_WSREP
wsrep_error_label:
#endif /* WITH_WSREP */
error:
  DBUG_RETURN(res);
}
