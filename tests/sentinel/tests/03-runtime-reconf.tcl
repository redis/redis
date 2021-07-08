# Test runtime reconfiguration command SENTINEL SET.

source "../tests/includes/init-tests.tcl"

set ::user "testuser"
set ::password "secret"

proc setup_server_auth {} {
    foreach_redis_id id {
        assert_equal {OK} [R $id CONFIG SET requirepass $::password]
        assert_equal {OK} [R $id AUTH $::password]
        assert_equal {OK} [R $id CONFIG SET masterauth $::password]
    }
}

proc teardown_server_auth {} {
    foreach_redis_id id {
        assert_equal {OK} [R $id CONFIG SET requirepass ""]
        assert_equal {OK} [R $id CONFIG SET masterauth ""]
    }
}

proc setup_server_acl {id} {
    assert_equal {OK} [R $id ACL SETUSER $::user >$::password +@all on]
    assert_equal {OK} [R $id ACL SETUSER default off]

    R $id CLIENT KILL USER default SKIPME no
    assert_equal {OK} [R $id AUTH $::user $::password]
    assert_equal {OK} [R $id CONFIG SET masteruser $::user]
    assert_equal {OK} [R $id CONFIG SET masterauth $::password]
}

proc teardown_server_acl {id} {
    assert_equal {OK} [R $id ACL SETUSER default on]
    assert_equal {1} [R $id ACL DELUSER $::user]

    assert_equal {OK} [R $id CONFIG SET masteruser ""]
    assert_equal {OK} [R $id CONFIG SET masterauth ""]
}

proc verify_replicas_flags {id} {
    foreach replica [S $id SENTINEL REPLICAS mymaster] {
        if {[string match "*down*" [dict get $replica flags]]} {
            return 0
        }
    }
    return 1
}

proc check_instances_status {} {
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            ![string match "*down*" [dict get [S $id SENTINEL MASTER mymaster] flags]]
        } else {
            fail "At least some sentinel can't connect to master"
        }
    }
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [verify_replicas_flags $id]
        } else {
            fail "At least some sentinel can't connect to replica"
        }
    }
}

proc re_monitor_mymaster {} {
    foreach_sentinel_id id {
        catch {S $id SENTINEL REMOVE mymaster}
    }

    set master_id 0
    set sentinels [llength $::sentinel_instances]
    # avoid failover
    set quorum [expr {$sentinels + 1}]
    foreach_sentinel_id id {
        S $id SENTINEL MONITOR mymaster \
              [get_instance_attrib redis $master_id host] \
              [get_instance_attrib redis $master_id port] $quorum
    }
}

test "(server-auth-init) Set up requirepass configuration" {
    setup_server_auth
}

test "All servers have non-empty requirepass before monitored" {
    re_monitor_mymaster
    foreach_sentinel_id id {
        assert {[S $id sentinel master mymaster] ne {}}
        assert_equal {OK} [S $id SENTINEL SET mymaster down-after-milliseconds 2200]
        assert_equal {OK} [S $id SENTINEL SET mymaster auth-pass $::password]
    }

    after 3000
    check_instances_status
}

test "Only replicas have non-empty requirepass before monitored" {
    assert_equal {OK} [R $master_id CONFIG SET requirepass ""]
    re_monitor_mymaster
    assert_equal {OK} [R $master_id CONFIG SET requirepass $::password]
    foreach_sentinel_id id {
        assert {[S $id sentinel master mymaster] ne {}}
        assert_equal {OK} [S $id SENTINEL SET mymaster down-after-milliseconds 2200]
        assert_equal {OK} [S $id SENTINEL SET mymaster auth-pass $::password]
    }

    after 3000
    check_instances_status
}

test "(server-auth-cleanup) Tear down requirepass configuration" {
    teardown_server_auth
}

test "(server-ACL-init) Set up ACL configuration" {
    foreach_redis_id id {
        setup_server_acl $id
    }
    assert_equal $::user [R 0 ACL WHOAMI]
}

test "All servers have ACL configuration before monitored" {
    re_monitor_mymaster
    foreach_sentinel_id id {
        assert {[S $id sentinel master mymaster] ne {}}
        assert_equal {OK} [S $id SENTINEL SET mymaster down-after-milliseconds 2200]
        assert_equal {OK} [S $id SENTINEL SET mymaster auth-user $::user]
        assert_equal {OK} [S $id SENTINEL SET mymaster auth-pass $::password]
    }

    after 3000
    check_instances_status
}

test "Only replicas have ACL configuration before monitored" {
    teardown_server_acl $master_id
    re_monitor_mymaster
    setup_server_acl $master_id
    foreach_sentinel_id id {
        assert {[S $id sentinel master mymaster] ne {}}
        assert_equal {OK} [S $id SENTINEL SET mymaster down-after-milliseconds 2200]
        assert_equal {OK} [S $id SENTINEL SET mymaster auth-user $::user]
        assert_equal {OK} [S $id SENTINEL SET mymaster auth-pass $::password]
    }

    after 3000
    check_instances_status
}

test "(server-ACL-cleanup) Tear down ACL configuration" {
    foreach_redis_id id {
        teardown_server_acl $id
    }
}