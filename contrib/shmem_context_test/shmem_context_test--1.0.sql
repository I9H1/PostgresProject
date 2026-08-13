/* contrib/shmem_context_test/shmem_context_test--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION shmem_context_test" to load this file. \quit

CREATE OR REPLACE FUNCTION check_allocator()
    RETURNS TEXT
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE OR REPLACE FUNCTION shmem_test()
    RETURNS INTEGER
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
