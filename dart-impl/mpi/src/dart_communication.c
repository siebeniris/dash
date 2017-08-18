/**
 * \file dart_communication.c
 *
 * Implementations of all the dart communication operations.
 *
 * All the following functions are implemented with the underling *MPI-3*
 * one-sided runtime system.
 */

#include <dash/dart/if/dart_types.h>
#include <dash/dart/if/dart_initialization.h>
#include <dash/dart/if/dart_globmem.h>
#include <dash/dart/if/dart_team_group.h>
#include <dash/dart/if/dart_communication.h>

#include <dash/dart/mpi/dart_communication_priv.h>
#include <dash/dart/mpi/dart_team_private.h>
#include <dash/dart/mpi/dart_mem.h>
#include <dash/dart/mpi/dart_mpi_util.h>
#include <dash/dart/mpi/dart_segment.h>
#include <dash/dart/mpi/dart_globmem_priv.h>

#include <dash/dart/base/logging.h>
#include <dash/dart/base/math.h>

#include <stdio.h>
#include <mpi.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <alloca.h>

/**
 * The maximum number of elements of a certain type to be
 * transfered in one chunk.
 */
#define MAX_CONTIG_ELEMENTS INT_MAX

#define CHECK_UNITID_RANGE(_unitid, _team_data)                             \
  do {                                                                      \
    if (dart__unlikely(_unitid.id < 0 || _unitid.id > _team_data->size)) {  \
      DART_LOG_ERROR("%s ! failed: unitid out of range 0 <= %d < %d",       \
                     __FUNCTION__, _unitid.id, _team_data->size);           \
      return DART_ERR_INVAL;                                                \
    }                                                                       \
  } while (0)


/**
 * Temporary space allocation:
 *   - on the stack for allocations <=64B
 *   - on the heap otherwise
 * Mainly meant to be used in dart_waitall_* and dart_testall_local
 */
#define ALLOC_TMP(__size) ((__size)<=64) ? alloca((__size)) : malloc((__size))
/**
 * Temporary space release: calls free() for allocations >64B
 */
#define FREE_TMP(__size, __ptr)        \
  do {                                 \
    if ((__size) > 64)                 \
      free(__ptr);                     \
  } while (0)

int dart__mpi__datatype_sizes[DART_TYPE_COUNT];
static MPI_Datatype dart__mpi__max_chunk_datatype[DART_TYPE_COUNT];

dart_ret_t
dart__mpi__datatype_init()
{
  for (int i = DART_TYPE_UNDEFINED+1; i < DART_TYPE_COUNT; i++) {

    // query the size of the data type
    int ret = MPI_Type_size(
                dart__mpi__datatype(i),
                &dart__mpi__datatype_sizes[i]);
    if (ret != MPI_SUCCESS) {
      DART_LOG_ERROR("Failed to query size of DART data type %i", i);
      return DART_ERR_INVAL;
    }

    // create the chunk data type
    ret = MPI_Type_contiguous(MAX_CONTIG_ELEMENTS,
                              dart__mpi__datatype(i),
                              &dart__mpi__max_chunk_datatype[i]);
    if (ret != MPI_SUCCESS) {
      DART_LOG_ERROR("Failed to create chunk type of DART data type %i", i);
      return DART_ERR_INVAL;
    }
    ret = MPI_Type_commit(&dart__mpi__max_chunk_datatype[i]);
    if (ret != MPI_SUCCESS) {
      DART_LOG_ERROR("Failed to commit chunk type of DART data type %i", i);
      return DART_ERR_INVAL;
    }

  }
  return DART_OK;
}

dart_ret_t
dart__mpi__datatype_fini()
{
  for (int i = DART_TYPE_UNDEFINED+1; i < DART_TYPE_COUNT; i++) {
    int ret = MPI_Type_free(&dart__mpi__max_chunk_datatype[i]);
    if (ret != MPI_SUCCESS) {
      DART_LOG_ERROR("Failed to commit chunk type of DART data type %i", i);
      return DART_ERR_INVAL;
    }
  }
  return DART_OK;
}

#if !defined(DART_MPI_DISABLE_SHARED_WINDOWS)
static dart_ret_t get_shared_mem(
  dart_team_data_t * team_data,
  void             * dest,
  dart_gptr_t        gptr,
  size_t             nelem,
  dart_datatype_t    dtype)
{
  int16_t      seg_id            = gptr.segid;
  uint64_t     offset            = gptr.addr_or_offs.offset;
  DART_LOG_DEBUG("dart_get: shared windows enabled");
  dart_team_unit_t luid = team_data->sharedmem_tab[gptr.unitid];
  char * baseptr;
  /*
   * Use memcpy if the target is in the same node as the calling unit:
   */
  DART_LOG_DEBUG("dart_get: shared memory segment, seg_id:%d",
                 seg_id);
  if (seg_id) {
    if (dart_segment_get_baseptr(
          &team_data->segdata, seg_id, luid, &baseptr) != DART_OK) {
      DART_LOG_ERROR("dart_get ! "
                     "dart_adapt_transtable_get_baseptr failed");
      return DART_ERR_INVAL;
    }
  } else {
    baseptr = dart_sharedmem_local_baseptr_set[luid.id];
  }
  baseptr += offset;
  DART_LOG_DEBUG(
    "dart_get: memcpy %zu bytes", nelem * dart__mpi__datatype_sizeof(dtype));
  memcpy((char*)dest, baseptr, nelem * dart__mpi__datatype_sizeof(dtype));
  return DART_OK;
}
#endif // !defined(DART_MPI_DISABLE_SHARED_WINDOWS)

