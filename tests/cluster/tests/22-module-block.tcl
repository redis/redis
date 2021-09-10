#Check no permanent blocking when a client is blocked by module commands and a resharding happens at the same time.

source "../tests/includes/init-tests.tcl"
source "../../../tests/support/cli.tcl"

set testmodule [file normalize ../../../tests/modules/blockonkeys.so]

test "Create a 5 nodes cluster" {
    create_cluster 5 5 
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test 0
}

test "Load module in all the instances" {

    foreach_redis_id id {
        R $id module load $testmodule
    }

}

set srcid -1
set blockpid -1
test "Exec module block cmd" {

    exec \
        ../../../src/redis-cli -c -h\
        127.0.0.1 -p [get_instance_attrib redis 0 port] \
	del k1 >>/dev/null

    # Exec module block command
    # k1 is mapped to slot 12706
    set blockpid [exec \
        ../../../src/redis-cli -c -h\
        127.0.0.1 -p [get_instance_attrib redis 0 port] \
	fsl.bpop k1 0 >>/dev/null & ]
}

test "Check block node before resharding" {

    after 500

    foreach_redis_id id {
	if { [RI $id blocked_clients] eq {1} } {
	    puts "Before the resharding, block in #$id"
	    set srcid $id
        }
    }
}

test "Perform a Resharding" {

    set dstid [expr ($srcid+1)%5]
    set dst [dict get [get_myself $dstid] id]

    puts "Move slot 12706 from #$srcid to #$dstid"

    R $dstid cluster bumpepoch
    R $dstid cluster setslot 12706 node $dst

}

test "Wait cluster to be stable" {

    wait_for_condition 1000 50 {
        [catch {exec ../../../src/redis-cli --cluster \
            check 127.0.0.1:[get_instance_attrib redis 0 port] \
            {*}[rediscli_tls_config "../../../tests"] \
            }] == 0
    } else {
        fail "Cluster doesn't stabilize"
    }
}

test "Check block node after resharding" {

    foreach_redis_id id {
	if { [RI $id blocked_clients] eq {1} } {
	    puts "After the resharding, block in #$id"
        } 
    }
}

test "Send push cmd" {

    # The cmd will be redirected to dstid.
    # If the client is also redirected to this id, 
    # it can be unblocked.

    exec \
        ../../../src/redis-cli -c -h\
        127.0.0.1 -p [get_instance_attrib redis 0 port] \
	fsl.push k1 1
}

test "Check no blocking clients" {

    foreach_redis_id id {
	if { [RI $id blocked_clients] ne {0} } {
	    fail "Still have blocking clients after resharding"
            exec kill -9 $blockpid     
        }
    }
}
