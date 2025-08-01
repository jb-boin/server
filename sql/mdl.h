#ifndef MDL_H
#define MDL_H
/* Copyright (c) 2009, 2012, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2020, 2021, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include "sql_plist.h"
#include "ilist.h"
#include <my_sys.h>
#include <m_string.h>
#include <mysql_com.h>
#include <lf.h>
#include <atomic>
#include "lex_ident.h"

class THD;

class MDL_context;
class MDL_lock;
class MDL_ticket;

typedef unsigned short mdl_bitmap_t;


/**
  Get a bit corresponding to enum_mdl_type value in a granted/waiting bitmaps
  and compatibility matrices.
*/
#define MDL_BIT(A) static_cast<mdl_bitmap_t>(1U << A)


/**
  @def ENTER_COND(C, M, S, O)
  Start a wait on a condition.
  @param C the condition to wait on
  @param M the associated mutex
  @param S the new stage to enter
  @param O the previous stage
  @sa EXIT_COND().
*/
#define ENTER_COND(C, M, S, O) enter_cond(C, M, S, O, __func__, __FILE__, __LINE__)

/**
  @def EXIT_COND(S)
  End a wait on a condition
  @param S the new stage to enter
*/
#define EXIT_COND(S) exit_cond(S, __func__, __FILE__, __LINE__)

/**
   An interface to separate the MDL module from the THD, and the rest of the
   server code.
 */

class MDL_context_owner
{
public:
  virtual ~MDL_context_owner() = default;

  /**
    Enter a condition wait.
    For @c enter_cond() / @c exit_cond() to work the mutex must be held before
    @c enter_cond(); this mutex is then released by @c exit_cond().
    Usage must be: lock mutex; enter_cond(); your code; exit_cond().
    @param cond the condition to wait on
    @param mutex the associated mutex
    @param [in] stage the stage to enter, or NULL
    @param [out] old_stage the previous stage, or NULL
    @param src_function function name of the caller
    @param src_file file name of the caller
    @param src_line line number of the caller
    @sa ENTER_COND(), THD::enter_cond()
    @sa EXIT_COND(), THD::exit_cond()
  */
  virtual void enter_cond(mysql_cond_t *cond, mysql_mutex_t *mutex,
                          const PSI_stage_info *stage, PSI_stage_info *old_stage,
                          const char *src_function, const char *src_file,
                          int src_line) = 0;

  /**
    @def EXIT_COND(S)
    End a wait on a condition
    @param [in] stage the new stage to enter
    @param src_function function name of the caller
    @param src_file file name of the caller
    @param src_line line number of the caller
    @sa ENTER_COND(), THD::enter_cond()
    @sa EXIT_COND(), THD::exit_cond()
  */
  virtual void exit_cond(const PSI_stage_info *stage,
                         const char *src_function, const char *src_file,
                         int src_line) = 0;
  /**
     Has the owner thread been killed?
   */
  virtual int  is_killed() = 0;

  /**
     This one is only used for DEBUG_SYNC.
     (Do not use it to peek/poke into other parts of THD.)
   */
  virtual THD* get_thd() = 0;

  /**
     @see THD::notify_shared_lock()
   */
  virtual bool notify_shared_lock(MDL_context_owner *in_use,
                                  bool needs_thr_lock_abort,
                                  bool needs_non_slave_abort) = 0;
};

/**
  Type of metadata lock request.

  @sa Comments for MDL_object_lock::can_grant_lock() and
      MDL_scoped_lock::can_grant_lock() for details.

  Scoped locks are database (or schema) locks.
  The object locks are for tables, triggers etc.
*/

