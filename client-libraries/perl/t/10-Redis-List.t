#!/usr/bin/perl

use warnings;
use strict;

use Test::More tests => 8;
use lib 'lib';
use Data::Dump qw/dump/;

BEGIN {
	use_ok( 'Redis::List' );
}

my @a;

ok( my $o = tie( @a, 'Redis::List', 'test-redis-list' ), 'tie' );

isa_ok( $o, 'Redis::List' );

$o->CLEAR;

ok( ! @a, 'empty list' );

ok( @a = ( 'foo', 'bar', 'baz' ), '=' );
is_deeply( [ @a ], [ 'foo', 'bar', 'baz' ] );

ok( push( @a, 'push' ), 'push' );
is_deeply( [ @a ], [ 'foo', 'bar', 'baz', 'push' ] );

#diag dump( @a );
