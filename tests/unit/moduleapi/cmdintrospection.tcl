set testmodule [file normalize tests/modules/cmdintrospection.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module command introspection via COMMAND" {
        # cmdintrospection.xadd mimics XADD with regards to how
        # what COMMAND exposes. There are three differences:
        # 1. cmdintrospection.xadd (and all module commands) do not have ACL categories
        # 2. cmdintrospection.xadd's `group` is "module"
        # 3. cmdintrospection.xadd has command hints (for testing) and XADD doesn't (for now)
        # This test verifies that, apart from the above differences, the output of COMMAND
        # is identical.
        set redis_reply [lindex [r command info xadd] 0]
        set module_reply [lindex [r command info cmdintrospection.xadd] 0]
        for {set i 1} {$i < [llength $redis_reply]} {incr i} {
            if {$i == 2} {
                # Remove the "module" flag
                set mylist [lindex $module_reply $i]
                set idx [lsearch $mylist "module"]
                set mylist [lreplace $mylist $idx $idx]
                lset module_reply $i $mylist
            }
            if {$i == 6} {
                # Skip ACL categories
                continue
            }
            if {$i == 7} {
                # Compare the map. We need to pop "group" first
                unset -nocomplain redis_dict
                foreach {k v} [lindex $redis_reply $i] {
                    dict append redis_dict $k $v
                }
                unset -nocomplain module_dict
                foreach {k v} [lindex $module_reply $i] {
                    dict append module_dict $k $v
                }
                dict unset redis_dict group
                dict unset module_dict group
                # Now we verify the command hints (which don't exist, yet, in vanilla XADD, just added synthetic ones for testing)
                set hints [dict get $module_dict hints]
                assert_equal $hints {hint1 hint2 hint3}
                dict unset module_dict hints

                assert_equal $redis_dict $module_dict
                continue
            }
            assert_equal [lindex $redis_reply $i] [lindex $module_reply $i]
        }
    }
}
