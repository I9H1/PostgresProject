#include "postgres.h"
#include "utils/memutils_internal.h"
#include "storage/shmem.h"
#include "storage/shared_mapping.h"
#include "nodes/memnodes.h"
#include "storage/lwlock.h"
#include "storage/condition_variable.h"
#include "storage/dsm.h"
#include "storage/procsignal.h"
#include "utils/wait_event_types.h"
#include "miscadmin.h"
#include "sys/mman.h"

#define SHM_RESERVED_START 0x00007F8000000000UL
#define SHM_RESERVED_SIZE  (128ULL * 1024 * 1024 * 1024) /* 128 Gb */
#define SHM_BLOCK_SIZE (64UL * 1024 * 1024) /* 64 Mb */
#define SHM_MAX_BLOCKS (SHM_RESERVED_SIZE / SHM_BLOCK_SIZE)

extern int shmem_context_memory_size_kb;

typedef struct ShmemContextSet ShmemContextSet;
typedef struct ShmemContextControl ShmemContextControl;
typedef struct ShmemChunkHeader ShmemChunkHeader;
typedef struct ShmemBlockInfo ShmemBlockInfo;

typedef struct ShmemContextSet {
    MemoryContextData header;
} ShmemContextSet;

typedef struct ShmemChunkHeader {
    ShmemContextSet *context;
    Size size;
    bool is_free;
    ShmemChunkHeader *next;
    ShmemChunkHeader *prev;
    uint64 method_id;
} ShmemChunkHeader;

typedef struct ShmemBlockInfo
{
    Size size;
    ShmemChunkHeader *first_chunk;
    dsm_handle handle;
} ShmemBlockInfo;

typedef struct ShmemContextControl {
    uint64 magic;
    LWLock lwLock;
    bool extend_in_progress;
    ConditionVariable extendCV;
    void *user_data;
    
    Size total_allocated;
    Size total_used;
    Size total_free;
    Size total_metadata;

    int num_blocks;
    ShmemBlockInfo blocks[SHM_MAX_BLOCKS];

    void *next_reserved_addr;
} ShmemContextControl;

static ShmemContextControl *ctl;

MemoryContext ShmemContextCreate(MemoryContext parent, const char *name);
void *ShmemContextAlloc(MemoryContext context, Size size, int flags);
void ShmemContextFree(void *pointer);
void *ShmemContextRealloc(void *pointer, Size size, int flags);
void ShmemContextReset(MemoryContext context);
void ShmemContextDelete(MemoryContext context);
MemoryContext ShmemContextGetChunkContext(void *pointer);
Size ShmemContextGetChunkSpace(void *pointer);
bool ShmemContextIsEmpty(MemoryContext context);
void ShmemContextCheck(MemoryContext context);

Size ShmemContextGetShmemSize(void);
void ShmemContextInit(void);
MemoryContext ShmemGetRootContext(void);
void *ShmemGetOrCreateUserData(Size size);
bool StartShmemContext(void);
static bool IsContextShared(MemoryContext context);
static ShmemBlockInfo *AddNewBlock(void);
static ShmemChunkHeader *FindFreeChunk(Size size);
static ShmemChunkHeader *ClaimFreeChunk(Size size, MemoryContext context);
static void StartExtend(void);
static void FinishExtend(void);

Size 
ShmemContextGetShmemSize(void)
{
    Size size = 0;
    
    size = add_size(size, MAXALIGN(sizeof(ShmemContextControl)));
    size = add_size(size, MAXALIGN(sizeof(SharedMappingControl)));
    size = add_size(size, MAXALIGN(sizeof(ShmemContextSet)));
    size = add_size(size, MAXALIGN(sizeof(ShmemChunkHeader)));
    size = add_size(size, MAXALIGN((Size)shmem_context_memory_size_kb) * 1024);

    return size;
}

