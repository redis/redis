# test-redis.tcl
# Redis test suite. Copyright (C) 2009 Salvatore Sanfilippo antirez@gmail.com
# This softare is released under the BSD License. See the COPYING file for
# more information.

set tcl_precision 17
source redis.tcl

set ::passed 0
set ::failed 0
set ::testnum 0

proc test {name code okpattern} {
    incr ::testnum
    if {$::testnum < $::first || $::testnum > $::last} return
    puts -nonewline [format "%-70s " "#$::testnum $name"]
    flush stdout
    set retval [uplevel 1 $code]
    if {$okpattern eq $retval || [string match $okpattern $retval]} {
        puts "PASSED"
        incr ::passed
    } else {
        puts "!! ERROR expected\n'$okpattern'\nbut got\n'$retval'"
        incr ::failed
    }
}

proc randstring {min max {type binary}} {
    set len [expr {$min+int(rand()*($max-$min+1))}]
    set output {}
    if {$type eq {binary}} {
        set minval 0
        set maxval 255
    } elseif {$type eq {alpha}} {
        set minval 48
        set maxval 122
    } elseif {$type eq {compr}} {
        set minval 48
        set maxval 52
    }
    while {$len} {
        append output [format "%c" [expr {$minval+int(rand()*($maxval-$minval+1))}]]
        incr len -1
    }
    return $output
}

# Useful for some test
proc zlistAlikeSort {a b} {
    if {[lindex $a 0] > [lindex $b 0]} {return 1}
    if {[lindex $a 0] < [lindex $b 0]} {return -1}
    string compare [lindex $a 1] [lindex $b 1]
}

proc waitForBgsave r {
    while 1 {
        set i [$r info]
        if {[string match {*bgsave_in_progress:1*} $i]} {
            puts -nonewline "\nWaiting for background save to finish... "
            flush stdout
            after 1000
        } else {
            break
        }
    }
}

proc waitForBgrewriteaof r {
    while 1 {
        set i [$r info]
        if {[string match {*bgrewriteaof_in_progress:1*} $i]} {
            puts -nonewline "\nWaiting for background AOF rewrite to finish... "
            flush stdout
            after 1000
        } else {
            break
        }
    }
}

proc randomInt {max} {
    expr {int(rand()*$max)}
}

proc randpath args {
    set path [expr {int(rand()*[llength $args])}]
    uplevel 1 [lindex $args $path]
}

proc randomValue {} {
    randpath {
        # Small enough to likely collide
        randomInt 1000
    } {
        # 32 bit compressible signed/unsigned
        randpath {randomInt 2000000000} {randomInt 4000000000}
    } {
        # 64 bit
        randpath {randomInt 1000000000000}
    } {
        # Random string
        randpath {randstring 0 256 alpha} \
                {randstring 0 256 compr} \
                {randstring 0 256 binary}
    }
}

proc randomKey {} {
    randpath {
        # Small enough to likely collide
        randomInt 1000
    } {
        # 32 bit compressible signed/unsigned
        randpath {randomInt 2000000000} {randomInt 4000000000}
    } {
        # 64 bit
        randpath {randomInt 1000000000000}
    } {
        # Random string
        randpath {randstring 1 256 alpha} \
                {randstring 1 256 compr}
    }
}

proc createComplexDataset {r ops} {
    for {set j 0} {$j < $ops} {incr j} {
        set k [randomKey]
        set v [randomValue]
        randpath {
            set d [expr {rand()}]
        } {
            set d [expr {rand()}]
        } {
            set d [expr {rand()}]
        } {
            set d [expr {rand()}]
        } {
            set d [expr {rand()}]
        } {
            randpath {set d +inf} {set d -inf}
        }
        set t [$r type $k]

        if {$t eq {none}} {
            randpath {
                $r set $k $v
            } {
                $r lpush $k $v
            } {
                $r sadd $k $v
            } {
                $r zadd $k $d $v
            }
            set t [$r type $k]
        }

        switch $t {
            {string} {
                # Nothing to do
            }
            {list} {
                randpath {$r lpush $k $v} \
                        {$r rpush $k $v} \
                        {$r lrem $k 0 $v} \
                        {$r rpop $k} \
                        {$r lpop $k}
            }
            {set} {
                randpath {$r sadd $k $v} \
                        {$r srem $k $v}
            }
            {zset} {
                randpath {$r zadd $k $d $v} \
                        {$r zrem $k $v}
            }
        }
    }
}

proc datasetDigest r {
    set keys [lsort [$r keys *]]
    set digest {}
    foreach k $keys {
        set t [$r type $k]
        switch $t {
            {string} {
                set aux [::sha1::sha1 -hex [$r get $k]]
            } {list} {
                if {[$r llen $k] == 0} {
                    set aux {}
                } else {
                    set aux [::sha1::sha1 -hex [$r lrange $k 0 -1]]
                }
            } {set} {
                if {[$r scard $k] == 0} {
                    set aux {}
                } else {
                    set aux [::sha1::sha1 -hex [lsort [$r smembers $k]]]
                }
            } {zset} {
                if {[$r zcard $k] == 0} {
                    set aux {}
                } else {
                    set aux [::sha1::sha1 -hex [$r zrange $k 0 -1]]
                }
            } default {
                error "Type not supported: $t"
            }
        }
        if {$aux eq {}} continue
        set digest [::sha1::sha1 -hex [join [list $aux $digest $k] "\n"]]
    }
    return $digest
}

