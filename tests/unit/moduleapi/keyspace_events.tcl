set testmodule [file normalize tests/modules/keyspace_events.so]

tags "modules" {
    start_server [list overrides [list loadmodule "$testmodule"]] {

        test {Test loaded key space event} {
            r set x 1
            r hset y f v
            r lpush z 1 2 3
            r sadd p 1 2 3
            r zadd t 1 f1 2 f2
            r xadd s * f v
            r debug reload
            assert_equal {1 x} [r keyspace.is_key_loaded x]
            assert_equal {1 y} [r keyspace.is_key_loaded y]
            assert_equal {1 z} [r keyspace.is_key_loaded z]
            assert_equal {1 p} [r keyspace.is_key_loaded p]
            assert_equal {1 t} [r keyspace.is_key_loaded t]
            assert_equal {1 s} [r keyspace.is_key_loaded s]
        }

        test {Nested multi due to RM_Call} {
            r del multi
            r del lua

            r set x 1
            r set x_copy 1
            r keyspace.del_key_copy x
            r keyspace.incr_case1 x
            r keyspace.incr_case2 x
            r keyspace.incr_case3 x
            assert_equal {} [r get multi]
            assert_equal {} [r get lua]
            r get x
        } {3}
        
        test {Nested multi due to RM_Call, with client MULTI} {
            r del multi
            r del lua

            r set x 1
            r set x_copy 1
            r multi
            r keyspace.del_key_copy x
            r keyspace.incr_case1 x
            r keyspace.incr_case2 x
            r keyspace.incr_case3 x
            r exec
            assert_equal {1} [r get multi]
            assert_equal {} [r get lua]
            r get x
        } {3}
        
        test {Nested multi due to RM_Call, with EVAL} {
            r del multi
            r del lua

            r set x 1
            r set x_copy 1
            r eval {
                redis.pcall('keyspace.del_key_copy', KEYS[1])
                redis.pcall('keyspace.incr_case1', KEYS[1])
                redis.pcall('keyspace.incr_case2', KEYS[1])
                redis.pcall('keyspace.incr_case3', KEYS[1])
            } 1 x
            assert_equal {} [r get multi]
            assert_equal {1} [r get lua]
            r get x
        } {3}

        test {Test module key space event} {
            r keyspace.notify x
            assert_equal {1 x} [r keyspace.is_module_key_notified x]
        }

        test "Keyspace notifications: module events test" {
            r config set notify-keyspace-events Kd
            r del x
            set rd1 [redis_deferring_client]
            assert_equal {1} [psubscribe $rd1 *]
            r keyspace.notify x
            assert_equal {pmessage * __keyspace@9__:x notify} [$rd1 read]
            $rd1 close
        }

        test {Test expired key space event} {
            set prev_expired [s expired_keys]
            r set exp 1 PX 10
            wait_for_condition 100 10 {
                [s expired_keys] eq $prev_expired + 1
            } else {
                fail "key not expired"
            }
            assert_equal [r get testkeyspace:expired] 1
        }

        test {Test removed key space event} {
            r set a abcd
            r del a
            # For String Type value is returned
            assert_equal {1 abcd} [r keyspace.is_key_removed a]

            r hset b f v
            r hdel b f
            assert_equal {1 b} [r keyspace.is_key_removed b]

            r lpush c 1
            r lpop c
            assert_equal {1 c} [r keyspace.is_key_removed c]

            r sadd d 1
            r spop d
            assert_equal {1 d} [r keyspace.is_key_removed d]

            r zadd e 1 f
            r zpopmin e
            assert_equal {1 e} [r keyspace.is_key_removed e]

            r xadd f 1-1 f v
            r xdel f 1-1
            # Stream does not delete object when del entry
            assert_equal {0 {}} [r keyspace.is_key_removed f]
            r del f
            assert_equal {1 f} [r keyspace.is_key_removed f]

            # delete key because of active expire
            set size [r dbsize]
            r set g abcd px 1
            #ensure active expire
            wait_for_condition 50 100 {
                [r dbsize] == $size
            } else {
                fail "Active expire not trigger"
            }
            assert_equal {1 abcd} [r keyspace.is_key_removed g]

            # delete key because of lazy expire
            r debug set-active-expire 0
            r set h abcd px 1
            after 10
            r get h
            assert_equal {1 abcd} [r keyspace.is_key_removed h]
            r debug set-active-expire 1

            # delete key not yet expired
            r set i abcd ex 100
            r del i
            assert_equal {1 abcd} [r keyspace.is_key_removed i]
        } {} {needs:debug}

        test "Unload the module - testkeyspace" {
            assert_equal {OK} [r module unload testkeyspace]
        }
    }
}
