# DUMP from RedisBloom: CF.RESERVE cf 4 BUCKETSIZE 2 MAXITERATIONS 20 EXPANSION 2,
# followed by CF.ADD for binary items {i,0,255}, i=0..29, then CF.DEL for i=0.
proc legacy_cuckoo_payload {} {
    return [binary format H* 07813016e5a28982140402040202021d020102020214020202020504221a005e02040508f09034dbed62d03302080510da390a446f00c787000004783e006eac0210052000000000000000007d000000d200000000000000a000af00b800500000000000000d00e60077b1b6c1a52a]
}

start_server {tags {"cuckoo"} overrides {save ""}} {
    test {Legacy Cuckoo DUMP loads as a native object without RedisBloom} {
        assert_equal OK [r restore cf 0 [legacy_cuckoo_payload]]
        assert_equal cuckoo [r type cf]
        assert_equal cuckoo [r object encoding cf]
        foreach module [r module list] {
            assert {[dict get $module name] ne "bf"}
        }
    }

    test {Cuckoo read commands preserve membership and counts after module import} {
        for {set i 1} {$i < 30} {incr i} {
            set item [binary format ccc $i 0 -1]
            assert_equal 1 [r cf.exists cf $item]
            assert {[r cf.count cf $item] >= 1}
        }
        set a [binary format ccc 1 0 -1]
        set b [binary format ccc 2 0 -1]
        assert_equal {1 1} [r cf.mexists cf $a $b]
        set info [r cf.info cf]
        assert_equal 29 [dict get $info {Number of items inserted}]
        assert_equal 1 [dict get $info {Number of items deleted}]
        assert_equal 4 [dict get $info {Number of filters}]
        assert_equal 2 [dict get $info {Bucket size}]
        assert_equal 2 [dict get $info {Expansion rate}]
        assert_equal 20 [dict get $info {Max iterations}]
        assert {[dict get $info Size] > 0}
        assert_equal {bktsize:2 buckets:2 items:29 deletes:1 filters:4 max_iterations:20 expansion:2} [r cf.debug cf]
        set before [r dump cf]
        r hello 3
        assert_equal 1 [r cf.exists cf $a]
        assert_equal {1 1} [r cf.mexists cf $a $b]
        assert {[r cf.count cf $a] >= 1}
        assert_equal $info [r cf.info cf]
        r hello 2
        assert_equal $before [r dump cf]
    }

    test {Cuckoo read commands preserve module missing-key and wrong-type behavior} {
        r set wrongtype x
        foreach key {missing wrongtype} {
            assert_equal 0 [r cf.exists $key x]
            assert_equal 0 [r cf.count $key x]
            assert_equal {0 0} [r cf.mexists $key x y]
        }
        foreach cmd {cf.info cf.debug} {
            assert_error {*not found*} {r $cmd missing}
            assert_error WRONGTYPE* {r $cmd wrongtype}
        }
        foreach cmd {cf.exists cf.count cf.mexists} {
            assert_error {*wrong number of arguments*} {r $cmd cf}
        }
        foreach cmd {cf.info cf.debug} {
            assert_error {*wrong number of arguments*} {r $cmd cf extra}
        }
        r del wrongtype
    }

    test {Cuckoo read commands have dedicated ACL category and key specifications} {
        assert_equal {cf.add cf.addnx cf.compact cf.count cf.debug cf.del cf.exists cf.info cf.insert cf.insertnx cf.loadchunk cf.mexists cf.reserve cf.scandump} [lsort [r acl cat cuckoo]]
        foreach cmd {cf.exists cf.count cf.mexists} {
            assert_equal cf [r command getkeys $cmd cf item]
        }
        foreach cmd {cf.info cf.debug} {
            assert_equal cf [r command getkeys $cmd cf]
        }
    }

    test {Native Cuckoo supports COPY memory accounting and SCAN TYPE} {
        assert {[r memory usage cf] > 100}
        assert_equal 1 [r copy cf copied]
        assert_equal [r dump cf] [r dump copied]
        assert_equal [r debug digest-value cf] [r debug digest-value copied]
        assert_equal {cf copied} [lsort [lindex [r scan 0 type cuckoo] 1]]
        assert_equal 1 [r unlink copied]
    } {} {needs:debug}

    test {Other type commands reject native Cuckoo objects} {
        assert_error WRONGTYPE* {r get cf}
        assert_error WRONGTYPE* {r sadd cf x}
        assert_error WRONGTYPE* {r bf.add cf x}
    }

    test {Native Cuckoo DUMP RESTORE preserves contents} {
        set payload [r dump cf]
        assert_equal OK [r restore copied 0 $payload]
        assert_equal $payload [r dump copied]
        assert_equal 1 [r del copied]
    }

    test {Cuckoo survives native RDB reload and redis-check-rdb} {
        set payload [r dump cf]
        r pexpire cf 60000
        r debug reload
        assert_equal $payload [r dump cf]
        assert {[r pttl cf] > 0}
        set path [file join [lindex [r config get dir] 1] dump.rdb]
        assert_match {*RDB looks OK*} [exec src/redis-check-rdb $path]
        r persist cf
    } {1} {needs:debug}

    test {Cuckoo rejects corrupt metadata and truncated native and module payloads} {
        r debug set-skip-checksum-validation 1
        set payload [r dump cf]
        # Native one-byte fields: filter count, base buckets, bucket size,
        # max iterations, expansion, and first subfilter buckets.
        foreach {offset value} {1 0 2 0 2 3 5 0 6 0 7 0 8 0 8 3 9 3 1 192} {
            set bad [string replace $payload $offset $offset [binary format c $value]]
            assert_error {*Bad data format*} {r restore bad 0 $bad}
            assert_equal 0 [r exists bad]
        }
        set legacy [legacy_cuckoo_payload]
        # Reject an unsupported module version, an invalid unsigned opcode,
        # and an invalid final module EOF opcode.
        foreach offset [list 9 10 [expr {[string length $legacy]-11}]] {
            set bad [string replace $legacy $offset $offset [binary format c 5]]
            assert_error {*Bad data format*} {r restore bad 0 $bad}
            assert_equal 0 [r exists bad]
        }
        r debug set-skip-checksum-validation 0
        foreach payload [list $payload [legacy_cuckoo_payload]] {
            for {set cut 1} {$cut < [string length $payload]-10} {incr cut} {
                set bad "[string range $payload 0 [expr {$cut-1}]][string range $payload end-9 end]"
                assert_error {*DUMP payload version or checksum are wrong*} {r restore bad 0 $bad}
                assert_equal 0 [r exists bad]
            }
        }
        r debug set-skip-checksum-validation 0
        assert_equal PONG [r ping]
    } {} {needs:debug}

    foreach preamble {no yes} {
        test "Cuckoo survives AOF rewrite with RDB preamble $preamble" {
            r config set aof-use-rdb-preamble $preamble
            r config set appendonly yes
            waitForBgrewriteaof r
            set payload [r dump cf]
            r pexpire cf 60000
            r bgrewriteaof
            waitForBgrewriteaof r
            r debug loadaof
            assert_equal $payload [r dump cf]
            assert {[r pttl cf] > 0}
            r persist cf
            r config set appendonly no
        } {OK} {needs:debug}
    }
}

