[![codecov](https://codecov.io/github/redis/redis/graph/badge.svg?token=6bVHb5fRuz)](https://codecov.io/github/redis/redis)

This README is just a fast *quick start* document. You can find more detailed documentation at [redis.io](https://redis.io).

### What is Redis?

For developers, who are building real-time data-driven applications, Redis is the preferred, fastest, and most feature-rich cache, data structure server, and document and vector query engine.

Redis covers a wide range of use cases across a wide range of industries and projects, serving as
- **A cache**, supporting multiple key eviction policies, key expiration, and hash-field expiration
- **A distributed session store**, supporting multiple session data modeling options (string, JSON, hash).
- **A data structure server**: low-level data structures (string, JSON, list, hash, set, sorted set, bitmap, bitfield, and more - see full list below) with high-level semantics (counter, stack, queue, priority queue, rate limiter, leaderboard, ...), and with transactions and scripting support.
- **A NoSQL data store**: key-value, document, and time series.
- **A secondary index and a search and query engine**: with Redis Query Engine users can define indexes for hash and JSON documents, and use a rich query language for vector search, full-text search, geospatial queries, ranking, and aggregations.
- **A distributed event store, stream-processing platform, and message broker**: queue (list), priority queue (sorted set), event deduplication (set), streams, and pub/sub, with probabilistic stream processing capabilities.
- **A vector store**, integrated with GenAI applications and ecosystems (e.g., LangGraph, mem0), providing short-term memory (checkpointers), long-term memory (store), LLM response caching (LLM semantic cache), and context retrieval (RAG applications).
- **Real time analytics**, including personalization, recommendations, fraud and anomaly detection, and risk assessment.


Redis can be relied upon (it is robust and has a well-defined behavior), it comes with a long-term commitment (we keep maintaining Redis, avoid introducing breaking changes, and keep it innovative and competitive). Redis is fast and has a low memory footprint (with the right tradeoffs), easy to understand, learn, and use, and easy to adopt across a wide range of development environments and languages.

If you want to know more, here is a list of starting points:

- Introduction to Redis data types - https://redis.io/docs/latest/develop/data-types/
- The full list of Redis commands - https://redis.io/commands
- Redis for AI - https://redis.io/docs/latest/develop/ai/
- and much more - https://redis.io/documentation

### What is Redis Open Source?

Redis Community Edition (Redis CE) was renamed Redis Open Source with the v8.0 release.

Redis Ltd. also offers [Redis Software](https://redis.io/enterprise/), a self-managed software with additional compliance, reliability, and resiliency for enterprise scaling,
and [Redis Cloud](https://redis.io/cloud/), a fully managed service integrated with Google Cloud, Azure, and AWS for production-ready apps.

Read more about the differences between Redis Open Source and Redis [here](https://redis.io/technology/advantages/).

### Redis Open Source - binary distributions

The fastest way to deploy Redis is using one the binary distributions:

- Alpine and Debian Docker images - https://hub.docker.com/_/redis
- Install using snap - see https://github.com/redis/redis-snap
- Install using brew - see https://github.com/redis/homebrew-redis
- Install using RPM - see https://github.com/redis/redis-rpm
- Install using Debian APT - see https://github.com/redis/redis-debian

If you prefer to build Redis from source - see instructions below.

### Using Redis with redis-cli

`redis-cli` is Redis' command line interface. It is available as part of all the binary distributions and when you build Redis from source.

See https://redis.io/docs/latest/develop/tools/cli/

You can start a redis-server instance, and then, in another terminal try the following:

```
% cd src
% ./redis-cli
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

### Using Redis with Redis Insight
For a more visual and user-friendly experience, use Redis Insight - a tool that lets you explore data, design, develop, and optimize your applications while also serving as a platform for Redis education and onboarding. Redis Insight integrates Redis Copilot, a natural language AI assistant that improves the experience when working with data and commands.
See https://redis.io/docs/latest/develop/tools/insight/ and https://github.com/RedisInsight/RedisInsight.


### Using Redis with client libraries

To connect your application to Redis, you will need a client library. The list of client libraries is available in https://redis.io/docs/latest/develop/clients/.

### Redis Data types, processing engines, and capabilities

Redis provides a variety of data types, processing engines, and capabilities to support a wide range of use cases:

1. **String**
    - Strings store sequences of bytes, including text, serialized objects, and binary arrays. As such, strings are the simplest type of value you can associate with a Redis key.
    - [Documentation: Strings](https://redis.io/docs/latest/develop/data-types/strings)

1. **JSON** (*)
    - The JSON data type provides JavaScript Object Notation (JSON) support for Redis. It lets you store, retrieve, and update JSON documents. A JSON document can be queried and manipulated using JSONPath expressions. JSON also works seamlessly with the Redis Query Engine to let you index and query JSON documents.
    - [Documentation: JSON quick start](https://redis.io/docs/latest/develop/data-types/json/#use-redisjson)

1. **Hash**
    - A hash is a collection of fields, each field is a name-value string pair. You can use hashes to represent flat objects and to store groupings of counters, among other things. Expiration time or a time-to-live can be set for each hash field.
    - [Documentation: Hashes](https://redis.io/docs/latest/develop/data-types/hashes)

1. **Redis Query Engine**  (*)
    - The Redis Query Engine allows you to use Redis as a document database, a vector database, a secondary index, and a search engine. With Redis Query Engine, users can define indexes for hash and JSON documents, and use a rich query language for vector search, full-text search, geospatial queries, and aggregations.
    - [Documentation: Redis Query Engine](https://redis.io/docs/latest/develop/interact/search-and-query/)

1. **List**
    - A list is a list of strings, sorted by insertion order. They are great for stacks, queues, and for queue management and worker systems.
    - [Documentation: Lists](https://redis.io/docs/latest/develop/data-types/lists)

1. **Set**
    - A set is an unordered collection of unique strings (members). Sets can be used to track unique items (e.g., track all unique IP addresses accessing a given blog post), represent relations (e.g., the set of all users with a given role). Redis also supports common set operations such as intersection, unions, and differences.
    - [Documentation: Sets](https://redis.io/docs/latest/develop/data-types/sets)

1. **Sorted Set**
    - A sorted set is similar to a set, but each member is associated with a score. When more than one member has the same score, the members are ordered lexicographically. Some use cases for sorted sets include leaderboards and sliding-window rate limiters.
    - [Documentation: Sorted Sets](https://redis.io/docs/latest/develop/data-types/sorted-sets)

1. **Vector Set** (beta)
    - Vector set is a data type similar to a sorted set, but instead of a score, each member is associated with a vector embedding. You can add items to a vector set, and then retrieve the members that are the most similar to a specified vector embedding, or to the vector embedding of an existing member.
    -  [Documentation: Vector sets](https://redis.io/docs/latest/develop/data-types/vector-sets)

1. **Geospatial Index**
    - Geospatial indexes let you store coordinates and search for them. This data structure is useful for finding nearby points within a given radius or bounding box.
    - [Documentation: Geo](https://redis.io/docs/latest/develop/data-types/geospatial/)

1. **Bitmap**
    - Bitmaps provide bit-oriented operations on a bit vector. You can get bits, set bits, and perform bitwise operations between bitmaps. Bitmaps are often used for efficient management of memberships or permissions, where each bit represents a particular member.
    - [Documentation: Bitmap](https://redis.io/docs/latest/develop/data-types/bitmaps/)

1. **Bitfield**
    - Bitfields let you set, increment, and get integer values of arbitrary bit length - from unsigned 1-bit integers to signed 63-bit integers. Bitfields are often used for efficient management of arrays of limited-range counters or numerical values.
    - [Documentation: Bitfield](https://redis.io/docs/latest/develop/data-types/bitfields/)

1. **HyperLogLog**
    - HyperLogLog is a probabilistic data structure used for approximating the cardinality of a stream (i.e., the number of unique elements).
    - [Documentation: HyperLogLog](https://redis.io/docs/latest/develop/data-types/hyperloglogs)

1. **Bloom filter**  (*)
    - Bloom filter is a probabilistic data structure used for checking if a given value is present in a stream.
    - [Documentation: Bloom filter](https://redis.io/docs/latest/develop/data-types/probabilistic/bloom-filter/)

1. **Cuckoo filter**  (*)
    - Cuckoo filter, just like Bloom filter, is a probabilistic data structure for checking if a given value is present in a stream, while also allowing limited counting and deletions.
    - [Documentation: Cuckoo filter](https://redis.io/docs/latest/develop/data-types/probabilistic/cuckoo-filter/)

1. **Top-k**  (*)
    - Top-k is a probabilistic data structure used for tracking the most frequent values in a data stream.
    - [Documentation: Top-k](https://redis.io/docs/latest/develop/data-types/probabilistic/top-k/)

1. **Count-min sketch**  (*)
    - Count-Min Sketch is a probabilistic data structure used for estimating how many times a given value appears in the data stream.
    - [Documentation: Count-min sketch](https://redis.io/docs/latest/develop/data-types/probabilistic/count-min-sketch/)

1. **t-digest**  (*)
    - t-digest is a probabilistic data structure used for estimating which fraction / how many values in a data stream are smaller than a given value, which value is smaller than p percent of the values in a data stream, or what are the smallest/largest values in the data stream.
    - [Documentation: t-digeset](https://redis.io/docs/latest/develop/data-types/probabilistic/t-digest/)

1. **Time series**  (*)
    - a time series allows storing and querying timetagged data points (samples).
    - [Documentation: Time series quick start](https://redis.io/docs/latest/develop/data-types/timeseries/quickstart/)

1. **Pub/Sub**
    - Pub/Sub (short for publish/subscribe) is a lightweight messaging capability. Publishers send messages to a channel, and subscribers receive messages from that channel.
    - [Documentation: Redis streams](https://redis.io/docs/latest/develop/interact/pubsub/)

1. **Stream**
    - A stream is a data structure that acts like an append-only log. Each stream entry consists of name-value string pairs, similar to a hash. Streams support multiple consumption strategies, where consumers can pop entries, consume entries by range, or listen to entries. Consumer groups allow multiple workers to retrieve different stream entries in order to scale message processing.
    - [Documentation: Redis streams](https://redis.io/docs/latest/develop/data-types/streams/)

1. **Transactions**
    - A transaction allows the execution of a group of commands in a single step. A request sent by another client will never be served in the middle of the execution of a transaction. This guarantees that the commands are executed as a single isolated operation.
    - [Documentation: Transactions](https://redis.io/docs/latest/develop/interact/transactions/)

1. **Programmability**
    - Users can upload and execute Lua scripts on the server. Scripts can employ programmatic control structures and use most of the commands while executing to access the database. Because scripts are executed on the server, reading and writing data from scripts is very efficient. Functions provide the same core functionality as scripts but are first-class software artifacts of the database. Redis manages functions as an integral part of the database and ensures their availability via data persistence and replication.
    - [Documentation: Scripting with Lua](https://redis.io/docs/latest/develop/interact/programmability/eval-intro/)
    - [Documentation: Functions](https://redis.io/docs/latest/develop/interact/programmability/functions-intro/)

Items marked with (*) require building with `BUILD_WITH_MODULES=yes`.

### Build and run Redis with all data structures - Ubuntu 20.04 (Focal)

Tested with the following Docker images:
- ubuntu:20.04

1. Install required dependencies

   Update your package lists and install the necessary development tools and libraries:

   ```
   apt-get update
   apt-get install -y sudo
   sudo apt-get install -y --no-install-recommends ca-certificates wget dpkg-dev gcc g++ libc6-dev libssl-dev make git python3 python3-pip python3-venv python3-dev unzip rsync clang automake autoconf gcc-10 g++-10 libtool
   ```

2. Use GCC 10 as the default compiler

   Update the system's default compiler to GCC 10:

   ```
   sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100 --slave /usr/bin/g++ g++ /usr/bin/g++-10
   ```

3. Install CMake

   Install CMake using `pip3` and link it for system-wide access:

   ```
   pip3 install cmake==3.31.6
   sudo ln -sf /usr/local/bin/cmake /usr/bin/cmake
   cmake --version
   ```

   Note: CMake version 3.31.6 is the latest supported version. Newer versions cannot be used.

4. Download the Redis source

   Download a specific version of the Redis source code archive from GitHub.

   Replace `<version>` with the Redis version, for example: `8.0.0`.

   ```
   cd /usr/src
   wget -O redis-<version>.tar.gz https://github.com/redis/redis/archive/refs/tags/<version>.tar.gz
   ```

5. Extract the source archive

   Create a directory for the source code and extract the contents into it:

   ```
   cd /usr/src
   tar xvf redis-<version>.tar.gz
   rm redis-<version>.tar.gz
   ```

6. Build Redis

   Set the necessary environment variables and compile Redis:

   ```
   cd /usr/src/redis-<version>
   export BUILD_TLS=yes BUILD_WITH_MODULES=yes INSTALL_RUST_TOOLCHAIN=yes DISABLE_WERRORS=yes
   make -j "$(nproc)" all
   ```

7. Run Redis

   ```
   cd /usr/src/redis-<version>
   ./src/redis-server redis-full.conf
    ```

### Build and run Redis with all data structures - Ubuntu 22.04 (Jammy) / 24.04 (Noble)

Tested with the following Docker image:
- ubuntu:22.04
- ubuntu:24.04

1. Install required dependencies

   Update your package lists and install the necessary development tools and libraries:

   ```
   apt-get update
   apt-get install -y sudo
   sudo apt-get install -y --no-install-recommends ca-certificates wget dpkg-dev gcc g++ libc6-dev libssl-dev make git cmake python3 python3-pip python3-venv python3-dev unzip rsync clang automake autoconf libtool
   ```

2. Download the Redis source

   Download a specific version of the Redis source code archive from GitHub.

   Replace `<version>` with the Redis version, for example: `8.0.0`.

   ```
   cd /usr/src
   wget -O redis-<version>.tar.gz https://github.com/redis/redis/archive/refs/tags/<version>.tar.gz
   ```

3. Extract the source archive

   Create a directory for the source code and extract the contents into it:

   ```
   cd /usr/src
   tar xvf redis-<version>.tar.gz
   rm redis-<version>.tar.gz
   ```

4. Build Redis

   Set the necessary environment variables and build Redis:

   ```
   cd /usr/src/redis-<version>
   export BUILD_TLS=yes BUILD_WITH_MODULES=yes INSTALL_RUST_TOOLCHAIN=yes DISABLE_WERRORS=yes
   make -j "$(nproc)" all
   ```

5. Run Redis

   ```
   cd /usr/src/redis-<version>
   ./src/redis-server redis-full.conf
   ```

### Build and run Redis with all data structures - Debian 11 (Bullseye) / 12 (Bookworm)


Tested with the following Docker images:
- debian:bullseye
- debian:bullseye-slim
- debian:bookworm
- debian:bookworm-slim

1. Install required dependencies

   Update your package lists and install the necessary development tools and libraries:

   ```
   apt-get update
   apt-get install -y sudo
   sudo apt-get install -y --no-install-recommends ca-certificates wget dpkg-dev gcc g++ libc6-dev libssl-dev make git cmake python3 python3-pip python3-venv python3-dev unzip rsync clang automake autoconf libtool
   ```

2. Download the Redis source

   Download a specific version of the Redis source code archive from GitHub.

   Replace `<version>` with the Redis version, for example: `8.0.0`.

   ```
   cd /usr/src
   wget -O redis-<version>.tar.gz https://github.com/redis/redis/archive/refs/tags/<version>.tar.gz
   ```

3. Extract the source archive

   Create a directory for the source code and extract the contents into it:

   ```
   cd /usr/src
   tar xvf redis-<version>.tar.gz
   rm redis-<version>.tar.gz
   ```

4. Build Redis

   Set the necessary environment variables and build Redis:

   ```
   cd /usr/src/redis-<version>
   export BUILD_TLS=yes BUILD_WITH_MODULES=yes INSTALL_RUST_TOOLCHAIN=yes DISABLE_WERRORS=yes
   make -j "$(nproc)" all
   ```

5. Run Redis


   ```
   cd /usr/src/redis-<version>
   ./src/redis-server redis-full.conf
   ```

### Build and run Redis with all data structures - AlmaLinux 8.10 / Rocky Linux 8.10

Tested with the following Docker images:
- almalinux:8.10
- almalinux:8.10-minimal
- rockylinux/rockylinux:8.10
- rockylinux/rockylinux:8.10-minimal

1. Prepare the system

   For 8.10-minimal, install `sudo` and `dnf` as follows:

   ```
   microdnf install dnf sudo -y
   ```

   For 8.10 (regular), install sudo as follows:

   ```
   dnf install sudo -y
   ```

   Clean the package metadata, enable required repositories, and install development tools:

   ```
   sudo dnf clean all
   sudo tee /etc/yum.repos.d/goreleaser.repo > /dev/null <<EOF
   [goreleaser]
   name=GoReleaser
   baseurl=https://repo.goreleaser.com/yum/
   enabled=1
   gpgcheck=0
   EOF
   sudo dnf update -y
   sudo dnf groupinstall "Development Tools" -y
   sudo dnf config-manager --set-enabled powertools
   sudo dnf install -y epel-release
   ```

2. Install required dependencies

   Update your package lists and install the necessary development tools and libraries:

   ```
   sudo dnf install -y --nobest --skip-broken pkg-config wget gcc-toolset-13-gcc gcc-toolset-13-gcc-c++ git make openssl openssl-devel python3.11 python3.11-pip python3.11-devel unzip rsync clang curl libtool automake autoconf jq systemd-devel
   ```

   Create a Python virtual environment:

   ```
   python3.11 -m venv /opt/venv
   ```

   Enable the GCC toolset:

   ```
   sudo cp /opt/rh/gcc-toolset-13/enable /etc/profile.d/gcc-toolset-13.sh
   echo "source /etc/profile.d/gcc-toolset-13.sh" | sudo tee -a /etc/bashrc
   ```

3. Install CMake

   Install CMake 3.25.1 manually:

   ```
   CMAKE_VERSION=3.25.1
   ARCH=$(uname -m)
   if [ "$ARCH" = "x86_64" ]; then
     CMAKE_FILE=cmake-${CMAKE_VERSION}-linux-x86_64.sh
   else
     CMAKE_FILE=cmake-${CMAKE_VERSION}-linux-aarch64.sh
   fi
   wget https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/${CMAKE_FILE}
   chmod +x ${CMAKE_FILE}
   ./${CMAKE_FILE} --skip-license --prefix=/usr/local --exclude-subdir
   rm ${CMAKE_FILE}
   cmake --version
   ```

4. Download the Redis source

   Download a specific version of the Redis source code archive from GitHub.

   Replace `<version>` with the Redis version, for example: `8.0.0`.

   ```
   cd /usr/src
   wget -O redis-<version>.tar.gz https://github.com/redis/redis/archive/refs/tags/<version>.tar.gz
   ```

5. Extract the source archive

   Create a directory for the source code and extract the contents into it:

   ```
   cd /usr/src
   tar xvf redis-<version>.tar.gz
   rm redis-<version>.tar.gz
   ```

6. Build Redis

   Enable the GCC toolset, set the necessary environment variables, and build Redis:

   ```
   source /etc/profile.d/gcc-toolset-13.sh
   cd /usr/src/redis-<version>
   export BUILD_TLS=yes BUILD_WITH_MODULES=yes INSTALL_RUST_TOOLCHAIN=yes DISABLE_WERRORS=yes
   make -j "$(nproc)" all
   ```

7. Run Redis

   ```
   cd /usr/src/redis-<version>
   ./src/redis-server redis-full.conf
   ```

### Build and run Redis with all data structures - AlmaLinux 9.5 / Rocky Linux 9.5

Tested with the following Docker images:
- almalinux:9.5
- almalinux:9.5-minimal
- rockylinux/rockylinux:9.5
- rockylinux/rockylinux:9.5-minimal

1. Prepare the system

   For 9.5-minimal, install `sudo` and `dnf` as follows:

   ```
   microdnf install dnf sudo -y
   ```

   For 9.5 (regular), install sudo as follows:

   ```
   dnf install sudo -y
   ```

   Clean the package metadata, enable required repositories, and install development tools:

   ```
   sudo tee /etc/yum.repos.d/goreleaser.repo > /dev/null <<EOF
   [goreleaser]
   name=GoReleaser
   baseurl=https://repo.goreleaser.com/yum/
   enabled=1
   gpgcheck=0
   EOF
   sudo dnf clean all
   sudo dnf makecache
   sudo dnf update -y
   ```

2. Install required dependencies

   Update your package lists and install the necessary development tools and libraries:

   ```
   sudo dnf install -y --nobest --skip-broken pkg-config xz wget which gcc-toolset-13-gcc gcc-toolset-13-gcc-c++ git make openssl openssl-devel python3 python3-pip python3-devel unzip rsync clang curl libtool automake autoconf jq systemd-devel
   ```

   Create a Python virtual environment:

   ```
   python3 -m venv /opt/venv
   ```

   Enable the GCC toolset:

   ```
   sudo cp /opt/rh/gcc-toolset-13/enable /etc/profile.d/gcc-toolset-13.sh
   echo "source /etc/profile.d/gcc-toolset-13.sh" | sudo tee -a /etc/bashrc
   ```

3. Install CMake

   Install CMake 3.25.1 manually:

   ```
   CMAKE_VERSION=3.25.1
   ARCH=$(uname -m)
   if [ "$ARCH" = "x86_64" ]; then
     CMAKE_FILE=cmake-${CMAKE_VERSION}-linux-x86_64.sh
   else
     CMAKE_FILE=cmake-${CMAKE_VERSION}-linux-aarch64.sh
   fi
   wget https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/${CMAKE_FILE}
   chmod +x ${CMAKE_FILE}
   ./${CMAKE_FILE} --skip-license --prefix=/usr/local --exclude-subdir
   rm ${CMAKE_FILE}
   cmake --version
   ```

4. Download the Redis source

   Download a specific version of the Redis source code archive from GitHub.

   Replace `<version>` with the Redis version, for example: `8.0.0`.

   ```
   cd /usr/src
   wget -O redis-<version>.tar.gz https://github.com/redis/redis/archive/refs/tags/<version>.tar.gz
   ```

5. Extract the source archive

   Create a directory for the source code and extract the contents into it:

   ```
   cd /usr/src
   tar xvf redis-<version>.tar.gz
   rm redis-<version>.tar.gz
   ```

6. Build Redis

   Enable the GCC toolset, set the necessary environment variables, and build Redis:

   ```
   source /etc/profile.d/gcc-toolset-13.sh
   cd /usr/src/redis-<version>
   export BUILD_TLS=yes BUILD_WITH_MODULES=yes INSTALL_RUST_TOOLCHAIN=yes DISABLE_WERRORS=yes
   make -j "$(nproc)" all
   ```

7. Run Redis

   ```
   cd /usr/src/redis-<version>
   ./src/redis-server redis-full.conf
   ```

### Build and run Redis with all data structures - macOS 13 (Ventura) and macOS 14 (Sonoma)

1. Install Homebrew

   If Homebrew is not already installed, follow the installation instructions on the [Homebrew home page](https://brew.sh/).

2. Install required packages

   ```
   export HOMEBREW_NO_AUTO_UPDATE=1
   brew update
   brew install coreutils
   brew install make
   brew install openssl
   brew install llvm@18
   brew install cmake
   brew install gnu-sed
   brew install automake
   brew install libtool
   brew install wget
    ```

3. Install Rust

   Rust is required to build the JSON package.

   ```
   RUST_INSTALLER=rust-1.80.1-$(if [ "$(uname -m)" = "arm64" ]; then echo "aarch64"; else echo "x86_64"; fi)-apple-darwin
   wget --quiet -O ${RUST_INSTALLER}.tar.xz https://static.rust-lang.org/dist/${RUST_INSTALLER}.tar.xz
   tar -xf ${RUST_INSTALLER}.tar.xz
   (cd ${RUST_INSTALLER} && sudo ./install.sh)
   ```

4. Download the Redis source

   Download a specific version of the Redis source code archive from GitHub.

   Replace `<version>` with the Redis version, for example: `8.0.0`.

   ```
   cd ~/src
   wget -O redis-<version>.tar.gz https://github.com/redis/redis/archive/refs/tags/<version>.tar.gz
   ```

5. Extract the source archive

   Create a directory for the source code and extract the contents into it:

   ```
   cd ~/src
   tar xvf redis-<version>.tar.gz
   rm redis-<version>.tar.gz
   ```

6. Build Redis

   ```
   cd ~/src/redis-<version>
   export HOMEBREW_PREFIX="$(brew --prefix)"
   export BUILD_WITH_MODULES=yes
   export BUILD_TLS=yes
   export DISABLE_WERRORS=yes
   PATH="$HOMEBREW_PREFIX/opt/libtool/libexec/gnubin:$HOMEBREW_PREFIX/opt/llvm@18/bin:$HOMEBREW_PREFIX/opt/make/libexec/gnubin:$HOMEBREW_PREFIX/opt/gnu-sed/libexec/gnubin:$HOMEBREW_PREFIX/opt/coreutils/libexec/gnubin:$PATH"
   export LDFLAGS="-L$HOMEBREW_PREFIX/opt/llvm@18/lib"
   export CPPFLAGS="-I$HOMEBREW_PREFIX/opt/llvm@18/include"
   mkdir -p build_dir/etc
   make -C redis-8.0 -j "$(nproc)" all OS=macos
   make -C redis-8.0 install PREFIX=$(pwd)/build_dir OS=macos
   ```

7. Run Redis

   ```
   export LC_ALL=en_US.UTF-8
   export LANG=en_US.UTF-8
   build_dir/bin/redis-server redis-full.conf
   ```

### Build and run Redis with all data structures - macOS 15 (Sequoia)

Support and instructions will be provided at a later date.

### Building Redis - flags and general notes

Redis can be compiled and used on Linux, OSX, OpenBSD, NetBSD, FreeBSD.
We support big endian and little endian architectures, and both 32 bit and 64 bit systems.

It may compile on Solaris derived systems (for instance SmartOS) but our support for this platform is *best effort* and Redis is not guaranteed to work as well as in Linux, OSX, and \*BSD.

To build Redis with all the data structures (including JSON, time series, Bloom filter, cuckoo filter, count-min sketch, top-k, and t-digest) and with Redis Query Engine, make sure first that all the prerequisites are installed (see build instructions above, per operating system). You need to use the following flag in the make command:


```
make BUILD_WITH_MODULES=yes
```


To build Redis with just the core data structures, use:

```
make
```


To build with TLS support, you need OpenSSL development libraries (e.g. libssl-dev on Debian/Ubuntu) and the following flag in the make command:

```
make BUILD_TLS=yes
```


To build with systemd support, you need systemd development libraries (such as libsystemd-dev on Debian/Ubuntu or systemd-devel on CentOS), and the following flag:

```
make USE_SYSTEMD=yes
```


To append a suffix to Redis program names, add the following flag:

```
make PROG_SUFFIX="-alt"
```


You can build a 32 bit Redis binary using:

```
make 32bit
```


After building Redis, it is a good idea to test it using:

```
make test
```

If TLS is built, running the tests with TLS enabled (you will need `tcl-tls` installed):

```
./utils/gen-test-certs.sh
./runtest --tls
```

### Fixing build problems with dependencies or cached build options

Redis has some dependencies which are included in the `deps` directory. `make` does not automatically rebuild dependencies even if something in the source code of dependencies changes.

When you update the source code with `git pull` or when code inside the dependencies tree is modified in any other way, make sure to use the following command in order to really clean everything and rebuild from scratch:

```
make distclean
```


This will clean: jemalloc, lua, hiredis, linenoise and other dependencies.

Also if you force certain build options like 32bit target, no C compiler optimizations (for debugging purposes), and other similar build time options, those options are cached indefinitely until you issue a `make distclean`
command.

### Fixing problems building 32 bit binaries

If after building Redis with a 32 bit target you need to rebuild it
with a 64 bit target, or the other way around, you need to perform a `make distclean` in the root directory of the Redis distribution.

In case of build errors when trying to build a 32 bit binary of Redis, try the following steps:

* Install the package libc6-dev-i386 (also try g++-multilib).
* Try using the following command line instead of `make 32bit`:
  `make CFLAGS="-m32 -march=native" LDFLAGS="-m32"`

### Allocator

Selecting a non-default memory allocator when building Redis is done by setting the `MALLOC` environment variable. Redis is compiled and linked against libc malloc by default, with the exception of jemalloc being the default on Linux systems. This default was picked because jemalloc has proven to have fewer fragmentation problems than libc malloc.

To force compiling against libc malloc, use:

```
make MALLOC=libc
```

To compile against jemalloc on Mac OS X systems, use:

```
make MALLOC=jemalloc
```

### Monotonic clock

By default, Redis will build using the POSIX clock_gettime function as the monotonic clock source.  On most modern systems, the internal processor clock can be used to improve performance.  Cautions can be found here: http://oliveryang.net/2015/09/pitfalls-of-TSC-usage/

To build with support for the processor's internal instruction clock, use:

```
make CFLAGS="-DUSE_PROCESSOR_CLOCK"
```

### Verbose build

Redis will build with a user-friendly colorized output by default.
If you want to see a more verbose output, use the following:

```
make V=1
```

### Running Redis with TLS

Please consult the [TLS.md](TLS.md) file for more information on how to use Redis with TLS.

### Code contributions

By contributing code to the Redis project in any form, including sending a pull request via GitHub, a code fragment or patch via private email or public discussion groups, you agree to release your code under the terms of the Redis Software Grant and Contributor License Agreement. Please see the CONTRIBUTING.md file in this source distribution for more information. For security bugs and vulnerabilities, please see SECURITY.md. Open Source Redis releases are subject to the following licenses:

1. Version 7.2.x and prior releases are subject to BSDv3. These contributions to the original Redis core project are owned by their contributors and licensed under the 3BSDv3 license as referenced in the REDISCONTRIBUTIONS.txt file. Any copy of that license in this repository applies only to those contributions;

2. Versions 7.4.x to 7.8.x are subject to your choice of RSALv2 or SSPLv1; and

3. Version 8.0.x and subsequent releases are subject to the tri-license RSALv2/SSPLv1/AGPLv3 at your option as referenced in the LICENSE.txt file.

### Redis Trademarks

The purpose of a trademark is to identify the goods and services of a person or company without causing confusion. As the registered owner of its name and logo, Redis accepts certain limited uses of its trademarks but it has requirements that must be followed as described in its Trademark Guidelines available at: https://redis.io/legal/trademark-policy/.
