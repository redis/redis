# Failover stress test.
# In this test a different node is killed in a loop for N
# iterations. The test checks that certain properties
# are preserved across iterations.

source "../tests/includes/init-tests.tcl"
source "../../../tests/support/cli.tcl"

test "Create a 5 nodes cluster" {
    create_cluster 5 5
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "Enable AOF in all the instances" {
    foreach_redis_id id {
        R $id config set appendonly yes
        # We use "appendfsync no" because it's fast but also guarantees that
        # write(2) is performed before replying to client.
        R $id config set appendfsync no
    }

    foreach_redis_id id {
        wait_for_condition 1000 500 {
            [RI $id aof_rewrite_in_progress] == 0 &&
            [RI $id aof_enabled] == 1
        } else {
            fail "Failed to enable AOF on instance #$id"
        }
    }
}

# Return non-zero if the specified PID is about a process still in execution,
# otherwise 0 is returned.
proc process_is_running {pid} {
    # PS should return with an error if PID is non existing,
    # and catch will return non-zero. We want to return non-zero if
    # the PID exists, so we invert the return value with expr not operator.
    expr {![catch {exec ps -p $pid}]}
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
set cluster [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 port]]
catch {unset content}
array set content {}
set tribpid {}

test "Cluster consistency during live resharding" {
    set ele 0
    for {set j 0} {$j < $numops} {incr j} {
        # Trigger the resharding once we execute half the ops.
        if {$tribpid ne {} &&
            ($j % 10000) == 0 &&
            ![process_is_running $tribpid]} {
            set tribpid {}
        }

        if {$j >= $numops/2 && $tribpid eq {}} {
            puts -nonewline "...Starting resharding..."
            flush stdout
            set target [dict get [get_myself [randomInt 5]] id]
            set tribpid [lindex [exec \
                ../../../src/redis-cli --cluster reshard \
                127.0.0.1:[get_instance_attrib redis 0 port] \
                --cluster-from all \
                --cluster-to $target \
                --cluster-slots 100 \
                --cluster-yes \
                {*}[rediscli_tls_config "../../../tests"] \
                | [info nameofexecutable] \
                ../tests/helpers/onlydots.tcl \
                &] 0]
        }

        # Write random data to random list.
        set listid [randomInt $numkeys]
        set key "key:$listid"
        incr ele
        # We write both with Lua scripts and with plain commands.
        # This way we are able to stress Lua -> Redis command invocation
        # as well, that has tests to prevent Lua to write into wrong
        # hash slots.
        if {$listid % 2} {
            $cluster rpush $key $ele
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
        [process_is_running $tribpid] == 0
    } else {
        fail "Resharding is not terminating after some time."
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

test "Crash and restart all the instances" {
    foreach_redis_id id {
        kill_instance redis $id
        restart_instance redis $id
    }
}

test "Cluster should eventually be up again" {
    assert_cluster_state ok
}

test "Verify $numkeys keys after the crash & restart" {
    # Check that the Redis Cluster content matches our logical content.
    foreach {key value} [array get content] {
        if {[$cluster lrange $key 0 -1] ne $value} {
            fail "Key $key expected to hold '$value' but actual content is [$cluster lrange $key 0 -1]"
        }
    }
}

test "Disable AOF in all the instances" {
    foreach_redis_id id {
        R $id config set appendonly no
    }
}

test "Verify slaves consistency" {
    set verified_masters 0
    foreach_redis_id id {
        set role [R $id role]
        lassign $role myrole myoffset slaves
        if {$myrole eq {slave}} continue
        set masterport [get_instance_attrib redis $id port]
        set masterdigest [R $id debug digest]
        foreach_redis_id sid {
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