dart_ret_t dart_get(
  void            * dest,
  dart_gptr_t       gptr,
  size_t            nelem,
  dart_datatype_t   dtype)
{
  MPI_Win          win;
  MPI_Datatype     mpi_dtype    = dart__mpi__datatype(dtype);
  uint64_t         offset       = gptr.addr_or_offs.offset;
  int16_t          seg_id       = gptr.segid;
  dart_team_unit_t team_unit_id = DART_TEAM_UNIT_ID(gptr.unitid);

  dart_team_data_t *team_data = dart_adapt_teamlist_get(gptr.teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_get ! failed: Unknown team %i!", gptr.teamid);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(team_unit_id, team_data);

  DART_LOG_DEBUG("dart_get() uid:%d o:%"PRIu64" s:%d t:%d nelem:%zu",
                 team_unit_id.id, offset, seg_id, gptr.teamid, nelem);

#if !defined(DART_MPI_DISABLE_SHARED_WINDOWS)
  DART_LOG_DEBUG("dart_get: shared windows enabled");
  if (seg_id >= 0 && team_data->sharedmem_tab[gptr.unitid].id >= 0) {
    return get_shared_mem(team_data, dest, gptr, nelem, dtype);
  }
#else
  DART_LOG_DEBUG("dart_get: shared windows disabled");
#endif // !defined(DART_MPI_DISABLE_SHARED_WINDOWS)

  /*
   * MPI shared windows disabled or target and calling unit are on different
   * nodes, use MPI_Get:
   */
  if (seg_id) {
    MPI_Aint disp_s;
    if (dart_segment_get_disp(
          &team_data->segdata,
          seg_id,
          team_unit_id,
          &disp_s) != DART_OK) {
      return DART_ERR_INVAL;
    }

    if (team_data->unitid == team_unit_id.id) {
      // use direct memcpy if we are on the same unit
      memcpy(dest, ((void*)disp_s) + offset,
          nelem * dart__mpi__datatype_sizeof(dtype));
      DART_LOG_TRACE("dart_get: memcpy nelem:%zu "
                     "source (coll.): disp:%"PRId64" -> dest:%p",
                     nelem, offset, dest);
      return DART_OK;
    }

    offset += disp_s;
    win = team_data->window;
    DART_LOG_TRACE("dart_get:  nelem:%zu "
                   "source (coll.): win:%"PRIu64" unit:%d disp:%"PRId64" "
                   "-> dest:%p",
                   nelem, (unsigned long)win, team_unit_id.id, offset, dest);

  } else {

    if (team_data->unitid == team_unit_id.id) {
      // use direct memcpy if we are on the same unit
      memcpy(dest, dart_mempool_localalloc + offset,
          nelem * dart__mpi__datatype_sizeof(dtype));
      DART_LOG_TRACE("dart_get: memcpy nelem:%zu "
                     "source (local): disp:%"PRId64" -> dest:%p",
                     nelem, offset, dest);
      return DART_OK;
    }

    win      = dart_win_local_alloc;
    DART_LOG_TRACE("dart_get:  nelem:%zu "
                   "source (local): win:%"PRIu64" unit:%d disp:%"PRId64" "
                   "-> dest:%p",
                   nelem, (unsigned long)win, team_unit_id.id, offset, dest);
  }

  /*
   * MPI uses offset type int, do not copy more than INT_MAX elements:
   */
  // chunk up the get
  const size_t nchunks   = nelem / MAX_CONTIG_ELEMENTS;
  const size_t remainder = nelem % MAX_CONTIG_ELEMENTS;
        char * dest_ptr  = (char*) dest;

  if (nchunks > 0) {
    DART_LOG_TRACE("dart_get:  MPI_Get (dest %p, size %zu)",
                   dest_ptr, nchunks * MAX_CONTIG_ELEMENTS);
    if (MPI_Get(dest_ptr,
                nchunks,
                dart__mpi__max_chunk_datatype[dtype],
                team_unit_id.id,
                offset,
                nchunks,
                dart__mpi__max_chunk_datatype[dtype],
                win) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_get ! MPI_Get failed");
      return DART_ERR_INVAL;
    }
    offset   += nchunks * MAX_CONTIG_ELEMENTS;
    dest_ptr += nchunks * MAX_CONTIG_ELEMENTS;
  }

  if (remainder > 0) {
    DART_LOG_TRACE("dart_get:  MPI_Get (dest %p, size %zu)", dest_ptr, remainder);
    if (MPI_Get(dest_ptr,
                remainder,
                mpi_dtype,
                team_unit_id.id,
                offset,
                remainder,
                mpi_dtype,
                win) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_get ! MPI_Get failed");
      return DART_ERR_INVAL;
    }
  }

  DART_LOG_DEBUG("dart_get > finished");
  return DART_OK;
}

dart_ret_t dart_put(
  dart_gptr_t       gptr,
  const void      * src,
  size_t            nelem,
  dart_datatype_t   dtype)
{
  MPI_Win          win;
  MPI_Datatype     mpi_dtype    = dart__mpi__datatype(dtype);
  uint64_t         offset       = gptr.addr_or_offs.offset;
  int16_t          seg_id       = gptr.segid;
  dart_team_unit_t team_unit_id = DART_TEAM_UNIT_ID(gptr.unitid);

  dart_team_data_t *team_data = dart_adapt_teamlist_get(gptr.teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_put ! failed: Unknown team %i!", gptr.teamid);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(team_unit_id, team_data);

  if (seg_id) {

    MPI_Aint disp_s;
    if (dart_segment_get_disp(
          &team_data->segdata,
          seg_id,
          team_unit_id,
          &disp_s) != DART_OK) {
      return DART_ERR_INVAL;
    }

    /* copy data directly if we are on the same unit */
    if (team_unit_id.id == team_data->unitid) {
      memcpy(((void*)disp_s) + offset, src,
          nelem * dart__mpi__datatype_sizeof(dtype));
      DART_LOG_DEBUG("dart_put: memcpy nelem:%zu (from global allocation)"
                     "offset: %"PRIu64"", nelem, offset);
      return DART_OK;
    }

    win     = team_data->window;
    offset += disp_s;

  } else {

    /* copy data directly if we are on the same unit */
    if (team_unit_id.id == team_data->unitid) {
      memcpy(dart_mempool_localalloc + offset, src,
          nelem * dart__mpi__datatype_sizeof(dtype));
      DART_LOG_DEBUG("dart_put: memcpy nelem:%zu (from local allocation)"
                     "offset: %"PRIu64"", nelem, offset);
      return DART_OK;
    }

    win = dart_win_local_alloc;
  }


  // chunk up the put
  const size_t nchunks   = nelem / MAX_CONTIG_ELEMENTS;
  const size_t remainder = nelem % MAX_CONTIG_ELEMENTS;
  const char * src_ptr   = (const char*) src;

  if (nchunks > 0) {
    DART_LOG_TRACE("dart_put:  MPI_Put (src %p, size %zu)",
                   src_ptr, nchunks * MAX_CONTIG_ELEMENTS);
    if (MPI_Put(src_ptr,
                nchunks,
                dart__mpi__max_chunk_datatype[dtype],
                team_unit_id.id,
                offset,
                nchunks,
                dart__mpi__max_chunk_datatype[dtype],
                win) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_put ! MPI_Put failed");
      return DART_ERR_INVAL;
    }
    offset  += nchunks * MAX_CONTIG_ELEMENTS;
    src_ptr += nchunks * MAX_CONTIG_ELEMENTS;
  }

  if (remainder > 0) {
    DART_LOG_TRACE("dart_put:  MPI_Put (src %p, size %zu)", src_ptr, remainder);
    if (MPI_Put(src_ptr,
                remainder,
                mpi_dtype,
                team_unit_id.id,
                offset,
                remainder,
                mpi_dtype,
                win) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_put ! MPI_Put failed");
      return DART_ERR_INVAL;
    }
  }

  return DART_OK;
}

dart_ret_t dart_accumulate(
  dart_gptr_t      gptr,
  const void     * values,
  size_t           nelem,
  dart_datatype_t  dtype,
  dart_operation_t op)
{
  MPI_Win      win;
  MPI_Datatype mpi_dtype;
  MPI_Op       mpi_op;
  dart_team_unit_t  team_unit_id = DART_TEAM_UNIT_ID(gptr.unitid);
  uint64_t offset   = gptr.addr_or_offs.offset;
  int16_t  seg_id   = gptr.segid;
  mpi_dtype         = dart__mpi__datatype(dtype);
  mpi_op            = dart__mpi__op(op);

  dart_team_data_t *team_data = dart_adapt_teamlist_get(gptr.teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_accumulate ! failed: Unknown team %i!", gptr.teamid);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(team_unit_id, team_data);

  DART_LOG_DEBUG("dart_accumulate() nelem:%zu dtype:%d op:%d unit:%d",
                 nelem, dtype, op, team_unit_id.id);

  if (seg_id) {
    MPI_Aint disp_s;
    win = team_data->window;
    if (dart_segment_get_disp(
          &team_data->segdata,
          seg_id,
          team_unit_id,
          &disp_s) != DART_OK) {
      DART_LOG_ERROR("dart_accumulate ! "
                     "dart_adapt_transtable_get_disp failed");
      return DART_ERR_INVAL;
    }
    offset += disp_s;
    DART_LOG_TRACE("dart_accumulate:  nelem:%zu (from collective allocation) "
                   "target unit: %d offset: %"PRIu64"",
                   nelem, team_unit_id.id, offset);
  } else {
    win = dart_win_local_alloc;
    DART_LOG_TRACE("dart_accumulate:  nelem:%zu (from local allocation) "
                   "target unit: %d offset: %"PRIu64"",
                   nelem, team_unit_id.id, offset);
  }

  // chunk up the put
  const size_t nchunks   = nelem / MAX_CONTIG_ELEMENTS;
  const size_t remainder = nelem % MAX_CONTIG_ELEMENTS;
  const char * src_ptr   = (const char*) values;

  if (nchunks > 0) {
    DART_LOG_TRACE("dart_accumulate:  MPI_Accumulate (src %p, size %zu)",
                   src_ptr, nchunks * MAX_CONTIG_ELEMENTS);
    if (MPI_Accumulate(
          src_ptr,
          nchunks,
          dart__mpi__max_chunk_datatype[dtype],
          team_unit_id.id,
          offset,
          nchunks,
          dart__mpi__max_chunk_datatype[dtype],
          mpi_op,
          win) != MPI_SUCCESS) {
      DART_LOG_ERROR("MPI_Accumulate ! MPI_Put failed");
      return DART_ERR_INVAL;
    }
    offset  += nchunks * MAX_CONTIG_ELEMENTS;
    src_ptr += nchunks * MAX_CONTIG_ELEMENTS;
  }

  if (remainder > 0) {
    DART_LOG_TRACE("dart_accumulate:  MPI_Accumulate (src %p, size %zu)",
                   src_ptr, remainder);
    if (MPI_Accumulate(
          src_ptr,
          remainder,
          mpi_dtype,
          team_unit_id.id,
          offset,
          remainder,
          mpi_dtype,
          mpi_op,
          win) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_accumulate ! MPI_Accumulate failed");
      return DART_ERR_INVAL;
    }
  }

  DART_LOG_DEBUG("dart_accumulate > finished");
  return DART_OK;
}

dart_ret_t dart_fetch_and_op(
  dart_gptr_t      gptr,
  const void *     value,
  void *           result,
  dart_datatype_t  dtype,
  dart_operation_t op)
{
  MPI_Win      win;
  MPI_Datatype mpi_dtype;
  MPI_Op       mpi_op;
  dart_team_unit_t  team_unit_id = DART_TEAM_UNIT_ID(gptr.unitid);
  uint64_t offset   = gptr.addr_or_offs.offset;
  int16_t  seg_id   = gptr.segid;
  mpi_dtype         = dart__mpi__datatype(dtype);
  mpi_op            = dart__mpi__op(op);
  
  dart_team_data_t *team_data = dart_adapt_teamlist_get(gptr.teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_fetch_and_op ! failed: Unknown team %i!",
                    gptr.teamid);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(team_unit_id, team_data);

  DART_LOG_DEBUG("dart_fetch_and_op() dtype:%d op:%d unit:%d "
                 "offset:%"PRIu64" segid:%d",
                 dtype, op, team_unit_id.id,
                 gptr.addr_or_offs.offset, gptr.segid);
  if (seg_id) {

    MPI_Aint disp_s;
    if (dart_segment_get_disp(
          &team_data->segdata,
          seg_id,
          team_unit_id,
          &disp_s) != DART_OK) {
      DART_LOG_ERROR("dart_fetch_and_op ! "
                     "dart_adapt_transtable_get_disp failed");
      return DART_ERR_INVAL;
    }
    offset += disp_s;
    win = team_data->window;
    DART_LOG_TRACE("dart_fetch_and_op:  (from coll. allocation) "
                   "target unit: %d offset: %"PRIu64,
                   team_unit_id.id, offset);
  } else {
    win = dart_win_local_alloc;
    DART_LOG_TRACE("dart_fetch_and_op:  (from local allocation) "
                   "target unit: %d offset: %"PRIu64,
                   team_unit_id.id, offset);
  }
  MPI_Fetch_and_op(
    value,             // Origin address
    result,            // Result address
    mpi_dtype,         // Data type of each buffer entry
    team_unit_id.id,   // Rank of target
    offset,            // Displacement from start of window to beginning
                       // of target buffer
    mpi_op,            // Reduce operation
    win);
  DART_LOG_DEBUG("dart_fetch_and_op > finished");
  return DART_OK;
}

dart_ret_t dart_compare_and_swap(
  dart_gptr_t      gptr,
  const void     * value,
  const void     * compare,
  void           * result,
  dart_datatype_t  dtype)
{
  MPI_Win win;
  dart_team_unit_t  team_unit_id = DART_TEAM_UNIT_ID(gptr.unitid);
  uint64_t offset   = gptr.addr_or_offs.offset;
  int16_t  seg_id   = gptr.segid;
  MPI_Datatype mpi_dtype         = dart__mpi__datatype(dtype);

  dart_team_data_t *team_data = dart_adapt_teamlist_get(gptr.teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_compare_and_swap ! failed: Unknown team %i!",
                    gptr.teamid);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(team_unit_id, team_data);

  if (dtype > DART_TYPE_LONGLONG) {
    DART_LOG_ERROR("dart_compare_and_swap ! failed: "
                   "only valid on integral types");
    return DART_ERR_INVAL;
  }

  DART_LOG_TRACE("dart_compare_and_swap() dtype:%d unit:%d offset:%"PRIu64,
                 dtype, team_unit_id.id, gptr.addr_or_offs.offset);

  if (seg_id) {
    MPI_Aint disp_s;

    win = team_data->window;
    if (dart_segment_get_disp(
          &team_data->segdata,
          seg_id,
          team_unit_id,
          &disp_s) != DART_OK) {
      DART_LOG_ERROR("dart_accumulate ! "
                     "dart_adapt_transtable_get_disp failed");
      return DART_ERR_INVAL;
    }
    offset += disp_s;
    DART_LOG_TRACE("dart_compare_and_swap: target unit: %d offset: %"PRIu64"",
              team_unit_id.id, offset);
  } else {
    win = dart_win_local_alloc;
    DART_LOG_TRACE("dart_compare_and_swap: target unit: %d offset: %"PRIu64"",
              team_unit_id.id, offset);
  }
  MPI_Compare_and_swap(
        value,
        compare,
        result,
        mpi_dtype,
        team_unit_id.id,
        offset,
        win);
  DART_LOG_DEBUG("dart_compare_and_swap > finished");
  return DART_OK;
}

/* -- Non-blocking dart one-sided operations -- */

dart_ret_t dart_get_handle(
  void          * dest,
  dart_gptr_t     gptr,
  size_t          nelem,
  dart_datatype_t dtype,
  dart_handle_t * handleptr)
{
  MPI_Win      win;
  dart_team_unit_t  team_unit_id = DART_TEAM_UNIT_ID(gptr.unitid);
  uint64_t     offset = gptr.addr_or_offs.offset;
  int16_t      seg_id = gptr.segid;

  *handleptr = DART_HANDLE_NULL;

  dart_team_data_t *team_data = dart_adapt_teamlist_get(gptr.teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_get_handle ! failed: Unknown team %i!", gptr.teamid);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(team_unit_id, team_data);

  DART_LOG_DEBUG("dart_get_handle() uid:%d o:%"PRIu64" s:%d t:%d, nelem:%zu",
                 team_unit_id.id, offset, seg_id, gptr.teamid, nelem);
  DART_LOG_TRACE("dart_get_handle:  allocated handle:%p", (void *)(*handleptr));

#if !defined(DART_MPI_DISABLE_SHARED_WINDOWS)
  DART_LOG_DEBUG("dart_get_handle: shared windows enabled");

  if (seg_id >= 0 && team_data->sharedmem_tab[gptr.unitid].id >= 0) {
    dart_ret_t ret = get_shared_mem(team_data, dest, gptr, nelem, dtype);
    // return NULL request
    return ret;
  }
#else
  DART_LOG_DEBUG("dart_get_handle: shared windows disabled");
#endif /* !defined(DART_MPI_DISABLE_SHARED_WINDOWS) */
  /*
   * MPI shared windows disabled or target and calling unit are on different
   * nodes, use MPI_RGet:
   */
  if (seg_id) {

    /*
     * The memory accessed is allocated with collective allocation.
     */
    win = team_data->window;

    MPI_Aint disp_s;
    if (dart_segment_get_disp(
          &team_data->segdata,
          seg_id,
          team_unit_id,
          &disp_s) != DART_OK) {
      DART_LOG_ERROR(
        "dart_get_handle ! dart_adapt_transtable_get_disp failed");
      return DART_ERR_INVAL;
    }
    offset += disp_s;

    DART_LOG_DEBUG("dart_get_handle:  -- %zu elements (collective allocation) "
                   "from %d at offset %"PRIu64"",
                   nelem, team_unit_id.id, offset);
  } else {
    /*
     * The memory accessed is allocated with local allocation.
     */
    DART_LOG_DEBUG("dart_get_handle:  -- %zu elements (local allocation) "
                   "from %d at offset %"PRIu64"",
                   nelem, team_unit_id.id, offset);
    win     = dart_win_local_alloc;
  }

  // chunk up the get
  const size_t nchunks   = nelem / MAX_CONTIG_ELEMENTS;
  const size_t remainder = nelem % MAX_CONTIG_ELEMENTS;
  char * dest_ptr  = (char*) dest;

  dart_handle_t handle = calloc(1, sizeof(struct dart_handle_struct));
  handle->dest         = team_unit_id.id;
  handle->win          = win;
  handle->needs_flush  = false;

  if (nchunks > 0) {
    DART_LOG_TRACE("dart_get_blocking:  MPI_Rget (dest %p, size %zu)",
                   dest_ptr, nchunks * MAX_CONTIG_ELEMENTS);
    if (MPI_Rget(dest_ptr,
                  nchunks,
                  dart__mpi__max_chunk_datatype[dtype],
                  team_unit_id.id,
                  offset,
                  nchunks,
                  dart__mpi__max_chunk_datatype[dtype],
                  win,
                 &handle->reqs[handle->num_reqs++]) != MPI_SUCCESS) {
      free(handle);
      DART_LOG_ERROR("dart_get_blocking ! MPI_Rget failed");
      return DART_ERR_INVAL;
    }
    offset   += nchunks * MAX_CONTIG_ELEMENTS;
    dest_ptr += nchunks * MAX_CONTIG_ELEMENTS;
  }

  if (remainder > 0) {
    MPI_Datatype mpi_dtype = dart__mpi__datatype(dtype);
    DART_LOG_TRACE(
      "dart_get_blocking:  MPI_Rget (dest %p, size %zu)", dest_ptr, remainder);
    if (MPI_Rget(dest_ptr,
                 remainder,
                 mpi_dtype,
                 team_unit_id.id,
                 offset,
                 remainder,
                 mpi_dtype,
                 win,
                 &handle->reqs[handle->num_reqs++]) != MPI_SUCCESS) {
      free(handle);
      DART_LOG_ERROR("dart_get_blocking ! MPI_Rget failed");
      return DART_ERR_INVAL;
    }
  }

  *handleptr = handle;

  DART_LOG_TRACE("dart_get_handle > handle(%p) dest:%d win:%"PRIu64,
                 (void*)(handle), handle->dest,
                 (unsigned long)win);
  return DART_OK;
}

dart_ret_t dart_put_handle(
  dart_gptr_t       gptr,
  const void      * src,
  size_t            nelem,
  dart_datatype_t   dtype,
  dart_handle_t   * handleptr)
{
  dart_team_unit_t  team_unit_id = DART_TEAM_UNIT_ID(gptr.unitid);
  uint64_t     offset   = gptr.addr_or_offs.offset;
  int16_t      seg_id   = gptr.segid;
  MPI_Win      win;

  *handleptr = DART_HANDLE_NULL;
  
  dart_team_data_t *team_data = dart_adapt_teamlist_get(gptr.teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_put_handle ! failed: Unknown team %i!", gptr.teamid);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(team_unit_id, team_data);

  if (seg_id != 0) {
    win = team_data->window;

    MPI_Aint disp_s;
    if (dart_segment_get_disp(
          &team_data->segdata,
          seg_id,
          team_unit_id,
          &disp_s) != DART_OK) {
      return DART_ERR_INVAL;
    }
    offset += disp_s;

    DART_LOG_DEBUG("dart_put_handle: nelem:%zu dtype:%d"
                   "(from collective allocation) "
                   "target_unit:%d offset:%"PRIu64"",
                   nelem, dtype, team_unit_id.id, offset);
  } else {
    win = dart_win_local_alloc;
    DART_LOG_DEBUG("dart_put_handle: nlem:%zu dtype:%d"
                   "(from local allocation) "
                   "target_unit:%d offset:%"PRIu64"",
                   nelem, dtype, team_unit_id.id, offset);
  }

  DART_LOG_DEBUG("dart_put_handle: MPI_RPut");
  // chunk up the put

  dart_handle_t handle   = calloc(1, sizeof(struct dart_handle_struct));
  handle->dest           = team_unit_id.id;
  handle->win            = win;
  handle->needs_flush    = true;
  const size_t nchunks   = nelem / MAX_CONTIG_ELEMENTS;
  const size_t remainder = nelem % MAX_CONTIG_ELEMENTS;
  const char   * src_ptr = (const char*) src;

  if (nchunks > 0) {
    DART_LOG_TRACE("dart_put_handle:  MPI_Rput (src %p, size %zu)",
                   src_ptr, nchunks * MAX_CONTIG_ELEMENTS);
    if (MPI_Rput(src_ptr,
                nchunks,
                dart__mpi__max_chunk_datatype[dtype],
                team_unit_id.id,
                offset,
                nchunks,
                dart__mpi__max_chunk_datatype[dtype],
                win,
                &handle->reqs[handle->num_reqs++]) != MPI_SUCCESS) {
      free(handle);
      DART_LOG_ERROR("dart_put_handle ! MPI_Rput failed");
      return DART_ERR_INVAL;
    }
    src_ptr += nchunks * MAX_CONTIG_ELEMENTS;
    offset  += nchunks * MAX_CONTIG_ELEMENTS;
  }

  if (remainder > 0) {
    MPI_Datatype mpi_dtype = dart__mpi__datatype(dtype);
    DART_LOG_TRACE(
      "dart_put_handle:  MPI_Rput (src %p, size %zu)", src_ptr, remainder);
    if (MPI_Rput(src_ptr,
                remainder,
                mpi_dtype,
                team_unit_id.id,
                offset,
                remainder,
                mpi_dtype,
                win,
                &handle->reqs[handle->num_reqs++]) != MPI_SUCCESS) {
      free(handle);
      DART_LOG_ERROR("dart_get ! MPI_Put failed");
      return DART_ERR_INVAL;
    }
  }

  *handleptr = handle;

  return DART_OK;
}

/* -- Blocking dart one-sided operations -- */

/**
 * \todo Check if MPI_Get_accumulate (MPI_NO_OP) yields better performance
 */
dart_ret_t dart_put_blocking(
  dart_gptr_t     gptr,
  const void    * src,
  size_t          nelem,
  dart_datatype_t dtype)
{
  MPI_Win           win;
  dart_team_unit_t  team_unit_id = DART_TEAM_UNIT_ID(gptr.unitid);
  uint64_t          offset       = gptr.addr_or_offs.offset;
  int16_t           seg_id       = gptr.segid;

  dart_team_data_t *team_data = dart_adapt_teamlist_get(gptr.teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_put_blocking ! failed: Unknown team %i!", gptr.teamid);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(team_unit_id, team_data);

  DART_LOG_DEBUG("dart_put_blocking() uid:%d o:%"PRIu64" s:%d t:%d, nelem:%zu",
                 team_unit_id.id, offset, seg_id, gptr.teamid, nelem);

#if !defined(DART_MPI_DISABLE_SHARED_WINDOWS)
  DART_LOG_DEBUG("dart_put_blocking: shared windows enabled");
  if (seg_id >= 0) {
    /*
     * Use memcpy if the target is in the same node as the calling unit:
     * The value of i will be the target's relative ID in teamid.
     */
    dart_team_unit_t luid = team_data->sharedmem_tab[gptr.unitid];
    if (luid.id >= 0) {
      char * baseptr;
      DART_LOG_DEBUG("dart_put_blocking: shared memory segment, seg_id:%d",
                     seg_id);
      if (seg_id) {
        if (dart_segment_get_baseptr(
                &team_data->segdata,
                seg_id,
                luid,
                &baseptr) != DART_OK) {
          DART_LOG_ERROR("dart_put_blocking ! "
                         "dart_adapt_transtable_get_baseptr failed");
          return DART_ERR_INVAL;
        }
      } else {
        baseptr = dart_sharedmem_local_baseptr_set[luid.id];
      }
      baseptr += offset;
      DART_LOG_DEBUG("dart_put_blocking: memcpy %zu bytes",
                        nelem * dart__mpi__datatype_sizeof(dtype));
      memcpy(baseptr, src, nelem * dart__mpi__datatype_sizeof(dtype));
      return DART_OK;
    }
  }
#else
  DART_LOG_DEBUG("dart_put_blocking: shared windows disabled");
#endif /* !defined(DART_MPI_DISABLE_SHARED_WINDOWS) */
  /*
   * MPI shared windows disabled or target and calling unit are on different
   * nodes, use MPI_Rput:
   */
  if (seg_id) {
    MPI_Aint disp_s;
    if (dart_segment_get_disp(
          &team_data->segdata,
          seg_id,
          team_unit_id,
          &disp_s) != DART_OK) {
      DART_LOG_ERROR("dart_put_blocking ! "
                     "dart_adapt_transtable_get_disp failed");
      return DART_ERR_INVAL;
    }

    /* copy data directly if we are on the same unit */
    if (team_unit_id.id == team_data->unitid) {
      memcpy(((void*)disp_s) + offset, src,
          nelem*dart__mpi__datatype_sizeof(dtype));
      DART_LOG_DEBUG("dart_put_blocking: memcpy nelem:%zu "
                     "target unit: %d offset: %"PRIu64"",
                     nelem, team_unit_id.id, offset);
      return DART_OK;
    }

    win = team_data->window;
    offset += disp_s;
    DART_LOG_DEBUG("dart_put_blocking:  nelem:%zu "
                   "target (coll.): win:%p unit:%d offset:%lu "
                   "<- source: %p",
                   nelem, (void*)win, team_unit_id.id,
                   offset, src);

  } else {

    /* copy data directly if we are on the same unit */
    if (team_unit_id.id == team_data->unitid) {
      memcpy(dart_mempool_localalloc + offset, src,
          nelem * dart__mpi__datatype_sizeof(dtype));
      DART_LOG_DEBUG("dart_put_blocking: memcpy nelem:%zu offset: %"PRIu64"",
                     nelem, offset);
      return DART_OK;
    }

    win      = dart_win_local_alloc;
    DART_LOG_DEBUG("dart_put_blocking:  nelem:%zu "
                   "target (local): win:%p unit:%d offset:%lu "
                   "<- source: %p",
                   nelem, (void*)win, team_unit_id.id,
                   offset, src);
  }

  /*
   * Using MPI_Put as MPI_Win_flush is required to ensure remote completion.
   */
  // chunk up the put
  const size_t nchunks   = nelem / MAX_CONTIG_ELEMENTS;
  const size_t remainder = nelem % MAX_CONTIG_ELEMENTS;
  const char   * src_ptr = (const char*) src;

  if (nchunks > 0) {
    DART_LOG_TRACE("dart_put_blocking:  MPI_Put (src %p, size %zu)",
                   src_ptr, nchunks * MAX_CONTIG_ELEMENTS);
    if (MPI_Put(src_ptr,
                nchunks,
                dart__mpi__max_chunk_datatype[dtype],
                team_unit_id.id,
                offset,
                nchunks,
                dart__mpi__max_chunk_datatype[dtype],
                win) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_put_blocking ! MPI_Put failed");
      return DART_ERR_INVAL;
    }
    src_ptr += nchunks * MAX_CONTIG_ELEMENTS;
    offset  += nchunks * MAX_CONTIG_ELEMENTS;
  }

  if (remainder > 0) {
    MPI_Datatype  mpi_dtype = dart__mpi__datatype(dtype);
    DART_LOG_TRACE(
      "dart_put_blocking:  MPI_Put (src %p, size %zu)", src_ptr, remainder);
    if (MPI_Put(src_ptr,
                remainder,
                mpi_dtype,
                team_unit_id.id,
                offset,
                remainder,
                mpi_dtype,
                win) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_put_blocking ! MPI_Put failed");
      return DART_ERR_INVAL;
    }
  }


  DART_LOG_DEBUG("dart_put_blocking: MPI_Win_flush");
  if (MPI_Win_flush(team_unit_id.id, win) != MPI_SUCCESS) {
    DART_LOG_ERROR("dart_put_blocking ! MPI_Win_flush failed");
    return DART_ERR_INVAL;
  }

  DART_LOG_DEBUG("dart_put_blocking > finished");
  return DART_OK;
}

/**
 * \todo Check if MPI_Accumulate (REPLACE) yields better performance
 */
dart_ret_t dart_get_blocking(
  void          * dest,
  dart_gptr_t     gptr,
  size_t          nelem,
  dart_datatype_t dtype)
{
  MPI_Win           win;
  dart_team_unit_t  team_unit_id = DART_TEAM_UNIT_ID(gptr.unitid);
  uint64_t          offset       = gptr.addr_or_offs.offset;
  int16_t           seg_id       = gptr.segid;

  dart_team_data_t *team_data = dart_adapt_teamlist_get(gptr.teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_get_blocking ! failed: Unknown team %i!", gptr.teamid);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(team_unit_id, team_data);

  DART_LOG_DEBUG("dart_get_blocking() uid:%d "
                 "o:%"PRIu64" s:%d t:%u, nelem:%zu",
                 team_unit_id.id,
                 offset, seg_id, gptr.teamid, nelem);

#if !defined(DART_MPI_DISABLE_SHARED_WINDOWS)
  DART_LOG_DEBUG("dart_get_blocking: shared windows enabled");
  if (seg_id >= 0 && team_data->sharedmem_tab[gptr.unitid].id >= 0) {
    return get_shared_mem(team_data, dest, gptr, nelem, dtype);
  }
#else
  DART_LOG_DEBUG("dart_get_blocking: shared windows disabled");
#endif /* !defined(DART_MPI_DISABLE_SHARED_WINDOWS) */
  /*
   * MPI shared windows disabled or target and calling unit are on different
   * nodes, use MPI_Rget:
   */
  if (seg_id) {
    MPI_Aint disp_s;
    if (dart_segment_get_disp(
          &team_data->segdata,
          seg_id,
          team_unit_id,
          &disp_s) != DART_OK) {
      DART_LOG_ERROR("dart_get_blocking ! "
                     "dart_adapt_transtable_get_disp failed");
      return DART_ERR_INVAL;
    }

    if (team_data->unitid == team_unit_id.id) {
      // use direct memcpy if we are on the same unit
      memcpy(dest, ((void*)disp_s) + offset,
          nelem * dart__mpi__datatype_sizeof(dtype));
      DART_LOG_DEBUG("dart_get_blocking: memcpy nelem:%zu "
                     "source (coll.): offset:%lu -> dest: %p",
                     nelem, offset, dest);
      return DART_OK;
    }

    win     = team_data->window;
    offset += disp_s;
    DART_LOG_DEBUG("dart_get_blocking:  nelem:%zu "
                   "source (coll.): win:%p unit:%d offset:%lu "
                   "-> dest: %p",
                   nelem, (void*)win, team_unit_id.id,
                   offset, dest);

  } else {

    if (team_data->unitid == team_unit_id.id) {
      /* use direct memcpy if we are on the same unit */
      memcpy(dest, dart_mempool_localalloc + offset,
          nelem * dart__mpi__datatype_sizeof(dtype));
      DART_LOG_DEBUG("dart_get_blocking: memcpy nelem:%zu "
                     "source (coll.): offset:%lu -> dest: %p",
                     nelem, offset, dest);
      return DART_OK;
    }

    win = dart_win_local_alloc;
    DART_LOG_DEBUG("dart_get_blocking:  nelem:%zu "
                   "source (local): win:%p unit:%d offset:%lu "
                   "-> dest: %p",
                   nelem, (void*)win, team_unit_id.id,
                   offset, dest);
  }

  /*
   * Using MPI_Get as MPI_Win_flush is required to ensure remote completion.
   */
  // chunk up the get
  const size_t nchunks   = nelem / MAX_CONTIG_ELEMENTS;
  const size_t remainder = nelem % MAX_CONTIG_ELEMENTS;
  char * dest_ptr  = (char*) dest;
  MPI_Request reqs[2];
  int nreqs = 0;

  if (nchunks > 0) {
    DART_LOG_TRACE("dart_get_blocking:  MPI_Rget (dest %p, size %zu)",
                   dest_ptr, nchunks * MAX_CONTIG_ELEMENTS);
    if (MPI_Rget(dest_ptr,
                 nchunks,
                 dart__mpi__max_chunk_datatype[dtype],
                 team_unit_id.id,
                 offset,
                 nchunks,
                 dart__mpi__max_chunk_datatype[dtype],
                 win,
                 &reqs[nreqs++]) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_get ! MPI_Get failed");
      return DART_ERR_INVAL;
    }
    offset   += nchunks * MAX_CONTIG_ELEMENTS;
    dest_ptr += nchunks * MAX_CONTIG_ELEMENTS;
  }

  if (remainder > 0) {
    MPI_Datatype mpi_dtype = dart__mpi__datatype(dtype);
    DART_LOG_TRACE(
      "dart_get_blocking:  MPI_Rget (dest %p, size %zu)", dest_ptr, remainder);
    if (MPI_Rget(dest_ptr,
                remainder,
                mpi_dtype,
                team_unit_id.id,
                offset,
                remainder,
                mpi_dtype,
                win,
                &reqs[nreqs++]) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_get ! MPI_Get failed");
      return DART_ERR_INVAL;
    }
  }

  MPI_Waitall(nreqs, reqs, MPI_STATUSES_IGNORE);

  DART_LOG_DEBUG("dart_get_blocking > finished");
  return DART_OK;
}

/* -- Dart RMA Synchronization Operations -- */

dart_ret_t dart_flush(
  dart_gptr_t gptr)
{
  MPI_Win          win;
  MPI_Comm         comm         = DART_COMM_WORLD;
  dart_team_unit_t team_unit_id = DART_TEAM_UNIT_ID(gptr.unitid);
  int16_t          seg_id       = gptr.segid;
  DART_LOG_DEBUG("dart_flush() gptr: "
                 "unitid:%d offset:%"PRIu64" segid:%d teamid:%d",
                 gptr.unitid, gptr.addr_or_offs.offset,
                 gptr.segid,  gptr.teamid);

  dart_team_data_t *team_data = dart_adapt_teamlist_get(gptr.teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_flush ! failed: Unknown team %i!", gptr.teamid);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(team_unit_id, team_data);

  if (seg_id) {
    win = team_data->window;
    comm = team_data->comm;
  } else {
    win = dart_win_local_alloc;
  }

  DART_LOG_TRACE("dart_flush: MPI_Win_flush");
  if (MPI_Win_flush(team_unit_id.id, win) != MPI_SUCCESS) {
    DART_LOG_ERROR("dart_flush ! MPI_Win_flush failed!");
    return DART_ERR_OTHER;
  }
  DART_LOG_TRACE("dart_flush: MPI_Win_sync");
  if (MPI_Win_sync(win) != MPI_SUCCESS) {
    DART_LOG_ERROR("dart_flush ! MPI_Win_sync failed!");
    return DART_ERR_OTHER;
  }

  // trigger progress
  int flag;
  MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm, &flag, MPI_STATUS_IGNORE);

  DART_LOG_DEBUG("dart_flush > finished");
  return DART_OK;
}

dart_ret_t dart_flush_all(
  dart_gptr_t gptr)
{
  MPI_Win  win;
  MPI_Comm comm   = DART_COMM_WORLD;
  int16_t  seg_id = gptr.segid;
  DART_LOG_DEBUG("dart_flush_all() gptr: "
                 "unitid:%d offset:%"PRIu64" segid:%d teamid:%d",
                 gptr.unitid, gptr.addr_or_offs.offset,
                 gptr.segid,  gptr.teamid);

  if (seg_id) {
    dart_team_data_t *team_data = dart_adapt_teamlist_get(gptr.teamid);
    if (team_data == NULL) {
      DART_LOG_ERROR("dart_flush_all ! failed: Unknown team %i!", gptr.teamid);
      return DART_ERR_INVAL;
    }

    win  = team_data->window;
    comm = team_data->comm;
  } else {
    win = dart_win_local_alloc;
  }
  DART_LOG_TRACE("dart_flush_all: MPI_Win_flush_all");
  if (MPI_Win_flush_all(win) != MPI_SUCCESS) {
    DART_LOG_ERROR("dart_flush_all ! MPI_Win_flush_all failed!");
    return DART_ERR_OTHER;
  }
  DART_LOG_TRACE("dart_flush_all: MPI_Win_sync");
  if (MPI_Win_sync(win) != MPI_SUCCESS) {
    DART_LOG_ERROR("dart_flush_all ! MPI_Win_sync failed!");
    return DART_ERR_OTHER;
  }

  // trigger progress
  int flag;
  MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm, &flag, MPI_STATUS_IGNORE);

  DART_LOG_DEBUG("dart_flush_all > finished");
  return DART_OK;
}

dart_ret_t dart_flush_local(
  dart_gptr_t gptr)
{
  MPI_Win  win;
  MPI_Comm comm   = DART_COMM_WORLD;
  int16_t  seg_id = gptr.segid;
  dart_team_unit_t team_unit_id = DART_TEAM_UNIT_ID(gptr.unitid);

  DART_LOG_DEBUG("dart_flush_local() gptr: "
                 "unitid:%d offset:%"PRIu64" segid:%d teamid:%d",
                 gptr.unitid, gptr.addr_or_offs.offset,
                 gptr.segid,  gptr.teamid);

  dart_team_data_t *team_data = dart_adapt_teamlist_get(gptr.teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_flush_local ! failed: Unknown team %i!", gptr.segid);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(team_unit_id, team_data);

  if (seg_id) {
    win = team_data->window;
    comm = team_data->comm;
    DART_LOG_DEBUG("dart_flush_local() win:%"PRIu64" seg:%d unit:%d",
                   (unsigned long)win, seg_id, team_unit_id.id);
  } else {
    win = dart_win_local_alloc;
    DART_LOG_DEBUG("dart_flush_local() lwin:%"PRIu64" seg:%d unit:%d",
                   (unsigned long)win, seg_id, team_unit_id.id);
  }
  DART_LOG_TRACE("dart_flush_local: MPI_Win_flush_local");
  if (MPI_Win_flush_local(team_unit_id.id, win) != MPI_SUCCESS) {
    DART_LOG_ERROR("dart_flush_all ! MPI_Win_flush_local failed!");
    return DART_ERR_OTHER;
  }

  // trigger progress
  int flag;
  MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm, &flag, MPI_STATUS_IGNORE);

  DART_LOG_DEBUG("dart_flush_local > finished");
  return DART_OK;
}

dart_ret_t dart_flush_local_all(
  dart_gptr_t gptr)
{
  MPI_Win  win;
  MPI_Comm comm   = DART_COMM_WORLD;
  int16_t  seg_id = gptr.segid;
  DART_LOG_DEBUG("dart_flush_local_all() gptr: "
                 "unitid:%d offset:%"PRIu64" segid:%d teamid:%d",
                 gptr.unitid, gptr.addr_or_offs.offset,
                 gptr.segid,  gptr.teamid);

  if (seg_id) {
    dart_team_data_t *team_data = dart_adapt_teamlist_get(gptr.teamid);
    if (team_data == NULL) {
      DART_LOG_ERROR("dart_flush_local_all ! failed: Unknown team %i!",
                          gptr.teamid);
      return DART_ERR_INVAL;
    }
    win  = team_data->window;
    comm = team_data->comm;
  } else {
    win = dart_win_local_alloc;
  }
  if (MPI_Win_flush_local_all(win) != MPI_SUCCESS) {
    DART_LOG_ERROR("dart_flush_all ! MPI_Win_flush_local_all failed!");
    return DART_ERR_OTHER;
  }

  // trigger progress
  int flag;
  MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, comm, &flag, MPI_STATUS_IGNORE);

  DART_LOG_DEBUG("dart_flush_local_all > finished");
  return DART_OK;
}

dart_ret_t dart_wait_local(
  dart_handle_t * handleptr)
{
  DART_LOG_DEBUG("dart_wait_local() handle:%p", (void*)(handleptr));
  if (handleptr != NULL && *handleptr != DART_HANDLE_NULL) {
    dart_handle_t handle = *handleptr;
    DART_LOG_TRACE("dart_wait_local:     handle->dest: %d",
                   handle->dest);
    DART_LOG_TRACE("dart_wait_local:     handle->win:  %p",
                   (void*)(unsigned long)(handle->win));
    if (handle->num_reqs > 0) {
      int ret = MPI_Waitall(handle->num_reqs, handle->reqs, MPI_STATUS_IGNORE);
      if (ret != MPI_SUCCESS) {
        DART_LOG_DEBUG("dart_wait_local ! MPI_Wait failed");
        return DART_ERR_INVAL;
      }
    } else {
      DART_LOG_TRACE("dart_wait_local:     handle->num_reqs == 0");
    }
    free(handle);
    *handleptr = DART_HANDLE_NULL;
  }
  DART_LOG_DEBUG("dart_wait_local > finished");
  return DART_OK;
}

dart_ret_t dart_wait(
  dart_handle_t * handleptr)
{
  DART_LOG_DEBUG("dart_wait() handle:%p", (void*)(handleptr));
  if (handleptr != NULL && *handleptr != DART_HANDLE_NULL) {
    dart_handle_t handle = *handleptr;
    DART_LOG_TRACE("dart_wait_local:     handle->dest: %d",
                   handle->dest);
    DART_LOG_TRACE("dart_wait_local:     handle->win:  %"PRIu64"",
                   (unsigned long)handle->win);
    if (handle->num_reqs > 0) {
      DART_LOG_DEBUG("dart_wait:     -- MPI_Wait");
      int ret = MPI_Waitall(handle->num_reqs, handle->reqs, MPI_STATUS_IGNORE);
      if (ret != MPI_SUCCESS) {
        DART_LOG_ERROR("dart_wait ! MPI_Wait failed");
        return DART_ERR_INVAL;
      }
      if (handle->needs_flush) {
        DART_LOG_DEBUG("dart_wait:     -- MPI_Win_flush");
        ret = MPI_Win_flush(handle->dest, handle->win);
        if (ret != MPI_SUCCESS) {
          DART_LOG_ERROR("dart_wait ! MPI_Win_flush failed");
          return DART_ERR_INVAL;
        }
      }
    } else {
      DART_LOG_TRACE("dart_wait:     handle->num_reqs == 0");
    }
    /* Free handle resource */
    free(handle);
    *handleptr = DART_HANDLE_NULL;
  }
  DART_LOG_DEBUG("dart_wait > finished");
  return DART_OK;
}

dart_ret_t dart_waitall_local(
  dart_handle_t handles[],
  size_t        num_handles)
{
  dart_ret_t ret = DART_OK;

  DART_LOG_DEBUG("dart_waitall_local()");
  if (num_handles == 0) {
    DART_LOG_DEBUG("dart_waitall_local > number of handles = 0");
    return DART_OK;
  }
  if (num_handles > INT_MAX) {
    DART_LOG_ERROR("dart_waitall_local ! number of handles > INT_MAX");
    return DART_ERR_INVAL;
  }
  if (handles != NULL) {
    size_t r_n = 0;
    MPI_Request *mpi_req = ALLOC_TMP(2 * num_handles * sizeof(MPI_Request));
    for (size_t i = 0; i < num_handles; ++i) {
      if (handles[i] != DART_HANDLE_NULL) {
        for (uint8_t j = 0; j < handles[i]->num_reqs; ++j) {
          if (handles[i]->reqs[j] != MPI_REQUEST_NULL){
            DART_LOG_TRACE("dart_waitall_local: -- handle[%"PRIu64"]: %p)",
                          i, (void*)handles[i]->reqs[j]);
            DART_LOG_TRACE("dart_waitall_local:    handle[%"PRIu64"]->dest: %d",
                          i, handles[i]->dest);
            mpi_req[r_n] = handles[i]->reqs[j];
            r_n++;
          }
        }
      }
    }

    /*
     * Wait for local completion of MPI requests:
     */
    DART_LOG_DEBUG("dart_waitall_local: "
                   "MPI_Waitall, %"PRIu64" requests from %"PRIu64" handles",
                   r_n, num_handles);
    if (r_n > 0) {
      if (MPI_Waitall(r_n, mpi_req, MPI_STATUSES_IGNORE) == MPI_SUCCESS) {
        DART_LOG_DEBUG("dart_waitall_local: MPI_Waitall completed");
      } else {
        DART_LOG_ERROR("dart_waitall_local: MPI_Waitall failed");
        FREE_TMP(2 * num_handles * sizeof(MPI_Request), mpi_req);
        return DART_ERR_INVAL;
      }
    } else {
      DART_LOG_DEBUG("dart_waitall_local > number of requests = 0");
      FREE_TMP(2 * num_handles * sizeof(MPI_Request), mpi_req);
      return DART_OK;
    }

    /*
     * free DART handles
     */
    DART_LOG_TRACE("dart_waitall_local: releasing DART handles");
    for (size_t i = 0; i < num_handles; i++) {
      if (handles[i] != DART_HANDLE_NULL) {
        DART_LOG_TRACE("dart_waitall_local: free handle[%zu] %p",
                       i, (void*)(handles[i]));
        // free the handle
        free(handles[i]);
        handles[i] = DART_HANDLE_NULL;
      }
    }
    FREE_TMP(2 * num_handles * sizeof(MPI_Request), mpi_req);
  }
  DART_LOG_DEBUG("dart_waitall_local > %d", ret);
  return ret;
}

dart_ret_t dart_waitall(
  dart_handle_t handles[],
  size_t        n)
{
  DART_LOG_DEBUG("dart_waitall()");
  if (n == 0) {
    DART_LOG_DEBUG("dart_waitall > number of handles = 0");
    return DART_OK;
  }
  if (n > INT_MAX) {
    DART_LOG_ERROR("dart_waitall ! number of handles > INT_MAX");
    return DART_ERR_INVAL;
  }

  DART_LOG_DEBUG("dart_waitall: number of handles: %zu", n);

  if (handles != NULL) {
    MPI_Request *mpi_req = ALLOC_TMP(2 * n * sizeof(MPI_Request));
    /*
     * copy requests from DART handles to MPI request array:
     */
    DART_LOG_TRACE("dart_waitall: copying DART handles to MPI request array");
    size_t r_n = 0;
    for (size_t i = 0; i < n; i++) {
      if (handles[i] != DART_HANDLE_NULL) {
        for (uint8_t j = 0; j < handles[i]->num_reqs; ++j) {
          if (handles[i]->reqs[j] != MPI_REQUEST_NULL){
            DART_LOG_DEBUG("dart_waitall: -- handle[%zu](%p): "
                          "dest:%d win:%"PRIu64,
                          i, (void*)handles[i]->reqs[0],
                          handles[i]->dest,
                          (unsigned long)handles[i]->win);
            mpi_req[r_n] = handles[i]->reqs[j];
            r_n++;
          }
        }
      }
    }

    /*
     * wait for communication of MPI requests:
     */
    DART_LOG_DEBUG("dart_waitall: MPI_Waitall, %zu requests from %zu handles",
                   r_n, n);
    /* From the MPI 3.1 standard:
     *
     * The i-th entry in array_of_statuses is set to the return
     * status of the i-th operation. Active persistent requests
     * are marked inactive.
     * Requests of any other type are deallocated and the
     * corresponding handles in the array are set to
     * MPI_REQUEST_NULL.
     * The list may contain null or inactive handles.
     * The call sets to empty the status of each such entry.
     */
    if (r_n > 0) {
      if (MPI_Waitall(r_n, mpi_req, MPI_STATUSES_IGNORE) == MPI_SUCCESS) {
        DART_LOG_DEBUG("dart_waitall: MPI_Waitall completed");
      } else {
        DART_LOG_ERROR("dart_waitall: MPI_Waitall failed");
        FREE_TMP(2 * n * sizeof(MPI_Request), mpi_req);
        return DART_ERR_INVAL;
      }
    } else {
      DART_LOG_DEBUG("dart_waitall > number of requests = 0");
      FREE_TMP(2 * n * sizeof(MPI_Request), mpi_req);
      return DART_OK;
    }

    /*
     * wait for completion of MPI requests at origins and targets:
     */
    DART_LOG_DEBUG("dart_waitall: waiting for remote completion");
    for (size_t i = 0; i < n; i++) {
      if (handles[i] != DART_HANDLE_NULL && handles[i]->needs_flush) {
        DART_LOG_DEBUG("dart_waitall: -- MPI_Win_flush(handle[%zu]: %p, dest: %d))",
                       i, (void*)handles[i], handles[i]->dest);
        /*
         * MPI_Win_flush to wait for remote completion if required:
         */
        if (MPI_Win_flush(handles[i]->dest, handles[i]->win) != MPI_SUCCESS) {
          DART_LOG_ERROR("dart_waitall: MPI_Win_flush failed");
          FREE_TMP(2 * n * sizeof(MPI_Request), mpi_req);
          return DART_ERR_INVAL;
        }
      }
    }

    /*
     * free memory:
     */
    DART_LOG_DEBUG("dart_waitall: free handles");
    for (size_t i = 0; i < n; i++) {
      if (handles[i] != DART_HANDLE_NULL) {
        /* Free handle resource */
        DART_LOG_TRACE("dart_waitall: -- free handle[%zu]: %p",
                       i, (void*)(handles[i]));
        // free the handle
        free(handles[i]);
        handles[i] = DART_HANDLE_NULL;
      }
    }
    DART_LOG_TRACE("dart_waitall: free MPI_Request temporaries");
    FREE_TMP(2 * n * sizeof(MPI_Request), mpi_req);
  }
  DART_LOG_DEBUG("dart_waitall > finished");
  return DART_OK;
}

dart_ret_t dart_test_local(
  dart_handle_t * handleptr,
  int32_t       * is_finished)
{
  DART_LOG_DEBUG("dart_test_local()");
  if (handleptr == NULL ||
      *handleptr == DART_HANDLE_NULL ||
      (*handleptr)->num_reqs == 0) {
    *is_finished = 1;
    return DART_OK;
  }

  dart_handle_t handle = *handleptr;
  if (MPI_Testall(handle->num_reqs, handle->reqs,
               is_finished, MPI_STATUS_IGNORE) != MPI_SUCCESS) {
    DART_LOG_ERROR("dart_test_local: MPI_Test failed!");
    return DART_ERR_OTHER;
  }

  if (is_finished) {
    // deallocate handle
    free(handle);
    *handleptr = DART_HANDLE_NULL;
  }
  DART_LOG_DEBUG("dart_test_local > finished");
  return DART_OK;
}

dart_ret_t dart_testall_local(
  dart_handle_t   handles[],
  size_t          n,
  int32_t       * is_finished)
{
  DART_LOG_DEBUG("dart_testall_local()");
  if (handles == NULL || n == 0) {
    DART_LOG_DEBUG("dart_testall_local: empty handles");
    return DART_OK;
  }

  MPI_Request *mpi_req = ALLOC_TMP(2 * n * sizeof (MPI_Request));
  size_t r_n = 0;
  for (size_t i = 0; i < n; ++i) {
    if (handles[i] != DART_HANDLE_NULL) {
      for (uint8_t j = 0; j < handles[i]->num_reqs; ++j) {
        if (handles[i]->reqs[j] != MPI_REQUEST_NULL){
          mpi_req[r_n] = handles[i]->reqs[j];
          ++r_n;
        }
      }
    }
  }

  if (r_n) {
    if (MPI_Testall(r_n, mpi_req, is_finished,
                    MPI_STATUSES_IGNORE) != MPI_SUCCESS){
      FREE_TMP(2 * n * sizeof(MPI_Request), mpi_req);
      DART_LOG_ERROR("dart_testall_local: MPI_Testall failed!");
      return DART_ERR_OTHER;
    }

    if (*is_finished) {
      for (size_t i = 0; i < n; i++) {
        if (handles[i] != DART_HANDLE_NULL) {
          // free the handle
          free(handles[i]);
          handles[i] = DART_HANDLE_NULL;
        }
      }
    }
  }
  FREE_TMP(2 * n * sizeof(MPI_Request), mpi_req);
  DART_LOG_DEBUG("dart_testall_local > finished");
  return DART_OK;
}

/* -- Dart collective operations -- */

static int _dart_barrier_count = 0;

dart_ret_t dart_barrier(
  dart_team_t teamid)
{
  MPI_Comm comm;

  DART_LOG_DEBUG("dart_barrier() barrier count: %d", _dart_barrier_count);

  if (teamid == DART_UNDEFINED_TEAM_ID) {
    DART_LOG_ERROR("dart_barrier ! failed: team may not be DART_UNDEFINED_TEAM_ID");
    return DART_ERR_INVAL;
  }

  _dart_barrier_count++;

  dart_team_data_t *team_data = dart_adapt_teamlist_get(teamid);
  if (team_data == NULL) {
    return DART_ERR_INVAL;
  }
  /* Fetch proper communicator from teams. */
  comm = team_data->comm;
  if (MPI_Barrier(comm) == MPI_SUCCESS) {
    DART_LOG_DEBUG("dart_barrier > finished");
    return DART_OK;
  }
  DART_LOG_DEBUG("dart_barrier ! MPI_Barrier failed");
  return DART_ERR_INVAL;
}

dart_ret_t dart_bcast(
  void              * buf,
  size_t              nelem,
  dart_datatype_t     dtype,
  dart_team_unit_t    root,
  dart_team_t         teamid)
{
  MPI_Comm comm;
  MPI_Datatype mpi_dtype = dart__mpi__datatype(dtype);

  DART_LOG_TRACE("dart_bcast() root:%d team:%d nelem:%"PRIu64"",
                 root.id, teamid, nelem);

  dart_team_data_t *team_data = dart_adapt_teamlist_get(teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_bcast ! failed: unknown team %d", teamid);
    return DART_ERR_INVAL;
  }
  comm = team_data->comm;

  CHECK_UNITID_RANGE(root, team_data);

  // chunk up the bcast if necessary
  const size_t nchunks   = nelem / MAX_CONTIG_ELEMENTS;
  const size_t remainder = nelem % MAX_CONTIG_ELEMENTS;
        char * src_ptr   = (char*) buf;

  if (nchunks > 0) {
    if (MPI_Bcast(src_ptr, nchunks,
                  dart__mpi__max_chunk_datatype[dtype],
                  root.id, comm) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_bcast ! root:%d -> team:%d "
                     "MPI_Bcast failed", root.id, teamid);
      return DART_ERR_INVAL;
    }
    src_ptr += nchunks * MAX_CONTIG_ELEMENTS;
  }

  if (remainder > 0) {
    if (MPI_Bcast(src_ptr, remainder, mpi_dtype, root.id, comm) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_bcast ! root:%d -> team:%d "
      "MPI_Bcast failed", root.id, teamid);
      return DART_ERR_INVAL;
    }
  }

  DART_LOG_TRACE("dart_bcast > root:%d team:%d nelem:%zu finished",
                 root.id, teamid, nelem);
  return DART_OK;
}

dart_ret_t dart_scatter(
  const void        * sendbuf,
  void              * recvbuf,
  size_t              nelem,
  dart_datatype_t     dtype,
  dart_team_unit_t    root,
  dart_team_t         teamid)
{
  dart_team_data_t *team_data = dart_adapt_teamlist_get(teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_scatter ! failed: unknown team %d", teamid);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(root, team_data);

  // chunk up the scatter if necessary
  const size_t nchunks   = nelem / MAX_CONTIG_ELEMENTS;
  const size_t remainder = nelem % MAX_CONTIG_ELEMENTS;
  const char * send_ptr  = (const char*) sendbuf;
        char * recv_ptr  = (char*) recvbuf;

  MPI_Comm comm = team_data->comm;

  if (nchunks > 0) {
    MPI_Datatype mpi_dtype = dart__mpi__max_chunk_datatype[dtype];
    if (MPI_Scatter(
          send_ptr,
          nchunks,
          mpi_dtype,
          recv_ptr,
          nchunks,
          mpi_dtype,
          root.id,
          comm) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_scatter ! root:%d -> team:%d "
                     "MPI_Scatter failed", root.id, teamid);
      return DART_ERR_INVAL;
    }
    send_ptr += nchunks * MAX_CONTIG_ELEMENTS;
    recv_ptr += nchunks * MAX_CONTIG_ELEMENTS;
  }

  if (remainder > 0) {
    MPI_Datatype mpi_dtype = dart__mpi__datatype(dtype);
    if (MPI_Scatter(
          send_ptr,
          remainder,
          mpi_dtype,
          recv_ptr,
          remainder,
          mpi_dtype,
          root.id,
          comm) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_scatter ! root:%d -> team:%d "
                     "MPI_Scatter failed", root.id, teamid);
      return DART_ERR_INVAL;
    }
  }

  return DART_OK;
}

dart_ret_t dart_gather(
  const void         * sendbuf,
  void               * recvbuf,
  size_t               nelem,
  dart_datatype_t      dtype,
  dart_team_unit_t     root,
  dart_team_t          teamid)
{
  DART_LOG_TRACE("dart_gather() team:%d nelem:%"PRIu64"",
                 teamid, nelem);

  dart_team_data_t *team_data = dart_adapt_teamlist_get(teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_gather ! failed: unknown teamid %d", teamid);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(root, team_data);

  // chunk up the scatter if necessary
  const size_t nchunks   = nelem / MAX_CONTIG_ELEMENTS;
  const size_t remainder = nelem % MAX_CONTIG_ELEMENTS;
  const char * send_ptr  = (const char*) sendbuf;
        char * recv_ptr  = (char*) recvbuf;

  MPI_Comm comm = team_data->comm;

  if (nchunks > 0) {
    MPI_Datatype mpi_dtype = dart__mpi__max_chunk_datatype[dtype];
    if (MPI_Gather(
          send_ptr,
          nchunks,
          mpi_dtype,
          recv_ptr,
          nchunks,
          mpi_dtype,
          root.id,
          comm) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_gather ! root:%d -> team:%d "
                     "MPI_Gather failed", root.id, teamid);
      return DART_ERR_INVAL;
    }
    send_ptr += nchunks * MAX_CONTIG_ELEMENTS;
    recv_ptr += nchunks * MAX_CONTIG_ELEMENTS;
  }

  if (remainder > 0) {
    MPI_Datatype mpi_dtype = dart__mpi__datatype(dtype);
    if (MPI_Gather(
          send_ptr,
          remainder,
          mpi_dtype,
          recv_ptr,
          remainder,
          mpi_dtype,
          root.id,
          comm) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_gather ! root:%d -> team:%d "
                     "MPI_Gather failed", root.id, teamid);
      return DART_ERR_INVAL;
    }
  }

  return DART_OK;
}

dart_ret_t dart_allgather(
  const void      * sendbuf,
  void            * recvbuf,
  size_t            nelem,
  dart_datatype_t   dtype,
  dart_team_t       teamid)
{
  DART_LOG_TRACE("dart_allgather() team:%d nelem:%"PRIu64"",
                 teamid, nelem);

  dart_team_data_t *team_data = dart_adapt_teamlist_get(teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_allgather ! unknown teamid %d", teamid);
    return DART_ERR_INVAL;
  }

  if (sendbuf == recvbuf || NULL == sendbuf) {
    sendbuf = MPI_IN_PLACE;
  }

  // chunk up the scatter if necessary
  const size_t nchunks   = nelem / MAX_CONTIG_ELEMENTS;
  const size_t remainder = nelem % MAX_CONTIG_ELEMENTS;
  const char * send_ptr  = (const char*) sendbuf;
        char * recv_ptr  = (char*) recvbuf;

  MPI_Comm comm = team_data->comm;

  if (nchunks > 0) {
    MPI_Datatype mpi_dtype = dart__mpi__max_chunk_datatype[dtype];
    if (MPI_Allgather(
          send_ptr,
          nchunks,
          mpi_dtype,
          recv_ptr,
          nchunks,
          mpi_dtype,
          comm) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_allgather ! team:%d MPI_Allgather failed", teamid);
      return DART_ERR_INVAL;
    }
    send_ptr += nchunks * MAX_CONTIG_ELEMENTS;
    recv_ptr += nchunks * MAX_CONTIG_ELEMENTS;
  }

  if (remainder > 0) {
    MPI_Datatype mpi_dtype = dart__mpi__datatype(dtype);
    if (MPI_Allgather(
          send_ptr,
          remainder,
          mpi_dtype,
          recv_ptr,
          remainder,
          mpi_dtype,
          comm) != MPI_SUCCESS) {
      DART_LOG_ERROR("dart_allgather ! team:%d MPI_Allgather failed", teamid);
      return DART_ERR_INVAL;
    }
  }

  DART_LOG_TRACE("dart_allgather > team:%d nelem:%"PRIu64"",
                 teamid, nelem);
  return DART_OK;
}

dart_ret_t dart_allgatherv(
  const void      * sendbuf,
  size_t            nsendelem,
  dart_datatype_t   dtype,
  void            * recvbuf,
  const size_t    * nrecvcounts,
  const size_t    * recvdispls,
  dart_team_t       teamid)
{
  MPI_Datatype mpi_dtype = dart__mpi__datatype(dtype);
  MPI_Comm     comm;
  int          comm_size;
  DART_LOG_TRACE("dart_allgatherv() team:%d nsendelem:%"PRIu64"",
                 teamid, nsendelem);

  /*
   * MPI uses offset type int, do not copy more than INT_MAX elements:
   */
  if (nsendelem > MAX_CONTIG_ELEMENTS) {
    DART_LOG_ERROR("dart_allgather ! failed: nsendelem (%zu) > INT_MAX", nsendelem);
    return DART_ERR_INVAL;
  }

  dart_team_data_t *team_data = dart_adapt_teamlist_get(teamid);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_allgatherv ! unknown teamid %d", teamid);
    return DART_ERR_INVAL;
  }
  if (sendbuf == recvbuf || NULL == sendbuf) {
    sendbuf = MPI_IN_PLACE;
  }
  comm = team_data->comm;

  // convert nrecvcounts and recvdispls
  MPI_Comm_size(comm, &comm_size);
  int *inrecvcounts = malloc(sizeof(int) * comm_size);
  int *irecvdispls  = malloc(sizeof(int) * comm_size);
  for (int i = 0; i < comm_size; i++) {
    if (nrecvcounts[i] > MAX_CONTIG_ELEMENTS || 
        recvdispls[i] > MAX_CONTIG_ELEMENTS) 
    {
      DART_LOG_ERROR(
        "dart_allgatherv ! failed: nrecvcounts[%i] (%zu) > INT_MAX || "
        "recvdispls[%i] (%zu) > INT_MAX", i, nrecvcounts[i], i, recvdispls[i]);
      free(inrecvcounts);
      free(irecvdispls);
      return DART_ERR_INVAL;
    }
    inrecvcounts[i] = nrecvcounts[i];
    irecvdispls[i]  = recvdispls[i];
  }

  if (MPI_Allgatherv(
           sendbuf,
           nsendelem,
           mpi_dtype,
           recvbuf,
           inrecvcounts,
           irecvdispls,
           mpi_dtype,
           comm) != MPI_SUCCESS) {
    DART_LOG_ERROR("dart_allgatherv ! team:%d nsendelem:%"PRIu64" failed",
                   teamid, nsendelem);
    free(inrecvcounts);
    free(irecvdispls);
    return DART_ERR_INVAL;
  }
  free(inrecvcounts);
  free(irecvdispls);
  DART_LOG_TRACE("dart_allgatherv > team:%d nsendelem:%"PRIu64"",
                 teamid, nsendelem);
  return DART_OK;
}

