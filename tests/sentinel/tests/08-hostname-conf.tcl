source "../tests/includes/utils.tcl"

proc set_redis_announce_ip {addr} {
    foreach_redis_id id {
        R $id config set replica-announce-ip $addr
    }
}

proc set_sentinel_config {keyword value} {
    foreach_sentinel_id id {
        S $id sentinel config set $keyword $value
    }
}

proc set_all_instances_hostname {hostname} {
    foreach_sentinel_id id {
        set_instance_attrib sentinel $id host $hostname
    }
    foreach_redis_id id {
        set_instance_attrib redis $id host $hostname
    }
}

test "(pre-init) Configure instances and sentinel for hostname use" {
    set ::host "localhost"
    restart_killed_instances
    set_all_instances_hostname $::host
    set_redis_announce_ip $::host
    set_sentinel_config resolve-hostnames yes
    set_sentinel_config announce-hostnames yes
}

source "../tests/includes/init-tests.tcl"

proc verify_hostname_announced {hostname} {
    foreach_sentinel_id id {
        # Master is reported with its hostname
        if {![string equal [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] 0] $hostname]} {
            return 0
        }

        # Replicas are reported with their hostnames
        foreach replica [S $id SENTINEL REPLICAS mymaster] {
            if {![string equal [dict get $replica ip] $hostname]} {
                return 0
            }
        }
    }
    return 1
}

test "Sentinel announces hostnames" {
    # Check initial state
    verify_hostname_announced $::host

    # Disable announce-hostnames and confirm IPs are used
    set_sentinel_config announce-hostnames no
    assert {[verify_hostname_announced "127.0.0.1"] || [verify_hostname_announced "::1"]}
}

test "HELLO retries the same config epoch after a DNS failure and persists the update" {
    # Use a separate master name so other sentinels' HELLO messages cannot
    # interfere with the configuration under test.
    set master_name dns-epoch-master
    set old_port [RPort $master_id]
    set new_port [RPort 1]
    S 0 SENTINEL CONFIG SET announce-hostnames yes
    S 0 SENTINEL MONITOR $master_name 127.0.0.1 $old_port 1

    set old_epoch [dict get [S 0 SENTINEL MASTER $master_name] config-epoch]
    set new_epoch [expr {$old_epoch + 1}]
    set sender_host [get_instance_attrib sentinel 1 host]
    set sender_port [get_instance_attrib sentinel 1 port]
    set sender_id [S 1 SENTINEL MYID]
    set hello "$sender_host,$sender_port,$sender_id,$new_epoch,$master_name"

    # An unresolvable hostname must leave both the address and epoch intact.
    assert_equal 1 [S 0 PUBLISH __sentinel__:hello \
        "$hello,unresolvable.invalid,$new_port,$new_epoch"]
    assert_equal [list 127.0.0.1 $old_port] \
        [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME $master_name]
    assert_equal $old_epoch \
        [dict get [S 0 SENTINEL MASTER $master_name] config-epoch]

    # Retry with a resolvable hostname and exactly the same config epoch.
    assert_equal 1 [S 0 PUBLISH __sentinel__:hello \
        "$hello,localhost,$new_port,$new_epoch"]
    assert_equal [list localhost $new_port] \
        [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME $master_name]
    assert_equal $new_epoch \
        [dict get [S 0 SENTINEL MASTER $master_name] config-epoch]

    # Check the automatic rewrite, without an explicit FLUSHCONFIG that could
    # hide an epoch assigned only after the address change was persisted.
    set config [split [read_file [file join sentinel_0 sentinel.conf]] "\n"]
    assert {[lsearch -exact $config \
        "sentinel monitor $master_name localhost $new_port 1"] >= 0}
    assert {[lsearch -exact $config \
        "sentinel config-epoch $master_name $new_epoch"] >= 0}

    S 0 SENTINEL REMOVE $master_name
    S 0 SENTINEL CONFIG SET announce-hostnames no
}

# We need to revert any special configuration because all tests currently
# share the same instances.
test "(post-cleanup) Configure instances and sentinel for IPs" {
    set ::host "127.0.0.1"
    set_all_instances_hostname $::host
    set_redis_announce_ip $::host
    set_sentinel_config resolve-hostnames no
    set_sentinel_config announce-hostnames no
}
