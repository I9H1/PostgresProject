#ifndef SHARED_MAPPING_H
#define SHARED_MAPPING_H

#include "storage/lwlock.h"
#include "storage/dsm.h"

typedef struct SharedMappingControl
{
    LWLock lwlock;
    uint64 generation;
    dsm_handle handle;
    void *address;
    pid_t coordinator_pid;
    bool can_replace;
    pg_atomic_uint32 failed;

} SharedMappingControl;

extern SharedMappingControl *sharedMappingControl;

extern bool ProcessBarrierShmemAttachAll(void);
extern bool ProcessBarrierShmemDetach(void);

#endif /* SHARED_MAPPING_H */
