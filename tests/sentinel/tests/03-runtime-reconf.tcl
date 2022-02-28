# Test runtime reconfiguration command SENTINEL SET.
source "../tests/includes/init-tests.tcl"
set num_sentinels [llength $::sentinel_instances]

test "Set parameters in normal case" {

    set info [S 0 SENTINEL master mymaster]
    set origin_quorum [dict get $info quorum]
    set origin_down_after_milliseconds [dict get $info down-after-milliseconds]
    set update_quorum [expr $origin_quorum+1]
    set update_down_after_milliseconds [expr $origin_down_after_milliseconds+1000]
    
    assert_equal [S 0 SENTINEL SET mymaster quorum $update_quorum] "OK"
    assert_equal [S 0 SENTINEL SET mymaster down-after-milliseconds $update_down_after_milliseconds] "OK"

    set update_info [S 0 SENTINEL master mymaster]
    assert {[dict get $update_info quorum] != $origin_quorum}
    assert {[dict get $update_info down-after-milliseconds] != $origin_down_after_milliseconds}
    
    #restore to origin config parameters
    assert_equal [S 0 SENTINEL SET mymaster quorum $origin_quorum] "OK"
    assert_equal [S 0 SENTINEL SET mymaster down-after-milliseconds $origin_down_after_milliseconds] "OK"
}

test "Set parameters in normal case with bad format" {

    set info [S 0 SENTINEL master mymaster]
    set origin_down_after_milliseconds [dict get $info down-after-milliseconds]

    assert_error "ERR Invalid argument '-20' for SENTINEL SET 'down-after-milliseconds'*" {S 0 SENTINEL SET mymaster down-after-milliseconds -20}
    assert_error "ERR Invalid argument 'abc' for SENTINEL SET 'down-after-milliseconds'*" {S 0 SENTINEL SET mymaster down-after-milliseconds "abc"}

    set current_info [S 0 SENTINEL master mymaster]
    assert {[dict get $current_info down-after-milliseconds] == $origin_down_after_milliseconds}
}

test "Sentinel Set with other error situations" {

   # non-existing script
   assert_error "ERR Notification script seems non existing*" {S 0 SENTINEL SET mymaster notification-script test.txt}

   # wrong parameter number
   assert_error "ERR wrong number of arguments for 'sentinel|set' command" {S 0 SENTINEL SET mymaster fakeoption}

   # unknown parameter option
   assert_error "ERR Unknown option or number of arguments for SENTINEL SET 'fakeoption'" {S 0 SENTINEL SET mymaster fakeoption fakevalue}

   # save new config to disk failed
   set info [S 0 SENTINEL master mymaster]
   set origin_quorum [dict get $info quorum]
   set update_quorum [expr $origin_quorum+1]
   set sentinel_id 0
   set configfilename [file join "sentinel_$sentinel_id" "sentinel.conf"]
   set configfilename_bak [file join "sentinel_$sentinel_id" "sentinel.conf.bak"]

   file rename $configfilename $configfilename_bak
   file mkdir $configfilename

   catch {[S 0 SENTINEL SET mymaster quorum $update_quorum]} err

   file delete $configfilename
   file rename $configfilename_bak $configfilename

   assert_match "ERR Failed to save config file*" $err
}