dart_ret_t dart_allreduce(
  const void       * sendbuf,
  void             * recvbuf,
  size_t             nelem,
  dart_datatype_t    dtype,
  dart_operation_t   op,
  dart_team_t        team)
{
  MPI_Comm     comm;
  MPI_Op       mpi_op    = dart__mpi__op(op);
  MPI_Datatype mpi_dtype = dart__mpi__datatype(dtype);

  /*
   * MPI uses offset type int, do not copy more than INT_MAX elements:
   */
  if (nelem > MAX_CONTIG_ELEMENTS) {
    DART_LOG_ERROR("dart_allreduce ! failed: nelem (%zu) > INT_MAX", nelem);
    return DART_ERR_INVAL;
  }

  dart_team_data_t *team_data = dart_adapt_teamlist_get(team);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_allreduce ! unknown teamid %d", team);
    return DART_ERR_INVAL;
  }
  comm = team_data->comm;
  if (MPI_Allreduce(
           sendbuf,   // send buffer
           recvbuf,   // receive buffer
           nelem,     // buffer size
           mpi_dtype, // datatype
           mpi_op,    // reduce operation
           comm) != MPI_SUCCESS) {
    return DART_ERR_INVAL;
  }
  return DART_OK;
}

dart_ret_t dart_reduce(
  const void        * sendbuf,
  void              * recvbuf,
  size_t              nelem,
  dart_datatype_t     dtype,
  dart_operation_t    op,
  dart_team_unit_t    root,
  dart_team_t         team)
{
  MPI_Comm     comm;
  MPI_Op       mpi_op    = dart__mpi__op(op);
  MPI_Datatype mpi_dtype = dart__mpi__datatype(dtype);

  /*
   * MPI uses offset type int, do not copy more than INT_MAX elements:
   */
  if (nelem > MAX_CONTIG_ELEMENTS) {
    DART_LOG_ERROR("dart_allreduce ! failed: nelem (%zu) > INT_MAX", nelem);
    return DART_ERR_INVAL;
  }

  dart_team_data_t *team_data = dart_adapt_teamlist_get(team);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_reduce ! unknown teamid %d", team);
    return DART_ERR_INVAL;
  }
  
  CHECK_UNITID_RANGE(root, team_data);

  comm = team_data->comm;
  if (MPI_Reduce(
           sendbuf,
           recvbuf,
           nelem,
           mpi_dtype,
           mpi_op,
           root.id,
           comm) != MPI_SUCCESS) {
    return DART_ERR_INVAL;
  }
  return DART_OK;
}

