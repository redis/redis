set testmodule [file normalize tests/modules/basics.so]


start_server {tags {"modules"}} {
    r module load $testmodule

    test {test module api basics} {
        r test.basics
    } {ALL TESTS PASSED}

    test {test resp3 big number protocol parsing} {
        r eval {
            redis.setresp(3);
            return redis.call('test.execute', 'debug', 'protocol', 'bignum')
        } 0
    } {1234567999999999999999999999999999999}

    test {test resp3 map protocol parsing} {
        r eval {
            redis.setresp(3);
            return redis.call('test.execute', 'debug', 'protocol', 'map')
        } 0
    } {1 1 2 0 0 0}

    test {test resp3 set protocol parsing} {
        r eval {
            redis.setresp(3);
            return redis.call('test.execute', 'debug', 'protocol', 'set')
        } 0
    } {1 2 0}

    test {test resp3 double protocol parsing} {
        r eval {
            redis.setresp(3);
            return redis.call('test.execute', 'debug', 'protocol', 'double')
        } 0
    } {3.1415926535900001}

    test {test resp3 null protocol parsing} {
        r eval {
            redis.setresp(3);
            return redis.call('test.execute', 'debug', 'protocol', 'null')
        } 0
    } {}

    test {test resp3 verbatim protocol parsing} {
        r eval {
            redis.setresp(3);
            return redis.call('test.execute', 'debug', 'protocol', 'verbatim')
        } 0
    } "This is a verbatim\nstring"

    test {test resp3 true protocol parsing} {
        r eval {
            redis.setresp(3);
            return redis.call('test.execute', 'debug', 'protocol', 'true')
        } 0
    } {1}

    test {test resp3 false protocol parsing} {
        r eval {
            redis.setresp(3);
            return redis.call('test.execute', 'debug', 'protocol', 'false')
        } 0
    } {0}

    r module unload test
}
