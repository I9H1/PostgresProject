/*
 * This file contains ProcBarrierSignal handler for
 * ShmemContext. See shmem_alloc.c
 */

#include "postgres.h"
#include "storage/shared_mapping.h"
#include "storage/shmem.h"
#include "miscadmin.h"

SharedMappingControl *sharedMappingControl = NULL;

/*
 * Attach created dsm segment.
 * sharedMappingControl must be initialized is shared memory first.
 */
bool
ProcessBarrierShmemAttachAll(void)
{
    dsm_handle handle;
    void *addr;
    dsm_segment *segment;
    bool can_replace;

    elog(LOG, "ProcessBarrierShmemAttachAll: pid=%d", MyProcPid);

    LWLockAcquire(&sharedMappingControl->lwlock, LW_SHARED);
    handle = sharedMappingControl->handle;
    addr = sharedMappingControl->address;
    can_replace = sharedMappingControl->can_replace;
    LWLockRelease(&sharedMappingControl->lwlock);

    if (handle == DSM_HANDLE_INVALID || addr == NULL)
    {
        elog(WARNING, "Failed to process BarrierShmemAttachAll: invalid params");
        pg_atomic_write_u32(&sharedMappingControl->failed, 1);
        return false;
    }

    segment = dsm_attach_at(handle, addr, can_replace);
    if (segment == NULL)
    {
        elog(WARNING, "Process %d: failed to map segment %u at %p",
             MyProcPid, handle, addr);
        pg_atomic_write_u32(&sharedMappingControl->failed, 1);
        return false;
    }

    if (dsm_segment_address(segment) != addr)
    {
        elog(WARNING, "Process %d: wrong mapping", MyProcPid);
        pg_atomic_write_u32(&sharedMappingControl->failed, 1);
        return false;
    }

    dsm_pin_mapping(segment);

    return true;
}

/*
 * Detach existing dsm segment.
 * sharedMappingControl must be initialized in shared memory first.
 */
bool
ProcessBarrierShmemDetach(void)
{
    dsm_segment *segment;
    dsm_handle handle = DSM_HANDLE_INVALID;
    void *addr;
    
    elog(LOG, "detach");

    LWLockAcquire(&sharedMappingControl->lwlock, LW_SHARED);
    handle = sharedMappingControl->handle;
    addr = sharedMappingControl->address;
    LWLockRelease(&sharedMappingControl->lwlock);

    if (addr == NULL || handle == DSM_HANDLE_INVALID)
    {
        return true;
    }

    segment = dsm_find_mapping(handle);
    if (segment == NULL)
        return true;
        
    dsm_detach(segment);

    elog(LOG, "Process %d: successfully detached from %p",
         MyProcPid, addr);

    return true;
}