dart_ret_t dart_send(
  const void         * sendbuf,
  size_t              nelem,
  dart_datatype_t     dtype,
  int                 tag,
  dart_global_unit_t  unit)
{
  MPI_Comm comm;
  MPI_Datatype mpi_dtype = dart__mpi__datatype(dtype);
  dart_team_t team = DART_TEAM_ALL;

  /*
   * MPI uses offset type int, do not copy more than INT_MAX elements:
   */
  if (nelem > MAX_CONTIG_ELEMENTS) {
    DART_LOG_ERROR("dart_send ! failed: nelem (%zu) > INT_MAX", nelem);
    return DART_ERR_INVAL;
  }

  dart_team_data_t *team_data = dart_adapt_teamlist_get(team);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_send ! unknown teamid %d", team);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(unit, team_data);

  comm = team_data->comm;
  // dart_unit = MPI rank in comm_world
  if(MPI_Send(
        sendbuf,
        nelem,
        mpi_dtype,
        unit.id,
        tag,
        comm) != MPI_SUCCESS) {
    return DART_ERR_INVAL;
  }
  return DART_OK;
}

dart_ret_t dart_recv(
  void                * recvbuf,
  size_t                nelem,
  dart_datatype_t       dtype,
  int                   tag,
  dart_global_unit_t    unit)
{
  MPI_Comm comm;
  MPI_Datatype mpi_dtype = dart__mpi__datatype(dtype);
  dart_team_t team = DART_TEAM_ALL;

  /*
   * MPI uses offset type int, do not copy more than INT_MAX elements:
   */
  if (nelem > MAX_CONTIG_ELEMENTS) {
    DART_LOG_ERROR("dart_recv ! failed: nelem (%zu) > INT_MAX", nelem);
    return DART_ERR_INVAL;
  }

  dart_team_data_t *team_data = dart_adapt_teamlist_get(team);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_recv ! unknown teamid %d", team);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(unit, team_data);

  comm = team_data->comm;
  // dart_unit = MPI rank in comm_world
  if(MPI_Recv(
        recvbuf,
        nelem,
        mpi_dtype,
        unit.id,
        tag,
        comm,
        MPI_STATUS_IGNORE) != MPI_SUCCESS) {
    return DART_ERR_INVAL;
  }
  return DART_OK;
}

