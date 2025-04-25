[![codecov](https://codecov.io/github/redis/redis/graph/badge.svg?token=6bVHb5fRuz)](https://codecov.io/github/redis/redis)

Redis is an open-source, in-memory data structure store. People use Redis as a
database, cache, message broker, streaming engine, and vector store.

* New to Redis? Start with [What is Redis](#what-is-redis) and [Getting Started](#getting-started)
* Ready to build from source? Jump to [Building Redis from Source](#building-redis-from-source)
* Want to contribute? See the [Code contributions](#code-contributions) section
and [CONTRIBUTING.md](./CONTRIBUTING.md)
* Looking for detailed documentation? Navigate to [redis.io/docs](https://redis.io/docs/)

## Table of contents

- [Table of contents](#table-of-contents)
- [What is Redis?](#what-is-redis)
  - [What is Redis Community Edition?](#what-is-redis-community-edition)
- [Why use Redis?](#why-use-redis)
- [Getting Started](#getting-started)
- [Drivers](#drivers)
- [Learn Redis](#learn-redis)
- [Cloud managed Redis](#cloud-managed-redis)
- [Community](#community)
- [Building Redis From Source](#building-redis-from-source)
  - [Fixing build problems with dependencies or cached build options](#fixing-build-problems-with-dependencies-or-cached-build-options)
  - [Fixing problems building 32 bit binaries](#fixing-problems-building-32-bit-binaries)
  - [Allocator](#allocator)
  - [Monotonic clock](#monotonic-clock)
  - [Verbose build](#verbose-build)
  - [Running Redis](#running-redis)
    - [Running Redis with TLS](#running-redis-with-tls)
  - [Playing with Redis](#playing-with-redis)
  - [Installing Redis](#installing-redis)
  - [Code contributions](#code-contributions)
- [Redis Trademarks](#redis-trademarks)
- [Redis internals](#redis-internals)

## What is Redis?

Redis is an **in-memory key-value database** (with
[persistence](https://redis.io/docs/latest/operate/oss_and_stack/management/persistence/))
in the class of NoSQL databases. It provides
[data structures](https://redis.io/docs/latest/develop/data-types/)
such as:

* [**strings**](https://redis.io/docs/latest/develop/data-types/strings/): text,
serialized objects, or binary arrays used for caching, counters, and bitwise
operations
* [**JSON**](https://redis.io/docs/latest/develop/data-types/json/): nested JSON
documents that are indexed and searchable using JSONPath expressions
* [**hashes**](https://redis.io/docs/latest/develop/data-types/hashes/):
field-value maps used to represent basic objects and store groupings of
key-value pairs
* [**lists**](https://redis.io/docs/latest/develop/data-types/lists/): linked lists
of string values used as stacks and queues
* [**sets**](https://redis.io/docs/latest/develop/data-types/sets/): unordered
collection of unique strings used for tracking unique items, relations, and
common set operations
* [**sorted sets**](https://redis.io/docs/latest/develop/data-types/sorted-sets/):
collection of unique strings ordered by an associated score used for
leaderboards and rate limiters
* [**vector sets**](https://redis.io/docs/latest/develop/data-types/vector-sets/):
collection of vector strings used for semantic search, semantic caching,
semantic routing, and Retrieval Augmented Generation (RAG)
* [**streams**](https://redis.io/docs/latest/develop/data-types/streams/):
append-only log with random access capabilities used for event sourcing,
sensor monitoring, and notifications
* [**geospatial indexes**](https://redis.io/docs/latest/develop/data-types/geospatial/):
coordinates used for finding nearby points within a given radius or bounding box
* [**bitmaps**](https://redis.io/docs/latest/develop/data-types/bitmaps/): a set of
bit-oriented operations defined on the string type used for efficient set
representations and object permissions
* [**bitfields**](https://redis.io/docs/latest/develop/data-types/bitfields/):
binary-encoded strings the let you set, increment, and get integer values of
arbitrary bit length used for counters and similar numeric values
* [**probabilistic structures**](https://redis.io/docs/latest/develop/data-types/probabilistic/):
collection of structures that give approximations of statistics such as counts,
frequencies, and rankings useful for efficient calculations when absolute
precision is not needed
* [**time series**](https://redis.io/docs/latest/develop/data-types/timeseries/):
data points indexed in time order used for monitoring sensor data, asset
tracking, and predictive analytics

Redis also offers a number of built-in features that are natural to find in a
database:

* [**replication**](https://redis.io/topics/replication): high availability and
failover with exact-copy replicas
* [**Lua scripting**](https://redis.io/docs/latest/commands/eval/): enables running
server-side scripts for lowest-latency operations and atomicity
* [**eviction**](https://redis.io/docs/latest/develop/reference/eviction/): LRU,
LFU, TTL, and random data eviction policies to manage memory usage and data
expiration
* [**transactions**](https://redis.io/docs/latest/develop/interact/transactions/):
group commands in a single step guaranteeing serialization, sequential
execution, and recoverability
* [**on-disk persistence**](https://redis.io/docs/latest/operate/oss_and_stack/management/persistence/):
write data to durable storage such as SSDs using a range of persistence options
* [**high availability (Redis Sentinel)**](https://redis.io/docs/latest/operate/oss_and_stack/management/sentinel/):
monitoring, notifications, automatic failover, and configuration providing for
high availability when not using Redis Cluster
* [**partitioning/clustering (Redis Cluster)**](https://redis.io/docs/latest/operate/oss_and_stack/management/scaling/):
Automatically split your dataset among multiple nodes, continue operations
when a subset of nodes fail or are unable to communicate with the rest of the
cluster

Finally, Redis has a wide range of [client libraries for most languages](https://redis.io/docs/latest/develop/clients/) 
so you can use Redis in your language of choice.

Redis is often referred to as a *data structures* server. What this means is
that Redis provides access to mutable data structures via a set of commands,
which are sent using a *server-client* model with TCP sockets and a simple
protocol. So different processes can query and modify the same data structures
in a shared way.

Data structures implemented into Redis have a couple special properties:

* Redis cares to store them on disk, even if they are always served and
modified into the server memory. This means that Redis is fast, but that it is
also non-volatile.
* The implementation of data structures emphasizes memory efficiency, so data
structures inside Redis will likely use less memory compared to the same data
structure modelled using a high-level programming language.

Another good example is to think of Redis as a more complex version of
memcached, where the operations are not just SETs and GETs, but operations that
work with complex data types listed above.
If you want to know more, this is a list of selected starting points:

* The full list of Redis commands. https://redis.io/commands
* The official Redis documentation. https://redis.io/docs

### What is Redis Community Edition?

Redis OSS was renamed Redis Community Edition (CE) with the v7.4 release. Other
Redis variants include:

* [**Redis Software**](https://redis.io/software/): a self-managed software with
additional compliance, reliability, and resiliency for enterprise scaling
* [**Redis Cloud**](https://redis.io/cloud/): a fully
managed service integrated with Google Cloud, Azure, and AWS for
production-ready apps.

For more details, see [which Redis is right for you](https://redis.io/compare/community-edition/).

## Why use Redis?

Redis is a popular choice for developers worldwide due to its combination of
speed, flexibility, and rich feature set. Here's why you might choose Redis for
either an existing project or your next project:

* **Performance**: Because Redis keeps data primarily in memory and uses efficient
data structures, it achieves extremely low latency (often sub-millisecond) for
both read and write operations. This makes it ideal for applications demanding
real-time responsiveness.
* **Flexibility**: Redis isn't just a key-value store, it provides native support
for a wide range of data structures and capabilities listed in
[What is Redis?](#what-is-redis)
* **Extensibility**: Redis is not limited to the built-in data structures, it has a
[modules API](https://redis.io/docs/latest/develop/reference/modules/) that
makes it possible to extend Redis functionality and rapidly implement new
Redis commands
* **Simplicity**: Redis has a simple, text-based protocol and
[well-documented command set](https://redis.io/docs/latest/commands/)
* **Ubiquity**: Redis is battle tested in production workloads at a massive scale.
There is a good chance you indirectly interact with Redis several times daily
* Versatility: Redis is the de facto standard for use cases such as:
  * **Caching**: quickly access frequently used data without needing to query your
  primary database
  * **Session management**: read and write user session data without hurting user
  experience or slowing down every API call
  * **Querying, sorting, and analytics**: perform deduplication, full text search,
  and secondary indexing on in-memory data as fast as possible
  * **Messaging and interservice communication**: job queues, message brokering,
  pub/sub, and streams for communicating between services
  * **Vector operations**: Long-term and short-term LLM memory, RAG content retrieval,
  semantic caching, semantic routing, and vector similarity search

In summary, Redis provides a powerful, fast, and flexible toolkit for solving
a wide variety of data management challenges.

## Getting Started

If you want to get up and running with Redis quickly without needing to build
from source, use one of the following methods:

* [Redis Cloud](https://cloud.redis.io/)
* [Official Redis docker images](https://hub.docker.com/_/redis)
* [Redis quick start guides](https://redis.io/docs/latest/develop/get-started/)

## Drivers

Redis has client drivers for most programming languages:
https://redis.io/docs/latest/develop/clients/

## Learn Redis

* **Documentation**: https://redis.io/docs/
* **Developer Hub**: https://redis.io/learn/
* **Redis University**: https://university.redis.io/

## Cloud managed Redis

https://redis.io/cloud/

## Community

https://redis.io/community/

## Building Redis From Source

This section refers to building Redis from source. If you want to get up and
running with Redis quickly without needing to build from source you can find
the [quick start guides](https://redis.io/docs/latest/develop/get-started/) on
the official Redis docs site.

Redis can be compiled and used on Linux, OSX, OpenBSD, NetBSD, FreeBSD.
We support big endian and little endian architectures, and both 32 bit
and 64-bit systems.

It may compile on Solaris derived systems (for instance SmartOS) but our
support for this platform is *best effort* and Redis is not guaranteed to
work as well as on Linux, OSX, and \*BSD.

It is as simple as:

```bash
make
```

To build with TLS support, you'll need OpenSSL development libraries (e.g.
libssl-dev on Debian/Ubuntu) and run:

```bash
make BUILD_TLS=yes
```

To build with systemd support, you'll need systemd development libraries (such
as libsystemd-dev on Debian/Ubuntu or systemd-devel on CentOS) and run:

```bash
make USE_SYSTEMD=yes
```

To append a suffix to Redis program names, use:

```bash
make PROG_SUFFIX="-alt"
```

You can build a 32 bit Redis binary using:

```bash
make 32bit
```

After building Redis, it is a good idea to test it using:

```bash
make test
```

If TLS is built, running the tests with TLS enabled (you will need `tcl-tls`
installed):

```bash
./utils/gen-test-certs.sh
./runtest --tls
```


### Fixing build problems with dependencies or cached build options

Redis has some dependencies which are included in the `deps` directory.
`make` does not automatically rebuild dependencies even if something in
the source code of dependencies changes.

When you update the source code with `git pull` or when code inside the
dependencies tree is modified in any other way, make sure to use the following
command in order to really clean everything and rebuild from scratch:

```bash
make distclean
```

This will clean: jemalloc, lua, hiredis, linenoise and other dependencies.

Also, if you force certain build options like 32bit target, no C compiler
optimizations (for debugging purposes), and other similar build time options,
those options are cached indefinitely until you issue a `make distclean`
command.

### Fixing problems building 32 bit binaries

If after building Redis with a 32 bit target you need to rebuild it
with a 64 bit target, or the other way around, you need to perform a
`make distclean` in the root directory of the Redis distribution.

In case of build errors when trying to build a 32 bit binary of Redis, try
the following steps:

* Install the package libc6-dev-i386 (also try g++-multilib).
* Try using the following command line instead of `make 32bit`:
  `make CFLAGS="-m32 -march=native" LDFLAGS="-m32"`

### Allocator

Selecting a non-default memory allocator when building Redis is done by setting
the `MALLOC` environment variable. Redis is compiled and linked against libc
malloc by default, except for jemalloc being the default on Linux
systems. This default was picked because jemalloc has proven to have fewer
fragmentation problems than libc malloc.

To force compiling against libc malloc, use:

```bash
make MALLOC=libc
```

To compile against jemalloc on Mac OS X systems, use:

```bash
make MALLOC=jemalloc
```

### Monotonic clock

By default, Redis will build using the POSIX clock_gettime function as the
monotonic clock source.  On most modern systems, the internal processor clock
can be used to improve performance.  Cautions can be found here:
    http://oliveryang.net/2015/09/pitfalls-of-TSC-usage/

To build with support for the processor's internal instruction clock, use:

```bash
make CFLAGS="-DUSE_PROCESSOR_CLOCK"
```

### Verbose build

Redis will build with a user-friendly colorized output by default.
If you want to see a more verbose output, use the following:

```bash
make V=1
```

### Running Redis

To run Redis with the default configuration, just type:

```bash
cd src
./redis-server
```

If you want to provide your redis.conf, you have to run it using an additional
parameter (the path of the configuration file):

```bash
cd src
./redis-server /path/to/redis.conf
```

It is possible to alter the Redis configuration by passing parameters directly
as options using the command line. Examples:

```bash
./redis-server --port 9999 --replicaof 127.0.0.1 6379
./redis-server /etc/redis/6379.conf --loglevel debug
```

All the options in redis.conf are also supported as options using the command
line, with exactly the same name.

#### Running Redis with TLS

Please consult the [TLS.md](TLS.md) file for more information on
how to use Redis with TLS.

### Playing with Redis

You can use redis-cli to play with Redis. Start a redis-server instance,
then in another terminal try the following:

```bash
cd src
./redis-cli
redis> ping
PONG
redis> set foo bar
OK
redis> get foo
"bar"
redis> incr mycounter
(integer) 1
redis> incr mycounter
(integer) 2
redis>
```

You can find the list of all the available commands at https://redis.io/commands.

### Installing Redis

In order to install Redis binaries into `/usr/local/bin`, just use:

```bash
make install
```

You can use `make PREFIX=/some/other/directory install` if you wish to use a
different destination.

`make install` will just install binaries in your system, but will not configure
init scripts and configuration files in the appropriate place. This is not
needed if you just want to play a bit with Redis, but if you are installing
it the proper way for a production system, we have a script that does this
for Ubuntu and Debian systems:

```bash
cd utils
./install_server.sh
```

_Note_: `install_server.sh` will not work on Mac OSX; it is built for Linux only.

The script will ask you a few questions and will set up everything you need
to run Redis properly as a background daemon that will start again on
system reboots.

You'll be able to stop and start Redis using the script named
`/etc/init.d/redis_<portnumber>`, for instance `/etc/init.d/redis_6379`.

### Code contributions

By contributing code to the Redis project in any form, including sending a pull
request via GitHub, a code fragment or patch via private email or public
discussion groups, you agree to release your code under the terms of the
[Redis Software Grant and Contributor License Agreement][1]. Redis software
contains contributions to the original Redis core project, which are owned by
their contributors and licensed under the 3BSD license. Any copy of that
license in this repository applies only to those contributions. Redis releases
all Redis Community Edition versions from 7.4.x and thereafter under the
RSALv2/SSPL dual-license as described in the [LICENSE.txt][2] file included in
the Redis Community Edition source distribution.

Please see the [CONTRIBUTING.md][1] file in this source distribution for more
information. For security bugs and vulnerabilities, please see
[SECURITY.md][3].

[1]: https://github.com/redis/redis/blob/unstable/CONTRIBUTING.md
[2]: https://github.com/redis/redis/blob/unstable/LICENSE.txt
[3]: https://github.com/redis/redis/blob/unstable/SECURITY.md

## Redis Trademarks

The purpose of a trademark is to identify the goods and services of a person
or company without causing confusion. As the registered owner of its name and
logo, Redis accepts certain limited uses of its trademarks, but it has
requirements that must be followed as described in its Trademark Guidelines
available at: https://redis.com/legal/trademark-guidelines/.

## Redis internals

Please see the [INTERNALS.md](./INTERNALS.md) file for more information about
the Redis source code layout.

Enjoy!
