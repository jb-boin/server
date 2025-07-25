/* Copyright (C) 2006 MySQL AB & MySQL Finland AB & TCX DataKonsult AB
   Copyright (c) 2009, 2022, MariaDB Corporation Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/* open an Aria table */

#include "ma_fulltext.h"
#include "ma_sp_defs.h"
#include "ma_rt_index.h"
#include "ma_blockrec.h"
#include "trnman.h"
#include "ma_trnman.h"
#include <m_ctype.h>
#include "ma_crypt.h"
#include "s3_func.h"

#ifdef _WIN32
#include <fcntl.h>
#endif

static void setup_key_functions(MARIA_KEYDEF *keyinfo);
static my_bool maria_scan_init_dummy(MARIA_HA *info);
static void maria_scan_end_dummy(MARIA_HA *info);
static my_bool maria_once_init_dummy(MARIA_SHARE *, File);
static my_bool maria_once_end_dummy(MARIA_SHARE *);
static uchar *_ma_state_info_read(uchar *, MARIA_STATE_INFO *, myf);

#define get_next_element(to,pos,size) { memcpy((char*) to,pos,(size_t) size); \
					pos+=size;}


#define disk_pos_assert(share, pos, end_pos)     \
if (pos > end_pos)             \
{                              \
  _ma_set_fatal_error_with_share(share, HA_ERR_CRASHED);    \
  goto err;                    \
}


/******************************************************************************
** Return the shared struct if the table is already open.
** In MySQL the server will handle version issues.
******************************************************************************/

MARIA_HA *_ma_test_if_reopen(const char *filename)
{
  LIST *pos;

  for (pos=maria_open_list ; pos ; pos=pos->next)
  {
    MARIA_HA *info=(MARIA_HA*) pos->data;
    MARIA_SHARE *share= info->s;
    if (!strcmp(share->unique_file_name.str,filename) && share->last_version)
      return info;
  }
  return 0;
}


/*
  Open a new instance of an already opened Maria table

  SYNOPSIS
    maria_clone_internal()
    share	Share of already open table
    mode	Mode of table (O_RDONLY | O_RDWR)
    data_file   Filedescriptor of data file to use < 0 if one should open
	        open it.
    internal_table <> 0 if this is an internal temporary table

 RETURN
    #   Maria handler
    0   Error
*/

static MARIA_HA *maria_clone_internal(MARIA_SHARE *share,
                                      int mode, File data_file,
                                      uint internal_table,
                                      struct ms3_st *s3)
{
  int save_errno;
  uint errpos;
  MARIA_HA info,*m_info;
  my_bitmap_map *changed_fields_bitmap;
  myf flag= MY_WME | share->malloc_flag;
  DBUG_ENTER("maria_clone_internal");

  errpos= 0;
  bzero((uchar*) &info,sizeof(info));

  if (mode == O_RDWR && share->mode == O_RDONLY)
  {
    my_errno=EACCES;				/* Can't open in write mode */
    goto err;
  }
  if (data_file >= 0)
  {
    info.dfile.file= data_file;
    info.dfile.pagecache= share->pagecache;
  }
  else if (_ma_open_datafile(&info, share))
    goto err;
  errpos= 5;

  /* alloc and set up private structure parts */
  if (!my_multi_malloc(PSI_INSTRUMENT_ME, flag,
		       &m_info,sizeof(MARIA_HA),
		       &info.blobs,sizeof(MARIA_BLOB)*share->base.blobs,
		       &info.buff,(share->base.max_key_block_length*2+
				   share->base.max_key_length),
		       &info.lastkey_buff,share->base.max_key_length*3,
		       &info.first_mbr_key, share->base.max_key_length,
		       &info.maria_rtree_recursion_state,
                       share->have_rtree ? 1024 : 0,
                       &changed_fields_bitmap,
                       bitmap_buffer_size(share->base.fields),
		       NullS))
    goto err;
  errpos= 6;

  info.s3= s3;
  memcpy(info.blobs,share->blobs,sizeof(MARIA_BLOB)*share->base.blobs);
  info.lastkey_buff2= info.lastkey_buff + share->base.max_key_length;
  info.last_key.data= info.lastkey_buff;

  info.s=share;
  info.cur_row.lastpos= HA_OFFSET_ERROR;
  /* Impossible first index to force initialization in _ma_check_index() */
  info.lastinx= ~0;
  info.update= (short) (HA_STATE_NEXT_FOUND+HA_STATE_PREV_FOUND);
  info.opt_flag=READ_CHECK_USED;
  info.this_unique= (ulong) info.dfile.file; /* Uniq number in process */
#ifdef MARIA_EXTERNAL_LOCKING
  if (share->data_file_type == COMPRESSED_RECORD)
    info.this_unique= share->state.unique;
  info.this_loop=0;				/* Update counter */
  info.last_unique= share->state.unique;
  info.last_loop=   share->state.update_count;
#endif
  info.errkey= -1;
  info.page_changed= 1;
  info.autocommit= 1;
  info.keyread_buff= info.buff + share->base.max_key_block_length;

  info.lock_type= F_UNLCK;
  if (share->options & HA_OPTION_TMP_TABLE)
    info.lock_type= F_WRLCK;

  _ma_set_data_pagecache_callbacks(&info.dfile, share);
  my_bitmap_init(&info.changed_fields, changed_fields_bitmap, share->base.fields);
  if ((*share->init)(&info))
    goto err;

  /* The following should be big enough for all pinning purposes */
  if (my_init_dynamic_array(PSI_INSTRUMENT_ME, &info.pinned_pages,
                            sizeof(MARIA_PINNED_PAGE),
                            MY_MAX(share->base.blobs*2 + 4,
                            MARIA_MAX_TREE_LEVELS*3), 16, flag))
    goto err;


  mysql_mutex_lock(&share->intern_lock);
  info.read_record= share->read_record;
  share->reopen++;
  if (share->options & HA_OPTION_READ_ONLY_DATA)
  {
    info.lock_type=F_RDLCK;
    share->r_locks++;
    share->tot_locks++;
  }
  if ((share->options & HA_OPTION_DELAY_KEY_WRITE) &&
      maria_delay_key_write)
    share->delay_key_write=1;

  if (!share->now_transactional)       /* If not transactional table */
  {
    /* Pagecache requires access to info->trn->rec_lsn */
    _ma_set_tmp_trn_for_table(&info, &dummy_transaction_object);
    info.state= &share->state.state;	/* Change global values by default */
  }
  else
  {
    info.state=  &share->state.common;
    *info.state= share->state.state;            /* Initial values */
  }
  info.state_start= info.state;                 /* Initial values */

  mysql_mutex_unlock(&share->intern_lock);

  /* Allocate buffer for one record */
  /* prerequisites: info->rec_buffer == 0 && info->rec_buff_size == 0 */
  if (_ma_alloc_buffer(&info.rec_buff, &info.rec_buff_size,
                       share->base.default_rec_buff_size, flag))
    goto err;

  bzero(info.rec_buff, share->base.default_rec_buff_size);

  *m_info=info;
  thr_lock_data_init(&share->lock,&m_info->lock,(void*) m_info);

  if (share->options & HA_OPTION_TMP_TABLE)
    m_info->lock.type= TL_WRITE;

  if (!internal_table)
  {
    m_info->open_list.data= m_info->share_list.data= (void*) m_info;
    maria_open_list=  list_add(maria_open_list,  &m_info->open_list);
    share->open_list= list_add(share->open_list, &m_info->share_list);
  }
  else
  {
    /* We don't need to mark internal temporary tables as changed on disk */
    share->internal_table= 1;
    share->global_changed= 1;
  }
  DBUG_RETURN(m_info);

err:
  DBUG_PRINT("error", ("error: %d", my_errno));
  save_errno=my_errno ? my_errno : HA_ERR_END_OF_FILE;
  if ((save_errno == HA_ERR_CRASHED) ||
      (save_errno == HA_ERR_CRASHED_ON_USAGE) ||
      (save_errno == HA_ERR_CRASHED_ON_REPAIR))
    _ma_report_error(save_errno, &share->open_file_name,
                     MYF(ME_ERROR_LOG));
  switch (errpos) {
  case 6:
    (*share->end)(&info);
    delete_dynamic(&info.pinned_pages);
    my_free(m_info->s3);
    my_free(m_info);
    /* fall through */
  case 5:
    if (data_file < 0)
      mysql_file_close(info.dfile.file, MYF(0));
    break;
  }
  my_errno=save_errno;
  DBUG_RETURN (NULL);
} /* maria_clone_internal */


/******************************************************************************
  open a MARIA table

  See my_base.h for the handle_locking argument
  if handle_locking and HA_OPEN_ABORT_IF_CRASHED then abort if the table
  is marked crashed or if we are not using locking and the table doesn't
  have an open count of 0.
******************************************************************************/

