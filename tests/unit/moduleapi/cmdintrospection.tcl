set testmodule [file normalize tests/modules/cmdintrospection.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    # cmdintrospection.xadd mimics XADD with regards to how
    # what COMMAND exposes. There are three differences:
    #
    # 1. cmdintrospection.xadd (and all module commands) do not have ACL categories
    # 2. cmdintrospection.xadd's `group` is "module"
    # 3. cmdintrospection.xadd has command hints and XADD doesn't (for now)
    #
    # This tests verify that, apart from the above differences, the output of
    # COMMAND INFO and COMMAND DOCS are identical for the two commands.
    test "Module command introspection via COMMAND INFO" {
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
                # Hints
                set hints [lindex $module_reply $i]
                assert_equal $hints {hint1 hint2 hint3}
                continue
            }
            assert_equal [lindex $redis_reply $i] [lindex $module_reply $i]
        }
    }

    test "Module command introspection via COMMAND DOCS" {
        set redis_reply [lindex [r command docs xadd] 1]
        set module_reply [lindex [r command docs cmdintrospection.xadd] 1]
        # Compare the map. We need to pop "group" first
        unset -nocomplain redis_dict
        foreach {k v} $redis_reply {
            dict append redis_dict $k $v
        }
        unset -nocomplain module_dict
        foreach {k v} $module_reply {
            dict append module_dict $k $v
        }
        dict unset redis_dict group
        dict unset module_dict group

        assert_equal $redis_dict $module_dict
    }
}