void 
ShmemContextInit(void)
{
    bool found;
    Size total_shmem_size;
    ShmemBlockInfo *first_block;
    ShmemContextSet *root_context;
    ShmemChunkHeader *first_chunck;
    void *reserved;

    total_shmem_size = ShmemContextGetShmemSize();
    
    ctl = (ShmemContextControl *) ShmemInitStruct("shmem_context", total_shmem_size, &found);

    elog(LOG, "Size of memory requested for Shmem context: %lu", total_shmem_size);

    elog(LOG, "Ctl at address %p size %lu", ctl, MAXALIGN(sizeof(ShmemContextControl)));

    if (!found)
    {
        /* Initialization of sharedMappingControl */
        sharedMappingControl = (SharedMappingControl *) ((char *)ctl + MAXALIGN(sizeof(ShmemContextControl)));
        elog(LOG, "MappingCtl at address %p size %lu", sharedMappingControl, MAXALIGN(sizeof(SharedMappingControl)));

        LWLockInitialize(&sharedMappingControl->lwlock, LW_EXCLUSIVE);
        sharedMappingControl->address = NULL;
        pg_atomic_init_u32(&sharedMappingControl->failed, 0);
        sharedMappingControl->generation = 0;
        sharedMappingControl->handle = DSM_HANDLE_INVALID;
        elog(LOG, "SharedMappingControl initialized");

        /* Initialization of root context, which will be an ancestor to all shmem contexts */
        root_context = (ShmemContextSet *) ((char *) sharedMappingControl + MAXALIGN(sizeof(SharedMappingControl)));
        memset(root_context, 0, sizeof(ShmemContextSet));

        /* For now we use a foreign node, custom node is to be implemented */
        MemoryContextCreate((MemoryContext) root_context,
                            T_AllocSetContext,
                            MCTX_SHMEM_ID,
                            NULL,
                            "root_shmem_context");

        elog(LOG, "Root_context at address %p size %lu", root_context, MAXALIGN(sizeof(ShmemContextSet)));

        /* Initialization of first block */
        first_block = &ctl->blocks[0];
        memset(first_block, 0, sizeof(ShmemBlockInfo));
        first_block->size = total_shmem_size 
                    - MAXALIGN(sizeof(ShmemContextControl))
                    - MAXALIGN(sizeof(SharedMappingControl))
                    - MAXALIGN(sizeof(ShmemContextSet));
        first_block->handle = DSM_HANDLE_INVALID;

        elog(LOG, "First blockInfo at address %p size %lu", first_block, MAXALIGN(sizeof(ShmemBlockInfo)));

        /* Initialization of first_chunck */
        first_chunck = (ShmemChunkHeader *) ((char *) root_context + MAXALIGN(sizeof(ShmemContextSet)));
        memset(first_chunck, 0, sizeof(ShmemChunkHeader));
        first_chunck->context = root_context;
        first_chunck->is_free = true;
        first_chunck->size = first_block->size - MAXALIGN(sizeof(ShmemChunkHeader));
        first_chunck->next = NULL;
        first_chunck->prev = NULL;
        first_chunck->method_id = MCTX_SHMEM_ID;

        first_block->first_chunk = first_chunck;

        elog(LOG, "First chunk header at address %p size %lu", first_chunck, MAXALIGN(sizeof(ShmemChunkHeader)));
        elog(LOG, "First chunk space at address %p size %lu", (char* )first_chunck + MAXALIGN(sizeof(ShmemChunkHeader)), first_chunck->size);

        /* Initialization of control block */

        /* Reserve huge region of virtual memory for new blocks */
        reserved = mmap((void *)SHM_RESERVED_START, SHM_RESERVED_SIZE,
                      PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                      -1, 0);

        if (reserved == MAP_FAILED)
        {
            elog(ERROR, "Failed to reserve virtual memory at %p: %m", 
            (void *)SHM_RESERVED_START);
            ctl->next_reserved_addr = NULL;
        }
        else
        {
            ctl->next_reserved_addr = (void *)SHM_RESERVED_START;
        }

        ctl->total_allocated = total_shmem_size;
        ctl->total_free = first_block->size;
        ctl->total_used = 0;
        ctl->total_metadata = total_shmem_size - first_block->size; 

        ctl->magic = 0xDEADBEEF12345678ULL;
        ctl->user_data = NULL;
        ctl->num_blocks = 1;
        LWLockInitialize(&ctl->lwLock, LW_EXCLUSIVE);
        ConditionVariableInit(&ctl->extendCV);
    }
    else
    {
        Assert(ctl->magic == 0xDEADBEEF12345678ULL);
    }
}

