# Failover stress test.
# In this test a different node is killed in a loop for N
# iterations. The test checks that certain properties
# are preserved across iterations.

start_cluster 5 5 {tags {external:skip cluster slow valgrind:skip} overrides {appendonly yes appendfsync no}} {

test "Enable AOF in all the instances" {
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        R $id config set appendonly yes
        # We use "appendfsync no" because it's fast but also guarantees that
        # write(2) is performed before replying to client.
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

# Our resharding test performs the following actions:
#
# - N commands are sent to the cluster in the course of the test.
# - Every command selects a random key from key:0 to key:MAX-1.
# - The operation RPUSH key <randomvalue> is performed.
# - Tcl remembers into an array all the values pushed to each list.
# - After N/2 commands, the resharding process is started in background.
# - The test continues while the resharding is in progress.
# - At the end of the test, we wait for the resharding process to stop.
# - Finally the keys are checked to see if they contain the value they should.
set numkeys 50000
set numops 200000
set start_node_port [srv 0 port]
set cluster [redis_cluster 127.0.0.1:$start_node_port]
if {$::tls} {
    # setup a non-TLS cluster client to the TLS cluster
    set plaintext_port [srv 0 pport]
    set cluster_plaintext [redis_cluster 127.0.0.1:$plaintext_port 0]
    puts "Testing TLS cluster on start node 127.0.0.1:$start_node_port, plaintext port $plaintext_port"
} else {
    set cluster_plaintext $cluster
    puts "Testing using non-TLS cluster"
}
catch {unset content}
array set content {}
set tribpid {}
set reshard_output [tmpfile redis-cli-reshard.log]

test "Cluster consistency during live resharding" {
    set ele 0
    for {set j 0} {$j < $numops} {incr j} {
        # Trigger the resharding once we execute half the ops.
        if {$tribpid ne {} &&
            ($j % 10000) == 0 &&
            ![is_alive $tribpid]} {
            set tribpid {}
        }

        if {$j >= $numops/2 && $tribpid eq {}} {
            puts -nonewline "...Starting resharding..."
            flush stdout
            set target [dict get [cluster_get_myself [randomInt 5]] id]
            set tribpid [lindex [exec \
                src/redis-cli --cluster reshard \
                127.0.0.1:[srv 0 port] \
                --cluster-from all \
                --cluster-to $target \
                --cluster-slots 100 \
                --cluster-yes \
                {*}[rediscli_tls_config "./tests"] \
                > $reshard_output 2>@1 &] 0]
        }

        # Write random data to random list.
        set listid [randomInt $numkeys]
        set key "key:$listid"
        incr ele
        # We write both with Lua scripts and with plain commands.
        # This way we are able to stress Lua -> Redis command invocation
        # as well, that has tests to prevent Lua to write into wrong
        # hash slots.
        # We also use both TLS and plaintext connections.
        if {$listid % 3 == 0} {
            $cluster rpush $key $ele
        } elseif {$listid % 3 == 1} {
            $cluster_plaintext rpush $key $ele
        } else {
            $cluster eval {redis.call("rpush",KEYS[1],ARGV[1])} 1 $key $ele
        }
        lappend content($key) $ele

        if {($j % 1000) == 0} {
            puts -nonewline W; flush stdout
        }
    }

    # Wait for the resharding process to end
    wait_for_condition 1000 500 {
        ![is_alive $tribpid]
    } else {
        set fd [open $reshard_output r]
        set output [read $fd]
        close $fd
        fail "Resharding did not terminate. redis-cli output:\n$output"
    }
}

test "Verify $numkeys keys for consistency with logical content" {
    # Check that the Redis Cluster content matches our logical content.
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
    # Check that the Redis Cluster content matches our logical content.
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

test "Verify slaves consistency" {
    set verified_masters 0
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        set role [R $id role]
        lassign $role myrole myoffset slaves
        if {$myrole eq {slave}} continue
        set masterport [srv -$id port]
        set masterdigest [R $id debug digest]
        for {set sid 0} {$sid < [llength $::servers]} {incr sid} {
            set srole [R $sid role]
            if {[lindex $srole 0] eq {master}} continue
            if {[lindex $srole 2] != $masterport} continue
            wait_for_condition 1000 500 {
                [R $sid debug digest] eq $masterdigest
            } else {
                fail "Master and slave data digest are different"
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