start_server {tags {"cuckoo external:skip"} overrides {save ""}} {
    test {Cuckoo persistence defaults support CONFIG SET and bounds} {
        foreach {name default minimum maximum} {
            cf-bucket-size 2 1 255
            cf-max-iterations 20 1 65535
            cf-expansion-factor 1 0 32768
        } {
            assert_equal $default [lindex [r config get $name] 1]
            assert_equal OK [r config set $name $minimum]
            assert_equal OK [r config set $name $maximum]
            assert_error {*} {r config set $name [expr {$minimum-1}]}
            assert_error {*} {r config set $name [expr {$maximum+1}]}
            assert_equal $maximum [lindex [r config get $name] 1]
            r config set $name $default
        }
    }
    test {Cuckoo persistence defaults survive CONFIG REWRITE and restart} {
        r config set cf-bucket-size 4 cf-max-iterations 31 cf-expansion-factor 2
        r config rewrite
        restart_server 0 true false
        assert_equal 4 [lindex [r config get cf-bucket-size] 1]
        assert_equal 31 [lindex [r config get cf-max-iterations] 1]
        assert_equal 2 [lindex [r config get cf-expansion-factor] 1]
    }
}

start_server {tags {"cuckoo"} overrides {save "" key-memory-histograms yes}} {
    test {Cuckoo deletion and compaction preserve membership and module errors} {
        r restore cf 0 [legacy_cuckoo_payload]
        foreach protocol {2 3} {
            r hello $protocol
            set item [binary format ccc $protocol 0 -1]
            assert_equal 1 [r cf.del cf $item]
            assert_equal 0 [r cf.del cf $item]
            assert_equal OK [r cf.compact cf]
        }
        r hello 2
        assert_equal 27 [dict get [r cf.info cf] {Number of items inserted}]
        for {set i 4} {$i < 30} {incr i} {
            assert_equal 1 [r cf.exists cf [binary format ccc $i 0 -1]]
        }
        r set wrong x
        foreach key {wrong missing} {
            assert_error {*Not found*} {r cf.del $key x}
            assert_error {*Cuckoo filter was not found*} {r cf.compact $key}
        }
        assert_error {*wrong number of arguments*} {r cf.del cf}
        assert_error {*wrong number of arguments*} {r cf.compact cf extra}
    }
    test {Cuckoo mutations invalidate WATCH} {
        r watch cf
        r cf.compact cf
        r multi
        r ping
        assert_equal {} [r exec]
        r watch cf
        r cf.del cf [binary format ccc 4 0 -1]
        r multi
        r ping
        assert_equal {} [r exec]
    }
    test {Cuckoo mutation commands survive incremental AOF replay} {
        r config set appendonly yes
        waitForBgrewriteaof r
        r cf.del cf [binary format ccc 5 0 -1]
        r cf.compact cf
        set payload [r dump cf]
        r debug loadaof
        assert_equal $payload [r dump cf]
        r config set appendonly no
    } {OK} {needs:debug}
    start_server {tags {"cuckoo needs:repl external:skip"} overrides {save ""}} {
        test {Cuckoo deletion and compaction replicate the same filter} {
            set master [srv -1 client]
            set replica [srv 0 client]
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_sync $replica
            assert_error {*READONLY*} {$replica cf.compact cf}
            $master cf.del cf [binary format ccc 6 0 -1]
            $master cf.compact cf
            wait_for_ofs_sync $master $replica
            assert_equal [$master dump cf] [$replica dump cf]
        }
    }
}

