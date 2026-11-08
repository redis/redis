# Native Bloom fixture: ten binary items {i, 0, 255}, i=0..9, inserted into
# RedisBloom with capacity 2, error 0.01, expansion 2, FORCE64 | NOROUND.
# Three filters, encoded using the native RDB format and a valid DUMP checksum.
proc native_bloom_payload {} {
    return [binary format H* 250a030502027b14ae47e17a743f08e9862fb2350e264040400008008808008800e09f02047b14ae47e17a643f094af7d49edef028404040000855c8c8405527260604087b14ae47e17a543f0aaa677a8b87d32b404080001054128444444c24a005010d08540424020411008bfb631895c24bdd]
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
        set long_items {customer:12345678 customer:abcdefgh customer:12345678901234567890123456789012}
        r bf.madd long-native {*}$long_items
        r pexpire bloom 60000
        set digest [r debug digest-value bloom]
        r debug reload
        set dump_path [file join [lindex [r config get dir] 1] dump.rdb]
        assert_match {*RDB looks OK*} [exec src/redis-check-rdb $dump_path]
        assert_equal bloom [r type bloom]
        assert_equal $digest [r debug digest-value bloom]
        assert_equal {1 1 1} [r bf.mexists long-native {*}$long_items]
        r del long-native
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

    test {Bloom propagates original commands and omits failed items} {
        set repl [attach_to_replication_stream]
        r bf.reserve replay-fixed 0.000001 2 nonscaling
        r bf.add replay-added a
        r bf.insert replay-inserted capacity 2 error 0.000001 expansion 1 items a b
        assert_error {*non scaling filter is full*} {r bf.madd replay-fixed a b c}
        assert_error {*non scaling filter is full*} {r bf.add replay-fixed c}
        assert_error {*non scaling filter is full*} {r bf.insert replay-partial capacity 1 error 0.000001 nonscaling items a b}
        # A failed item in the middle must not discard later successful replies.
        r bf.reserve replay-tiny 1e-323 1
        assert_error {*problem inserting into filter*} {r bf.madd replay-tiny a b a}
        assert_replication_stream $repl {
            {select *}
            {bf.reserve replay-fixed 0.000001 2 nonscaling}
            {bf.add replay-added a}
            {bf.insert replay-inserted capacity 2 error 0.000001 expansion 1 items a b}
            {bf.madd replay-fixed a b}
            {bf.insert replay-partial capacity 1 error 0.000001 nonscaling items a}
            {bf.reserve replay-tiny 1e-323 1}
            {bf.madd replay-tiny a a}
        }
        close_replication_stream $repl
    } {} {needs:repl external:skip}

    test {Bloom command writes survive incremental AOF with matching defaults} {
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
        r config set propagation-error-behavior panic
        r debug loadaof
        assert_equal $payloads [list [r dump aof-bf] [r dump aof-empty] [r dump aof-full] [r dump aof-tiny]]
        r config set bf-initial-size 100 bf-error-rate 0.01 bf-expansion-factor 2
        r config set propagation-error-behavior ignore
        assert_equal OK [r config set appendonly no]
    } {} {needs:debug}

    start_server {tags {"bloom needs:repl external:skip"} overrides {save ""}} {
        set master [srv -1 client]
        set replica [srv 0 client]
        test {Bloom replication preserves geometry and partial writes with matching defaults} {
            $replica config set bf-initial-size 2 bf-error-rate 0.001 bf-expansion-factor 1
            $replica config set propagation-error-behavior panic
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_sync $replica
            $master config set bf-initial-size 2 bf-error-rate 0.001 bf-expansion-factor 1
            $master bf.madd replicated a b c d e f
            $master bf.reserve replicated-empty 0.001 10 nonscaling
            $master bf.reserve replicated-full 0.000001 1 nonscaling
            assert_error {*non scaling filter is full*} {$master bf.madd replicated-full a b c}
            assert_error {*non scaling filter is full*} {$master bf.insert replicated-partial capacity 1 error 0.000001 nonscaling items a b}
            $master bf.reserve replicated-tiny 1e-323 1
            assert_error {*problem inserting into filter*} {$master bf.madd replicated-tiny a b a}
            $master eval {return redis.call('BF.MADD',KEYS[1],'x','y')} 1 replicated-lua
            $master multi
            $master bf.reserve replicated-multi 0.01 2
            $master bf.add replicated-multi a
            $master exec
            wait_for_ofs_sync $master $replica
            foreach key {grown chunks replicated replicated-empty replicated-full replicated-partial replicated-tiny replicated-lua replicated-multi} {
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

# MBbloom-- encodings 0..3 use the original RedisBloom algorithm and historical
# field layouts; encoding 4 is an actual module DUMP. All contain {i,0,255},
# i=0..19. Version 0 uses 1102 bits in 138 bytes; its padding must not change
# the hash modulus. Versions 0/1/3 use 32-bit hashes, and 2/4 use 64-bit hashes.
proc legacy_bloom_payload {version} {
    set payloads {
        0 07813016e5a289bef80002140201024064047b14ae47e17a743f020804e9862fb2350e26400540900002094200048000615810000048800020000200820104820822020202004025100400018808600c801060040291c0020080002000800000208008201200008842001002228008400011a00b020088000018020200008000087208e8220000082002010408000080e010a000821002140201d001000040208000005002c27208000000030000820000000000000000000214000b0025e91036481e6d84
        1 07813016e5a289bef801021402010240b9047b14ae47e17a743f020804e9862fb2350e2640024800020b05410000e0040040000000000040040200800000200c12000000000000900402000010000044000000000000282001280100000011000002000001080040000004800000000040000200060aa04020808000000002000000009008800000001000022010a804028160010020000000222402000000200020250001200200000020000000008000030a00100000000200001008000080000000808302004000000012188802000000000200000080020002100480800440000000020000000800008080002100000000002001000084000000000840800080000008000000008020102240020800000080000020008002400000082000012c0a010028308000080000000214000b001d2afaa09014c39b
        2 07813016e5a289bef8020214020402050202047b14ae47e17a743f020804e9862fb2350e264002404002000508008808008800e09f02020204047b14ae47e17a643f0209044af7d49edef028400240400200050855c8c8405527260602040208047b14ae47e17a543f020a04aa677a8b87d32b40024080020005105c1a94e445ccaca18fc50d3c7c85ae6202080210047b14ae47e17a443f020b040bd81f7830b62e40024100020005200081000b800050851c1098408a880100852d9100608c083184140890404080110206000b0059ea4f67ff55b777
        3 07813016e5a289bef803021402010209024064047b14ae47e17a843f02070488168ac58c2b23400243c0020005407882a0c40000090008082020002020010a40288420130c000100220004002000072201c0000223133080000801010400040000a0800000808008b2801000020010801800022024900020a2080a0880822000009000220092000802044680000080280000010a20920208800480a202001010604400a04200000214000b005eb173d0efe8e3dc
        4 07813016e5a289bef80402140204020502020202047b14ae47e17a743f020804e9862fb2350e264002404002000508008808008800e09f02020204047b14ae47e17a643f0209044af7d49edef028400240400200050855c8c8405527260602040208047b14ae47e17a543f020a04aa677a8b87d32b40024080020005105c1a94e445ccaca18fc50d3c7c85ae6202080210047b14ae47e17a443f020b040bd81f7830b62e40024100020005200081000b800050851c1098408a880100852d9100608c08318414089040408011020600110020ee7eea0e5a6337
    }
    return [binary format H* [dict get $payloads $version]]
}

start_server {tags {"bloom"} overrides {save ""}} {
    test {Legacy Bloom DUMP encodings 0 through 4 become native objects} {
        foreach version {0 1 2 3 4} {
            set key legacy-$version
            assert_equal OK [r restore $key 60000 [legacy_bloom_payload $version]]
            assert_equal bloom [r type $key]
            assert_equal bloom [r object encoding $key]
            assert_equal 20 [r bf.card $key]
            for {set i 0} {$i < 20} {incr i} {
                assert_equal 1 [r bf.exists $key [binary format ccc $i 0 -1]]
            }
            assert {[r pttl $key] > 0}
            binary scan [r dump $key] c type
            assert_equal 37 $type
            r persist $key
        }
        assert_match {*bits:1102*hashwidth:32*} [lindex [r bf.debug legacy-0] 1]
        assert_match {*hashwidth:32*} [lindex [r bf.debug legacy-1] 1]
        assert_match {*hashwidth:64*} [lindex [r bf.debug legacy-2] 1]
        assert_match {*hashwidth:32*} [lindex [r bf.debug legacy-3] 1]
        assert_match {*hashwidth:64*} [lindex [r bf.debug legacy-4] 1]
    }

    test {Imported Bloom filters preserve native DUMP COPY and chunk round trips} {
        foreach version {0 1 2 3 4} {
            set key legacy-$version
            set payload [r dump $key]
            r restore restored 0 $payload
            assert_equal $payload [r dump restored]
            assert_equal 1 [r copy $key copied]
            assert_equal $payload [r dump copied]
            set iter 0
            while 1 {
                lassign [r bf.scandump $key $iter] iter data
                if {$iter == 0} break
                r bf.loadchunk chunks $iter $data
            }
            assert_equal $payload [r dump chunks]
            for {set i 0} {$i < 20} {incr i} {
                assert_equal 1 [r bf.exists chunks [binary format ccc $i 0 -1]]
            }
            r del restored copied chunks
        }
    }

    test {Imported Bloom filters can grow while retaining old membership} {
        foreach version {0 1 2 3 4} {
            set limit [expr {$version == 3 ? 40 : 200}]
            for {set i 20} {$i < $limit} {incr i} {
                r bf.add legacy-$version [binary format ccc $i 0 -1]
            }
            for {set i 0} {$i < $limit} {incr i} {
                assert_equal 1 [r bf.exists legacy-$version [binary format ccc $i 0 -1]]
            }
        }
        assert {[lindex [r bf.info legacy-0 filters] end] > 1}
        assert {[lindex [r bf.info legacy-1 filters] end] > 1}
    }

    test {Imported Bloom filters survive a native RDB reload} {
        set payloads {}
        foreach version {0 1 2 3 4} {lappend payloads [r dump legacy-$version]}
        r debug reload
        foreach version {0 1 2 3 4} {
            assert_equal [lindex $payloads $version] [r dump legacy-$version]
        }
    } {} {needs:debug}

    foreach preamble {no yes} {
        test "Imported Bloom filters survive AOF rewrite with RDB preamble $preamble" {
            r config set aof-use-rdb-preamble $preamble
            r config set appendonly yes
            waitForBgrewriteaof r
            set payloads {}
            foreach version {0 1 2 3 4} {lappend payloads [r dump legacy-$version]}
            r bgrewriteaof
            waitForBgrewriteaof r
            r debug loadaof
            foreach version {0 1 2 3 4} {
                assert_equal [lindex $payloads $version] [r dump legacy-$version]
            }
            assert_equal OK [r config set appendonly no]
        } {} {needs:debug}
    }

    test {Legacy Bloom rejects truncated payloads without replacing the destination} {
        r set destination original
        r debug set-skip-checksum-validation 1
        foreach version {0 1 2 3 4} {
            set payload [legacy_bloom_payload $version]
            for {set cut 1} {$cut < [string length $payload]-10} {incr cut} {
                set bad "[string range $payload 0 [expr {$cut-1}]][string range $payload end-9 end]"
                assert_error {*Bad data format*} {r restore destination 0 $bad replace}
            }
        }
        r debug set-skip-checksum-validation 0
        assert_equal original [r get destination]
    } {} {needs:debug}

    test {Legacy Bloom validates encoding version, opcodes, metadata, and EOF} {
        r debug set-skip-checksum-validation 1
        set payload [legacy_bloom_payload 4]
        # Module version; UINT/DOUBLE/STRING opcodes; size/count/options/growth;
        # hashes; n2; encoded length tags (which are illegal for integers).
        foreach {offset value} {9 5 10 1 10 192 11 0 12 4 13 0 14 4 15 255 16 1 17 0 18 192 20 2 29 4 30 0 31 2 40 4 43 4 44 64 45 2} {
            set bad [string replace $payload $offset $offset [binary format c $value]]
            assert_error {*Bad data format*} {r restore bad 0 $bad}
            assert_equal 0 [r exists bad]
        }
        foreach offset {21 32} {
            set bad [string replace $payload $offset [expr {$offset+7}] [binary format H* 000000000000f87f]]
            assert_error {*Bad data format*} {r restore bad 0 $bad}
        }
        set bad [string replace $payload end-10 end-10 [binary format c 1]]
        assert_error {*Bad data format*} {r restore bad 0 $bad}
        # Another module name must still follow the ordinary module loader.
        set other [string replace $payload 2 2 [binary format c 0]]
        assert_error {*Bad data format*} {r restore other 0 $other}
        r debug set-skip-checksum-validation 0
        assert_equal PONG [r ping]
    } {} {needs:debug}
}

# A complete historical RDB using the same module values and no checksum.
set legacy_dir [tmpdir bloom-legacy]
set legacy_path [file join $legacy_dir legacy.rdb]
set legacy_file [open $legacy_path wb]
puts -nonewline $legacy_file "REDIS0011[binary format H* fe09]"
foreach version {0 1 2 3 4} {
    set key legacy-$version
    set payload [legacy_bloom_payload $version]
    puts -nonewline $legacy_file [string index $payload 0]
    puts -nonewline $legacy_file [binary format ca* [string length $key] $key]
    puts -nonewline $legacy_file [string range $payload 1 end-10]
}
# Legacy-v4 long-item fixture; module hash word order follows the source host.
if {$::tcl_platform(byteOrder) eq "littleEndian"} {
    set long_payload [binary format H* 0201020102050202020a047b14ae47e17a843f02070484168ac58c2b23400240600200050c208800000002000088200000020100]
} else {
    set long_payload [binary format H* 0201020102050202020a047b14ae47e17a843f02070484168ac58c2b23400240600200050c200088000002004000100100020100]
}
puts -nonewline $legacy_file [binary format ca* 7 "\x0blegacy-long"]
puts -nonewline $legacy_file [binary format H* 813016e5a289bef804]
puts -nonewline $legacy_file $long_payload
puts -nonewline $legacy_file [binary format H* ff0000000000000000]
close $legacy_file

start_server [list tags {bloom external:skip} overrides [list dir $legacy_dir dbfilename legacy.rdb save ""]] {
    test {Legacy module RDB loads without RedisBloom and passes redis-check-rdb} {
        assert_match {*RDB looks OK*} [exec src/redis-check-rdb $legacy_path]
        foreach version {0 1 2 3 4} {
            assert_equal bloom [r type legacy-$version]
            assert_equal 20 [r bf.card legacy-$version]
            for {set i 0} {$i < 20} {incr i} {
                assert_equal 1 [r bf.exists legacy-$version [binary format ccc $i 0 -1]]
            }
        }
        assert_equal 1 [r bf.exists legacy-long customer:12345678]
        assert_equal 1 [r bf.card legacy-long]
        r bf.add legacy-long customer:abcdefgh
        r save
        restart_server 0 true false
        assert_equal {1 1} [r bf.mexists legacy-long customer:12345678 customer:abcdefgh]
        foreach version {0 1 2 3 4} {
            assert_equal bloom [r type legacy-$version]
            assert_equal 20 [r bf.card legacy-$version]
            assert_equal 1 [r bf.exists legacy-$version [binary format ccc 0 0 -1]]
        }
    }
}
