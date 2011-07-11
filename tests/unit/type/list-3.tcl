start_server {
    tags {list ziplist}
    overrides {
        "list-max-ziplist-value" 200000
        "list-max-ziplist-entries" 256
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

    tags {slow} {
        test {ziplist implementation: value encoding and backlink} {
            for {set j 0} {$j < 100} {incr j} {
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
                for {set i 0} {$i < 200} {incr i} {
                    if {[lindex $l $i] ne [r lindex l $i]} {
                        assert_equal [lindex $l $i] [r lindex l $i]
                    }
                }
            }
        }
    }
}
