# contrib/shmem_context_test/Makefile

MODULE_big = shmem_context_test
EXTENSION = shmem_context_test

DATA = shmem_context_test--1.0.sql

OBJS = shmem_context_test.o

PGFILEDESC = "shmem_context_test - memory allocation for MemoryContext in shmem"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/shmem_context_test
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif