# contrib/shmem_context_test/Makefile

USE_PGXS = 1

MODULE_big = shmem_context_test
EXTENSION = shmem_context_test

DATA = shmem_context_test--1.0.sql

OBJS = shmem_context_test.o

PGFILEDESC = "shmem_context_test - memory allocation for MemoryContext in shmem"

TAP_TESTS = 1

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