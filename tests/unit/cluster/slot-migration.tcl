proc get_open_slots {srv_idx} {
  foreach line [split [R $srv_idx cluster nodes] "\n"] {
    set line [string trim $line]
    if {$line eq {}} continue
    if {[regexp {myself} $line] == 0} continue
    set slots {}
    regexp {\[.*} $line slots
    return $slots
  }
}

proc get_cluster_role {srv_idx} {
  foreach line [split [R $srv_idx cluster nodes] "\n"] {
    set line [string trim $line]
    if {$line eq {}} continue
    if {[regexp {myself} $line] == 0} continue
    set role {}
    regexp {myself,(\w+)} $line -> role
    return $role
  }
}

proc wait_for_role {srv_idx role} {
  set node_timeout [lindex [R 0 config get cluster-node-timeout] 1]
  # wait for a gossip cycle for states to be propagated throughout the cluster
  after $node_timeout
  wait_for_condition 100 100 {
    [lindex [split [R $srv_idx role] " "] 0] eq $role
  } else {
    fail "R $srv_idx didn't assume the replication $role in time"
  }
  wait_for_condition 100 100 {
      [get_cluster_role $srv_idx] eq $role
  } else {
      fail "R $srv_idx didn't assume the cluster $role in time"
  }
}

proc wait_for_slot_state {srv_idx pattern} {
  wait_for_condition 100 100 {
    [get_open_slots $srv_idx] eq $pattern
  } else {
    fail "incorrect slot state on R $srv_idx"
  }
}

start_cluster 3 3 {tags {external:skip cluster}
                   overrides {cluster-allow-replica-migration no
                              cluster-node-timeout 1000} } {

set node_timeout [lindex [R 0 config get cluster-node-timeout] 1]
set R0_id [R 0 cluster myid]
set R1_id [R 1 cluster myid]
set R2_id [R 2 cluster myid]
set R3_id [R 3 cluster myid]
set R4_id [R 4 cluster myid]
set R5_id [R 5 cluster myid]

test "Slot migration states are replicated" {
  # Validate initial states
  assert_not_equal [get_open_slots 0] "\[609->-$R1_id\]"
  assert_not_equal [get_open_slots 1] "\[609-<-$R0_id\]"
  assert_not_equal [get_open_slots 3] "\[609->-$R1_id\]"
  assert_not_equal [get_open_slots 4] "\[609-<-$R0_id\]"
  # Kick off the migration of slot 609 from R0 to R1
  assert_equal {OK} [R 0 cluster setslot 609 migrating $R1_id]
  assert_equal {OK} [R 1 cluster setslot 609 importing $R0_id]
  # Validate that R0 is migrating slot 609 to R1
  assert_equal [get_open_slots 0] "\[609->-$R1_id\]"
  # Validate that R1 is importing slot 609 from R0 
  assert_equal [get_open_slots 1] "\[609-<-$R0_id\]"
  # Validate final states
  wait_for_slot_state 0 "\[609->-$R1_id\]"
  wait_for_slot_state 1 "\[609-<-$R0_id\]"
  wait_for_slot_state 3 "\[609->-$R1_id\]"
  wait_for_slot_state 4 "\[609-<-$R0_id\]"
}

test "Migration target is auto-updated after failover in target shard" {
  # Restart R1 to trigger an auto-failover to R4
  # Make sure wait for twice the node timeout time
  # to ensure the failover does occur
  catch {R 1 debug restart [expr 2*$node_timeout]} e
  catch {I/O error reading reply} $e
  # Wait for R1 to come back
  after [expr 3*$node_timeout]
  # Wait for R1 to become a replica
  wait_for_role 1 slave
  # Validate final states
  wait_for_slot_state 0 "\[609->-$R4_id\]"
  wait_for_slot_state 1 "\[609-<-$R0_id\]"
  wait_for_slot_state 3 "\[609->-$R4_id\]"
  wait_for_slot_state 4 "\[609-<-$R0_id\]"
  # Restore R1's primaryship
  assert_equal {OK} [R 1 cluster failover]
  wait_for_role 1 master
  # Validate initial states
  wait_for_slot_state 0 "\[609->-$R1_id\]"
  wait_for_slot_state 1 "\[609-<-$R0_id\]"
  wait_for_slot_state 3 "\[609->-$R1_id\]"
  wait_for_slot_state 4 "\[609-<-$R0_id\]"
}

test "Migration source is auto-updated after failover in source shard" {
  # Restart R0 to trigger an auto-failover to R3
  # Make sure wait for twice the node timeout time
  # to ensure the failover does occur
  catch {R 0 debug restart [expr 2*$node_timeout]} e
  catch {I/O error reading reply} $e
  # Wait for R0 to come back
  after [expr 3*$node_timeout]
  # Wait for R0 to become a replica
  wait_for_role 0 slave
  # Validate final states
  wait_for_slot_state 0 "\[609->-$R1_id\]"
  wait_for_slot_state 1 "\[609-<-$R3_id\]"
  wait_for_slot_state 3 "\[609->-$R1_id\]"
  wait_for_slot_state 4 "\[609-<-$R3_id\]"
  # Restore R0's primaryship
  assert_equal {OK} [R 0 cluster failover]
  wait_for_role 0 master
  # Validate final states
  wait_for_slot_state 0 "\[609->-$R1_id\]"
  wait_for_slot_state 1 "\[609-<-$R0_id\]"
  wait_for_slot_state 3 "\[609->-$R1_id\]"
  wait_for_slot_state 4 "\[609-<-$R0_id\]"
}

test "Replica redirects key access in migrating slots" {
  # Validate initial states
  assert_equal [get_open_slots 0] "\[609->-$R1_id\]"
  assert_equal [get_open_slots 1] "\[609-<-$R0_id\]"
  assert_equal [get_open_slots 3] "\[609->-$R1_id\]"
  assert_equal [get_open_slots 4] "\[609-<-$R0_id\]"
  catch {[R 3 GET aga]} e
  assert_equal {ASK} [lindex [split $e] 0]
  assert_equal {609} [lindex [split $e] 1]
}

test "New replica inherits migrating slot" {
  # Reset R3 to turn it into an empty node
  assert_equal [get_open_slots 3] "\[609->-$R1_id\]"
  assert_equal {OK} [R 3 cluster reset]
  assert_not_equal [get_open_slots 3] "\[609->-$R1_id\]"
  # Add R3 back as a replica of R0
  assert_equal {OK} [R 3 cluster meet [srv 0 "host"] [srv 0 "port"]]
  after $node_timeout
  assert_equal {OK} [R 3 cluster replicate $R0_id]
  wait_for_role 3 slave
  # Validate that R3 now sees slot 609 open
  assert_equal [get_open_slots 3] "\[609->-$R1_id\]"
}

test "New replica inherits importing slot" {
  # Reset R4 to turn it into an empty node
  assert_equal [get_open_slots 4] "\[609-<-$R0_id\]"
  assert_equal {OK} [R 4 cluster reset]
  assert_not_equal [get_open_slots 4] "\[609-<-$R0_id\]"
  # Add R4 back as a replica of R1
  assert_equal {OK} [R 4 cluster meet [srv -1 "host"] [srv -1 "port"]]
  after $node_timeout
  assert_equal {OK} [R 4 cluster replicate $R1_id]
  wait_for_role 4 slave
  # Validate that R4 now sees slot 609 open
  assert_equal [get_open_slots 4] "\[609-<-$R0_id\]"
}
}

proc create_empty_shard {p r} {
  set node_timeout [lindex [R 0 config get cluster-node-timeout] 1]
  assert_equal {OK} [R $p cluster reset]
  assert_equal {OK} [R $r cluster reset]
  assert_equal {OK} [R $p cluster meet [srv 0 "host"] [srv 0 "port"]]
  assert_equal {OK} [R $r cluster meet [srv 0 "host"] [srv 0 "port"]]
  after $node_timeout
  assert_equal {OK} [R $r cluster replicate [R $p cluster myid]]
  wait_for_role $r slave
  wait_for_role $p master
}

start_cluster 3 5 {tags {external:skip cluster}
                   overrides {cluster-allow-replica-migration no
                              cluster-node-timeout 1000} } {

set node_timeout [lindex [R 0 config get cluster-node-timeout] 1]
set R0_id [R 0 cluster myid]
set R1_id [R 1 cluster myid]
set R2_id [R 2 cluster myid]
set R3_id [R 3 cluster myid]
set R4_id [R 4 cluster myid]
set R5_id [R 5 cluster myid]

create_empty_shard 6 7
set R6_id [R 6 cluster myid]
set R7_id [R 7 cluster myid]

test "Empty-shard migration replicates slot importing states" {
  # Validate initial states
  assert_not_equal [get_open_slots 0] "\[609->-$R6_id\]"
  assert_not_equal [get_open_slots 6] "\[609-<-$R0_id\]"
  assert_not_equal [get_open_slots 3] "\[609->-$R6_id\]"
  assert_not_equal [get_open_slots 7] "\[609-<-$R0_id\]"
  # Kick off the migration of slot 609 from R0 to R6
  assert_equal {OK} [R 0 cluster setslot 609 migrating $R6_id]
  assert_equal {OK} [R 6 cluster setslot 609 importing $R0_id]
  # Validate that R0 is migrating slot 609 to R6
  assert_equal [get_open_slots 0] "\[609->-$R6_id\]"
  # Validate that R6 is importing slot 609 from R0 
  assert_equal [get_open_slots 6] "\[609-<-$R0_id\]"
  # Validate final states
  wait_for_slot_state 0 "\[609->-$R6_id\]"
  wait_for_slot_state 6 "\[609-<-$R0_id\]"
  wait_for_slot_state 3 "\[609->-$R6_id\]"
  wait_for_slot_state 7 "\[609-<-$R0_id\]"
}

test "Empty-shard migration target is auto-updated after faiover in target shard" {
  wait_for_role 6 master
  # Restart R6 to trigger an auto-failover to R7
  catch {R 6 debug restart [expr 3*$node_timeout]} e
  catch {I/O error reading reply} $e
  # Wait for R6 to come back
  after [expr 3*$node_timeout]
  # Wait for R6 to become a replica
  wait_for_role 6 slave
  # Validate final states
  wait_for_slot_state 0 "\[609->-$R7_id\]"
  wait_for_slot_state 6 "\[609-<-$R0_id\]"
  wait_for_slot_state 3 "\[609->-$R7_id\]"
  wait_for_slot_state 7 "\[609-<-$R0_id\]"
  # Restore R6's primaryship
  assert_equal {OK} [R 6 cluster failover]
  wait_for_role 6 master
  # Validate final states
  wait_for_slot_state 0 "\[609->-$R6_id\]"
  wait_for_slot_state 6 "\[609-<-$R0_id\]"
  wait_for_slot_state 3 "\[609->-$R6_id\]"
  wait_for_slot_state 7 "\[609-<-$R0_id\]"
}

test "Empty-shard migration source is auto-updated after source faiover in source shard" {
  wait_for_role 0 master
  # Restart R0 to trigger an auto-failover to R3
  catch {R 0 debug restart [expr 2*$node_timeout]} e
  catch {I/O error reading reply} $e
  # Wait for R0 to come back
  after [expr 3*$node_timeout]
  # Wait for R7 to become a replica
  wait_for_role 0 slave
  # Validate final states
  wait_for_slot_state 0 "\[609->-$R6_id\]"
  wait_for_slot_state 6 "\[609-<-$R3_id\]"
  wait_for_slot_state 3 "\[609->-$R6_id\]"
  wait_for_slot_state 7 "\[609-<-$R3_id\]"
  # Restore R0's primaryship
  assert_equal {OK} [R 0 cluster failover]
  wait_for_role 0 master
  # Validate final states
  wait_for_slot_state 0 "\[609->-$R6_id\]"
  wait_for_slot_state 6 "\[609-<-$R0_id\]"
  wait_for_slot_state 3 "\[609->-$R6_id\]"
  wait_for_slot_state 7 "\[609-<-$R0_id\]"
}
}

proc migrate_slot {from to slot} {
  set from_id [R $from cluster myid]
  set to_id [R $to cluster myid]
  assert_equal {OK} [R $from cluster setslot $slot migrating $to_id]
  assert_equal {OK} [R $to cluster setslot $slot importing $from_id]
}

start_cluster 3 3 {tags {external:skip cluster}
                   overrides {cluster-allow-replica-migration no
                              cluster-node-timeout 1000} } {

set node_timeout [lindex [R 0 config get cluster-node-timeout] 1]
set R0_id [R 0 cluster myid]
set R1_id [R 1 cluster myid]
set R2_id [R 2 cluster myid]
set R3_id [R 3 cluster myid]
set R4_id [R 4 cluster myid]
set R5_id [R 5 cluster myid]

test "Multiple slot migration states are replicated" {
  migrate_slot 0 1 13
  migrate_slot 0 1 7
  migrate_slot 0 1 17
  # Validate final states
  wait_for_slot_state 0 "\[7->-$R1_id\] \[13->-$R1_id\] \[17->-$R1_id\]"
  wait_for_slot_state 1 "\[7-<-$R0_id\] \[13-<-$R0_id\] \[17-<-$R0_id\]"
  wait_for_slot_state 3 "\[7->-$R1_id\] \[13->-$R1_id\] \[17->-$R1_id\]"
  wait_for_slot_state 4 "\[7-<-$R0_id\] \[13-<-$R0_id\] \[17-<-$R0_id\]"
}

test "New replica inherits multiple migrating slots" {
  # Reset R3 to turn it into an empty node
  assert_equal {OK} [R 3 cluster reset]
  # Add R3 back as a replica of R0
  assert_equal {OK} [R 3 cluster meet [srv 0 "host"] [srv 0 "port"]]
  after $node_timeout
  assert_equal {OK} [R 3 cluster replicate $R0_id]
  wait_for_role 3 slave
  # Validate final states
  wait_for_slot_state 3 "\[7->-$R1_id\] \[13->-$R1_id\] \[17->-$R1_id\]"
}

test "Slot finalization on replicas" {
  # Trigger slot finalization on replicas
  assert_equal {OK} [R 1 cluster setslot 7 node $R1_id replicaonly]
  assert_equal {1} [R 1 wait 1 1000]
  wait_for_slot_state 1 "\[7-<-$R0_id\] \[13-<-$R0_id\] \[17-<-$R0_id\]"
  wait_for_slot_state 4 "\[13-<-$R0_id\] \[17-<-$R0_id\]"
  assert_equal {OK} [R 1 cluster setslot 13 node $R1_id replicaonly]
  assert_equal {1} [R 1 wait 1 1000]
  wait_for_slot_state 1 "\[7-<-$R0_id\] \[13-<-$R0_id\] \[17-<-$R0_id\]"
  wait_for_slot_state 4 "\[17-<-$R0_id\]"
  assert_equal {OK} [R 1 cluster setslot 17 node $R1_id replicaonly]
  assert_equal {1} [R 1 wait 1 1000]
  wait_for_slot_state 1 "\[7-<-$R0_id\] \[13-<-$R0_id\] \[17-<-$R0_id\]"
  wait_for_slot_state 4 ""
}

test "Finalizing incorrect slot" {
  catch {R 1 cluster setslot 123 node $R1_id replicaonly} e
  assert_equal {ERR Slot is not open for importing} $e
}

test "Slot migration without target replicas" {
  migrate_slot 0 1 100
  # Move the target replica away
  assert_equal {OK} [R 4 cluster replicate $R0_id]
  after $node_timeout
  # Slot finalization should fail
  catch {R 1 cluster setslot 100 node $R1_id replicaonly} e
  assert_equal {ERR Target node has no replicas} $e
}
}
