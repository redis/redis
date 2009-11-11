# TODO # test pipelining

source redis.tcl

set ::passed 0
set ::failed 0

proc test {name code okpattern} {
    puts -nonewline [format "%-70s " $name]
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

proc main {server port} {
    set r [redis $server $port]
    $r select 9
    set err ""

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

    test {SET 10000 numeric keys and access all them in reverse order} {
        for {set x 0} {$x < 10000} {incr x} {
            $r set $x $x
        }
        set sum 0
        for {set x 9999} {$x >= 0} {incr x -1} {
            incr sum [$r get $x]
        }
        format $sum
    } {49995000}

    test {DBSIZE should be 10001 now} {
        $r dbsize
    } {10001}

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
        $r lpush mylist a
        $r lpush mylist b
        $r rpush mylist c
        set res [$r llen mylist]
        append res [$r lindex mylist 0]
        append res [$r lindex mylist 1]
        append res [$r lindex mylist 2]
    } {3bac}

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

    test {LLEN against non-list value error} {
        $r del mylist
        $r set mylist foobar
        catch {$r llen mylist} err
        format $err
    } {ERR*}

    test {LINDEX against non-list value error} {
        catch {$r lindex mylist 0} err
        format $err
    } {ERR*}

    test {LPUSH against non-list value error} {
        catch {$r lpush mylist 0} err
        format $err
    } {ERR*}

    test {RPUSH against non-list value error} {
        catch {$r rpush mylist 0} err
        format $err
    } {ERR*}

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

    test {LSET} {
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

    test {SUNIONSTORE with two sets} {
        $r sunionstore setres set1 set2
        lsort [$r smembers setres]
    } [lsort -uniq "[$r smembers set1] [$r smembers set2]"]

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
        $r lpush mysavelist hello
        $r lpush mysavelist world
        $r set myemptykey {}
        $r set mynormalkey {blablablba}
        $r zadd mytestzset a 10
        $r zadd mytestzset b 20
        $r zadd mytestzset c 30
        $r save
    } {OK}
    
    test {Create a random list} {
        set tosort {}
        array set seenrand {}
        for {set i 0} {$i < 10000} {incr i} {
            while 1 {
                # Make sure all the weights are different because
                # Redis does not use a stable sort but Tcl does.
                set rint [expr int(rand()*1000000)]
                if {![info exists seenrand($rint)]} break
            }
            set seenrand($rint) x
            $r lpush tosort $i
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

    test {ZSET basic ZADD and score update} {
        $r zadd ztmp 10 x
        $r zadd ztmp 20 y
        $r zadd ztmp 30 z
        set aux1 [$r zrange ztmp 0 -1]
        $r zadd ztmp 1 y
        set aux2 [$r zrange ztmp 0 -1]
        list $aux1 $aux2
    } {{x y z} {y x z}}

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

    test {ZRANGE and ZREVRANGE} {
        list [$r zrange ztmp 0 -1] [$r zrevrange ztmp 0 -1]
    } {{y x z} {z x y}}

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

    puts "\n[expr $::passed+$::failed] tests, $::passed passed, $::failed failed"
    if {$::failed > 0} {
        puts "\n*** WARNING!!! $::failed FAILED TESTS ***\n"
    }
    close $fd
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

# Before to run the test check if DB 9 and DB 10 are empty
set r [redis]
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

if {[llength $argv] == 0} {
    main 127.0.0.1 6379
} elseif {[llength $argv] == 1 && [lindex $argv 0] eq {stress}} {
    stress
} else {
    main [lindex $argv 0] [lindex $argv 1]
}
