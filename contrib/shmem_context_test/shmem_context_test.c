/* contrib/shmem_context_test/shmem_context_test.c */

#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "miscadmin.h"
#include "utils/shmem_context.h"
#include "utils/memutils.h"
#include "libpq/auth.h"

PG_MODULE_MAGIC_EXT(
        .name = "shmem_context_test",
        .version = PG_VERSION
);

static ClientAuthentication_hook_type prev_ClientAuthentication_hook = NULL;

static void
my_ClientAuthentication_hook(Port *port, int status)
{
    if (prev_ClientAuthentication_hook)
        prev_ClientAuthentication_hook(port, status);

    if (status == STATUS_OK)
    {
        if (!StartShmemContext())
            ereport(FATAL, (errmsg("failed to attach shared memory blocks")));
    }
}

void
_PG_init(void)
{
    prev_ClientAuthentication_hook = ClientAuthentication_hook;
    ClientAuthentication_hook = my_ClientAuthentication_hook;
}

/* Simple shmem access test */
PG_FUNCTION_INFO_V1(shmem_test);

typedef struct {
    int value;
} SharedInt;

static SharedInt *data = NULL;

Datum
shmem_test(PG_FUNCTION_ARGS)
{
 	MemoryContext old = MemoryContextSwitchTo(ShmemGetRootContext());

    data = (SharedInt *) ShmemGetOrCreateUserData(sizeof(SharedInt));
    
    data->value++;
    
    elog(LOG, "Value: %d (pid: %d, address: %p)", 
         data->value, MyProcPid, data);
    
    MemoryContextSwitchTo(old);
    PG_RETURN_INT32(data->value);
}


/* helpers */

static void
collect_stats_line(MemoryContext context, void *passthru,
                    const char *stats_string, bool print_to_stderr)
{
    StringInfo buf = (StringInfo) passthru;
    appendStringInfo(buf, "%s\n", stats_string);
}

/* basic alloc/free */

PG_FUNCTION_INFO_V1(test_basic_roundtrip);
Datum
test_basic_roundtrip(PG_FUNCTION_ARGS)
{
    MemoryContext old;
    int        *p;
    int         i;

    old = MemoryContextSwitchTo(ShmemGetRootContext());
    p = (int *) palloc(1000 * sizeof(int));

    for (i = 0; i < 1000; i++)
        p[i] = i * 7;

    for (i = 0; i < 1000; i++)
        if (p[i] != i * 7)
            elog(ERROR, "data mismatch at index %d (got %d, expected %d)",
                 i, p[i], i * 7);

    pfree(p);
    MemoryContextSwitchTo(old);

    PG_RETURN_BOOL(true);
}

/* repalloc: growing and shrinking */

PG_FUNCTION_INFO_V1(test_realloc_roundtrip);
Datum
test_realloc_roundtrip(PG_FUNCTION_ARGS)
{
    MemoryContext old;
    char       *ptr;
    const char *pattern = "the quick brown fox jumps over the lazy dog";
    size_t      patlen = strlen(pattern) + 1;

    old = MemoryContextSwitchTo(ShmemGetRootContext());

    ptr = (char *) palloc(patlen);
    memcpy(ptr, pattern, patlen);

    /* grow */
    ptr = (char *) repalloc(ptr, 8192);
    if (strcmp(ptr, pattern) != 0)
        elog(ERROR, "data corrupted after growing repalloc");
    memset(ptr + patlen, 0x5A, 8192 - patlen); /* touch the new tail */

    /* shrink back down */
    ptr = (char *) repalloc(ptr, patlen);
    if (strcmp(ptr, pattern) != 0)
        elog(ERROR, "data corrupted after shrinking repalloc");

    pfree(ptr);
    MemoryContextSwitchTo(old);

    PG_RETURN_BOOL(true);
}

/* Double free must be caught */