MemoryContext
ShmemContextCreate(MemoryContext parent, const char *name)
{
    ShmemContextSet *context;

    if (parent != NULL)
    {
        /* Check if parent context is within our shared memory block */
        if (!IsContextShared(parent))
        {
            ereport(ERROR,
                    (errcode(ERRCODE_INTERNAL_ERROR),
                    errmsg("parent context must be a shared memory context")));
        }
    }

    context = (ShmemContextSet *) MemoryContextAllocZero(parent, sizeof(ShmemContextSet));

    elog(LOG, "context at %p, size of ShmemContextSet = %lu", context, sizeof(ShmemContextSet));
    elog(LOG, "Chunk header before context at %p", (char*)context - MAXALIGN(sizeof(ShmemChunkHeader)));

    LWLockAcquire(&ctl->lwLock, LW_EXCLUSIVE);

    /* Using existing T_Node for now */
    MemoryContextCreate((MemoryContext) context,
                        T_AllocSetContext,
                        MCTX_SHMEM_ID,
                        parent,
                        name);

    LWLockRelease(&ctl->lwLock);

    elog(LOG, "Child context was created");

    return (MemoryContext) context;
}

static bool 
IsContextShared(MemoryContext context)
{
    ShmemBlockInfo *block;
    int n;

    LWLockAcquire(&ctl->lwLock, LW_SHARED);
    n = ctl->num_blocks;
    LWLockRelease(&ctl->lwLock);

    for (int i = 0; i < n; ++i)
    {
        block = &ctl->blocks[i];
        if (context == ShmemGetRootContext())
            return true;

        if ((char *) context >= (char *) block->first_chunk + MAXALIGN(sizeof(ShmemChunkHeader)) &&
                (char *) context <= (char *) block->first_chunk 
                + block->size - MAXALIGN(sizeof(ShmemContextSet)))
            return true;
    }

    return false;
}

void *
ShmemGetOrCreateUserData(Size size)
{
    if (ctl->user_data == NULL) {
        MemoryContext root = ShmemGetRootContext();
        MemoryContext old = MemoryContextSwitchTo(root);
        ctl->user_data = palloc0(size);
        MemoryContextSwitchTo(old);
    }
    return ctl->user_data;
}

/*
 * Attach all additional block segments. Should be called by process 
 * at the start of working with ShmemContext. It is nessessary since
 * new processes don't inherit these mappings by default.
 */
bool
StartShmemContext(void)
{
    ShmemBlockInfo *block;
    dsm_segment *segment;
    dsm_handle handle;
    void *addr;
    int n;

    if (ctl == NULL || ctl->num_blocks == 0)
        return false;

    LWLockAcquire(&ctl->lwLock, LW_SHARED);
    n = ctl->num_blocks;
    LWLockRelease(&ctl->lwLock);

    for (int i = 1; i < n; ++i)
    {
        elog(LOG, "maping block %d at start", i);
        block = &ctl->blocks[i];
        handle = block->handle;
        addr = (char *) block->first_chunk;

        elog(LOG, "Attaching %p", addr);
        if (handle == DSM_HANDLE_INVALID || addr == NULL)
        {
            elog(WARNING, "Failed to StartContext: invalid block params");
            return false;
        }

        segment = dsm_attach_at(handle, addr, true);
        if (segment == NULL)
        {
            elog(WARNING, "Backend %d: failed to map segment %u at %p",
                 MyProcPid, handle, addr);
            return false;
        }

        if (dsm_segment_address(segment) != addr)
        {
            elog(WARNING, "Backend %d: wrong mapping", MyProcPid);
            return false;
        }

        dsm_pin_mapping(segment);
    }

    elog(LOG, "BLOCKES MAPPED");

    return true;
}

