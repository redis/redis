start_server {
    tags {"list"}
    overrides {
        "list-max-ziplist-value" 16
        "list-max-ziplist-entries" 256
    }
} {
    source "tests/unit/type/list-common.tcl"

    foreach {type large} [array get largevalue] {
        tags {"slow"} {
            test "LTRIM stress testing - $type" {
                set mylist {}
                set startlen 32
                r del mylist

                # Start with the large value to ensure the
                # right encoding is used.
                r rpush mylist $large
                lappend mylist $large

                for {set i 0} {$i < $startlen} {incr i} {
                    set str [randomInt 9223372036854775807]
                    r rpush mylist $str
                    lappend mylist $str
                }

                for {set i 0} {$i < 1000} {incr i} {
                    set min [expr {int(rand()*$startlen)}]
                    set max [expr {$min+int(rand()*$startlen)}]
                    set mylist [lrange $mylist $min $max]
                    r ltrim mylist $min $max
                    assert_equal $mylist [r lrange mylist 0 -1]

                    for {set j [r llen mylist]} {$j < $startlen} {incr j} {
                        set str [randomInt 9223372036854775807]
                        r rpush mylist $str
                        lappend mylist $str
                    }
                }
            }
        }
    }

    foreach {type large} [array get largevalue] {
        tags {"slow"} {
            test "LSPLICE stress testing - $type" {
                set mylist {}
                set startlen 32
                r del mylist

                # Start with the large value to ensure the
                # right encoding is used.
                r rpush mylist $large
                lappend mylist $large

                for {set i 0} {$i < $startlen} {incr i} {
                    set str [randomInt 9223372036854775807]
                    r rpush mylist $str
                    lappend mylist $str
                }

                for {set i 0} {$i < 1000} {incr i} {
                    set min [expr {int(rand()*$startlen)}]
                    set max [expr {$min+int(rand()*$startlen)}]
                    set lhs {}
                    if {$min > 0} {
                        set lhs [r lrange mylist 0 [expr $min-1]]
                    }
                    set rhs [r lrange mylist [expr $max+1] [r llen mylist]]
                    set expected [concat $lhs $rhs]
                    r lsplice mylist $min $max
                    assert_equal [r lrange mylist 0 [r llen mylist]] $expected

                    for {set j [r llen mylist]} {$j < $startlen} {incr j} {
                        set str [randomInt 9223372036854775807]
                        r rpush mylist $str
                        lappend mylist $str
                    }
                }
            }
        }
    }
}
