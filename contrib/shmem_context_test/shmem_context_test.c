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

PG_FUNCTION_INFO_V1(check_allocator);

PG_FUNCTION_INFO_V1(shmem_test);

typedef struct {
    int value;
} SharedInt;

static SharedInt *data = NULL;

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

/* For local */
Datum
check_allocator(PG_FUNCTION_ARGS)
{
	MemoryContext root_shmem;
	MemoryContext my_context;
	MemoryContext oldContext;

	void *ptr;
	char *test_string = "hello_allocator";
	
	root_shmem = ShmemGetRootContext();
	my_context = ShmemContextCreate(root_shmem, "my_context");

	oldContext = MemoryContextSwitchTo(my_context);

	ptr = palloc(1024);
	if (ptr == NULL)
		elog(ERROR, "palloc returned NULL");

	strcpy((char *) ptr, test_string);

	ptr = repalloc(ptr, 2048);

	if (strcmp((char *)ptr, test_string) != 0)
        elog(ERROR, "Data corrupted after repalloc");

	pfree(ptr);

	ptr = palloc(512);
	MemoryContextSwitchTo(oldContext);
	MemoryContextDelete(my_context);

	PG_RETURN_TEXT_P(cstring_to_text("shmem context passed all tests"));
}
