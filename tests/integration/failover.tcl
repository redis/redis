start_server {tags {"failover"}} {
start_server {} {
start_server {} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    set replica_1 [srv -1 client]
    set replica_1_host [srv -1 host]
    set replica_1_port [srv -1 port]
    set replica_2 [srv -2 client]

    test {failover command fails without connected replica} {
        catch { $primary failover to $replica_1_host $replica_1_port } err
        if {! [string match "ERR*" $err]} {
            fail "failover command succeeded when replica not connected"
        }
    }

    $replica_1 replicaof $primary_host $primary_port
    $replica_2 replicaof $primary_host $primary_port
    wait_for_sync $replica_1
    wait_for_sync $replica_2

    test {failover command fails with invalid host} {
        catch { $primary failover to invalidhost $replica_1_port } err
        if {! [string match "ERR*" $err]} {
            fail "failover command succeeded with invalid host"
        }
    }

    test {failover command fails with invalid port} {
        catch { $primary failover to $replica_1_host invalidport } err
        if {! [string match "ERR*" $err]} {
            fail "failover command succeeded with invalid port"
        }
    }

    test {failover command fails when sent to a replica} {
        catch { $replica_1 failover to $replica_1_host $replica_1_port } err
        if {! [string match "ERR*" $err]} {
            fail "failover command succeeded when sent to replica"
        }
    }

    test {failover command to specific replica works} {
        $primary failover to $replica_1_host $replica_1_port

        # Wait for failover to end
        wait_for_condition 50 100 {
            [s 0 master_failover_state] == "no-failover"
        } else {
            fail "Failover from primary to replica did not finish"
        }

        assert_match *slave* [$primary role]
        assert_match *master* [$replica_1 role]
    }

    # Reset replication state
    $primary replicaof no one
    $replica_1 replicaof $primary_host $primary_port
    $replica_2 replicaof $primary_host $primary_port
    wait_for_sync $replica_1
    wait_for_sync $replica_2

    test {failover command to any replica works} {
        $primary failover to any one

        # Wait for failover to end
        wait_for_condition 50 100 {
            [s 0 master_failover_state] == "no-failover"
        } else {
            fail "Failover from primary to replica did not finish"
        }

        assert_match *slave* [$primary role]
        assert_match *master* [$replica_1 role]
        assert_match *slave* [$replica_2 role]
    }

    $primary replicaof no one
    $replica_1 replicaof $primary_host $primary_port
    $replica_2 replicaof $primary_host $primary_port
    wait_for_sync $replica_1
    wait_for_sync $replica_2

    test {failover to a replica with force works} {
        exec kill -SIGSTOP [srv -1 pid]
        #replica will never acknowledge this write
        $primary set case 2
        $primary failover to [srv -1 host] [srv -1 port] TIMEOUT 10 FORCE
        exec kill -SIGCONT [srv -1 pid]

        # Wait for failover to end
        wait_for_condition 50 100 {
            [s 0 master_failover_state] == "no-failover"
        } else {
            fail "Failover from primary to replica did not finish"
        }

        assert_match *slave* [$primary role]
        assert_match *master* [$replica_1 role]
        assert_match *slave* [$replica_2 role]
    }

    $primary replicaof no one
    $replica_1 replicaof $primary_host $primary_port
    $replica_2 replicaof $primary_host $primary_port
    wait_for_sync $replica_1
    wait_for_sync $replica_2

    test {failover with timeout aborts if replica never catches up} {
        # Stop replica so it never catches up
        exec kill -SIGSTOP [srv -1 pid]
        $primary SET CASE 1
        
        $primary failover to [srv -1 host] [srv -1 port] TIMEOUT 500
        # Wait for failover to end
        wait_for_condition 50 20 {
            [s 0 master_failover_state] == "no-failover"
        } else {
            fail "Failover from primary to replica did not finish"
        }

        exec kill -SIGCONT [srv -1 pid]

        assert_match *master* [$primary role]
        assert_match *slave* [$replica_1 role]
        assert_match *slave* [$replica_2 role]
    }

    $replica_1 replicaof $primary_host $primary_port
    $replica_2 replicaof $primary_host $primary_port
    wait_for_sync $replica_1
    wait_for_sync $replica_2

    test {failover aborts if target rejects sync attempt} {
        $replica_1 acl setuser default -psync
        $primary failover to $replica_1_host $replica_1_port

        # Wait for failover to end
        wait_for_condition 50 100 {
            [s 0 master_failover_state] == "no-failover"
        } else {
            fail "Failover from primary to replica did not finish"
        }

        # We need to make sure the nodes actually sync back up
        wait_for_ofs_sync $primary $replica_2
        wait_for_ofs_sync $primary $replica_2

        assert_match *master* [$primary role]
        assert_match *slave* [$replica_1 role]
        assert_match *slave* [$replica_2 role]
    }
}
}
}
