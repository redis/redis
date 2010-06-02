start_server {} {
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

    foreach fuzztype {binary alpha compr} {
        test "FUZZ stresser with data model $fuzztype" {
            set err 0
            for {set i 0} {$i < 10000} {incr i} {
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
    } {*invalid*}

    if {![catch {package require sha1}]} {
        test {Check consistency of different data types after a reload} {
            r flushdb
            createComplexDataset r 10000
            set sha1 [r debug digest]
            r debug reload
            set sha1_after [r debug digest]
            expr {$sha1 eq $sha1_after}
        } {1}

        test {Same dataset digest if saving/reloading as AOF?} {
            r bgrewriteaof
            waitForBgrewriteaof r
            r debug loadaof
            set sha1_after [r debug digest]
            expr {$sha1 eq $sha1_after}
        } {1}
    }

    test {EXPIRES after a reload (snapshot + append only file)} {
        r flushdb
        r set x 10
        r expire x 1000
        r save
        r debug reload
        set ttl [r ttl x]
        set e1 [expr {$ttl > 900 && $ttl <= 1000}]
        r bgrewriteaof
        waitForBgrewriteaof r
        set ttl [r ttl x]
        set e2 [expr {$ttl > 900 && $ttl <= 1000}]
        list $e1 $e2
    } {1 1}

    test {PIPELINING stresser (also a regression for the old epoll bug)} {
        set fd2 [socket $::host $::port]
        fconfigure $fd2 -encoding binary -translation binary
        puts -nonewline $fd2 "SELECT 9\r\n"
        flush $fd2
        gets $fd2

        for {set i 0} {$i < 100000} {incr i} {
            set q {}
            set val "0000${i}0000"
            append q "SET key:$i [string length $val]\r\n$val\r\n"
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

    test {MUTLI / EXEC basics} {
        r del mylist
        r rpush mylist a
        r rpush mylist b
        r rpush mylist c
        r multi
        set v1 [r lrange mylist 0 -1]
        set v2 [r ping]
        set v3 [r exec]
        list $v1 $v2 $v3
    } {QUEUED QUEUED {{a b c} PONG}}

    test {DISCARD} {
        r del mylist
        r rpush mylist a
        r rpush mylist b
        r rpush mylist c
        r multi
        set v1 [r del mylist]
        set v2 [r discard]
        set v3 [r lrange mylist 0 -1]
        list $v1 $v2 $v3
    } {QUEUED OK {a b c}}

    test {APPEND basics} {
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

    test {SUBSTR basics} {
        set res {}
        r set foo "Hello World"
        lappend res [r substr foo 0 3]
        lappend res [r substr foo 0 -1]
        lappend res [r substr foo -4 -1]
        lappend res [r substr foo 5 3]
        lappend res [r substr foo 5 5000]
        lappend res [r substr foo -5000 10000]
        set _ $res
    } {Hell {Hello World} orld {} { World} {Hello World}}

    test {SUBSTR against integer encoded values} {
        r set foo 123
        r substr foo 0 -2
    } {12}

    test {SUBSTR fuzzing} {
        set err {}
        for {set i 0} {$i < 1000} {incr i} {
            set bin [randstring 0 1024 binary]
            set _start [set start [randomInt 1500]]
            set _end [set end [randomInt 1500]]
            if {$_start < 0} {set _start "end-[abs($_start)-1]"}
            if {$_end < 0} {set _end "end-[abs($_end)-1]"}
            set s1 [string range $bin $_start $_end]
            r set bin $bin
            set s2 [r substr bin $start $end]
            if {$s1 != $s2} {
                set err "String mismatch"
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
        r save
    } {OK}
}
