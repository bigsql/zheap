/*-------------------------------------------------------------------------
 *
 * undorecordset.c
 *	  management of sets of records in undo logs
 *
 * An UndoRecordSet acts as a contained for zero or more undo records.
 * To allow for flexibility, an UndoRecordSet can be of any of a number
 * of types; for details and interfaces pertaining to the important
 * URST_TRANSACTION type, see xactundo.c/h.
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/undo/undorecordset.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/undo.h"
#include "access/undolog.h"
#include "access/undopage.h"
#include "access/undorecordset.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogreader.h"
#include "access/xlogutils.h"
#include "catalog/pg_class.h"
#include "catalog/pg_control.h"
#include "miscadmin.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/ipc.h"

#define OPS_HEADER_SIZE		4

/*
 * Per-chunk bookkeeping.
 */
typedef struct UndoRecordSetChunk
{
	UndoLogSlot	   *slot;
	UndoLogOffset	chunk_header_offset;
	int				chunk_header_buffer_index[2];
	uint8			chunk_header_ops[2][OPS_HEADER_SIZE];
} UndoRecordSetChunk;

typedef enum UndoRecordSetState
{
	URS_STATE_CLEAN,			/* has written no data */
	URS_STATE_DIRTY,			/* has written some data */
	URS_STATE_CLOSED			/* wrote data and was then closed */
} UndoRecordSetState;

struct UndoRecordSet
{
	UndoRecordSetType type;
	char			persistence;

	/* Management of chunks, when space runs out. */
	UndoRecordSetChunk *chunks;
	int				nchunks;
	int				max_chunks;

	/* Management of currently pinned and locked buffers. */
	uint8			first_block_id;
	Buffer		   *buffers;
	uint8		   *buffer_flag;
	int				nbuffers;
	int				max_buffers;

	/* UndoAllocate's decision on headers for the in-progress insertion. */
	UndoRecPtr		previous_chunk;
	bool			need_chunk_header;
	UndoRecordSetChunkHeader chunk_header;
	char			type_header[64];
	uint8			type_header_size;
	bool			need_type_header;

	/* Currently active slot for insertion. */
	UndoLogSlot *slot;

	UndoLogOffset	recent_end;

	/* Resource management. */
	UndoRecordSetState	state;
	slist_node		link;
	int				nestingLevel;
};

#define URS_BUFFER_IS_NEW			0x01
#define URS_BUFFER_NEEDS_INIT		0x02
#define URS_BUFFER_NEEDS_DIRTY		0x04
#define URS_BUFFER_NEEDS_XLOGBUF	0x08

/* TODO: should perhaps make type a char and not include the padding */
#define UndoRecordSetChunkHeaderSize sizeof(UndoRecordSetChunkHeader)

static inline void reserve_buffer_array(UndoRecordSet *urs, size_t capacity);

/* Every UndoRecordSet created and not yet destroyed in this backend. */
static slist_head UndoRecordSetList = SLIST_STATIC_INIT(UndoRecordSetList);

/*
 * Create a new UndoRecordSet with the indicated type and persistence level.
 *
 * The persistence level may be RELPERSISTENCE_TEMP, RELPERSISTENCE_UNLOGGED,
 * or RELPERSISTENCE_PERMANENT.
 *
 * An UndoRecordSet is created using this function must be properly closed;
 * see UndoPrepareToMarkClosed and UndoMarkClosed. If nestingLevel > 0, the
 * UndoRecordSet will automatically be closed when the transaction nesting
 * depth drops below this value, unless it has been previously closed
 * explicitly. Even if you plan to close the UndoRecordSet explicitly in
 * normal cases, the use of this facility is advisable to make sure that
 * the UndoRecordSet is closed even in case of ERROR or FATAL.
 */
UndoRecordSet *
UndoCreate(UndoRecordSetType type, char persistence, int nestingLevel,
		   Size type_header_size, char *type_header)
{
	UndoRecordSet *urs;
	MemoryContext	oldcontext;

	Assert(UndoContext != NULL);

	oldcontext = MemoryContextSwitchTo(UndoContext);
	urs = palloc0(sizeof(UndoRecordSet));
	urs->type = type;
	urs->persistence = persistence;
	urs->chunks = palloc(sizeof(urs->chunks[0]));
	urs->max_chunks = 1;
	urs->buffers = palloc(sizeof(urs->buffers[0]));
	urs->buffer_flag = palloc(sizeof(urs->buffer_flag[0]));
	urs->max_buffers = 1;
	urs->type_header_size = type_header_size;
	urs->need_type_header = true;

	/* XXX Why do we have a fixed-size buffer here? */
	Assert(urs->type_header_size <= sizeof(urs->type_header));
	memcpy(urs->type_header, type_header, urs->type_header_size);

	slist_push_head(&UndoRecordSetList, &urs->link);
	urs->nestingLevel = nestingLevel;
	MemoryContextSwitchTo(oldcontext);

	return urs;
}

/*
 * Return the index in urs->buffers of the requested buffer.
 */
