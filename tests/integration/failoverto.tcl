start_server {tags {"failoverto"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    start_server {} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        test {failoverto command fails without connected replica } {
            catch { $primary failoverto $replica_host $replica_port } err
            if {! [string match "ERR*" $err]} {
                fail "failoverto command succeeded when replica not connected"
            }
        }
    }
}

start_server {tags {"failoverto"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    start_server {} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        # Start the replication process...
        $replica replicaof $primary_host $primary_port

        wait_for_condition 50 100 {
            [string match *state=online* [$primary info replication]]
        } else {
            fail "Replica did not finish sync"
        }

        test {failoverto command fails with invalid host } {
            catch { $primary failoverto invalidhost $replica_port } err
            if {! [string match "ERR*" $err]} {
                fail "failoverto command succeeded with invalid host"
            }
        }

        test {failoverto command fails with invalid port } {
            catch { $primary failoverto $replica_host invalidport } err
            if {! [string match "ERR*" $err]} {
                fail "failoverto command succeeded with invalid port"
            }
        }

        test {failoverto command fails when sent to a replica } {
            catch { $replica failoverto $replica_host $replica_port } err
            if {! [string match "ERR*" $err]} {
                fail "failoverto command succeeded when sent to replica"
            }
        }

        test {failoverto command to specific replica works} {
            $primary failoverto $replica_host $replica_port
            wait_for_condition 50 100 {
                [string match *slave* [$primary role]]
            } else {
                fail "Failover from primary to replica did not occur"
            }
        }
    }
}

start_server {tags {"failoverto"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    start_server {} {
        set primary [srv 0 client]
        set primary_host [srv 0 host]
        set primary_port [srv 0 port]

        # Start the replication process...
        $replica replicaof $primary_host $primary_port

        wait_for_condition 50 100 {
            [string match *state=online* [$primary info replication]]
        } else {
            fail "Replica did not finish sync"
        }

        test {failoverto command to any replica works} {
            $primary failoverto any one
            wait_for_condition 50 100 {
                [string match *slave* [$primary role]]
            } else {
                fail "Failover from primary to replica did not occur"
            }
        }
    }
}