MARIA_HA *maria_open(const char *name, int mode, uint open_flags,
                     S3_INFO *s3)
{
  int open_mode= 0,save_errno;
  uint i,j,len,errpos,head_length,base_pos,keys, realpath_err,
    key_parts,base_key_parts,unique_key_parts,fulltext_keys,uniques;
  uint internal_table= MY_TEST(open_flags & HA_OPEN_INTERNAL_TABLE);
  myf common_flag= (((open_flags & HA_OPEN_TMP_TABLE) &&
                     !(open_flags & HA_OPEN_GLOBAL_TMP_TABLE)) ?
                    MY_THREAD_SPECIFIC : 0);
  uint file_version;
  size_t info_length;
  char name_buff[FN_REFLEN], org_name[FN_REFLEN], index_name[FN_REFLEN],
       data_name[FN_REFLEN];
  uchar *UNINIT_VAR(disk_cache), *disk_pos, *end_pos;
  MARIA_HA info, *UNINIT_VAR(m_info), *old_info= NULL;
  MARIA_SHARE share_buff,*share;
  double *rec_per_key_part;
  ulong  *nulls_per_key_part;
  my_off_t key_root[HA_MAX_POSSIBLE_KEY];
  ulonglong max_key_file_length, max_data_file_length;
  my_bool versioning= 1, born_transactional;
  File data_file= -1, kfile= -1;
  struct ms3_st *s3_client= 0;
  S3_INFO *share_s3= 0;
  S3_BLOCK index_header;
  DBUG_ENTER("maria_open");

  errpos= 0;
  head_length=sizeof(share_buff.state.header);
  bzero((uchar*) &info,sizeof(info));
  bzero((uchar*) &index_header, sizeof(index_header));

#ifndef WITH_S3_STORAGE_ENGINE
  DBUG_ASSERT(!s3);
#else
  if (!s3)
#endif /* WITH_S3_STORAGE_ENGINE */
  {
    realpath_err= my_realpath(name_buff, fn_format(org_name, name, "",
                                                   MARIA_NAME_IEXT,
                                                   MY_UNPACK_FILENAME),MYF(0));
    if (realpath_err > 0) /* File not found, no point in looking further. */
    {
      DBUG_RETURN(NULL);
    }

    if (my_is_symlink(org_name) &&
        (realpath_err || mysys_test_invalid_symlink(name_buff)))
    {
      my_errno= HA_WRONG_CREATE_OPTION;
      DBUG_RETURN(0);
    }
  }
#ifdef WITH_S3_STORAGE_ENGINE
  else
  {
    strmake(name_buff, name, sizeof(name_buff)-1); /* test_if_reopen() */
    if (!(s3_client= s3f.open_connection(s3)))
    {
      internal_table= 1;                        /* Avoid unlock on error */
      goto err;
    }
  }
#endif /* WITH_S3_STORAGE_ENGINE */

  if (!internal_table)
    mysql_mutex_lock(&THR_LOCK_maria);
  if ((open_flags & HA_OPEN_COPY) ||
      (internal_table || !(old_info=_ma_test_if_reopen(name_buff))))
  {
    share= &share_buff;
    bzero((uchar*) &share_buff,sizeof(share_buff));
    share_buff.state.key_root=key_root;

    if (!s3)
    {
      DBUG_EXECUTE_IF("maria_pretend_crashed_table_on_open",
                      if (strstr(name, "/t1"))
                      {
                        my_errno= HA_ERR_CRASHED;
                        goto err;
                      });
      DEBUG_SYNC_C("mi_open_kfile");
      if ((kfile=mysql_file_open(key_file_kfile, name_buff,
                                 (open_mode=O_RDWR) | O_SHARE | O_NOFOLLOW | O_CLOEXEC,
                                 MYF(common_flag | MY_NOSYMLINKS))) < 0)
      {
        if ((errno != EROFS && errno != EACCES) ||
            mode != O_RDONLY ||
            (kfile=mysql_file_open(key_file_kfile, name_buff,
                                   (open_mode=O_RDONLY) | O_SHARE | O_NOFOLLOW | O_CLOEXEC,
                                   MYF(common_flag | MY_NOSYMLINKS))) < 0)
          goto err;
      }
      errpos= 1;
      if (mysql_file_pread(kfile,share->state.header.file_version, head_length,
                           0, MYF(MY_NABP)))
      {
        my_errno= HA_ERR_NOT_A_TABLE;
        goto err;
      }
    }
#ifdef WITH_S3_STORAGE_ENGINE
    else
    {
      open_mode= mode;
      errpos= 1;
      if (s3f.set_database_and_table_from_path(s3, name_buff))
      {
        my_printf_error(HA_ERR_NO_SUCH_TABLE,
                        "Can't find database and path from %s",  MYF(0),
                        name_buff);
        my_errno= HA_ERR_NO_SUCH_TABLE;
        goto err;
      }
      if (!(share_s3= share->s3_path= s3f.info_copy(s3)))
        goto err;                             /* EiOM */

      /* Check if table has changed in S3 */
      if (s3f.check_frm_version(s3_client, share_s3) == 1)
      {
        my_errno= HA_ERR_TABLE_DEF_CHANGED;
        goto err;
      }

      if (s3f.read_index_header(s3_client, share_s3, &index_header))
        goto err;
      if (index_header.length < head_length)
      {
        my_errno=HA_ERR_NOT_A_TABLE;
        goto err;
      }
      memcpy(share->state.header.file_version, index_header.str,
             head_length);
      kfile= s3f.unique_file_number();
    }
#endif /* WITH_S3_STORAGE_ENGINE */

    share->mode=open_mode;
    if (memcmp(share->state.header.file_version, maria_file_magic, 4))
    {
      DBUG_PRINT("error",("Wrong header in %s",name_buff));
      DBUG_DUMP("error_dump", share->state.header.file_version,
		head_length);
      my_errno=HA_ERR_NOT_A_TABLE;
      goto err;
    }
    share->options= mi_uint2korr(share->state.header.options);
    if (share->options &
	~(HA_OPTION_PACK_RECORD | HA_OPTION_PACK_KEYS |
	  HA_OPTION_COMPRESS_RECORD | HA_OPTION_READ_ONLY_DATA |
	  HA_OPTION_TEMP_COMPRESS_RECORD | HA_OPTION_CHECKSUM |
          HA_OPTION_TMP_TABLE | HA_OPTION_DELAY_KEY_WRITE |
          HA_OPTION_RELIES_ON_SQL_LAYER | HA_OPTION_NULL_FIELDS |
          HA_OPTION_PAGE_CHECKSUM))
    {
      DBUG_PRINT("error",("wrong options: 0x%lx", share->options));
      my_errno=HA_ERR_NEW_FILE;
      goto err;
    }
    if ((share->options & HA_OPTION_RELIES_ON_SQL_LAYER) &&
        ! (open_flags & HA_OPEN_FROM_SQL_LAYER))
    {
      DBUG_PRINT("error", ("table cannot be opened from non-sql layer"));
      my_errno= HA_ERR_UNSUPPORTED;
      goto err;
    }
    if (!s3)
    {
      /* Don't call realpath() if the name can't be a link */
      if (!strcmp(name_buff, org_name) ||
          my_readlink(index_name, org_name, MYF(0)) == -1)
        (void) strmov(index_name, org_name);
      *strrchr(org_name, FN_EXTCHAR)= '\0';
      (void) fn_format(data_name,org_name,"",MARIA_NAME_DEXT,
                       MY_APPEND_EXT|MY_UNPACK_FILENAME);
      if (my_is_symlink(data_name))
      {
        if (my_realpath(data_name, data_name, MYF(0)))
          goto err;
        if (mysys_test_invalid_symlink(data_name))
        {
          my_errno= HA_WRONG_CREATE_OPTION;
          goto err;
        }
        share->mode|= O_NOFOLLOW; /* all symlinks are resolved by realpath() */
      }
    }
    else
    {
      /* Don't show DIRECTORY in show create table */
      index_name[0]= data_name[0]= 0;
    }

    info_length=mi_uint2korr(share->state.header.header_length);
    base_pos= mi_uint2korr(share->state.header.base_pos);

    /*
      Allocate space for header information and for data that is too
      big to keep on stack
    */
    if (!(disk_cache= my_malloc(PSI_INSTRUMENT_ME, info_length+128,
                                MYF(MY_WME | common_flag))))
    {
      my_errno=ENOMEM;
      goto err;
    }

    end_pos=disk_cache+info_length;
    errpos= 3;
    if (!s3)
    {
      if (mysql_file_pread(kfile, disk_cache, info_length, 0L, MYF(MY_NABP)))
      {
        _ma_set_fatal_error_with_share(share, HA_ERR_CRASHED);
        goto err;
      }
    }
#ifdef WITH_S3_STORAGE_ENGINE
    else
    {
      if (index_header.length < info_length)
      {
        my_errno=HA_ERR_NOT_A_TABLE;
        goto err;
      }
      memcpy(disk_cache, index_header.str, info_length);
    }
#endif /* WITH_S3_STORAGE_ENGINE */

    len=mi_uint2korr(share->state.header.state_info_length);
    keys=    (uint) share->state.header.keys;
    uniques= (uint) share->state.header.uniques;
    fulltext_keys= (uint) share->state.header.fulltext_keys;
    base_key_parts= key_parts= mi_uint2korr(share->state.header.key_parts);
    unique_key_parts= mi_uint2korr(share->state.header.unique_key_parts);
    if (len != MARIA_STATE_INFO_SIZE)
    {
      DBUG_PRINT("warning",
		 ("saved_state_info_length: %d  state_info_length: %d",
		  len,MARIA_STATE_INFO_SIZE));
    }
    share->state_diff_length=len-MARIA_STATE_INFO_SIZE;

    if (!_ma_state_info_read(disk_cache, &share->state, common_flag))
      goto err;
    len= mi_uint2korr(share->state.header.base_info_length);
    if (len != MARIA_BASE_INFO_SIZE)
    {
      DBUG_PRINT("warning",("saved_base_info_length: %d  base_info_length: %d",
			    len,MARIA_BASE_INFO_SIZE));
    }
    disk_pos= _ma_base_info_read(disk_cache + base_pos, &share->base);
    /*
      Check if old version of Aria file. Version 0 has language
      stored in header.not_used
    */
    file_version= (share->state.header.not_used == 0);
    if (file_version == 0)
      share->base.language= share->state.header.not_used;
    born_transactional= share->base.born_transactional;

    share->state.state_length=base_pos;
    /* For newly opened tables we reset the error-has-been-printed flag */
    share->state.changed&= ~STATE_CRASHED_PRINTED;
    share->state.org_changed= share->state.changed;

    if (!(open_flags & HA_OPEN_FOR_REPAIR) &&
	((share->state.changed & STATE_CRASHED_FLAGS) ||
	 ((open_flags & HA_OPEN_ABORT_IF_CRASHED) &&
	  (my_disable_locking && share->state.open_count))))
    {
      DBUG_PRINT("error",("Table is marked as crashed. open_flags: %u  "
                          "changed: %u  open_count: %u  !locking: %d",
                          open_flags, share->state.changed,
                          share->state.open_count, my_disable_locking));
      my_errno=((share->state.changed & STATE_CRASHED_ON_REPAIR) ?
		HA_ERR_CRASHED_ON_REPAIR : HA_ERR_CRASHED_ON_USAGE);
      goto err;
    }
    if (share->state.open_count)
      share->open_count_not_zero_on_open= 1;

    /*
      A transactional table is not usable on this system if:
      - share->state.create_trid > trnman_get_max_trid()
        - Critical as trid is stored relative to create_trid.
      - uuid is different

        STATE_NOT_MOVABLE is reset when a table is zerofilled
        (has no LSN's and no trids)

      We can ignore testing uuid if STATE_NOT_MOVABLE is not set, as in this
      case the uuid will be set in _ma_mark_file_changed().
    */
    if (born_transactional &&
        ((share->state.create_trid > max_trid_in_system() &&
         !maria_in_recovery) ||
         ((share->state.changed & STATE_NOT_MOVABLE) &&
          ((!(open_flags & HA_OPEN_IGNORE_MOVED_STATE) &&
            memcmp(share->base.uuid, maria_uuid, MY_UUID_SIZE)))) ||
         ((share->state.changed & (STATE_MOVED | STATE_NOT_ZEROFILLED)) ==
          (STATE_MOVED | STATE_NOT_ZEROFILLED))))
    {
      DBUG_PRINT("warning", ("table is moved from another system.  uuid_diff: %d  create_trid: %lu  max_trid: %lu  moved: %d",
                             memcmp(share->base.uuid, maria_uuid,
                                    MY_UUID_SIZE) != 0,
                             (ulong) share->state.create_trid,
                             (ulong) trnman_get_max_trid(),
                             MY_TEST((share->state.changed & STATE_MOVED))));
      if (open_flags & HA_OPEN_FOR_REPAIR)
        share->state.changed|= STATE_MOVED;
      else
      {
        my_errno= HA_ERR_OLD_FILE;
        goto err;
      }
    }

    /* sanity check */
    if (share->base.keystart > 65535 || share->base.rec_reflength > 8)
    {
      _ma_set_fatal_error_with_share(share, HA_ERR_CRASHED);
      goto err;
    }

    key_parts+=fulltext_keys*FT_SEGS;
    if (share->base.max_key_length > _ma_max_key_length() ||
        keys > MARIA_MAX_KEY || key_parts > MARIA_MAX_KEY * HA_MAX_KEY_SEG)
    {
      DBUG_PRINT("error",("Wrong key info:  Max_key_length: %d  keys: %d  key_parts: %d", share->base.max_key_length, keys, key_parts));
      my_errno=HA_ERR_UNSUPPORTED;
      goto err;
    }

    /* Ensure we have space in the key buffer for transaction id's */
    if (born_transactional)
      share->base.max_key_length= ALIGN_SIZE(share->base.max_key_length +
                                             MARIA_MAX_PACK_TRANSID_SIZE);

    /*
      If page cache is not initialized, then assume we will create the
      page_cache after the table is opened!
      This is only used by maria_check to allow it to check/repair tables
      with different block sizes.
    */
    if (share->base.block_size != maria_block_size &&
        maria_pagecaches.initialized)
    {
      DBUG_PRINT("error", ("Wrong block size %u; Expected %u",
                           (uint) share->base.block_size,
                           (uint) maria_block_size));
      my_errno=HA_ERR_UNSUPPORTED;
      my_printf_error(my_errno, "Wrong block size %u; Expected %u",
                      MYF(0),
                      (uint) share->base.block_size,
                      (uint) maria_block_size);
      goto err;
    }

    /* Correct max_file_length based on length of sizeof(off_t) */
    max_data_file_length=
      (share->options & (HA_OPTION_PACK_RECORD | HA_OPTION_COMPRESS_RECORD)) ?
      (((ulonglong) 1 << (share->base.rec_reflength*8))-1) :
      (_ma_safe_mul(share->base.pack_reclength,
		   (ulonglong) 1 << (share->base.rec_reflength*8))-1);

    max_key_file_length=
      _ma_safe_mul(share->base.block_size,
		  ((ulonglong) 1 << (share->base.key_reflength*8))-1);
#if SIZEOF_OFF_T == 4
    set_if_smaller(max_data_file_length, INT_MAX32);
    set_if_smaller(max_key_file_length, INT_MAX32);
#endif
    /* For internal temporary tables, max_data_file_length is already set */
    if (!internal_table || !share->base.max_data_file_length)
      share->base.max_data_file_length=(my_off_t) max_data_file_length;
    DBUG_ASSERT(share->base.max_data_file_length);
    share->base.max_key_file_length=(my_off_t) max_key_file_length;

    if (share->options & HA_OPTION_COMPRESS_RECORD)
      share->base.max_key_length+=2;	/* For safety */
    /* Add space for node pointer */
    share->base.max_key_length+= share->base.key_reflength;

    share->unique_file_name.length= strlen(name_buff);
    share->index_file_name.length=  strlen(index_name);
    share->data_file_name.length=   strlen(data_name);
    share->open_file_name.length=   strlen(name);
    if (!my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME | common_flag),
			 &share,sizeof(*share),
			 &rec_per_key_part, sizeof(double) * key_parts,
                         &nulls_per_key_part, sizeof(long)* key_parts,
			 &share->keyinfo,keys*sizeof(MARIA_KEYDEF),
			 &share->uniqueinfo,uniques*sizeof(MARIA_UNIQUEDEF),
			 &share->keyparts,
			 (key_parts+unique_key_parts+keys+uniques) *
			 sizeof(HA_KEYSEG),
			 &share->columndef,
			 (share->base.fields+1)*sizeof(MARIA_COLUMNDEF),
                         &share->column_nr, share->base.fields*sizeof(uint16),
			 &share->blobs,sizeof(MARIA_BLOB)*share->base.blobs,
			 &share->unique_file_name.str,
			 share->unique_file_name.length+1,
			 &share->index_file_name.str,
                         share->index_file_name.length+1,
			 &share->data_file_name.str,
                         share->data_file_name.length+1,
                         &share->open_file_name.str,
                         share->open_file_name.length+1,
			 &share->state.key_root,keys*sizeof(my_off_t),
			 &share->mmap_lock,sizeof(mysql_rwlock_t),
			 NullS))
      goto err;
    errpos= 4;

    *share= share_buff;
    share->state.rec_per_key_part=   rec_per_key_part;
    share->state.nulls_per_key_part= nulls_per_key_part;

    memcpy((char*) rec_per_key_part,
	   (char*) share_buff.state.rec_per_key_part,
           sizeof(double)*base_key_parts);
    memcpy((char*) nulls_per_key_part,
	   (char*) share_buff.state.nulls_per_key_part,
           sizeof(long)*base_key_parts);
    memcpy((char*) share->state.key_root,
	   (char*) key_root, sizeof(my_off_t)*keys);
    strmov(share->unique_file_name.str, name_buff);
    strmov(share->index_file_name.str, index_name);
    strmov(share->data_file_name.str,  data_name);
    strmov(share->open_file_name.str,  name);

    share->block_size= share->base.block_size;   /* Convenience */
    share->max_index_block_size= share->block_size - KEYPAGE_CHECKSUM_SIZE;
    share->keypage_header= ((born_transactional ?
                             LSN_STORE_SIZE + TRANSID_SIZE :
                             0) + KEYPAGE_KEYID_SIZE + KEYPAGE_FLAG_SIZE +
                            KEYPAGE_USED_SIZE);

    if (MY_TEST(share->base.extra_options & MA_EXTRA_OPTIONS_ENCRYPTED))
    {
      share->keypage_header+= ma_crypt_get_index_page_header_space(share);
    }

    {
      HA_KEYSEG *pos=share->keyparts;
      uint32 ftkey_nr= 1;
      for (i=0 ; i < keys ; i++)
      {
        MARIA_KEYDEF *keyinfo= &share->keyinfo[i];
        keyinfo->share= share;
	disk_pos=_ma_keydef_read(disk_pos, keyinfo);
        keyinfo->key_nr= i;

        /* Calculate length to store a key + nod flag and transaction info */
        keyinfo->max_store_length= (keyinfo->maxlength +
                                    share->base.key_reflength);
        if (born_transactional)
          keyinfo->max_store_length+= MARIA_INDEX_OVERHEAD_SIZE;

        /* See ma_delete.cc::underflow() */
        if (!(keyinfo->flag & (HA_BINARY_PACK_KEY | HA_PACK_KEY)))
          keyinfo->underflow_block_length= keyinfo->block_length/3;
        else
        {
          /* Packed key, ensure we don't get overflow in underflow() */
          keyinfo->underflow_block_length=
            MY_MAX((int) (share->max_index_block_size - keyinfo->maxlength * 3),
                (int) (share->keypage_header + share->base.key_reflength));
          set_if_smaller(keyinfo->underflow_block_length,
                         keyinfo->block_length/3);
        }

        disk_pos_assert(share,
                        disk_pos + keyinfo->keysegs * HA_KEYSEG_SIZE,
 			end_pos);
        if (keyinfo->key_alg == HA_KEY_ALG_RTREE)
          share->have_rtree= 1;
	keyinfo->seg=pos;
	for (j=0 ; j < keyinfo->keysegs; j++,pos++)
	{
	  disk_pos=_ma_keyseg_read(disk_pos, pos);
	  if (pos->type == HA_KEYTYPE_TEXT ||
              pos->type == HA_KEYTYPE_VARTEXT1 ||
              pos->type == HA_KEYTYPE_VARTEXT2)
	  {
	    if (!pos->language)
	      pos->charset=default_charset_info;
	    else if (!(pos->charset= get_charset(pos->language, MYF(MY_WME))))
	    {
	      my_errno=HA_ERR_UNKNOWN_CHARSET;
	      goto err;
	    }
	  }
	  else if (pos->type == HA_KEYTYPE_BINARY)
	    pos->charset= &my_charset_bin;
	}
	if (keyinfo->key_alg == HA_KEY_ALG_RTREE)
	{
	  uint sp_segs=SPDIMS*2;
	  keyinfo->seg=pos-sp_segs;
	  keyinfo->keysegs--;
          versioning= 0;
	}
        else if (keyinfo->key_alg == HA_KEY_ALG_FULLTEXT)
	{
          versioning= 0;
          DBUG_ASSERT(fulltext_keys);
          {
            uint k;
            keyinfo->seg=pos;
            for (k=0; k < FT_SEGS; k++)
            {
              *pos= ft_keysegs[k];
              pos[0].language= pos[-1].language;
              if (!(pos[0].charset= pos[-1].charset))
              {
                _ma_set_fatal_error_with_share(share, HA_ERR_CRASHED);
                goto err;
              }
              pos++;
            }
          }
          if (!share->ft2_keyinfo.seg)
          {
            memcpy(&share->ft2_keyinfo, keyinfo, sizeof(MARIA_KEYDEF));
            share->ft2_keyinfo.keysegs=1;
            share->ft2_keyinfo.flag=0;
            share->ft2_keyinfo.key_alg=HA_KEY_ALG_BTREE;
            share->ft2_keyinfo.keylength=
            share->ft2_keyinfo.minlength=
            share->ft2_keyinfo.maxlength=HA_FT_WLEN+share->base.rec_reflength;
            share->ft2_keyinfo.seg=pos-1;
            share->ft2_keyinfo.end=pos;
            setup_key_functions(& share->ft2_keyinfo);
          }
          keyinfo->ftkey_nr= ftkey_nr++;
	}
        setup_key_functions(keyinfo);
	keyinfo->end=pos;
	pos->type=HA_KEYTYPE_END;			/* End */
	pos->length=share->base.rec_reflength;
	pos->null_bit=0;
	pos->flag=0;					/* For purify */
	pos++;
      }
      for (i=0 ; i < uniques ; i++)
      {
	disk_pos=_ma_uniquedef_read(disk_pos, &share->uniqueinfo[i]);
        disk_pos_assert(share,
                        disk_pos + share->uniqueinfo[i].keysegs *
			HA_KEYSEG_SIZE, end_pos);
	share->uniqueinfo[i].seg=pos;
	for (j=0 ; j < share->uniqueinfo[i].keysegs; j++,pos++)
	{
	  disk_pos=_ma_keyseg_read(disk_pos, pos);
	  if (pos->type == HA_KEYTYPE_TEXT ||
              pos->type == HA_KEYTYPE_VARTEXT1 ||
              pos->type == HA_KEYTYPE_VARTEXT2)
	  {
	    if (!pos->language)
	      pos->charset=default_charset_info;
	    else if (!(pos->charset= get_charset(pos->language, MYF(MY_WME))))
	    {
	      my_errno=HA_ERR_UNKNOWN_CHARSET;
	      goto err;
	    }
	  }
	}
	share->uniqueinfo[i].end=pos;
	pos->type=HA_KEYTYPE_END;			/* End */
	pos->null_bit=0;
	pos->flag=0;
	pos++;
      }
      share->ftkeys= ftkey_nr;
    }
    share->data_file_type= share->state.header.data_file_type;
    share->base_length= (BASE_ROW_HEADER_SIZE +
                         share->base.is_nulls_extended +
                         share->base.null_bytes +
                         share->base.pack_bytes +
                         MY_TEST(share->options & HA_OPTION_CHECKSUM));
    share->kfile.file= kfile;
    /* Pagecaches are not initialize when using aria_chk */
    if (maria_pagecaches.initialized)
      share->pagecache= share->kfile.pagecache=
        multi_get_pagecache(&maria_pagecaches);

    if (open_flags & HA_OPEN_COPY)
    {
      /*
        this instance will be a temporary one used just to create a data
        file for REPAIR. Don't do logging. This base information will not go
        to disk.
      */
      born_transactional= FALSE;
    }
    if (born_transactional)
    {
      share->page_type= PAGECACHE_LSN_PAGE;
      if (share->state.create_rename_lsn == LSN_NEEDS_NEW_STATE_LSNS)
      {
        /*
          Was repaired with maria_chk, maybe later maria_pack-ed. Some sort of
          import into the server. It starts its existence (from the point of
          view of the server, including server's recovery) now.
        */
        if (((open_flags & HA_OPEN_FROM_SQL_LAYER) &&
             (share->state.changed & STATE_NOT_MOVABLE)) || maria_in_recovery)
          _ma_update_state_lsns_sub(share, LSN_IMPOSSIBLE,
                                    trnman_get_min_safe_trid(), TRUE, TRUE);
      }
      else if ((!LSN_VALID(share->state.create_rename_lsn) ||
                !LSN_VALID(share->state.is_of_horizon) ||
                (cmp_translog_addr(share->state.create_rename_lsn,
                                   share->state.is_of_horizon) > 0) ||
                !LSN_VALID(share->state.skip_redo_lsn) ||
                (cmp_translog_addr(share->state.create_rename_lsn,
                                   share->state.skip_redo_lsn) > 0)))
      {
        if (!(open_flags & HA_OPEN_FOR_REPAIR))
        {
          /*
            If in Recovery, it will not work. If LSN is invalid and not
            LSN_NEEDS_NEW_STATE_LSNS, header must be corrupted.
            In both cases, must repair.
          */
          my_errno=((share->state.changed & STATE_CRASHED_ON_REPAIR) ?
                    HA_ERR_CRASHED_ON_REPAIR : HA_ERR_CRASHED_ON_USAGE);
          goto err;
        }
        else
        {
          /*
            Open in repair mode. Ensure that we mark the table crashed, so
            that we run auto_repair on it
          */
          maria_mark_crashed_share(share);
        }
      }
      else if (!(open_flags & HA_OPEN_FOR_REPAIR))
      {
        /* create_rename_lsn != LSN_NEEDS_NEW_STATE_LSNS */
        share->state.changed|= STATE_NOT_MOVABLE;
      }
    }
    else
      share->page_type= PAGECACHE_PLAIN_PAGE;
    share->now_transactional= born_transactional;

    /* Use pack_reclength as we don't want to modify base.pack_recklength */
    if (share->state.header.org_data_file_type == DYNAMIC_RECORD)
    {
      /* add bits used to pack data to pack_reclength for faster allocation */
      share->base.pack_reclength+= share->base.pack_bytes;
      share->base.extra_rec_buff_size=
        (ALIGN_SIZE(MARIA_MAX_DYN_BLOCK_HEADER) + MARIA_SPLIT_LENGTH +
         MARIA_REC_BUFF_OFFSET);
    }
    if (share->data_file_type == COMPRESSED_RECORD)
    {
      /* Need some extra bytes for decode_bytes */
      share->base.extra_rec_buff_size+= 7;
    }
    share->base.default_rec_buff_size= MY_MAX(share->base.pack_reclength +
                                           share->base.extra_rec_buff_size,
                                           share->base.max_key_length);

    disk_pos_assert(share,
                    disk_pos + share->base.fields *MARIA_COLUMNDEF_SIZE,
                    end_pos);
    for (i= j= 0 ; i < share->base.fields ; i++)
    {
      disk_pos=_ma_columndef_read(disk_pos,&share->columndef[i]);
      share->columndef[i].pack_type=0;
      share->columndef[i].huff_tree=0;
      if (share->columndef[i].type == FIELD_BLOB)
      {
	share->blobs[j].pack_length=
	  share->columndef[i].length-portable_sizeof_char_ptr;
	share->blobs[j].offset= share->columndef[i].offset;
	j++;
      }
      if (share->columndef[i].type == FIELD_VARCHAR)
        share->has_varchar_fields= 1;
      if (share->columndef[i].null_bit)
        share->has_null_fields= 1;
    }
    share->columndef[i].type= FIELD_LAST;	/* End marker */
    disk_pos= _ma_column_nr_read(disk_pos, share->column_nr,
                                 share->base.fields);

    if (MY_TEST(share->base.extra_options & MA_EXTRA_OPTIONS_ENCRYPTED))
    {
      if (!(disk_pos= ma_crypt_read(share, disk_pos,
                                    MY_TEST(open_flags & HA_OPEN_FOR_DROP))))
        goto err;
    }

    if ((share->data_file_type == BLOCK_RECORD ||
         share->data_file_type == COMPRESSED_RECORD))
    {
      if (!s3)
      {
        if (_ma_open_datafile(&info, share))
          goto err;
        data_file= info.dfile.file;
      }
#ifdef WITH_S3_STORAGE_ENGINE
      else
        data_file= info.dfile.file= s3f.unique_file_number();
#endif /* WITH_S3_STORAGE_ENGINE */
    }
    errpos= 5;

    if (open_flags & HA_OPEN_DELAY_KEY_WRITE)
      share->options|= HA_OPTION_DELAY_KEY_WRITE;
    if (mode == O_RDONLY)
      share->options|= HA_OPTION_READ_ONLY_DATA;
    share->is_log_table= FALSE;

    share->write_flag=MYF(MY_NABP | MY_WAIT_IF_FULL);
    if (open_flags & HA_OPEN_TMP_TABLE || share->options & HA_OPTION_TMP_TABLE)
    {
      share->options|= HA_OPTION_TMP_TABLE;
      share->temporary= share->delay_key_write= 1;
      share->malloc_flag=
        (open_flags & HA_OPEN_GLOBAL_TMP_TABLE) ? 0 : MY_THREAD_SPECIFIC;
      share->write_flag=MYF(MY_NABP);
      share->w_locks++;			/* We don't have to update status */
      share->tot_locks++;
    }
    share->tracked= MY_TEST(open_flags & HA_OPEN_SIZE_TRACKING);

    _ma_set_index_pagecache_callbacks(&share->kfile, share);
    share->this_process=(ulong) getpid();
