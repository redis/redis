# Test runtime reconfiguration command SENTINEL SET.
source "../tests/includes/init-tests.tcl"
set num_sentinels [llength $::sentinel_instances]

set ::user "testuser"
set ::password "secret"

proc server_set_password {} {
    foreach_redis_id id {
        assert_equal {OK} [R $id CONFIG SET requirepass $::password]
        assert_equal {OK} [R $id AUTH $::password]
        assert_equal {OK} [R $id CONFIG SET masterauth $::password]
    }
}

proc server_reset_password {} {
    foreach_redis_id id {
        assert_equal {OK} [R $id CONFIG SET requirepass ""]
        assert_equal {OK} [R $id CONFIG SET masterauth ""]
    }
}

proc server_set_acl {id} {
    assert_equal {OK} [R $id ACL SETUSER $::user on >$::password allchannels +@all]
    assert_equal {OK} [R $id ACL SETUSER default off]

    R $id CLIENT KILL USER default SKIPME no
    assert_equal {OK} [R $id AUTH $::user $::password]
    assert_equal {OK} [R $id CONFIG SET masteruser $::user]
    assert_equal {OK} [R $id CONFIG SET masterauth $::password]
}

proc server_reset_acl {id} {
    assert_equal {OK} [R $id ACL SETUSER default on]
    assert_equal {1} [R $id ACL DELUSER $::user]

    assert_equal {OK} [R $id CONFIG SET masteruser ""]
    assert_equal {OK} [R $id CONFIG SET masterauth ""]
}

proc verify_sentinel_connect_replicas {id} {
    foreach replica [S $id SENTINEL REPLICAS mymaster] {
        if {[string match "*disconnected*" [dict get $replica flags]]} {
            return 0
        }
    }
    return 1
}

proc wait_for_sentinels_connect_servers { {is_connect 1} } {
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [string match "*disconnected*" [dict get [S $id SENTINEL MASTER mymaster] flags]] != $is_connect
        } else {
            fail "At least some sentinel can't connect to master"
        }

        wait_for_condition 1000 50 {
            [verify_sentinel_connect_replicas $id] == $is_connect
        } else {
            fail "At least some sentinel can't connect to replica"
        }
    }
}

test "Sentinels (re)connection following SENTINEL SET mymaster auth-pass" {
    # 3 types of sentinels to test:
    # (re)started while master changed pwd. Manage to connect only after setting pwd
    set sent2re 0
    # (up)dated in advance with master new password
    set sent2up 1
    # (un)touched. Yet manage to maintain (old) connection
    set sent2un 2

    wait_for_sentinels_connect_servers
    kill_instance sentinel $sent2re
    server_set_password
    assert_equal {OK} [S $sent2up SENTINEL SET mymaster auth-pass $::password]
    restart_instance sentinel $sent2re

    # Verify sentinel that restarted failed to connect master
    wait_for_condition 100 50 {
        [string match "*disconnected*" [dict get [S $sent2re SENTINEL MASTER mymaster] flags]] != 0
    } else {
        fail "Expected to be disconnected from master due to wrong password"
    }

    # Update restarted sentinel with master password
    assert_equal {OK} [S $sent2re SENTINEL SET mymaster auth-pass $::password]

    # All sentinels expected to connect successfully
    wait_for_sentinels_connect_servers

    # remove requirepass and verify sentinels manage to connect servers
    server_reset_password
    wait_for_sentinels_connect_servers
    # Sanity check
    verify_sentinel_auto_discovery
}

test "Sentinels (re)connection following master ACL change" {
    # Three types of sentinels to test during ACL change:
    # 1. (re)started Sentinel. Manage to connect only after setting new pwd
    # 2. (up)dated Sentinel, get just before ACL change the new password
    # 3. (un)touched Sentinel that kept old connection with master and didn't
    #    set new ACL password won't persist ACL pwd change (unlike legacy auth-pass)
    set sent2re 0
    set sent2up 1
    set sent2un 2

    wait_for_sentinels_connect_servers
    # kill sentinel 'sent2re' and restart it after ACL change
    kill_instance sentinel $sent2re

    # Update sentinel 'sent2up' with new user and pwd
    assert_equal {OK} [S $sent2up SENTINEL SET mymaster auth-user $::user]
    assert_equal {OK} [S $sent2up SENTINEL SET mymaster auth-pass $::password]

    foreach_redis_id id {
        server_set_acl $id
    }

    restart_instance sentinel $sent2re

    # Verify sentinel that restarted failed to reconnect master
    wait_for_condition 100 50 {
        [string match "*disconnected*" [dict get [S $sent2re SENTINEL MASTER mymaster] flags]] != 0
    } else {
        fail "Expected: Restarted sentinel to be disconnected from master due to obsolete password"
    }

    # Verify sentinel with updated password managed to connect (wait for sentinelTimer to reconnect)
    wait_for_condition 100 50 {
        [string match "*disconnected*" [dict get [S $sent2up SENTINEL MASTER mymaster] flags]] == 0
    } else {
        fail "Expected: Sentinel to be connected to master"
    }

    # Verify sentinel untouched gets failed to connect master
    wait_for_condition 100 50 {
        [string match "*disconnected*" [dict get [S $sent2un SENTINEL MASTER mymaster] flags]] != 0
    } else {
        fail "Expected: Sentinel to be disconnected from master due to obsolete password"
    }

    # Now update all sentinels with new password
    foreach_sentinel_id id {
        assert_equal {OK} [S $id SENTINEL SET mymaster auth-user $::user]
        assert_equal {OK} [S $id SENTINEL SET mymaster auth-pass $::password]
    }

    # All sentinels expected to connect successfully
    wait_for_sentinels_connect_servers

    # remove requirepass and verify sentinels manage to connect servers
    foreach_redis_id id {
        server_reset_acl $id
    }

    wait_for_sentinels_connect_servers
    # Sanity check
    verify_sentinel_auto_discovery
}

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