static int
find_or_read_buffer(UndoRecordSet *urs, UndoLogNumber logno, BlockNumber block)
{
	RelFileNode rnode;

	/* Do we have it pinned and locked already? */
	for (int i = 0; i < urs->nbuffers; ++i)
	{
		ForkNumber tag_fork;
		BlockNumber tag_block;

		BufferGetTag(urs->buffers[i], &rnode, &tag_fork, &tag_block);
		if (rnode.relNode == logno && tag_block == block)
			return i;
	}

	/* Pin it and lock it. */
	reserve_buffer_array(urs, urs->nbuffers + 1);
	UndoRecPtrAssignRelFileNode(rnode, MakeUndoRecPtr(logno, 0));
	urs->buffers[urs->nbuffers] =
		ReadBufferWithoutRelcache(rnode,
								  UndoLogForkNum,
								  block,
								  RBM_NORMAL,
								  NULL,
								  urs->persistence);
	urs->buffer_flag[urs->nbuffers] = URS_BUFFER_NEEDS_DIRTY;
	if (urs->persistence == RELPERSISTENCE_PERMANENT)
		urs->buffer_flag[urs->nbuffers] |= URS_BUFFER_NEEDS_XLOGBUF;
	LockBuffer(urs->buffers[urs->nbuffers], BUFFER_LOCK_EXCLUSIVE);	

	return urs->nbuffers++;
}

/*
 * Pin and lock buffers that hold all chunk headers, in preparation for
 * marking them closed.
 *
 * Returns 'true' if work needs to be done and 'false' if not. If the return
 * value is 'false', it is acceptable to call UndoDestroy without doing
 * anything further.
 */
bool
UndoPrepareToMarkClosed(UndoRecordSet *urs)
{
	for (int i = 0; i < urs->nchunks; ++i)
	{
		UndoRecordSetChunk *chunk = &urs->chunks[i];
		UndoLogOffset header = chunk->chunk_header_offset;
		BlockNumber header_block = header / BLCKSZ;
		int header_offset = header % BLCKSZ;

		/*
		 * We might need one or two buffers, depending on the position of the
		 * header.  We might need to read a new buffer, but to support
		 * inserting and closing as part of the same WAL record, we also have
		 * to check if we already have the buffer pinned.
		 */
		chunk->chunk_header_buffer_index[0] =
			find_or_read_buffer(urs, chunk->slot->logno, header_block);
		if (header_offset <= BLCKSZ - sizeof(UndoLogOffset))
			chunk->chunk_header_buffer_index[1] = -1;
		else
			chunk->chunk_header_buffer_index[1] =
				find_or_read_buffer(urs, chunk->slot->logno, header_block + 1);
	}

	return (urs->nchunks > 0);
}

static void
write_update_ops_header(uint8 *ops, uint16 offset, uint16 size)
{
	/*
	 * Since the maximum BLCKSZ is 32KB, we can use the top bit to indicate
	 * that this is an 'update' rather than an 'insert'.
	 */
	Assert(BLCKSZ <= 0x8000);
	Assert(offset < BLCKSZ);
	Assert((offset + size) <= BLCKSZ);
	Assert(offset >= SizeOfUndoPageHeaderData);

	ops[0] = 0x80 | (offset >> 8);
	ops[1] = offset & 0xff;
	ops[2] = size >> 8;
	ops[3] = size & 0xff;
}

/*
 * Do the per-page work associated with marking an UndoRecordSet closed.
 */
static int
UndoMarkPageClosed(UndoRecordSet *urs, UndoRecordSetChunk *chunk, int chbidx,
				   int page_offset, int data_offset, UndoLogOffset size)
{
	int		index = chunk->chunk_header_buffer_index[chbidx];
	Buffer	buffer = urs->buffers[index];
	uint8  *ops = chunk->chunk_header_ops[chbidx];
	int		bytes_on_this_page;

	/* Compute the number of bytes on this page. */
	bytes_on_this_page = Min(BLCKSZ - page_offset, sizeof(size) - data_offset);

	/* Update the page. */
	memcpy((char *) BufferGetPage(buffer) + page_offset,
		   (char *) &size + data_offset,
		   bytes_on_this_page);

	/* Mark the buffer dirty, if not yet done. */
	if ((urs->buffer_flag[index] & URS_BUFFER_NEEDS_DIRTY) != 0)
	{
		MarkBufferDirty(buffer);
		urs->buffer_flag[index] &= ~URS_BUFFER_NEEDS_DIRTY;
	}

	/* Register the buffer with XLOG system, if needed and not yet done. */
	if ((urs->buffer_flag[index] & URS_BUFFER_NEEDS_XLOGBUF) != 0)
	{
		XLogRegisterBuffer(urs->first_block_id + index, buffer, 0);
		urs->buffer_flag[index] &= ~URS_BUFFER_NEEDS_XLOGBUF;
	}

	/* Capture this edit as buffer data. */
	if (urs->persistence == RELPERSISTENCE_PERMANENT)
	{
		write_update_ops_header(ops, page_offset, bytes_on_this_page);
		XLogRegisterBufData(urs->first_block_id + index, (char *) ops,
							OPS_HEADER_SIZE);
		XLogRegisterBufData(urs->first_block_id + index,
							BufferGetPage(buffer) + page_offset,
							bytes_on_this_page);
	}

	return bytes_on_this_page;
}