#ifdef MARIA_EXTERNAL_LOCKING
    share->last_process= share->state.process;
#endif
    share->base.key_parts=key_parts;
    share->base.all_key_parts=key_parts+unique_key_parts;
    if (!(share->last_version=share->state.version))
      share->last_version=1;			/* Safety */
    share->rec_reflength=share->base.rec_reflength; /* May be changed */
    share->base.margin_key_file_length=(share->base.max_key_file_length -
					(keys ? MARIA_INDEX_BLOCK_MARGIN *
					 share->block_size * keys : 0));
    my_free(disk_cache);
    my_free(share_buff.state.rec_per_key_part);
    disk_cache= 0;
    share_buff.state.rec_per_key_part= 0;

    _ma_setup_functions(share);
    max_data_file_length= share->base.max_data_file_length;
    if ((*share->once_init)(share, info.dfile.file))
      goto err;
    errpos= 6;
    if (internal_table)
      set_if_smaller(share->base.max_data_file_length,
                     max_data_file_length);
    if (share->now_transactional)
    {
      /* Setup initial state that is visible for all */
      MARIA_STATE_HISTORY_CLOSED *history;
      if ((history= (MARIA_STATE_HISTORY_CLOSED *)
           my_hash_search(&maria_stored_state,
                       (uchar*) &share->state.create_rename_lsn,
                       sizeof(share->state.create_rename_lsn))))
      {
        /*
          Move history from hash to share. This is safe to do as we
          know we are the only one that is using the share.
        */
        share->state_history=
          _ma_remove_not_visible_states(history->state_history, 0, 0);
        history->state_history= 0;
        (void) my_hash_delete(&maria_stored_state, (uchar*) history);
        DBUG_PRINT("info", ("Reading state history.  trid: %lu  records: %lld",
                            (ulong) share->state_history->trid,
                            share->state_history->state.records));
      }
      else
      {
        /* Table is not part of any active transaction; Create new history */
        if (!(share->state_history= (MARIA_STATE_HISTORY *)
              my_malloc(PSI_INSTRUMENT_ME, sizeof(*share->state_history),
                        MYF(MY_WME))))
          goto err;
        share->state_history->trid= 0;          /* Visible by all */
        share->state_history->state= share->state.state;
        share->state_history->next= 0;
      }
    }
    errpos= 7;
    thr_lock_init(&share->lock);
    mysql_mutex_init(key_SHARE_intern_lock,
                     &share->intern_lock, MY_MUTEX_INIT_FAST);
    mysql_mutex_init(key_SHARE_key_del_lock,
                     &share->key_del_lock, MY_MUTEX_INIT_FAST);
    mysql_cond_init(key_SHARE_key_del_cond, &share->key_del_cond, 0);
    mysql_mutex_init(key_SHARE_close_lock,
                     &share->close_lock, MY_MUTEX_INIT_FAST);
    for (i=0; i<keys; i++)
      mysql_rwlock_init(key_KEYINFO_root_lock,
                        &share->keyinfo[i].root_lock);
    mysql_rwlock_init(key_SHARE_mmap_lock, &share->mmap_lock);

    share->row_is_visible=   _ma_row_visible_always;
    share->lock.get_status=  _ma_reset_update_flag;
    share->lock.start_trans= _ma_start_trans;

    if (!thr_lock_inited)
    {
      /* Probably a single threaded program; Don't use concurrent inserts */
      maria_concurrent_insert=0;
    }
    else if (maria_concurrent_insert)
    {
      share->non_transactional_concurrent_insert=
	((share->options & (HA_OPTION_READ_ONLY_DATA | HA_OPTION_TMP_TABLE |
                            HA_OPTION_COMPRESS_RECORD |
                            HA_OPTION_TEMP_COMPRESS_RECORD)) ||
	 (open_flags & HA_OPEN_TMP_TABLE) ||
         share->data_file_type == BLOCK_RECORD ||
	 share->have_rtree) ? 0 : 1;
      if (share->non_transactional_concurrent_insert ||
          (!share->temporary && share->now_transactional && versioning))
      {
        share->lock_key_trees= 1;
        if (share->data_file_type == BLOCK_RECORD)
        {
          DBUG_ASSERT(share->now_transactional);
          share->have_versioning= 1;
          share->row_is_visible=     _ma_row_visible_transactional_table;
          share->lock.get_status=    _ma_block_get_status;
          share->lock.check_status=  _ma_block_check_status;
          share->lock.start_trans=   _ma_block_start_trans;
          /*
            We can for the moment only allow multiple concurrent inserts
            only if there is no auto-increment key.  To lift this restriction
            we have to:
            - Extend statement base replication to support auto-increment
            intervalls.
            - Fix that we allocate auto-increment in intervals and that
              it's properly reset if the interval was not used
          */
          share->lock.allow_multiple_concurrent_insert=
            share->base.auto_key == 0;
          share->lock_restore_status= 0;
        }
        else
        {
          share->row_is_visible=      _ma_row_visible_non_transactional_table;
          share->lock.get_status=     _ma_get_status;
          share->lock.copy_status=    _ma_copy_status;
          share->lock.update_status=  _ma_update_status;
          share->lock.restore_status= _ma_restore_status;
          share->lock.check_status=   _ma_check_status;
          share->lock_restore_status= _ma_restore_status;
        }
      }
      else if (share->now_transactional)
      {
        DBUG_ASSERT(share->data_file_type == BLOCK_RECORD);
        share->lock.start_trans=    _ma_block_start_trans_no_versioning;
      }
    }
