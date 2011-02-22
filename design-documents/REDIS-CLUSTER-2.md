Redis Cluster - Alternative 1

28 Apr 2010: Ver 1.0 - initial version

Overview
========

The motivations and design goals of Redis Cluster are already outlined in the
first design document of Redis Cluster. This document is just an attempt to
provide a completely alternative approach in order to explore more ideas.

In this document the alternative explored is a cluster where communication is
performed directly from client to the target node, without intermediate layer.

The intermediate layer can be used, in the form of a proxy, in order to provide
the same functionality to clients not able to directly use the cluster protocol.
So in a first stage clients can use a proxy to implement the hash ring, but
later this clients can switch to a native implementation, following a
specification that the Redis project will provide.

In this new design fault tolerance is achieved by replicating M-1 times every
data node instead of storing the same key M times across nodes.

From the point of view of CAP our biggest sacrifice is about "P", that is
resistance to partitioning. Only M-1 nodes can go down for the cluster still
be functional. Also when possible "A" is somewhat sacrificed for "L", that
is, Latency. Not really in the CAP equation but a very important parameter.

Network layout
==============

In this alternative design the network layout is simple as there are only
clients talking directly to N data nodes. So we can imagine to have:

- K Redis clients, directly talking to the data nodes.
- N Redis data nodes, that are, normal Redis instances.

Data nodes are replicate M-1 times (so there are a total of M copies for
every node). If M is one, the system is not fault tolerant. If M is 2 one
data node can go off line without affecting the operations. And so forth.

Hash slots
==========

The key space is divided into 1024 slots.

Given a key, the SHA1 function is applied to it.
The first 10 bytes of the SHA1 digest are interpreted as an unsigned integer
from 0 to 1023. This is the hash slot of the key.

Data nodes
==========

Data nodes are normal Redis instances, but a few additional commands are
provided.

    HASHRING ADD ... list of hash slots ...
    HASHRING DEL ... list of hash slots ...
    HASHRING REHASHING slot
    HASHRING SLOTS => returns the list of configured slots
    HSAHRING KEYS ... list of hash slots ...

By default Redis instances are configured to accept operations about all
the hash slots. With this commands it's possible to configure a Redis instance
to accept only a subset of the key space.

If an operation is performed against a key hashing to a slot that is not
configured to be accepted, the Redis instance will reply with:

    -ERR wrong hash slot

More details on the `HASHRING` command and sub commands will be showed later
in this document.

Additionally three other commands are added:

    DUMP key
    RESTORE key <dump data>
    MIGRATE key host port

`DUMP` is used to output a very compact binary representation of the data stored at key.

`RESTORE` re-creates a value (storing it at key) starting from the output produced by DUMP.

`MIGRATE` is like a server-side DUMP+RESTORE command. This atomic command moves one key from the connected instance to another instance, returning the status code of the operation (+OK or an error).

The protocol described in this draft only uses the `MIGRATE` command, but this in turn will use `RESTORE` internally when connecting to another server, and DUMP is provided for symmetry.

Querying the cluster
====================

1) Reading the cluster config
-----------------------------

Clients of the cluster are required to have the cluster configuration loaded
into memory. The cluster configuration is the sum of the following info:

- Number of data nodes in the cluster, for instance, 10
- A map between hash slots and nodes, so for instnace:
  hash slot 1 -> node 0
  hash slot 2 -> node 5
  hash slot 3 -> node 3
  ... and so forth ...
- Physical address of nodes, and their replicas.
  `node 0 addr -> 192.168.1.100`
  `node 0 replicas -> 192.168.1.101, 192.168.1.105`
- Configuration version: the SHA1 of the whole configuration

The configuration is stored in every single data node of the cluster.

A client without the configuration in memory is require, as a first step, to
read the config. In order to do so the client requires to have a list of IPs
that are with good probability data nodes of the cluster.

The client will try to get the config from all this nodes. If no node is found
responding, an error is reported to the user.