enum enum_mdl_type {
  /* This means that the MDL_request is not initialized */
  MDL_NOT_INITIALIZED= -1,
  /*
    An intention exclusive metadata lock (IX). Used only for scoped locks.
    Owner of this type of lock can acquire upgradable exclusive locks on
    individual objects.
    Compatible with other IX locks, but is incompatible with scoped S and
    X locks.
    IX lock is taken in SCHEMA namespace when we intend to modify
    object metadata. Object may refer table, stored procedure, trigger,
    view/etc.
  */
  MDL_INTENTION_EXCLUSIVE= 0,
  /*
    A shared metadata lock (S).
    To be used in cases when we are interested in object metadata only
    and there is no intention to access object data (e.g. for stored
    routines or during preparing prepared statements).
    We also mis-use this type of lock for open HANDLERs, since lock
    acquired by this statement has to be compatible with lock acquired
    by LOCK TABLES ... WRITE statement, i.e. SNRW (We can't get by by
    acquiring S lock at HANDLER ... OPEN time and upgrading it to SR
    lock for HANDLER ... READ as it doesn't solve problem with need
    to abort DML statements which wait on table level lock while having
    open HANDLER in the same connection).
    To avoid deadlock which may occur when SNRW lock is being upgraded to
    X lock for table on which there is an active S lock which is owned by
    thread which waits in its turn for table-level lock owned by thread
    performing upgrade we have to use thr_abort_locks_for_thread()
    facility in such situation.
    This problem does not arise for locks on stored routines as we don't
    use SNRW locks for them. It also does not arise when S locks are used
    during PREPARE calls as table-level locks are not acquired in this
    case.
    This lock is taken for global read lock, when caching a stored
    procedure in memory for the duration of the transaction and for
    tables used by prepared statements.
  */
  MDL_SHARED,
  /*
    A high priority shared metadata lock.
    Used for cases when there is no intention to access object data (i.e.
    data in the table).
    "High priority" means that, unlike other shared locks, it is granted
    ignoring pending requests for exclusive locks. Intended for use in
    cases when we only need to access metadata and not data, e.g. when
    filling an INFORMATION_SCHEMA table.
    Since SH lock is compatible with SNRW lock, the connection that
    holds SH lock lock should not try to acquire any kind of table-level
    or row-level lock, as this can lead to a deadlock. Moreover, after
    acquiring SH lock, the connection should not wait for any other
    resource, as it might cause starvation for X locks and a potential
    deadlock during upgrade of SNW or SNRW to X lock (e.g. if the
    upgrading connection holds the resource that is being waited for).
  */
  MDL_SHARED_HIGH_PRIO,
  /*
    A shared metadata lock (SR) for cases when there is an intention to read
    data from table.
    A connection holding this kind of lock can read table metadata and read
    table data (after acquiring appropriate table and row-level locks).
    This means that one can only acquire TL_READ, TL_READ_NO_INSERT, and
    similar table-level locks on table if one holds SR MDL lock on it.
    To be used for tables in SELECTs, subqueries, and LOCK TABLE ...  READ
    statements.
  */
  MDL_SHARED_READ,
  /*
    A shared metadata lock (SW) for cases when there is an intention to modify
    (and not just read) data in the table.
    A connection holding SW lock can read table metadata and modify or read
    table data (after acquiring appropriate table and row-level locks).
    To be used for tables to be modified by INSERT, UPDATE, DELETE
    statements, but not LOCK TABLE ... WRITE or DDL). Also taken by
    SELECT ... FOR UPDATE.
  */
  MDL_SHARED_WRITE,
  /*
    An upgradable shared metadata lock for cases when there is an
    intention to modify (and not just read) data in the table.
    Can be upgraded to MDL_SHARED_NO_WRITE and MDL_EXCLUSIVE.
    A connection holding SU lock can read table metadata and modify or read
    table data (after acquiring appropriate table and row-level locks).
    To be used for the first phase of ALTER TABLE.
  */
  MDL_SHARED_UPGRADABLE,
  /*
    A shared metadata lock for cases when we need to read data from table
    and block all concurrent modifications to it (for both data and metadata).
    Used by LOCK TABLES READ statement.
  */
  MDL_SHARED_READ_ONLY,
  /*
    An upgradable shared metadata lock which blocks all attempts to update
    table data, allowing reads.
    A connection holding this kind of lock can read table metadata and read
    table data.
    Can be upgraded to X metadata lock.
    Note, that since this type of lock is not compatible with SNRW or SW
    lock types, acquiring appropriate engine-level locks for reading
    (TL_READ* for MyISAM, shared row locks in InnoDB) should be
    contention-free.
    To be used for the first phase of ALTER TABLE, when copying data between
    tables, to allow concurrent SELECTs from the table, but not UPDATEs.
  */
  MDL_SHARED_NO_WRITE,
  /*
    An upgradable shared metadata lock which allows other connections
    to access table metadata, but not data.
    It blocks all attempts to read or update table data, while allowing
    INFORMATION_SCHEMA and SHOW queries.
    A connection holding this kind of lock can read table metadata modify and
    read table data.
    Can be upgraded to X metadata lock.
    To be used for LOCK TABLES WRITE statement.
    Not compatible with any other lock type except S and SH.
  */
  MDL_SHARED_NO_READ_WRITE,
  /*
    An exclusive metadata lock (X).
    A connection holding this lock can modify both table's metadata and data.
    No other type of metadata lock can be granted while this lock is held.
    To be used for CREATE/DROP/RENAME TABLE statements and for execution of
    certain phases of other DDL statements.
  */
  MDL_EXCLUSIVE,
  /* This should be the last !!! */
  MDL_TYPE_END
};


/** Backup locks */

/**
  Block concurrent backup
*/
#define MDL_BACKUP_START enum_mdl_type(0)
/**
   Block new write requests to non transactional tables
*/
#define MDL_BACKUP_FLUSH enum_mdl_type(1)
/**
   In addition to previous locks, blocks running requests to non trans tables
   Used to wait until all DML usage of on trans tables are finished
*/
#define MDL_BACKUP_WAIT_FLUSH enum_mdl_type(2)
/**
   In addition to previous locks, blocks new DDL's from starting
*/
#define MDL_BACKUP_WAIT_DDL enum_mdl_type(3)
/**
   In addition to previous locks, blocks commits
*/
#define MDL_BACKUP_WAIT_COMMIT enum_mdl_type(4)