#ifdef SAFE_MUTEX
    if (share->data_file_type == BLOCK_RECORD)
    {
      /*
        We must have internal_lock before bitmap_lock because we call
        _ma_flush_table_files() with internal_lock locked.
      */
      mysql_mutex_lock(&share->intern_lock);
      mysql_mutex_lock(&share->bitmap.bitmap_lock);
      mysql_mutex_unlock(&share->bitmap.bitmap_lock);
      mysql_mutex_unlock(&share->intern_lock);
    }
#endif
    /*
      Memory mapping can only be requested after initializing intern_lock.
    */
    if (open_flags & HA_OPEN_MMAP)
    {
      info.s= share;
      maria_extra(&info, HA_EXTRA_MMAP, 0);
    }
#ifdef WITH_S3_STORAGE_ENGINE
    if (s3_client)
    {
      size_t block_size= share->base.s3_block_size;
      s3f.set_option(s3_client, MS3_OPT_BUFFER_CHUNK_SIZE, &block_size);
    }
#endif /* WITH_S3_STORAGE_ENGINE */
  }
  else
  {
    share= old_info->s;
    if (share->data_file_type == BLOCK_RECORD)
      data_file= share->bitmap.file.file;       /* Only opened once */
  }

#ifdef WITH_S3_STORAGE_ENGINE
  if (index_header.alloc_ptr)
    s3f.free(&index_header);