/*
 * TODO: Currently, all opened URSs *must* be closed, because otherwise they
 * may hold an UndoLogSlot that is never returned to the appropriate shared
 * memory freelist, and so it won't be reused.
 */
void
UndoMarkClosed(UndoRecordSet *urs)
{
	/* Must be in a critical section. */
	Assert(CritSectionCount > 0);

	/* Shouldn't already be closed, and should have chunks if it's dirty. */
	Assert(urs->state != URS_STATE_CLOSED);
	Assert(urs->state == URS_STATE_CLEAN || urs->nchunks != 0);

	for (int i = 0; i < urs->nchunks; ++i)
	{
		UndoRecordSetChunk *chunk = &urs->chunks[i];
		UndoLogOffset header = chunk->chunk_header_offset;
		UndoLogOffset insert = chunk->slot->meta.insert;
		UndoLogOffset size = insert - header;
		int page_offset = header % BLCKSZ;
		int data_offset = 0;
		int	chbidx = 0;

		while (data_offset < sizeof(size))
		{
			data_offset += UndoMarkPageClosed(urs, chunk, chbidx++,
											  page_offset, data_offset, size);
			page_offset = SizeOfUndoPageHeaderData;
		}
	}

	/* If it was dirty, mark it closed. */
	if (urs->state == URS_STATE_DIRTY)
		urs->state = URS_STATE_CLOSED;
}

/*
 * Replay the effects of UndoMarkClosed(), and in future perhaps other
 * in-place modifications of undo contents.  Such modifications can be
 * attached to any WAL record.
 */
void
UndoUpdateInRecovery(XLogReaderState *xlog_record)
{
	/* Read and lock all referenced undo log buffers. */
	for (uint8 block_id = 0; block_id <= xlog_record->max_block_id; ++block_id)
	{
		DecodedBkpBlock *block = &xlog_record->blocks[block_id];

		if (block->in_use && block->rnode.dbNode == UndoDbOid)
		{
			Buffer buffer;
			XLogRedoAction action;

			/* Read the buffer. */
			action = XLogReadBufferForRedoExtended(xlog_record,
												   block_id,
												   RBM_NORMAL,
												   false,
												   &buffer);
			if (action == BLK_NEEDS_REDO)
			{
				size_t ops_size;
				uint8 *ops;
				uint8 *ops_end;

				ops = (uint8 *) XLogRecGetBlockData(xlog_record, block_id, &ops_size);
				ops_end = ops + ops_size;

				/* Apply all updates to this page. */
				while (ops < ops_end)
				{
					uint16 offset;
					uint16 size;

					/*
					 * Skip insertions (those are for
					 * UndoInsertInRecovery()).
					 */
					Assert(*ops != 0);
					if ((*ops & 0x80) == 0)
					{
						ops += (*ops + 1);
						continue;
					}

					/* We have an update.  Apply it. */
					if (ops + OPS_HEADER_SIZE >= ops_end)
						elog(ERROR, "corrupted undo update instruction");
					offset = ((ops[0] & 0x7f) << 8) | ops[1];
					size = (ops[2] << 8) | ops[3];
					ops += OPS_HEADER_SIZE;

					if (ops + size > ops_end)
						elog(ERROR, "corrupted undo update instruction");

					memcpy(BufferGetPage(buffer) + offset, ops, size);
					ops += size;
				}

				PageSetLSN(BufferGetPage(buffer), xlog_record->ReadRecPtr);
			}

			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
		}
	}
}

/*
 * Make sure we have enough space to hold a buffer array of a given size.
 */
static inline void
reserve_buffer_array(UndoRecordSet *urs, size_t capacity)
{
	if (unlikely(urs->max_buffers < capacity))
	{
		urs->buffers = repalloc(urs->buffers,
								sizeof(urs->buffers[0]) * capacity);
		urs->buffer_flag = repalloc(urs->buffer_flag,
									sizeof(urs->buffer_flag) * capacity);
		urs->max_buffers = capacity;
	}
}

/*
 * Attach to a new undo log so that we can begin a new chunk.
 */
static void
create_new_chunk(UndoRecordSet *urs)
{
	/* Make sure there is book-keeping space for one more chunk. */
	if (urs->nchunks == urs->max_chunks)
	{
		urs->chunks = repalloc(urs->chunks,
							   sizeof(urs->chunks[0]) * urs->max_chunks * 2);
		urs->max_chunks *= 2;
	}

	/* Get our hands on a new undo log, and go around again. */
	urs->need_chunk_header = true;
	urs->recent_end = 0;
	urs->slot = UndoLogGetForPersistence(urs->persistence);
	urs->chunks[urs->nchunks].slot = urs->slot;
	urs->chunks[urs->nchunks].chunk_header_offset = urs->slot->meta.insert;
	urs->chunks[urs->nchunks].chunk_header_buffer_index[0] = -1;
	urs->chunks[urs->nchunks].chunk_header_buffer_index[1] = -1;
	urs->nchunks++;
}