start_server {tags {"cuckoo"} overrides {save ""}} {
    test {Cuckoo reserve creates a filter with explicit options} {
        r cf.reserve reserved 10 bucketsize 4 maxiterations 31 expansion 3
        set info [r cf.info reserved]
        assert_equal 4 [dict get $info {Bucket size}]
        assert_equal 31 [dict get $info {Max iterations}]
        assert_equal 4 [dict get $info {Expansion rate}]
        assert_equal 0 [dict get $info {Number of items inserted}]
        assert_error {*item exists*} {r cf.reserve reserved 10}
        r set wrong x
        assert_error WRONGTYPE* {r cf.reserve wrong 10}
        foreach args {{bad} {1} {1073741825} {10 BUCKETSIZE 0} {10 MAXITERATIONS 0} {10 EXPANSION -1} {10 BUCKETSIZE}} {
            assert_error {*} {r cf.reserve invalid {*}$args}
            assert_equal 0 [r exists invalid]
        }
    }
    test {Cuckoo creation settings enforce dependent bounds} {
        r config set cf-initial-size 8
        assert_error {*} {r config set cf-bucket-size 5}
        r config set cf-bucket-size 4
        assert_error {*} {r config set cf-initial-size 7}
        assert_error {*} {r cf.reserve bad-bucket 100 bucketsize 5}
        r config set cf-max-iterations 33 cf-expansion-factor 0 cf-max-expansions 16
        r cf.reserve defaults 8
        assert_equal 4 [dict get [r cf.info defaults] {Bucket size}]
        assert_equal 33 [dict get [r cf.info defaults] {Max iterations}]
        assert_equal 0 [dict get [r cf.info defaults] {Expansion rate}]
        r config set cf-initial-size 1024 cf-bucket-size 2 cf-max-iterations 20 cf-expansion-factor 1
    }
    test {Cuckoo reserve survives incremental AOF replay} {
        r config set appendonly yes
        waitForBgrewriteaof r
        r cf.reserve aof-reserved 16 bucketsize 2 maxiterations 30 expansion 2
        set payload [r dump aof-reserved]
        r debug loadaof
        assert_equal $payload [r dump aof-reserved]
        r config set appendonly no
    } {OK} {needs:debug}
    start_server {tags {"cuckoo needs:repl external:skip"} overrides {save ""}} {
        test {Cuckoo reserve replicates native creation} {
            set master [srv -1 client]
            set replica [srv 0 client]
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_sync $replica
            $master cf.reserve replicated-reserve 16 bucketsize 4 maxiterations 30 expansion 2
            wait_for_ofs_sync $master $replica
            assert_equal [$master dump replicated-reserve] [$replica dump replicated-reserve]
        }
    }
}