#endif /* WITH_S3_STORAGE_ENGINE */

  if (!(m_info= maria_clone_internal(share, mode, data_file,
                                     internal_table, s3_client)))
    goto err;

  if (maria_is_crashed(m_info))
    DBUG_PRINT("warning", ("table is crashed: changed: %u",
                           share->state.changed));

  if (!internal_table)
    mysql_mutex_unlock(&THR_LOCK_maria);

  m_info->open_flags= open_flags;
  m_info->stack_end_ptr= &my_thread_var->stack_ends_here;
  DBUG_PRINT("exit", ("table: %p  name: %s",m_info, name));
  DBUG_RETURN(m_info);

err:
  DBUG_PRINT("error", ("error: %d  errpos: %d", my_errno, errpos));
  save_errno=my_errno ? my_errno : HA_ERR_END_OF_FILE;
  if ((save_errno == HA_ERR_CRASHED) ||
      (save_errno == HA_ERR_CRASHED_ON_USAGE) ||
      (save_errno == HA_ERR_CRASHED_ON_REPAIR))
  {
    LEX_STRING tmp_name;
    tmp_name.str= (char*) name;
    tmp_name.length= strlen(name);
    _ma_report_error(save_errno, &tmp_name, MYF(ME_ERROR_LOG));
  }
  switch (errpos) {
  case 7:
    thr_lock_delete(&share->lock);
    /* fall through */
  case 6:
    /* Avoid mutex test in _ma_bitmap_end() */
    share->internal_table= 1;
    (*share->once_end)(share);
    /* fall through */
  case 5:
    if (data_file >= 0 && !s3_client)
      mysql_file_close(data_file, MYF(0));
    if (old_info)
      break;					/* Don't remove open table */
    /* fall through */
  case 4:
    ma_crypt_free(share);
    my_free(share);
    /* fall through */
  case 3:
    my_free(disk_cache);
    my_free(share_buff.state.rec_per_key_part);
    /* fall through */
  case 1:
    if (!s3)
      mysql_file_close(kfile,MYF(0));
    my_free(share_s3);
    /* fall through */
  case 0:
  default:
    break;
  }
#ifdef WITH_S3_STORAGE_ENGINE
  if (s3_client)
    s3f.deinit(s3_client);
  if (index_header.alloc_ptr)
    s3f.free(&index_header);
#endif /* WITH_S3_STORAGE_ENGINE */
  if (!internal_table)
    mysql_mutex_unlock(&THR_LOCK_maria);
  my_errno= save_errno;
  DBUG_RETURN (NULL);
} /* maria_open */


/*
  Reallocate a buffer, if the current buffer is not large enough
*/

my_bool _ma_alloc_buffer(uchar **old_addr, size_t *old_size,
                         size_t new_size, myf flag)
{
  if (*old_size < new_size)
  {
    uchar *addr;
    if (!(addr= (uchar*) my_realloc(PSI_INSTRUMENT_ME, *old_addr, new_size,
                                    MYF(MY_ALLOW_ZERO_PTR | flag))))
      return 1;
    *old_addr= addr;
    *old_size= new_size;
  }
  return 0;
}


ulonglong _ma_safe_mul(ulonglong a, ulonglong b)
{
  ulonglong max_val= ~ (ulonglong) 0;		/* my_off_t is unsigned */

  if (!a || max_val / a < b)
    return max_val;
  return a*b;
}

	/* Set up functions in structs */

void _ma_setup_functions(register MARIA_SHARE *share)
{
  share->once_init=          maria_once_init_dummy;
  share->once_end=           maria_once_end_dummy;
  share->init=      	     maria_scan_init_dummy;
  share->end=       	     maria_scan_end_dummy;
  share->scan_init=          maria_scan_init_dummy;/* Compat. dummy function */
  share->scan_end=           maria_scan_end_dummy;/* Compat. dummy function */
  share->scan_remember_pos=  _ma_def_scan_remember_pos;
  share->scan_restore_pos=   _ma_def_scan_restore_pos;

  share->write_record_init=  _ma_write_init_default;
  share->write_record_abort= _ma_write_abort_default;
  share->keypos_to_recpos=   _ma_transparent_recpos;
  share->recpos_to_keypos=   _ma_transparent_recpos;

  switch (share->data_file_type) {
  case COMPRESSED_RECORD:
    share->read_record= _ma_read_pack_record;
    share->scan= _ma_read_rnd_pack_record;
    share->once_init= _ma_once_init_pack_row;
    share->once_end=  _ma_once_end_pack_row;
    /*
      Calculate checksum according to data in the original, not compressed,
      row.
    */
    if (share->state.header.org_data_file_type == STATIC_RECORD &&
        ! (share->options & HA_OPTION_NULL_FIELDS))
      share->calc_checksum= _ma_static_checksum;
    else
      share->calc_checksum= _ma_checksum;
    share->calc_write_checksum= share->calc_checksum;
    break;
  case DYNAMIC_RECORD:
    share->read_record= _ma_read_dynamic_record;
    share->scan= _ma_read_rnd_dynamic_record;
    share->delete_record= _ma_delete_dynamic_record;
    share->compare_record= _ma_cmp_dynamic_record;
    share->compare_unique= _ma_cmp_dynamic_unique;
    share->calc_checksum= share->calc_write_checksum= _ma_checksum;
    if (share->base.blobs)
    {
      share->update_record= _ma_update_blob_record;
      share->write_record= _ma_write_blob_record;
    }
    else
    {
      share->write_record= _ma_write_dynamic_record;
      share->update_record= _ma_update_dynamic_record;
    }
    break;
  case STATIC_RECORD:
    share->read_record=      _ma_read_static_record;
    share->scan=             _ma_read_rnd_static_record;
    share->delete_record=    _ma_delete_static_record;
    share->compare_record=   _ma_cmp_static_record;
    share->update_record=    _ma_update_static_record;
    share->write_record=     _ma_write_static_record;
    share->compare_unique=   _ma_cmp_static_unique;
    share->keypos_to_recpos= _ma_static_keypos_to_recpos;
    share->recpos_to_keypos= _ma_static_recpos_to_keypos;
    if (share->state.header.org_data_file_type == STATIC_RECORD &&
        ! (share->options & HA_OPTION_NULL_FIELDS))
      share->calc_checksum= _ma_static_checksum;
    else
      share->calc_checksum= _ma_checksum;
    break;
  case NO_RECORD:
    share->read_record=      _ma_read_no_record;
    share->scan=             _ma_read_rnd_no_record;
    share->delete_record=    _ma_delete_no_record;
    share->update_record=    _ma_update_no_record;
    share->write_record=     _ma_write_no_record;
    share->recpos_to_keypos= _ma_no_keypos_to_recpos;
    share->keypos_to_recpos= _ma_no_keypos_to_recpos;

    /* Abort if following functions are called */
    share->compare_record=   0;
    share->compare_unique=   0;
    share->calc_checksum= 0;
    break;
  case BLOCK_RECORD:
    share->once_init= _ma_once_init_block_record;
    share->once_end=  _ma_once_end_block_record;
    share->init=      _ma_init_block_record;
    share->end=       _ma_end_block_record;
    share->write_record_init= _ma_write_init_block_record;
    share->write_record_abort= _ma_write_abort_block_record;
    share->scan_init=   _ma_scan_init_block_record;
    share->scan_end=    _ma_scan_end_block_record;
    share->scan=        _ma_scan_block_record;
    share->scan_remember_pos=  _ma_scan_remember_block_record;
    share->scan_restore_pos=   _ma_scan_restore_block_record;
    share->read_record= _ma_read_block_record;
    share->delete_record= _ma_delete_block_record;
    share->compare_record= _ma_compare_block_record;
    share->update_record= _ma_update_block_record;
    share->write_record=  _ma_write_block_record;
    share->compare_unique= _ma_cmp_block_unique;
    share->calc_checksum= _ma_checksum;
    share->keypos_to_recpos= _ma_transaction_keypos_to_recpos;
    share->recpos_to_keypos= _ma_transaction_recpos_to_keypos;

    /*
      write_block_record() will calculate the checksum; Tell maria_write()
      that it doesn't have to do this.
    */
    share->calc_write_checksum= 0;
    break;
  }
  share->file_read= _ma_nommap_pread;
  share->file_write= _ma_nommap_pwrite;
  share->calc_check_checksum= share->calc_checksum;

  if (!(share->options & HA_OPTION_CHECKSUM) &&
      share->data_file_type != COMPRESSED_RECORD)
    share->calc_checksum= share->calc_write_checksum= 0;
  return;
}