/*
 * Return a pointer to an undo log span that is guaranteed to be backed by
 * enough physical space for the given number of bytes.
 */
static UndoRecPtr
reserve_physical_undo(UndoRecordSet *urs, size_t total_size)
{
	UndoLogOffset new_insert;

	Assert(urs->nchunks >= 1);
	Assert(urs->chunks);

	new_insert = UndoLogOffsetPlusUsableBytes(urs->slot->meta.insert,
											  total_size);

	/* The fast case: we already know there is enough space. */
	if (new_insert <= urs->recent_end)
		return MakeUndoRecPtr(urs->slot->logno, urs->slot->meta.insert);

	/*
	 * Another backend might have advanced 'end' while discarding,
	 * since we last updated it.
	 */
	LWLockAcquire(&urs->slot->meta_lock, LW_SHARED);
	urs->recent_end = urs->slot->end;
	LWLockRelease(&urs->slot->meta_lock);
	if (new_insert <= urs->recent_end)
		return MakeUndoRecPtr(urs->slot->logno, urs->slot->meta.insert);

	/*
	 * Can we extend this undo log to make space?  Again, it's possible
	 * for end to advance concurrently, but adjust_physical_range() can
	 * deal with that.
	 */
	if (new_insert <= UndoLogMaxSize)
	{
		UndoLogAdjustPhysicalRange(urs->slot->logno, 0, new_insert);
		return MakeUndoRecPtr(urs->slot->logno, urs->slot->meta.insert);
	}

	/*
	 * Mark it full, so that we stop trying to allocate new space
	 * here, and a checkpoint will eventually give up its slot for
	 * reuse.
	 */
	UndoLogMarkFull(urs->slot);
	urs->slot = NULL;
	return InvalidUndoRecPtr;
}

/*
 * Return a pointer to an undo log region backed by physical space, and pin
 * all buffers in the region.
 */
UndoRecPtr
UndoAllocate(UndoRecordSet *urs, size_t data_size)
{
	UndoRecPtr begin;
	size_t header_size;
	size_t total_size;
	RelFileNode rnode;
	BlockNumber block;
	int offset;

	for (;;)
	{
		/* Figure out the total range we need to pin. */
		if (!urs->need_chunk_header)
			header_size = 0;
		else if (!urs->need_type_header)
			header_size = UndoRecordSetChunkHeaderSize;
		else
			header_size = UndoRecordSetChunkHeaderSize + urs->type_header_size;
		total_size = data_size + header_size;

		/* Try to use the active undo log, if there is one. */
		if (urs->slot)
		{
			begin = reserve_physical_undo(urs, total_size);
			if (begin != InvalidUndoRecPtr)
				break;
		}

		/* We need to attach to a new undo log. */
		create_new_chunk(urs);
	}

	/* Make sure our buffer array is large enough. */
	reserve_buffer_array(urs, total_size / BLCKSZ + 2);

	/* We'd better not have any pinned already. */
	Assert(urs->nbuffers == 0);

	/* Figure out which undo log we're in. */
	UndoRecPtrAssignRelFileNode(rnode, begin);
	block = UndoRecPtrGetBlockNum(begin);
	offset = UndoRecPtrGetPageOffset(begin);

	/* Loop, pinning buffers. */
	while (total_size > 0)
	{
		int bytes_on_this_page;
		ReadBufferMode rbm;
		Buffer buffer;

		/*
		 * We always need to mark the buffer dirty when we first modify it.
		 * If this is a permanent relation, we also need to register it with
		 * the XLOG machinery.
		 */
		urs->buffer_flag[urs->nbuffers] = URS_BUFFER_NEEDS_DIRTY;
		if (urs->persistence == RELPERSISTENCE_PERMANENT)
			urs->buffer_flag[urs->nbuffers] |= URS_BUFFER_NEEDS_XLOGBUF;

		/*
		 * If we are writing the first data into this page, we don't need to
		 * read it from disk.  We can just get a zeroed buffer and initialize
		 * it.
		 */
		if (offset == SizeOfUndoPageHeaderData)
		{
			rbm = RBM_ZERO;
			urs->buffer_flag[urs->nbuffers] |=
				URS_BUFFER_IS_NEW | URS_BUFFER_NEEDS_INIT;
		}
		else
			rbm = RBM_NORMAL;

		/*
		 * TODO: Andres doesn't like "without relcache" here.
		 *
		 * (Couldn't we just open the relation normally and use regular old
		 * ReadBuffer? In some earlier versions of the code, this was shared
		 * with the recovery path, but now UndoAllocateInRecovery is separate
		 * anyway.)
		 */
		/* Get a buffer. */
		buffer = urs->buffers[urs->nbuffers++] =
			ReadBufferWithoutRelcache(rnode,
									  UndoLogForkNum,
									  block,
									  rbm,
									  NULL,
									  urs->persistence);

		/* How much to go? */
		bytes_on_this_page = Min(BLCKSZ - offset, total_size);
		total_size -= bytes_on_this_page;

		/* Advance to start of next page. */
		++block;
		offset = SizeOfUndoPageHeaderData;
	}

	/*
	 * Now loop to obtain the content locks.  This is done as a separate loop
	 * so that we don't hold a content lock while potentially evicting a page.
	 *
	 * TODO: This doesn't actually address Andres's complaint, which is that
	 * we will presumably still do the eviction above at a time when an AM
	 * like zheap already has content locks.
	 */
	for (int i = 0; i < urs->nbuffers; ++i)
		LockBuffer(urs->buffers[i], BUFFER_LOCK_EXCLUSIVE);

	/* Return the URP for the first byte of the caller's data. */
	return UndoRecPtrPlusUsableBytes(begin, header_size);
}

