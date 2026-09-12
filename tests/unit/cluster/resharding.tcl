# Stress writes while redis-cli performs a live reshard, then verify AOF
# persistence and replica consistency across process restarts.

start_cluster 5 5 {tags {external:skip cluster slow valgrind:skip} overrides {appendonly yes appendfsync no}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Enable AOF in all the instances" {
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        R $id config set appendonly yes
        # This is fast while still ensuring write(2) happens before the reply.
        R $id config set appendfsync no
    }

    for {set id 0} {$id < [llength $::servers]} {incr id} {
        wait_for_condition 1000 500 {
            [s -$id aof_rewrite_in_progress] == 0 &&
            [s -$id aof_enabled] == 1
        } else {
            fail "Failed to enable AOF on instance #$id"
        }
    }
}

# The test sends N commands to random keys and starts a background reshard
# halfway through. Tcl records every list value so the logical and physical
# contents can be compared after the reshard and again after process restarts.
set numkeys 50000
set numops 200000
set start_node_port [srv 0 port]
set cluster [redis_cluster 127.0.0.1:$start_node_port]
if {$::tls} {
    # Exercise both TLS and plaintext connections to the TLS cluster.
    set plaintext_port [srv 0 pport]
    set cluster_plaintext [redis_cluster 127.0.0.1:$plaintext_port 0]
    puts "Testing TLS cluster on start node 127.0.0.1:$start_node_port, plaintext port $plaintext_port"
} else {
    set cluster_plaintext $cluster
    puts "Testing using non-TLS cluster"
}
catch {unset content}
array set content {}
set reshard_pid {}
set reshard_output [tmpfile redis-cli-reshard.log]

test "Cluster consistency during live resharding" {
    set ele 0
    for {set j 0} {$j < $numops} {incr j} {
        if {$reshard_pid ne {} &&
            ($j % 10000) == 0 &&
            ![is_alive $reshard_pid]} {
            set reshard_pid {}
        }

        if {$j >= $numops/2 && $reshard_pid eq {}} {
            puts -nonewline "...Starting resharding..."
            flush stdout
            set target [dict get [cluster_get_myself [randomInt 5]] id]
            set reshard_pid [lindex [exec \
                src/redis-cli --cluster reshard \
                127.0.0.1:[srv 0 port] \
                --cluster-from all \
                --cluster-to $target \
                --cluster-slots 100 \
                --cluster-yes \
                {*}[rediscli_tls_config "./tests"] \
                > $reshard_output 2>@1 &] 0]
        }

        set listid [randomInt $numkeys]
        set key "key:$listid"
        incr ele
        # Cover direct, plaintext, and Lua-originated writes.
        if {$listid % 3 == 0} {
            $cluster rpush $key $ele
        } elseif {$listid % 3 == 1} {
            $cluster_plaintext rpush $key $ele
        } else {
            $cluster eval {redis.call("rpush",KEYS[1],ARGV[1])} 1 $key $ele
        }
        lappend content($key) $ele

        if {($j % 1000) == 0} {
            puts -nonewline W
            flush stdout
        }
    }

    wait_for_condition 1000 500 {
        ![is_alive $reshard_pid]
    } else {
        set fd [open $reshard_output r]
        set output [read $fd]
        close $fd
        fail "Resharding did not terminate. redis-cli output:\n$output"
    }
}

test "Verify $numkeys keys for consistency with logical content" {
    foreach {key value} [array get content] {
        if {[$cluster lrange $key 0 -1] ne $value} {
            fail "Key $key expected to hold '$value' but actual content is [$cluster lrange $key 0 -1]"
        }
    }
}

test "Terminate and restart all the instances" {
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        # Stop AOF so an initial AOFRW cannot delay process termination. The
        # configured value is still yes, so the restarted server loads AOF.
        R $id config set appendonly no
        cluster_kill_node $id
        cluster_restart_node $id
    }
}

test "Cluster should eventually be up again" {
    wait_for_cluster_state ok
}

test "Verify $numkeys keys after the restart" {
    foreach {key value} [array get content] {
        if {[$cluster lrange $key 0 -1] ne $value} {
            fail "Key $key expected to hold '$value' but actual content is [$cluster lrange $key 0 -1]"
        }
    }
}

test "Disable AOF in all the instances" {
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        R $id config set appendonly no
    }
}

test "Verify replicas consistency" {
    set verified_masters 0
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        set role [R $id role]
        lassign $role myrole myoffset replicas
        if {$myrole eq {slave}} continue
        set masterport [srv -$id port]
        set masterdigest [R $id debug digest]
        for {set replica_id 0} {$replica_id < [llength $::servers]} {incr replica_id} {
            set replica_role [R $replica_id role]
            if {[lindex $replica_role 0] eq {master}} continue
            if {[lindex $replica_role 2] != $masterport} continue
            wait_for_condition 1000 500 {
                [R $replica_id debug digest] eq $masterdigest
            } else {
                fail "Master and replica data digests are different"
            }
            incr verified_masters
        }
    }
    assert {$verified_masters >= 5}
}

test "Dump sanitization was skipped for migrations" {
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        assert {[s -$id dump_payload_sanitizations] == 0}
    }
}

if {$cluster_plaintext ne $cluster} {
    $cluster_plaintext close
}
$cluster close

} ;# start_cluster
