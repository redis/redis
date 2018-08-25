start_server {tags {"other"}} {
    if {$::force_failure} {
        # This is used just for test suite development purposes.
        test {Failing test} {
            format err
        } {ok}
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
    } {OK}

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

    test {BGSAVE} {
        waitForBgsave r
        r flushdb
        r save
        r set x 10
        r bgsave
        waitForBgsave r
        r debug reload
        r get x
    } {10}

    test {SELECT an out of range DB} {
        catch {r select 1000000} err
        set _ $err
    } {*index is out of range*}

    tags {consistency} {
        if {![catch {package require sha1}]} {
            if {$::accurate} {set numops 10000} else {set numops 1000}
            test {Check consistency of different data types after a reload} {
                r flushdb
                createComplexDataset r $numops
                set dump [csvdump r]
                set sha1 [r debug digest]
                r debug reload
                set sha1_after [r debug digest]
                if {$sha1 eq $sha1_after} {
                    set _ 1
                } else {
                    set newdump [csvdump r]
                    puts "Consistency test failed!"
                    puts "You can inspect the two dumps in /tmp/repldump*.txt"

                    set fd [open /tmp/repldump1.txt w]
                    puts $fd $dump
                    close $fd
                    set fd [open /tmp/repldump2.txt w]
                    puts $fd $newdump
                    close $fd

                    set _ 0
                }
            } {1}

            test {Same dataset digest if saving/reloading as AOF?} {
                r config set aof-use-rdb-preamble no
                r bgrewriteaof
                waitForBgrewriteaof r
                r debug loadaof
                set sha1_after [r debug digest]
                if {$sha1 eq $sha1_after} {
                    set _ 1
                } else {
                    set newdump [csvdump r]
                    puts "Consistency test failed!"
                    puts "You can inspect the two dumps in /tmp/aofdump*.txt"

                    set fd [open /tmp/aofdump1.txt w]
                    puts $fd $dump
                    close $fd
                    set fd [open /tmp/aofdump2.txt w]
                    puts $fd $newdump
                    close $fd

                    set _ 0
                }
            } {1}
        }
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
    } {1 1}

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
    }

    tags {protocol} {
        test {PIPELINING stresser (also a regression for the old epoll bug)} {
            set fd2 [socket $::host $::port]
            fconfigure $fd2 -encoding binary -translation binary
            puts -nonewline $fd2 "SELECT 9\r\n"
            flush $fd2
            gets $fd2

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
        r select 9
        r flushdb
        lappend aux [r dbsize]
        r select 10
        r flushdb
        lappend aux [r dbsize]
    } {0 0}

    test {Perform a final SAVE to leave a clean DB on disk} {
        waitForBgsave r
        r save
    } {OK}
}
