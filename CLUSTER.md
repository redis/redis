# Redis Cluster v2 Proposal

## Table of Contents

<!-- toc -->
- [Summary](#summary)
- [Motivation](#motivation)
- [Design](#design)
    - [Terminology](#terminology)
    - [High Level Overview](#high-level-overview)
    - [The Topology Director](#the-topology-director)
      - [Interfaces](#interfaces)
    - [The Failover Coordinator](#the-failover-coordinator)
        - [Interfaces](#interfaces)
    - [The Node](#the-node)
        - [Interfaces](#interfaces)
- [Flows](#flows)
    - [Membership and role management](#membership-and-role-management)
        - [Cluster Creation](#cluster-creation)
        - [Join](#join)
        - [Remove Node](#remove-node)
        - [Migrate A Shard between Failover Coordinators](#migrate-a-shard-between-failover-coordinators)
    - [Failover](#failover)
        - [Failover Coordinator Tick](#failover-coordinator-tick)
    - [Resharding](#resharding)
- [Flotilla Implementation Details](#flotilla-implementation-details)
    - [Roles](#roles)
    - [Node and Failover Coordinator IDs](#node-and-failover-coordinator-ids)
    - [Publish/Subscribe](#publish/subscribe) 
    - [Global Configuration](#global-configuration)
    - [Reference Implementation](#reference-implementation)
        - [Road to Production](#road-to-production) 
        - [Potential Enhancements](#potential-enhancements)
<!-- /toc -->

## Summary

A new approach to Redis Clustering is proposed in order to:

- Provide predictable behavior & correctness guarantees across the various possible failure modes
- Create a minimal, well-defined internal interface to the clustering code so alternative approaches can easily replace the
  entire clustering functionality at compile time
- Support a significantly higher number of shards
- Significantly reduce failover time to under 10 seconds (at worse) and under 5 seconds in some configurations
- Allow for alternative consistency mode of operation which can provide strong consistency guarantees for user data 
- Long term: Enable deprecating and eventually removing all Sentinel code by supporting an unsharded (One primary node 
  holding all the slots) deployment mode with no cross-slot limitations
- Long term: Enable centralized management for global configuration settings & Redis Functions

This new approach is:
- Based on a two-tier strongly consistent, consensus-based control plane
- Maintains full backwards compatibility with data path Cluster commands (MOVED, NODES, SLOTS, ...), but not 
  administrative commands (SETSLOT, FORGET, MEET,...)
- Has a consistent source of truth for all cluster metadata
- Requires the shards to reconcile their state according to the above source
- Maintains the ability to scale down to a bare minimum 3 redis-server processes
- Is called Flotilla :)

And is intended to fully replace the current clustering implementation after a deprecation period in which the two 
implementations will live side-by-side with compile time flag to chose between the two.

## Motivation

The current Redis Cluster is based on Gossip with the use of epochs to attempt to 
control/reduce the impact of concurrent changes. It is easy to set up but has a lot of issues when performing day 2
operations & scaling.

For example:
- Reliance on primaries (that may fail-over and move around) for quorum makes it impossible to guarantee availability using rack awareness.
- Node removal has an inherent race condition which can lead to nodes being re-added
- No single source of truth about who the primary is for a given slot range

Our main goal with this new approach is to provide a more robust solution, with an easy-to-understand
behavior and predictable failure modes. Beyond this, we also aim to substantially improve the scalability in order to
be able to easily support a cluster with thousands of shards.

## Design

### Terminology

| Term | Description |
| --- | --- |
| Node | A Single redis-server process |
| Shard | A logical entity comprised of a group of nodes owning the same slots. It is automatically allocated when a new unique slot range is assigned to a node and automatically deallocated when all slots are moved away |
| Host | A physical / virtual machine hosting the Flotilla processes |

### High Level Overview

Flotilla is composed of two tiers of strongly consistent, consensus-based control plane systems - The Topology Director 
(TD) and the Failover Coordinator (FC).

The Topology Director is the source of truth for shard existence and metadata, slot range allocation and Failover Coordinator
assignments. In addition to the above it also propagates failover information to all Failover Coordinators so nodes across 
the cluster will be able to return an up-to-date CLUSTER SLOTS response.

The Failover Coordinators is the source of truth for Primary/Replica roles. Each Failover Coordinators is assigned a subset 
of the total nodes in the cluster, receives & propagates heartbeat information from them and enables them to make failover decisions in a
consistent & timely manner.

Maintaining these two tiers allows scaling the number of nodes while:
- Maintaining an upper bound over the time required to decide and propagate failover decisions.
While the time required to propagate the updated primary/replica status depends on the number of Failover Coordinators in the deployment
the maximum number of these should always be small enough to allow us to guarantee a substantially better SLA than what
the current implementation provides.
- Limiting the clustering overhead on individual nodes in the deployment.
Depending on pub/sub mode, an individual node only needs to maintain a connection to its Failover Coordinator (and primary/replicas).

While Flotilla can be implemented using any sort of strongly consistent system, the reference implementation we propose
uses Redis to host the Topology Director & Failover Coordinator logic. This allows for different deployment modes depending on 
the needs and orchestrating abilities of the administrator:
- Full Blown: Dedicated nodes holding the Topology Directors and multiple Failover Coordinators dividing the data nodes among them.
- Midrange: Dedicated nodes holding both the Topology Director and Failover Coordinator (with just one Failover Coordinator owning all the data nodes).
- Lightweight: Topology Director and Failover Coordinator located on a subset of data nodes. This removes the need to maintain and orchestrate 
  non-data nodes at the cost of potentially impacting performance on the dual-use nodes.
- Naive: Topology Director and Failover Coordinator located on all nodes. This allows for the quickest bootstrapping, but 
  severely limits scalability as all nodes are part of the topology quorum.

It is important to note that these deployment modes are not encoded into the Redis/Flotilla implementation (Other than redis-cli
as mentioned below). It is possible to move between them by migrating slots and replacing nodes which hold role combinations 
which aren't relevant anymore.  
The various options are mentioned here in order to make sure we consider the ramification of each deployment mode in the
rest of the design.

When redis-cli is used to bootstrap a cluster, it will support receiving a flag specifying the desired deployment mode.
For example, when bootstrapping <= 11 nodes (exact number TBD once we have a full implementation we can benchmark) it will default to
provisioning all nodes as having all three roles as this is the easiest deployment to orchestrate.

![Flotilla High Level overview](images/high_level.jpg?raw=true "High Level Overview")

### The Topology Director

The Topology Director serves as the source of truth for the cluster topology (excluding primary/replica status) and helps propagate
information from Failover Coordinators. It never handles any user data, only topology metadata.
Since it owns the metadata of the entire cluster it can allow changing this metadata while preserving the strong
consistency guarantees we desire.
For example - it can ensure that if a Shard holds a slot marked as Migrating, there's a corresponding Shard with the slot marked as Importing.
We also rely on the Failover Coordinator to distribute the primary/replica state of individual nodes - This state is received periodically
from every Failover Coordinator, cached in the Topology Director and relayed to all other Failover Coordinators when they send their state.

- A completely reactive entity
- Receives requests to alter the cluster topology and adjusts the internal state as needed
- Validates change requests for correctness
- Responds to topology queries by the other components

#### Interfaces

The Topology Director interface is declarative other than join/remove flows. This allows for easy orchestration via various tools. 
All the below commands are synchronous and blocking from the client perspective.

With the administrator, directly for creating the cluster / updating and removing nodes:

**FLOTILLA.TOPOLOGY**: Similar to CLUSTER.SLOTS/NODES - response lists all the Flotilla nodes and their metadata  
**FLOTILLA.REMOVE_NODE**: Remove a given node from the cluster  
**FLOTILLA.ASSIGN_SLOTS**: Assign a set of slots to specified Shard  
**FLOTILLA.UPDATE_NODES**: Change the topology of a list of nodes    

    Update/Assign_slots commands can specify the epoch they are based on in order to allow flotilla to reject outdated requests.
    Update allows:
    - Taking ownership of unassigned slots
    - Moving an unallocated node / shard between Failover Coordinators
  
With the administrator via the individual node for adding nodes / Failover Coordinators to Flotilla:

**FLOTILLA.CREATE_TD**: Create the Flotilla Cluster from the current node  
**FLOTILLA.CREATE_FC**: Create a new Failover Coordinator entity from the current node  
**FLOTILLA.JOIN_TD**: Add the node to be part of the Topology Director quorum  
**FLOTILLA.JOIN_FC**: Add the node to be part of a given Failover Coordinator  
**FLOTILLA.JOIN_NODE**: Add the data node to Flotilla  
    
    All Join/Creation requests can specify a desired Shard ID/slot range up front or set it later using an UPDATE command.
    Responses contain the new node/Failover Coordinator IDs and the topology information needed for the new node to start functioning - a list
    of all Topology Director nodes and all the nodes belongining to the relevant Failover Coordinator.  
    Full topology will be retrieved later (for most nodes from their Failover Coordinator) in order to minimize load on the Topology Director in very large 
    clusters.
  
With the Failover Coordinators for topology & primary/replica state propagation:

**FLOTILLA.FAILOVER_STATUS**: Sent periodically from every Failover Coordinator

    Includes the last known cluster epoch.  
    Lists all nodes owned by the Failover Coordinator:
    - The last configuration epoch they acknowledged   
    - Their primary/replica status
    - Metadata like:
        - Replication offset
        - Last heard timestamp
     
    The response includes:
    - The last failover status received from all other Failover Coordinators
    - If epoch is outdated, the current topology


### The Failover Coordinator

The Failover Coordinator allows individual nodes to achieve consensus regarding Primary / Replica roles within the shard
they belong to and also propagates topology state changes to all the nodes it is responsible for.

It periodically sends a snapshot of the primary/replica status of all the nodes it owns to the Topology Director, and
receives topology updates as the response.

#### Interfaces

The Failover Coordinator does not expose an external interface, it's entirely internal to Flotilla and beyond troubleshooting there's no
need for the administrator to interact directly with it.

- With the Topology Director for topology & primary/replica state propagation
- With the nodes for heartbeat, leadership change requests & topology propagation.

### The Node

Each node is responsible to periodically send heartbeat information to the Failover Coordinator which owns it. The response it receives
contains the updated topology & last heartbeat times of the other nodes in the same shard (the group of nodes
owning the same slots). The node is responsible for promoting itself by writing the updated status to the Failover Coordinator if it
detects that its current primary has not sent a heartbeat for a long enough time.

The node will mark itself as "down" when it unable to reach the FC for X number of seconds (this can't be ticks since it isn't receiving them). 
It will either stop serving all traffic, or stop serving write traffic based off of the `cluster-allow-reads-during-cluster-down`
configuration.

#### Interfaces

- With the Failover Coordinator for topology updates and setting primary status
- With the Topology Director for registering into the cluster

**FLOTILLA.HEARTBEAT**: Detailed in depth under [Failover](#failover)

## Flows

### Membership and role management

All membership management is performed against the Topology Director. The change is then propagated to all other nodes via the Failover Coordinators.
The Topology Director is responsible for preventing *concurrent* changes to the same node / shard.
Concurrent here means changes which have not yet been propagated to the entity in question and acknowledged by it via the
known epoch field in the node(s) heartbeat.

Every membership change triggers a configuration epoch update by the Topology Director, and this epoch is then propagated to the Failover Coordinators
and through them to the individual nodes. The periodical status message sent by the Failover Coordinator to the Topology Director allows the Topology Director to know
the configuration epoch last received by the individual nodes.

#### Cluster creation

The first step in provisioning a Flotilla cluster is to create the Topology Director & at least one Failover Coordinator (In the reference - Redis based
implementation - this can be the same shard). Once this is done other nodes & Failover Coordinators can be joined / created and deleted at will. 
<!--
Should elaborate more here -- but it is very much tied to the reference implementation with Topology Director/Failover Coordinator running on Redis nodes
-->


#### Join

In order to join a shard into the cluster the administrator issues a join command on the joining node with the address
(or addresses) of the Topology Director. The node then emits the join request to Flotilla and receives the essential cluster topology
in the response.
The request can specify:
- Slot range (in case we're claiming unassigned slots) / Shard ID. In case none is provided, the node will be added as 
  free-floating in the initial implementation.
- Failover Coordinator ID. In case this isn't provided / implied by the Shard ID the node will be added to the Failover Coordinator 
  with the smallest number of nodes with the Failover Coordinator ID acting as tiebreaker.


The Topology Director is responsible to validate that:
- A requested slot range is either fully unassigned OR entirely (i.e., all requested slots and only them) assigned to an
existing shard.
- The Failover Coordinator exists, and it owns the other nodes of the relevant shard
(if the new node is supposed to join an existing shard).
  
The Topology Director response provides the node with the details of the Failover Coordinator which owns the node and its ID. 
The full topology will be received from the Failover Coordinator once the new node starts communicating with it. A node must
persist this metadata locally in order to be able to rejoin the cluster automatically after crashes/restarts, otherwise the node
will start from scratch and would need to be added again.

![Join Node](images/join_node.jpg?raw=true "Add a data node to a cluster")
    
#### Remove Node

When the Topology Director receives a request to delete a node, it first validates that this isn't this last node in its shard.
If it isn't (or a force flag is provided), then the Topology Director removes the node from its topology and increases the configuration
epoch.
Once this new configuration epoch reaches the relevant Failover Coordinator, it will refuse heartbeat requests from the removed node with
an error indicating the node does not exist in the system. This will cause a *fully joined* node to stop communication
with the Failover Coordinator and its Primary. In case the node is a Primary which hasn't been demoted by the admin, it will
stop responding to client requests but keep replication connection open to prevent data loss (since it will no longer succeed
in emitting a heartbeat, fail-over will soon take place).

#### Migrate a Shard between Failover Coordinators

*One of the Flotilla invariants is that all nodes in a certain shard belong to the same Failover Coordinator -- this means we must
move an entire shard at once, and not one by one. In order to move a single node between Failover Coordinators one must first
remove the association with its shard*

When the Topology Director receives a request to move a shard to Failover Coordinator X, it simply sets the correct Failover Coordinator in its topology and increases
the epoch. This new topology will then propagate to the various Failover Coordinators and nodes. There's no requirement for ordering in this
flow -- either the target Failover Coordinator or the source Failover Coordinator can get the message first and the system will still achieve the desired end
state.
When an Failover Coordinator receives a heartbeat request from a node it does not own it returns a MOVED response to the correct Failover Coordinator
(there will always be such an Failover Coordinator since the replacement is done in an atomic manner). As an implementation detail, after
being moved between Failover Coordinators a node should wait double its normal time before initiating failover in order to avoid redundant
fail-overs.
If the target Failover Coordinator is still unaware of the new topology it might cause the shards to ping-pong a bit between the Failover Coordinators, so a node should contain logic to reduce
the frequency of heartbeats after a moved response, but even if it doesn't this situation will self-resolve without
causing any damage.

Worth noting that this flow is the main reason why a Topology Director **must** ensure no concurrent updates to the same shard:
- Shard X comprised of 3 nodes X1,X2,X3 belong to Failover Coordinator A
- X is moved to Failover Coordinator B
- All entities other than X3 get the updated topology
- X1 sends a heartbeat as primary to Failover Coordinator **B**
- X is moved back to Failover Coordinator A, but Failover Coordinator B is delayed in receiving the updated topology
- X3 sends a heartbeat as primary to Failover Coordinator **A**
While this situation will self-resolve when Failover Coordinator B receives the updated topology, Flotilla aims to guarantee a strongly 
consistent topology - so it needs to prevent concurrent updates.

![Move Shard](images/move_shards.jpg?raw=true "Move shard between Failover Coordinators")

### Failover

Every K milliseconds, every node sends a heartbeat to the Failover Coordinator which owns it. This heartbeat includes:
- Replication offset (for replicas)
- Last known cluster topology epoch
- Last known Shard topology epoch
- Role (primary/replica)

The Failover Coordinator can respond with the following:
- ACK: The node topology is up-to-date. The heartbeat data is saved in the Failover Coordinator along with the current tick.
- NACK: The node Topology is out-of-date
- MOVED: The node does not belong to the Failover Coordinator. This response includes the details of the relevant Failover Coordinator.

The response includes:
- The heartbeat results from all the nodes in the same shard (and their 'tick' value)
- If needed, the updated cluster topology.

If a replica node detects that its primary has not sent a heartbeat for more than N ticks it checks if it is the most
up-to-date replica using the reported replication offset. If it is, then it sends a heartbeat request with an updated
role and an incremented shard epoch. If more than one node has the most recent offset, then the node ID is used
as tiebreaker. When the Failover Coordinator notes the epoch change it modifies the heartbeats of all other nodes in the shard to
the replica role.
Note that if multiple nodes attempt to promote themselves only one can succeed (The first one to be processed by the Failover Coordinator)
All the rest will have their heartbeat rejected with a NACK.
A planned failover works very similarly to the current CLUSTER FAILOVER command and can also support the FORCE flag (The
TAKEOVER flag _is not_ supported as it violated the topology consistency by definition). Once the replica sees its
replication offset matches the one provided by the primary it simply announces itself as the new primary, and the former
primary will set itself as replica.

If a node does not get a heartbeat response for a long enough time period it will fence itself off and stop serving traffic.

![Failover](images/failover.jpg?raw=true "Failover")

#### Failover Coordinator Tick

In order to avoid relying on multiple clocks when making failover decisions, the node relies on a monotonically increasing
counter maintained by the Failover Coordinator. Every time a heartbeat is received by the Failover Coordinator, it is saved internally with the current tick
(which is increased by the Failover Coordinator every K milliseconds). Since a node knows what tick its heartbeat was received in, it can tell how
many ticks have passed since the primary sent its last heartbeat.

### Resharding

When a Topology Director receives an update which moves a set of slot to a specific shard it sets the appropriate slots to the
importing/migrating state and bumps the topology epoch. This new slot topology will eventually reach all the relevant nodes.  

It is expected that we will have atomic slot migration (based on fork+serialize like Redis replication) in Redis 
_before_ Flotilla is GA - in which case Flotilla will only support that and not the existing key migration flow.
Failing that, the behavior around key migration will be exactly same as in the current Redis Cluster - A node with a slot 
set to Migrating returns ASK as appropriate, a node with a slot set to Importing requires Asking (and that behavior can
be enhanced with things like bulk/full slot migrate without any impact on the Flotilla design).

In our reference implementation we currently require the administrator to explicitly call the Topology Director with another update to terminate
the Migrating/Importing state, but an alternative would be to handle this inside Flotilla by having nodes with migrating
slots add the number of keys per slot to their heartbeat -- once that number drops to zero the Topology Director can automatically clear
the state (It's also possible to emit the MIGRATE_DONE command from the primary migrating shard).

![Resharding](images/resharding.jpg?raw=true "Failover")

## Flotilla Implementation Details

<!--
Maybe mention Flotilla as a reference architecture and different possible implementations and an existing prototype implementation.
-->

### Roles

While the Flotilla spec mostly discusses data nodes (and not Failover Coordinator & Topology Director implementation), in our reference implementation
nodes within a Flotilla deployment have one or more roles:
- Topology Director
- Failover Coordinator
- Data node

The role of the node is specified in the join request (see [TBD Topics](#tbd-topics)).

### Node and Failover Coordinator IDs

All IDs in the system are:
- Unique numbers
- Monotonically increasing (with the caveat that admin can manually specify an ID for an entity and Flotilla will accept 
  it as long as it does not violate the uniqueness constraint)
- Non-repeating

The advantage to using numeric IDs is that:

- It minimizes the payload for topology messages
  (even if someone constantly adds nodes at one per <b>n</b>s(!!), it will ~500 years for the ID counters to exceed 64 bits)
- It's easier to parse & communicate for humans

The disadvantage is it requires some garbage collection by the orchestrating admin when a node joining fails at the last second
(after the node is registered in the system but before it gets the acknowledgment).

For backwards compatibility, IDs will be padded with zeroes to a 160 bit number when responding to the CLUSTER SLOTS / NODES command.

Sidenote: *Switching to GUIDs which are determined either by the joining node is an alternative, but the big disadvantage to this
approach is that proper GUIDs require quite a lot of space (current Redis Cluster uses 160 bits) to make the chance of
collision tiny. The overhead of transmitting these 20 bytes per node in each and every msg can get prohibitive.*

Normally, a node join / Failover Coordinator creation request will not contain an ID and the Topology Director will allocate one and
return it to the calling node in the response. These commands will also allow the caller to specify an ID and it is the
responsibility of the Topology Director to ensure that this ID has not been allocated already before accepting the request.

### Publish/Subscribe

Flotilla will support both sharded and un-sharded pub/sub.  

For sharded pub/sub, all primaries maintain a connection to all replicas and use that connection to propagate notifications. 

For unsharded pub/sub (and this is still somewhat TBD), in addition to the connection to all replicas, primaries will maintain 
a connection to either all other primaries (in the case of a small cluster) or a subset of other primary nodes. 
That subset will in turn propagate to another distinct subset thereby tiering the notification delivery across the entire cluster.

This will require adding a flag to the publish request which will mark it as a secondary publish in order to prevent potential loops. 

One option for selecting the subset is by dividing the primary nodes into K buckets:
- K^2 = ~ Number of shards in the cluster
- Primary node belongs to bucket A if its shard ID % K == A
- The primary receiving the initial notification will propagate it to an arbitrary primary in each bucket (including its own).
- That primary will then send the notification to all other primaries in its bucket.

It is worth noting we can (and probably should) decouple and implement this sort of mechanism ASAP (without depending the new
cluster implementation) as it will substantially improve the behavior of the existing cluster.

### Global Configuration

Flotilla can distribute configuration settings in addition to topology - This will greatly ease the pain of managing
large clusters (currently admins need to set configs on every single node in the cluster).
These config settings can include things like:
- ACLs
- A subset of configuration from redis.conf
- Functions (?)

The simplest way to support this is to add the required information to every TopologyState response (using another epoch counter
to decide when to send the information).
This approach assumes that the rate of change for the global configuration is (at most) on the same order of magnitude as 
the rate of change for the cluster topology (adding / removing nodes & slot ownership). Settings which change very frequently 
cannot be handled by the centralized control plane as it would quickly be overwhelmed.
By propagating the individual node 'global config' epoch as part of the status messages to the TD we can expose when a certain configuration
change has been received by all nodes.
Node local config changes (to the relevant global settings) would be overridden every time a change is propagated through
the cluster.

Another possibility would be to leverage node level distribution mechanism like unsharded Pub/Sub.
- Would require some kind of reliability guarantee that isn't present in the current/proposed implementation.
- Has the benefit of much higher scalability, supporting Lua-like Functions usage.

### Reference Implementation

Our reference implementation uses a single Redis module that contains both the Failover Coordinator & Topology Director functionality
and incorporates an open-source Raft library (The module is written in Rust with the Raft lib in C).
The interface between the various components (Topology Director, Failover Coordinator, Nodes) uses Protobuf serialized over RESP.
Other than the very minimal data node logic, all Failover Coordinator/Topology Director logic is external to the main Redis event loop.

It is important to stress that it is perfectly possible to implement the Flotilla specification using something other 
than Redis+modules. 

#### Road to Production

Once this specification goes through a review process by the community, we intend to push a PoC implementation of the spec
to the main Redis repo (with the same BSD license as Redis) and enhance it until it reaches GA quality. When this is achieved, 
we will use the next major version to deprecate the existing cluster implementation and release the two side-by-side (with a compilation
flag to control which one is used).

As a big TODO - we need to create a troubleshooting "Everything is broken, what do!" section once we have a more concrete
implementation. Said implementation will also need to provide "force override" flags for various operations which will allow an admin
to override some Flotilla validations in order to restore availability ASAP.
Specifically, need to address:
- How to fix a situation where there's an FC level issue which prevents failovers from taking place.
- Handling rejected topology changes due to nodes which haven't acknowledged a previous change by overriding the validation 
  or removing the unresponsive nodes. 

<!--
Some future topics:

* Define an official RESP-based wire interface
* Define a function call interface for a "pluggable cluster.c"
* Describe how ClusterV2 will be implemented based on that.
-->

#### Potential Enhancements

- Topology Director Proxy mode: Allow sending topology commands to an arbitrary node and have that node proxy the request to the Topology Director. 
  This increases management simplicity, but would also be undesirable in orchestrated deployments.
- In a Fork+Replicate resharding flow, there's a potential for increased memory consumption due to CoW buffers which might 
  break extremely resource constrained environments. We might offer a 'forkless' configuration which will block writes to the 
  entire slot while the data is being serialized/copied to the other node.
- Handling misconfig bugs: In cases where a node receives an illegal topology (for example: a node belonging to shard A is now 
  marked as belonging to shard B) it may decide to reject the topology in order to avoid data loss due to Flotilla bugs.
  (to be clear -- all such potential misconfigs would be considered Flotilla bugs since the Topology Director is never supposed
  to emit an invalid topology).
- Auto-assign free floating nodes according to need.
- It is possible to add smarter orchestration into the Topology Director layer which will allow it to adjust the roles of nodes 
  participating in the cluster as needed and autonomously move between the various deployments modes as the cluster scales.
  Doing this properly will also require the ability to change the role of an existing node.
    


### TBD Topics:

- Do we need to support control plane role changes (eg: Convert a node that was added as a Topology Director+Failover Coordinator+Data to be Data only)  
  Supporting this will somewhat ease the scaling from/to very small clusters, but does have some implementation cost