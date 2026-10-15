# Native Bloom fixture: ten binary items {i, 0, 255}, i=0..9, inserted into
# RedisBloom with capacity 2, error 0.01, expansion 2, FORCE64 | NOROUND.
# Three filters, encoded using the native RDB format and a valid DUMP checksum.
proc native_bloom_payload {} {
    return [binary format H* 230a030502027b14ae47e17a743f08e9862fb2350e264040400008008808008800e09f02047b14ae47e17a643f094af7d49edef028404040000855c8c8405527260604087b14ae47e17a543f0aaa677a8b87d32b404080001054128444444c24a005010d08540424020411006c4d19c6a8d7d3a8]
}

start_server {tags {"bloom"}} {
    test {Native Bloom reports its type and encoding without a module} {
        assert_equal OK [r restore bloom 0 [native_bloom_payload]]
        assert_equal bloom [r type bloom]
        assert_equal bloom [r object encoding bloom]
        assert_equal bf.add [lindex [lindex [r command info bf.add] 0] 0]
    }

    test {Native Bloom supports memory accounting, COPY, and SCAN TYPE} {
        assert {[r memory usage bloom] > 100}
        assert_equal 1 [r copy bloom copied]
        assert_equal [r dump bloom] [r dump copied]
        assert_equal [r debug digest-value bloom] [r debug digest-value copied]
        assert_equal {bloom copied} [lsort [lindex [r scan 0 type bloom] 1]]
        r del bloom
        assert_equal bloom [r type copied]
        r rename copied bloom
    }

    test {Other data type commands reject native Bloom objects} {
        assert_error WRONGTYPE* {r get bloom}
        assert_error WRONGTYPE* {r sadd bloom x}
        assert_error WRONGTYPE* {r pfadd bloom x}
    }

    test {Bloom DUMP RESTORE round trip preserves filter data} {
        set payload [r dump bloom]
        r restore restored 0 $payload
        assert_equal $payload [r dump restored]
        r unlink restored
        assert_equal 0 [r exists restored]
    }

    test {Native Bloom survives RDB reload with expiry and digest intact} {
        r pexpire bloom 60000
        set digest [r debug digest-value bloom]
        r debug reload
        set dump_path [file join [lindex [r config get dir] 1] dump.rdb]
        assert_match {*RDB looks OK*} [exec src/redis-check-rdb $dump_path]
        assert_equal bloom [r type bloom]
        assert_equal $digest [r debug digest-value bloom]
        assert {[r pttl bloom] > 0}
        r persist bloom
    } {1} {needs:debug}

    test {Native Bloom rejects malformed RDB fields and truncated payloads} {
        r debug set-skip-checksum-validation 1
        set payload [native_bloom_payload]
        # Count mismatch, no filters, invalid options/growth/hash count/n2,
        # a per-filter count exceeding capacity, and an encoded integer tag.
        foreach {offset value} {1 0 2 0 3 255 4 0 14 0 25 64 35 3 2 192} {
            set bad [string replace $payload $offset $offset [binary format c $value]]
            assert_error {*Bad data format*} {r restore bad 0 $bad}
            assert_equal 0 [r exists bad]
        }
        set bad [string replace $payload 6 13 [binary format H* 000000000000f87f]]
        assert_error {*Bad data format*} {r restore bad 0 $bad}
        for {set cut 1} {$cut < [string length $payload]-10} {incr cut} {
            set bad "[string range $payload 0 [expr {$cut-1}]][string range $payload end-9 end]"
            assert_error {*Bad data format*} {r restore bad 0 $bad}
        }
        r debug set-skip-checksum-validation 0
        assert_equal PONG [r ping]
    } {} {needs:debug}

    foreach preamble {no yes} {
        test "Native Bloom survives AOF rewrite with RDB preamble $preamble" {
            r config set aof-use-rdb-preamble $preamble
            r config set appendonly yes
            waitForBgrewriteaof r
            set payload [r dump bloom]
            r pexpire bloom 60000
            r bgrewriteaof
            waitForBgrewriteaof r
            assert_equal ok [s aof_last_bgrewrite_status]
            r debug loadaof
            assert_equal $payload [r dump bloom]
            assert {[r pttl bloom] > 0}
            r persist bloom
            r config set appendonly no
        } {OK} {needs:debug}
    }

    test {Native Bloom expiration and deletion free the key} {
        r pexpire bloom 1
        wait_for_condition 50 10 {![r exists bloom]} else {fail "Bloom key did not expire"}
        r restore bloom 0 [native_bloom_payload]
        assert_equal 1 [r del bloom]
    }
}