/**
  Blocks (or is blocked by) statements that intend to modify data. Acquired
  before commit lock by FLUSH TABLES WITH READ LOCK.
*/
#define MDL_BACKUP_FTWRL1 enum_mdl_type(5)

/**
  Blocks (or is blocked by) commits. Acquired after global read lock by
  FLUSH TABLES WITH READ LOCK.
*/
#define MDL_BACKUP_FTWRL2 enum_mdl_type(6)

#define MDL_BACKUP_DML enum_mdl_type(7)
#define MDL_BACKUP_TRANS_DML enum_mdl_type(8)
#define MDL_BACKUP_SYS_DML enum_mdl_type(9)

/**
  Must be acquired by DDL statements that intend to modify data.
  Currently it's also used for LOCK TABLES.
*/
#define MDL_BACKUP_DDL enum_mdl_type(10)

/**
   Blocks new DDL's. Used by backup code to enable DDL logging
*/
#define MDL_BACKUP_BLOCK_DDL enum_mdl_type(11)

/*
  Statement is modifying data, but will not block MDL_BACKUP_DDL or earlier
  BACKUP stages.
  ALTER TABLE is started with MDL_BACKUP_DDL, but changed to
  MDL_BACKUP_ALTER_COPY while alter table is copying or modifying data.
*/

#define MDL_BACKUP_ALTER_COPY enum_mdl_type(12)

/**
  Must be acquired during commit.
*/
#define MDL_BACKUP_COMMIT enum_mdl_type(13)
#define MDL_BACKUP_END enum_mdl_type(14)


/** Duration of metadata lock. */

enum enum_mdl_duration {
  /**
    Locks with statement duration are automatically released at the end
    of statement or transaction.
  */
  MDL_STATEMENT= 0,
  /**
    Locks with transaction duration are automatically released at the end
    of transaction.
  */
  MDL_TRANSACTION,
  /**
    Locks with explicit duration survive the end of statement and transaction.
    They have to be released explicitly by calling MDL_context::release_lock().
  */
  MDL_EXPLICIT,
  /* This should be the last ! */
  MDL_DURATION_END };


/** Maximal length of key for metadata locking subsystem. */
#define MAX_MDLKEY_LENGTH (1 + NAME_LEN + 1 + NAME_LEN + 1)


/**
  Metadata lock object key.

  A lock is requested or granted based on a fully qualified name and type.
  E.g. They key for a table consists of <0 (=table)>+<database>+<table name>.
  Elsewhere in the comments this triple will be referred to simply as "key"
  or "name".
*/

struct MDL_key
{
public:
#ifdef HAVE_PSI_INTERFACE
  static void init_psi_keys();
#endif

  /**
    Object namespaces.
    Sic: when adding a new member to this enum make sure to
    update m_namespace_to_wait_state_name array in mdl.cc and
    metadata_lock_info_lock_name in metadata_lock_info.cc!

    Different types of objects exist in different namespaces
     - SCHEMA is for databases (to protect against DROP DATABASE)
     - TABLE is for tables and views.
     - BACKUP is for locking DML, DDL and COMMIT's during BACKUP STAGES
     - FUNCTION is for stored functions.
     - PROCEDURE is for stored procedures.
     - TRIGGER is for triggers.
     - EVENT is for event scheduler events
    Note that although there isn't metadata locking on triggers,
    it's necessary to have a separate namespace for them since
    MDL_key is also used outside of the MDL subsystem.
  */
  enum enum_mdl_namespace { BACKUP=0,
                            SCHEMA,
                            TABLE,
                            FUNCTION,
                            PROCEDURE,
                            PACKAGE_BODY,
                            TRIGGER,
                            EVENT,
                            USER_LOCK,           /* user level locks. */
                            /* This should be the last ! */
                            NAMESPACE_END };

  const uchar *ptr() const { return (uchar*) m_ptr; }
  uint length() const { return m_length; }

  const char *db_name() const { return m_ptr + 1; }
  uint db_name_length() const { return m_db_name_length; }

  const char *name() const { return m_ptr + m_db_name_length + 2; }
  uint name_length() const { return m_length - m_db_name_length - 3; }

  enum_mdl_namespace mdl_namespace() const
  { return (enum_mdl_namespace)(m_ptr[0]); }