typedef struct UndoInsertState
{
	Buffer	   *buffers;
	uint8	   *buffer_flag;
	uint8		first_block_id;
	int			buffer_index;
	UndoRecPtr	insert;
} UndoInsertState;

static void
begin_append(UndoInsertState *state, Buffer *buffers, uint8 *buffer_flag,
			 uint8 first_block_id, UndoRecPtr urp)
{
	state->buffers = buffers;
	state->buffer_flag = buffer_flag;
	state->first_block_id = first_block_id;
	state->buffer_index = 0;
	state->insert = urp;
}

static void
append_bytes(UndoInsertState *state, void *data, size_t size)
{
	while (size > 0)
	{
		int index = state->buffer_index;
		Buffer buffer = state->buffers[index];
		Page page = BufferGetPage(buffer);
		PageHeader header = (PageHeader) page;
		int offset = UndoRecPtrGetPageOffset(state->insert);
		int bytes_on_this_page = Min(BLCKSZ - offset, size);

		/* Initialize the page, if needed and not yet done. */
		if ((state->buffer_flag[index] & URS_BUFFER_NEEDS_INIT) != 0)
		{
			UndoPageInit(BufferGetPage(buffer));
			state->buffer_flag[index] &= ~URS_BUFFER_NEEDS_INIT;
		}

		/* Mark the buffer dirty, if not yet done. */
		if ((state->buffer_flag[index] & URS_BUFFER_NEEDS_DIRTY) != 0)
		{
			MarkBufferDirty(buffer);
			state->buffer_flag[index] &= ~URS_BUFFER_NEEDS_DIRTY;
		}

		/* Register the buffer with XLOG system, if needed and not yet done. */
		if ((state->buffer_flag[index] & URS_BUFFER_NEEDS_XLOGBUF) != 0)
		{
			/*
			 * We don't use REGBUF_STANDARD because we use pd_lower in a way
			 * that is not compatible with 'hole' compression.
			 */
			int flags = 0;

			/*
			 * No need for a full page image to be logged or a page to be read
			 * in if it will be empty.
			 */
			if ((state->buffer_flag[index] & URS_BUFFER_IS_NEW) != 0)
				flags |= REGBUF_WILL_INIT;

			/* TODO: make sure that FPIs can't be turned off for undo pages */

			XLogRegisterBuffer(state->first_block_id + index, buffer, flags);
			state->buffer_flag[index] &= ~URS_BUFFER_NEEDS_XLOGBUF;
		}

		/*
		 * We store the insertion point in pd_lower.  UndoInsertInRecovery()
		 * will use it to resynchronize the insert location.
		 */
		header->pd_lower = offset;
		memcpy(((char *) page) + offset,
			   data,
			   bytes_on_this_page);

		size -= bytes_on_this_page;
		offset += bytes_on_this_page;
		state->insert += bytes_on_this_page;

		/*
		 * If there is no more space on this page, position the next write at
		 * the start of the next page.
		 */
		if (offset == BLCKSZ)
		{
			state->buffer_index++;
			state->insert += SizeOfUndoPageHeaderData;
		}
	}
}

static const uint8 chunk_header_size = UndoRecordSetChunkHeaderSize;

/*
 * Append data to an undo log.  The space must previously have been allocated
 * with UndoAllocate().  The caller must have called XLogBeginInsert() for a
 * WAL record.  This function will register all dirtied buffers, but the
 * caller must provide the first block ID to use, to avoid collision with any
 * other block IDs.
 */
