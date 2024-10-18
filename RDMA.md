RDMA Support
============

Connections
-----------
RDMA operations also go through a connection abstraction layer that hides I/O and read/write event handling from the caller.

Redis works under a stream-oriented protocol while RDMA is a message protocol, so additional work is required to support RDMA-based Redis.

## Protocol
In Redis, separate control-plane(to exchange control message) and data-plane(to
transfer the real payload for Redis).

### Control message
For control message, use a fixed 32 bytes message which defines structures:
```
typedef struct RedisRdmaFeature {
    /* defined as following Opcodes */
    uint16_t opcode;
    /* select features, 0 means bit0-63, 1 means 64-127, etc */
    uint16_t select;
    uint8_t rsvd[20];
    /* feature bits */
    uint64_t features;
} RedisRdmaFeature;

typedef struct RedisRdmaKeepalive {
    /* defined as following Opcodes */
    uint16_t opcode;
    uint8_t rsvd[30];
} RedisRdmaKeepalive;

typedef struct RedisRdmaMemory {
    /* defined as following Opcodes */
    uint16_t opcode;
    uint8_t rsvd[14];
    /* address of a transfer buffer which is used to receive remote streaming data,
     * aka 'RX buffer address'. The remote side should use this as 'TX buffer address' */
    uint64_t addr;
    /* length of the 'RX buffer' */
    uint32_t length;
    /* the RDMA remote key of 'RX buffer' */
    uint32_t key;
} RedisRdmaMemory;

typedef union RedisRdmaCmd {
    RedisRdmaFeature feature;
    RedisRdmaKeepalive keepalive;
    RedisRdmaMemory memory;
} RedisRdmaCmd;
```

### Opcodes
| Command|  Value | Structure | Description |
| :----: | :----: | :----: | :----: |
| GetServerFeature   | 0 | RedisRdmaFeature   | required, get the features offered by Redis server |
| SetClientFeature   | 1 | RedisRdmaFeature   | required, negotiate features and set it to Redis server |
| Keepalive          | 2 | RedisRdmaKeepalive | required, detect unexpected orphan connection |
| RegisterXferMemory | 3 | RedisRdmaMemory    | required, tell the 'RX transfer buffer' information to the remote side, and the remote side uses this as 'TX transfer buffer'. Note that only one transfer buffer is allowed. |

### Operations of RDMA
- To send a control message by RDMA '**ibv_post_send**' with opcode '**IBV_WR_SEND**' with structure
  'RedisRdmaCmd'.
- To receive a control message by RDMA '**ibv_post_recv**', and the received buffer
  size should be size of 'RedisRdmaCmd'.
- To transfer stream data by RDMA '**ibv_post_send**' with opcode '**IBV_WR_RDMA_WRITE**'(optional) and
  '**IBV_WR_RDMA_WRITE_WITH_IMM**'(required), to write data segments into a connection by
  RDMA [WRITE][WRITE][WRITE]...[WRITE WITH IMM], the length of total buffer is described by
  immediate data(unsigned int 32).


### Maximum WQE(s) of RDMA
Currently no specific restriction is defined in this protocol. Recommended WQEs is 1024.
Flow control for WQE MAY be defined/implemented in the future.


### The workflow of this protocol
```
                                                                  server
                                                                  listen RDMA port
          client
                -------------------RDMA connect------------------>
                                                                  accept connection
                <--------------- Establish RDMA ------------------

                --------Get server feature [@IBV_WR_SEND] ------->

                --------Set client feature [@IBV_WR_SEND] ------->
                                                                  setup RX buffer
                <---- Register transfer memory [@IBV_WR_SEND] ----
[@ibv_post_recv]
setup TX buffer
                ----- Register transfer memory [@IBV_WR_SEND] --->
                                                                  [@ibv_post_recv]
                                                                  setup TX buffer
                -- Redis commands [@IBV_WR_RDMA_WRITE_WITH_IMM] ->
                <- Redis response [@IBV_WR_RDMA_WRITE_WITH_IMM] --
                                  .......
                -- Redis commands [@IBV_WR_RDMA_WRITE_WITH_IMM] ->
                <- Redis response [@IBV_WR_RDMA_WRITE_WITH_IMM] --
                                  .......


RX is full
                ------ Register Local buffer [@IBV_WR_SEND] ----->
                                                                  [@ibv_post_recv]
                                                                  setup TX buffer
                <- Redis response [@IBV_WR_RDMA_WRITE_WITH_IMM] --
                                  .......

                                                                  RX is full
                <----- Register Local buffer [@IBV_WR_SEND] ------
[@ibv_post_recv]
setup TX buffer
                -- Redis commands [@IBV_WR_RDMA_WRITE_WITH_IMM] ->
                <- Redis response [@IBV_WR_RDMA_WRITE_WITH_IMM] --
                                  .......

                ------------------RDMA disconnect---------------->
                <-----------------RDMA disconnect-----------------
```


## Event handling
There is no POLLOUT event of RDMA comp channel:
   1, if TX is not full, it's always writable.
   2, if TX is full, should wait a 'RegisterLocalAddr' message to refresh
      'TX buffer'.

To-Do List
----------
- [ ] Redis Over RDMA implementation for redis-server
- [ ] Redis Over RDMA implementation for hiredis
- [ ] rdma client & benchmark
- [ ] POLLOUT event emulation for hiredis
- [ ] auto-test suite is not implemented currently