void *
ShmemContextAlloc(MemoryContext context, Size size, int flags)
{
    ShmemChunkHeader *chunk = NULL;
    ShmemBlockInfo *new_block;
    void *result;

    if (size == 0)
        return NULL;

    size = MAXALIGN(size);

    /* Find and claim free chunk with required size */
    chunk = ClaimFreeChunk(size, context);

    if (chunk == NULL)
    {
        elog(LOG, "No free chunk found, trying to add new block");

        StartExtend();

        /* Check if another process added new block while we were waiting */
        chunk = ClaimFreeChunk(size, context);

        if (chunk == NULL)
        {
            new_block = AddNewBlock();
            if (new_block == NULL)
            {
                elog(WARNING, "Failed to add new block for allocation of size %lu", size);
                FinishExtend();
                return NULL;
            }

            chunk = ClaimFreeChunk(size, context);
            if (chunk == NULL) 
            {
                elog(WARNING, "internal error: freshly added block has no free space");
                FinishExtend();
                return NULL;
            }
        }
        
        FinishExtend();
    }

    result = (void *) ((char *) chunk + MAXALIGN(sizeof(ShmemChunkHeader)));

    elog(LOG, "Chunk header at address %p size %lu", chunk, MAXALIGN(sizeof(ShmemChunkHeader)));
    elog(LOG, "Allocated space at address %p size %lu", result, size);

    return result;
}

static void
StartExtend(void)
{
    for (;;)
    {
        LWLockAcquire(&ctl->lwLock, LW_EXCLUSIVE);

        if (!ctl->extend_in_progress)
        {
            ctl->extend_in_progress = true;
            LWLockRelease(&ctl->lwLock);
            return;
        }

        LWLockRelease(&ctl->lwLock);

        ConditionVariableSleep(&ctl->extendCV, WAIT_EVENT_SHMEM_EXTENSION);
    }

    ConditionVariableCancelSleep();
}

static void
FinishExtend(void)
{
    LWLockAcquire(&ctl->lwLock, LW_EXCLUSIVE);
    ctl->extend_in_progress = false;
    LWLockRelease(&ctl->lwLock);

    ConditionVariableBroadcast(&ctl->extendCV);
}

static ShmemChunkHeader *
FindFreeChunk(Size size)
{
    ShmemBlockInfo *block;
    ShmemChunkHeader *chunk;

    for (int i = 0; i < ctl->num_blocks; ++i) 
    {
        block = &ctl->blocks[i];
        chunk = block->first_chunk;
        while (chunk != NULL)
        {
            if (chunk->is_free && chunk->size >= size)
            {
                return chunk;
            }
            chunk = chunk->next;
        }
    }

    return NULL;
}

static ShmemChunkHeader *
ClaimFreeChunk(Size size, MemoryContext context)
{
    ShmemChunkHeader *chunk;

    LWLockAcquire(&ctl->lwLock, LW_EXCLUSIVE);

    chunk = FindFreeChunk(size);
    if (chunk == NULL)
    {
        LWLockRelease(&ctl->lwLock);
        return NULL;
    }

    /* If size of potential new chunk >= size of header, split chunk into two */
    if (chunk->size - size >= 2 * MAXALIGN(sizeof(ShmemChunkHeader)))
    {
        ShmemChunkHeader *new_chunk = (ShmemChunkHeader *) ((char *) chunk + size + 
                                                MAXALIGN(sizeof(ShmemChunkHeader)));
        new_chunk->is_free = true;
        new_chunk->next = chunk->next;
        new_chunk->size = chunk->size - size - MAXALIGN(sizeof(ShmemChunkHeader));
        new_chunk->prev = chunk;
        new_chunk->method_id = MCTX_SHMEM_ID;
        if (new_chunk->next != NULL)
            new_chunk->next->prev = new_chunk;
        chunk->next = new_chunk;
        chunk->size = size;
        ctl->total_free -= MAXALIGN(sizeof(ShmemChunkHeader));
        ctl->total_metadata += MAXALIGN(sizeof(ShmemChunkHeader));
    }

    chunk->is_free = false;
    chunk->context = (ShmemContextSet *) context;
    chunk->method_id = MCTX_SHMEM_ID;

    ctl->total_used += chunk->size;
    ctl->total_free -= chunk->size;

    LWLockRelease(&ctl->lwLock);
    return chunk;
}