2) Caching and refreshing the configuration
-------------------------------------------

A node is allowed to cache the configuration in memory or in a different way
(for instance storing the configuration into a file), but every client is
required to check if the configuration changed at max every 10 seconds, asking
for the configuration version key with a single GET call, and checking if the
configuration version matches the one loaded in memory.

Also a client is required to refresh the configuration every time a node
replies with:

    -ERR wrong hash slot

As this means that hash slots were reassigned in some way.

Checking the configuration every 10 seconds is not required in theory but is
a good protection against errors and failures that may happen in real world
environments. It is also very cheap to perform, as a GET operation from time
to time is going to have no impact in the overall performance.

3) Read query
-------------

To perform a read query the client hashes the key argument from the command
(in the initial version of Redis Cluster only single-key commands are
allowed). Using the in memory configuration it maps the hash key to the
node ID.

If the client is configured to support read-after-write consistency, then
the `master` node for this hash slot is queried.

Otherwise the client picks a random node from the master and the replicas
available.

4) Write query
--------------

A write query is exactly like a read query, with the difference that the
write always targets the master node, instead of the replicas.

Creating a cluster
==================

In order to create a new cluster, the `redis-cluster` command line utility is
used. It gets a list of available nodes and replicas, in order to write the
initial configuration in all the nodes.

At this point the cluster is usable by clients.

Adding nodes to the cluster
===========================

The command line utility `redis-cluster` is used in order to add a node to the
cluster:

1. The cluster configuration is loaded.
2. A fair number of hash slots are assigned to the new data node.
3. Hash slots moved to the new node are marked as "REHASHING" in the old nodes, using the HASHRING command:

    `HASHRING SETREHASHING 1 192.168.1.103 6380`

The above command set the hash slot "1" in rehashing state, with the
"forwarding address" to 192.168.1.103:6380. As a result if this node receives
a query about a key hashing to hash slot 1, that *is not present* in the
current data set, it replies with:

    -MIGRATED 192.168.1.103:6380

The client can then reissue the query against the new node.

Instead even if the hash slot is marked as rehashing but the requested key
is still there, the query is processed. This allows for non blocking
rehashing.

Note that no additional memory is used by Redis in order to provide such a
feature.

4. While the Hash slot is marked as `REHASHING`, `redis-cluster` asks this node the list of all the keys matching the specified hash slot. Then all the keys are moved to the new node using the `MIGRATE` command.
5. Once all the keys are migrated, the hash slot is deleted from the old node configuration with `HASHRING DEL 1`. And the configuration is update.

Using this algorithm all the hash slots are migrated one after the other to the new node.
In practical implementation before to start the migration the
`redis-cluster` utility should write a log into the configuration so that
in case of crash or any other problem the utility is able to recover from
were it left.

Fault tolerance
===============

Fault tolerance is reached replicating every data node M-1 times, so that we
have one master and M-1 replicas for a total of M nodes holding the same
hash slots. Up to M-1 nodes can go down without affecting the cluster.

The tricky part about fault tolerance is detecting when a node is failing and
signaling it to all the other clients.

When a master node is failing in a permanent way, promoting the first slave
is easy:
1. At some point a client will notice there are problems accessing a given node. It will try to refresh the config, but will notice that the config is already up to date.
2. In order to make sure the problem is not about the client connectivity itself, it will try to reach other nodes as well. If more than M-1 nodes appear to be down, it's either a client networking problem or alternatively the cluster can't be fixed as too many nodes are down anyway. So no action is taken, but an error is reported.
3. If instead only 1 or at max M-1 nodes appear to be down, the client promotes a slave as master and writes the new configuration to all the data nodes.

All the other clients will see the data node not working, and as a first step will try to refresh the configuration. They will successful refresh the configuration and the cluster will work again.

Every time a slave is promoted, the information is written in a log that is actually a Redis list, in all the data nodes, so that system administration tools can detect what happened in order to send notifications to the admin.

Intermittent problems
---------------------

