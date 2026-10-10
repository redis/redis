# Check cluster info stats

start_cluster 2 0 {tags {external:skip cluster}} {

set primary1 [srv 0 "client"]
set primary2 [srv -1 "client"]

proc cmdstat {instance cmd} {
    return [cmdrstat $cmd $instance]
}

proc errorstat {instance cmd} {
    return [errorrstat $cmd $instance]
}

test "cluster bus byte counters grow after a PING/PONG exchange" {
    foreach instance {0 1} {
        wait_for_condition 100 100 {
            [CI $instance cluster_stats_bytes_sent] > 0 &&
            [CI $instance cluster_stats_bytes_received] > 0
        } else {
            fail "cluster bus byte counters did not become positive on node $instance"
        }
    }

    wait_for_condition 100 100 {
        [CI 0 cluster_stats_messages_ping_sent] ne {}
    } else {
        fail "node 0 did not send a cluster PING"
    }

    set ping_before [CI 0 cluster_stats_messages_ping_sent]
    set sent0_before [CI 0 cluster_stats_bytes_sent]
    set received0_before [CI 0 cluster_stats_bytes_received]
    set sent1_before [CI 1 cluster_stats_bytes_sent]
    set received1_before [CI 1 cluster_stats_bytes_received]

    wait_for_condition 100 100 {
        [CI 0 cluster_stats_messages_ping_sent] > $ping_before &&
        [CI 0 cluster_stats_bytes_sent] > $sent0_before &&
        [CI 0 cluster_stats_bytes_received] > $received0_before &&
        [CI 1 cluster_stats_bytes_sent] > $sent1_before &&
        [CI 1 cluster_stats_bytes_received] > $received1_before
    } else {
        fail "cluster bus byte counters did not grow after a PING/PONG exchange"
    }
}

test "errorstats: rejected call due to MOVED Redirection" {
    $primary1 config resetstat
    $primary2 config resetstat
    assert_match {} [errorstat $primary1 MOVED]
    assert_match {} [errorstat $primary2 MOVED]
    # we know that one will have a MOVED reply and one will succeed
    catch {$primary1 set key b} replyP1
    catch {$primary2 set key b} replyP2
    # sort servers so we know which one failed
    if {$replyP1 eq {OK}} {
        assert_match {MOVED*} $replyP2
        set pok $primary1
        set perr $primary2
    } else {
        assert_match {MOVED*} $replyP1
        set pok $primary2
        set perr $primary1
    }
    assert_match {} [errorstat $pok MOVED]
    assert_match {*count=1*} [errorstat $perr MOVED]
    assert_match {*calls=0,*,rejected_calls=1,failed_calls=0*} [cmdstat $perr set]
}

} ;# start_cluster
