#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Copyright (c) 2024-present, Valkey contributors.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#
# Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
#

source tests/support/cli.tcl

# Allocate slot 0 to the last primary and evenly distribute the remaining
# slots to the remaining primaries.
proc my_slot_allocation {masters} {
    set avg [expr double(16384) / [expr $masters-1]]
    set slot_start 1
    for {set j 0} {$j < $masters-1} {incr j} {
        set slot_end [expr int(ceil(($j + 1) * $avg) - 1)]
        R $j cluster addslotsrange $slot_start $slot_end
        set slot_start [expr $slot_end + 1]
    }
    R [expr $masters-1] cluster addslots 0
}

proc get_my_primary_peer {srv_idx} {
    set role_response [R $srv_idx role]
    set primary_ip [lindex $role_response 1]
    set primary_port [lindex $role_response 2]
    set primary_peer "$primary_ip:$primary_port"
    return $primary_peer
}

proc test_migrated_replica {type validity_factor sub_replica} {
    if {$sub_replica && $validity_factor == 0} {
        set test_name "Sub-replica reports zero repl offset and rank, and fails to win election"
    } elseif {$sub_replica} {
        set test_name "Unsynchronized sub-replica cannot fail over with the default validity factor"
    } elseif {$validity_factor == 0} {
        set test_name "Migrated replica reports zero repl offset and rank, and fails to win election"
    } else {
        set test_name "Unsynchronized migrated replicas cannot fail over with the default validity factor"
    }

    test "$test_name - $type" {
        # Write a key to primary 0, slot 1, make a small repl_offset.
        set small_value [string repeat "x" 1024]
        R 0 set key_991803 $small_value
        assert_equal $small_value [R 0 get key_991803]
        wait_for_ofs_sync [Rn 0] [Rn 4]

        # Write a key to primary 3, slot 0, make a big repl_offset.
        set large_value [string repeat "y" 10240]
        R 3 set key_977613 $large_value
        assert_equal $large_value [R 3 get key_977613]
        wait_for_ofs_sync [Rn 3] [Rn 7]

        R 3 config set cluster-replica-validity-factor $validity_factor
        R 7 config set cluster-replica-validity-factor $validity_factor
        R 3 config set cluster-allow-replica-migration yes
        if {$sub_replica} {
            # Keep replica 7 attached to replica 3 long enough to exercise the
            # sub-replica flattening path after replica 3 moves to shard 0.
            R 7 config set cluster-allow-replica-migration no
        } else {
            R 7 config set cluster-allow-replica-migration yes
        }

        # 100s, make sure primary 0 will hang in the save.
        R 0 config set rdb-key-save-delay 100000000

        # Move the slot 0 from primary 3 to primary 0
        set addr "[srv 0 host]:[srv 0 port]"
        set myid [R 3 CLUSTER MYID]
        set code [catch {
            exec src/redis-cli {*}[rediscli_tls_config "tests"] --cluster rebalance $addr \
                --cluster-weight $myid=0 --cluster-yes
        } result]
        if {$code != 0} {
            fail "redis-cli --cluster rebalance returns non-zero exit code, output below:\n$result"
        }

        # redis-cli returns after rebalancing the slots, but the replicas are
        # reconfigured asynchronously, including sub-replica flattening. Before
        # failing primary 0, wait until both migrated replicas point to it and
        # verify that neither has completed its first full sync.
        wait_for_condition 1000 50 {
            [get_my_primary_peer 3] eq $addr &&
            [get_my_primary_peer 7] eq $addr &&
            [lindex [R 3 role] 4] <= 0 &&
            [lindex [R 7 role] 4] <= 0
        } else {
            puts "R 3 role: [R 3 role]"
            puts "R 7 role: [R 7 role]"
            fail "Migrated replicas did not remain unsynchronized with primary 0"
        }

        if {$sub_replica} {
            verify_log_message -7 "*I'm a sub-replica!*" 0
        }

        if {$validity_factor != 0} {
            # Pause the only replica that was synchronized with primary 0. This
            # leaves the two cross-shard replicas as the only active candidates.
            set replica4_pid [s -4 process_id]
            pause_process $replica4_pid
        }

        if {$type == "shutdown"} {
            # Shutdown primary 0.
            catch {R 0 shutdown nosave}
        } elseif {$type == "sigstop"} {
            # Pause primary 0.
            set primary0_pid [s 0 process_id]
            pause_process $primary0_pid
        }

        if {$validity_factor != 0} {
            # Neither migrated replica may start an election before completing
            # its first synchronization with its new primary.
            foreach replica {3 7} {
                wait_for_log_messages -$replica {
                    "*Currently unable to failover: Disconnected from master for longer than allowed*"
                } 0 1000 50
                assert_equal slave [s -$replica role]
                assert_equal 0 [count_log_message -$replica "Start of election"]
            }

            # Once the synchronized replica is available, it must restore the
            # shard instead of either cross-shard replica.
            resume_process $replica4_pid
        }

        # Wait for the replica to become a primary, and make sure
        # the other primary become a replica.
        wait_for_condition 1000 50 {
            [s -4 role] eq {master} &&
            [s -3 role] eq {slave} &&
            [s -7 role] eq {slave}
        } else {
            puts "s -4 role: [s -4 role]"
            puts "s -3 role: [s -3 role]"
            puts "s -7 role: [s -7 role]"
            fail "Failover does not happened"
        }

        if {$validity_factor == 0} {
            # Cluster communication may let replica 4 start its election before
            # replicas 3 and 7 schedule theirs. If either migrated replica does
            # schedule an election, make sure it reports the cleared offset.
            foreach replica {3 7} {
                if {[count_log_message -$replica "Start of election"] != 0} {
                    verify_log_message -$replica "*Start of election*offset 0*" 0
                }
            }
        } else {
            # The earlier assertions were made before replica 4 was resumed.
            # Check again after failover so an unsynchronized migrated replica
            # cannot start an election in the interval before replica 4 wins.
            foreach replica {3 7} {
                assert_equal 0 [count_log_message -$replica "Start of election"]
            }
        }

        # Make sure the right replica gets the higher rank.
        verify_log_message -4 "*Start of election*rank #0*" 0

        # Wait for the cluster to be ok.
        wait_for_condition 1000 50 {
            [normalize_cluster_slots [R 3 cluster slots]] eq [normalize_cluster_slots [R 4 cluster slots]] &&
            [normalize_cluster_slots [R 4 cluster slots]] eq [normalize_cluster_slots [R 7 cluster slots]] &&
            [CI 3 cluster_state] eq "ok" &&
            [CI 4 cluster_state] eq "ok" &&
            [CI 7 cluster_state] eq "ok"
        } else {
            puts "R 3: [R 3 cluster info]"
            puts "R 4: [R 4 cluster info]"
            puts "R 7: [R 7 cluster info]"
            fail "Cluster is down"
        }

        # Make sure the key exists and is consistent.
        R 3 readonly
        R 7 readonly
        wait_for_condition 1000 50 {
            [R 3 get key_991803] eq $small_value && [R 3 get key_977613] eq $large_value &&
            [R 4 get key_991803] eq $small_value && [R 4 get key_977613] eq $large_value &&
            [R 7 get key_991803] eq $small_value && [R 7 get key_977613] eq $large_value
        } else {
            catch {puts "R 3: [R 3 keys *]"}
            catch {puts "R 4: [R 4 keys *]"}
            catch {puts "R 7: [R 7 keys *]"}
            fail "Key not consistent"
        }

        if {$type == "sigstop"} {
            resume_process $primary0_pid

            # Wait for the old primary to go online and become a replica.
            wait_for_condition 1000 50 {
                [s 0 role] eq {slave}
            } else {
                fail "The old primary was not converted into replica"
            }
        }
    }
} ;# proc

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_migrated_replica "shutdown" 0 false
} my_slot_allocation ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_migrated_replica "sigstop" 0 false
} my_slot_allocation ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_migrated_replica "shutdown" 10 false
} my_slot_allocation ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_migrated_replica "sigstop" 10 false
} my_slot_allocation ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_migrated_replica "shutdown" 0 true
} my_slot_allocation ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_migrated_replica "sigstop" 0 true
} my_slot_allocation ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_migrated_replica "shutdown" 10 true
} my_slot_allocation ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_migrated_replica "sigstop" 10 true
} my_slot_allocation ;# start_cluster