dart_ret_t dart_sendrecv(
  const void         * sendbuf,
  size_t               send_nelem,
  dart_datatype_t      send_dtype,
  int                  send_tag,
  dart_global_unit_t   dest,
  void               * recvbuf,
  size_t               recv_nelem,
  dart_datatype_t      recv_dtype,
  int                  recv_tag,
  dart_global_unit_t   src)
{
  MPI_Comm comm;
  MPI_Datatype mpi_send_dtype = dart__mpi__datatype(send_dtype);
  MPI_Datatype mpi_recv_dtype = dart__mpi__datatype(recv_dtype);
  dart_team_t team = DART_TEAM_ALL;

  /*
   * MPI uses offset type int, do not copy more than INT_MAX elements:
   */
  if (send_nelem > MAX_CONTIG_ELEMENTS || recv_nelem > MAX_CONTIG_ELEMENTS) {
    DART_LOG_ERROR("dart_sendrecv ! failed: nelem (%zu, %zu) > INT_MAX",
                   recv_nelem, send_nelem);
    return DART_ERR_INVAL;
  }

  dart_team_data_t *team_data = dart_adapt_teamlist_get(team);
  if (team_data == NULL) {
    DART_LOG_ERROR("dart_sendrecv ! unknown teamid %d", team);
    return DART_ERR_INVAL;
  }

  CHECK_UNITID_RANGE(dest, team_data);
  CHECK_UNITID_RANGE(src, team_data);

  comm = team_data->comm;
  if(MPI_Sendrecv(
        sendbuf,
        send_nelem,
        mpi_send_dtype,
        dest.id,
        send_tag,
        recvbuf,
        recv_nelem,
        mpi_recv_dtype,
        src.id,
        recv_tag,
        comm,
        MPI_STATUS_IGNORE) != MPI_SUCCESS) {
    return DART_ERR_INVAL;
  }
  return DART_OK;
}