  /**
    Construct a metadata lock key from a triplet (mdl_namespace,
    database and name).

    @remark The key for a table is <mdl_namespace>+<database name>+<table name>

    @param  mdl_namespace Id of namespace of object to be locked
    @param  db            Name of database to which the object belongs
    @param  name          Name of of the object
    @param  key           Where to store the the MDL key.
  */
  void mdl_key_init(enum_mdl_namespace mdl_namespace_arg,
                    const char *db, const char *name_arg)
  {
    m_ptr[0]= (char) mdl_namespace_arg;
    /*
      It is responsibility of caller to ensure that db and object names
      are not longer than NAME_LEN. Still we play safe and try to avoid
      buffer overruns.
    */
    DBUG_ASSERT(strlen(db) <= NAME_LEN);
    DBUG_ASSERT(strlen(name_arg) <= NAME_LEN);
    m_db_name_length= static_cast<uint16>(strmake(m_ptr + 1, db, NAME_LEN) -
                                          m_ptr - 1);
    m_length= static_cast<uint16>(strmake(m_ptr + m_db_name_length + 2,
                                          name_arg,
                                          NAME_LEN) - m_ptr + 1);
    m_hash_value= my_hash_sort(&my_charset_bin, (uchar*) m_ptr + 1,
                               m_length - 1);
    DBUG_SLOW_ASSERT(mdl_namespace_arg == USER_LOCK ||
                     Lex_ident_fs(db, m_db_name_length).
                       ok_for_lower_case_names());
  }
  void mdl_key_init(const MDL_key *rhs)
  {
    memcpy(m_ptr, rhs->m_ptr, rhs->m_length);
    m_length= rhs->m_length;
    m_db_name_length= rhs->m_db_name_length;
    m_hash_value= rhs->m_hash_value;
  }
  bool is_equal(const MDL_key *rhs) const
  {
    return (m_length == rhs->m_length &&
            memcmp(m_ptr, rhs->m_ptr, m_length) == 0);
  }
  /**
    Compare two MDL keys lexicographically.
  */
  int cmp(const MDL_key *rhs) const
  {
    /*
      The key buffer is always '\0'-terminated. Since key
      character set is utf-8, we can safely assume that no
      character starts with a zero byte.
    */
    return memcmp(m_ptr, rhs->m_ptr, MY_MIN(m_length, rhs->m_length));
  }

  MDL_key(const MDL_key *rhs)
  {
    mdl_key_init(rhs);
  }
  MDL_key(enum_mdl_namespace namespace_arg,
          const char *db_arg, const char *name_arg)
  {
    mdl_key_init(namespace_arg, db_arg, name_arg);
  }
  MDL_key() = default; /* To use when part of MDL_request. */

  /**
    Get thread state name to be used in case when we have to
    wait on resource identified by key.
  */
  const PSI_stage_info * get_wait_state_name() const
  {
    return & m_namespace_to_wait_state_name[(int)mdl_namespace()];
  }
  my_hash_value_type hash_value() const
  {
    return m_hash_value + mdl_namespace();
  }
  my_hash_value_type tc_hash_value() const
  {
    return m_hash_value;
  }

private:
  uint16 m_length;
  uint16 m_db_name_length;
  my_hash_value_type m_hash_value;
  char m_ptr[MAX_MDLKEY_LENGTH];
  static PSI_stage_info m_namespace_to_wait_state_name[NAMESPACE_END];
private:
  MDL_key(const MDL_key &);                     /* not implemented */
  MDL_key &operator=(const MDL_key &);          /* not implemented */
  friend my_hash_value_type mdl_hash_function(CHARSET_INFO *,
                                              const uchar *, size_t);
};


/**
  A pending metadata lock request.

  A lock request and a granted metadata lock are represented by
  different classes because they have different allocation
  sites and hence different lifetimes. The allocation of lock requests is
  controlled from outside of the MDL subsystem, while allocation of granted
  locks (tickets) is controlled within the MDL subsystem.

  MDL_request is a C structure, you don't need to call a constructor
  or destructor for it.
*/

class MDL_request
{
public:
  /** Type of metadata lock. */
  enum          enum_mdl_type type;
  /** Duration for requested lock. */
  enum enum_mdl_duration duration;

  /**
    Pointers for participating in the list of lock requests for this context.
  */
  MDL_request *next_in_list;
  MDL_request **prev_in_list;
  /**
    Pointer to the lock ticket object for this lock request.
    Valid only if this lock request is satisfied.
  */
  MDL_ticket *ticket;

  /** A lock is requested based on a fully qualified name and type. */
  MDL_key key;

  const char *m_src_file;
  uint m_src_line;

public:

  static void *operator new(size_t size, MEM_ROOT *mem_root) throw ()
  { return alloc_root(mem_root, size); }
  static void operator delete(void *, MEM_ROOT *) {}

