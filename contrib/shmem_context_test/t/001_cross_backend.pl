use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf',
    "shared_preload_libraries = 'shmem_context_test'");
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION shmem_context_test');

my $s1 = $node->background_psql('postgres');
$s1->query_safe('SELECT shmem_write_marker(424242)');

$s1->query_safe('SELECT test_block_growth()');

my $result = $node->safe_psql('postgres', 'SELECT shmem_read_marker()');
is($result, '424242', 'second backend sees data written by the first');

$s1->quit;
$node->stop;
done_testing();