proc main {server port} {
    set r [redis $server $port]
    $r select 9
    set err ""
    set res ""

    # The following AUTH test should be enabled only when requirepass
    # <PASSWORD> is set in redis.conf and redis-server was started with
    # redis.conf as the first argument.  

    #test {AUTH with requirepass in redis.conf} {
    #    $r auth foobared
    #} {OK}

    test {DEL all keys to start with a clean DB} {
        foreach key [$r keys *] {$r del $key}
        $r dbsize
    } {0}

    test {SET and GET an item} {
        $r set x foobar
        $r get x
    } {foobar}

    test {SET and GET an empty item} {
        $r set x {}
        $r get x
    } {}

    test {DEL against a single item} {
        $r del x
        $r get x
    } {}

    test {Vararg DEL} {
        $r set foo1 a
        $r set foo2 b
        $r set foo3 c
        list [$r del foo1 foo2 foo3 foo4] [$r mget foo1 foo2 foo3]
    } {3 {{} {} {}}}

    test {KEYS with pattern} {
        foreach key {key_x key_y key_z foo_a foo_b foo_c} {
            $r set $key hello
        }
        lsort [$r keys foo*]
    } {foo_a foo_b foo_c}

    test {KEYS to get all keys} {
        lsort [$r keys *]
    } {foo_a foo_b foo_c key_x key_y key_z}

    test {DBSIZE} {
        $r dbsize
    } {6}

    test {DEL all keys} {
        foreach key [$r keys *] {$r del $key}
        $r dbsize
    } {0}

    test {Very big payload in GET/SET} {
        set buf [string repeat "abcd" 1000000]
        $r set foo $buf
        $r get foo
    } [string repeat "abcd" 1000000]

    test {Very big payload random access} {
        set err {}
        array set payload {}
        for {set j 0} {$j < 100} {incr j} {
            set size [expr 1+[randomInt 100000]]
            set buf [string repeat "pl-$j" $size]
            set payload($j) $buf
            $r set bigpayload_$j $buf
        }
        for {set j 0} {$j < 1000} {incr j} {
            set index [randomInt 100]
            set buf [$r get bigpayload_$index]
            if {$buf != $payload($index)} {
                set err "Values differ: I set '$payload($index)' but I read back '$buf'"
                break
            }
        }
        unset payload
        set _ $err
    } {}

    test {SET 10000 numeric keys and access all them in reverse order} {
        set err {}
        for {set x 0} {$x < 10000} {incr x} {
            $r set $x $x
        }
        set sum 0
        for {set x 9999} {$x >= 0} {incr x -1} {
            set val [$r get $x]
            if {$val ne $x} {
                set err "Eleemnt at position $x is $val instead of $x"
                break
            }
        }
        set _ $err
    } {}

    test {DBSIZE should be 10101 now} {
        $r dbsize
    } {10101}

    test {INCR against non existing key} {
        set res {}
        append res [$r incr novar]
        append res [$r get novar]
    } {11}

    test {INCR against key created by incr itself} {
        $r incr novar
    } {2}

    test {INCR against key originally set with SET} {
        $r set novar 100
        $r incr novar
    } {101}

    test {INCR over 32bit value} {
        $r set novar 17179869184
        $r incr novar
    } {17179869185}

    test {INCRBY over 32bit value with over 32bit increment} {
        $r set novar 17179869184
        $r incrby novar 17179869184
    } {34359738368}

    test {INCR against key with spaces (no integer encoded)} {
        $r set novar "    11    "
        $r incr novar
    } {12}

    test {DECRBY over 32bit value with over 32bit increment, negative res} {
        $r set novar 17179869184
        $r decrby novar 17179869185
    } {-1}

    test {SETNX target key missing} {
        $r setnx novar2 foobared
        $r get novar2
    } {foobared}

    test {SETNX target key exists} {
        $r setnx novar2 blabla
        $r get novar2
    } {foobared}

    test {SETNX will overwrite EXPIREing key} {
        $r set x 10
        $r expire x 10000
        $r setnx x 20
        $r get x
    } {20}

    test {EXISTS} {
        set res {}
        $r set newkey test
        append res [$r exists newkey]
        $r del newkey
        append res [$r exists newkey]
    } {10}

    test {Zero length value in key. SET/GET/EXISTS} {
        $r set emptykey {}
        set res [$r get emptykey]
        append res [$r exists emptykey]
        $r del emptykey
        append res [$r exists emptykey]
    } {10}

    test {Commands pipelining} {
        set fd [$r channel]
        puts -nonewline $fd "SET k1 4\r\nxyzk\r\nGET k1\r\nPING\r\n"
        flush $fd
        set res {}
        append res [string match OK* [::redis::redis_read_reply $fd]]
        append res [::redis::redis_read_reply $fd]
        append res [string match PONG* [::redis::redis_read_reply $fd]]
        format $res
    } {1xyzk1}

    test {Non existing command} {
        catch {$r foobaredcommand} err
        string match ERR* $err
    } {1}

    test {Basic LPUSH, RPUSH, LLENGTH, LINDEX} {
        set res [$r lpush mylist a]
        append res [$r lpush mylist b]
        append res [$r rpush mylist c]
        append res [$r llen mylist]
        append res [$r rpush anotherlist d]
        append res [$r lpush anotherlist e]
        append res [$r llen anotherlist]
        append res [$r lindex mylist 0]
        append res [$r lindex mylist 1]
        append res [$r lindex mylist 2]
        append res [$r lindex anotherlist 0]
        append res [$r lindex anotherlist 1]
        list $res [$r lindex mylist 100]
    } {1233122baced {}}

    test {DEL a list} {
        $r del mylist
        $r exists mylist
    } {0}

    test {Create a long list and check every single element with LINDEX} {
        set ok 0
        for {set i 0} {$i < 1000} {incr i} {
            $r rpush mylist $i
        }
        for {set i 0} {$i < 1000} {incr i} {
            if {[$r lindex mylist $i] eq $i} {incr ok}
            if {[$r lindex mylist [expr (-$i)-1]] eq [expr 999-$i]} {
                incr ok
            }
        }
        format $ok
    } {2000}

    test {Test elements with LINDEX in random access} {
        set ok 0
        for {set i 0} {$i < 1000} {incr i} {
            set rint [expr int(rand()*1000)]
            if {[$r lindex mylist $rint] eq $rint} {incr ok}
            if {[$r lindex mylist [expr (-$rint)-1]] eq [expr 999-$rint]} {
                incr ok
            }
        }
        format $ok
    } {2000}

    test {Check if the list is still ok after a DEBUG RELOAD} {
        $r debug reload
        set ok 0
        for {set i 0} {$i < 1000} {incr i} {
            set rint [expr int(rand()*1000)]
            if {[$r lindex mylist $rint] eq $rint} {incr ok}
            if {[$r lindex mylist [expr (-$rint)-1]] eq [expr 999-$rint]} {
                incr ok
            }
        }
        format $ok
    } {2000}

    test {LLEN against non-list value error} {
        $r del mylist
        $r set mylist foobar
        catch {$r llen mylist} err
        format $err
    } {ERR*}

    test {LLEN against non existing key} {
        $r llen not-a-key
    } {0}

    test {LINDEX against non-list value error} {
        catch {$r lindex mylist 0} err
        format $err
    } {ERR*}

    test {LINDEX against non existing key} {
        $r lindex not-a-key 10
    } {}

    test {LPUSH against non-list value error} {
        catch {$r lpush mylist 0} err
        format $err
    } {ERR*}

    test {RPUSH against non-list value error} {
        catch {$r rpush mylist 0} err
        format $err
    } {ERR*}

    test {RPOPLPUSH base case} {
        $r del mylist
        $r rpush mylist a
        $r rpush mylist b
        $r rpush mylist c
        $r rpush mylist d
        set v1 [$r rpoplpush mylist newlist]
        set v2 [$r rpoplpush mylist newlist]
        set l1 [$r lrange mylist 0 -1]
        set l2 [$r lrange newlist 0 -1]
        list $v1 $v2 $l1 $l2
    } {d c {a b} {c d}}

    test {RPOPLPUSH with the same list as src and dst} {
        $r del mylist
        $r rpush mylist a
        $r rpush mylist b
        $r rpush mylist c
        set l1 [$r lrange mylist 0 -1]
        set v [$r rpoplpush mylist mylist]
        set l2 [$r lrange mylist 0 -1]
        list $l1 $v $l2
    } {{a b c} c {c a b}}

    test {RPOPLPUSH target list already exists} {
        $r del mylist
        $r del newlist
        $r rpush mylist a
        $r rpush mylist b
        $r rpush mylist c
        $r rpush mylist d
        $r rpush newlist x
        set v1 [$r rpoplpush mylist newlist]
        set v2 [$r rpoplpush mylist newlist]
        set l1 [$r lrange mylist 0 -1]
        set l2 [$r lrange newlist 0 -1]
        list $v1 $v2 $l1 $l2
    } {d c {a b} {c d x}}

    test {RPOPLPUSH against non existing key} {
        $r del mylist
        $r del newlist
        set v1 [$r rpoplpush mylist newlist]
        list $v1 [$r exists mylist] [$r exists newlist]
    } {{} 0 0}

    test {RPOPLPUSH against non list src key} {
        $r del mylist
        $r del newlist
        $r set mylist x
        catch {$r rpoplpush mylist newlist} err
        list [$r type mylist] [$r exists newlist] [string range $err 0 2]
    } {string 0 ERR}

    test {RPOPLPUSH against non list dst key} {
        $r del mylist
        $r del newlist
        $r rpush mylist a
        $r rpush mylist b
        $r rpush mylist c
        $r rpush mylist d
        $r set newlist x
        catch {$r rpoplpush mylist newlist} err
        list [$r lrange mylist 0 -1] [$r type newlist] [string range $err 0 2]
    } {{a b c d} string ERR}

    test {RPOPLPUSH against non existing src key} {
        $r del mylist
        $r del newlist
        $r rpoplpush mylist newlist
    } {}

    test {RENAME basic usage} {
        $r set mykey hello
        $r rename mykey mykey1
        $r rename mykey1 mykey2
        $r get mykey2
    } {hello}

    test {RENAME source key should no longer exist} {
        $r exists mykey
    } {0}

    test {RENAME against already existing key} {
        $r set mykey a
        $r set mykey2 b
        $r rename mykey2 mykey
        set res [$r get mykey]
        append res [$r exists mykey2]
    } {b0}

    test {RENAMENX basic usage} {
        $r del mykey
        $r del mykey2
        $r set mykey foobar
        $r renamenx mykey mykey2
        set res [$r get mykey2]
        append res [$r exists mykey]
    } {foobar0}

    test {RENAMENX against already existing key} {
        $r set mykey foo
        $r set mykey2 bar
        $r renamenx mykey mykey2
    } {0}

    test {RENAMENX against already existing key (2)} {
        set res [$r get mykey]
        append res [$r get mykey2]
    } {foobar}

    test {RENAME against non existing source key} {
        catch {$r rename nokey foobar} err
        format $err
    } {ERR*}

    test {RENAME where source and dest key is the same} {
        catch {$r rename mykey mykey} err
        format $err
    } {ERR*}

    test {DEL all keys again (DB 0)} {
        foreach key [$r keys *] {
            $r del $key
        }
        $r dbsize
    } {0}

    test {DEL all keys again (DB 1)} {
        $r select 10
        foreach key [$r keys *] {
            $r del $key
        }
        set res [$r dbsize]
        $r select 9
        format $res
    } {0}

    test {MOVE basic usage} {
        $r set mykey foobar
        $r move mykey 10
        set res {}
        lappend res [$r exists mykey]
        lappend res [$r dbsize]
        $r select 10
        lappend res [$r get mykey]
        lappend res [$r dbsize]
        $r select 9
        format $res
    } [list 0 0 foobar 1]

    test {MOVE against key existing in the target DB} {
        $r set mykey hello
        $r move mykey 10
    } {0}

    test {SET/GET keys in different DBs} {
        $r set a hello
        $r set b world
        $r select 10
        $r set a foo
        $r set b bared
        $r select 9
        set res {}
        lappend res [$r get a]
        lappend res [$r get b]
        $r select 10
        lappend res [$r get a]
        lappend res [$r get b]
        $r select 9
        format $res
    } {hello world foo bared}

    test {Basic LPOP/RPOP} {
        $r del mylist
        $r rpush mylist 1
        $r rpush mylist 2
        $r lpush mylist 0
        list [$r lpop mylist] [$r rpop mylist] [$r lpop mylist] [$r llen mylist]
    } [list 0 2 1 0]

    test {LPOP/RPOP against empty list} {
        $r lpop mylist
    } {}

    test {LPOP against non list value} {
        $r set notalist foo
        catch {$r lpop notalist} err
        format $err
    } {ERR*kind*}

    test {Mass LPUSH/LPOP} {
        set sum 0
        for {set i 0} {$i < 1000} {incr i} {
            $r lpush mylist $i
            incr sum $i
        }
        set sum2 0
        for {set i 0} {$i < 500} {incr i} {
            incr sum2 [$r lpop mylist]
            incr sum2 [$r rpop mylist]
        }
        expr $sum == $sum2
    } {1}

    test {LRANGE basics} {
        for {set i 0} {$i < 10} {incr i} {
            $r rpush mylist $i
        }
        list [$r lrange mylist 1 -2] \
                [$r lrange mylist -3 -1] \
                [$r lrange mylist 4 4]
    } {{1 2 3 4 5 6 7 8} {7 8 9} 4}

    test {LRANGE inverted indexes} {
        $r lrange mylist 6 2
    } {}

    test {LRANGE out of range indexes including the full list} {
        $r lrange mylist -1000 1000
    } {0 1 2 3 4 5 6 7 8 9}

    test {LRANGE against non existing key} {
        $r lrange nosuchkey 0 1
    } {}

    test {LTRIM basics} {
        $r del mylist
        for {set i 0} {$i < 100} {incr i} {
            $r lpush mylist $i
            $r ltrim mylist 0 4
        }
        $r lrange mylist 0 -1
    } {99 98 97 96 95}

    test {LTRIM stress testing} {
        set mylist {}
        set err {}
        for {set i 0} {$i < 20} {incr i} {
            lappend mylist $i
        }

        for {set j 0} {$j < 100} {incr j} {
            # Fill the list
            $r del mylist
            for {set i 0} {$i < 20} {incr i} {
                $r rpush mylist $i
            }
            # Trim at random
            set a [randomInt 20]
            set b [randomInt 20]
            $r ltrim mylist $a $b
            if {[$r lrange mylist 0 -1] ne [lrange $mylist $a $b]} {
                set err "[$r lrange mylist 0 -1] != [lrange $mylist $a $b]"
                break
            }
        }
        set _ $err
    } {}

    test {LSET} {
        $r del mylist
        foreach x {99 98 97 96 95} {
            $r rpush mylist $x
        }
        $r lset mylist 1 foo
        $r lset mylist -1 bar
        $r lrange mylist 0 -1
    } {99 foo 97 96 bar}

    test {LSET out of range index} {
        catch {$r lset mylist 10 foo} err
        format $err
    } {ERR*range*}

    test {LSET against non existing key} {
        catch {$r lset nosuchkey 10 foo} err
        format $err
    } {ERR*key*}

    test {LSET against non list value} {
        $r set nolist foobar
        catch {$r lset nolist 0 foo} err
        format $err
    } {ERR*value*}

    test {SADD, SCARD, SISMEMBER, SMEMBERS basics} {
        $r sadd myset foo
        $r sadd myset bar
        list [$r scard myset] [$r sismember myset foo] \
            [$r sismember myset bar] [$r sismember myset bla] \
            [lsort [$r smembers myset]]
    } {2 1 1 0 {bar foo}}

    test {SADD adding the same element multiple times} {
        $r sadd myset foo
        $r sadd myset foo
        $r sadd myset foo
        $r scard myset
    } {2}

    test {SADD against non set} {
        catch {$r sadd mylist foo} err
        format $err
    } {ERR*kind*}

    test {SREM basics} {
        $r sadd myset ciao
        $r srem myset foo
        lsort [$r smembers myset]
    } {bar ciao}

    test {Mass SADD and SINTER with two sets} {
        for {set i 0} {$i < 1000} {incr i} {
            $r sadd set1 $i
            $r sadd set2 [expr $i+995]
        }
        lsort [$r sinter set1 set2]
    } {995 996 997 998 999}

    test {SUNION with two sets} {
        lsort [$r sunion set1 set2]
    } [lsort -uniq "[$r smembers set1] [$r smembers set2]"]
    
    test {SINTERSTORE with two sets} {
        $r sinterstore setres set1 set2
        lsort [$r smembers setres]
    } {995 996 997 998 999}

    test {SINTERSTORE with two sets, after a DEBUG RELOAD} {
        $r debug reload
        $r sinterstore setres set1 set2
        lsort [$r smembers setres]
    } {995 996 997 998 999}

    test {SUNIONSTORE with two sets} {
        $r sunionstore setres set1 set2
        lsort [$r smembers setres]
    } [lsort -uniq "[$r smembers set1] [$r smembers set2]"]

    test {SUNIONSTORE against non existing keys} {
        $r set setres xxx
        list [$r sunionstore setres foo111 bar222] [$r exists xxx]
    } {0 0}

    test {SINTER against three sets} {
        $r sadd set3 999
        $r sadd set3 995
        $r sadd set3 1000
        $r sadd set3 2000
        lsort [$r sinter set1 set2 set3]
    } {995 999}

    test {SINTERSTORE with three sets} {
        $r sinterstore setres set1 set2 set3
        lsort [$r smembers setres]
    } {995 999}

    test {SUNION with non existing keys} {
        lsort [$r sunion nokey1 set1 set2 nokey2]
    } [lsort -uniq "[$r smembers set1] [$r smembers set2]"]

    test {SDIFF with two sets} {
        for {set i 5} {$i < 1000} {incr i} {
            $r sadd set4 $i
        }
        lsort [$r sdiff set1 set4]
    } {0 1 2 3 4}

    test {SDIFF with three sets} {
        $r sadd set5 0
        lsort [$r sdiff set1 set4 set5]
    } {1 2 3 4}

    test {SDIFFSTORE with three sets} {
        $r sdiffstore sres set1 set4 set5
        lsort [$r smembers sres]
    } {1 2 3 4}

    test {SPOP basics} {
        $r del myset
        $r sadd myset 1
        $r sadd myset 2
        $r sadd myset 3
        list [lsort [list [$r spop myset] [$r spop myset] [$r spop myset]]] [$r scard myset]
    } {{1 2 3} 0}

    test {SAVE - make sure there are all the types as values} {
        # Wait for a background saving in progress to terminate
        waitForBgsave $r
        $r lpush mysavelist hello
        $r lpush mysavelist world
        $r set myemptykey {}
        $r set mynormalkey {blablablba}
        $r zadd mytestzset a 10
        $r zadd mytestzset b 20
        $r zadd mytestzset c 30
        $r save
    } {OK}

    test {SRANDMEMBER} {
        $r del myset
        $r sadd myset a
        $r sadd myset b
        $r sadd myset c
        unset -nocomplain myset
        array set myset {}
        for {set i 0} {$i < 100} {incr i} {
            set myset([$r srandmember myset]) 1
        }
        lsort [array names myset]
    } {a b c}
    
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
            $r lpush tosort $i
            $r sadd tosort-set $i
            $r set weight_$i $rint
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
        $r sort tosort {BY weight_*}
    } $res

    test {the same SORT with BY, but against the newly created set} {
        $r sort tosort-set {BY weight_*}
    } $res

    test {SORT with BY and STORE against the newly created list} {
        $r sort tosort {BY weight_*} store sort-res
        $r lrange sort-res 0 -1
    } $res

    test {SORT direct, numeric, against the newly created list} {
        $r sort tosort
    } [lsort -integer $res]

    test {SORT decreasing sort} {
        $r sort tosort {DESC}
    } [lsort -decreasing -integer $res]

    test {SORT speed, sorting 10000 elements list using BY, 100 times} {
        set start [clock clicks -milliseconds]
        for {set i 0} {$i < 100} {incr i} {
            set sorted [$r sort tosort {BY weight_* LIMIT 0 10}]
        }
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts -nonewline "\n  Average time to sort: [expr double($elapsed)/100] milliseconds "
        flush stdout
        format {}
    } {}

    test {SORT speed, sorting 10000 elements list directly, 100 times} {
        set start [clock clicks -milliseconds]
        for {set i 0} {$i < 100} {incr i} {
            set sorted [$r sort tosort {LIMIT 0 10}]
        }
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts -nonewline "\n  Average time to sort: [expr double($elapsed)/100] milliseconds "
        flush stdout
        format {}
    } {}

    test {SORT speed, pseudo-sorting 10000 elements list, BY <const>, 100 times} {
        set start [clock clicks -milliseconds]
        for {set i 0} {$i < 100} {incr i} {
            set sorted [$r sort tosort {BY nokey LIMIT 0 10}]
        }
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts -nonewline "\n  Average time to sort: [expr double($elapsed)/100] milliseconds "
        flush stdout
        format {}
    } {}

    test {SORT regression for issue #19, sorting floats} {
        $r flushdb
        foreach x {1.1 5.10 3.10 7.44 2.1 5.75 6.12 0.25 1.15} {
            $r lpush mylist $x
        }
        $r sort mylist
    } [lsort -real {1.1 5.10 3.10 7.44 2.1 5.75 6.12 0.25 1.15}]

    test {SORT with GET #} {
        $r del mylist
        $r lpush mylist 1
        $r lpush mylist 2
        $r lpush mylist 3
        $r mset weight_1 10 weight_2 5 weight_3 30
        $r sort mylist BY weight_* GET #
    } {2 1 3}

    test {SORT with constant GET} {
        $r sort mylist GET foo
    } {{} {} {}}

    test {LREM, remove all the occurrences} {
        $r flushdb
        $r rpush mylist foo
        $r rpush mylist bar
        $r rpush mylist foobar
        $r rpush mylist foobared
        $r rpush mylist zap
        $r rpush mylist bar
        $r rpush mylist test
        $r rpush mylist foo
        set res [$r lrem mylist 0 bar]
        list [$r lrange mylist 0 -1] $res
    } {{foo foobar foobared zap test foo} 2}

    test {LREM, remove the first occurrence} {
        set res [$r lrem mylist 1 foo]
        list [$r lrange mylist 0 -1] $res
    } {{foobar foobared zap test foo} 1}

    test {LREM, remove non existing element} {
        set res [$r lrem mylist 1 nosuchelement]
        list [$r lrange mylist 0 -1] $res
    } {{foobar foobared zap test foo} 0}

    test {LREM, starting from tail with negative count} {
        $r flushdb
        $r rpush mylist foo
        $r rpush mylist bar
        $r rpush mylist foobar
        $r rpush mylist foobared
        $r rpush mylist zap
        $r rpush mylist bar
        $r rpush mylist test
        $r rpush mylist foo
        $r rpush mylist foo
        set res [$r lrem mylist -1 bar]
        list [$r lrange mylist 0 -1] $res
    } {{foo bar foobar foobared zap test foo foo} 1}

    test {LREM, starting from tail with negative count (2)} {
        set res [$r lrem mylist -2 foo]
        list [$r lrange mylist 0 -1] $res
    } {{foo bar foobar foobared zap test} 2}

    test {LREM, deleting objects that may be encoded as integers} {
        $r lpush myotherlist 1
        $r lpush myotherlist 2
        $r lpush myotherlist 3
        $r lrem myotherlist 1 2
        $r llen myotherlist
    } {2}

    test {MGET} {
        $r flushdb
        $r set foo BAR
        $r set bar FOO
        $r mget foo bar
    } {BAR FOO}

    test {MGET against non existing key} {
        $r mget foo baazz bar
    } {BAR {} FOO}

    test {MGET against non-string key} {
        $r sadd myset ciao
        $r sadd myset bau
        $r mget foo baazz bar myset
    } {BAR {} FOO {}}

    test {RANDOMKEY} {
        $r flushdb
        $r set foo x
        $r set bar y
        set foo_seen 0
        set bar_seen 0
        for {set i 0} {$i < 100} {incr i} {
            set rkey [$r randomkey]
            if {$rkey eq {foo}} {
                set foo_seen 1
            }
            if {$rkey eq {bar}} {
                set bar_seen 1
            }
        }
        list $foo_seen $bar_seen
    } {1 1}

    test {RANDOMKEY against empty DB} {
        $r flushdb
        $r randomkey
    } {}

    test {RANDOMKEY regression 1} {
        $r flushdb
        $r set x 10
        $r del x
        $r randomkey
    } {}

    test {GETSET (set new value)} {
        list [$r getset foo xyz] [$r get foo]
    } {{} xyz}

    test {GETSET (replace old value)} {
        $r set foo bar
        list [$r getset foo xyz] [$r get foo]
    } {bar xyz}

    test {SMOVE basics} {
        $r sadd myset1 a
        $r sadd myset1 b
        $r sadd myset1 c
        $r sadd myset2 x
        $r sadd myset2 y
        $r sadd myset2 z
        $r smove myset1 myset2 a
        list [lsort [$r smembers myset2]] [lsort [$r smembers myset1]]
    } {{a x y z} {b c}}

    test {SMOVE non existing key} {
        list [$r smove myset1 myset2 foo] [lsort [$r smembers myset2]] [lsort [$r smembers myset1]]
    } {0 {a x y z} {b c}}

    test {SMOVE non existing src set} {
        list [$r smove noset myset2 foo] [lsort [$r smembers myset2]]
    } {0 {a x y z}}

    test {SMOVE non existing dst set} {
        list [$r smove myset2 myset3 y] [lsort [$r smembers myset2]] [lsort [$r smembers myset3]]
    } {1 {a x z} y}

    test {SMOVE wrong src key type} {
        $r set x 10
        catch {$r smove x myset2 foo} err
        format $err
    } {ERR*}

    test {SMOVE wrong dst key type} {
        $r set x 10
        catch {$r smove myset2 x foo} err
        format $err
    } {ERR*}

    test {MSET base case} {
        $r mset x 10 y "foo bar" z "x x x x x x x\n\n\r\n"
        $r mget x y z
    } [list 10 {foo bar} "x x x x x x x\n\n\r\n"]

    test {MSET wrong number of args} {
        catch {$r mset x 10 y "foo bar" z} err
        format $err
    } {*wrong number*}

    test {MSETNX with already existent key} {
        list [$r msetnx x1 xxx y2 yyy x 20] [$r exists x1] [$r exists y2]
    } {0 0 0}

    test {MSETNX with not existing keys} {
        list [$r msetnx x1 xxx y2 yyy] [$r get x1] [$r get y2]
    } {1 xxx yyy}

    test {MSETNX should remove all the volatile keys even on failure} {
        $r mset x 1 y 2 z 3
        $r expire y 10000
        $r expire z 10000
        list [$r msetnx x A y B z C] [$r mget x y z]
    } {0 {1 {} {}}}

    test {ZSET basic ZADD and score update} {
        $r zadd ztmp 10 x
        $r zadd ztmp 20 y
        $r zadd ztmp 30 z
        set aux1 [$r zrange ztmp 0 -1]
        $r zadd ztmp 1 y
        set aux2 [$r zrange ztmp 0 -1]
        list $aux1 $aux2
    } {{x y z} {y x z}}

    test {ZCARD basics} {
        $r zcard ztmp
    } {3}

    test {ZCARD non existing key} {
        $r zcard ztmp-blabla
    } {0}

    test {ZRANK basics} {
        $r zadd zranktmp 10 x
        $r zadd zranktmp 20 y
        $r zadd zranktmp 30 z
        list [$r zrank zranktmp x] [$r zrank zranktmp y] [$r zrank zranktmp z]
    } {0 1 2}

    test {ZREVRANK basics} {
        list [$r zrevrank zranktmp x] [$r zrevrank zranktmp y] [$r zrevrank zranktmp z]
    } {2 1 0}

    test {ZRANK - after deletion} {
        $r zrem zranktmp y
        list [$r zrank zranktmp x] [$r zrank zranktmp z]
    } {0 1}

    test {ZSCORE} {
        set aux {}
        set err {}
        for {set i 0} {$i < 1000} {incr i} {
            set score [expr rand()]
            lappend aux $score
            $r zadd zscoretest $score $i
        }
        for {set i 0} {$i < 1000} {incr i} {
            if {[$r zscore zscoretest $i] != [lindex $aux $i]} {
                set err "Expected score was [lindex $aux $i] but got [$r zscore zscoretest $i] for element $i"
                break
            }
        }
        set _ $err
    } {}

    test {ZSCORE after a DEBUG RELOAD} {
        set aux {}
        set err {}
        $r del zscoretest
        for {set i 0} {$i < 1000} {incr i} {
            set score [expr rand()]
            lappend aux $score
            $r zadd zscoretest $score $i
        }
        $r debug reload
        for {set i 0} {$i < 1000} {incr i} {
            if {[$r zscore zscoretest $i] != [lindex $aux $i]} {
                set err "Expected score was [lindex $aux $i] but got [$r zscore zscoretest $i] for element $i"
                break
            }
        }
        set _ $err
    } {}

    test {ZRANGE and ZREVRANGE basics} {
        list [$r zrange ztmp 0 -1] [$r zrevrange ztmp 0 -1] \
            [$r zrange ztmp 1 -1] [$r zrevrange ztmp 1 -1]
    } {{y x z} {z x y} {x z} {x y}}

    test {ZRANGE WITHSCORES} {
        $r zrange ztmp 0 -1 withscores
    } {y 1 x 10 z 30}

    test {ZSETs stress tester - sorting is working well?} {
        set delta 0
        for {set test 0} {$test < 2} {incr test} {
            unset -nocomplain auxarray
            array set auxarray {}
            set auxlist {}
            $r del myzset
            for {set i 0} {$i < 1000} {incr i} {
                if {$test == 0} {
                    set score [expr rand()]
                } else {
                    set score [expr int(rand()*10)]
                }
                set auxarray($i) $score
                $r zadd myzset $score $i
                # Random update
                if {[expr rand()] < .2} {
                    set j [expr int(rand()*1000)]
                    if {$test == 0} {
                        set score [expr rand()]
                    } else {
                        set score [expr int(rand()*10)]
                    }
                    set auxarray($j) $score
                    $r zadd myzset $score $j
                }
            }
            foreach {item score} [array get auxarray] {
                lappend auxlist [list $score $item]
            }
            set sorted [lsort -command zlistAlikeSort $auxlist]
            set auxlist {}
            foreach x $sorted {
                lappend auxlist [lindex $x 1]
            }
            set fromredis [$r zrange myzset 0 -1]
            set delta 0
            for {set i 0} {$i < [llength $fromredis]} {incr i} {
                if {[lindex $fromredis $i] != [lindex $auxlist $i]} {
                    incr delta
                }
            }
        }
        format $delta
    } {0}

    test {ZINCRBY - can create a new sorted set} {
        $r del zset
        $r zincrby zset 1 foo
        list [$r zrange zset 0 -1] [$r zscore zset foo]
    } {foo 1}

    test {ZINCRBY - increment and decrement} {
        $r zincrby zset 2 foo
        $r zincrby zset 1 bar
        set v1 [$r zrange zset 0 -1]
        $r zincrby zset 10 bar
        $r zincrby zset -5 foo
        $r zincrby zset -5 bar
        set v2 [$r zrange zset 0 -1]
        list $v1 $v2 [$r zscore zset foo] [$r zscore zset bar]
    } {{bar foo} {foo bar} -2 6}

    test {ZRANGEBYSCORE and ZCOUNT basics} {
        $r del zset
        $r zadd zset 1 a
        $r zadd zset 2 b
        $r zadd zset 3 c
        $r zadd zset 4 d
        $r zadd zset 5 e
        list [$r zrangebyscore zset 2 4] [$r zrangebyscore zset (2 (4] \
             [$r zcount zset 2 4] [$r zcount zset (2 (4]
    } {{b c d} c 3 1}

    test {ZRANGEBYSCORE withscores} {
        $r del zset
        $r zadd zset 1 a
        $r zadd zset 2 b
        $r zadd zset 3 c
        $r zadd zset 4 d
        $r zadd zset 5 e
        $r zrangebyscore zset 2 4 withscores
    } {b 2 c 3 d 4}

    test {ZRANGEBYSCORE fuzzy test, 100 ranges in 1000 elements sorted set} {
        set err {}
        $r del zset
        for {set i 0} {$i < 1000} {incr i} {
            $r zadd zset [expr rand()] $i
        }
        for {set i 0} {$i < 100} {incr i} {
            set min [expr rand()]
            set max [expr rand()]
            if {$min > $max} {
                set aux $min
                set min $max
                set max $aux
            }
            set low [$r zrangebyscore zset -inf $min]
            set ok [$r zrangebyscore zset $min $max]
            set high [$r zrangebyscore zset $max +inf]
            set lowx [$r zrangebyscore zset -inf ($min]
            set okx [$r zrangebyscore zset ($min ($max]
            set highx [$r zrangebyscore zset ($max +inf]

            if {[$r zcount zset -inf $min] != [llength $low]} {
                append err "Error, len does not match zcount\n"
            }
            if {[$r zcount zset $min $max] != [llength $ok]} {
                append err "Error, len does not match zcount\n"
            }
            if {[$r zcount zset $max +inf] != [llength $high]} {
                append err "Error, len does not match zcount\n"
            }
            if {[$r zcount zset -inf ($min] != [llength $lowx]} {
                append err "Error, len does not match zcount\n"
            }
            if {[$r zcount zset ($min ($max] != [llength $okx]} {
                append err "Error, len does not match zcount\n"
            }
            if {[$r zcount zset ($max +inf] != [llength $highx]} {
                append err "Error, len does not match zcount\n"
            }

            foreach x $low {
                set score [$r zscore zset $x]
                if {$score > $min} {
                    append err "Error, score for $x is $score > $min\n"
                }
            }
            foreach x $lowx {
                set score [$r zscore zset $x]
                if {$score >= $min} {
                    append err "Error, score for $x is $score >= $min\n"
                }
            }
            foreach x $ok {
                set score [$r zscore zset $x]
                if {$score < $min || $score > $max} {
                    append err "Error, score for $x is $score outside $min-$max range\n"
                }
            }
            foreach x $okx {
                set score [$r zscore zset $x]
                if {$score <= $min || $score >= $max} {
                    append err "Error, score for $x is $score outside $min-$max open range\n"
                }
            }
            foreach x $high {
                set score [$r zscore zset $x]
                if {$score < $max} {
                    append err "Error, score for $x is $score < $max\n"
                }
            }
            foreach x $highx {
                set score [$r zscore zset $x]
                if {$score <= $max} {
                    append err "Error, score for $x is $score <= $max\n"
                }
            }
        }
        set _ $err
    } {}

    test {ZRANGEBYSCORE with LIMIT} {
        $r del zset
        $r zadd zset 1 a
        $r zadd zset 2 b
        $r zadd zset 3 c
        $r zadd zset 4 d
        $r zadd zset 5 e
        list \
            [$r zrangebyscore zset 0 10 LIMIT 0 2] \
            [$r zrangebyscore zset 0 10 LIMIT 2 3] \
            [$r zrangebyscore zset 0 10 LIMIT 2 10] \
            [$r zrangebyscore zset 0 10 LIMIT 20 10]
    } {{a b} {c d e} {c d e} {}}

    test {ZRANGEBYSCORE with LIMIT and withscores} {
        $r del zset
        $r zadd zset 10 a
        $r zadd zset 20 b
        $r zadd zset 30 c
        $r zadd zset 40 d
        $r zadd zset 50 e
        $r zrangebyscore zset 20 50 LIMIT 2 3 withscores
    } {d 40 e 50}

    test {ZREMRANGEBYSCORE basics} {
        $r del zset
        $r zadd zset 1 a
        $r zadd zset 2 b
        $r zadd zset 3 c
        $r zadd zset 4 d
        $r zadd zset 5 e
        list [$r zremrangebyscore zset 2 4] [$r zrange zset 0 -1]
    } {3 {a e}}

    test {ZREMRANGEBYSCORE from -inf to +inf} {
        $r del zset
        $r zadd zset 1 a
        $r zadd zset 2 b
        $r zadd zset 3 c
        $r zadd zset 4 d
        $r zadd zset 5 e
        list [$r zremrangebyscore zset -inf +inf] [$r zrange zset 0 -1]
    } {5 {}}

    test {ZREMRANGEBYRANK basics} {
        $r del zset
        $r zadd zset 1 a
        $r zadd zset 2 b
        $r zadd zset 3 c
        $r zadd zset 4 d
        $r zadd zset 5 e
        list [$r zremrangebyrank zset 1 3] [$r zrange zset 0 -1]
    } {3 {a e}}

    test {SORT against sorted sets} {
        $r del zset
        $r zadd zset 1 a
        $r zadd zset 5 b
        $r zadd zset 2 c
        $r zadd zset 10 d
        $r zadd zset 3 e
        $r sort zset alpha desc
    } {e d c b a}

    test {Sorted sets +inf and -inf handling} {
        $r del zset
        $r zadd zset -100 a
        $r zadd zset 200 b
        $r zadd zset -300 c
        $r zadd zset 1000000 d
        $r zadd zset +inf max
        $r zadd zset -inf min
        $r zrange zset 0 -1
    } {min c a b d max}

    test {EXPIRE - don't set timeouts multiple times} {
        $r set x foobar
        set v1 [$r expire x 5]
        set v2 [$r ttl x]
        set v3 [$r expire x 10]
        set v4 [$r ttl x]
        list $v1 $v2 $v3 $v4
    } {1 5 0 5}

    test {EXPIRE - It should be still possible to read 'x'} {
        $r get x
    } {foobar}

    test {EXPIRE - After 6 seconds the key should no longer be here} {
        after 6000
        list [$r get x] [$r exists x]
    } {{} 0}

    test {EXPIRE - Delete on write policy} {
        $r del x
        $r lpush x foo
        $r expire x 1000
        $r lpush x bar
        $r lrange x 0 -1
    } {bar}

    test {EXPIREAT - Check for EXPIRE alike behavior} {
        $r del x
        $r set x foo
        $r expireat x [expr [clock seconds]+15]
        $r ttl x
    } {1[345]}

    test {ZSETs skiplist implementation backlink consistency test} {
        set diff 0
        set elements 10000
        for {set j 0} {$j < $elements} {incr j} {
            $r zadd myzset [expr rand()] "Element-$j"
            $r zrem myzset "Element-[expr int(rand()*$elements)]"
        }
        set l1 [$r zrange myzset 0 -1]
        set l2 [$r zrevrange myzset 0 -1]
        for {set j 0} {$j < [llength $l1]} {incr j} {
            if {[lindex $l1 $j] ne [lindex $l2 end-$j]} {
                incr diff
            }
        }
        format $diff
    } {0}

    test {ZSETs ZRANK augmented skip list stress testing} {
        set err {}
        $r del myzset
        for {set k 0} {$k < 10000} {incr k} {
            set i [expr {$k%1000}]
            if {[expr rand()] < .2} {
                $r zrem myzset $i
            } else {
                set score [expr rand()]
                $r zadd myzset $score $i
            }
            set card [$r zcard myzset]
            if {$card > 0} {
                set index [randomInt $card]
                set ele [lindex [$r zrange myzset $index $index] 0]
                set rank [$r zrank myzset $ele]
                if {$rank != $index} {
                    set err "$ele RANK is wrong! ($rank != $index)"
                    break
                }
            }
        }
        set _ $err
    } {}

    foreach fuzztype {binary alpha compr} {
        test "FUZZ stresser with data model $fuzztype" {
            set err 0
            for {set i 0} {$i < 10000} {incr i} {
                set fuzz [randstring 0 512 $fuzztype]
                $r set foo $fuzz
                set got [$r get foo]
                if {$got ne $fuzz} {
                    set err [list $fuzz $got]
                    break
                }
            }
            set _ $err
        } {0}
    }

    test {BGSAVE} {
        waitForBgsave $r
        $r flushdb
        $r save
        $r set x 10
        $r bgsave
        waitForBgsave $r
        $r debug reload
        $r get x
    } {10}

    test {Handle an empty query well} {
        set fd [$r channel]
        puts -nonewline $fd "\r\n"
        flush $fd
        $r ping
    } {PONG}

    test {Negative multi bulk command does not create problems} {
        set fd [$r channel]
        puts -nonewline $fd "*-10\r\n"
        flush $fd
        $r ping
    } {PONG}

    test {Negative multi bulk payload} {
        set fd [$r channel]
        puts -nonewline $fd "SET x -10\r\n"
        flush $fd
        gets $fd
    } {*invalid bulk*}

    test {Too big bulk payload} {
        set fd [$r channel]
        puts -nonewline $fd "SET x 2000000000\r\n"
        flush $fd
        gets $fd
    } {*invalid bulk*count*}

    test {Multi bulk request not followed by bulk args} {
        set fd [$r channel]
        puts -nonewline $fd "*1\r\nfoo\r\n"
        flush $fd
        gets $fd
    } {*protocol error*}

    test {Generic wrong number of args} {
        catch {$r ping x y z} err
        set _ $err
    } {*wrong*arguments*ping*}

    test {SELECT an out of range DB} {
        catch {$r select 1000000} err
        set _ $err
    } {*invalid*}

    if {![catch {package require sha1}]} {
        test {Check consistency of different data types after a reload} {
            $r flushdb
            createComplexDataset $r 10000
            set sha1 [datasetDigest $r]
            $r debug reload
            set sha1_after [datasetDigest $r]
            expr {$sha1 eq $sha1_after}
        } {1}

        test {Same dataset digest if saving/reloading as AOF?} {
            $r bgrewriteaof
            waitForBgrewriteaof $r
            $r debug loadaof
            set sha1_after [datasetDigest $r]
            expr {$sha1 eq $sha1_after}
        } {1}
    }

    test {EXPIRES after a reload (snapshot + append only file)} {
        $r flushdb
        $r set x 10
        $r expire x 1000
        $r save
        $r debug reload
        set ttl [$r ttl x]
        set e1 [expr {$ttl > 900 && $ttl <= 1000}]
        $r bgrewriteaof
        waitForBgrewriteaof $r
        set ttl [$r ttl x]
        set e2 [expr {$ttl > 900 && $ttl <= 1000}]
        list $e1 $e2
    } {1 1}

    test {PIPELINING stresser (also a regression for the old epoll bug)} {
        set fd2 [socket 127.0.0.1 6379]
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
        $r del mylist
        $r rpush mylist a
        $r rpush mylist b
        $r rpush mylist c
        $r multi
        set v1 [$r lrange mylist 0 -1]
        set v2 [$r ping]
        set v3 [$r exec]
        list $v1 $v2 $v3
    } {QUEUED QUEUED {{a b c} PONG}}

    test {DISCARD} {
        $r del mylist
        $r rpush mylist a
        $r rpush mylist b
        $r rpush mylist c
        $r multi
        set v1 [$r del mylist]
        set v2 [$r discard]
        set v3 [$r lrange mylist 0 -1]
        list $v1 $v2 $v3
    } {QUEUED OK {a b c}}

    test {APPEND basics} {
        list [$r append foo bar] [$r get foo] \
             [$r append foo 100] [$r get foo]
    } {3 bar 6 bar100}

    test {APPEND fuzzing} {
        set err {}
        foreach type {binary alpha compr} {
            set buf {}
            $r del x
            for {set i 0} {$i < 1000} {incr i} {
                set bin [randstring 0 10 $type]
                append buf $bin
                $r append x $bin
            }
            if {$buf != [$r get x]} {
                set err "Expected '$buf' found '[$r get x]'"
                break
            }
        }
        set _ $err
    } {}

    # Leave the user with a clean DB before to exit
    test {FLUSHDB} {
        set aux {}
        $r select 9
        $r flushdb
        lappend aux [$r dbsize]
        $r select 10
        $r flushdb
        lappend aux [$r dbsize]
    } {0 0}

    test {Perform a final SAVE to leave a clean DB on disk} {
        $r save
    } {OK}

    catch {
        if {[string match {*Darwin*} [exec uname -a]]} {
            test {Check for memory leaks} {
                exec leaks redis-server
            } {*0 leaks*}
        }
    }

    puts "\n[expr $::passed+$::failed] tests, $::passed passed, $::failed failed"
    if {$::failed > 0} {
        puts "\n*** WARNING!!! $::failed FAILED TESTS ***\n"
    }
}

proc stress {} {
    set r [redis]
    $r select 9
    $r flushdb
    while 1 {
        set randkey [expr int(rand()*10000)]
        set randval [expr int(rand()*10000)]
        set randidx0 [expr int(rand()*10)]
        set randidx1 [expr int(rand()*10)]
        set cmd [expr int(rand()*20)]
        catch {
            if {$cmd == 0} {$r set $randkey $randval}
            if {$cmd == 1} {$r get $randkey}
            if {$cmd == 2} {$r incr $randkey}
            if {$cmd == 3} {$r lpush $randkey $randval}
            if {$cmd == 4} {$r rpop $randkey}
            if {$cmd == 5} {$r del $randkey}
            if {$cmd == 6} {$r llen $randkey}
            if {$cmd == 7} {$r lrange $randkey $randidx0 $randidx1}
            if {$cmd == 8} {$r ltrim $randkey $randidx0 $randidx1}
            if {$cmd == 9} {$r lindex $randkey $randidx0}
            if {$cmd == 10} {$r lset $randkey $randidx0 $randval}
            if {$cmd == 11} {$r sadd $randkey $randval}
            if {$cmd == 12} {$r srem $randkey $randval}
            if {$cmd == 13} {$r smove $randkey $randval}
            if {$cmd == 14} {$r scard $randkey}
            if {$cmd == 15} {$r expire $randkey [expr $randval%60]}
        }
        flush stdout
    }
    $r flushdb
    $r close
}

# Set a few configuration defaults
set ::host 127.0.0.1
set ::port 6379
set ::stress 0
set ::flush 0
set ::first 0
set ::last 1000000

# Parse arguments
for {set j 0} {$j < [llength $argv]} {incr j} {
    set opt [lindex $argv $j]
    set arg [lindex $argv [expr $j+1]]
    set lastarg [expr {$arg eq {}}]
    if {$opt eq {-h} && !$lastarg} {
        set ::host $arg
        incr j
    } elseif {$opt eq {-p} && !$lastarg} {
        set ::port $arg
        incr j
    } elseif {$opt eq {-stress}} {
        set ::stress 1
    } elseif {$opt eq {--flush}} {
        set ::flush 1
    } elseif {$opt eq {--first} && !$lastarg} {
        set ::first $arg
        incr j
    } elseif {$opt eq {--last} && !$lastarg} {
        set ::last $arg
        incr j
    } else {
        puts "Wrong argument: $opt"
        exit 1
    }
}

# Before to run the test check if DB 9 and DB 10 are empty
set r [redis]

if {$::flush} {
    $r flushall
}

$r select 9
set db9size [$r dbsize]
$r select 10
set db10size [$r dbsize]
if {$db9size != 0 || $db10size != 0} {
    puts "Can't run the tests against DB 9 and 10: DBs are not empty."
    exit 1
}
$r close
unset r
unset db9size
unset db10size

if {$::stress} {
    stress
} else {
    main $::host $::port
}