static void setup_key_functions(register MARIA_KEYDEF *keyinfo)
{
  if (keyinfo->key_alg == HA_KEY_ALG_RTREE)
  {
    keyinfo->ck_insert = maria_rtree_insert;
    keyinfo->ck_delete = maria_rtree_delete;
  }
  else
  {
    keyinfo->ck_insert = _ma_ck_write;
    keyinfo->ck_delete = _ma_ck_delete;
  }
  if (keyinfo->key_alg == HA_KEY_ALG_RTREE)
    keyinfo->make_key= _ma_sp_make_key;
  else
    keyinfo->make_key= _ma_make_key;

  if (keyinfo->flag & HA_BINARY_PACK_KEY)
  {						/* Simple prefix compression */
    keyinfo->bin_search= _ma_seq_search;
    keyinfo->get_key= _ma_get_binary_pack_key;
    keyinfo->skip_key= _ma_skip_binary_pack_key;
    keyinfo->pack_key= _ma_calc_bin_pack_key_length;
    keyinfo->store_key= _ma_store_bin_pack_key;
  }
  else if (keyinfo->flag & HA_VAR_LENGTH_KEY)
  {
    keyinfo->get_key=  _ma_get_pack_key;
    keyinfo->skip_key= _ma_skip_pack_key;
    if (keyinfo->seg[0].flag & HA_PACK_KEY)
    {						/* Prefix compression */
      /*
        _ma_prefix_search() compares end-space against ASCII blank (' ').
        It cannot be used for character sets, that do not encode the
        blank character like ASCII does. UCS2 is an example. All
        character sets with a fixed width > 1 or a mimimum width > 1
        cannot represent blank like ASCII does. In these cases we have
        to use _ma_seq_search() for the search.
      */
      if (!keyinfo->seg->charset || use_strnxfrm(keyinfo->seg->charset) ||
          (keyinfo->seg->flag & HA_NULL_PART) ||
          keyinfo->seg->charset->mbminlen > 1)
        keyinfo->bin_search= _ma_seq_search;
      else
        keyinfo->bin_search= _ma_prefix_search;
      keyinfo->pack_key= _ma_calc_var_pack_key_length;
      keyinfo->store_key= _ma_store_var_pack_key;
    }
    else
    {
      keyinfo->bin_search= _ma_seq_search;
      keyinfo->pack_key= _ma_calc_var_key_length; /* Variable length key */
      keyinfo->store_key= _ma_store_static_key;
    }
  }
  else
  {
    keyinfo->bin_search= _ma_bin_search;
    keyinfo->get_key= _ma_get_static_key;
    keyinfo->skip_key= _ma_skip_static_key;
    keyinfo->pack_key= _ma_calc_static_key_length;
    keyinfo->store_key= _ma_store_static_key;
  }

  /* set keyinfo->write_comp_flag */
  if (keyinfo->flag & HA_SORT_ALLOWS_SAME)
    keyinfo->write_comp_flag=SEARCH_BIGGER; /* Put after same key */
  else if (keyinfo->flag & HA_NOSAME)
  {
    keyinfo->write_comp_flag= SEARCH_FIND | SEARCH_UPDATE; /* No duplicates */
    if (keyinfo->flag & HA_NULL_ARE_EQUAL)
      keyinfo->write_comp_flag|= SEARCH_NULL_ARE_EQUAL;
  }
  else if (keyinfo->key_alg == HA_KEY_ALG_FULLTEXT)
  {
    keyinfo->write_comp_flag= SEARCH_FIND | SEARCH_UPDATE;
  }
  else
    keyinfo->write_comp_flag= SEARCH_SAME; /* Keys in rec-pos order */
  keyinfo->write_comp_flag|= SEARCH_INSERT;
  return;
}


/**
   @brief Function to save and store the header in the index file (.MAI)

   Operates under MARIA_SHARE::intern_lock if requested.
   Sets MARIA_SHARE::MARIA_STATE_INFO::is_of_horizon if transactional table.
   Then calls _ma_state_info_write_sub().

   @param  share           table
   @param  pWrite          bitmap: if 1 (MA_STATE_INFO_WRITE_DONT_MOVE_OFFSET)
                           is set my_pwrite() is used otherwise my_write();
                           if 2 (MA_STATE_INFO_WRITE_FULL_INFO) is set, info
                           about keys is written (should only be needed
                           after ALTER TABLE ENABLE/DISABLE KEYS, and
                           REPAIR/OPTIMIZE); if 4 (MA_STATE_INFO_WRITE_LOCK)
                           is set, MARIA_SHARE::intern_lock is taken.

   @return Operation status
     @retval 0      OK
     @retval 1      Error
*/

uint _ma_state_info_write(MARIA_SHARE *share, uint pWrite)
{
  uint res;
  if (share->options & HA_OPTION_READ_ONLY_DATA)
    return 0;

  if (pWrite & MA_STATE_INFO_WRITE_LOCK)
    mysql_mutex_lock(&share->intern_lock);
  else if (maria_multi_threaded && !share->temporary)
    mysql_mutex_assert_owner(&share->intern_lock);
  if (share->base.born_transactional && translog_status == TRANSLOG_OK &&
      !maria_in_recovery)
  {
    /*
      In a recovery, we want to set is_of_horizon to the LSN of the last
      record executed by Recovery, not the current EOF of the log (which
      is too new). Recovery does it by itself.
    */
    share->state.is_of_horizon= translog_get_horizon();
    DBUG_PRINT("info", ("is_of_horizon set to LSN " LSN_FMT "",
                        LSN_IN_PARTS(share->state.is_of_horizon)));
  }
  res= _ma_state_info_write_sub(share->kfile.file, &share->state, pWrite);
  if (pWrite & MA_STATE_INFO_WRITE_LOCK)
    mysql_mutex_unlock(&share->intern_lock);
  /* If open_count != 0 we have to write the state again at close */
  share->changed= share->state.open_count != 0;
  return res;
}


/**
   @brief Function to save and store the header in the index file (.MYI).

   Shortcut to use instead of _ma_state_info_write() when appropriate.

   @param  file            descriptor of the index file to write
   @param  state           state information to write to the file
   @param  pWrite          bitmap: if 1 (MA_STATE_INFO_WRITE_DONT_MOVE_OFFSET)
                           is set my_pwrite() is used otherwise my_write();
                           if 2 (MA_STATE_INFO_WRITE_FULL_INFO) is set, info
                           about keys is written (should only be needed
                           after ALTER TABLE ENABLE/DISABLE KEYS, and
                           REPAIR/OPTIMIZE).

   @notes
     For transactional multiuser tables, this function is called
     with intern_lock & translog_lock or when the last thread who
     is using the table is closing it.
     Because of the translog_lock we don't need to have a lock on
     key_del_lock.

   @return Operation status
     @retval 0      OK
     @retval 1      Error
*/

/* MARIA_STATE_INFO_SIZE + MARIA_STATE_EXTRA_SIZE == 25559 */
PRAGMA_DISABLE_CHECK_STACK_FRAME

uint _ma_state_info_write_sub(File file, MARIA_STATE_INFO *state, uint pWrite)
{
  uchar  buff[MARIA_STATE_INFO_SIZE + MARIA_STATE_EXTRA_SIZE];
  uchar *ptr=buff;
  uint	i, keys= (uint) state->header.keys;
  size_t res;
  DBUG_ENTER("_ma_state_info_write_sub");
  DBUG_PRINT("info", ("Records: %lld", state->state.records));

  memcpy(ptr,&state->header,sizeof(state->header));
  ptr+=sizeof(state->header);

  /* open_count must be first because of _ma_mark_file_changed ! */
  mi_int2store(ptr,state->open_count);			ptr+= 2;
  /* changed must be second, because of _ma_mark_file_crashed */
  mi_int2store(ptr,state->changed);			ptr+= 2;

  /*
    If you change the offset of these LSNs, note that some functions do a
    direct write of them without going through this function.
  */
  lsn_store(ptr, state->create_rename_lsn);		ptr+= LSN_STORE_SIZE;
  lsn_store(ptr, state->is_of_horizon);			ptr+= LSN_STORE_SIZE;
  lsn_store(ptr, state->skip_redo_lsn);			ptr+= LSN_STORE_SIZE;
  mi_rowstore(ptr,state->state.records);		ptr+= 8;
  mi_rowstore(ptr,state->state.del);			ptr+= 8;
  mi_rowstore(ptr,state->split);			ptr+= 8;
  mi_sizestore(ptr,state->dellink);			ptr+= 8;
  mi_sizestore(ptr,state->first_bitmap_with_space);	ptr+= 8;
  mi_sizestore(ptr,state->state.key_file_length);	ptr+= 8;
  mi_sizestore(ptr,state->state.data_file_length);	ptr+= 8;
  mi_sizestore(ptr,state->state.empty);			ptr+= 8;
  mi_sizestore(ptr,state->state.key_empty);		ptr+= 8;
  mi_int8store(ptr,state->auto_increment);		ptr+= 8;
  mi_int8store(ptr,(ulonglong) state->state.checksum);	ptr+= 8;
  mi_int8store(ptr,state->create_trid);			ptr+= 8;
  mi_int4store(ptr,state->status);			ptr+= 4;
  mi_int4store(ptr,state->update_count);		ptr+= 4;
  *ptr++= state->sortkey;
  *ptr++= 0;                                    /* Reserved */
  ptr+=	state->state_diff_length;

  for (i=0; i < keys; i++)
  {
    mi_sizestore(ptr,state->key_root[i]);		ptr+= 8;
  }
  mi_sizestore(ptr,state->key_del);	        	ptr+= 8;
  if (pWrite & MA_STATE_INFO_WRITE_FULL_INFO)	/* From maria_chk */
  {
    uint key_parts= mi_uint2korr(state->header.key_parts);
    mi_int4store(ptr,state->sec_index_changed); 	ptr+= 4;
    mi_int4store(ptr,state->sec_index_used);		ptr+= 4;
    mi_int4store(ptr,state->version);			ptr+= 4;
    mi_int8store(ptr,state->key_map);			ptr+= 8;
    mi_int8store(ptr,(ulonglong) state->create_time);	ptr+= 8;
    mi_int8store(ptr,(ulonglong) state->recover_time);	ptr+= 8;
    mi_int8store(ptr,(ulonglong) state->check_time);	ptr+= 8;
    mi_sizestore(ptr, state->records_at_analyze);	ptr+= 8;
    /* reserve place for some information per key */
    bzero(ptr, keys*4); 				ptr+= keys*4;
    for (i=0 ; i < key_parts ; i++)
    {
      float8store(ptr, state->rec_per_key_part[i]);  	ptr+= 8;
      mi_int4store(ptr, state->nulls_per_key_part[i]);  ptr+= 4;
    }
  }

  res= (pWrite & MA_STATE_INFO_WRITE_DONT_MOVE_OFFSET) ?
    mysql_file_pwrite(file, buff, (size_t) (ptr-buff), 0L,
              MYF(MY_NABP | MY_THREADSAFE)) :
    mysql_file_write(file,  buff, (size_t) (ptr-buff),
             MYF(MY_NABP));
  DBUG_RETURN(res != 0);
}
PRAGMA_REENABLE_CHECK_STACK_FRAME