void
UndoInsert(UndoRecordSet *urs,
		   uint8 first_block_id,
		   void *data,
		   size_t data_size)
{
	UndoInsertState state;

	Assert(!InRecovery);

	begin_append(&state, urs->buffers, urs->buffer_flag,
				 first_block_id, urs->slot->meta.insert);
	urs->state = URS_STATE_DIRTY;

	/* Write any required chunk header. */
	if (urs->need_chunk_header)
	{
		urs->chunk_header.type = urs->type;
		urs->chunk_header.previous_chunk = urs->previous_chunk;
		urs->chunk_header.size = 0;

		/* Append it to the undo log. */
		append_bytes(&state, &urs->chunk_header, UndoRecordSetChunkHeaderSize);

		/*
		 * Also attach it verbatim to the first buffer in the WAL record, so
		 * we have it in recovery.  We write a size byte first, which
		 * identifies this as an 'insert' operation because the high bit is
		 * not set.
		 */
		XLogRegisterBufData(first_block_id,
							(char *) &chunk_header_size,
							sizeof(chunk_header_size));
		XLogRegisterBufData(first_block_id,
							(char *) &urs->chunk_header,
							UndoRecordSetChunkHeaderSize);
	}

	/* Write the type header, if we have one. */
	if (urs->need_type_header)
	{
		append_bytes(&state, urs->type_header, urs->type_header_size);

		/*
		 * Also attach it verbatim to the first buffer in the WAL record, so
		 * we have it in recovery.
		 */
		XLogRegisterBufData(first_block_id, (char *) &urs->type_header_size,
							sizeof(urs->type_header_size));
		XLogRegisterBufData(first_block_id, urs->type_header,
							urs->type_header_size);
	}

	/* Finally, write the caller's data. */
	append_bytes(&state, data, data_size);

	/* Advance the insert pointer in shared memory. */
	LWLockAcquire(&urs->slot->meta_lock, LW_EXCLUSIVE);
	urs->slot->meta.insert = state.insert;
	LWLockRelease(&urs->slot->meta_lock);

	/* Don't need another chunk header unless we switch undo logs. */
	urs->need_chunk_header = false;

	/* Don't ever need another type header. */
	urs->need_type_header = false;
}

/*
 * Append data to an undo log during recovery.  We figure out where the data
 * should go by looking at the undo log blocks registered for the WAL record
 * we are replaying.
 */
UndoRecPtr
UndoInsertInRecovery(XLogReaderState *xlog_record, void *data, size_t data_size)
{
	uint8 *ops = NULL;
	size_t ops_size = 0;
	size_t header_size = 0;
	UndoLogSlot *slot = NULL;
	Buffer *buffers;
	uint8 *buffer_flag;
	int nbuffers;
	UndoInsertState state;
	UndoRecPtr result;
	bool skip = false;

	Assert(InRecovery);

	/* Make an array big enough to hold all registered blocks. */
	nbuffers = 0;
	buffers = palloc(sizeof(*buffers) * (xlog_record->max_block_id + 1));
	buffer_flag =
		palloc0(sizeof(*buffer_flag) * (xlog_record->max_block_id + 1));

	/* Read and lock all referenced undo log buffers. */
	for (uint8 block_id = 0; block_id <= xlog_record->max_block_id; ++block_id)
	{
		DecodedBkpBlock *block = &xlog_record->blocks[block_id];

		if (block->in_use && block->rnode.dbNode == UndoDbOid)
		{
			XLogRedoAction action;
			ReadBufferMode rbm;
			UndoLogOffset	past_this_block;

			/* Figure out which undo log is referenced. */
			if (nbuffers == 0)
				slot = UndoLogGetSlot(block->rnode.relNode, false);
			else
				Assert(slot->logno == block->rnode.relNode);

			/*
			 * Check if we need to extend the physical range to cover this
			 * block.
			 */
			past_this_block = (block->blkno + 1) * BLCKSZ;
			if (slot->end < past_this_block)
				UndoLogAdjustPhysicalRange(slot->logno, 0, past_this_block);

			/* Buffer will need to be dirtied when modified. */
			buffer_flag[nbuffers] |= URS_BUFFER_NEEDS_DIRTY;

			/*
			 * We can't yet say if we think it'll be zeroed or not, because we
			 * don't trust our own insert pointer until we've checked whether
			 * there was a FPI.  So just check the flags to determine whether
			 * RBM_ZERO_AND_LOCK is needed.
			 */
			if ((block->flags & BKPBLOCK_WILL_INIT) != 0)
			{
				rbm = RBM_ZERO_AND_LOCK;
				buffer_flag[nbuffers] |=
					URS_BUFFER_IS_NEW | URS_BUFFER_NEEDS_INIT;
			}
			else
				rbm = RBM_NORMAL;

			/* Read the buffer. */
			action = XLogReadBufferForRedoExtended(xlog_record,
												   block_id,
												   rbm,
												   false,
												   &buffers[nbuffers]);
			if (action == BLK_RESTORED && nbuffers == 0)
			{
				/*
				 * If the first page included a full-page image, we use its
				 * page header to resynchronize the insert location.  This
				 * avoids a problem caused by online checkpoints capturing
				 * future insert locations.
				 */
				Page page = BufferGetPage(buffers[nbuffers]);
				PageHeader header = (PageHeader) page;

				/* Step over page header. */
				if (header->pd_lower == 0)
					header->pd_lower = SizeOfUndoPageHeaderData;

				slot->meta.insert = BLCKSZ * block->blkno + header->pd_lower;
			}
			else if (nbuffers == 0)
			{
				/*
				 * Our incrementally maintained shared-memory insert location
				 * had better be pointing to the first registered block.
				 */
				Assert(UndoRecPtrGetBlockNum(slot->meta.insert) == block->blkno);
			}

			if (action == BLK_NOTFOUND)
			{
				/*
				 * It must be discarded later in the WAL, so we should just
				 * forget about inserting this data; we just need to maintain
				 * the insert location correctly.
				 */
				/* TODO: Think harder about this case and how to test it! */
				skip = true;
			}

			/* TODO: Why does pd_flags finish up different in recovery? */

			/*
			 * There may be a chunk header and a type header attached to the first
			 * block.  If so we'll insert them.
			 */
			if (nbuffers == 0)
				ops = (uint8 *) XLogRecGetBlockData(xlog_record, block_id, &ops_size);

			++nbuffers;
		}
	}

	if (nbuffers == 0)
		elog(LOG, "couldn't find any registered undo log blocks");

	/* Append the data. */
	if (!skip)
		begin_append(&state, buffers, buffer_flag, -1,
					 MakeUndoRecPtr(slot->logno, slot->meta.insert));

	/* Were any insertions recorded for this buffer? */
	if (ops != NULL)
	{
		uint8 *ops_end = ops + ops_size;

		while (ops < ops_end && *ops < 0x80)
		{
			uint8 length = *ops;

			++ops;
			if (ops + length > ops_end)
				elog(ERROR, "undo insert data corrupted");

			append_bytes(&state, ops, length);
			ops += length;
			header_size += length;
		}
		if (!skip)
			append_bytes(&state, data, data_size);
	}

	/* Update the page LSNs and release. */
	for (int i = 0; i < nbuffers; ++i)
	{
		if (BufferIsValid(buffers[i]))
		{
			PageSetLSN(BufferGetPage(buffers[i]), xlog_record->ReadRecPtr);
			LockBuffer(buffers[i], BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffers[i]);
		}
	}

	pfree(buffers);

	/*
	 * We return a pointer to the start of the passed-in data, after any
	 * headers that precede it.
	 */
	result =
		UndoRecPtrPlusUsableBytes(MakeUndoRecPtr(slot->logno, slot->meta.insert),
								  header_size);

	/* Advance insert pointer past this undo record. */
	slot->meta.insert =
		UndoLogOffsetPlusUsableBytes(slot->meta.insert, header_size + data_size);

	return result;
}