proc test_nonempty_replica {type validity_factor} {
    if {$validity_factor == 0} {
        set test_name "New non-empty replica reports zero repl offset and rank, and fails to win election"
    } else {
        set test_name "Unsynchronized non-empty replica cannot fail over with the default validity factor"
    }

    test "$test_name - $type" {
        # Write a key to primary 0, slot 1, make a small repl_offset.
        set small_value [string repeat "x" 1024]
        R 0 set key_991803 $small_value
        assert_equal $small_value [R 0 get key_991803]
        wait_for_ofs_sync [Rn 0] [Rn 4]

        # Write a key to primary 3, slot 0, make a big repl_offset.
        set large_value [string repeat "y" 10240]
        R 3 set key_977613 $large_value
        assert_equal $large_value [R 3 get key_977613]
        wait_for_ofs_sync [Rn 3] [Rn 7]

        # 100s, make sure primary 0 will hang in the save.
        R 0 config set rdb-key-save-delay 100000000

        # Make server 7 a replica of server 0.
        R 7 config set cluster-replica-validity-factor $validity_factor
        R 7 config set cluster-allow-replica-migration yes
        set addr "[srv 0 host]:[srv 0 port]"
        R 7 cluster replicate [R 0 cluster myid]
        assert_equal $addr [get_my_primary_peer 7]
        assert {[lindex [R 7 role] 4] <= 0}

        if {$validity_factor != 0} {
            # Pause the only replica that has synchronized with primary 0, so
            # server 7 is the only active failover candidate.
            set replica4_pid [s -4 process_id]
            pause_process $replica4_pid
        }

        if {$type == "shutdown"} {
            # Shutdown primary 0.
            catch {R 0 shutdown nosave}
        } elseif {$type == "sigstop"} {
            # Pause primary 0.
            set primary0_pid [s 0 process_id]
            pause_process $primary0_pid
        }

        if {$validity_factor != 0} {
            wait_for_log_messages -7 {
                "*Currently unable to failover: Disconnected from master for longer than allowed*"
            } 0 1000 50
            assert_equal slave [s -7 role]
            assert_equal 0 [count_log_message -7 "Start of election"]

            # Let the synchronized replica restore the shard.
            resume_process $replica4_pid
        }

        # Wait for the replica to become a primary.
        wait_for_condition 1000 50 {
            [s -4 role] eq {master} &&
            [s -7 role] eq {slave}
        } else {
            puts "s -4 role: [s -4 role]"
            puts "s -7 role: [s -7 role]"
            fail "Failover does not happened"
        }

        verify_log_message -4 "*Start of election*rank #0*" 0

        if {$validity_factor == 0} {
            # Replica 4 may start its election before replica 7 can schedule
            # one. If replica 7 does schedule an election, make sure it reports
            # the cleared offset.
            if {[count_log_message -7 "Start of election"] != 0} {
                verify_log_message -7 "*Start of election*offset 0*" 0
            }
        } else {
            # The earlier assertion was made before replica 4 was resumed.
            # Check again after failover so replica 7 cannot start an election
            # in the interval before replica 4 wins.
            assert_equal 0 [count_log_message -7 "Start of election"]
        }

        # Wait for the cluster to be ok.
        wait_for_condition 1000 50 {
            [normalize_cluster_slots [R 4 cluster slots]] eq [normalize_cluster_slots [R 7 cluster slots]] &&
            [CI 4 cluster_state] eq "ok" &&
            [CI 7 cluster_state] eq "ok"
        } else {
            puts "R 4: [R 4 cluster info]"
            puts "R 7: [R 7 cluster info]"
            fail "Cluster is down"
        }

        # Make sure the key exists and is consistent.
        R 7 readonly
        wait_for_condition 1000 50 {
            [R 4 get key_991803] eq $small_value &&
            [R 7 get key_991803] eq $small_value
        } else {
            catch {puts "R 4: [R 4 get key_991803]"}
            catch {puts "R 7: [R 7 get key_991803]"}
            fail "Key not consistent"
        }

        if {$type == "sigstop"} {
            resume_process $primary0_pid

            # Wait for the old primary to go online and become a replica.
            wait_for_condition 1000 50 {
                [s 0 role] eq {slave}
            } else {
                fail "The old primary was not converted into replica"
            }
        }
    }
} ;# proc

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_nonempty_replica "shutdown" 0
} my_slot_allocation ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_nonempty_replica "sigstop" 0
} my_slot_allocation ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_nonempty_replica "shutdown" 10
} my_slot_allocation ;# start_cluster

start_cluster 4 4 {tags {external:skip cluster} overrides {cluster-node-timeout 1000 cluster-migration-barrier 999 shutdown-timeout 0}} {
    test_nonempty_replica "sigstop" 10
} my_slot_allocation ;# start_cluster