static uchar *_ma_state_info_read(uchar *ptr, MARIA_STATE_INFO *state, myf flag)
{
  uint i,keys,key_parts;
  DBUG_ENTER("_ma_state_info_read");

  memcpy(&state->header,ptr, sizeof(state->header));
  ptr+= sizeof(state->header);
  keys= (uint) state->header.keys;
  key_parts= mi_uint2korr(state->header.key_parts);

  /* Allocate memory for key parts if not already done */
  if (!state->rec_per_key_part &&
      !my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME | flag),
                       &state->rec_per_key_part,
                       sizeof(*state->rec_per_key_part) * key_parts,
                       &state->nulls_per_key_part,
                       sizeof(*state->nulls_per_key_part) * key_parts,
                       NullS))
    DBUG_RETURN(0);

  state->open_count = mi_uint2korr(ptr);		ptr+= 2;
  state->changed= mi_uint2korr(ptr);			ptr+= 2;
  state->create_rename_lsn= lsn_korr(ptr);		ptr+= LSN_STORE_SIZE;
  state->is_of_horizon= lsn_korr(ptr);			ptr+= LSN_STORE_SIZE;
  state->skip_redo_lsn= lsn_korr(ptr);			ptr+= LSN_STORE_SIZE;
  state->state.records= mi_rowkorr(ptr);		ptr+= 8;
  state->state.del = mi_rowkorr(ptr);			ptr+= 8;
  state->split	= mi_rowkorr(ptr);			ptr+= 8;
  state->dellink= mi_sizekorr(ptr);			ptr+= 8;
  state->first_bitmap_with_space= mi_sizekorr(ptr);	ptr+= 8;
  state->state.key_file_length = mi_sizekorr(ptr);	ptr+= 8;
  state->state.data_file_length= mi_sizekorr(ptr);	ptr+= 8;
  state->state.empty	= mi_sizekorr(ptr);		ptr+= 8;
  state->state.key_empty= mi_sizekorr(ptr);		ptr+= 8;
  state->auto_increment=mi_uint8korr(ptr);		ptr+= 8;
  state->state.checksum=(ha_checksum) mi_uint8korr(ptr);ptr+= 8;
  state->create_trid= mi_uint8korr(ptr);		ptr+= 8;
  state->status = mi_uint4korr(ptr);			ptr+= 4;
  state->update_count=mi_uint4korr(ptr);		ptr+= 4;
  state->sortkey= 					(uint) *ptr++;
  ptr++;                                                /* reserved */

  ptr+= state->state_diff_length;

  for (i=0; i < keys; i++)
  {
    state->key_root[i]= mi_sizekorr(ptr);		ptr+= 8;
  }
  state->key_del= mi_sizekorr(ptr);			ptr+= 8;
  state->sec_index_changed = mi_uint4korr(ptr); 	ptr+= 4;
  state->sec_index_used =    mi_uint4korr(ptr); 	ptr+= 4;
  state->version     = mi_uint4korr(ptr);		ptr+= 4;
  state->key_map     = mi_uint8korr(ptr);		ptr+= 8;
  state->create_time = (time_t) mi_sizekorr(ptr);	ptr+= 8;
  state->recover_time =(time_t) mi_sizekorr(ptr);	ptr+= 8;
  state->check_time =  (time_t) mi_sizekorr(ptr);	ptr+= 8;
  state->records_at_analyze=    mi_sizekorr(ptr);	ptr+= 8;
  ptr+= keys * 4;                               /* Skip reserved bytes */
  for (i=0 ; i < key_parts ; i++)
  {
    float8get(state->rec_per_key_part[i], ptr);		ptr+= 8;
    state->nulls_per_key_part[i]= mi_uint4korr(ptr);	ptr+= 4;
  }

  DBUG_PRINT("info", ("Records: %lld", state->state.records));
  DBUG_RETURN(ptr);
}


/**
   @brief Fills the state by reading its copy on disk.

   Should not be called for transactional tables, as their state on disk is
   rarely current and so is often misleading for a reader.
   Does nothing in single user mode.

   @param  file            file to read from
   @param  state           state which will be filled
*/

uint _ma_state_info_read_dsk(File file __attribute__((unused)),
                             MARIA_STATE_INFO *state __attribute__((unused)))
{
#ifdef MARIA_EXTERNAL_LOCKING
  uchar	buff[MARIA_STATE_INFO_SIZE + MARIA_STATE_EXTRA_SIZE];

  /* trick to detect transactional tables */
  DBUG_ASSERT(state->create_rename_lsn == LSN_IMPOSSIBLE);
  if (!maria_single_user)
  {
    if (mysql_file_pread(file, buff, state->state_length, 0L, MYF(MY_NABP)))
      return 1;
    _ma_state_info_read(buff, state);
  }
#endif
  return 0;
}


/****************************************************************************
**  store MARIA_BASE_INFO
****************************************************************************/

uint _ma_base_info_write(File file, MARIA_BASE_INFO *base)
{
  uchar buff[MARIA_BASE_INFO_SIZE], *ptr=buff;

  bmove(ptr, maria_uuid, MY_UUID_SIZE);
  ptr+= MY_UUID_SIZE;
  mi_sizestore(ptr,base->keystart);			ptr+= 8;
  mi_sizestore(ptr,base->max_data_file_length);		ptr+= 8;
  mi_sizestore(ptr,base->max_key_file_length);		ptr+= 8;
  mi_rowstore(ptr,base->records);			ptr+= 8;
  mi_rowstore(ptr,base->reloc);				ptr+= 8;
  mi_int4store(ptr,base->mean_row_length);		ptr+= 4;
  mi_int4store(ptr,base->reclength);			ptr+= 4;
  mi_int4store(ptr,base->pack_reclength);		ptr+= 4;
  mi_int4store(ptr,base->min_pack_length);		ptr+= 4;
  mi_int4store(ptr,base->max_pack_length);		ptr+= 4;
  mi_int4store(ptr,base->min_block_length);		ptr+= 4;
  mi_int2store(ptr,base->fields);			ptr+= 2;
  mi_int2store(ptr,base->fixed_not_null_fields);	ptr+= 2;
  mi_int2store(ptr,base->fixed_not_null_fields_length);	ptr+= 2;
  mi_int2store(ptr,base->max_field_lengths);		ptr+= 2;
  mi_int2store(ptr,base->pack_fields);			ptr+= 2;
  mi_int2store(ptr,base->extra_options)			ptr+= 2;
  mi_int2store(ptr,base->null_bytes);                   ptr+= 2;
  mi_int2store(ptr,base->original_null_bytes);	        ptr+= 2;
  mi_int2store(ptr,base->field_offsets);	        ptr+= 2;
  mi_int2store(ptr,base->language);		        ptr+= 2;
  mi_int2store(ptr,base->block_size);	        	ptr+= 2;
  *ptr++= base->rec_reflength;
  *ptr++= base->key_reflength;
  *ptr++= base->keys;
  *ptr++= base->auto_key;
  *ptr++= base->born_transactional;
  *ptr++= base->compression_algorithm;
  mi_int2store(ptr,base->pack_bytes);			ptr+= 2;
  mi_int2store(ptr,base->blobs);			ptr+= 2;
  mi_int2store(ptr,base->max_key_block_length);		ptr+= 2;
  mi_int2store(ptr,base->max_key_length);		ptr+= 2;
  mi_int2store(ptr,base->extra_alloc_bytes);		ptr+= 2;
  *ptr++= base->extra_alloc_procent;
  mi_int3store(ptr, base->s3_block_size);               ptr+= 3;
  bzero(ptr,13);					ptr+= 13; /* extra */
  DBUG_ASSERT((ptr - buff) == MARIA_BASE_INFO_SIZE);
  return mysql_file_write(file, buff, (size_t) (ptr-buff), MYF(MY_NABP)) != 0;
}


/*--------------------------------------------------------------------------
  maria_keydef
---------------------------------------------------------------------------*/

my_bool _ma_keydef_write(File file, MARIA_KEYDEF *keydef)
{
  uchar buff[MARIA_KEYDEF_SIZE];
  uchar *ptr=buff;

  *ptr++= (uchar) keydef->keysegs;
  *ptr++= keydef->key_alg;			/* Rtree or Btree */
  mi_int2store(ptr,keydef->flag);		ptr+= 2;
  mi_int2store(ptr,keydef->block_length);	ptr+= 2;
  mi_int2store(ptr,keydef->keylength);		ptr+= 2;
  mi_int2store(ptr,keydef->minlength);		ptr+= 2;
  mi_int2store(ptr,keydef->maxlength);		ptr+= 2;
  return mysql_file_write(file, buff, (size_t) (ptr-buff), MYF(MY_NABP)) != 0;
}

uchar *_ma_keydef_read(uchar *ptr, MARIA_KEYDEF *keydef)
{
   keydef->keysegs	= (uint) *ptr++;
   keydef->key_alg	= *ptr++;		/* Rtree or Btree */

   keydef->flag		= mi_uint2korr(ptr);	ptr+= 2;
   keydef->block_length = mi_uint2korr(ptr);	ptr+= 2;
   keydef->keylength	= mi_uint2korr(ptr);	ptr+= 2;
   keydef->minlength	= mi_uint2korr(ptr);	ptr+= 2;
   keydef->maxlength	= mi_uint2korr(ptr);	ptr+= 2;
   keydef->version	= 0;			/* Not saved */
   keydef->parser       = &ft_default_parser;
   keydef->ftkey_nr     = 0;
   return ptr;
}

/***************************************************************************
**  maria_keyseg
***************************************************************************/

my_bool _ma_keyseg_write(File file, const HA_KEYSEG *keyseg)
{
  uchar buff[HA_KEYSEG_SIZE];
  uchar *ptr=buff;
  ulong pos;

  *ptr++= keyseg->type;
  *ptr++= keyseg->language & 0xFF; /* Collation ID, low byte */
  *ptr++= keyseg->null_bit;
  *ptr++= keyseg->bit_start;
  *ptr++= keyseg->language >> 8; /* Collation ID, high byte */
  *ptr++= keyseg->bit_length;
  mi_int2store(ptr,keyseg->flag);	ptr+= 2;
  mi_int2store(ptr,keyseg->length);	ptr+= 2;
  mi_int4store(ptr,keyseg->start);	ptr+= 4;
  pos= keyseg->null_bit ? keyseg->null_pos : keyseg->bit_pos;
  mi_int4store(ptr, pos);
  ptr+=4;

  return mysql_file_write(file, buff, (size_t) (ptr-buff), MYF(MY_NABP)) != 0;
}


uchar *_ma_keyseg_read(uchar *ptr, HA_KEYSEG *keyseg)
{
   keyseg->type		= *ptr++;
   keyseg->language	= *ptr++;
   keyseg->null_bit	= *ptr++;
   keyseg->bit_start	= *ptr++;
   keyseg->language	+= ((uint16) (*ptr++)) << 8;
   keyseg->bit_length   = *ptr++;
   keyseg->flag		= mi_uint2korr(ptr);  ptr+= 2;
   keyseg->length	= mi_uint2korr(ptr);  ptr+= 2;
   keyseg->start	= mi_uint4korr(ptr);  ptr+= 4;
   keyseg->null_pos	= mi_uint4korr(ptr);  ptr+= 4;
   keyseg->charset=0;				/* Will be filled in later */
   if (keyseg->null_bit)
     keyseg->bit_pos= (uint16)(keyseg->null_pos + (keyseg->null_bit == 7));
   else
   {
     keyseg->bit_pos= (uint16)keyseg->null_pos;
     keyseg->null_pos= 0;
   }
   return ptr;
}

/*--------------------------------------------------------------------------
  maria_uniquedef
---------------------------------------------------------------------------*/

my_bool _ma_uniquedef_write(File file, MARIA_UNIQUEDEF *def)
{
  uchar buff[MARIA_UNIQUEDEF_SIZE];
  uchar *ptr=buff;

  mi_int2store(ptr,def->keysegs);		ptr+=2;
  *ptr++=  (uchar) def->key;
  *ptr++ = (uchar) def->null_are_equal;

  return mysql_file_write(file, buff, (size_t) (ptr-buff), MYF(MY_NABP)) != 0;
}