static ShmemBlockInfo *
AddNewBlock(void)
{
    ShmemBlockInfo *new_block;
    ShmemChunkHeader *new_chunk;
    void *addr = NULL;
    Size block_size = SHM_BLOCK_SIZE;
    uint64 generation;
    int failed_count;
    dsm_segment *segment = NULL;
    dsm_handle handle = DSM_HANDLE_INVALID;

    elog(LOG, "AddNewBlock is called");

    Assert(IsUnderPostmaster);

    if (ctl->num_blocks >= SHM_MAX_BLOCKS)
    {
        elog(WARNING, "Amount of additional blocks is eshausted");
        return NULL;
    }

    /* Get virtual address for new block */
    if (ctl->next_reserved_addr != NULL)
    {
        addr = ctl->next_reserved_addr;
        ctl->next_reserved_addr = (char *) addr + block_size;
    } 
    else 
    {
        elog(WARNING, "No reserved space for new blocks is available");
        return NULL;
    }

    if (addr != (void *) ((uintptr_t) addr & ~4095))
    {
        elog(WARNING, "Block address is not aligned");
        return NULL;
    }

    /* Check if we are out of reserved space */
    if ((char *) ctl->next_reserved_addr > (char *) SHM_RESERVED_START + SHM_RESERVED_SIZE)
    {
        elog(WARNING, "Reserved space for new blocks is exhausted");
        return NULL;
    }

    /* Creating dsm segment */
    segment = dsm_create(block_size, 0);
    if (segment == NULL)
    {
        elog(WARNING, "Unable to create dsm segment for new block");
        return NULL;
    }

    handle = dsm_segment_handle(segment);
    if (handle == DSM_HANDLE_INVALID)
    {
        elog(WARNING, "Unable to get dsm segment handle");
        dsm_detach(segment);
        return NULL;
    }
    
    /*
     * Unmap segment, since it was created and automatically
     * mapped to a random address
     * But first we pin it so detach does not destroy it
     */
    dsm_pin_segment(segment);
    dsm_detach(segment);
    
    /* Map segment in current process */
    segment = dsm_attach_at(handle, addr, true);

    if (segment == NULL)
    {
        elog(WARNING, "Failed to attach DSM at %p", addr);
        dsm_unpin_segment(handle);
        return NULL;
    }

    if (dsm_segment_address(segment) != addr)
    {
        elog(WARNING, "Mapped at %p but requested %p", 
        dsm_segment_address(segment), addr);
        dsm_detach(segment);
        dsm_unpin_segment(handle);
        return NULL;
    }

    dsm_pin_mapping(segment);

    /* Initialize structures for new block */
    new_block = &ctl->blocks[ctl->num_blocks];
    new_block->size = SHM_BLOCK_SIZE;
    new_block->handle = handle;

    new_chunk = (ShmemChunkHeader *) addr;
    new_chunk->context = (ShmemContextSet *) ShmemGetRootContext();
    new_chunk->size = new_block->size - MAXALIGN(sizeof(ShmemChunkHeader));
    new_chunk->is_free = true;
    new_chunk->method_id = MCTX_SHMEM_ID;
    new_chunk->prev = NULL;
    new_chunk->next = NULL;
    
    new_block->first_chunk = new_chunk;

    elog(LOG, "AddNewBlock: new block at %p, chunk at %p (size %zu)",
         new_block, new_chunk, new_chunk->size);

    /* Publish mapping parameters */
    if (sharedMappingControl == NULL)
    {
        elog(WARNING, "SharedMappingControl was not initialized");
        return NULL;
    }

    LWLockAcquire(&sharedMappingControl->lwlock, LW_EXCLUSIVE);

    sharedMappingControl->address = addr;
    sharedMappingControl->generation++;
    sharedMappingControl->handle = handle;
    pg_atomic_write_u32(&sharedMappingControl->failed, 0);
    sharedMappingControl->coordinator_pid = MyProcPid;

    elog(LOG, "coord. pid: %d", MyProcPid);

    LWLockRelease(&sharedMappingControl->lwlock);

    elog(LOG, "AddNewBlock: published mapping params;");

    /* All processes map */
    generation = EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SHMEM_ATTACH_ALL);
    WaitForProcSignalBarrier(generation);

    failed_count = pg_atomic_read_u32(&sharedMappingControl->failed);
    
    if (failed_count > 0)
    {
        elog(WARNING, "Processes failed to attach segment");
        generation = EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SHMEM_DETACH);
        WaitForProcSignalBarrier(generation);
        return NULL;
    }

    elog(LOG, "Processes mapped successfully");

    /* Add new block to the block list */
    LWLockAcquire(&ctl->lwLock, LW_EXCLUSIVE);
    
    ctl->num_blocks++;
    ctl->total_allocated += new_block->size;
    ctl->total_free += new_chunk->size;
    ctl->total_metadata += MAXALIGN(sizeof(ShmemChunkHeader));

    LWLockRelease(&ctl->lwLock);

    elog(LOG, "Block added successfully");

    return new_block;
}

