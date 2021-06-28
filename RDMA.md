RDMA Support
============

Getting Started
---------------

### Building

To build with RDMA support you'll need RDMA development libraries (e.g.
librdmacm-dev and libibverbs-dev on Debian/Ubuntu).

Run `make BUILD_RDMA=yes`.

### Running manually

To manually run a Redis server with RDMA mode:

    ./src/redis-server --rdma-port 6379  --bind 192.168.122.100 \
        --protected-mode no

To connect to this Redis server with `redis-cli`:

    ./src/redis-cli -h 192.168.122.100 --rdma

Note that the network card (192.168.122.100 of this example) should support
RDMA. It's also possible to have both RDMA and TCP available, and there is no
conflict of TCP(6379) and RDMA(6379), Ex:

    ./src/redis-server --rdma-port 6379 --port 6379  --bind 192.168.122.100 \
        --protected-mode no

Connections
-----------

All socket operations now go through a connection abstraction layer that hides
I/O and read/write event handling from the caller.

Because RDMA is a message-oriented protocol, TCP is a stream-oriented protocol,
to implement the stream-like operations(Ex, read/write) needs additional works
in RDMA scenario.

In redis, separate control-plane(to exchange control message) and data-plane(to
transfer the real payload for redis).

#### Protocol
For control message, use a 32 bytes message which defines a structure:
```
typedef struct RedisRdmaCmd {
    uint8_t magic;
    uint8_t version;
    uint8_t opcode;
    uint8_t rsvd[13];
    uint64_t addr;
    uint32_t length;
    uint32_t key;
} RedisRdmaCmd;
```

magic: should be 'R'
version: 1
opcode: define as following enum RedisRdmaOpcode
rsvd: reserved
addr: local address of a buffer which is used to receive remote streaming data,
      aka 'RX buffer address'
length: length of the 'RX buffer'
key: the RDMA remote key of 'RX buffer'


```
typedef enum RedisRdmaOpcode {
    RegisterLocalAddr,
} RedisRdmaOpcode;
```

RegisterLocalAddr: tell the 'RX buffer' information to the remote side, and the
                  remote side uses this as 'TX buffer'


The workflow of this protocol:
```
client                     server
      ----RDMA connect---->
                           handle connection, create RDMA resources
      <---Establish RDMA---
      <-Register RX buffer-
setup TX buffer
      -Register RX buffer->
                           setup TX buffer
      ---send commands---->
      <--send response-----

          .......

RX buffer is full
      -Register RX buffer->
                           setup TX buffer
      <-continue response--
```


#### Event handling
There is no POLLOUT event of RDMA comp channel:
   1, if TX is not full, it's always writable.
   2, if TX is full, should wait a 'RegisterLocalAddr' message to refresh
      'TX buffer'.
   3, run a timer of a RDMA connection to kick write handler periodically

To-Do List
----------

- [ ] rdma-cluster is to be implemented
- [ ] POLLOUT event emulation for hiredis
- [ ] auto-test suite is not implemented currently
