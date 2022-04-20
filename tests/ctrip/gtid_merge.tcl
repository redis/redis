

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
        r ctrip.merge k $val -1
        assert_equal [r get k] v
        r ctrip.merge_end A:2-100,B:3-200
        assert_equal [dict get [get_gtid r] "A"] "1-100"
        assert_equal [dict get [get_gtid r] "B"] "2-200"
    }


}