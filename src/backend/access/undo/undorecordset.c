/*-------------------------------------------------------------------------
 *
 * undorecordset.c
 *	  management of sets of records in undo logs
 *
 * An UndoRecordSet object is used to manage the creation of a set of
 * related undo records on disk. Typically, this corresponds to all the
 * records written by a single transaction for a single persistence
 * level (permanent, temporary, unlogged) but we don't assume that here,
 * since other uses of the undo storage mechanism are possible.
 *
 * Multiple undo record sets may be written within a single undo log,
 * and a single undo record set may span multiple undo logs. The latter
 * is fairly uncommon, because undo logs are big (1TB) and most
 * transactions will write far less than that amount of undo. A
 * single undo record, however, cannot span multiple undo logs. An
 * undo record set on disk therefore consists of a series of one or more
 * chunks, each of which consists of a chunk header and one or more of
 * records, and the first of which also has a type-specific header
 * containing whatever data is needed for the particular type of record
 * set that it is. For example, if it belongs to a transaction, the
 * type-specific header will contain the transaction ID. The
 * type-specific header and chunk header are written at the same time
 * as the first record in the chunk so as to minimize WAL volume.
 *
 * Every undo record set that is created must be properly closed,
 * for two principal reasons.  First, if any records have been written
 * to disk, the final size of the last chunk must be set on disk; by
 * convention, the last undo record set within an undo log may have
 * a size of 0, indicating that data is still being written, but all
 * previous ones must have a correct size. Second, while one backend
 * is writing to an undo record set, no other backend can write to
 * the same undo log, since record sets are not interleaved; closing
 * the undo record set makes that undo log available for reuse.
 * In the event of a crash, undolog.c will put all undo logs back on
 * the free list. Also, the last chunk in each undo log will be inspected
 * to see whether the size is 0; if so, the size will be set based on the
 * insert pointer for that undo log.
 *
 * Clients of this module are responsible for ensuring that undo record
 * sets are closed in all cases that do not involve a system crash.
 * If they fail to do so, this module will trigger a PANIC at backend
 * exit; the crash recovery algorithm described above should get
 * things back to a sane state.
 *
 * Code that wants to write transactional undo should interface with
 * xactundo.c, q.v., rather than calling these interfaces directly.
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
#include "access/undorecordset_xlog.h"
#include "access/xactundo.h"	/* XXX should we avoid this? */
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

/*
 * Per-chunk bookkeeping.
 */
typedef struct UndoRecordSetChunk
{
	UndoLogSlot	   *slot;
	bool			chunk_header_written;
	/* The offset of the chunk header. */
	UndoLogOffset	chunk_header_offset;
	/* The index of the one or two buffers that hold the size. */
	int				chunk_header_buffer_index[2];
} UndoRecordSetChunk;

typedef enum UndoRecordSetState
{
	URS_STATE_CLEAN,			/* has written no data */
	URS_STATE_DIRTY,			/* has written some data */
	URS_STATE_CLOSED			/* wrote data and was then closed */
} UndoRecordSetState;

typedef struct UndoBuffer
{
	Buffer			buffer;
	bool			is_new;
	bool			needs_init;
	UndoRecordSetXLogBufData bufdata;
} UndoBuffer;

struct UndoRecordSet
{
	UndoRecordSetType type;
	char			persistence;

	/*
	 * Management of chunks used when inserting.  Typically there will only be
	 * one, but when the end of the address space in an undo log is reached,
	 * we may need to wrap into another.
	 */
	UndoRecordSetChunk *chunks;
	int				nchunks;
	int				max_chunks;

	/* Management of currently pinned and locked buffers. */
	UndoBuffer	   *buffers;
	int				nbuffers;
	int				max_buffers;

	/*
	 * UndoPrepareToInsert's decision on headers for the in-progress
	 * insertion.
	 */
	UndoRecPtr		previous_chunk;
	bool			need_chunk_header;
	UndoRecordSetChunkHeader chunk_header;
	char		   *type_header;
	uint8			type_header_size;
	bool			need_type_header;
	UndoRecPtr		begin;

	/* Currently active slot for insertion. */
	UndoLogSlot	   *slot;
	UndoRecPtr		chunk_start;		/* where the chunk started */

	UndoLogOffset	recent_end;

	/* Resource management. */
	UndoRecordSetState	state;
	slist_node		link;
	int				nestingLevel;
};

#define URSNeedsWAL(urs) ((urs)->persistence == RELPERSISTENCE_PERMANENT)

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
	urs->max_buffers = 1;
	urs->type_header_size = type_header_size;
	urs->need_type_header = true;

	urs->type_header = palloc(type_header_size);
	memcpy(urs->type_header, type_header, type_header_size);
	urs->type_header_size = type_header_size;

	slist_push_head(&UndoRecordSetList, &urs->link);
	urs->nestingLevel = nestingLevel;
	MemoryContextSwitchTo(oldcontext);

	return urs;
}

/*
 * TODO: think about where this belongs.
 */
static size_t
get_type_header_size(UndoRecordSetType type)
{
	switch (type)
	{
	case URST_TRANSACTION:
		return 8;
	case URST_FOO:
		return 4;
	default:
		return 0;
	}
}

/*
 * Return the index in urs->buffers of the requested buffer, or create a new
 * one.
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

		BufferGetTag(urs->buffers[i].buffer, &rnode, &tag_fork, &tag_block);
		if (rnode.relNode == logno && tag_block == block)
			return i;
	}

	/* Pin it and lock it. */
	reserve_buffer_array(urs, urs->nbuffers + 1);
	memset(&urs->buffers[urs->nbuffers], 0, sizeof(urs->buffers[0]));
	UndoRecPtrAssignRelFileNode(rnode, MakeUndoRecPtr(logno, 0));
	urs->buffers[urs->nbuffers].buffer =
		ReadBufferWithoutRelcache(rnode,
								  UndoLogForkNum,
								  block,
								  RBM_NORMAL,
								  NULL,
								  urs->persistence);
	LockBuffer(urs->buffers[urs->nbuffers].buffer, BUFFER_LOCK_EXCLUSIVE);

	return urs->nbuffers++;
}