if {$::debug_defrag} {
    start_server {tags {"bloom defrag needs:debug external:skip cluster:skip"} overrides {save "" key-memory-histograms yes}} {
        foreach scan_fields {1000 1} {
            test "Forced Bloom defrag preserves contents with scan limit $scan_fields" {
                r restore bloom 0 [native_bloom_payload] replace
                set payload [r dump bloom]
                r config resetstat
                r config set active-defrag-max-scan-fields $scan_fields
                r config set hz 100
                r config set active-defrag-ignore-bytes 1
                r config set active-defrag-threshold-lower 1 active-defrag-threshold-upper 1
                r config set active-defrag-cycle-min 65 active-defrag-cycle-max 75
                r config set activedefrag yes
                wait_for_condition 500 10 {[s active_defrag_key_hits] > 0} else {
                    fail "Bloom key was not relocated"
                }
                r config set activedefrag no
                wait_for_condition 500 10 {[s active_defrag_running] == 0} else {
                    fail "Bloom defrag did not stop"
                }
                assert_equal $payload [r dump bloom]
                assert {[r memory usage bloom] > 100}
            }
        }
    }
}

start_server {tags {"bloom"} overrides {save "" key-memory-histograms yes}} {
    r hello 2
    test {Bloom ADD MADD INSERT and membership preserve binary items} {
        set item [binary format H* 0061ff00]
        assert_equal 1 [r bf.add bf $item]
        assert_equal 0 [r bf.add bf $item]
        assert_equal {0 1 1} [r bf.madd bf $item b c]
        assert_equal {1 0} [r bf.insert bf items d b]
        assert_equal {1 1 1 1 0} [r bf.mexists bf $item b c d missing]
        assert_equal 4 [r bf.card bf]
        assert_equal 0 [r bf.card absent]
        assert_equal {0 0} [r bf.mexists absent a b]
        r set string value
        assert_equal 0 [r bf.exists string a]
        assert_error WRONGTYPE* {r bf.add string a}
        assert_error WRONGTYPE* {r bf.card string}
        assert_error WRONGTYPE* {r bf.reserve string 0.01 10}
        assert_error {*not found*} {r bf.insert absent nocreate items a}
        assert_error {*not found*} {r bf.info absent}
        assert_equal 0 [r exists absent]
    }

    test {Bloom command metadata exposes key specs and ACL category} {
        set commands {bf.add bf.card bf.debug bf.exists bf.info bf.insert bf.loadchunk bf.madd bf.mexists bf.reserve bf.scandump}
        assert_equal $commands [lsort [r acl cat bloom]]
        foreach cmd $commands {
            assert_equal bf [r command getkeys $cmd bf {*}[switch -- $cmd {
                bf.add - bf.exists - bf.madd - bf.mexists {list a}
                bf.reserve {list 0.01 10}
                bf.insert {list items a}
                bf.scandump {list 0}
                bf.loadchunk {list 1 payload}
                default {list}
            }]]
        }
        r acl setuser bloom-reader reset on nopass ~* +@bloom -@write
        assert_equal OK [r acl dryrun bloom-reader bf.exists bf a]
        assert_match {*no permissions*} [r acl dryrun bloom-reader bf.add bf a]
        r acl deluser bloom-reader
    }

    test {Bloom options validate ranges, tokens, and arity before creating keys} {
        foreach error {0 -1 1 2 nan inf -inf invalid} {
            assert_error {*error rate*} {r bf.reserve bad $error 10}
            assert_error {*error rate*} {r bf.insert bad error $error items a}
        }
        foreach capacity {0 -1 1073741825 invalid} {
            assert_error {*capacity*} {r bf.reserve bad 0.01 $capacity}
            assert_error {*capacity*} {r bf.insert bad capacity $capacity items a}
        }
        foreach expansion {-1 32769 invalid} {
            assert_error {*expansion*} {r bf.reserve bad 0.01 10 expansion $expansion}
            assert_error {*expansion*} {r bf.insert bad expansion $expansion items a}
        }
        assert_error {*no expansion*} {r bf.reserve bad 0.01 10 expansion}
        assert_error {*Nonscaling*} {r bf.reserve bad 0.01 10 nonscaling expansion 2}
        assert_error {*Nonscaling*} {r bf.reserve bad 0.01 10 expansion 2 nonscaling}
        assert_error {*syntax*} {r bf.reserve bad 0.01 10 junk}
        foreach args {{error} {items} {capacity 10} {expansion}} {
            assert_error {*wrong number*} {r bf.insert bad {*}$args}
        }
        foreach arg {i n e c {} {ITEMSx}} {
            assert_error {*Unknown argument*} {r bf.insert bad $arg a}
        }
        assert_equal 0 [r exists bad]
        assert_error {*wrong number*} {r bf.info bf items extra}
    }

    test {Bloom growth matches the module fixture and preserves expiry} {
        r bf.reserve grown 0.01 2 expansion 2
        r pexpire grown 60000
        for {set i 0} {$i < 10} {incr i} {
            assert_equal 1 [r bf.add grown [binary format ccc $i 0 -1]]
        }
        assert_equal [native_bloom_payload] [r dump grown]
        assert_equal {3} [r bf.info grown filters]
        assert {[r pttl grown] > 0}
        assert_equal size:10 [lindex [r bf.debug grown] 0]
        r persist grown
    }

    test {Bloom nonscaling partial insertion stops at full and permits duplicates} {
        r bf.reserve full 0.000001 2 nonscaling
        assert_error {*non scaling filter is full*} {r bf.madd full a b c d}
        assert_equal 2 [r bf.card full]
        assert_equal {1 1 0 0} [r bf.mexists full a b c d]
        assert_equal 0 [r bf.add full a]
        assert_error {*non scaling filter is full*} {r bf.add full c}
        assert_equal {{}} [r bf.info full expansion]
        assert_error {*item exists*} {r bf.reserve full 0.01 10}
    }

    test {Bloom rejects impossible growth without losing earlier inserts} {
        r bf.reserve tiny 1e-323 1
        assert_error {*problem inserting into filter*} {r bf.madd tiny a b a}
        assert_equal 1 [r bf.card tiny]
        assert_equal {1} [r bf.info tiny filters]
        assert_equal {1 0} [r bf.mexists tiny a b]
    }

    test {Bloom SCANDUMP LOADCHUNK round trip and invalid chunk rejection} {
        set iter 0
        while 1 {
            lassign [r bf.scandump grown $iter] iter data
            if {$iter == 0} {
                assert_equal {} $data
                break
            }
            assert_equal OK [r bf.loadchunk chunks $iter $data]
        }
        assert_equal [r dump grown] [r dump chunks]
        assert_error {*numeric*} {r bf.scandump grown invalid}
        assert_error {*numeric*} {r bf.loadchunk chunks invalid data}
        assert_error {*} {r bf.loadchunk bad-header 1 garbage}
        assert_equal 0 [r exists bad-header]
        set before [r dump chunks]
        foreach iter {-1 0 1 9223372036854775807} {
            assert_error {*} {r bf.loadchunk chunks $iter garbage}
        }
        assert_equal $before [r dump chunks]
        assert_error {*not found*} {r bf.loadchunk absent 2 data}
        assert_equal {0 {}} [r bf.scandump chunks -1]
    }

    test {Bloom RESP3 returns booleans and INFO maps} {
        r hello 3
        r readraw 1
        assert_equal #t [r bf.exists bf b]
        r readraw 0
        assert_equal {1 0} [r bf.mexists bf b missing]
        assert_equal {1 0} [r bf.madd resp3 a a]
        assert_equal 1 [dict get [r bf.info resp3] {Number of items inserted}]
        assert_equal 100 [dict get [r bf.info resp3 capacity] Capacity]
        assert_equal {} [dict get [r bf.info full expansion] {Expansion rate}]
        r hello 2
    }

    test {Bloom defaults and CONFIG SET validation and rollback} {
        r config set bf-error-rate 0.02 bf-initial-size 3 bf-expansion-factor 0
        r bf.add defaults a
        assert_equal {3} [r bf.info defaults capacity]
        assert_equal {{}} [r bf.info defaults expansion]
        assert_match {*ratio:0.02} [lindex [r bf.debug defaults] 1]
        r config set bf-error-rate 0.9
        assert_equal 0.25 [lindex [r config get bf-error-rate] 1]
        foreach value {0 1 nan inf invalid} {
            assert_error {*} {r config set bf-error-rate $value}
        }
        assert_error {*} {r config set bf-error-rate 0.1 bf-initial-size 0}
        assert_equal 0.25 [lindex [r config get bf-error-rate] 1]
        assert_error {*} {r config set bf-expansion-factor 32769}
        assert_error {*} {r config set bf-initial-size 1073741825}
        r config set bf-error-rate 0.01 bf-initial-size 100 bf-expansion-factor 2
    }

    test {Bloom mutations invalidate WATCH, while duplicate inserts do not} {
        set observer [redis [srv host] [srv port] 0 $::tls]
        $observer select 9
        $observer watch watched
        r bf.reserve watched 0.000001 100
        $observer multi
        $observer ping
        assert_equal {} [$observer exec]
        $observer watch watched
        r bf.add watched a
        $observer multi
        $observer ping
        assert_equal {} [$observer exec]
        $observer watch watched
        r bf.add watched a
        $observer multi
        $observer ping
        assert_equal PONG [$observer exec]
        $observer watch watched
        lassign [r bf.scandump watched 1] iter data
        r bf.loadchunk watched $iter $data
        $observer multi
        $observer ping
        assert_equal {} [$observer exec]
        $observer close
    }

    test {Bloom reads participate in client tracking invalidation} {
        set subscriber [redis_deferring_client]
        $subscriber client id
        set id [$subscriber read]
        $subscriber subscribe __redis__:invalidate
        $subscriber read
        r client tracking on redirect $id
        r bf.exists tracked a
        set writer [redis_client]
        $writer bf.add tracked a
        assert_equal {message __redis__:invalidate tracked} [$subscriber read]
        r client tracking off
        $subscriber close
        $writer close
    }

    test {Bloom mutation from Lua and MULTI works with propagation} {
        assert_equal {1 0} [r eval {return redis.call('BF.MADD',KEYS[1],'a','a')} 1 lua-bf]
        r multi
        r bf.reserve multi-bf 0.01 3
        r bf.add multi-bf a
        assert_equal {OK 1} [r exec]
    }

    test {Bloom command writes survive incremental AOF with changed defaults} {
        r config set appendonly yes
        waitForBgrewriteaof r
        r config set bf-initial-size 2 bf-error-rate 0.001 bf-expansion-factor 1
        r bf.madd aof-bf a b c d e f
        r bf.reserve aof-empty 0.025 7 nonscaling
        r bf.reserve aof-full 0.000001 1 nonscaling
        assert_error {*non scaling filter is full*} {r bf.madd aof-full a b c}
        r bf.reserve aof-tiny 1e-323 1
        assert_error {*problem inserting into filter*} {r bf.madd aof-tiny a b a}
        set payloads [list [r dump aof-bf] [r dump aof-empty] [r dump aof-full] [r dump aof-tiny]]
        r config set bf-initial-size 100 bf-error-rate 0.01 bf-expansion-factor 2
        r debug loadaof
        assert_equal $payloads [list [r dump aof-bf] [r dump aof-empty] [r dump aof-full] [r dump aof-tiny]]
        assert_equal OK [r config set appendonly no]
    } {} {needs:debug}

    start_server {tags {"bloom needs:repl external:skip"} overrides {save ""}} {
        set master [srv -1 client]
        set replica [srv 0 client]
        test {Bloom replication preserves geometry and partial writes with different defaults} {
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_sync $replica
            $master config set bf-initial-size 2 bf-error-rate 0.001 bf-expansion-factor 1
            $master bf.madd replicated a b c d e f
            $master bf.reserve replicated-empty 0.001 10 nonscaling
            $master bf.reserve replicated-full 0.000001 1 nonscaling
            assert_error {*non scaling filter is full*} {$master bf.madd replicated-full a b c}
            $master bf.reserve replicated-tiny 1e-323 1
            assert_error {*problem inserting into filter*} {$master bf.madd replicated-tiny a b a}
            $master eval {return redis.call('BF.MADD',KEYS[1],'x','y')} 1 replicated-lua
            $master multi
            $master bf.reserve replicated-multi 0.01 2
            $master bf.add replicated-multi a
            $master exec
            wait_for_ofs_sync $master $replica
            foreach key {grown chunks replicated replicated-empty replicated-full replicated-tiny replicated-lua replicated-multi} {
                assert_equal [$master dump $key] [$replica dump $key]
            }
            $master config set bf-initial-size 100 bf-error-rate 0.01 bf-expansion-factor 2
        }
    }
}

start_server {tags {"bloom external:skip"} overrides {save ""}} {
    test {Bloom CONFIG REWRITE persists defaults across restart} {
        r config set bf-error-rate 0.02 bf-initial-size 7 bf-expansion-factor 0
        r config rewrite
        restart_server 0 true false
        assert_equal {bf-error-rate 0.02} [r config get bf-error-rate]
        assert_equal {bf-initial-size 7} [r config get bf-initial-size]
        assert_equal {bf-expansion-factor 0} [r config get bf-expansion-factor]
        r bf.add configured a
        assert_match {*capacity:7*ratio:0.02} [lindex [r bf.debug configured] 1]
    }
}
