start_server {tags {"scan"}} {
    test "SCAN basic" {
        r flushdb
        r debug populate 1000

        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys $k
            if {$cur == 0} break
        }

        set keys [lsort -unique [concat {*}$keys]]
        assert_equal 1000 [llength $keys]
    }

    test "SCAN COUNT" {
        r flushdb
        r debug populate 1000

        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur count 5]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys $k
            if {$cur == 0} break
        }

        set keys [lsort -unique [concat {*}$keys]]
        assert_equal 1000 [llength $keys]
    }

    test "SCAN MATCH" {
        r flushdb
        r debug populate 1000

        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur match "key:1??"]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys $k
            if {$cur == 0} break
        }

        set keys [lsort -unique [concat {*}$keys]]
        assert_equal 100 [llength $keys]
    }
}