/*
 * Set page LSNs for buffers dirtied by UndoInsert or UndoMarkClosed.
 */
void
UndoPageSetLSN(UndoRecordSet *urs, UndoRecPtr lsn)
{
	for (int i = 0; i < urs->nbuffers; ++i)
		PageSetLSN(BufferGetPage(urs->buffers[i]), lsn);
}

/*
 * Release buffer locks and pins held by an UndoRecordSet.
 */
void
UndoRelease(UndoRecordSet *urs)
{
	for (int i = 0; i < urs->nbuffers; ++i)
		UnlockReleaseBuffer(urs->buffers[i]);
	urs->nbuffers = 0;
}

/*
 * Destroy an UndoRecordSet.
 *
 * If any data has been written, the UndoRecordSet must be closed before it
 * is destroyed.
 */
void
UndoDestroy(UndoRecordSet *urs)
{
	/* Release buffer locks. */
	UndoRelease(urs);

	/* If you write any data, you also have to close it properly. */
	if (urs->state == URS_STATE_DIRTY)
		elog(PANIC, "dirty undo record set not closed before release");

	/* Return undo logs to appropriate free lists. */
	for (int i = 0; i < urs->nchunks; ++i)
		UndoLogPut(urs->chunks[i].slot);

	/* Remove from list of all known record sets. */
	slist_delete(&UndoRecordSetList, &urs->link);

	/* Free memory. */
	pfree(urs->chunks);
	pfree(urs->buffers);
	pfree(urs->buffer_flag);
	pfree(urs);
}

/*
 * Reset undo insertion state.
 *
 * This code is invoked during transaction abort to forget about any buffers
 * we think we've locked in UndoAllocate() or UndoPrepareToMarkClosed(); such
 * locks have already been released, and we'll have to reacquire them to
 * close the UndoRecordSet.
 */
void
UndoResetInsertion(void)
{
	slist_iter	iter;

	slist_foreach(iter, &UndoRecordSetList)
	{
		UndoRecordSet *urs = slist_container(UndoRecordSet, link, iter.cur);

		urs->nbuffers = 0;
	}
}

/*
 * Prepare to mark UndoRecordSets for this transaction level closed.
 *
 * Like UndoPrepareToMarkClosed, this should be called prior to entering
 * a critical section.
 *
 * Returns true if there is work to be done and false otherwise; caller may
 * skip directly to UndoDestroyForXactLevel if the return value is false.
 */