  void init_with_source(MDL_key::enum_mdl_namespace namespace_arg,
            const char *db_arg, const char *name_arg,
            enum_mdl_type mdl_type_arg,
            enum_mdl_duration mdl_duration_arg,
            const char *src_file, uint src_line);
  void init_by_key_with_source(const MDL_key *key_arg, enum_mdl_type mdl_type_arg,
            enum_mdl_duration mdl_duration_arg,
            const char *src_file, uint src_line);
  /** Set type of lock request. Can be only applied to pending locks. */
  inline void set_type(enum_mdl_type type_arg)
  {
    DBUG_ASSERT(ticket == NULL);
    type= type_arg;
  }
  void move_from(MDL_request &from)
  {
    type= from.type;
    duration= from.duration;
    ticket= from.ticket;
    next_in_list= from.next_in_list;
    prev_in_list= from.prev_in_list;
    key.mdl_key_init(&from.key);
    from.ticket=  NULL; // that's what "move" means
  }

  /**
    Is this a request for a lock which allow data to be updated?

    @note This method returns true for MDL_SHARED_UPGRADABLE type of
          lock. Even though this type of lock doesn't allow updates
          it will always be upgraded to one that does.
  */
  bool is_write_lock_request() const
  {
    return (type >= MDL_SHARED_WRITE &&
            type != MDL_SHARED_READ_ONLY);
  }

  /*
    This is to work around the ugliness of TABLE_LIST
    compiler-generated assignment operator. It is currently used
    in several places to quickly copy "most" of the members of the
    table list. These places currently never assume that the mdl
    request is carried over to the new TABLE_LIST, or shared
    between lists.

    This method does not initialize the instance being assigned!
    Use of init() for initialization after this assignment operator
    is mandatory. Can only be used before the request has been
    granted.
  */
  MDL_request& operator=(const MDL_request &)
  {
    type= MDL_NOT_INITIALIZED;
    ticket= NULL;
    /* Do nothing, in particular, don't try to copy the key. */
    return *this;
  }
  /* Another piece of ugliness for TABLE_LIST constructor */
  MDL_request(): type(MDL_NOT_INITIALIZED), ticket(NULL) {}

  MDL_request(const MDL_request *rhs)
    :type(rhs->type),
    duration(rhs->duration),
    ticket(NULL),
    key(&rhs->key)
  {}
};


typedef void (*mdl_cached_object_release_hook)(void *);

#define MDL_REQUEST_INIT(R, P1, P2, P3, P4, P5) \
  (*R).init_with_source(P1, P2, P3, P4, P5, __FILE__, __LINE__)

#define MDL_REQUEST_INIT_BY_KEY(R, P1, P2, P3) \
  (*R).init_by_key_with_source(P1, P2, P3, __FILE__, __LINE__)


/**
  An abstract class for inspection of a connected
  subgraph of the wait-for graph.
*/

class MDL_wait_for_graph_visitor
{
public:
  virtual bool enter_node(MDL_context *node) = 0;
  virtual void leave_node(MDL_context *node) = 0;

  virtual bool inspect_edge(MDL_context *dest) = 0;
  virtual ~MDL_wait_for_graph_visitor();
  MDL_wait_for_graph_visitor() = default;
};

/**
  Abstract class representing an edge in the waiters graph
  to be traversed by deadlock detection algorithm.
*/

class MDL_wait_for_subgraph
{
public:
  virtual ~MDL_wait_for_subgraph();

  /**
    Accept a wait-for graph visitor to inspect the node
    this edge is leading to.
  */
  virtual bool accept_visitor(MDL_wait_for_graph_visitor *gvisitor) = 0;

  enum enum_deadlock_weight
  {
    DEADLOCK_WEIGHT_FTWRL1= 0,
    DEADLOCK_WEIGHT_DML= 1,
    DEADLOCK_WEIGHT_DDL= 100
  };
  /* A helper used to determine which lock request should be aborted. */
  virtual uint get_deadlock_weight() const = 0;
};


/**
  A granted metadata lock.

  @warning MDL_ticket members are private to the MDL subsystem.

  @note Multiple shared locks on a same object are represented by a
        single ticket. The same does not apply for other lock types.

  @note There are two groups of MDL_ticket members:
        - "Externally accessible". These members can be accessed from
          threads/contexts different than ticket owner in cases when
          ticket participates in some list of granted or waiting tickets
          for a lock. Therefore one should change these members before
          including then to waiting/granted lists or while holding lock
          protecting those lists.
        - "Context private". Such members are private to thread/context
          owning this ticket. I.e. they should not be accessed from other
          threads/contexts.
*/

class MDL_ticket : public MDL_wait_for_subgraph, public ilist_node<>
{
public:
  /**
    Pointers for participating in the list of lock requests for this context.
    Context private.
  */
  MDL_ticket *next_in_context;
  MDL_ticket **prev_in_context;

#ifndef DBUG_OFF
  /**
    Duration of lock represented by this ticket.
    Context public. Debug-only.
  */
public:
  enum_mdl_duration m_duration;
#endif
  ulonglong m_time;

#ifdef WITH_WSREP
  void wsrep_report(bool debug) const;
#endif /* WITH_WSREP */
  bool has_pending_conflicting_lock() const;

