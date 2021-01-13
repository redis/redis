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
            # If a previous test failed, we need to wait for any ongoing failover to finish
            wait_for_condition 50 100 {
                [string match *master_failover_in_progress:0* [$primary info replication]]
            } else {
                fail "Existing failover did not finish"
            }

            # In case we stopped the replica
            exec kill -SIGCONT $replica_pid

            # Start the replication process and add some data
            $primary replicaof no one
            $primary flushall
            $primary set FOO BAR
            $replica replicaof $primary_host $primary_port

            # Wait for the replica to think we're up
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
                fail "Failover from primary to replica did not finish"
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
                fail "Failover from primary to replica did not finish"
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
            wait_for_condition 50 20 {
                [s 0 master_failover_in_progress] == 0
            } else {
                fail "Failover from primary to replica did not finish"
            }

            exec kill -SIGCONT $replica_pid
            assert_match *master* [$primary role]
            assert_match *slave* [$replica role]
        }

        test {failoverto aborts if primary can't sync with new replica} {
            reset_replication $replica $replica_pid $primary $primary_host $primary_port

            
            $primary SET CASE 2
            # We puase the primary to stop any offset accumulating before
            # start the failover. We need to make sure the two nodes are
            # caught and nothing is replicated in between commands. 
            $primary client pause 1000 WRITE
            set offset [s 0 master_repl_offset]

            wait_for_condition 50 20 {
                [string match *state=online,offset=$offset* [$primary info replication]]
            } else {
                fail "Primary never attempted to hand off to a replica"
            }

            # Stop the replica here, the two nodes have the same offset
            # so the primary should demote itself and send the request here.
            exec kill -SIGSTOP $replica_pid
            
            $primary failoverto any one TIMEOUT 500
            
            # Wait for primary to find the replica and start handoff
            wait_for_condition 50 20 {
                [string match *slave* [$primary role]]
            } else {
                fail "Primary never attempted to hand off to a replica"
            }

            # Wait for failover to end, we add 5 seconds here
            wait_for_condition 50 100 {
                [s 0 master_failover_in_progress] == 0
            } else {
                fail "Failover from primary to replica did not finish"
            }
            exec kill -SIGCONT $replica_pid
            assert_match *master* [$primary role]
        }
    }
}

