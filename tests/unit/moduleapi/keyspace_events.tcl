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

        proc __consume_subscribe_messages {client type channels} {
            set numsub -1
            set counts {}

            for {set i [llength $channels]} {$i > 0} {incr i -1} {
                set msg [$client read]
                assert_equal $type [lindex $msg 0]

                # when receiving subscribe messages the channels names
                # are ordered. when receiving unsubscribe messages
                # they are unordered
                set idx [lsearch -exact $channels [lindex $msg 1]]
                if {[string match "*unsubscribe" $type]} {
                    assert {$idx >= 0}
                } else {
                    assert {$idx == 0}
                }
                set channels [lreplace $channels $idx $idx]

                # aggregate the subscription count to return to the caller
                lappend counts [lindex $msg 2]
            }

            # we should have received messages for channels
            assert {[llength $channels] == 0}
            return $counts
        }

        proc psubscribe {client channels} {
            $client psubscribe {*}$channels
            __consume_subscribe_messages $client psubscribe $channels
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
	}
}