  MDL_context *get_ctx() const { return m_ctx; }
  bool is_upgradable_or_exclusive() const
  {
    return m_type == MDL_SHARED_UPGRADABLE ||
           m_type == MDL_SHARED_NO_WRITE ||
           m_type == MDL_SHARED_NO_READ_WRITE ||
           m_type == MDL_EXCLUSIVE;
  }
  enum_mdl_type get_type() const { return m_type; }
  const LEX_STRING *get_type_name() const;
  const LEX_STRING *get_type_name(enum_mdl_type type) const;
  MDL_lock *get_lock() const { return m_lock; }
  const MDL_key *get_key() const;
  void downgrade_lock(enum_mdl_type type);

  bool has_stronger_or_equal_type(enum_mdl_type type) const;

  bool is_incompatible_when_granted(enum_mdl_type type) const;
  bool is_incompatible_when_waiting(enum_mdl_type type) const;

  /** Implement MDL_wait_for_subgraph interface. */
  bool accept_visitor(MDL_wait_for_graph_visitor *dvisitor) override;
  uint get_deadlock_weight() const override;
  /**
    Status of lock request represented by the ticket as reflected in P_S.
  */
  enum enum_psi_status { PENDING = 0, GRANTED,
                         PRE_ACQUIRE_NOTIFY, POST_RELEASE_NOTIFY };
private:
  friend class MDL_context;
  friend class MDL_lock;

  MDL_ticket(MDL_context *ctx_arg, MDL_request *request);
  ~MDL_ticket();
private:
  /** Property of MDL_lock::Fast_road, unauthorized access is prohibited. */
  std::atomic<void*> m_fast_lane;

  /** Type of metadata lock. Externally accessible. */
  enum enum_mdl_type m_type;

  /**
    Context of the owner of the metadata lock ticket. Externally accessible.
  */
  MDL_context *m_ctx;

  /**
    Pointer to the lock object for this lock ticket. Externally accessible.
  */
  MDL_lock *m_lock;

  PSI_metadata_lock *m_psi;

private:
  MDL_ticket(const MDL_ticket &);               /* not implemented */
  MDL_ticket &operator=(const MDL_ticket &);    /* not implemented */
};


/**
  Savepoint for MDL context.

  Doesn't include metadata locks with explicit duration as
  they are not released during rollback to savepoint.
*/

class MDL_savepoint
{
public:
  MDL_savepoint() = default;;

private:
  MDL_savepoint(MDL_ticket *stmt_ticket, MDL_ticket *trans_ticket)
    : m_stmt_ticket(stmt_ticket), m_trans_ticket(trans_ticket)
  {}

  friend class MDL_context;

private:
  /**
    Pointer to last lock with statement duration which was taken
    before creation of savepoint.
  */
  MDL_ticket *m_stmt_ticket;
  /**
    Pointer to last lock with transaction duration which was taken
    before creation of savepoint.
  */
  MDL_ticket *m_trans_ticket;
};


/**
  A reliable way to wait on an MDL lock.
*/

class MDL_wait
{
public:
  MDL_wait();
  ~MDL_wait();

  enum enum_wait_status { EMPTY = 0, GRANTED, VICTIM, TIMEOUT, KILLED };

  bool set_status(enum_wait_status result_arg);
  enum_wait_status get_status();
  void reset_status();
  enum_wait_status timed_wait(MDL_context_owner *owner,
                              struct timespec *abs_timeout,
                              bool signal_timeout,
                              const PSI_stage_info *wait_state_name);
private:
  /**
    Condvar which is used for waiting until this context's pending
    request can be satisfied or this thread has to perform actions
    to resolve a potential deadlock (we subscribe to such
    notification by adding a ticket corresponding to the request
    to an appropriate queue of waiters).
  */
  mysql_mutex_t m_LOCK_wait_status;
  mysql_cond_t m_COND_wait_status;
  enum_wait_status m_wait_status;
};


typedef I_P_List<MDL_request, I_P_List_adapter<MDL_request,
                 &MDL_request::next_in_list,
                 &MDL_request::prev_in_list>,
                 I_P_List_counter>
        MDL_request_list;

/**
  Context of the owner of metadata locks. I.e. each server
  connection has such a context.
*/

class MDL_context
{
public:
  typedef I_P_List<MDL_ticket,
                   I_P_List_adapter<MDL_ticket,
                                    &MDL_ticket::next_in_context,
                                    &MDL_ticket::prev_in_context> >
          Ticket_list;

  typedef Ticket_list::Iterator Ticket_iterator;

  MDL_context();
  void destroy();

  bool try_acquire_lock(MDL_request *mdl_request);
  bool acquire_lock(MDL_request *mdl_request, double lock_wait_timeout);
  bool acquire_locks(MDL_request_list *requests, double lock_wait_timeout);
  bool upgrade_shared_lock(MDL_ticket *mdl_ticket,
                           enum_mdl_type new_type,
                           double lock_wait_timeout);

  bool clone_ticket(MDL_request *mdl_request);

