start_server {tags {"failoverto"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    set replica_pid [srv 0 pid]

    start_server {} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        proc reset_replication {replica replica_pid primary primary_host primary_port} {
            wait_for_condition 50 100 {
                [string match *master_failover_in_progress:0* [$primary info replication]]
            } else {
                fail "Replica did not finish sync"
            }

            # In case we stopped the replica
            exec kill -SIGCONT $replica_pid
            # Start the replication process...
            $primary replicaof no one
            $primary flushall
            $primary set FOO BAR
            $replica replicaof $primary_host $primary_port

            wait_for_condition 50 100 {
                [string match *master_link_status:up* [$replica info replication]]
            } else {
                fail "Replica did not finish sync"
            }
        }

        test {failoverto command fails without connected replica} {
            catch { $primary failoverto $replica_host $replica_port } err
            if {! [string match "ERR*" $err]} {
                fail "failoverto command succeeded when replica not connected"
            }
        }

        test {failoverto command fails with invalid host} {
            reset_replication $replica $replica_pid $primary $primary_host $primary_port
            catch { $primary failoverto invalidhost $replica_port } err
            if {! [string match "ERR*" $err]} {
                fail "failoverto command succeeded with invalid host"
            }
        }

        test {failoverto command fails with invalid port} {
            reset_replication $replica $replica_pid $primary $primary_host $primary_port
            catch { $primary failoverto $replica_host invalidport } err
            if {! [string match "ERR*" $err]} {
                fail "failoverto command succeeded with invalid port"
            }
        }

        test {failoverto command fails when sent to a replica} {
            reset_replication $replica $replica_pid $primary $primary_host $primary_port
            catch { $replica failoverto $replica_host $replica_port } err
            if {! [string match "ERR*" $err]} {
                fail "failoverto command succeeded when sent to replica"
            }
        }

        test {failoverto command to specific replica works} {
            reset_replication $replica $replica_pid $primary $primary_host $primary_port
            $primary failoverto $replica_host $replica_port

            # Wait for failover to end
            wait_for_condition 50 100 {
                [string match *master_failover_in_progress:0* [$primary info replication]]
            } else {
                fail "Failover from primary to replica did not occur"
            }

            assert_match *slave* [$primary role]
            assert_match *master* [$replica role]
        }

        test {failoverto command to any replica works} {
            reset_replication $replica $replica_pid $primary $primary_host $primary_port
            $primary failoverto any one

            # Wait for failover to end
            wait_for_condition 50 100 {
                [string match *master_failover_in_progress:0* [$primary info replication]]
            } else {
                fail "Failover from primary to replica did not occur"
            }

            assert_match *slave* [$primary role]
            assert_match *master* [$replica role]
        }

        test {failoverto aborts if replica never catches up} {
            reset_replication $replica $replica_pid $primary $primary_host $primary_port

            # Stop replica so it never catches up
            exec kill -SIGSTOP $replica_pid
            $primary SET CASE 1
            
            $primary failoverto any one TIMEOUT 500

            # Wait for failover to end
            wait_for_condition 50 10 {
                [string match *master_failover_in_progress:0* [$primary info replication]]
            } else {
                fail "Failover from primary to replica did not occur"
            }

            exec kill -SIGCONT $replica_pid
            assert_match *master* [$primary role]
            assert_match *slave* [$replica role]
        }

        test {failoverto aborts if replica never becomes primary up} {
            reset_replication $replica $replica_pid $primary $primary_host $primary_port

            $primary SET CASE 2

            # Wait for dataset to sync up
            wait_for_condition 50 100 {
                [string match [s 0 master_repl_offset] [s -1 master_repl_offset]]
            } else {
                fail "Failover from primary to replica did not occur"
            }

            # Stop the replica here, the primary will become a replica
            exec kill -SIGSTOP $replica_pid
            $primary failoverto any one TIMEOUT 500

            # Wait for primary to find the replica and sync up with it
            wait_for_condition 50 100 {
                [string match *master* [$primary role]]
            } else {
                fail "Failover from primary to replica did not occur"
            }

            # Wait for failover to end
            wait_for_condition 50 100 {
                [string match *master_failover_in_progress:0* [$primary info replication]]
            } else {
                fail "Failover from primary to replica did not occur"
            }

            exec kill -SIGCONT $replica_pid
            assert_match *master* [$primary role]
            assert_match *slave* [$replica role]
        }


    }
}