PG_FUNCTION_INFO_V1(test_double_free_detected);
Datum
test_double_free_detected(PG_FUNCTION_ARGS)
{
    MemoryContext old;
    void       *ptr;
    bool        caught = false;

    old = MemoryContextSwitchTo(ShmemGetRootContext());
    ptr = palloc(64);
    MemoryContextSwitchTo(old);

    pfree(ptr);

    PG_TRY();
    {
        pfree(ptr); /* second free of the same pointer */
    }
    PG_CATCH();
    {
        FlushErrorState();
        caught = true;
    }
    PG_END_TRY();

    if (!caught)
        elog(ERROR, "double free was not detected");

    PG_RETURN_BOOL(true);
}

/* Unaligned pointer must be rejected */

PG_FUNCTION_INFO_V1(test_unaligned_pointer_rejected);
Datum
test_unaligned_pointer_rejected(PG_FUNCTION_ARGS)
{
    MemoryContext old;
    void       *ptr;
    void       *bad_ptr;
    bool        caught = false;

    old = MemoryContextSwitchTo(ShmemGetRootContext());
    ptr = palloc(64);
    MemoryContextSwitchTo(old);

    bad_ptr = (char *) ptr + 1;

    PG_TRY();
    {
        ShmemContextFree(bad_ptr);
    }
    PG_CATCH();
    {
        FlushErrorState();
        caught = true;
    }
    PG_END_TRY();

    pfree(ptr); /* clean up the real, valid pointer */

    if (!caught)
        elog(ERROR, "unaligned pointer was not rejected");

    PG_RETURN_BOOL(true);
}

/* ShmemContextCreate must reject a non-shared parent */

PG_FUNCTION_INFO_V1(test_foreign_parent_rejected);
Datum
test_foreign_parent_rejected(PG_FUNCTION_ARGS)
{
    bool        caught = false;

    PG_TRY();
    {
        /* CurrentMemoryContext here is an ordinary backend-local context,
         * not one of ours - IsContextShared() must reject it. */
        ShmemContextCreate(CurrentMemoryContext, "should_fail");
    }
    PG_CATCH();
    {
        FlushErrorState();
        caught = true;
    }
    PG_END_TRY();

    if (!caught)
        elog(ERROR, "ShmemContextCreate accepted a non-shared parent");

    PG_RETURN_BOOL(true);
}

/* Nested contexts + reset */

PG_FUNCTION_INFO_V1(test_nested_context_reset);
Datum
test_nested_context_reset(PG_FUNCTION_ARGS)
{
    MemoryContext root = ShmemGetRootContext();
    MemoryContext child;
    MemoryContext grandchild;
    MemoryContext old;

    child = ShmemContextCreate(root, "test_child");
    grandchild = ShmemContextCreate(child, "test_grandchild");

    old = MemoryContextSwitchTo(grandchild);
    memset(palloc(128), 0xAB, 128);
    memset(palloc(256), 0xCD, 256);
    MemoryContextSwitchTo(old);

    if (ShmemContextIsEmpty(grandchild))
        elog(ERROR, "grandchild reported empty right after allocation");

    ShmemContextReset(grandchild);

    if (!ShmemContextIsEmpty(grandchild))
        elog(ERROR, "grandchild not empty after reset");

    /* the context must still be usable after a reset */
    old = MemoryContextSwitchTo(grandchild);
    pfree(palloc(64));
    MemoryContextSwitchTo(old);

    ShmemContextDelete(grandchild);
    ShmemContextDelete(child);

    PG_RETURN_BOOL(true);
}

/* freed chunk must actually come back off the free list,
 * not just be counted as free in the stats */

PG_FUNCTION_INFO_V1(test_free_list_reuse);
Datum
test_free_list_reuse(PG_FUNCTION_ARGS)
{
    MemoryContext old;
    void       *a,
               *b,
               *c;

    old = MemoryContextSwitchTo(ShmemGetRootContext());

    a = palloc(256);
    b = palloc(256);
    pfree(a);
    c = palloc(256);

    MemoryContextSwitchTo(old);

    if (a != c)
        elog(ERROR, "freed chunk was not reused: a=%p c=%p", a, c);

    old = MemoryContextSwitchTo(ShmemGetRootContext());
    pfree(b);
    pfree(c);
    MemoryContextSwitchTo(old);

    PG_RETURN_BOOL(true);
}

