#include "postgres.h"
#include "storage/shared_mapping.h"
#include "storage/shmem.h"
#include "miscadmin.h"

SharedMappingControl *sharedMappingControl = NULL;

bool
ProcessBarrierShmemAttachAll(void)
{
    dsm_handle handle;
    void *addr;
    dsm_segment *segment;

    elog(LOG, "ProcessBarrierShmemAttachAll: pid=%d coordinator_pid=%d",
         MyProcPid, sharedMappingControl->coordinator_pid);

    if (MyProcPid == PostmasterPid)
        return true;

    if (MyProcPid == sharedMappingControl->coordinator_pid)
        return true;

    LWLockAcquire(&sharedMappingControl->lwlock, LW_SHARED);
    handle = sharedMappingControl->handle;
    addr = sharedMappingControl->address;
    LWLockRelease(&sharedMappingControl->lwlock);

    if (handle == DSM_HANDLE_INVALID || addr == NULL)
    {
        elog(WARNING, "Failed to process BarrierShmemAttachAll: invalid params");
        pg_atomic_write_u32(&sharedMappingControl->failed, 1);
        return false;
    }

    segment = dsm_attach_at(handle, addr, true);
    if (segment == NULL)
    {
        elog(WARNING, "Backend %d: failed to map segment %u at %p",
             MyProcPid, handle, addr);
        pg_atomic_write_u32(&sharedMappingControl->failed, 1);
        return false;
    }

    if (dsm_segment_address(segment) != addr)
    {
        elog(WARNING, "Backend %d: wrong mapping", MyProcPid);
        pg_atomic_write_u32(&sharedMappingControl->failed, 1);
        return false;
    }

    dsm_pin_mapping(segment);

    return true;
}

bool
ProcessBarrierShmemDetach(void)
{
    dsm_segment *segment;
    dsm_handle handle = DSM_HANDLE_INVALID;
    void *addr;
    void *impl_private;
    void *mapped_address;
    Size mapped_size;
    bool is_postmaster = (MyProcPid == PostmasterPid);

    elog(LOG, "detach");

    LWLockAcquire(&sharedMappingControl->lwlock, LW_SHARED);
    handle = sharedMappingControl->handle;
    addr = sharedMappingControl->address;
    LWLockRelease(&sharedMappingControl->lwlock);

    if (addr == NULL || handle == DSM_HANDLE_INVALID)
    {
        return true;
    }

    if (is_postmaster)
    {
        if (!dsm_impl_op(DSM_OP_DETACH, handle, 0, &impl_private,
                         &mapped_address, &mapped_size, WARNING))
        {
            elog(WARNING, "Process %d: failed to detach from %p",
                 MyProcPid, addr);
            return false;
        }
    }
    else
    {
        segment = dsm_find_mapping(handle);
        if (segment == NULL)
            return true;
        
        dsm_detach(segment);

        elog(LOG, "Backend %d: successfully detached from %p",
             MyProcPid, addr);
    }
    return true;
}