static void
UndoPrepareToMarkChunkClosed(UndoRecordSet *urs, UndoRecordSetChunk *chunk)
{
	UndoLogOffset header;
	BlockNumber header_block;
	int header_offset;

	/* Find the header of this chunk. */
	header = chunk->chunk_header_offset;
	header_block = header / BLCKSZ;
	header_offset = header % BLCKSZ;

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

/*
 * Pin and lock the buffers that hold the active chunk's header, in
 * preparation for marking it closed.
 *
 * Returns 'true' if work needs to be done and 'false' if not. If the return
 * value is 'false', it is acceptable to call UndoDestroy without doing
 * anything further.
 */
bool
UndoPrepareToMarkClosed(UndoRecordSet *urs)
{
	if (urs->nchunks == 0)
		return false;

	UndoPrepareToMarkChunkClosed(urs, &urs->chunks[urs->nchunks - 1]);

	return true;
}

/*
 * Do the per-page work associated with marking an UndoRecordSet closed.
 */
static int
UndoMarkPageClosed(UndoRecordSet *urs, UndoRecordSetChunk *chunk, int chbidx,
				   int page_offset, int data_offset, UndoLogOffset size)
{
	int		index = chunk->chunk_header_buffer_index[chbidx];
	Buffer	buffer = urs->buffers[index].buffer;
	int		bytes_on_this_page;

	/* Update the page. */
	bytes_on_this_page =
		UndoPageOverwrite(BufferGetPage(buffer),
						  page_offset,
						  data_offset,
						  sizeof(size),
						  (char *) &size);
	MarkBufferDirty(buffer);

	return bytes_on_this_page;
}

static void
UndoMarkChunkClosed(UndoRecordSet *urs, UndoRecordSetChunk *chunk,
					bool close_urs)
{
	UndoLogOffset header;
	UndoLogOffset insert;
	UndoLogOffset size;
	int page_offset;
	int data_offset;
	int	chbidx;

	/* Must be in a critical section. */
	Assert(CritSectionCount > 0);

	/* Must have prepared the buffers for this. */
	Assert(chunk->chunk_header_buffer_index[0] != -1);

	header = chunk->chunk_header_offset;
	insert = chunk->slot->meta.insert;
	size = insert - header;
	page_offset = header % BLCKSZ;
	data_offset = 0;
	chbidx = 0;

	/* Record the close as bufdata on the first affected page. */
	if (URSNeedsWAL(urs))
	{
		UndoBuffer *ubuf;

		ubuf = &urs->buffers[chunk->chunk_header_buffer_index[0]];
		ubuf->bufdata.flags |= URS_XLOG_CLOSE_CHUNK;
		ubuf->bufdata.chunk_size_page_offset = page_offset;
		ubuf->bufdata.chunk_size = size;

		/*
		 * If we're closing the final chunk and thus the whole URS, we need to
		 * log some extra details.
		 */
		if (close_urs)
		{
			ubuf->bufdata.flags |= URS_XLOG_CLOSE;
			ubuf->bufdata.urs_type = urs->type;
			ubuf->bufdata.type_header = urs->type_header;
			ubuf->bufdata.type_header_size = urs->type_header_size;

			/* If not a single chunk, point to the first chunk. */
			if (urs->nchunks > 1)
			{
				ubuf->bufdata.flags |= URS_XLOG_CLOSE_MULTI_CHUNK;
				ubuf->bufdata.first_chunk_header_location =
					MakeUndoRecPtr(urs->chunks[0].slot->logno,
								   urs->chunks[0].chunk_header_offset);
			}
		}
	}

	while (data_offset < sizeof(size))
	{
		data_offset += UndoMarkPageClosed(urs, chunk, chbidx++,
										  page_offset, data_offset, size);
		page_offset = SizeOfUndoPageHeaderData;
	}
}

/*
 * Mark an undo record set closed.
 *
 * This should be called from the critical section, after having first called
 * UndoPrepareToMarkClosed before establishing the critical section.
 */
void
UndoMarkClosed(UndoRecordSet *urs)
{
	UndoRecordSetChunk *chunk;

	/* Shouldn't already be closed, and should have chunks if it's dirty. */
	Assert(urs->state != URS_STATE_CLOSED);
	Assert(urs->state == URS_STATE_CLEAN || urs->nchunks != 0);

	if (urs->state == URS_STATE_DIRTY)
	{
		/* Locate the active chunk. */
		chunk = &urs->chunks[urs->nchunks - 1];
		UndoMarkChunkClosed(urs, chunk, true);

		urs->state = URS_STATE_CLOSED;
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
	urs->chunks[urs->nchunks].chunk_header_written = false;
	urs->chunks[urs->nchunks].chunk_header_offset = urs->slot->meta.insert;
	urs->chunks[urs->nchunks].chunk_header_buffer_index[0] = -1;
	urs->chunks[urs->nchunks].chunk_header_buffer_index[1] = -1;
	urs->chunk_start = MakeUndoRecPtr(urs->slot->logno, urs->slot->meta.insert);
	urs->nchunks++;
}

/*
 * Return a pointer to an undo log span that is guaranteed to be backed by
 * enough physical space for the given number of bytes.  Return
 * InvalidUndoRecPtr if there is not enough space remaining in the current
 * active undo log, indicating that the caller needs to create a new chunk.
 */
static UndoRecPtr
reserve_physical_undo(UndoRecordSet *urs, size_t total_size)
{
	UndoLogOffset new_insert;
	UndoLogOffset size;

	Assert(urs->nchunks >= 1);
	Assert(urs->chunks);

	/*
	 * Although this is in shared memory, it can only be set (for testing) if
	 * we are currently attached to it, so it's safe to read it without
	 * locking.
	 */
	if (unlikely(urs->slot->force_truncate))
	{
		UndoLogTruncate(urs->slot);
		urs->slot->force_truncate = false;
		urs->slot = NULL;
		return InvalidUndoRecPtr;
	}

	new_insert = UndoLogOffsetPlusUsableBytes(urs->slot->meta.insert,
											  total_size);

	/* The fast path: we already know there is enough space. */
	if (new_insert <= urs->recent_end)
		return MakeUndoRecPtr(urs->slot->logno, urs->slot->meta.insert);

	/*
	 * Another backend might have advanced 'end' while discarding,
	 * since we last updated it.
	 */
	LWLockAcquire(&urs->slot->meta_lock, LW_SHARED);
	urs->recent_end = urs->slot->end;
	size = urs->slot->meta.size;
	LWLockRelease(&urs->slot->meta_lock);
	if (new_insert <= urs->recent_end)
		return MakeUndoRecPtr(urs->slot->logno, urs->slot->meta.insert);

	/*
	 * Can we extend this undo log to make space?  Again, it's possible for
	 * end to advance concurrently, but UndoLogAdjustPhysicalRange() can deal
	 * with that.
	 */
	if (new_insert <= size)
	{
		UndoLogAdjustPhysicalRange(urs->slot->logno, 0, new_insert);
		return MakeUndoRecPtr(urs->slot->logno, urs->slot->meta.insert);
	}

	/*
	 * There is not enough space left for this record.  Truncate any remaining
	 * space, so that we stop trying to reuse this undo log, and a checkpoint
	 * will eventually give up its slot for reuse.
	 */
	UndoLogTruncate(urs->slot);
	urs->slot = NULL;
	return InvalidUndoRecPtr;
}

/*
 * Return a pointer to an undo log region backed by sufficient physical space
 * for a record of a given size to be inserted, and pin all buffers in the
 * region.
 *
 * This operation may also prepare to mark an existing URS chunk to be marked
 * closed due to lack of space, if a new one must be created.
 */
UndoRecPtr
UndoPrepareToInsert(UndoRecordSet *urs, size_t record_size)
{
	UndoRecPtr begin;
	size_t header_size;
	size_t total_size;
	RelFileNode rnode;
	BlockNumber block;
	int offset;
	int chunk_number_to_close = -1;

	for (;;)
	{
		/* Figure out the total range we need to pin. */
		if (!urs->need_chunk_header)
			header_size = 0;
		else if (!urs->need_type_header)
			header_size = SizeOfUndoRecordSetChunkHeader;
		else
			header_size = SizeOfUndoRecordSetChunkHeader + urs->type_header_size;
		total_size = record_size + header_size;

		/* Try to use the active undo log, if there is one. */
		if (urs->slot)
		{
			begin = reserve_physical_undo(urs, total_size);
			if (begin != InvalidUndoRecPtr)
				break;

			/*
			 * The active chunk is full.  We will prepare to mark it closed,
			 * if we had already written a chunk header.  It's possible that
			 * we haven't written anything in there at all, in which case we
			 * just mark the chunk as unused again (otherwise we'd later try
			 * and reference it on disk).
			 */
			if (urs->chunks[urs->nchunks - 1].chunk_header_written)
				chunk_number_to_close = urs->nchunks - 1;
			else
				urs->nchunks--;
		}

		/* We need to create a new chunk in a new undo log. */
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

		memset(&urs->buffers[urs->nbuffers], 0, sizeof(urs->buffers[0]));

		/*
		 * If we are writing the first data into this page, we don't need to
		 * read it from disk.  We can just get a zeroed buffer and initialize
		 * it.
		 */
		if (offset == SizeOfUndoPageHeaderData)
		{
			rbm = RBM_ZERO;
			urs->buffers[urs->nbuffers].is_new = true;
			urs->buffers[urs->nbuffers].needs_init = true;
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
		urs->buffers[urs->nbuffers].buffer =
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
		++urs->nbuffers;
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
		LockBuffer(urs->buffers[i].buffer, BUFFER_LOCK_EXCLUSIVE);

	/*
	 * Tell UndoInsert() where the first byte is (which may be pointing to a
	 * header).
	 */
	urs->begin = begin;

	/*
	 * If we determined that we had to close an existing chunk, do so now.  It
	 * was important to deal with the insertion first, because UndoReplay()
	 * assumes that the blocks used for inserting headers and record data are
	 * registered before blocks touched by incidental work like marking chunks
	 * closed.
	 */
	if (chunk_number_to_close >= 0)
		UndoPrepareToMarkChunkClosed(urs, &urs->chunks[chunk_number_to_close]);

	/*
	 * Tell the caller where the first byte it where it can write record data
	 * (ie after any headers that the caller doesn't know/care about).
	 */
	return UndoRecPtrPlusUsableBytes(begin, header_size);
}

static void
init_if_needed(UndoBuffer *ubuf)
{
	if (ubuf->needs_init)
	{
		UndoPageInit(BufferGetPage(ubuf->buffer));
		ubuf->needs_init = false;
	}
}

static void
register_insert_page_offset_if_needed(UndoBuffer *ubuf, uint16 insert_page_offset)
{
	/*
	 * For now, we record the insertion point for the first insertion by this
	 * WAL record into each buffer.  Later we could find ways to avoid having
	 * to do this, to cut down on registered buffer data in the WAL.
	 */
	if ((ubuf->bufdata.flags & URS_XLOG_INSERT) == 0)
	{
		ubuf->bufdata.insert_page_offset = insert_page_offset;
		ubuf->bufdata.flags |= URS_XLOG_INSERT;
	}
}

static void
register_new_page(UndoBuffer *ubuf,
				  UndoRecordSetType chunk_type,
				  UndoRecPtr chunk_header_location)
{
	ubuf->bufdata.flags |= URS_XLOG_ADD_PAGE;
	ubuf->bufdata.chunk_header_location = chunk_header_location;
	ubuf->bufdata.urs_type = chunk_type;
}

/*
 * Append data to an undo log.  The space must previously have been allocated
 * with UndoPrepareToInsert().
 */
void
UndoInsert(UndoRecordSet *urs,
		   void *record_data,
		   size_t record_size)
{
	int bytes_written;
	int input_offset;
	int buffer_index;
	int page_offset;
	int type_header_size = urs->need_type_header ? urs->type_header_size : 0;
	int chunk_header_size = urs->need_chunk_header ? SizeOfUndoRecordSetChunkHeader : 0;
	int all_header_size = type_header_size + chunk_header_size;

	Assert(!InRecovery);
	Assert(CritSectionCount > 0);

	/* The caller must already have called UndoPrepareToInsert. */
	Assert(urs->slot);
	Assert(urs->nbuffers >= 1);

	/*
	 * We start of writing into the first buffer, at the offset that
	 * UndoPrepareToInsert provided.
	 */
	buffer_index = 0;
	page_offset = urs->begin % BLCKSZ;

	/* Can't be pointing into page header. */
	Assert(page_offset >= SizeOfUndoPageHeaderData);

	/* Write out the header(s), if necessary. */
	if (urs->need_chunk_header)
	{
		UndoRecordSetChunkHeader chunk_header;

		/* Initialize the chunk header. */
		chunk_header.size = 0;
		chunk_header.previous_chunk = InvalidUndoRecPtr;
		chunk_header.type = urs->type;
		if (urs->nchunks > 1)
		{
			UndoRecordSetChunk *prev_chunk;

			prev_chunk = &urs->chunks[urs->nchunks - 2];
			chunk_header.previous_chunk =
				MakeUndoRecPtr(prev_chunk->slot->logno,
							   prev_chunk->chunk_header_offset);
		}

		input_offset = 0;
		for (;;)
		{
			UndoBuffer *ubuf = &urs->buffers[buffer_index];

			if (buffer_index >= urs->nbuffers)
				elog(ERROR, "ran out of buffers while inserting undo record headers");
			init_if_needed(ubuf);
			if (URSNeedsWAL(urs))
			{
				register_insert_page_offset_if_needed(ubuf, page_offset);

				if (input_offset == 0)
				{
					if (urs->need_type_header)
					{
						/*
						 * We'll need to create a new URS in recovery, so we
						 * capture an image of the type header.
						 */
						ubuf->bufdata.flags |= URS_XLOG_CREATE;
						ubuf->bufdata.urs_type = urs->type;
						ubuf->bufdata.type_header = urs->type_header;
						ubuf->bufdata.type_header_size = urs->type_header_size;
					}
					else
					{
						/*
						 * We'll need to add a new chunk to an existing URS in
						 * recovery.
						 */
						ubuf->bufdata.flags |= URS_XLOG_ADD_CHUNK;
						ubuf->bufdata.urs_type = urs->type;
						ubuf->bufdata.previous_chunk_header_location =
							chunk_header.previous_chunk;
					}
				}
			}
			if (page_offset == SizeOfUndoPageHeaderData)
				register_new_page(ubuf, urs->type, urs->chunk_start);
			bytes_written =
				UndoPageInsertHeader(BufferGetPage(ubuf->buffer),
									 page_offset,
									 input_offset,
									 &chunk_header,
									 type_header_size,
									 urs->type_header,
									 urs->chunk_start);
			MarkBufferDirty(ubuf->buffer);
			urs->chunks[urs->nchunks - 1].chunk_header_written = true;
			page_offset += bytes_written;
			input_offset += bytes_written;
			if (input_offset >= all_header_size)
				break;

			/* Any remaining bytes go onto the next page. */
			page_offset = SizeOfUndoPageHeaderData;
			++buffer_index;
		}
	}

	/* Write out the record. */
	input_offset = 0;
	for (;;)
	{
		UndoBuffer *ubuf = &urs->buffers[buffer_index];

		if (buffer_index >= urs->nbuffers)
			elog(ERROR, "ran out of buffers while inserting undo record");
		init_if_needed(ubuf);
		if (URSNeedsWAL(urs))
			register_insert_page_offset_if_needed(ubuf, page_offset);
		if (page_offset == SizeOfUndoPageHeaderData)
			register_new_page(ubuf, urs->type, urs->chunk_start);
		bytes_written =
			UndoPageInsertRecord(BufferGetPage(urs->buffers[buffer_index].buffer),
								 page_offset,
								 input_offset,
								 record_size,
								 record_data,
								 urs->chunk_start,
								 urs->type);
		MarkBufferDirty(urs->buffers[buffer_index].buffer);
		page_offset += bytes_written;
		input_offset += bytes_written;
		if (input_offset >= record_size)
			break;

		/* Any remaining bytes go onto the next page. */
		page_offset = SizeOfUndoPageHeaderData;
		++buffer_index;
	}

	urs->state = URS_STATE_DIRTY;

	/* Advance the insert pointer in shared memory. */
	LWLockAcquire(&urs->slot->meta_lock, LW_EXCLUSIVE);
	urs->slot->meta.insert =
		UndoLogOffsetPlusUsableBytes(urs->slot->meta.insert,
									 all_header_size + record_size);
	LWLockRelease(&urs->slot->meta_lock);

	/*
	 * If we created a new chunk, we may also need to mark the previous chunk
	 * closed.  In that case, UndoPrepareToInsert() will have pinned and
	 * locked the relevant buffers for us.
	 */
	if (urs->nchunks > 1 &&
		urs->chunks[urs->nchunks - 2].chunk_header_buffer_index[0] != -1)
	{
		UndoMarkChunkClosed(urs, &urs->chunks[urs->nchunks - 2], false);
		urs->chunks[urs->nchunks - 2].chunk_header_buffer_index[0] = -1;
	}

	/* We don't need another chunk header unless we switch undo logs. */
	urs->need_chunk_header = false;

	/* We don't ever need another type header. */
	urs->need_type_header = false;
}

/*
 * Insert an undo record and/or replay other undo data modifications that were
 * performed at DO time.  If an undo record was inserted at DO time, the exact
 * same record data and size must be passed in at REDO time.  If no undo
 * record was inserted at DO time, but an URS might have been closed (thereby
 * updating a header), then pass a null pointer and zero size.
 *
 * Return a pointer to the record that was inserted, if record_data was
 * provided.
 */
UndoRecPtr
UndoReplay(XLogReaderState *xlog_record, void *record_data, size_t record_size)
{
	int nbuffers;
	UndoLogSlot *slot;
	UndoRecPtr result = InvalidUndoRecPtr;
	UndoBuffer *buffers;
	bool record_more = false;
	int record_offset = 0;
	UndoRecordSetChunkHeader chunk_header;
	bool header_more = false;
	int header_offset = 0;
	char *type_header = NULL;
	int type_header_size = 0;
	bool chunk_size_more = false;
	size_t chunk_size;
	int chunk_size_offset = 0;

	Assert(InRecovery);

	/* Make an array big enough to hold all registered blocks. */
	nbuffers = 0;
	buffers = palloc(sizeof(*buffers) * (xlog_record->max_block_id + 1));

	/* Read and lock all referenced undo log buffers. */
	for (uint8 block_id = 0; block_id <= xlog_record->max_block_id; ++block_id)
	{
		DecodedBkpBlock *block = &xlog_record->blocks[block_id];

		if (block->in_use && block->rnode.dbNode == UndoDbOid)
		{
			XLogRedoAction action;
			ReadBufferMode rbm;
			UndoLogOffset	past_this_block;
			bool		skip = false;
			UndoRecordSetXLogBufData *bufdata = &buffers[nbuffers].bufdata;
			Page		page;
			UndoPageHeader uph;
			UndoRecPtr chunk_start = InvalidUndoRecPtr;

			/* Figure out which undo log is referenced. */
			slot = UndoLogGetSlot(block->rnode.relNode, false);

			/*
			 * Check if we need to extend the physical range to cover this
			 * block.
			 */
			past_this_block = (block->blkno + 1) * BLCKSZ;
			if (slot->end < past_this_block)
				UndoLogAdjustPhysicalRange(slot->logno, 0, past_this_block);

			/*
			 * We could decide if it should be zeroed or not based on whether
			 * we're inserting the first byte into a page, as a kind of
			 * cross-check.  For now, we just check if a UndoInsert() marked
			 * it as needing to be initialized.
			 */
			if ((block->flags & BKPBLOCK_WILL_INIT) != 0)
			{
				rbm = RBM_ZERO_AND_LOCK;
				buffers[nbuffers].is_new = true;
				buffers[nbuffers].needs_init = true;
			}
			else
				rbm = RBM_NORMAL;

			/* Read the buffer. */
			action = XLogReadBufferForRedoExtended(xlog_record,
												   block_id,
												   rbm,
												   false,
												   &buffers[nbuffers].buffer);

			/*
			 * If the block was restored from a full-page image, we don't need
			 * to make any modifications, but we still need to keep track of
			 * the insertion pointer, in case an insertion spills over onto
			 * the next page.
			 *
			 * If the block was not found, then it must be discarded later in
			 * the WAL.
			 *
			 * In both of these cases, we'll just remember to skip modifying
			 * the page.
			 */
			if (action == BLK_RESTORED || action == BLK_NOTFOUND)
				skip = true;

			if (!DecodeUndoRecordSetXLogBufData(bufdata, xlog_record, block_id))
				elog(ERROR, "failed to decode undo xlog buffer data");
			page = BufferGetPage(buffers[nbuffers].buffer);
			uph = (UndoPageHeader) page;

			/*
			 * The UndoPageXXX function need the chunk start location, if they
			 * are writing to a new page.
			 */
			if (bufdata->flags & URS_XLOG_ADD_PAGE)
				chunk_start = bufdata->chunk_header_location;

			/*
			 * If there is an insertion point recorded, it must be restored before
			 * we redo (or skip) the insertion.
			 */
			if (bufdata->flags & URS_XLOG_INSERT)
			{
				if (!record_data)
					elog(ERROR, "undo buf data contained an insert page offset, but no record was passed to UndoReplay()");
				/* Update the insertion point on the page. */
				if (!skip)
					uph->ud_insertion_point = bufdata->insert_page_offset;
				/*
				 * Also update it in shared memory, though this isn't really
				 * necessary as it'll be overwritten after we write data into
				 * the page.
				 */
				slot->meta.insert =
					BLCKSZ * block->blkno + bufdata->insert_page_offset;
			}

			/*
			 * Are we still writing a chunk size that spilled into the next
			 * page?
			 */
			if (chunk_size_more)
			{
				if (skip)
				{
					chunk_size_offset += UndoPageSkipOverwrite(SizeOfUndoPageHeaderData,
															  chunk_size_offset,
															  sizeof(chunk_size));
				}
				else
				{
					chunk_size_offset += UndoPageOverwrite(page,
														   SizeOfUndoPageHeaderData,
														   chunk_size_offset,
														   sizeof(chunk_size),
														   (char *) &chunk_size);
					MarkBufferDirty(buffers[nbuffers].buffer);
				}
				Assert(chunk_size_offset == sizeof(chunk_size));
				chunk_size_more = false;
			}

			/* Are we still writing a header that spilled into the next page? */
			else if (header_more)
			{
				if (skip)
					header_offset += UndoPageSkipHeader(SizeOfUndoPageHeaderData,
														header_offset,
														type_header_size);
				else
				{
					header_offset += UndoPageInsertHeader(page,
														  SizeOfUndoPageHeaderData,
														  header_offset,
														  &chunk_header,
														  type_header_size,
														  type_header,
														  bufdata->chunk_header_location);
					MarkBufferDirty(buffers[nbuffers].buffer);
				}
				/* The shared memory insertion point must be after this fragment. */
				/* TODO: consolidate the places we maintain meta.insert, fix the locking, and update shm just once at the end of the WAL record */
				slot->meta.insert = BLCKSZ * block->blkno + uph->ud_insertion_point;
				/* Do we need to go around again, on the next page? */
				if (header_offset < SizeOfUndoRecordSetChunkHeader + type_header_size)
				{
					nbuffers++;
					continue;
				}

				/* We have finished writing the header. */
				header_more = false;
			}


			/* Are we still writing a record that spilled into the next page? */
			else if (record_more)
			{
				if (skip)
					record_offset += UndoPageSkipRecord(SizeOfUndoPageHeaderData,
														record_offset,
														record_size);
				else
				{
					record_offset += UndoPageInsertRecord(page,
														  SizeOfUndoPageHeaderData,
														  record_offset,
														  record_size,
														  record_data,
														  bufdata->chunk_header_location,
														  bufdata->urs_type);
					MarkBufferDirty(buffers[nbuffers].buffer);
				}

				/* The shared memory insertion point must be after this fragment. */
				slot->meta.insert = BLCKSZ * block->blkno + uph->ud_insertion_point;

				/* Do we need to go around again, on the next page? */
				if (record_offset < record_size)
				{
					nbuffers++;
					continue;
				}

				/* We have finished writing the record.*/
				record_more = false;
				record_data = NULL;
			}

			/* Check if we need to write a chunk header. */
			if (bufdata->flags & URS_XLOG_CREATE)
			{
				if (skip)
				{
					header_offset += UndoPageSkipHeader(SizeOfUndoPageHeaderData,
														header_offset,
														type_header_size);
				}
				else
				{
					chunk_header.size = 0;
					chunk_header.previous_chunk = InvalidUndoRecPtr;
					chunk_header.type = bufdata->urs_type;

					type_header = bufdata->type_header;
					type_header_size = bufdata->type_header_size;
					header_offset = UndoPageInsertHeader(page,
														 uph->ud_insertion_point,
														 0,
														 &chunk_header,
														 type_header_size,
														 type_header,
														 chunk_start);
				}

				/* Do we need to go around again, on the next page? */
				if (header_offset < SizeOfUndoRecordSetChunkHeader + type_header_size)
				{
					header_more = true;
					nbuffers++;
					continue;
				}
			}

			/* Check if we need to create a new chunk for an existing URS. */
			if (bufdata->flags & URS_XLOG_ADD_CHUNK)
			{
				/* Can only be creating one chunk per WAL record. */
				Assert(!(bufdata->flags & URS_XLOG_CREATE));

				if (skip)
				{
					header_offset += UndoPageSkipHeader(SizeOfUndoPageHeaderData,
														header_offset,
														type_header_size);
				}
				else
				{
					chunk_header.size = 0;
					chunk_header.previous_chunk = bufdata->previous_chunk_header_location;
					chunk_header.type = bufdata->urs_type;
					type_header = NULL;
					type_header_size = 0;
					header_offset = UndoPageInsertHeader(page,
														 uph->ud_insertion_point,
														 0,
														 &chunk_header,
														 0,
														 NULL,
														 chunk_start);
				}

				if (header_offset < SizeOfUndoRecordSetChunkHeader)
				{
					header_more = true;
					nbuffers++;
					continue;
				}
			}

			/* Check if we need to insert the caller's record data. */
			if (record_data)
			{
				if (skip)
				{
					record_offset += UndoPageSkipRecord(SizeOfUndoPageHeaderData,
														record_offset,
														record_size);
				}
				else
				{
					record_offset = UndoPageInsertRecord(page,
														 uph->ud_insertion_point,
														 0,
														 record_size,
														 record_data,
														 bufdata->chunk_header_location,
														 bufdata->urs_type);
				}

				/* The shared memory insertion point must be after this fragment. */
				slot->meta.insert = BLCKSZ * block->blkno + uph->ud_insertion_point;

				/* Do we need to go around again, on the next page? */
				if (record_offset < record_size)
				{
					record_more = true;
					nbuffers++;
					continue;
				}
				record_data = NULL;
			}

			if (bufdata->flags & URS_XLOG_CLOSE_CHUNK)
			{
				/* Update the chunk header size to mark it closed. */
				chunk_size = bufdata->chunk_size;

				if (skip)
				{
					chunk_size_offset = UndoPageSkipOverwrite(SizeOfUndoPageHeaderData,
															  chunk_size_offset,
															  sizeof(chunk_size));
				}
				else
				{
					chunk_size_offset =
						UndoPageOverwrite(page,
										  bufdata->chunk_size_page_offset,
										  0,
										  sizeof(bufdata->chunk_size),
										  (char *) &chunk_size);
				}

				/*
				 * If we closed an UndoRecordSet of type URST_TRANSACTION,
				 * we need to let xactundo.c know about the state change.
				 */
				if (bufdata->urs_type == URST_TRANSACTION &&
					(bufdata->flags & URS_XLOG_CLOSE) != 0)
				{
					UndoRecPtr begin;
					UndoRecPtr end;
					bool	isCommit = false;
					bool	isPrepare = false;
					uint8	info;

					/* Determine which type of record closed the record set. */
					if (XLogRecGetRmid(xlog_record) != RM_XACT_ID)
						elog(ERROR,
							 "transaction undo closed by unexpected rmgr %d",
							 XLogRecGetRmid(xlog_record));
					info = XLogRecGetInfo(xlog_record) & ~XLR_INFO_MASK;

					switch (info & XLOG_XACT_OPMASK)
					{
						case XLOG_XACT_COMMIT:
						case XLOG_XACT_COMMIT_PREPARED:
							isCommit = true;
							break;
						case XLOG_XACT_ABORT:
						case XLOG_XACT_ABORT_PREPARED:
							break;
						case XLOG_XACT_PREPARE:
							isPrepare = true;
							break;
						default:
							elog(ERROR,
								 "transaction undo closed by unexpected record %d",
								info);
					}

					/* Find the chunk start and end. */
					if (bufdata->flags & URS_XLOG_CLOSE_MULTI_CHUNK)
						begin = bufdata->first_chunk_header_location;
					else
						begin = MakeUndoRecPtr(slot->logno,
											   block->blkno * BLCKSZ +
											   bufdata->chunk_size_page_offset);
					end = MakeUndoRecPtr(slot->logno,
										 block->blkno * BLCKSZ +
										 bufdata->chunk_size_page_offset +
										 bufdata->chunk_size);

					/* Now we can report what happened. */
					XactUndoCloseRecordSet(bufdata->type_header, begin, end,
										   isCommit, isPrepare);
				}

				/*
				 * XXX IS it OK that we delivered the callback before writing
				 * the part the spills onto the next page?
				 */
				if (chunk_size_offset < sizeof(chunk_size))
				{
					chunk_size_more = true;
					nbuffers++;
					continue;
				}
			}

			++nbuffers;
		}
	}

	/*
	 * There had better not be any header or record data destined for the next
	 * buffer if we have run out of registered buffers.
	 */
	if (header_more || record_more)
		elog(ERROR, "undo data didn't fit on registered buffers");

	/* Update the page LSNs and release. */
	for (int i = 0; i < nbuffers; ++i)
	{
		Buffer buffer = buffers[i].buffer;

		if (BufferIsValid(buffer))
		{
			MarkBufferDirty(buffer);
			PageSetLSN(BufferGetPage(buffer), xlog_record->ReadRecPtr);
			UnlockReleaseBuffer(buffer);
		}
	}

	pfree(buffers);

	return result;
}

/*
 * Register all undo buffers touched by a single WAL record.  This must be
 * done after an UndoInsert() and any UndoMarkClosed() calls, but before
 * calling XLogInsert().
 *
 * The caller must have called XLogBeginInsert() for a WAL record, and
 * must provide the first block ID to use, to avoid collisions with any
 * other block IDs registered by the caller.
 */
void
UndoXLogRegisterBuffers(UndoRecordSet *urs, uint8 first_block_id)
{
	if (!URSNeedsWAL(urs))
		return;

	for (int i = 0; i < urs->nbuffers; ++i)
	{
		UndoBuffer *ubuf = &urs->buffers[i];

		if (URSNeedsWAL(urs))
		{
			XLogRegisterBuffer(first_block_id + i,
							   ubuf->buffer,
							   (ubuf->is_new ? REGBUF_WILL_INIT : 0) |
							   REGBUF_KEEP_DATA);
			if (ubuf->bufdata.flags != 0)
				EncodeUndoRecordSetXLogBufData(&ubuf->bufdata,
											   first_block_id + i);
		}
	}
}

/*
 * Set page LSNs for buffers dirtied by UndoInsert or UndoMarkClosed.
 */
void
UndoPageSetLSN(UndoRecordSet *urs, UndoRecPtr lsn)
{
	for (int i = 0; i < urs->nbuffers; ++i)
		PageSetLSN(BufferGetPage(urs->buffers[i].buffer), lsn);
}

/*
 * Release buffer locks and pins held by an UndoRecordSet.
 */
void
UndoRelease(UndoRecordSet *urs)
{
	for (int i = 0; i < urs->nbuffers; ++i)
		UnlockReleaseBuffer(urs->buffers[i].buffer);
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
 * Register XLog buffers for all UndoRecordSets for this transaction level.
 *
 * This should be called from within the critical section, during WAL record
 * construction.
 */
void
UndoXLogRegisterBuffersForXactLevel(int nestingLevel, uint8 first_block_id)
{
	slist_iter	iter;

	slist_foreach(iter, &UndoRecordSetList)
	{
		UndoRecordSet *urs = slist_container(UndoRecordSet, link, iter.cur);

		if (nestingLevel <= urs->nestingLevel &&
			urs->state != URS_STATE_CLEAN) /* TODO: can we get rid of the state test here? */
			UndoXLogRegisterBuffers(urs, first_block_id);
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
		char dummy[24] = { '\0' };

		START_CRIT_SECTION();
		XLogBeginInsert();
		UndoMarkClosedForXactLevel(nestingLevel);
		UndoXLogRegisterBuffersForXactLevel(nestingLevel, 0);
		XLogRegisterData(dummy, 24); /* TODO remove me */
		lsn = XLogInsert(RM_XLOG_ID, XLOG_NOOP);
		UndoPageSetLSNForXactLevel(nestingLevel, lsn);
		END_CRIT_SECTION();
	}

	UndoDestroyForXactLevel(nestingLevel);

	return needs_work;
}

/*
 * Find the start of the final chunk by examining a page that is known to be
 * the final page in an undo log (ie holding the byte that precedes the
 * insertion point).
 */
static UndoRecPtr
find_start_of_chunk_on_final_page(Page page, UndoRecPtr page_begin_urp)
{
	UndoPageHeader page_header = (UndoPageHeader) page;
	UndoLogOffset size;

	/*
	 * We'll access the initial size member of chunk headers directly, so
	 * let's assert that the layout is as this code expects.
	 */
	Assert(offsetof(UndoRecordSetChunkHeader, size) == 0);
	Assert(sizeof(((UndoRecordSetChunkHeader *) 0)->size) == sizeof(size));

	/* Search for the start of the final chunk on this page. */
	if (page_header->ud_first_chunk > 0)
	{
		uint16 page_offset = page_header->ud_first_chunk;

		/* Walk forwards until we find the last chunk on the page. */
		for (;;)
		{
			UndoLogOffset size;

			/*
			 * The size must be entirely on this page, or this wouldn't be
			 * the last page in the log.
			 */
			if (page_offset > BLCKSZ - sizeof(size))
				elog(ERROR, "unexpectedly ran out of undo page while reading chunk size");

			/* Read the aligned value. */
			memcpy(&size, page + page_offset, sizeof(size));

			/*
			 * The chunk can't spill onto the next page, or this wouldn't
			 * be the last page in the log.
			 */
			if (page_offset + size > BLCKSZ)
				elog(ERROR, "unexpectedly ran out of undo page while following chunks");

			/* The chunk can't extend past the insertion point. */
			if (page_offset + size > page_header->ud_insertion_point)
				elog(ERROR, "undo chunk exceeded expected range");

			/*
			 * The last chunk is the one that either hits the insertion point
			 * or is has size zero (unclosed).
			 */
			if (size == 0 || page_offset + size == page_header->ud_insertion_point)
				return page_begin_urp + page_offset;

			/* Keep walking. */
			page_offset += size;
		}
		return InvalidUndoRecPtr;		/* unreachable */
	}
	else
	{
		/*
		 * If no chunks have been started on the page, then the start of
		 * the chunk that spilled into this page is directly available
		 * from the header.
		 */
		return page_header->ud_continue_chunk;
	}
}

/*
 * Given the current insert pointer for an undo log, find the header for the
 * last chunk.  That is, the chunk that contains the byte that immediately
 * precedes the insert pointer.  The discard pointer must be before the insert
 * pointer (ie there must actually be an undiscared byte immediately preceding
 * the insert pointer).
 */
static UndoRecPtr
find_start_of_final_chunk_in_undo_log(UndoLogNumber logno, UndoLogOffset insert)
{
	RelFileNode rnode;
	UndoLogOffset last_data_offset;
	UndoRecPtr final_page_urp;
	UndoRecPtr result;
	BlockNumber chunk_last_blockno;
	Buffer buffer;

	/*
	 * Locate the page holding the byte preceding the insert point, skipping
	 * over the page header if necessary, because that's the last page that
	 * had anything written to it and thus that has the page header
	 * information we need to find our way.
	 */
	last_data_offset = insert - 1;
	if (last_data_offset % BLCKSZ < SizeOfUndoPageHeaderData)
		last_data_offset -= SizeOfUndoPageHeaderData;

	/* Read the last chunk location from the last page's header. */
	UndoRecPtrAssignRelFileNode(rnode, MakeUndoRecPtr(logno, last_data_offset));
	chunk_last_blockno = last_data_offset / BLCKSZ;
	buffer = ReadBufferWithoutRelcache(rnode,
									   MAIN_FORKNUM,
									   chunk_last_blockno,
									   RBM_NORMAL,
									   NULL,
									   RELPERSISTENCE_PERMANENT);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);

	/* Find the start of the final chunk by examining this page. */
	final_page_urp = MakeUndoRecPtr(logno, BLCKSZ * chunk_last_blockno);
	result = find_start_of_chunk_on_final_page(BufferGetPage(buffer),
											   final_page_urp);
	UnlockReleaseBuffer(buffer);

	return result;
}

/*
 * Read data at a given location, reading and locking buffers as required.
 * This is a helper function for CloseDanglingUndoRecordSets().
 */
static void
read_undo_header(char *out, size_t size, UndoRecPtr urp,
				 Buffer *buffers, int nbuffers)
{
	Buffer buffer;
	RelFileNode rnode;
	BlockNumber blockno;
	uint16 page_offset;
	uint16 bytes_on_this_page;
	size_t bytes_copied;
	int buffer_index = 0;

	UndoRecPtrAssignRelFileNode(rnode, urp);
	blockno = UndoRecPtrGetOffset(urp) / BLCKSZ;
	page_offset = urp % BLCKSZ;
	bytes_copied = 0;

	do
	{
		if (buffer_index >= nbuffers)
			elog(ERROR, "cannot read undo data: not enough buffers");
		buffer = ReadBufferWithoutRelcache(rnode,
										   MAIN_FORKNUM,
										   blockno,
										   RBM_NORMAL,
										   NULL,
										   RELPERSISTENCE_PERMANENT);
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
		buffers[buffer_index] = buffer;
		bytes_on_this_page = Min(size - bytes_copied, BLCKSZ - page_offset);
		memcpy((char *) out + bytes_copied,
			   BufferGetPage(buffer) + page_offset,
			   bytes_on_this_page);
		bytes_copied += bytes_on_this_page;
		blockno++;
		buffer_index++;
		page_offset = SizeOfUndoPageHeaderData;
	}
	while (bytes_copied < size);
}

static void
release_buffers(Buffer *buffers, int nbuffers)
{
	for (int i = 0; i < nbuffers; ++i)
	{
		if (buffers[i] != InvalidBuffer)
		{
			UnlockReleaseBuffer(buffers[i]);
			buffers[i] = InvalidBuffer;
		}
	}
}

/*
 * Scan the set of existing undo logs looking for URS chunks that are not
 * closed (ie that have a zero length header).  This is done to discover URSs
 * that were open at the time of a crash, at startup.  We'll set the chunk
 * length so that we know how to discard it, and we'll call the URS
 * type-specific callback to tell it we're closing one of its URSs that was
 * found to be dangling after a crash.
 */
void
CloseDanglingUndoRecordSets(void)
{
	UndoLogSlot *slot = NULL;

	while ((slot = UndoLogGetNextSlot(slot)))
	{
		UndoLogNumber logno = slot->logno;
		UndoLogOffset discard = slot->meta.discard;
		UndoLogOffset insert = slot->meta.insert;
		UndoRecPtr chunk_header_location;
		UndoRecPtr begin, end;
		UndoRecordSetChunkHeader chunk_header;
		UndoRecordSetType type;
		Buffer buffers[2];
		void *type_header;
		size_t type_header_size;
		size_t chunk_size;
		uint16 page_offset;
		uint16 bytes_on_first_page;
		UndoRecordSetXLogBufData bufdata = {0};
		char dummy[24] = {0};
		XLogRecPtr lsn;

		/* If the undo is empty, skip. */
		if (insert == discard)
			continue;

		/* Locate the header of the final chunk. */
		Assert(discard < insert);
		chunk_header_location =
			find_start_of_final_chunk_in_undo_log(logno, insert);

		for (int i = 0; i < lengthof(buffers); ++i)
			buffers[i] = InvalidBuffer;

		/* Read the chunk header. */
		read_undo_header((void *) &chunk_header, SizeOfUndoRecordSetChunkHeader,
						 chunk_header_location,
						 buffers, lengthof(buffers));
		release_buffers(buffers, lengthof(buffers));

		/*
		 * We already released the buffer(s) because it makes the later code
		 * a bit simpler, and it's also the expected outcome that we won't
		 * need them.  A non-zero size indicates that there is nothing to be
		 * done here, the chunk was already closed and we can move onto the
		 * next undo log.
		 */
		if (chunk_header.size > 0)
			continue;

		/* Compute the missing chunk size. */
		Assert(insert > UndoRecPtrGetOffset(chunk_header_location));
		chunk_size = insert - UndoRecPtrGetOffset(chunk_header_location);

		/*
		 * In order to log it, we need to get our hands on the location of the
		 * first chunk in this URS, and the type-specific header that follows
		 * its header.  Walk back to the start of the chain, if we aren't
		 * already there.
		 *
		 * XXX Be smarter about not releasing and reacquiring the buffers?
		 */
		begin = chunk_header_location;
		end = chunk_header_location + chunk_size;
		while (chunk_header.previous_chunk != InvalidUndoRecPtr)
		{
			begin = chunk_header.previous_chunk;
			/* XXX think harder about this case --- just skip it? */
			if (UndoRecPtrIsDiscarded(begin))
				elog(PANIC, "found partially discarded unclosed undo record set");
			read_undo_header((void *) &chunk_header, SizeOfUndoRecordSetChunkHeader,
							 begin,
							 buffers, lengthof(buffers));
			release_buffers(buffers, lengthof(buffers));
		}
		type = chunk_header.type;
		type_header_size = get_type_header_size(type);
		type_header = palloc(type_header_size);
		read_undo_header(type_header, type_header_size,
						 UndoRecPtrPlusUsableBytes(begin, SizeOfUndoRecordSetChunkHeader),
						 buffers, lengthof(buffers));
		release_buffers(buffers, lengthof(buffers));

		/* Prepare to write the final chunk's missing size. */
		read_undo_header((void *) &chunk_header, SizeOfUndoRecordSetChunkHeader,
						 chunk_header_location,
						 buffers, lengthof(buffers));
		page_offset = chunk_header_location % BLCKSZ;
		bytes_on_first_page = Min(BLCKSZ - page_offset, sizeof(chunk_size));

		/* Write the final chunk's missing size and log it. */
		START_CRIT_SECTION();
		XLogBeginInsert();
		UndoPageOverwrite(BufferGetPage(buffers[0]),
						  page_offset,
						  0,
						  sizeof(chunk_size),
						  (char *) &chunk_size);
		MarkBufferDirty(buffers[0]);
		XLogRegisterBuffer(0, buffers[0], REGBUF_KEEP_DATA);
		/* We're closing a chunk and a URS. */
		bufdata.flags = URS_XLOG_CLOSE_CHUNK | URS_XLOG_CLOSE;
		bufdata.chunk_size_page_offset = page_offset;
		bufdata.chunk_size = chunk_size;
		if (begin != chunk_header_location)
		{
			/*
			 * We only waste space on first_chunk_header_location if it's not
			 * derivable from chunk_size_page_offset.  This is needed so that
			 * we can call report_closed_urs() in recovery.
			 */
			bufdata.flags |= URS_XLOG_CLOSE_MULTI_CHUNK;
			bufdata.first_chunk_header_location = begin;
		}
		/*
		 * We log a copy of the type-specific header to pass to
		 * report_closed_urs() in recovery, because it can't safely read it
		 * from the URS's first chunk header.
		 */
		bufdata.urs_type = type;
		bufdata.type_header = type_header;
		bufdata.type_header_size = type_header_size;
		EncodeUndoRecordSetXLogBufData(&bufdata, 0);
		if (bytes_on_first_page < sizeof(chunk_size))
		{
			UndoPageOverwrite(BufferGetPage(buffers[1]),
							  SizeOfUndoPageHeaderData,
							  bytes_on_first_page,
							  sizeof(chunk_size),
							  (char *) &chunk_size);
			MarkBufferDirty(buffers[1]);
			XLogRegisterBuffer(1, buffers[1], REGBUF_KEEP_DATA);
		}
		XLogRegisterData(dummy, 24); /* TODO remove me */
		lsn = XLogInsert(RM_XLOG_ID, XLOG_NOOP); /* new record id? */
		PageSetLSN(BufferGetPage(buffers[0]), lsn);
		if (buffers[1] != InvalidBuffer)
			PageSetLSN(BufferGetPage(buffers[1]), lsn);
		END_CRIT_SECTION();

		release_buffers(buffers, lengthof(buffers));

		/*
		 * If this is a transaction, also inform xactundo.c so that the
		 * UndoRequest state gets updated.
		 */
		if (chunk_header.type == URST_TRANSACTION)
			XactUndoCloseRecordSet(type_header, begin, end, false, false);

		pfree(type_header);
	}
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
