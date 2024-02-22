start_server {tags {"other"}} {
    if {$::force_failure} {
        # This is used just for test suite development purposes.
        test {Failing test} {
            format err
        } {ok}
    }

    test {Coverage: HELP commands} {
        assert_match "*OBJECT <subcommand> *" [r OBJECT HELP]
        assert_match "*MEMORY <subcommand> *" [r MEMORY HELP]
        assert_match "*PUBSUB <subcommand> *" [r PUBSUB HELP]
        assert_match "*SLOWLOG <subcommand> *" [r SLOWLOG HELP]
        assert_match "*CLIENT <subcommand> *" [r CLIENT HELP]
        assert_match "*COMMAND <subcommand> *" [r COMMAND HELP]
        assert_match "*CONFIG <subcommand> *" [r CONFIG HELP]
        assert_match "*FUNCTION <subcommand> *" [r FUNCTION HELP]
        assert_match "*MODULE <subcommand> *" [r MODULE HELP]
    }

    test {Coverage: MEMORY MALLOC-STATS} {
        if {[string match {*jemalloc*} [s mem_allocator]]} {
            assert_match "*jemalloc*" [r memory malloc-stats]
        }
    }

    test {Coverage: MEMORY PURGE} {
        if {[string match {*jemalloc*} [s mem_allocator]]} {
            assert_equal {OK} [r memory purge]
        }
    }

    test {SAVE - make sure there are all the types as values} {
        # Wait for a background saving in progress to terminate
        waitForBgsave r
        r lpush mysavelist hello
        r lpush mysavelist world
        r set myemptykey {}
        r set mynormalkey {blablablba}
        r zadd mytestzset 10 a
        r zadd mytestzset 20 b
        r zadd mytestzset 30 c
        r save
    } {OK} {needs:save}

    tags {slow} {
        if {$::accurate} {set iterations 10000} else {set iterations 1000}
        foreach fuzztype {binary alpha compr} {
            test "FUZZ stresser with data model $fuzztype" {
                set err 0
                for {set i 0} {$i < $iterations} {incr i} {
                    set fuzz [randstring 0 512 $fuzztype]
                    r set foo $fuzz
                    set got [r get foo]
                    if {$got ne $fuzz} {
                        set err [list $fuzz $got]
                        break
                    }
                }
                set _ $err
            } {0}
        }
    }

    start_server {overrides {save ""} tags {external:skip}} {
        test {FLUSHALL should not reset the dirty counter if we disable save} {
            r set key value
            r flushall
            assert_morethan [s rdb_changes_since_last_save] 0
        }

        test {FLUSHALL should reset the dirty counter to 0 if we enable save} {
            r config set save "3600 1 300 100 60 10000"
            r set key value
            r flushall
            assert_equal [s rdb_changes_since_last_save] 0
        }
    }

    test {BGSAVE} {
        # Use FLUSHALL instead of FLUSHDB, FLUSHALL do a foreground save
        # and reset the dirty counter to 0, so we won't trigger an unexpected bgsave.
        r flushall
        r save
        r set x 10
        r bgsave
        waitForBgsave r
        r debug reload
        r get x
    } {10} {needs:debug needs:save}

    test {SELECT an out of range DB} {
        catch {r select 1000000} err
        set _ $err
    } {*index is out of range*} {cluster:skip}

    tags {consistency} {
        proc check_consistency {dumpname code} {
            set dump [csvdump r]
            set sha1 [debug_digest]

            uplevel 1 $code

            set sha1_after [debug_digest]
            if {$sha1 eq $sha1_after} {
                return 1
            }

            # Failed
            set newdump [csvdump r]
            puts "Consistency test failed!"
            puts "You can inspect the two dumps in /tmp/${dumpname}*.txt"

            set fd [open /tmp/${dumpname}1.txt w]
            puts $fd $dump
            close $fd
            set fd [open /tmp/${dumpname}2.txt w]
            puts $fd $newdump
            close $fd

            return 0
        }

        if {$::accurate} {set numops 10000} else {set numops 1000}
        test {Check consistency of different data types after a reload} {
            r flushdb
            createComplexDataset r $numops usetag
            if {$::ignoredigest} {
                set _ 1
            } else {
                check_consistency {repldump} {
                    r debug reload
                }
            }
        } {1} {needs:debug}

        test {Same dataset digest if saving/reloading as AOF?} {
            if {$::ignoredigest} {
                set _ 1
            } else {
                check_consistency {aofdump} {
                    r config set aof-use-rdb-preamble no
                    r bgrewriteaof
                    waitForBgrewriteaof r
                    r debug loadaof
                }
            }
        } {1} {needs:debug}
    }

    test {EXPIRES after a reload (snapshot + append only file rewrite)} {
        r flushdb
        r set x 10
        r expire x 1000
        r save
        r debug reload
        set ttl [r ttl x]
        set e1 [expr {$ttl > 900 && $ttl <= 1000}]
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        set ttl [r ttl x]
        set e2 [expr {$ttl > 900 && $ttl <= 1000}]
        list $e1 $e2
    } {1 1} {needs:debug needs:save}

    test {EXPIRES after AOF reload (without rewrite)} {
        r flushdb
        r config set appendonly yes
        r config set aof-use-rdb-preamble no
        r set x somevalue
        r expire x 1000
        r setex y 2000 somevalue
        r set z somevalue
        r expireat z [expr {[clock seconds]+3000}]

        # Milliseconds variants
        r set px somevalue
        r pexpire px 1000000
        r psetex py 2000000 somevalue
        r set pz somevalue
        r pexpireat pz [expr {([clock seconds]+3000)*1000}]

        # Reload and check
        waitForBgrewriteaof r
        # We need to wait two seconds to avoid false positives here, otherwise
        # the DEBUG LOADAOF command may read a partial file.
        # Another solution would be to set the fsync policy to no, since this
        # prevents write() to be delayed by the completion of fsync().
        after 2000
        r debug loadaof
        set ttl [r ttl x]
        assert {$ttl > 900 && $ttl <= 1000}
        set ttl [r ttl y]
        assert {$ttl > 1900 && $ttl <= 2000}
        set ttl [r ttl z]
        assert {$ttl > 2900 && $ttl <= 3000}
        set ttl [r ttl px]
        assert {$ttl > 900 && $ttl <= 1000}
        set ttl [r ttl py]
        assert {$ttl > 1900 && $ttl <= 2000}
        set ttl [r ttl pz]
        assert {$ttl > 2900 && $ttl <= 3000}
        r config set appendonly no
    } {OK} {needs:debug}

    tags {protocol} {
        test {PIPELINING stresser (also a regression for the old epoll bug)} {
            if {$::tls} {
                set fd2 [::tls::socket [srv host] [srv port]]
            } else {
                set fd2 [socket [srv host] [srv port]]
            }
            fconfigure $fd2 -encoding binary -translation binary
            if {!$::singledb} {
                puts -nonewline $fd2 "SELECT 9\r\n"
                flush $fd2
                gets $fd2
            }

            for {set i 0} {$i < 100000} {incr i} {
                set q {}
                set val "0000${i}0000"
                append q "SET key:$i $val\r\n"
                puts -nonewline $fd2 $q
                set q {}
                append q "GET key:$i\r\n"
                puts -nonewline $fd2 $q
            }
            flush $fd2

            for {set i 0} {$i < 100000} {incr i} {
                gets $fd2 line
                gets $fd2 count
                set count [string range $count 1 end]
                set val [read $fd2 $count]
                read $fd2 2
            }
            close $fd2
            set _ 1
        } {1}
    }

    test {APPEND basics} {
        r del foo
        list [r append foo bar] [r get foo] \
             [r append foo 100] [r get foo]
    } {3 bar 6 bar100}

    test {APPEND basics, integer encoded values} {
        set res {}
        r del foo
        r append foo 1
        r append foo 2
        lappend res [r get foo]
        r set foo 1
        r append foo 2
        lappend res [r get foo]
    } {12 12}

    test {APPEND fuzzing} {
        set err {}
        foreach type {binary alpha compr} {
            set buf {}
            r del x
            for {set i 0} {$i < 1000} {incr i} {
                set bin [randstring 0 10 $type]
                append buf $bin
                r append x $bin
            }
            if {$buf != [r get x]} {
                set err "Expected '$buf' found '[r get x]'"
                break
            }
        }
        set _ $err
    } {}

    # Leave the user with a clean DB before to exit
    test {FLUSHDB} {
        set aux {}
        if {$::singledb} {
            r flushdb
            lappend aux 0 [r dbsize]
        } else {
            r select 9
            r flushdb
            lappend aux [r dbsize]
            r select 10
            r flushdb
            lappend aux [r dbsize]
        }
    } {0 0}

    test {Perform a final SAVE to leave a clean DB on disk} {
        waitForBgsave r
        r save
    } {OK} {needs:save}

    test {RESET clears client state} {
        r client setname test-client
        r client tracking on

        assert_equal [r reset] "RESET"
        set client [r client list]
        assert_match {*name= *} $client
        assert_match {*flags=N *} $client
    } {} {needs:reset}

    test {RESET clears MONITOR state} {
        set rd [redis_deferring_client]
        $rd monitor
        assert_equal [$rd read] "OK"

        $rd reset
        assert_equal [$rd read] "RESET"
        $rd close

        assert_no_match {*flags=O*} [r client list]
    } {} {needs:reset}

    test {RESET clears and discards MULTI state} {
        r multi
        r set key-a a

        r reset
        catch {r exec} err
        assert_match {*EXEC without MULTI*} $err
    } {} {needs:reset}

    test {RESET clears Pub/Sub state} {
        r subscribe channel-1
        r reset

        # confirm we're not subscribed by executing another command
        r set key val
    } {OK} {needs:reset}

    test {RESET clears authenticated state} {
        r acl setuser user1 on >secret +@all
        r auth user1 secret
        assert_equal [r acl whoami] user1

        r reset

        assert_equal [r acl whoami] default
    } {} {needs:reset}

    test "Subcommand syntax error crash (issue #10070)" {
        assert_error {*unknown command*} {r GET|}
        assert_error {*unknown command*} {r GET|SET}
        assert_error {*unknown command*} {r GET|SET|OTHER}
        assert_error {*unknown command*} {r CONFIG|GET GET_XX}
        assert_error {*unknown subcommand*} {r CONFIG GET_XX}
    }
}

