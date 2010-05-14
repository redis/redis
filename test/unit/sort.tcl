start_server default.conf {} {
    test {SORT ALPHA against integer encoded strings} {
        r del mylist
        r lpush mylist 2
        r lpush mylist 1
        r lpush mylist 3
        r lpush mylist 10
        r sort mylist alpha
    } {1 10 2 3}

    test {Create a random list and a random set} {
        set tosort {}
        array set seenrand {}
        for {set i 0} {$i < 10000} {incr i} {
            while 1 {
                # Make sure all the weights are different because
                # Redis does not use a stable sort but Tcl does.
                randpath {
                    set rint [expr int(rand()*1000000)]
                } {
                    set rint [expr rand()]
                }
                if {![info exists seenrand($rint)]} break
            }
            set seenrand($rint) x
            r lpush tosort $i
            r sadd tosort-set $i
            r set weight_$i $rint
            r hset wobj_$i weight $rint
            lappend tosort [list $i $rint]
        }
        set sorted [lsort -index 1 -real $tosort]
        set res {}
        for {set i 0} {$i < 10000} {incr i} {
            lappend res [lindex $sorted $i 0]
        }
        format {}
    } {}

    test {SORT with BY against the newly created list} {
        r sort tosort {BY weight_*}
    } $res

    test {SORT with BY (hash field) against the newly created list} {
        r sort tosort {BY wobj_*->weight}
    } $res

    test {SORT with GET (key+hash) with sanity check of each element (list)} {
        set err {}
        set l1 [r sort tosort GET # GET weight_*]
        set l2 [r sort tosort GET # GET wobj_*->weight]
        foreach {id1 w1} $l1 {id2 w2} $l2 {
            set realweight [r get weight_$id1]
            if {$id1 != $id2} {
                set err "ID mismatch $id1 != $id2"
                break
            }
            if {$realweight != $w1 || $realweight != $w2} {
                set err "Weights mismatch! w1: $w1 w2: $w2 real: $realweight"
                break
            }
        }
        set _ $err
    } {}

    test {SORT with BY, but against the newly created set} {
        r sort tosort-set {BY weight_*}
    } $res

    test {SORT with BY (hash field), but against the newly created set} {
        r sort tosort-set {BY wobj_*->weight}
    } $res

    test {SORT with BY and STORE against the newly created list} {
        r sort tosort {BY weight_*} store sort-res
        r lrange sort-res 0 -1
    } $res

    test {SORT with BY (hash field) and STORE against the newly created list} {
        r sort tosort {BY wobj_*->weight} store sort-res
        r lrange sort-res 0 -1
    } $res

    test {SORT direct, numeric, against the newly created list} {
        r sort tosort
    } [lsort -integer $res]

    test {SORT decreasing sort} {
        r sort tosort {DESC}
    } [lsort -decreasing -integer $res]

    test {SORT speed, sorting 10000 elements list using BY, 100 times} {
        set start [clock clicks -milliseconds]
        for {set i 0} {$i < 100} {incr i} {
            set sorted [r sort tosort {BY weight_* LIMIT 0 10}]
        }
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts -nonewline "\n  Average time to sort: [expr double($elapsed)/100] milliseconds "
        flush stdout
        format {}
    } {}

    test {SORT speed, as above but against hash field} {
        set start [clock clicks -milliseconds]
        for {set i 0} {$i < 100} {incr i} {
            set sorted [r sort tosort {BY wobj_*->weight LIMIT 0 10}]
        }
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts -nonewline "\n  Average time to sort: [expr double($elapsed)/100] milliseconds "
        flush stdout
        format {}
    } {}

    test {SORT speed, sorting 10000 elements list directly, 100 times} {
        set start [clock clicks -milliseconds]
        for {set i 0} {$i < 100} {incr i} {
            set sorted [r sort tosort {LIMIT 0 10}]
        }
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts -nonewline "\n  Average time to sort: [expr double($elapsed)/100] milliseconds "
        flush stdout
        format {}
    } {}

    test {SORT speed, pseudo-sorting 10000 elements list, BY <const>, 100 times} {
        set start [clock clicks -milliseconds]
        for {set i 0} {$i < 100} {incr i} {
            set sorted [r sort tosort {BY nokey LIMIT 0 10}]
        }
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts -nonewline "\n  Average time to sort: [expr double($elapsed)/100] milliseconds "
        flush stdout
        format {}
    } {}

    test {SORT regression for issue #19, sorting floats} {
        r flushdb
        foreach x {1.1 5.10 3.10 7.44 2.1 5.75 6.12 0.25 1.15} {
            r lpush mylist $x
        }
        r sort mylist
    } [lsort -real {1.1 5.10 3.10 7.44 2.1 5.75 6.12 0.25 1.15}]

    test {SORT with GET #} {
        r del mylist
        r lpush mylist 1
        r lpush mylist 2
        r lpush mylist 3
        r mset weight_1 10 weight_2 5 weight_3 30
        r sort mylist BY weight_* GET #
    } {2 1 3}

    test {SORT with constant GET} {
        r sort mylist GET foo
    } {{} {} {}}
    
    test {SORT against sorted sets} {
        r del zset
        r zadd zset 1 a
        r zadd zset 5 b
        r zadd zset 2 c
        r zadd zset 10 d
        r zadd zset 3 e
        r sort zset alpha desc
    } {e d c b a}

    test {Sorted sets +inf and -inf handling} {
        r del zset
        r zadd zset -100 a
        r zadd zset 200 b
        r zadd zset -300 c
        r zadd zset 1000000 d
        r zadd zset +inf max
        r zadd zset -inf min
        r zrange zset 0 -1
    } {min c a b d max}
}
