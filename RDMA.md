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

    ./src/redis-server --rdma-port 6379  --rdma-bind 192.168.122.100 \
        --protected-mode no

To connect to this Redis server with `redis-cli`:

    ./src/redis-cli -h 192.168.122.100 --rdma

Note that the network card (192.168.122.100 of this example) should support
RDMA. It's also possible to have both RDMA and TCP available, and there is no
conflict of TCP(6379) and RDMA(6379), Ex:

    ./src/redis-server --bind 127.0.0.1 --port 6379 \
                       --rdma-port 6379 --rdma-bind 192.168.122.100 \
                       --protected-mode no

### rdma-core
Upgrading rdma-core is suggested, clone source code from:

    https://github.com/linux-rdma/rdma-core

Follow README.md of rdma-core and compile/install it. Or run redis with
environment variable like this:

    LD_LIBRARY_PATH=/root/path/of/rdma-core/build/lib:$LD_LIBRARY_PATH \
        ./src/redis-server --rdma-port 6379  --rdma-bind 192.168.122.100 \
        --protected-mode no



Connections
-----------

RDMA IO mechanism also goes through a connection abstraction layer that hides
read/write event handling from the caller.

Because RDMA is a message-oriented protocol, TCP is a stream-oriented protocol,
to implement the stream-like operations(Ex, read/write) needs additional works
in RDMA scenario. Luckly, rdma-core has already supported a stream like protocol
named "rsocket", redis could uses rsocket/rclose/rsend/rrecv/rpoll API directly
by linking librdmacm.so(no other dependency).

Note that rdma-core should be upgraded.


To-Do List
----------

- [ ] support sentinel mode to bind RDMA.
- [ ] more abstraction work should be implemented.
- [ ] auto-test suite is not implemented currently.
