start_server {tags {"migrate-async"}} {
    test {RESTORE-ASYNC-AUTH fails if there is no password configured server side} {
        assert_match {RESTORE-ASYNC-ACK 1 *} [r restore-async-auth foo]
    }
}

start_server {tags {"migrate-async"} overrides {requirepass foobar}} {
    test {RESTORE-ASYNC-AUTH fails when a wrong password is given} {
        assert_match {RESTORE-ASYNC-ACK 1 *} [r restore-async-auth wrong_passwd]
    }
}

start_server {tags {"migrate-async"} overrides {requirepass foobar}} {
    test {RESTORE-ASYNC-SELECT fails when password is not given} {
        catch {r restore-async-select 1} err
        assert_match {NOAUTH*} $err
    }

    test {RESTORE-ASYNC-AUTH succeeds when the right password is given} {
        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-auth foobar]
    }

    test {RESTORE-ASYNC-AUTH succeeded then we can actually send commands to the server} {
        assert_equal OK [r set foo 100]
        assert_equal {101} [r incr foo]
    }
}

start_server {tags {"migrate-async"}} {
    test {RESTORE-ASYNC-SELECT can change database} {
        r select 0
        r set foo 100

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 0]
        assert_equal {101} [r incr foo]

        r select 1
        r set foo 200

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 1]
        assert_equal {201} [r incr foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 0]
        assert_equal {102} [r incr foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async-select 1]
        assert_equal {202} [r incr foo]
    }

    test {RESTORE-ASYNC DELETE against a single item} {
        r set foo hello
        assert_equal {hello} [r get foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async delete foo]
        assert_equal {} [r get foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async delete foo]
        assert_equal {} [r get foo]
    }

    test {RESTORE-ASYNC STRING against a string item} {
        r del foo
        assert_equal {} [r get foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async string foo 0 hello]
        assert_equal {hello} [r get foo]
        assert_equal {-1} [r pttl foo]

        r del foo
        assert_equal {} [r get foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async string foo 5000 world]
        assert_equal {world} [r get foo]
        set ttl [r pttl foo]
        assert {$ttl >= 3000 && $ttl <= 5000}

        r del bar
        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async string bar 0 10000]
        assert_equal {10001} [r incr bar]
    }

    test {RESTORE-ASYNC OBJECT against a string item} {
        r set foo hello
        set encoded [r dump foo]

        r del foo
        assert_equal {} [r get foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async object foo 5000 $encoded]
        assert_equal {hello} [r get foo]
        set ttl [r pttl foo]
        assert {$ttl >= 3000 && $ttl <= 5000}
    }

    test {RESTORE-ASYNC EXPIRE against a string item} {
        r set foo hello
        set encoded [r dump foo]

        r del foo
        assert_equal {} [r get foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async object foo 5000 $encoded]
        assert_equal {hello} [r get foo]
        set ttl [r pttl foo]
        assert {$ttl >= 3000 && $ttl <= 5000}

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async expire foo 8000]
        assert_equal {hello} [r get foo]
        set ttl [r pttl foo]
        assert {$ttl >= 6000 && $ttl <= 8000}

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async expire foo 0]
        assert_equal {hello} [r get foo]
        set ttl [r pttl foo]
        assert {$ttl == -1}
    }

    test {RESTORE-ASYNC LIST against a list item} {
        r del foo
        assert_equal {0} [r llen foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async list foo 0 0 a1 a2]
        assert_equal {2} [r llen foo]
        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async list foo 0 0 b1 b2]
        assert_equal {4} [r llen foo]
        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async list foo 0 0 c1 c2]
        assert_equal {6} [r llen foo]
        assert_equal {-1} [r pttl foo]
        assert_encoding quicklist foo
        assert_equal {a1} [r lindex foo 0]
        assert_equal {a2} [r lindex foo 1]
        assert_equal {b1} [r lindex foo 2]
        assert_equal {b2} [r lindex foo 3]
        assert_equal {c1} [r lindex foo 4]
        assert_equal {c2} [r lindex foo 5]
    }

    test {RESTORE-ASYNC HASH against a hash item} {
        r del foo
        assert_equal {0} [r hlen foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async hash foo 0 0 k1 v1 k2 v2]
        assert_equal {2} [r hlen foo]
        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async hash foo 0 0 k3 v3 k1 v4]
        assert_equal {3} [r hlen foo]
        assert_equal {-1} [r pttl foo]
        assert_encoding hashtable foo
        assert_equal {v4 v2 v3} [r hmget foo k1 k2 k3]
    }

    test {RESTORE-ASYNC DICT against a set item} {
        r del foo
        assert_equal {0} [r scard foo]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async dict foo 0 0 e1 e2 e3]
        assert_equal {3} [r scard foo]
        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async dict foo 0 0 e1 e2 e4]
        assert_equal {4} [r scard foo]
        assert_equal {-1} [r pttl foo]
        assert_encoding hashtable foo
        assert_equal {1} [r sismember foo e1]
        assert_equal {1} [r sismember foo e2]
        assert_equal {1} [r sismember foo e3]
        assert_equal {1} [r sismember foo e4]
    }

    test {RESTORE-ASYNC ZSET against a zset item} {
        r del foo
        assert_equal {0} [r zcard foo]

        # 1.0 -> 3ff0000000000000 -> \x00\x00\x00\x00\x00\x00\xf0\x3f LE
        # 2.0 -> 4000000000000000 -> \x00\x00\x00\x00\x00\x00\x00\x40 LE
        # 3.0 -> 4008000000000000 -> \x00\x00\x00\x00\x00\x00\x08\x40 LE
        # 4.0 -> 4010000000000000 -> \x00\x00\x00\x00\x00\x00\x10\x40 LE

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async zset foo 0 0 e1 "\x00\x00\x00\x00\x00\x00\xf0\x3f" e2 "\x00\x00\x00\x00\x00\x00\x00\x40"]
        assert_equal {2} [r zcard foo]
        assert_equal {1} [r zscore foo e1]
        assert_equal {2} [r zscore foo e2]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async zset foo 0 0 e1 "\x00\x00\x00\x00\x00\x00\x08\x40"]
        assert_equal {2} [r zcard foo]
        assert_equal {3} [r zscore foo e1]
        assert_equal {2} [r zscore foo e2]

        assert_match {RESTORE-ASYNC-ACK 0 *} [r restore-async zset foo 0 0 e2 "\x00\x00\x00\x00\x00\x00\x10\x40"]
        assert_equal {2} [r zcard foo]
        assert_equal {3} [r zscore foo e1]
        assert_equal {4} [r zscore foo e2]

        assert_equal {-1} [r pttl foo]
        assert_encoding skiplist foo
    }
}

