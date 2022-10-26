

start_server {tags {"gtid merge"} overrides {gtid-enabled yes}} {
    test "merge fail" {
        r set create_val v 
        set val [r ctrip.get_robj create_val]
        catch {r ctrip.merge merge_fail $val -1} error 
        assert_equal $error "ERR full sync failed"

        catch {r ctrip.merge_end A:1-100,B:2-200} error 
        assert_equal $error "ERR full sync failed"
    }
    test "double merge_end fail" {
        r ctrip.merge_start 
        r ctrip.merge_end A:1,B:2
        assert_equal [dict get [get_gtid r] "A"] 1
        assert_equal [dict get [get_gtid r] "B"] 2

        catch { r ctrip.merge_end A:2,B:2} error
        assert_equal $error "ERR full sync failed"
    }

    test "merge" {
        r set test_k v
        set val [r ctrip.get_robj test_k]
        r ctrip.merge_start 
        set old_state [r info gtid]
        r ctrip.merge k $val -1
        assert_equal [r info gtid] $old_state
        r ctrip.merge_end A:2-100,B:3-200
        assert_equal [dict get [get_gtid r] "A"] "1-100"
        assert_equal [dict get [get_gtid r] "B"] "2-200"
    }

    test "normal command in gtid_full_sync" {
        r ctrip.merge_start 
        set old_state [r info gtid]
        r set k v
        assert_equal [r get k] v
        assert_equal [r info gtid] $old_state

        set client [redis [srv 0 host] [srv 0 port]]
        $client set k v1
        if {[r info gtid] == $old_state} {
            assert_failed "when a client in full sync, other client normal command => gtid command fail"
        }
        r ctrip.merge_end A:2-100,B:3-200
        assert_equal [dict get [get_gtid r] "A"] "1-100"
        assert_equal [dict get [get_gtid r] "B"] "2-200"
    }

    test "multi in gtid_full_sync" {
        r ctrip.merge_start 
        set old_state [r info gtid]
        r multi 
        r set k v
        r exec
        assert_equal [r get k] v
        assert_equal [r info gtid] $old_state
        r ctrip.merge_end A:100-200,B:200-300
        assert_equal [dict get [get_gtid r] "A"] "1-200"
        assert_equal [dict get [get_gtid r] "B"] "2-300"
    }
}


start_server {tags {"gtid merge"} overrides {gtid-enabled yes}} {
    test 'merge_start' {
        set before_gid [get_gtid r]
        set repl [attach_to_replication_stream]
        r ctrip.merge_start
        r set k v 
        r select 0
        r set k v 
        r ctrip.merge_end A:1

        if {$::swap_mode == "disk" } {
            assert_replication_stream $repl {
                {select *}
                {ctrip.merge_start}
                {set k v}
                {set k v}
                {ctrip.merge_end A:1}
            }
        } else {
            assert_replication_stream $repl {
                {select *}
                {ctrip.merge_start}
                {set k v}
                {select 0}
                {set k v}
                {ctrip.merge_end A:1}
            }
        }

        set after_gid [get_gtid r]
        dict for {addr node} $before_gid {
            assert_equal [dict get $after_gid $addr] $node
        }

        assert [string match "*$before_gid*" $after_gid] 

        # reset dbid
        if {$::swap_mode != "disk"} {
            r select 9
        }

        close_replication_stream $repl
    }



    test "ctrip.merge_end" {
        r ctrip.merge_start 
        catch {r ctrip.merge_end} error 
        assert_equal $error "ERR wrong number of arguments for 'ctrip.merge_end' command"
    }
}
