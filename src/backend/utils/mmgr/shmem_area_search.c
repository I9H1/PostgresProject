#include "postgres.h"
#include "storage/procarray.h"
#include "storage/proc.h"
#include "storage/lwlock.h"
#include "miscadmin.h"
#include "utils/memutils.h"

#define MAX_PIDS 1024
#define MAX_REGIONS 1024

typedef struct MemoryRegion
{
    unsigned long start;
    unsigned long end;
} MemoryRegion;

typedef struct ProcessMemoryMap
{
    int pid;
    int num_regions;
    MemoryRegion regions[MAX_REGIONS];
} ProcessMemoryMap;

typedef struct AllMemoryMaps
{
    int num_proc;
    ProcessMemoryMap maps[MAX_PIDS];
} AllMemoryMaps;

static int GetActivePids(int *pids, int max_pids);
static int ReadMemoryMap(int pid, MemoryRegion *regions, int max_regions);
static AllMemoryMaps *ReadMemoryMaps(int *pids, int num_pids);
static bool IsAddressFreeInProcess(ProcessMemoryMap *map, unsigned long address, Size size);
static void *FindCommonFreeRegion(AllMemoryMaps *all_maps, Size size, unsigned long search_from);
void *GetNewShmemArea(Size size, unsigned long search_from);

/*
 * Searches for region of memory of required size
 * which is free in all processes from ProcArray.
 */
void *
GetNewShmemArea(Size size, unsigned long search_from)
{
    void *address;
    AllMemoryMaps *maps;
    int pids[MAX_PIDS];
    int num_pids;

    elog(LOG, "GetNewShmemArea is called");

    num_pids = GetActivePids(pids, MAX_PIDS);
    if (num_pids == 0)
        return NULL;

    elog(LOG, "Got pids");

    maps = ReadMemoryMaps(pids, num_pids);

    if (!maps || maps->num_proc == 0)
        return NULL;
    
    elog(LOG, "Got maps");

    address = FindCommonFreeRegion(maps, size, search_from);

    elog(LOG, "Found region");

    pfree(maps);
    return address;
}

/*
 * Searches for region of memory of required size
 * which does not overlap with any memory map.
 */
static void *
FindCommonFreeRegion(AllMemoryMaps *all_maps, Size size, unsigned long search_from)
{
    ProcessMemoryMap *base_map;
    unsigned long prev_end;
    unsigned long gap_start;
    unsigned long gap_end;
    unsigned long aligned_start;
    unsigned long max_addr;
    bool free_in_all;

    if (!all_maps || all_maps->num_proc == 0)
        return NULL;

    if (size == 0)
        return NULL;

    base_map = &all_maps->maps[0];

    prev_end = search_from;

    for (int i = 0; i < base_map->num_regions; ++i)
    {
        if (base_map->regions[i].end <= search_from)
            continue;
        
        gap_start = prev_end;
        gap_end = base_map->regions[i].start;

        aligned_start = (gap_start + 4095) & ~4095;

        if (gap_end > aligned_start && (gap_end - aligned_start) >= size)
        {
            free_in_all = true;

            for (int j = 1; j < all_maps->num_proc; ++j)
            {
                if (!IsAddressFreeInProcess(&all_maps->maps[j], aligned_start, size))
                {
                    free_in_all = false;
                    break;
                }
            }

            if (free_in_all)
                return (void *) aligned_start;
        }

        prev_end = base_map->regions[i].end;
        elog(LOG, "Iter: %d", i);
    }

    /* Last gap between last region end and end of address space */
    max_addr = 0x00007fffffffffffUL;
    aligned_start = (prev_end + 4095) & ~4095;

    if (aligned_start + size <= max_addr)
    {
        free_in_all = true;

        for (int j = 1; j < all_maps->num_proc; ++j)
            {
                if (!IsAddressFreeInProcess(&all_maps->maps[j], aligned_start, size))
                {
                    free_in_all = false;
                    break;
                }
            }

            if (free_in_all)
                return (void *) aligned_start;
    }

    return NULL;
}

/*
 * Checks if region of memory is free in process.
 */
static bool
IsAddressFreeInProcess(ProcessMemoryMap *map, unsigned long address, Size size)
{
    unsigned long end = address + size;

    for (int i = 0; i < map->num_regions; ++i)
    {
        unsigned long region_start = map->regions[i].start;
        unsigned long region_end = map->regions[i].end;

        if (address < region_end && end > region_start)
            return false;
    }

    return true;
}

/*
 * Fills 'pids' with pids of processes in ProcArray.
 * Returns amount of them.
 */
static int
GetActivePids(int *pids, int max_pids)
{
    int count = 0;

    if (PostmasterPid > 0 && count < max_pids)
        pids[count++] = PostmasterPid;

    LWLockAcquire(ProcArrayLock, LW_SHARED);

    for (int i = 0; i < ProcGlobal->allProcCount && count < max_pids; ++i)
    {   
        PGPROC * proc = &ProcGlobal->allProcs[i];   

        if (proc->pid == 0 || proc->pid == PostmasterPid)
            continue;

        pids[count++] = proc->pid;
    }

    LWLockRelease(ProcArrayLock);

    return count;
}

/*
 * Returns a structure with mmaps of all processes
 * in 'pids'.
 */
static AllMemoryMaps *
ReadMemoryMaps(int *pids, int num_pids)
{
    AllMemoryMaps *all_maps;
    int processed = 0;

    if (!pids || num_pids <= 0)
        return NULL;

    MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
    all_maps = (AllMemoryMaps *) palloc(sizeof(AllMemoryMaps));
    MemoryContextSwitchTo(old);

    all_maps->num_proc = 0;
    
    for (int i = 0; i < num_pids && i < MAX_PIDS; ++i)
    {
        ProcessMemoryMap *map = &all_maps->maps[processed];

        int num_regions;

        num_regions = ReadMemoryMap(pids[i], map->regions, MAX_REGIONS);

        if (num_regions > 0)
        {
            map->pid = pids[i];
            map->num_regions = num_regions;
            processed++;
            
            elog(LOG, "Read %d regions for PID %d", num_regions, pids[i]);
        }
        else if (num_regions == -1)
        {
            elog(WARNING, "Failed to read memory map for PID %d", pids[i]);
        }
    }

    all_maps->num_proc = processed;
    return all_maps;
}

/*
 * Fills structure with mmaps of process
 * referred by 'pid'. Returns amount of 
 * mapped regions.
 */
static int 
ReadMemoryMap(int pid, MemoryRegion *regions, int max_regions)
{
    char path[64];
    char line[512];
    FILE *fp;
    int count = 0;
    
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    
    fp = fopen(path, "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp) && count < max_regions)
    {
        unsigned long start, end;
        
        if (sscanf(line, "%lx-%lx", &start, &end) == 2)
        {
            regions[count].start = start;
            regions[count].end = end;
            count++;
        }
    }
    
    fclose(fp);
    return count;
}
