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
        set f [randomValue]
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
            } {
                $r hset $k $f $v
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
            {hash} {
                randpath {$r hset $k $f $v} \
                        {$r hdel $k $f}
            }
        }
    }
}