bool
UndoPrepareToMarkClosedForXactLevel(int nestingLevel)
{
	slist_iter	iter;
	bool		needs_work = false;

	slist_foreach(iter, &UndoRecordSetList)
	{
		UndoRecordSet *urs = slist_container(UndoRecordSet, link, iter.cur);

		if (nestingLevel <= urs->nestingLevel &&
			urs->state == URS_STATE_DIRTY &&
			UndoPrepareToMarkClosed(urs))
			needs_work = true;
	}

	return needs_work;
}

/*
 * Mark UndoRecordSets for this transaction level closed.
 *
 * Like UndoMarkClosed, this should be called from within the critical section,
 * during WAL record construction.
 */
void
UndoMarkClosedForXactLevel(int nestingLevel)
{
	slist_iter	iter;

	slist_foreach(iter, &UndoRecordSetList)
	{
		UndoRecordSet *urs = slist_container(UndoRecordSet, link, iter.cur);

		if (nestingLevel <= urs->nestingLevel &&
			urs->state == URS_STATE_DIRTY)
			UndoMarkClosed(urs);
	}
}

/*
 * Set page LSNs for all UndoRecordSets for this transaction level.
 *
 * Like UndoPageSetLSN, this should be called just after XLogInsert.
 */
void
UndoPageSetLSNForXactLevel(int nestingLevel, XLogRecPtr lsn)
{
	slist_iter	iter;

	slist_foreach(iter, &UndoRecordSetList)
	{
		UndoRecordSet *urs = slist_container(UndoRecordSet, link, iter.cur);

		if (nestingLevel <= urs->nestingLevel &&
			urs->state == URS_STATE_DIRTY)
			UndoPageSetLSN(urs, lsn);
	}
}

/*
 * Destroy UndoRecordSets for this transaction level.
 *
 * Like UndoDestroy, this should be called after the UndoRecordSet has been
 * marked closed and the surrounding critical section has ended.
 */
void
UndoDestroyForXactLevel(int nestingLevel)
{
	slist_iter	iter;
	bool		restart = true;

	/*
	 * First, release all buffer locks.
	 *
	 * It seems like a good idea not to hold any LWLocks for longer than
	 * necessary, so do this step for every UndoRecordSet first.
	 */
	slist_foreach(iter, &UndoRecordSetList)
	{
		UndoRecordSet *urs = slist_container(UndoRecordSet, link, iter.cur);

		if (nestingLevel <= urs->nestingLevel)
			UndoRelease(urs);
	}

	/*
	 * Now destroy the UndoRecordSets.
	 *
	 * UndoDestroy will update UndoRecordSetList, so we have to restart
	 * the iterator after calling it. This might seem like an inefficient
	 * approach, but in practice the list shouldn't have more than a few
	 * elements and the ones we care about are probably all at the beginning,
	 * so it shouldn't really matter.
	 */
	while (restart)
	{
		restart = false;

		slist_foreach(iter, &UndoRecordSetList)
		{
			UndoRecordSet *urs;

			urs = slist_container(UndoRecordSet, link, iter.cur);
			if (nestingLevel <= urs->nestingLevel)
			{
				UndoDestroy(urs);
				restart = true;
				break;
			}
		}
	}
}

/*
 * Close and release all UndoRecordSets for this transaction level.
 *
 * This should normally be used only when a transaction or subtransaction ends
 * without writing some other WAL record to which the closure of the
 * UndoRecordSet could be attached.
 *
 * Closing an UndoRecordSet piggybacks on another WAL record; since this
 * is intended to be used when there is no such record, we write an XLOG_NOOP
 * record.
 *
 * Returns true if we did anything, and false if nothing needed to be done.
 */
bool
UndoCloseAndDestroyForXactLevel(int nestingLevel)
{
	XLogRecPtr	lsn;
	bool		needs_work;

	needs_work = UndoPrepareToMarkClosedForXactLevel(nestingLevel);

	if (needs_work)
	{
		START_CRIT_SECTION();
		XLogBeginInsert();
		UndoMarkClosedForXactLevel(nestingLevel);
		lsn = XLogInsert(RM_XLOG_ID, XLOG_NOOP);
		UndoPageSetLSNForXactLevel(nestingLevel, lsn);
		END_CRIT_SECTION();
	}

	UndoDestroyForXactLevel(nestingLevel);

	return needs_work;
}

/*
 * It should be impossible to reach this code with any UndoRecordSet
 * still in existence, but maybe there's someway for it to happen if
 * we experience failures while trying to abort the active transaction.
 *
 * It could also happen if somebody writes code that invokes UndoCreate()
 * and doesn't provide a mechanism to make sure that the UndoRecordSet
 * gets closed.
 *
 * If it does happen, use PANIC to recover. System restart will set
 * the size of any UndoRecordSet that was not properly closed. (We could
 * also try again here, but it's not clear whether all of the services
 * that we'd need in order to do so are still working. Also, if it already
 * failed during transaction abort, it doesn't seem all that likely to
 * work now.)
 */
void
AtProcExit_UndoRecordSet(void)
{
	if (!slist_is_empty(&UndoRecordSetList))
		elog(PANIC, "undo record set not closed before backend exit");
}
