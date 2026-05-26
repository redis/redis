# Test that redis-cli --cluster rebalance doesn't fail with CROSSSLOT
# when --user is specified without -a (no password).

source tests/support/cli.tcl

# make sure the test infra won't use SELECT
set old_singledb $::singledb
set ::singledb 1

proc write_keys_to_master0 {} {
    set client [srv 0 client]
    set slot_ranges {}
    foreach line [split [$client CLUSTER NODES] \n] {
        if {[string match "*myself*" $line]} {
            set fields [split $line " "]
            set slot_ranges [lrange $fields 8 end]
            break
        }
    }
    set count 0
    foreach range $slot_ranges {
        set parts [split $range "-"]
        set start [lindex $parts 0]
        set end [lindex $parts 1]
        if {$end eq ""} {set end $start}
        for {set s $start} {$s <= $end} {incr s} {
            exec src/redis-cli -c -p [srv 0 port] SET "key:$s:a" "value:$s"
            exec src/redis-cli -c -p [srv 0 port] SET "key:$s:b" "value:$s"
            incr count 2
            if {$count >= 100} break
        }
        if {$count >= 100} break
    }
}

tags {tls:skip external:skip cluster} {

set base_conf [list cluster-enabled yes cluster-node-timeout 1000]

# Test 1: rebalance without --user and without -a
start_multiple_servers 6 [list overrides $base_conf] {

    set node0 [srv 0 client]

    test {Create a 3 node cluster with replicas} {
        exec src/redis-cli --cluster-yes --cluster create \
                           127.0.0.1:[srv 0 port] \
                           127.0.0.1:[srv -1 port] \
                           127.0.0.1:[srv -2 port] \
                           127.0.0.1:[srv -3 port] \
                           127.0.0.1:[srv -4 port] \
                           127.0.0.1:[srv -5 port] \
                           --cluster-replicas 1

        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok} &&
            [CI 3 cluster_state] eq {ok} &&
            [CI 4 cluster_state] eq {ok} &&
            [CI 5 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }
    }

    test {Write keys to master 0 slots} {
        write_keys_to_master0
    }

    test {Rebalance without --user and without -a should succeed} {
        set master0_id [$node0 CLUSTER MYID]
        catch {
            exec src/redis-cli --cluster-yes --cluster rebalance \
                               127.0.0.1:[srv 0 port] \
                               --cluster-weight ${master0_id}=0 \
                               --cluster-timeout 10000
        } e
        assert_no_match "*CROSSSLOT*" $e
        assert_no_match "*CROSS*SLOT*" $e
    }

} ;# stop servers

# Test 2: rebalance with --user but no -a (the bug case)
start_multiple_servers 6 [list overrides $base_conf] {

    set node0 [srv 0 client]

    test {Create a 3 node cluster with replicas} {
        exec src/redis-cli --cluster-yes --cluster create \
                           127.0.0.1:[srv 0 port] \
                           127.0.0.1:[srv -1 port] \
                           127.0.0.1:[srv -2 port] \
                           127.0.0.1:[srv -3 port] \
                           127.0.0.1:[srv -4 port] \
                           127.0.0.1:[srv -5 port] \
                           --cluster-replicas 1

        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok} &&
            [CI 3 cluster_state] eq {ok} &&
            [CI 4 cluster_state] eq {ok} &&
            [CI 5 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }
    }

    test {Set up ACL user for testing} {
        foreach client [list $node0 [srv -1 client] [srv -2 client] \
                             [srv -3 client] [srv -4 client] [srv -5 client]] {
            $client ACL SETUSER testuser on nopass +@all ~*
        }
    }

    test {Write keys to master 0 slots} {
        write_keys_to_master0
    }

    test {Rebalance with --user but no -a should not CROSSSLOT} {
        # This used to fail with CROSSSLOT because the empty string
        # argument in MIGRATE was treated as a key with slot 0
        set master0_id [$node0 CLUSTER MYID]
        catch {
            exec src/redis-cli --cluster-yes --cluster rebalance \
                               127.0.0.1:[srv 0 port] \
                               --user testuser \
                               --cluster-weight ${master0_id}=0 \
                               --cluster-timeout 10000
        } e
        assert_no_match "*CROSSSLOT*" $e
        assert_no_match "*CROSS*SLOT*" $e
    }

} ;# stop servers

# Test 3: rebalance with --user and -a
start_multiple_servers 6 [list overrides $base_conf] {

    set node0 [srv 0 client]

    test {Create a 3 node cluster with replicas} {
        exec src/redis-cli --cluster-yes --cluster create \
                           127.0.0.1:[srv 0 port] \
                           127.0.0.1:[srv -1 port] \
                           127.0.0.1:[srv -2 port] \
                           127.0.0.1:[srv -3 port] \
                           127.0.0.1:[srv -4 port] \
                           127.0.0.1:[srv -5 port] \
                           --cluster-replicas 1

        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok} &&
            [CI 3 cluster_state] eq {ok} &&
            [CI 4 cluster_state] eq {ok} &&
            [CI 5 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }
    }

    test {Set up ACL user for testing} {
        foreach client [list $node0 [srv -1 client] [srv -2 client] \
                             [srv -3 client] [srv -4 client] [srv -5 client]] {
            $client ACL SETUSER testuser2 on >testpass +@all ~*
        }
    }

    test {Write keys to master 0 slots} {
        write_keys_to_master0
    }

    test {Rebalance with --user and -a should succeed} {
        set master0_id [$node0 CLUSTER MYID]
        catch {
            exec src/redis-cli --cluster-yes --cluster rebalance \
                               127.0.0.1:[srv 0 port] \
                               --user testuser2 \
                               -a testpass \
                               --no-auth-warning \
                               --cluster-weight ${master0_id}=0 \
                               --cluster-timeout 10000
        } e
        assert_no_match "*CROSSSLOT*" $e
        assert_no_match "*CROSS*SLOT*" $e
    }

} ;# stop servers

} ;# tags

set ::singledb $old_singledb