In the above scenario a master was failing in a permanent way. Now instead
let's think to a case where a network cable is not working well so a node
appears to be a few seconds up and a few seconds down.

When this happens recovering can be much harder, as a client may notice the
problem and will promote a slave to master as a result, but then the host
will be up again and the other clients will not see the problem, writing to
the old master for at max 10 seconds (after 10 seconds all the clients are
required to perform a few GETs to check the configuration version of the
cluster and update if needed).

One way to fix this problem is to delegate the fail over mechanism to a
failover agent. When clients notice problems will not take any active action
but will just log the problem into a `redis` list in all the reachable nodes,
wait, check for configuration change, and retry.

The failover agent constantly monitor this logs: if some client is reporting
a failing node, it can take appropriate actions, checking if the failure is
permanent or not. If it's not he can send a `SHUTDOWN` command to the failing
master if possible. The failover agent can also consider better the problem
checking if the failing mode is advertised by all the clients or just a single
one, and can check itself if there is a real problem before to proceed with
the fail over.

Redis proxy
===========

In order to make the switch to the clustered version of Redis simpler, and
because the client-side protocol is non trivial to implement compared to the
usual Redis client lib protocol (where a minimal lib can be as small as
100 lines of code), a proxy will be provided to implement the cluster protocol
as a proxy.

Every client will talk to a `redis-proxy` node that is responsible of using
the new protocol and forwarding back the replies.

In the long run the aim is to switch all the major client libraries to the
new protocol in a native way.

Supported commands
==================

Because with this design we talk directly to data nodes and there is a single
"master" version of every value (that's the big gain dropping "P" from CAP!)
almost all the redis commands can be supported by the clustered version
including MULTI/EXEC and multi key commands as long as all the keys will hash
to the same hash slot. In order to guarantee this, key tags can be used,
where when a specific pattern is present in the key name, only that part is
hashed in order to obtain the hash index.

Random remarks
==============

- It's still not clear how to perform an atomic election of a slave to master.
- In normal conditions (all the nodes working) this new design is just
  K clients talking to N nodes without intermediate layers, no routes:
  this means it is horizontally scalable with O(1) lookups.
- The cluster should optionally be able to work with manual fail over
  for environments where it's desirable to do so. For instance it's possible
  to setup periodic checks on all the nodes, and switch IPs when needed
  or other advanced configurations that can not be the default as they
  are too environment dependent.

A few ideas about client-side slave election
============================================

Detecting failures in a collaborative way
-----------------------------------------

In order to take the node failure detection and slave election a distributed
effort, without any "control program" that is in some way a single point
of failure (the cluster will not stop when it stops, but errors are not
corrected without it running), it's possible to use a few consensus-alike
algorithms.

For instance all the nodes may take a list of errors detected by clients.

If Client-1 detects some failure accessing Node-3, for instance a connection
refused error or a timeout, it logs what happened with LPUSH commands against
all the other nodes. This "error message" will have a timestamp and the Node
id. Something like:

    LPUSH __cluster__:errors 3:1272545939

So if the error is reported many times in a small amount of time, at some
point a client can have enough hints about the need of performing a
slave election.

Atomic slave election
---------------------

In order to avoid races when electing a slave to master (that is in order to
avoid that some client can still contact the old master for that node in
the 10 seconds timeframe), the client performing the election may write
some hint in the configuration, change the configuration SHA1 accordingly and
wait for more than 10 seconds, in order to be sure all the clients will
refresh the configuration before a new access.

The config hint may be something like:

    we are switching to a new master, that is x.y.z.k:port, in a few seconds

When a client updates the config and finds such a flag set, it starts to
continuously refresh the config until a change is noticed (this will take
at max 10-15 seconds).

The client performing the election will wait that famous 10 seconds time frame
and finally will update the config in a definitive way setting the new
slave as mater. All the clients at this point are guaranteed to have the new
config either because they refreshed or because in the next query their config
is already expired and they'll update the configuration.

EOF