  void release_all_locks_for_name(MDL_ticket *ticket);
  void release_lock(MDL_ticket *ticket);

  bool is_lock_owner(MDL_key::enum_mdl_namespace mdl_namespace,
                     const char *db, const char *name,
                     enum_mdl_type mdl_type);
  unsigned long get_lock_owner(MDL_key *mdl_key);

  bool has_lock(const MDL_savepoint &mdl_savepoint, MDL_ticket *mdl_ticket);

  inline bool has_locks() const
  {
    return !(m_tickets[MDL_STATEMENT].is_empty() &&
             m_tickets[MDL_TRANSACTION].is_empty() &&
             m_tickets[MDL_EXPLICIT].is_empty());
  }
  bool has_explicit_locks() const
  {
    return !m_tickets[MDL_EXPLICIT].is_empty();
  }
  inline bool has_transactional_locks() const
  {
    return !m_tickets[MDL_TRANSACTION].is_empty();
  }

  MDL_savepoint mdl_savepoint()
  {
    return MDL_savepoint(m_tickets[MDL_STATEMENT].front(),
                         m_tickets[MDL_TRANSACTION].front());
  }

  void set_explicit_duration_for_all_locks();
  void set_transaction_duration_for_all_locks();
  void set_lock_duration(MDL_ticket *mdl_ticket, enum_mdl_duration duration);

  void release_statement_locks();
  void release_transactional_locks(THD *thd);
  void release_explicit_locks();
  void rollback_to_savepoint(const MDL_savepoint &mdl_savepoint);

  MDL_context_owner *get_owner() { return m_owner; }

  /** @pre Only valid if we started waiting for lock. */
  inline uint get_deadlock_weight() const
  { return m_waiting_for->get_deadlock_weight() + m_deadlock_overweight; }
  void inc_deadlock_overweight() { m_deadlock_overweight++; }
  /**
    Post signal to the context (and wake it up if necessary).

    @retval FALSE - Success, signal was posted.
    @retval TRUE  - Failure, signal was not posted since context
                    already has received some signal or closed
                    signal slot.
  */
  void init(MDL_context_owner *arg) { m_owner= arg; reset(); }
  void reset() { m_deadlock_overweight= 0; }

  void set_needs_thr_lock_abort(bool needs_thr_lock_abort)
  {
    /*
      @note In theory, this member should be modified under protection
            of some lock since it can be accessed from different threads.
            In practice, this is not necessary as code which reads this
            value and so might miss the fact that value was changed will
            always re-try reading it after small timeout and therefore
            will see the new value eventually.
    */
    m_needs_thr_lock_abort= needs_thr_lock_abort;
  }
  bool get_needs_thr_lock_abort() const
  {
    return m_needs_thr_lock_abort;
  }
public:
  /**
    If our request for a lock is scheduled, or aborted by the deadlock
    detector, the result is recorded in this class.
  */
  MDL_wait m_wait;
private:
  /**
    Lists of all MDL tickets acquired by this connection.

    Lists of MDL tickets:
    ---------------------
    The entire set of locks acquired by a connection can be separated
    in three subsets according to their duration: locks released at
    the end of statement, at the end of transaction and locks are
    released explicitly.

    Statement and transactional locks are locks with automatic scope.
    They are accumulated in the course of a transaction, and released
    either at the end of uppermost statement (for statement locks) or
    on COMMIT, ROLLBACK or ROLLBACK TO SAVEPOINT (for transactional
    locks). They must not be (and never are) released manually,
    i.e. with release_lock() call.

    Tickets with explicit duration are taken for locks that span
    multiple transactions or savepoints.
    These are: HANDLER SQL locks (HANDLER SQL is
    transaction-agnostic), LOCK TABLES locks (you can COMMIT/etc
    under LOCK TABLES, and the locked tables stay locked), user level
    locks (GET_LOCK()/RELEASE_LOCK() functions) and
    locks implementing "global read lock".

    Statement/transactional locks are always prepended to the
    beginning of the appropriate list. In other words, they are
    stored in reverse temporal order. Thus, when we rollback to
    a savepoint, we start popping and releasing tickets from the
    front until we reach the last ticket acquired after the savepoint.

    Locks with explicit duration are not stored in any
    particular order, and among each other can be split into
    four sets:

    [LOCK TABLES locks] [USER locks] [HANDLER locks] [GLOBAL READ LOCK locks]

    The following is known about these sets:

    * GLOBAL READ LOCK locks are always stored last.
      This is because one can't say SET GLOBAL read_only=1 or
      FLUSH TABLES WITH READ LOCK if one has locked tables. One can,
      however, LOCK TABLES after having entered the read only mode.
      Note, that subsequent LOCK TABLES statement will unlock the previous
      set of tables, but not the GRL!
      There are no HANDLER locks after GRL locks because
      SET GLOBAL read_only performs a FLUSH TABLES WITH
      READ LOCK internally, and FLUSH TABLES, in turn, implicitly
      closes all open HANDLERs.
      However, one can open a few HANDLERs after entering the
      read only mode.
    * LOCK TABLES locks include intention exclusive locks on
      involved schemas and global intention exclusive lock.
  */
  Ticket_list m_tickets[MDL_DURATION_END];
  MDL_context_owner *m_owner;
  /**
    TRUE -  if for this context we will break protocol and try to
            acquire table-level locks while having only S lock on
            some table.
            To avoid deadlocks which might occur during concurrent
            upgrade of SNRW lock on such object to X lock we have to
            abort waits for table-level locks for such connections.
    FALSE - Otherwise.
  */
  bool m_needs_thr_lock_abort;

