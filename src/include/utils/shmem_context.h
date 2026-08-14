#ifndef POSTGRESQL_SHMEM_CONTEXT_H
#define POSTGRESQL_SHMEM_CONTEXT_H

#include "nodes/memnodes.h"
#include "storage/lwlock.h"
#include <c.h>

#define SHMEM_CONTEXT_MIN_SIZE_KB      (1024)        /* 1 MB */
#define SHMEM_CONTEXT_MAX_SIZE_KB      (10485760)    /* 10 GB */
#define SHMEM_CONTEXT_DEFAULT_SIZE_KB  (1048576)     /* 1 GB */

extern int shmem_context_memory_size_kb;

extern Size ShmemContextGetShmemSize(void);
extern void ShmemContextInit(void);
MemoryContext ShmemGetRootContext(void);
void *ShmemGetOrCreateUserData(Size size);
extern bool StartShmemContext(void);
extern void *GetNewShmemArea(Size size, unsigned long search_from);

#endif //POSTGRESQL_SHMEM_CONTEXT_H