start_server {tags {"migrate-async"}} {
    test {MIGRATE-ASYNC is caching connections} {
        set first [srv 0 client]
        start_server {tags {"migrate-async"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            assert_match {} [$first migrate-async-status]
            $first migrate-async $second_host $second_port 0 0 0 foo bar
            assert_match {host * port *} [$first migrate-async-status]

            assert_match {1} [$first migrate-async-cancel]
            assert_match {} [$first migrate-async-status]

            assert_match {} [$first migrate-async-status]
            $first migrate-async $second_host $second_port 0 0 0 foo bar
            assert_match {host * port *} [$first migrate-async-status]

            after 20000
            assert_match {} [$first migrate-async-status]
        }
    }

    test {MIGRATE-ASYNC is able to migrate a key between two instances} {
        set first [srv 0 client]
        start_server {tags {"migrate-async"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            assert_equal {1} [$second incr key]

            assert_equal {0} [$first exists key]
            assert_equal {0} [$first migrate-async $second_host $second_port 0 0 0 key]

            assert_equal {1} [$first incr key]
            $first pexpire key 5000

            assert_equal {1} [$first migrate-async $second_host $second_port 0 0 0 key]
            assert_equal {2} [$second incr key]
            set ttl [$second pttl key]
            assert {$ttl >= 3000 && $ttl <= 5000}

            assert_equal {0} [$first exists key]
        }
    }

    test {MIGRATE-ASYNC can correctly transfer large list} {
        set first [srv 0 client]
        start_server {tags {"migrate-async"}} {
            set second [srv 0 client]
            set second_host [srv 0 host]
            set second_port [srv 0 port]

            $first del key
            for {set j 0} {$j < 20000} {incr j} {
                $first rpush key "item $j"
            }

            assert_equal {1} [$first migrate-async $second_host $second_port 0 100000 100 key]
            assert_equal {0} [$first exists key]

            assert {[$second llen key] == 20000}
            for {set j 0} {$j < 20000} {incr j} {
                assert_equal "item $j" [$second lpop key]
            }
        }
    }
}
