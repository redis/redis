set testmodule [file normalize tests/modules/reply.so]

start_server {tags {"modules external:skip"}} {
    r module load $testmodule
    
    #   test all with hello 2/3
    for {set proto 2} {$proto <= 3} {incr proto} {
        if {[lsearch $::denytags "resp3"] >= 0} {
            if {$proto == 3} {continue}
        } elseif {$::force_resp3} {
            if {$proto == 2} {continue}
        }
        r hello $proto

        test "RESP$proto: RM_ReplyWithString: an string reply" {
            # RedisString
            set string [r rw.string "Redis"]
            assert_equal "Redis" $string
            # C string
            set string [r rw.cstring]
            assert_equal "A simple string" $string
        }

        test "RESP$proto: RM_ReplyWithBigNumber: an string reply" {
            assert_equal "123456778901234567890" [r rw.bignumber "123456778901234567890"]
        }

        test "RESP$proto: RM_ReplyWithInt: an integer reply" {
            assert_equal 42 [r rw.int 42]
        }

        test "RESP$proto: RM_ReplyWithDouble: a float reply" {
            assert_equal 3.141 [r rw.double 3.141]
        }

        test "RESP$proto: RM_ReplyWithDouble: inf" {
            if {$proto == 2} {
                assert_equal "inf" [r rw.double inf]
                assert_equal "-inf" [r rw.double -inf]
            } else {
                # TCL convert inf to different results on different platforms, e.g. inf on mac
                # and Inf on others, so use readraw to verify the protocol
                r readraw 1
                assert_equal ",inf" [r rw.double inf]
                assert_equal ",-inf" [r rw.double -inf]
                r readraw 0
            }
        }

        test "RESP$proto: RM_ReplyWithDouble: NaN" {
            if {$proto == 2} {
                assert_equal "nan" [r rw.double 0 0]
                assert_equal "nan" [r rw.double]
            } else {
                # TCL won't convert nan into a double, use readraw to verify the protocol
                r readraw 1
                assert_equal ",nan" [r rw.double 0 0]
                assert_equal ",nan" [r rw.double]
                r readraw 0
            }
        }

        set ld 0.00000000000000001
        test "RESP$proto: RM_ReplyWithLongDouble: a float reply" {
            if {$proto == 2} {
                # here the response gets to TCL as a string
                assert_equal $ld [r rw.longdouble $ld]
            } else {
                # TCL doesn't support long double and the test infra converts it to a
                # normal double which causes precision loss. so we use readraw instead
                r readraw 1
                assert_equal ",$ld" [r rw.longdouble $ld]
                r readraw 0
            }
        }

        test "RESP$proto: RM_ReplyWithVerbatimString: a string reply" {
            assert_equal "bla\nbla\nbla" [r rw.verbatim "bla\nbla\nbla"]
        }

        test "RESP$proto: RM_ReplyWithArray: an array reply" {
            assert_equal {0 1 2 3 4} [r rw.array 5]
        }

        test "RESP$proto: RM_ReplyWithMap: an map reply" {
            set res [r rw.map 3]
            if {$proto == 2} {
                assert_equal {0 0 1 1.5 2 3} $res
            } else {
                assert_equal [dict create 0 0.0 1 1.5 2 3.0] $res
            }
        }

        test "RESP$proto: RM_ReplyWithSet: an set reply" {
            assert_equal {0 1 2} [r rw.set 3]
        }

        test "RESP$proto: RM_ReplyWithAttribute: an set reply" {
            if {$proto == 2} {
                catch {[r rw.attribute 3]} e
                assert_match "Attributes aren't supported by RESP 2" $e
            } else {
                r readraw 1
                set res [r rw.attribute 3]
                assert_equal [r read] {:0}
                assert_equal [r read] {,0}
                assert_equal [r read] {:1}
                assert_equal [r read] {,1.5}
                assert_equal [r read] {:2}
                assert_equal [r read] {,3}
                assert_equal [r read] {+OK}
                r readraw 0
            }
        }

        test "RESP$proto: RM_ReplyWithBool: a boolean reply" {
            assert_equal {0 1} [r rw.bool]
        }

        test "RESP$proto: RM_ReplyWithNull: a NULL reply" {
            assert_equal {} [r rw.null]
        }

        test "RESP$proto: RM_ReplyWithError: an error reply" {
            catch {r rw.error} e
            assert_match "An error" $e
        }

        test "RESP$proto: RM_ReplyWithErrorFormat: error format reply" {
            catch {r rw.error_format "An error: %s" foo} e
            assert_match "An error: foo" $e  ;# Should not be used by a user, but compatible with RM_ReplyError

            catch {r rw.error_format "-ERR An error: %s" foo2} e
            assert_match "-ERR An error: foo2" $e  ;# Should not be used by a user, but compatible with RM_ReplyError (There are two hyphens, TCL removes the first one)

            catch {r rw.error_format "-WRONGTYPE A type error: %s" foo3} e
            assert_match "-WRONGTYPE A type error: foo3" $e  ;# Should not be used by a user, but compatible with RM_ReplyError (There are two hyphens, TCL removes the first one)

            catch {r rw.error_format "ERR An error: %s" foo4} e
            assert_match "ERR An error: foo4" $e

            catch {r rw.error_format "WRONGTYPE A type error: %s" foo5} e
            assert_match "WRONGTYPE A type error: foo5" $e
        }

        test "RESP$proto: redis.call reply parsing with invalid CRLF character" {
            # When Lua parses redis.call replies, the current implementation only
            # searches for '\r' characters without verifying that '\n' follows. If a '\r'
            # appears in the protocol data (not as part of the CRLF delimiter), the parser
            # incorrectly treats it as a valid '\r\n' terminator.
            #
            # Example: Protocol data containing "\rx=100000000000" would be parsed as:
            #   - '\rx' is treated as line terminator (should require '\r\n')
            #   - '=100000000000' is interpreted as length specifier
            #   - Lua attempts to create a massive string → Out of Memory
            r deferred 1
            r eval {return redis.call('rw.simplestring_array', '\rx=100000000000', 'hello')} 0
            assert_equal [r rawread 30] "*2\r\n+ x=100000000000\r\n+hello\r\n"
            r deferred 0
        }

        foreach size {32 65536} {
            set payload [string repeat x $size]
            test "RESP$proto: reply buffer nested collections, ordered moves and reuse ($size bytes)" {
                assert_equal [list before [list $payload [list key 42]] 1 after] [r rw.buffer reuse $payload]
                assert_equal PONG [r ping]
            }
            test "RESP$proto: reply buffer validation preserves content ($size bytes)" {
                assert_equal [list $payload] [r rw.buffer invalid $payload]
                assert_equal PONG [r ping]
            }
            test "RESP$proto: reply buffer discard and detached destinations ($size bytes)" {
                set errors [s total_error_replies]
                foreach mode {discard detached abort} {
                    assert_equal OK [r rw.buffer $mode $payload]
                    assert_equal PONG [r ping]
                }
                assert_equal $errors [s total_error_replies]
            }
        }

        test "RESP$proto: reply buffer errors are counted once at the final destination" {
            set errors [s total_error_replies]
            assert_error "BUFFERERR sent" {r rw.buffer errors unused}
            assert_equal [expr {$errors + 1}] [s total_error_replies]
            assert_equal PONG [r ping]
        }

        test "RESP$proto: discarded open reply buffer warns and is released" {
            set warnings [count_log_message 0 "API misuse detected in module replywith"]
            assert_equal OK [r rw.buffer open unused]
            wait_for_condition 50 10 {
                [count_log_message 0 "API misuse detected in module replywith"] == $warnings + 1
            } else { fail "Missing postponed collection warning" }
        }

        test "RESP$proto: reply buffer creation on a failed MULTI block" {
            r multi
            r rw.buffer discard unused
            assert_error {*Blocking module command called from transaction*} {r exec}
            assert_equal PONG [r ping]
        }

        foreach completion {normal timeout disconnect} {
            test "RESP$proto: worker reply buffer lifetime through $completion" {
                set rd [redis_deferring_client]
                $rd hello $proto
                $rd read
                $rd client id
                set id [$rd read]
                set freed [lindex [r rw.buffer_status] 2]
                set errors [s total_error_replies]
                set payload [string repeat z 65536]
                $rd rw.buffer_start $payload string
                wait_for_condition 100 10 {
                    [lindex [r rw.buffer_status] 1] == 1
                } else { fail "Worker did not publish its buffer" }
                if {$completion eq "timeout"} {
                    assert_equal 1 [r client unblock $id timeout]
                    assert_equal [list $payload done] [$rd read]
                    # Timeout returns while the worker and its buffers remain alive.
                    assert_equal [list 1 1 $freed] [r rw.buffer_status]
                    $rd ping
                    assert_equal PONG [$rd read]
                } elseif {$completion eq "disconnect"} {
                    assert_equal 1 [r client kill id $id]
                }
                assert_equal OK [r rw.buffer_finish]
                if {$completion eq "normal"} {
                    assert_equal [list $payload done] [$rd read]
                    $rd ping
                    assert_equal PONG [$rd read]
                }
                wait_for_condition 100 10 {
                    [r rw.buffer_status] eq [list 0 0 [expr {$freed + 1}]]
                } else { fail "Worker buffers were not released" }
                assert_equal $errors [s total_error_replies]
                $rd close
            }
        }

        test "RESP$proto: worker errors remain deferred across buffer moves" {
            set rd [redis_deferring_client]
            $rd hello $proto
            $rd read
            set errors [s total_error_replies]
            $rd rw.buffer_start unused error
            wait_for_condition 100 10 {
                [lindex [r rw.buffer_status] 1] == 1
            } else { fail "Worker did not publish its buffer" }
            assert_equal $errors [s total_error_replies]
            assert_error "BUFFERERR worker" {r rw.buffer_take}
            assert_equal [expr {$errors + 1}] [s total_error_replies]
            assert_equal OK [r rw.buffer_take]
            r rw.buffer_finish
            assert_equal done [$rd read]
            wait_for_condition 100 10 {
                [lindex [r rw.buffer_status] 0] == 0
            } else { fail "Worker buffers were not released" }
            assert_equal [expr {$errors + 1}] [s total_error_replies]
            $rd close
        }

        foreach drop {off off-error limit} {
            test "RESP$proto: reply buffer consumed when destination drops replies ($drop)" {
                set rd [redis_deferring_client]
                $rd hello $proto
                $rd read
                set errors [s total_error_replies]
                set kind [expr {$drop eq "off-error" ? "error" : "string"}]
                $rd rw.buffer_start [string repeat q 65536] $kind
                wait_for_condition 100 10 {
                    [lindex [r rw.buffer_status] 1] == 1
                } else { fail "Worker did not publish its buffer" }
                set dst [redis_deferring_client]
                $dst hello $proto
                $dst read
                if {[string match off* $drop]} {
                    $dst client reply off
                    $dst rw.buffer_take
                    $dst client reply on
                    assert_equal OK [$dst read]
                } else {
                    set limit [lindex [r config get client-output-buffer-limit] 1]
                    r config set client-output-buffer-limit {normal 1024 0 0}
                    $dst rw.buffer_take close
                    assert_error {*I/O error*} {$dst read}
                    r config set client-output-buffer-limit $limit
                }
                assert_equal OK [r rw.buffer_take]
                r rw.buffer_finish
                assert_equal done [$rd read]
                wait_for_condition 100 10 {
                    [lindex [r rw.buffer_status] 0] == 0
                } else { fail "Worker buffers were not released" }
                $dst close
                $rd close
                assert_equal $errors [s total_error_replies]
                assert_equal PONG [r ping]
            }
        }

        if {[lsearch $::denytags "resp3"] < 0} {
            test "RESP$proto: reply buffer protocol compatibility preserves rejected source" {
                set rd [redis_deferring_client]
                $rd hello $proto
                $rd read
                $rd rw.buffer_start payload string
                wait_for_condition 100 10 {
                    [lindex [r rw.buffer_status] 1] == 1
                } else { fail "Worker did not publish its buffer" }
                set other [expr {5 - $proto}]
                r hello $other
                if {$proto == 3} {
                    assert_error "ERR incompatible protocol" {r rw.buffer_take}
                    r hello $proto
                    assert_equal payload [r rw.buffer_take]
                } else {
                    assert_equal payload [r rw.buffer_take]
                    r hello $proto
                }
                assert_equal OK [r rw.buffer_take]
                r rw.buffer_finish
                assert_equal done [$rd read]
                wait_for_condition 100 10 {
                    [lindex [r rw.buffer_status] 0] == 0
                } else { fail "Worker buffers were not released" }
                $rd close
                assert_equal PONG [r ping]
            }
        }

        r hello 2
    }

    test "Unload the module - replywith" {
        assert_equal {OK} [r module unload replywith]
    }
}
