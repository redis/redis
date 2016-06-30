start_server {
    tags {list ziplist}
    overrides {
        "list-max-ziplist-size" 16
    }
} {
    test {Explicit regression for a list bug} {
        set mylist {49376042582 {BkG2o\pIC]4YYJa9cJ4GWZalG[4tin;1D2whSkCOW`mX;SFXGyS8sedcff3fQI^tgPCC@^Nu1J6o]meM@Lko]t_jRyo<xSJ1oObDYd`ppZuW6P@fS278YaOx=s6lvdFlMbP0[SbkI^Kr\HBXtuFaA^mDx:yzS4a[skiiPWhT<nNfAf=aQVfclcuwDrfe;iVuKdNvB9kbfq>tK?tH[\EvWqS]b`o2OCtjg:?nUTwdjpcUm]y:pg5q24q7LlCOwQE^}}
        r del l
        r rpush l [lindex $mylist 0]
        r rpush l [lindex $mylist 1]
        assert_equal [r lindex l 0] [lindex $mylist 0]
        assert_equal [r lindex l 1] [lindex $mylist 1]
    }

    test {Regression for quicklist #3343 bug} {
        r del mylist
        r lpush mylist 401
        r lpush mylist 392
        r rpush mylist [string repeat x 5105]"799"
        r lset mylist -1 [string repeat x 1014]"702"
        r lpop mylist
        r lset mylist -1 [string repeat x 4149]"852"
        r linsert mylist before 401 [string repeat x 9927]"12"
        r lrange mylist 0 -1
        r ping ; # It's enough if the server is still alive
    } {PONG}

    test {Stress tester for #3343-alike bugs} {
        r del key
        for {set j 0} {$j < 10000} {incr j} {
            set op [randomInt 6]
            set small_signed_count [expr 5-[randomInt 10]]
            if {[randomInt 2] == 0} {
                set ele [randomInt 1000]
            } else {
                set ele [string repeat x [randomInt 10000]][randomInt 1000]
            }
            switch $op {
                0 {r lpush key $ele}
                1 {r rpush key $ele}
                2 {r lpop key}
                3 {r rpop key}
                4 {
                    catch {r lset key $small_signed_count $ele}
                }
                5 {
                    set otherele [randomInt 1000]
                    if {[randomInt 2] == 0} {
                        set where before
                    } else {
                        set where after
                    }
                    r linsert key $where $otherele $ele
                }
            }
        }
    }

    tags {slow} {
        test {ziplist implementation: value encoding and backlink} {
            if {$::accurate} {set iterations 100} else {set iterations 10}
            for {set j 0} {$j < $iterations} {incr j} {
                r del l
                set l {}
                for {set i 0} {$i < 200} {incr i} {
                    randpath {
                        set data [string repeat x [randomInt 100000]]
                    } {
                        set data [randomInt 65536]
                    } {
                        set data [randomInt 4294967296]
                    } {
                        set data [randomInt 18446744073709551616]
                    } {
                        set data -[randomInt 65536]
                        if {$data eq {-0}} {set data 0}
                    } {
                        set data -[randomInt 4294967296]
                        if {$data eq {-0}} {set data 0}
                    } {
                        set data -[randomInt 18446744073709551616]
                        if {$data eq {-0}} {set data 0}
                    }
                    lappend l $data
                    r rpush l $data
                }
                assert_equal [llength $l] [r llen l]
                # Traverse backward
                for {set i 199} {$i >= 0} {incr i -1} {
                    if {[lindex $l $i] ne [r lindex l $i]} {
                        assert_equal [lindex $l $i] [r lindex l $i]
                    }
                }
            }
        }

        test {ziplist implementation: encoding stress testing} {
            for {set j 0} {$j < 200} {incr j} {
                r del l
                set l {}
                set len [randomInt 400]
                for {set i 0} {$i < $len} {incr i} {
                    set rv [randomValue]
                    randpath {
                        lappend l $rv
                        r rpush l $rv
                    } {
                        set l [concat [list $rv] $l]
                        r lpush l $rv
                    }
                }
                assert_equal [llength $l] [r llen l]
                for {set i 0} {$i < $len} {incr i} {
                    if {[lindex $l $i] ne [r lindex l $i]} {
                        assert_equal [lindex $l $i] [r lindex l $i]
                    }
                }
            }
        }
    }
}