start_server {tags {"cuckoo"} overrides {save ""}} {
    test {Cuckoo add and insert preserve duplicate and NX semantics} {
        assert_equal 1 [r cf.add adds a]
        assert_equal 1 [r cf.add adds a]
        assert_equal 0 [r cf.addnx adds a]
        assert_equal 2 [r cf.count adds a]
        assert_equal {1 1} [r cf.insert inserts capacity 8 items a b]
        assert_equal {0 1} [r cf.insertnx inserts items a c]
        assert_error {*not found*} {r cf.insert missing nocreate items a}
        assert_equal 0 [r exists missing]
        r set wrong x
        assert_error WRONGTYPE* {r cf.add wrong a}
        assert_error {*Bad capacity*} {r cf.insert invalid capacity bad items a}
        assert_error {*wrong number*} {r cf.insert invalid items}
    }
    test {Cuckoo partial insertions and max expansion creation survive AOF} {
        r config set appendonly yes
        waitForBgrewriteaof r
        r cf.reserve full 4 expansion 0
        assert_equal {1 1 -1 -1 -1} [r cf.insert full items a a a a a]
        assert_error {*Filter is full*} {r cf.add full a}
        r hello 3
        assert_equal {0 0} [r cf.insert full items a a]
        assert_equal {0 0} [r cf.insertnx full items a a]
        r hello 2
        r config set cf-max-expansions 1
        assert_error {*Maximum expansions reached*} {r cf.add guarded a}
        assert_equal cuckoo [r type guarded]
        assert_equal 0 [dict get [r cf.info guarded] {Number of items inserted}]
        r config set cf-max-expansions 32
        set payloads [list [r dump full] [r dump guarded]]
        r config set propagation-error-behavior panic
        r debug loadaof
        assert_equal $payloads [list [r dump full] [r dump guarded]]
        r config set propagation-error-behavior ignore
        r config set appendonly no
    } {OK} {needs:debug}
    start_server {tags {"cuckoo needs:repl external:skip"} overrides {save ""}} {
        test {Cuckoo insertions replicate partial success and guarded creation} {
            set master [srv -1 client]
            set replica [srv 0 client]
            $replica config set propagation-error-behavior panic
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_sync $replica
            $master cf.insert replicated capacity 8 items a a b
            $master cf.insertnx replicated items a c
            $master cf.reserve partial 4 expansion 0
            $master cf.insert partial items a a a a a
            $master config set cf-max-expansions 1
            assert_error {*Maximum expansions reached*} {$master cf.add guarded-repl a}
            $master config set cf-max-expansions 32
            wait_for_ofs_sync $master $replica
            foreach key {replicated partial guarded-repl} {
                assert_equal [$master dump $key] [$replica dump $key]
            }
        }
    }
}

start_server {tags {"cuckoo"} overrides {save ""}} {
    test {Cuckoo chunks preserve membership metadata and DUMP} {
        r cf.insert source capacity 4 items a b c d e f g h i j
        set pos 0
        set chunks {}
        while 1 {
            lassign [r cf.scandump source $pos] pos data
            if {$pos == 0} break
            lappend chunks [list $pos $data]
            assert_equal OK [r cf.loadchunk restored $pos $data]
        }
        assert_equal [r dump source] [r dump restored]
        assert_equal [r cf.info source] [r cf.info restored]
        assert_equal {1 1 1} [r cf.mexists restored a e j]
        r cf.reserve empty 4
        assert_equal {0 {}} [r cf.scandump empty 0]
    }
    test {Cuckoo chunk loading rejects invalid headers and bounds} {
        set before [r dump restored]
        assert_error {*Invalid position*} {r cf.scandump source -1}
        assert_error {*Invalid position*} {r cf.loadchunk invalid 0 x}
        assert_error {*Invalid header*} {r cf.loadchunk invalid 1 x}
        set header [lindex [lindex $chunks 0] 1]
        foreach {offset bytes} {24 0000000000000000 24 0000010000000000 8 0000000000000000 32 0000 36 0000} {
            set replacement [binary format H* $bytes]
            set bad [string replace $header $offset [expr {$offset+[string length $replacement]-1}] $replacement]
            assert_error {*Couldn't create filter!*} {r cf.loadchunk invalid 1 $bad}
            assert_equal 0 [r exists invalid]
        }
        foreach pos {-1 2 9223372036854775807} {
            assert_error {*Couldn't load chunk!*} {r cf.loadchunk restored $pos abcdef}
        }
        assert_error {*Couldn't load chunk!*} {r cf.loadchunk restored 2 {}}
        assert_equal $before [r dump restored]
    }
    test {Cuckoo chunks survive incremental AOF replay} {
        r config set appendonly yes
        waitForBgrewriteaof r
        foreach chunk $chunks {
            lassign $chunk pos data
            r cf.loadchunk aof-chunks $pos $data
        }
        set payload [r dump aof-chunks]
        r debug loadaof
        assert_equal $payload [r dump aof-chunks]
        r config set appendonly no
    } {OK} {needs:debug}
    start_server {tags {"cuckoo needs:repl external:skip"} overrides {save ""}} {
        test {Cuckoo chunks replicate native filters} {
            set master [srv -1 client]
            set replica [srv 0 client]
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_sync $replica
            foreach chunk $chunks {
                lassign $chunk pos data
                $master cf.loadchunk replicated-chunks $pos $data
            }
            wait_for_ofs_sync $master $replica
            assert_equal [$master dump replicated-chunks] [$replica dump replicated-chunks]
        }
    }
}