void
ShmemContextFree(void *pointer)
{
    ShmemChunkHeader *chunk;

    elog(LOG, "Free was called");

    if (pointer == NULL)
        return;

    /* Checkng if pointer is aligned */
    if (((uintptr_t) pointer & (MAXIMUM_ALIGNOF - 1)) != 0)
    {
        elog(ERROR, "invalid unaligned pointer passed to ShmemContextFree");
        return;
    }

    /* Can also add magic number to Header and check it */
    chunk = (ShmemChunkHeader *)((char *) pointer - MAXALIGN(sizeof(ShmemChunkHeader)));

    if (chunk->method_id != MCTX_SHMEM_ID)
    {
        elog(ERROR, "invalid chunk header: method_id mismatch");
        return;
    }

    LWLockAcquire(&ctl->lwLock, LW_EXCLUSIVE);

    if (chunk->is_free) {
        LWLockRelease(&ctl->lwLock);
        elog(ERROR, "double free detected");
    }
    
    chunk->is_free = true;
    ctl->total_used -= chunk->size;
    ctl->total_free += chunk->size;
    
    /* Merging chunks-neighbours if they are free as well */

    if (chunk->next != NULL && chunk->next->is_free)
    {
        chunk->size += chunk->next->size + MAXALIGN(sizeof(ShmemChunkHeader));
        chunk->next = chunk->next->next;
        if (chunk->next != NULL)
            chunk->next->prev = chunk;

        ctl->total_free += MAXALIGN(sizeof(ShmemChunkHeader));
        ctl->total_metadata -= MAXALIGN(sizeof(ShmemChunkHeader));
    }

    if (chunk->prev != NULL && chunk->prev->is_free)
    {
        chunk->prev->size += chunk->size + MAXALIGN(sizeof(ShmemChunkHeader));
        chunk->prev->next = chunk->next;
        if (chunk->next)
            chunk->next->prev = chunk->prev;

        chunk = chunk->prev;

        ctl->total_free += MAXALIGN(sizeof(ShmemChunkHeader));
        ctl->total_metadata -= MAXALIGN(sizeof(ShmemChunkHeader));
    }

    pointer = NULL;
    LWLockRelease(&ctl->lwLock);
}

void *
ShmemContextRealloc(void *pointer, Size size, int flags)
{
    ShmemChunkHeader *chunk;
    ShmemContextSet *set;
    void *result;
    void *src;
    
    elog(LOG, "Realloc was called");

    if (pointer == NULL)
    {
        elog(ERROR, "null pointer passed to ShmemContextRealloc");
        return NULL;
    }

    /* Checkng if pointer is aligned */
    if (((uintptr_t) pointer & (MAXIMUM_ALIGNOF - 1)) != 0)
    {
        elog(ERROR, "invalid unaligned pointer passed to ShmemContextRealloc");
        return NULL;
    }

    if (size == 0) 
        return NULL;

    size = MAXALIGN(size);
    
    chunk = (ShmemChunkHeader *)((char *) pointer - MAXALIGN(sizeof(ShmemChunkHeader)));
    set = chunk->context;

    LWLockAcquire(&ctl->lwLock, LW_EXCLUSIVE);

    if (size <= chunk->size)
    {
        if (chunk->size - size >= 2 * MAXALIGN(sizeof(ShmemChunkHeader)))
        {
            ShmemChunkHeader *new_chunk = (ShmemChunkHeader *) ((char *) chunk + size 
                                                                + MAXALIGN(sizeof(ShmemChunkHeader)));
            new_chunk->is_free = true;
            new_chunk->next = chunk->next;
            new_chunk->size = chunk->size - size - MAXALIGN(sizeof(ShmemChunkHeader));
            new_chunk->prev = chunk;
            new_chunk->method_id = MCTX_SHMEM_ID;
            if (new_chunk->next != NULL) 
                new_chunk->next->prev = new_chunk;

            chunk->next = new_chunk;
            chunk->size = size;

            ctl->total_used -= new_chunk->size;
            ctl->total_free += new_chunk->size;
            ctl->total_used -= MAXALIGN(sizeof(ShmemChunkHeader));
            ctl->total_metadata += MAXALIGN(sizeof(ShmemChunkHeader));
        }

        LWLockRelease(&ctl->lwLock);
        return pointer;
    }

    LWLockRelease(&ctl->lwLock);

    result = ShmemContextAlloc((MemoryContext) set, size, flags);

    if (result == NULL)
    {
        elog(ERROR, "could not allocate enough memory");
        return NULL;
    }

    src = (void *)((char *) chunk + MAXALIGN(sizeof(ShmemChunkHeader)));

    memcpy(result, src, chunk->size);

    ShmemContextFree(pointer);

    return result;
}