uchar *_ma_uniquedef_read(uchar *ptr, MARIA_UNIQUEDEF *def)
{
   def->keysegs = mi_uint2korr(ptr);
   def->key	= ptr[2];
   def->null_are_equal=ptr[3];
   return ptr+4;				/* 1 extra uchar */
}

/***************************************************************************
**  MARIA_COLUMNDEF
***************************************************************************/

my_bool _ma_columndef_write(File file, MARIA_COLUMNDEF *columndef)
{
  uchar buff[MARIA_COLUMNDEF_SIZE];
  uchar *ptr=buff;
  uint low_offset=  (uint) (columndef->offset & 0xffff);
  uint high_offset= (uint) (columndef->offset >> 16);

  mi_int2store(ptr,(ulong) columndef->column_nr); ptr+= 2;
  mi_int2store(ptr, low_offset);		  ptr+= 2;
  mi_int2store(ptr,columndef->type);		  ptr+= 2;
  mi_int2store(ptr,columndef->length);		  ptr+= 2;
  mi_int2store(ptr,columndef->fill_length);	  ptr+= 2;
  mi_int2store(ptr,columndef->null_pos);	  ptr+= 2;
  mi_int2store(ptr,columndef->empty_pos);	  ptr+= 2;

  (*ptr++)= columndef->null_bit;
  (*ptr++)= columndef->empty_bit;
  mi_int2store(ptr, high_offset);                 ptr+= 2;
  ptr[0]= ptr[1]= 0;                              ptr+= 2;  /* For future */
  return mysql_file_write(file, buff, (size_t) (ptr-buff), MYF(MY_NABP)) != 0;
}

uchar *_ma_columndef_read(uchar *ptr, MARIA_COLUMNDEF *columndef)
{
  uint high_offset;
  columndef->column_nr= mi_uint2korr(ptr);      ptr+= 2;
  columndef->offset= mi_uint2korr(ptr);         ptr+= 2;
  columndef->type=   mi_sint2korr(ptr);		ptr+= 2;
  columndef->length= mi_uint2korr(ptr);		ptr+= 2;
  columndef->fill_length= mi_uint2korr(ptr);	ptr+= 2;
  columndef->null_pos= mi_uint2korr(ptr);	ptr+= 2;
  columndef->empty_pos= mi_uint2korr(ptr);	ptr+= 2;
  columndef->null_bit=  (uint8) *ptr++;
  columndef->empty_bit= (uint8) *ptr++;
  high_offset=       mi_uint2korr(ptr);         ptr+= 2;
  columndef->offset|= ((ulong) high_offset << 16);
  ptr+= 2;
  return ptr;
}

my_bool _ma_column_nr_write(File file, uint16 *offsets, uint columns)
{
  uchar *buff, *ptr, *end;
  size_t size= columns*2;
  my_bool res;

  if (!(buff= (uchar*) my_alloca(size)))
    return 1;
  for (ptr= buff, end= ptr + size; ptr < end ; ptr+= 2, offsets++)
    int2store(ptr, *offsets);
  res= mysql_file_write(file, buff, size, MYF(MY_NABP)) != 0;
  my_afree(buff);
  return res;
}


uchar *_ma_column_nr_read(uchar *ptr, uint16 *offsets, uint columns)
{
  uchar *end;
  size_t size= columns*2;
  for (end= ptr + size; ptr < end ; ptr+=2, offsets++)
    *offsets= uint2korr(ptr);
  return ptr;
}

/**
   @brief Set callbacks for data pages

   @note
   We don't use pagecache_file_init here, as we want to keep the
   code readable
*/

void _ma_set_data_pagecache_callbacks(PAGECACHE_FILE *file,
                                      MARIA_SHARE *share)
{
  pagecache_file_set_null_hooks(file);
  file->callback_data= (uchar*) share;
  file->flush_log_callback= &maria_flush_log_for_page_none; /* Do nothing */
  file->post_write_hook= maria_page_write_failure;

  if (share->temporary)
  {
    file->post_read_hook= &maria_page_crc_check_none;
    file->pre_write_hook= &maria_page_filler_set_none;
  }
  else
  {
    file->post_read_hook= &maria_page_crc_check_data;
    if (share->options & HA_OPTION_PAGE_CHECKSUM)
      file->pre_write_hook= &maria_page_crc_set_normal;
    else
      file->pre_write_hook= &maria_page_filler_set_normal;
    if (share->now_transactional)
      file->flush_log_callback= maria_flush_log_for_page;
  }

  if (MY_TEST(share->base.extra_options & MA_EXTRA_OPTIONS_ENCRYPTED))
  {
    ma_crypt_set_data_pagecache_callbacks(file, share);
  }
}


/**
   @brief Set callbacks for index pages

   @note
   We don't use pagecache_file_init here, as we want to keep the
   code readable
*/

void _ma_set_index_pagecache_callbacks(PAGECACHE_FILE *file,
                                       MARIA_SHARE *share)
{
  pagecache_file_set_null_hooks(file);
  file->callback_data= (uchar*) share;
  file->flush_log_callback= &maria_flush_log_for_page_none; /* Do nothing */
  file->post_write_hook= maria_page_write_failure;

  if (share->temporary)
  {
    file->post_read_hook= &maria_page_crc_check_none;
    file->pre_write_hook= &maria_page_filler_set_none;
  }
  else
  {
    file->post_read_hook=  &maria_page_crc_check_index;
    if (share->options & HA_OPTION_PAGE_CHECKSUM)
      file->pre_write_hook= &maria_page_crc_set_index;
    else
      file->pre_write_hook= &maria_page_filler_set_normal;

    if (share->now_transactional)
      file->flush_log_callback= maria_flush_log_for_page;
  }

  if (MY_TEST(share->base.extra_options & MA_EXTRA_OPTIONS_ENCRYPTED))
  {
    ma_crypt_set_index_pagecache_callbacks(file, share);
  }
}


/**************************************************************************
 Open data file
  We can't use dup() here as the data file descriptors need to have different
  active seek-positions.
*************************************************************************/

int _ma_open_datafile(MARIA_HA *info, MARIA_SHARE *share)
{
  myf flags= ((share->mode & O_NOFOLLOW) ? MY_NOSYMLINKS | MY_WME : MY_WME) |
    share->malloc_flag;
  DEBUG_SYNC_C("mi_open_datafile");
  info->dfile.file= share->bitmap.file.file=
    mysql_file_open(key_file_dfile, share->data_file_name.str,
                    share->mode | O_SHARE | O_CLOEXEC, flags);
  /* Note that share->pagecache may be 0 here if run from aria_chk */
  info->dfile.pagecache= share->bitmap.file.pagecache= share->pagecache;
  return info->dfile.file >= 0 ? 0 : 1;
}


int _ma_open_keyfile(MARIA_SHARE *share)
{
  /*
    Modifications to share->kfile should be under intern_lock to protect
    against a concurrent checkpoint.
  */
  mysql_mutex_lock(&share->intern_lock);
  share->kfile.file= mysql_file_open(key_file_kfile,
                                     share->unique_file_name.str,
                                     share->mode | O_SHARE | O_NOFOLLOW | O_CLOEXEC,
                             MYF(MY_WME | MY_NOSYMLINKS));
  /*
    share->kfile.pagecache is updated in the caller if needed.
    This is needed as we don't want to change pagecache in ma_sort_index()
    as we want to check pagecache concistency in ma_close().
  */
  mysql_mutex_unlock(&share->intern_lock);
  return (share->kfile.file < 0);
}


/*
  Update pagecaches for a table. Used by aria_check() which creates
  the pagecache after the table has been opened.
*/

void ma_change_pagecache(MARIA_HA *info)
{
  MARIA_SHARE *share= info->s;
  DBUG_ASSERT(share->pagecache == 0);
  share->pagecache= share->bitmap.file.pagecache= share->kfile.pagecache=
    info->dfile.pagecache= multi_get_pagecache(&maria_pagecaches);
}


/*
  Disable all indexes.

  SYNOPSIS
    maria_disable_indexes()
    info        A pointer to the MARIA storage engine MARIA_HA struct.

  DESCRIPTION
    Disable all indexes.

  RETURN
    0  ok
*/

int maria_disable_indexes(MARIA_HA *info)
{
  MARIA_SHARE *share= info->s;

  maria_clear_all_keys_active(share->state.key_map);
  return 0;
}


/*
  Enable all indexes

  SYNOPSIS
    maria_enable_indexes()
    info        A pointer to the MARIA storage engine MARIA_HA struct.

  DESCRIPTION
    Enable all indexes. The indexes might have been disabled
    by maria_disable_index() before.
    The function works only if both data and indexes are empty,
    otherwise a repair is required.
    To be sure, call handler::delete_all_rows() before.

  RETURN
    0  ok
    HA_ERR_CRASHED data or index is non-empty.
*/

int maria_enable_indexes(MARIA_HA *info)
{
  int error= 0;
  MARIA_SHARE *share= info->s;
  DBUG_ENTER("maria_enable_indexes");

  if ((share->state.state.data_file_length !=
       (share->data_file_type == BLOCK_RECORD ? share->block_size : 0)) ||
      (share->state.state.key_file_length != share->base.keystart))
  {
    DBUG_PRINT("error", ("data_file_length: %lu  key_file_length: %lu",
                         (ulong) share->state.state.data_file_length,
                         (ulong) share->state.state.key_file_length));
    _ma_set_fatal_error(info, HA_ERR_CRASHED);
    error= HA_ERR_CRASHED;
  }
  else
    maria_set_all_keys_active(share->state.key_map, share->base.keys);
  DBUG_RETURN(error);
}


/*
  Test if indexes are disabled.

  SYNOPSIS
    maria_indexes_are_disabled()
    info        A pointer to the MARIA storage engine MARIA_HA struct.

  DESCRIPTION
    Test if indexes are disabled.

  RETURN
    0  indexes are not disabled
    1  all indexes are disabled
    2  non-unique indexes are disabled
*/

int maria_indexes_are_disabled(MARIA_HA *info)
{
  MARIA_SHARE *share= info->s;

  /*
    No keys or all are enabled. keys is the number of keys. Left shifted
    gives us only one bit set. When decreased by one, gives us all all bits
    up to this one set and it gets unset.
  */
  if (!share->base.keys ||
      (maria_is_all_keys_active(share->state.key_map, share->base.keys)))
    return 0;

  /* All are disabled */
  if (maria_is_any_key_active(share->state.key_map))
    return 1;

  /*
    We have keys. Some enabled, some disabled.
    Don't check for any non-unique disabled but return directly 2
  */
  return 2;
}


static my_bool maria_scan_init_dummy(MARIA_HA *info __attribute__((unused)))
{
  return 0;
}

static void maria_scan_end_dummy(MARIA_HA *info __attribute__((unused)))
{
}

static my_bool maria_once_init_dummy(MARIA_SHARE *share
                                     __attribute__((unused)),
                                     File dfile __attribute__((unused)))
{
  return 0;
}

static my_bool maria_once_end_dummy(MARIA_SHARE *share __attribute__((unused)))
{
  return 0;
}