  /**
    Read-write lock protecting m_waiting_for member.

    @note The fact that this read-write lock prefers readers is
          important as deadlock detector won't work correctly
          otherwise. @sa Comment for MDL_lock::m_rwlock.
  */
  mysql_prlock_t m_LOCK_waiting_for;
  /**
    Tell the deadlock detector what metadata lock or table
    definition cache entry this session is waiting for.
    In principle, this is redundant, as information can be found
    by inspecting waiting queues, but we'd very much like it to be
    readily available to the wait-for graph iterator.
   */
  MDL_wait_for_subgraph *m_waiting_for;
  LF_PINS *m_pins;
  uint m_deadlock_overweight;
private:
  MDL_ticket *find_ticket(MDL_request *mdl_req,
                          enum_mdl_duration *duration);
  void release_locks_stored_before(enum_mdl_duration duration, MDL_ticket *sentinel);
  void release_lock(enum_mdl_duration duration, MDL_ticket *ticket);
  bool try_acquire_lock_impl(MDL_request *mdl_request,
                             MDL_ticket **out_ticket);
  bool fix_pins();

public:
  THD *get_thd() const { return m_owner->get_thd(); }
  bool has_explicit_locks();
  void find_deadlock();

  ulong get_thread_id() const { return thd_get_thread_id(get_thd()); }

  bool visit_subgraph(MDL_wait_for_graph_visitor *dvisitor);

  /** Inform the deadlock detector there is an edge in the wait-for graph. */
  void will_wait_for(MDL_wait_for_subgraph *waiting_for_arg)
  {
    mysql_prlock_wrlock(&m_LOCK_waiting_for);
    m_waiting_for=  waiting_for_arg;
    mysql_prlock_unlock(&m_LOCK_waiting_for);
  }

  /** Remove the wait-for edge from the graph after we're done waiting. */
  void done_waiting_for()
  {
    mysql_prlock_wrlock(&m_LOCK_waiting_for);
    m_waiting_for= NULL;
    mysql_prlock_unlock(&m_LOCK_waiting_for);
  }
  void lock_deadlock_victim()
  {
    mysql_prlock_rdlock(&m_LOCK_waiting_for);
  }
  void unlock_deadlock_victim()
  {
    mysql_prlock_unlock(&m_LOCK_waiting_for);
  }
private:
  MDL_context(const MDL_context &rhs);          /* not implemented */
  MDL_context &operator=(MDL_context &rhs);     /* not implemented */

  /* metadata_lock_info plugin */
  friend int i_s_metadata_lock_info_fill_row(MDL_ticket*, void*);
#ifndef DBUG_OFF
public:
  /**
    This is for the case when the thread opening the table does not acquire
    the lock itself, but utilizes a lock guarantee from another MDL context.

    For example, in InnoDB, MDL is acquired by the purge_coordinator_task,
    but the table may be opened and used in a purge_worker_task.
    The coordinator thread holds the lock for the duration of worker's purge
    job, or longer, possibly reusing shared MDL for different workers and jobs.
  */
  MDL_context *lock_warrant= NULL;

  inline bool is_lock_warrantee(MDL_key::enum_mdl_namespace ns,
                                const char *db, const char *name,
                                enum_mdl_type mdl_type) const
  {
    return lock_warrant && lock_warrant->is_lock_owner(ns, db, name, mdl_type);
  }
#endif
};


void mdl_init();
void mdl_destroy();

extern "C" unsigned long thd_get_thread_id(const MYSQL_THD thd);

/**
  Check if a connection in question is no longer connected.

  @details
  Replication apply thread is always connected. Otherwise,
  does a poll on the associated socket to check if the client
  is gone.
*/
extern "C" int thd_is_connected(MYSQL_THD thd);


/*
  Metadata locking subsystem tries not to grant more than
  max_write_lock_count high-prio, strong locks successively,
  to avoid starving out weak, low-prio locks.
*/
extern "C" ulong max_write_lock_count;
extern uint mdl_instances;

typedef int (*mdl_iterator_callback)(MDL_ticket *ticket, void *arg,
                                     bool granted);
extern MYSQL_PLUGIN_IMPORT
int mdl_iterate(mdl_iterator_callback callback, void *arg);
#endif /* MDL_H */