void
ShmemContextReset(MemoryContext context)
{
    ShmemContextSet *set = (ShmemContextSet *) context;
    ShmemBlockInfo *block;
    ShmemChunkHeader *chunk;
    elog(LOG, "Reset was called");

    LWLockAcquire(&ctl->lwLock, LW_EXCLUSIVE);

    for (int i = 0; i < ctl->num_blocks; ++i) 
    {
        block = &ctl->blocks[i];
        chunk = block->first_chunk;
        while (chunk != NULL)
        {
            if (chunk->context == set && !chunk->is_free)
            {
                chunk->is_free = true;
                ctl->total_used -= chunk->size;
                ctl->total_free += chunk->size;

                /* Trying to merge with free neighbours */
                if (chunk->next != NULL && chunk->next->is_free)
                {
                    chunk->size += chunk->next->size + MAXALIGN(sizeof(ShmemChunkHeader));
                    chunk->next = chunk->next->next;
                    if (chunk->next != NULL)
                        chunk->next->prev = chunk;

                    ctl->total_free += MAXALIGN(sizeof(ShmemChunkHeader));
                    ctl->total_metadata -= MAXALIGN(sizeof(ShmemChunkHeader));
                }

                if (chunk->prev != NULL && chunk->prev->is_free)
                {
                    chunk->prev->size += chunk->size + MAXALIGN(sizeof(ShmemChunkHeader));
                    chunk->prev->next = chunk->next;
                    if (chunk->next != NULL)
                        chunk->next->prev = chunk->prev;

                    /* Changing ptr because we merged chunks */
                    chunk = chunk->prev;

                    ctl->total_free += MAXALIGN(sizeof(ShmemChunkHeader));
                    ctl->total_metadata -= MAXALIGN(sizeof(ShmemChunkHeader));
                }
            }
            chunk = chunk->next;
        }
    }

    LWLockRelease(&ctl->lwLock);
}

void
ShmemContextDelete(MemoryContext context)
{
    ShmemContextSet *set;

    elog(LOG, "Delete was called");

    ShmemContextReset(context);

    set = (ShmemContextSet *) context;

    if (set != (ShmemContextSet *) ShmemGetRootContext())
        pfree(set);
}

MemoryContext
ShmemGetRootContext(void)
{
    if (ctl == NULL)
        return NULL;

    return (MemoryContext) ((char *) ctl + MAXALIGN(sizeof(ShmemContextControl))
                                         + MAXALIGN(sizeof(SharedMappingControl))); 
}

MemoryContext
ShmemContextGetChunkContext(void *pointer)
{
    ShmemChunkHeader *chunk;

    if (pointer == NULL)
        return NULL;

    /* Checkng if pointer is aligned */
    if (((uintptr_t) pointer & (MAXIMUM_ALIGNOF - 1)) != 0)
    {
        elog(ERROR, "invalid unaligned pointer passed to ShmemContextGetChunkContext");
        return NULL;
    }

    chunk = (ShmemChunkHeader *) ((char *) pointer - MAXALIGN(sizeof(ShmemChunkHeader)));

    return (MemoryContext) chunk->context;
}