/*
 * Force at least one AddNewBlock() and make sure large,
 */
PG_FUNCTION_INFO_V1(test_block_growth);
Datum
test_block_growth(PG_FUNCTION_ARGS)
{
    MemoryContext ctx = ShmemContextCreate(ShmemGetRootContext(), "test_growth");
    MemoryContext old;
    Size        chunk_size = 1024 * 1024;  /* 1 MB */
    int         n_allocs = 200;
    void      **ptrs;
    int         i;

    old = MemoryContextSwitchTo(ctx);
    ptrs = (void **) palloc(sizeof(void *) * n_allocs);

    for (i = 0; i < n_allocs; i++)
    {
        ptrs[i] = palloc(chunk_size);
        memset(ptrs[i], i & 0xFF, chunk_size);
    }

    for (i = 0; i < n_allocs; i++)
    {
        unsigned char *p = (unsigned char *) ptrs[i];

        if (p[0] != (i & 0xFF) || p[chunk_size - 1] != (i & 0xFF))
            elog(ERROR, "data corruption detected in allocation %d", i);
        pfree(ptrs[i]);
    }

    MemoryContextSwitchTo(old);
    ShmemContextDelete(ctx);

    PG_RETURN_BOOL(true);
}

/* Consistency: total_/free_chunks invariants */

PG_FUNCTION_INFO_V1(test_consistency_check);
Datum
test_consistency_check(PG_FUNCTION_ARGS)
{
    /*
     * The pass/fail signal is
     * whether expected output stays clean of WARNINGs.
     */
    ShmemContextCheck(ShmemGetRootContext());
    PG_RETURN_BOOL(true);
}

/* Human-readable stats */

PG_FUNCTION_INFO_V1(shmem_get_stats);
Datum
shmem_get_stats(PG_FUNCTION_ARGS)
{
    StringInfoData buf;
    MemoryContextCounters totals;

    initStringInfo(&buf);
    memset(&totals, 0, sizeof(totals));

    ShmemContextStats(ShmemGetRootContext(), collect_stats_line, &buf,
                       &totals, false);

    appendStringInfo(&buf,
                      "TOTAL: %zu bytes in %zu chunks; %zu free (%zu chunks); %zu used\n",
                      totals.totalspace, totals.nblocks,
                      totals.freespace, totals.freechunks,
                      totals.totalspace - totals.freespace);

    PG_RETURN_TEXT_P(cstring_to_text(buf.data));
}

PG_FUNCTION_INFO_V1(shmem_write_marker);
Datum
shmem_write_marker(PG_FUNCTION_ARGS)
{
    int32       value = PG_GETARG_INT32(0);
    int32      *marker;

    marker = (int32 *) ShmemGetOrCreateUserData(sizeof(int32));
    *marker = value;

    PG_RETURN_INT32(value);
}

PG_FUNCTION_INFO_V1(shmem_read_marker);
Datum
shmem_read_marker(PG_FUNCTION_ARGS)
{
    int32      *marker = (int32 *) ShmemGetOrCreateUserData(sizeof(int32));

    PG_RETURN_INT32(*marker);
}

/* Functions to be used with pgbench for comparison */
PG_FUNCTION_INFO_V1(shmem_intensive_workload);
Datum
shmem_intensive_workload(PG_FUNCTION_ARGS)
{
    void *ptr;
    MemoryContext old = MemoryContextSwitchTo(ShmemGetRootContext());

    ptr = palloc(64);
    if (ptr == NULL)
        elog(ERROR, "allocation failed");

    pfree(ptr);

    MemoryContextSwitchTo(old);

    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(shmem_intensive_workload_def);
Datum
shmem_intensive_workload_def(PG_FUNCTION_ARGS)
{
    void *ptr;
    MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

    ptr = palloc(64);
    if (ptr == NULL)
        elog(ERROR, "allocation failed");

    pfree(ptr);

    MemoryContextSwitchTo(old);

    PG_RETURN_VOID();
}
