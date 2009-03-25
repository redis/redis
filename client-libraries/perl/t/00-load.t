#!perl -T

use Test::More tests => 1;

BEGIN {
	use_ok( 'Redis' );
}

diag( "Testing Redis $Redis::VERSION, Perl $], $^X" );