Size
ShmemContextGetChunkSpace(void *pointer)
{
    ShmemChunkHeader *chunk;

    if (pointer == NULL)
        return -1;

    /* Checkng if pointer is aligned */
    if (((uintptr_t) pointer & (MAXIMUM_ALIGNOF - 1)) != 0)
    {
        elog(ERROR, "invalid unaligned pointer passed to ShmemContextGetChunkSpace");
        return -1;
    }

    chunk = (ShmemChunkHeader *) ((char *) pointer - MAXALIGN(sizeof(ShmemChunkHeader)));

    return chunk->size;
}

bool
ShmemContextIsEmpty(MemoryContext context)
{
    ShmemContextSet *set;
    ShmemBlockInfo *block;
    ShmemChunkHeader *chunk;

    set = (ShmemContextSet *) context;

    LWLockAcquire(&ctl->lwLock, LW_SHARED);

    for (int i = 0; i < ctl->num_blocks; ++i) {
        block = &ctl->blocks[i];
        chunk = block->first_chunk;
        while (chunk != NULL)
        {
            if (chunk->context == set && !chunk->is_free)
            {
                LWLockRelease(&ctl->lwLock);
                return false;
            }

            chunk = chunk->next;
        }
    }

    LWLockRelease(&ctl->lwLock);

    return true;
}

void
ShmemContextStats(MemoryContext context, MemoryStatsPrintFunc printfunc,
                  void *passthru, MemoryContextCounters *totals,
                  bool print_to_stderr)
{
    ShmemContextSet *set;
    ShmemBlockInfo *block;
    ShmemChunkHeader *chunk;

    Size nblocks = 0;		    /* Total number of malloc blocks */
	Size freechunks = 0;		/* Total number of free chunks */
	Size totalspace = 0;		/* Total bytes requested from malloc */
	Size freespace = 0;		    /* The unused portion of totalspace */

    set = (ShmemContextSet *) context;

    LWLockAcquire(&ctl->lwLock, LW_SHARED);

    for (int i = 0; i < ctl->num_blocks; ++i)
    {
        block = &ctl->blocks[i];
        chunk = block->first_chunk;
        while (chunk != NULL)
        {
            if (chunk->context == set)
            {
                nblocks += 1;
                totalspace += chunk->size;

                if (chunk->is_free)
                {
                    freechunks += 1;
                    freespace += chunk->size;
                }
            }

            chunk = chunk->next;
        }
    }

    LWLockRelease(&ctl->lwLock);

    if (printfunc)
	{
		char stats_string[200];

		snprintf(stats_string, sizeof(stats_string),
				 "%zu total in %zu blocks; %zu free (%zu chunks); %zu used",
				 totalspace, nblocks, freespace, freechunks,
				 totalspace - freespace);
		printfunc(context, passthru, stats_string, print_to_stderr);
	}

    if (totals)
    {
        totals->nblocks += nblocks;
        totals->freechunks += freechunks;
        totals->totalspace += totalspace;
        totals->freespace += freespace;
    }
}

/*
 * ShmemContextCheck
 *		Walk through chunks and check consistency of memory.
 *
 * NOTE: report errors as WARNING, *not* ERROR or FATAL.  Otherwise you'll
 * find yourself in an infinite loop when trouble occurs, because this
 * routine will be entered again when elog cleanup tries to release memory!
 */

void
ShmemContextCheck(MemoryContext context)
{
    ShmemBlockInfo *block;
    ShmemChunkHeader *chunk;

    LWLockAcquire(&ctl->lwLock, LW_SHARED);

    if (ctl->total_allocated != ctl->total_used + ctl->total_free + ctl->total_metadata)
        elog(WARNING, "Sizes of used and free memory are not consistent");

    for (int i = 0; i < ctl->num_blocks; ++i)
    {    
        block = &ctl->blocks[i];
        chunk = block->first_chunk;
        while (chunk != NULL)
        {
            if (chunk->method_id != MCTX_SHMEM_ID)
                elog(WARNING, "problem in shmem context set %s: chunk %p is corrupted",
					    context->name, chunk);

            if (chunk->next && chunk->next->prev != chunk)
                elog(WARNING, "problem in shmem context set %s: chunk list is corrupted at chunk %p ",
			    		 context->name, chunk->next);

            chunk = chunk->next;
        }
    }
    LWLockRelease(&ctl->lwLock);
}