start_server {tags {"other external:skip"}} {
    test {Don't rehash if redis has child process} {
        r config set save ""
        r config set rdb-key-save-delay 1000000

        populate 4095 "" 1
        r bgsave
        wait_for_condition 10 100 {
            [s rdb_bgsave_in_progress] eq 1
        } else {
            fail "bgsave did not start in time"
        }

        r mset k1 v1 k2 v2
        # Hash table should not rehash
        assert_no_match "*table size: 8192*" [r debug HTSTATS 9]
        exec kill -9 [get_child_pid 0]
        waitForBgsave r

        # Hash table should rehash since there is no child process,
        # size is power of two and over 4096, so it is 8192
        wait_for_condition 50 100 {
            [string match "*table size: 8192*" [r debug HTSTATS 9]]
        } else {
            fail "hash table did not rehash after child process killed"
        }
    } {} {needs:debug needs:local-process}
}

proc read_proc_title {pid} {
    set fd [open "/proc/$pid/cmdline" "r"]
    set cmdline [read $fd 1024]
    close $fd

    return $cmdline
}

start_server {tags {"other external:skip"}} {
    test {Process title set as expected} {
        # Test only on Linux where it's easy to get cmdline without relying on tools.
        # Skip valgrind as it messes up the arguments.
        set os [exec uname]
        if {$os == "Linux" && !$::valgrind} {
            # Set a custom template
            r config set "proc-title-template" "TEST {title} {listen-addr} {port} {tls-port} {unixsocket} {config-file}"
            set cmdline [read_proc_title [srv 0 pid]]

            assert_equal "TEST" [lindex $cmdline 0]
            assert_match "*/redis-server" [lindex $cmdline 1]
            
            if {$::tls} {
                set expect_port [srv 0 pport]
                set expect_tls_port [srv 0 port]
                set port [srv 0 pport]
            } else {
                set expect_port [srv 0 port]
                set expect_tls_port 0
                set port [srv 0 port]
            }

            assert_equal "$::host:$port" [lindex $cmdline 2]
            assert_equal $expect_port [lindex $cmdline 3]
            assert_equal $expect_tls_port [lindex $cmdline 4]
            assert_match "*/tests/tmp/server.*/socket" [lindex $cmdline 5]
            assert_match "*/tests/tmp/redis.conf.*" [lindex $cmdline 6]

            # Try setting a bad template
            catch {r config set "proc-title-template" "{invalid-var}"} err
            assert_match {*template format is invalid*} $err
        }
    }
}

