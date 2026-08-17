/* contrib/shmem_context_test/shmem_context_test--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION shmem_context_test" to load this file. \quit

CREATE FUNCTION shmem_test() RETURNS int
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_basic_roundtrip() RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_realloc_roundtrip() RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_double_free_detected() RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_unaligned_pointer_rejected() RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_foreign_parent_rejected() RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_nested_context_reset() RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_free_list_reuse() RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_block_growth() RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION test_consistency_check() RETURNS bool
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION shmem_get_stats() RETURNS text
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION shmem_write_marker(int) RETURNS int
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION shmem_read_marker() RETURNS int
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION shmem_intensive_workload() RETURNS void
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
CREATE FUNCTION shmem_intensive_workload_def() RETURNS void
    AS 'MODULE_PATHNAME' LANGUAGE C STRICT;
