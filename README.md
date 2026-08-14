PostgreSQL with Shared Memory Context 
=====================================

This directory contains the source code distribution of the PostgreSQL
database management system.

Main changes for ShmemContext can be found in files:

src/backend/utils/mmgr/shmem_alloc.c
src/backend/utils/mmgr/shmem_area_search.c

src/backend/storage/ipc/dsm.c
src/backend/storage/ipc/dsm_impl.c
src/backend/storage/ipc/shared_mapping.c

src/include/utils/shmem_context.h
src/include/storage/shared_mapping.h

Extension with simple tests:
contrib/shmem_context_test/