start_cluster 1 0 {tags {"other external:skip cluster slow"}} {
    r config set dynamic-hz no hz 500
    test "Redis can trigger resizing" {
        r flushall
        # hashslot(foo) is 12182
        for {set j 1} {$j <= 128} {incr j} {
            r set "{foo}$j" a
        }
        assert_match "*table size: 128*" [r debug HTSTATS 0]

        # disable resizing, the reason for not using slow bgsave is because
        # it will hit the dict_force_resize_ratio.
        r debug dict-resizing 0

        # delete data to have lot's (96%) of empty buckets
        for {set j 1} {$j <= 123} {incr j} {
            r del "{foo}$j"
        }
        assert_match "*table size: 128*" [r debug HTSTATS 0]

        # enable resizing
        r debug dict-resizing 1

        # waiting for serverCron to resize the tables
        wait_for_condition 1000 10 {
            [string match {*table size: 8*} [r debug HTSTATS 0]]
        } else {
            puts [r debug HTSTATS 0]
            fail "hash tables weren't resize."
        }
    } {} {needs:debug}

    test "Redis can rewind and trigger smaller slot resizing" {
        # hashslot(foo) is 12182
        # hashslot(alice) is 749, smaller than hashslot(foo),
        # attempt to trigger a resize on it, see details in #12802.
        for {set j 1} {$j <= 128} {incr j} {
            r set "{alice}$j" a
        }

        # disable resizing, the reason for not using slow bgsave is because
        # it will hit the dict_force_resize_ratio.
        r debug dict-resizing 0

        for {set j 1} {$j <= 123} {incr j} {
            r del "{alice}$j"
        }

        # enable resizing
        r debug dict-resizing 1

        # waiting for serverCron to resize the tables
        wait_for_condition 1000 10 {
            [string match {*table size: 16*} [r debug HTSTATS 0]]
        } else {
            puts [r debug HTSTATS 0]
            fail "hash tables weren't resize."
        }
    } {} {needs:debug}
}

start_server {tags {"other external:skip"}} {
    test "Redis can resize empty dict" {
        # Write and then delete 128 keys, creating an empty dict
        r flushall
        for {set j 1} {$j <= 128} {incr j} {
            r set $j{b} a
        }
        for {set j 1} {$j <= 128} {incr j} {
            r del $j{b}
        }
        # The dict containing 128 keys must have expanded,
        # its hash table itself takes a lot more than 400 bytes
        wait_for_condition 100 50 {
            [dict get [r memory stats] db.9 overhead.hashtable.main] < 400
        } else {
            fail "dict did not resize in time"
        }   
    }
}
