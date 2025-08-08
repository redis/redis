[![codecov](https://codecov.io/github/redis/redis/graph/badge.svg?token=6bVHb5fRuz)](https://codecov.io/github/redis/redis)

This document serves as both a quick start guide to Redis and a detailed resource for building it from source.

- New to Redis? Start with [What is Redis](#what-is-redis) and [Getting Started](#getting-started)
- Ready to build from source? Jump to [Build Redis from Source](#build-redis-from-source)
- Want to contribute? See the [Code contributions](#code-contributions) section
  and [CONTRIBUTING.md](./CONTRIBUTING.md)
- Looking for detailed documentation? Navigate to [redis.io/docs](https://redis.io/docs/)

## Table of contents

- [What is Redis?](#what-is-redis)
  - [Key use cases](#key-use-cases)
- [Why choose Redis?](#why-choose-redis)
- [What is Redis Open Source?](#what-is-redis-open-source)
- [Getting started](#getting-started)
  - [Redis starter projects](#redis-starter-projects)
  - [Using Redis with client libraries](#using-redis-with-client-libraries)
  - [Using Redis with redis-cli](#using-redis-with-redis-cli)
  - [Using Redis with Redis Insight](#using-redis-with-redis-insight)
- [Redis data types, processing engines, and capabilities](#redis-data-types-processing-engines-and-capabilities)
- [Community](#community)
- [Build Redis from source](#build-redis-from-source)
  - [Build and run Redis with all data structures - Ubuntu 20.04 (Focal)](#build-and-run-redis-with-all-data-structures---ubuntu-2004-focal)
  - [Build and run Redis with all data structures - Ubuntu 22.04 (Jammy) / 24.04 (Noble)](#build-and-run-redis-with-all-data-structures---ubuntu-2204-jammy--2404-noble)
  - [Build and run Redis with all data structures - Debian 11 (Bullseye) / 12 (Bookworm)](#build-and-run-redis-with-all-data-structures---debian-11-bullseye--12-bookworm)
  - [Build and run Redis with all data structures - AlmaLinux 8.10 / Rocky Linux 8.10](#build-and-run-redis-with-all-data-structures---almalinux-810--rocky-linux-810)
  - [Build and run Redis with all data structures - AlmaLinux 9.5 / Rocky Linux 9.5](#build-and-run-redis-with-all-data-structures---almalinux-95--rocky-linux-95)
  - [Build and run Redis with all data structures - macOS 13 (Ventura) and macOS 14 (Sonoma)](#build-and-run-redis-with-all-data-structures---macos-13-ventura-and-macos-14-sonoma)
  - [Build and run Redis with all data structures - macOS 15 (Sequoia)](#build-and-run-redis-with-all-data-structures---macos-15-sequoia)
  - [Building Redis - flags and general notes](#building-redis---flags-and-general-notes)
  - [Fixing build problems with dependencies or cached build options](#fixing-build-problems-with-dependencies-or-cached-build-options)
  - [Fixing problems building 32 bit binaries](#fixing-problems-building-32-bit-binaries)
  - [Allocator](#allocator)
  - [Monotonic clock](#monotonic-clock)
  - [Verbose build](#verbose-build)
  - [Running Redis with TLS](#running-redis-with-tls)
- [Code contributions](#code-contributions)
- [Redis Trademarks](#redis-trademarks)

## What is Redis?

For developers, who are building real-time data-driven applications, Redis is the preferred, fastest, and most feature-rich cache, data structure server, and document and vector query engine.

### Key use cases

Redis excels in various applications, including:

- **Caching:** Supports multiple eviction policies, key expiration, and hash-field expiration.
- **Distributed Session Store:** Offers flexible session data modeling (string, JSON, hash).
- **Data Structure Server:** Provides low-level data structures (strings, lists, sets, hashes, sorted sets, JSON, etc.) with high-level semantics (counters, queues, leaderboards, rate limiters) and supports transactions & scripting.
- **NoSQL Data Store:** Key-value, document, and time series data storage.
- **Search and Query Engine:** Indexing for hash/JSON documents, supporting vector search, full-text search, geospatial queries, ranking, and aggregations via Redis Query Engine.
- **Event Store & Message Broker:** Implements queues (lists), priority queues (sorted sets), event deduplication (sets), streams, and pub/sub with probabilistic stream processing capabilities.
- **Vector Store for GenAI:** Integrates with AI applications (e.g. LangGraph, mem0) for short-term memory, long-term memory, LLM response caching (semantic caching), and retrieval augmented generation (RAG).
- **Real-Time Analytics:** Powers personalization, recommendations, fraud detection, and risk assessment.

## Why choose Redis?

Redis is a popular choice for developers worldwide due to its combination of speed, flexibility, and rich feature set. Here's why people choose Redis for:

- **Performance:** Because Redis keeps data primarily in memory and uses efficient data structures, it achieves extremely low latency (often sub-millisecond) for both read and write operations. This makes it ideal for applications demanding real-time responsiveness.
- **Flexibility:** Redis isn't just a key-value store, it provides native support for a wide range of data structures and capabilities listed in [What is Redis?](#what-is-redis)
- **Extensibility:** Redis is not limited to the built-in data structures, it has a [modules API](https://redis.io/docs/latest/develop/reference/modules/) that makes it possible to extend Redis functionality and rapidly implement new Redis commands
- **Simplicity:** Redis has a simple, text-based protocol and [well-documented command set](https://redis.io/docs/latest/commands/)
- **Ubiquity:** Redis is battle tested in production workloads at a massive scale. There is a good chance you indirectly interact with Redis several times daily
- **Versatility**: Redis is the de facto standard for use cases such as:
  - **Caching:** quickly access frequently used data without needing to query your primary database
  - **Session management:** read and write user session data without hurting user experience or slowing down every API call
  - **Querying, sorting, and analytics:** perform deduplication, full text search, and secondary indexing on in-memory data as fast as possible
  - **Messaging and interservice communication:** job queues, message brokering, pub/sub, and streams for communicating between services
  - **Vector operations:** Long-term and short-term LLM memory, RAG content retrieval, semantic caching, semantic routing, and vector similarity search

In summary, Redis provides a powerful, fast, and flexible toolkit for solving a wide variety of data management challenges. If you want to know more, here is a list of starting points:

- [**Introduction to Redis data types**](https://redis.io/docs/latest/develop/data-types/)
- [**The full list of Redis commands**](https://redis.io/commands/)
- [**Redis for AI**](https://redis.io/docs/latest/develop/ai/)
- [**Redis documentation**](https://redis.io/documentation/)

## What is Redis Open Source?

Redis Community Edition (Redis CE) was renamed Redis Open Source with the v8.0 release.

Redis Ltd. also offers [Redis Software](https://redis.io/enterprise/), a self-managed software with additional compliance, reliability, and resiliency for enterprise scaling,
and [Redis Cloud](https://redis.io/cloud/), a fully managed service integrated with Google Cloud, Azure, and AWS for production-ready apps.

Read more about the differences between Redis Open Source and Redis [here](https://redis.io/technology/advantages/).

## Getting started

If you want to get up and running with Redis quickly without needing to build from source, use one of the following methods:

- [**Redis Cloud**](https://cloud.redis.io/)
- [**Official Redis Docker images (Alpine/Debian)**](https://hub.docker.com/_/redis)
  ```sh
  docker run -d -p 6379:6379 redis:latest
  ```
- **Redis binary distributions**
  - [**Snap**](https://github.com/redis/redis-snap)
  - [**Homebrew**](https://github.com/redis/homebrew-redis)
  - [**RPM**](https://github.com/redis/redis-rpm)
  - [**Debian**](https://github.com/redis/redis-debian)
- [**Redis quick start guides**](https://redis.io/docs/latest/develop/get-started/)

If you prefer to [build Redis from source](#build-redis-from-source) - see instructions below.

### Redis starter projects

To get started as quickly as possible in your language of choice, use one of the following starter projects:

- [**Python (redis-py)**](https://github.com/redis-developer/redis-starter-python)
- [**C#/.NET (NRedisStack/StackExchange.Redis)**](https://github.com/redis-developer/redis-starter-csharp)
- [**Go (go-redis)**](https://github.com/redis-developer/redis-starter-go)
- [**JavaScript (node-redis)**](https://github.com/redis-developer/redis-starter-js)
- [**Java/Spring (Jedis)**](https://github.com/redis-developer/redis-starter-java)

### Using Redis with client libraries

To connect your application to Redis, you will need a client library. Redis has documented client libraries in most popular languages, with community-supported client libraries in additional languages.

- [**Python (redis-py)**](https://redis.io/docs/latest/develop/clients/redis-py/)
- [**Python (RedisVL)**](https://redis.io/docs/latest/integrate/redisvl/)
- [**C#/.NET (NRedisStack/StackExchange.Redis)**](https://redis.io/docs/latest/develop/clients/dotnet/)
- [**JavaScript (node-redis)**](https://redis.io/docs/latest/develop/clients/nodejs/)
- [**Java (Jedis)**](https://redis.io/docs/latest/develop/clients/jedis/)
- [**Java (Lettuce)**](https://redis.io/docs/latest/develop/clients/lettuce/)
- [**Go (go-redis)**](https://redis.io/docs/latest/develop/clients/go/)
- [**PHP (Predis)**](https://redis.io/docs/latest/develop/clients/php/)
- [**C (hiredis)**](https://redis.io/docs/latest/develop/clients/hiredis/)
- [**Full list of client libraries**](https://redis.io/docs/latest/develop/clients/)

### Using Redis with redis-cli

[`redis-cli`](https://redis.io/docs/latest/develop/tools/cli/) is Redis' command line interface. It is available as part of all the binary distributions and when you build Redis from source.

You can start a redis-server instance, and then, in another terminal try the following:

```sh
cd src
./redis-cli
```

```text
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

For a more visual and user-friendly experience, use [Redis Insight](https://redis.io/docs/latest/develop/tools/insight/) - a tool that lets you explore data, design, develop, and optimize your applications while also serving as a platform for Redis education and onboarding. Redis Insight integrates [Redis Copilot](https://redis.io/chat), a natural language AI assistant that improves the experience when working with data and commands.

- [**Redis Insight documentation**](https://redis.io/docs/latest/develop/tools/insight/)
- [**Redis Insight GitHub repository**](https://github.com/RedisInsight/RedisInsight)

## Redis data types, processing engines, and capabilities

Redis provides a variety of data types, processing engines, and capabilities to support a wide range of use cases:

**Important:** Features marked with an asterisk (\*) require Redis to be compiled with the `BUILD_WITH_MODULES=yes` flag when [building Redis from source](#build-redis-from-source)

- [**String:**](https://redis.io/docs/latest/develop/data-types/strings) Sequences of bytes, including text, serialized objects, and binary arrays used for caching, counters, and bitwise operations.
- [**JSON:**](https://redis.io/docs/latest/develop/data-types/json/) Nested JSON documents that are indexed and searchable using JSONPath expressions and with [Redis Query Engine](https://redis.io/docs/latest/develop/interact/search-and-query/)
- [**Hash:**](https://redis.io/docs/latest/develop/data-types/hashes/) Field-value maps used to represent basic objects and store groupings of key-value pairs with support for [hash field expiration (TTL)](https://redis.io/docs/latest/develop/data-types/hashes/#field-expiration)
- [**Redis Query Engine:**](https://redis.io/docs/latest/develop/interact/search-and-query/) Use Redis as a document database, a vector database, a secondary index, and a search engine. Define indexes for hash and JSON documents and then use a rich query language for vector search, full-text search, geospatial queries, and aggregations.
- [**List:**](https://redis.io/docs/latest/develop/data-types/lists/) Linked lists of string values used as stacks, queues, and for queue management.
- [**Set:**](https://redis.io/docs/latest/develop/data-types/sets/) Unordered collection of unique strings used for tracking unique items, relations, and common set operations (intersections, unions, differences).
- [**Sorted set:**](https://redis.io/docs/latest/develop/data-types/sorted-sets/) Collection of unique strings ordered by an associated score used for leaderboards and rate limiters.
- [**Vector set (beta):**](https://redis.io/docs/latest/develop/data-types/vector-sets/) Collection of vector embeddings used for semantic similarity search, semantic caching, semantic routing, and Retrieval Augmented Generation (RAG).
- [**Geospatial indexes:**](https://redis.io/docs/latest/develop/data-types/geospatial/) Coordinates used for finding nearby points within a given radius or bounding box.
- [**Bitmap:**](https://redis.io/docs/latest/develop/data-types/bitmaps/) A set of bit-oriented operations defined on the string type used for efficient set representations and object permissions.
- [**Bitfield:**](https://redis.io/docs/latest/develop/data-types/bitfields/) Binary-encoded strings that let you set, increment, and get integer values of arbitrary bit length used for limited-range counters, numeric values, and multi-level object permissions such as role-based access control (RBAC)
- [**Hyperloglog:**](https://redis.io/docs/latest/develop/data-types/probabilistic/hyperloglogs/) A probabilistic data structure for approximating the cardinality of a set used for analytics such as counting unique visits, form fills, etc.
- \*[**Bloom filter:**](https://redis.io/docs/latest/develop/data-types/probabilistic/bloom-filter/) A probabilistic data structure to check if a given value is present in a set. Used for fraud detection, ad placement, and unique column (i.e. username/email/slug) checks.
- \*[**Cuckoo filter:**](https://redis.io/docs/latest/develop/data-types/probabilistic/cuckoo-filter/) A probabilistic data structure for checking if a given value is present in a set while also allowing limited counting and deletions used in targeted advertising and coupon code validation.
- \*[**t-digest:**](https://redis.io/docs/latest/develop/data-types/probabilistic/t-digest/) A probabilistic data structure used for estimating the percentile of a large dataset without having to store and order all the data points. Used for hardware/software monitoring, online gaming, network traffic monitoring, and predictive maintenance.
- \*[**Top-k:**](https://redis.io/docs/latest/develop/data-types/probabilistic/top-k/) A probabilistic data structure for finding the most frequent values in a data stream used for trend discovery.
- \*[**Count-min sketch:**](https://redis.io/docs/latest/develop/data-types/probabilistic/count-min-sketch/) A probabilistic data structure for estimating how many times a given value appears in a data stream used for sales volume calculations.
- [**Time series:**](https://redis.io/docs/latest/develop/data-types/timeseries/) Data points indexed in time order used for monitoring sensor data, asset
  tracking, and predictive analytics
- [**Pub/sub**:](https://redis.io/docs/latest/develop/interact/pubsub/) A lightweight messaging capability. Publishers send messages to a channel, and subscribers receive messages from that channel.
- [**Stream**:](https://redis.io/docs/latest/develop/data-types/streams/) An append-only log with random access capabilities and complex consumption strategies such as consumer groups. Used for event sourcing, sensor monitoring, and notifications.
- [**Transaction:**](https://redis.io/docs/latest/develop/interact/transactions/) Allows the execution of a group of commands in a single step. A request sent by another client will never be served in the middle of the execution of a transaction. This guarantees that the commands are executed as a single isolated operation.
- [**Programmability:**](https://redis.io/docs/latest/develop/interact/programmability/eval-intro/) Upload and execute Lua scripts on the server. Scripts can employ programmatic control structures and use most of the commands while executing to access the database. Because scripts are executed on the server, reading and writing data from scripts is very efficient.

## Community

[**Redis Community Resources**](https://redis.io/community/)

## Build Redis from source

This section refers to building Redis from source. If you want to get up and running with Redis quickly without needing to build from source see the [Getting started section](#getting-started).

### Build and run Redis with all data structures - Ubuntu 20.04 (Focal)

Tested with the following Docker images:

- ubuntu:20.04

1. Install required dependencies

   Update your package lists and install the necessary development tools and libraries:

   ```sh
   apt-get update
   apt-get install -y sudo
   sudo apt-get install -y --no-install-recommends ca-certificates wget dpkg-dev gcc g++ libc6-dev libssl-dev make git python3 python3-pip python3-venv python3-dev unzip rsync clang automake autoconf gcc-10 g++-10 libtool
   ```

2. Use GCC 10 as the default compiler

   Update the system's default compiler to GCC 10:

   ```sh
   sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100 --slave /usr/bin/g++ g++ /usr/bin/g++-10
   ```

3. Install CMake

   Install CMake using `pip3` and link it for system-wide access:

   ```sh
   pip3 install cmake==3.31.6
   sudo ln -sf /usr/local/bin/cmake /usr/bin/cmake
   cmake --version
   ```

   Note: CMake version 3.31.6 is the latest supported version. Newer versions cannot be used.

4. Download the Redis source

   Download a specific version of the Redis source code archive from GitHub.

   Replace `<version>` with the Redis version, for example: `8.0.0`.

   ```sh
   cd /usr/src
   wget -O redis-<version>.tar.gz https://github.com/redis/redis/archive/refs/tags/<version>.tar.gz
   ```

5. Extract the source archive

   Create a directory for the source code and extract the contents into it:

   ```sh
   cd /usr/src
   tar xvf redis-<version>.tar.gz
   rm redis-<version>.tar.gz
   ```

6. Build Redis

   Set the necessary environment variables and compile Redis:

   ```sh
   cd /usr/src/redis-<version>
   export BUILD_TLS=yes BUILD_WITH_MODULES=yes INSTALL_RUST_TOOLCHAIN=yes DISABLE_WERRORS=yes
   make -j "$(nproc)" all
   ```

7. Run Redis

   ```sh
   cd /usr/src/redis-<version>
   ./src/redis-server redis-full.conf
   ```

### Build and run Redis with all data structures - Ubuntu 22.04 (Jammy) / 24.04 (Noble)

Tested with the following Docker image:

- ubuntu:22.04
- ubuntu:24.04

1. Install required dependencies

   Update your package lists and install the necessary development tools and libraries:

   ```sh
   apt-get update
   apt-get install -y sudo
   sudo apt-get install -y --no-install-recommends ca-certificates wget dpkg-dev gcc g++ libc6-dev libssl-dev make git cmake python3 python3-pip python3-venv python3-dev unzip rsync clang automake autoconf libtool
   ```

2. Download the Redis source

   Download a specific version of the Redis source code archive from GitHub.

   Replace `<version>` with the Redis version, for example: `8.0.0`.

   ```sh
   cd /usr/src
   wget -O redis-<version>.tar.gz https://github.com/redis/redis/archive/refs/tags/<version>.tar.gz
   ```

3. Extract the source archive

   Create a directory for the source code and extract the contents into it:

   ```sh
   cd /usr/src
   tar xvf redis-<version>.tar.gz
   rm redis-<version>.tar.gz
   ```

4. Build Redis

   Set the necessary environment variables and build Redis:

   ```sh
   cd /usr/src/redis-<version>
   export BUILD_TLS=yes BUILD_WITH_MODULES=yes INSTALL_RUST_TOOLCHAIN=yes DISABLE_WERRORS=yes
   make -j "$(nproc)" all
   ```

5. Run Redis

   ```sh
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

   ```sh
   apt-get update
   apt-get install -y sudo
   sudo apt-get install -y --no-install-recommends ca-certificates wget dpkg-dev gcc g++ libc6-dev libssl-dev make git cmake python3 python3-pip python3-venv python3-dev unzip rsync clang automake autoconf libtool
   ```

2. Download the Redis source

   Download a specific version of the Redis source code archive from GitHub.

   Replace `<version>` with the Redis version, for example: `8.0.0`.

   ```sh
   cd /usr/src
   wget -O redis-<version>.tar.gz https://github.com/redis/redis/archive/refs/tags/<version>.tar.gz
   ```

3. Extract the source archive

   Create a directory for the source code and extract the contents into it:

   ```sh
   cd /usr/src
   tar xvf redis-<version>.tar.gz
   rm redis-<version>.tar.gz
   ```

4. Build Redis

   Set the necessary environment variables and build Redis:

   ```sh
   cd /usr/src/redis-<version>
   export BUILD_TLS=yes BUILD_WITH_MODULES=yes INSTALL_RUST_TOOLCHAIN=yes DISABLE_WERRORS=yes
   make -j "$(nproc)" all
   ```

5. Run Redis

   ```sh
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

   ```sh
   microdnf install dnf sudo -y
   ```

   For 8.10 (regular), install sudo as follows:

   ```sh
   dnf install sudo -y
   ```

   Clean the package metadata, enable required repositories, and install development tools:

   ```sh
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

   ```sh
   sudo dnf install -y --nobest --skip-broken pkg-config wget gcc-toolset-13-gcc gcc-toolset-13-gcc-c++ git make openssl openssl-devel python3.11 python3.11-pip python3.11-devel unzip rsync clang curl libtool automake autoconf jq systemd-devel
   ```

   Create a Python virtual environment:

   ```sh
   python3.11 -m venv /opt/venv
   ```

   Enable the GCC toolset:

   ```sh
   sudo cp /opt/rh/gcc-toolset-13/enable /etc/profile.d/gcc-toolset-13.sh
   echo "source /etc/profile.d/gcc-toolset-13.sh" | sudo tee -a /etc/bashrc
   ```

3. Install CMake

   Install CMake 3.25.1 manually:

   ```sh
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

   ```sh
   cd /usr/src
   wget -O redis-<version>.tar.gz https://github.com/redis/redis/archive/refs/tags/<version>.tar.gz
   ```

5. Extract the source archive

   Create a directory for the source code and extract the contents into it:

   ```sh
   cd /usr/src
   tar xvf redis-<version>.tar.gz
   rm redis-<version>.tar.gz
   ```

6. Build Redis

   Enable the GCC toolset, set the necessary environment variables, and build Redis:

   ```sh
   source /etc/profile.d/gcc-toolset-13.sh
   cd /usr/src/redis-<version>
   export BUILD_TLS=yes BUILD_WITH_MODULES=yes INSTALL_RUST_TOOLCHAIN=yes DISABLE_WERRORS=yes
   make -j "$(nproc)" all
   ```

7. Run Redis

   ```sh
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

   ```sh
   microdnf install dnf sudo -y
   ```

   For 9.5 (regular), install sudo as follows:

   ```sh
   dnf install sudo -y
   ```

   Clean the package metadata, enable required repositories, and install development tools:

   ```sh
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

   ```sh
   sudo dnf install -y --nobest --skip-broken pkg-config xz wget which gcc-toolset-13-gcc gcc-toolset-13-gcc-c++ git make openssl openssl-devel python3 python3-pip python3-devel unzip rsync clang curl libtool automake autoconf jq systemd-devel
   ```

   Create a Python virtual environment:

   ```sh
   python3 -m venv /opt/venv
   ```

   Enable the GCC toolset:

   ```sh
   sudo cp /opt/rh/gcc-toolset-13/enable /etc/profile.d/gcc-toolset-13.sh
   echo "source /etc/profile.d/gcc-toolset-13.sh" | sudo tee -a /etc/bashrc
   ```

3. Install CMake

   Install CMake 3.25.1 manually:

   ```sh
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

   ```sh
   cd /usr/src
   wget -O redis-<version>.tar.gz https://github.com/redis/redis/archive/refs/tags/<version>.tar.gz
   ```

5. Extract the source archive

   Create a directory for the source code and extract the contents into it:

   ```sh
   cd /usr/src
   tar xvf redis-<version>.tar.gz
   rm redis-<version>.tar.gz
   ```

6. Build Redis

   Enable the GCC toolset, set the necessary environment variables, and build Redis:

   ```sh
   source /etc/profile.d/gcc-toolset-13.sh
   cd /usr/src/redis-<version>
   export BUILD_TLS=yes BUILD_WITH_MODULES=yes INSTALL_RUST_TOOLCHAIN=yes DISABLE_WERRORS=yes
   make -j "$(nproc)" all
   ```

7. Run Redis

   ```sh
   cd /usr/src/redis-<version>
   ./src/redis-server redis-full.conf
   ```

### Build and run Redis with all data structures - macOS 13 (Ventura) and macOS 14 (Sonoma)

1. Install Homebrew

   If Homebrew is not already installed, follow the installation instructions on the [Homebrew home page](https://brew.sh/).

2. Install required packages

   ```sh
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

   ```sh
   RUST_INSTALLER=rust-1.80.1-$(if [ "$(uname -m)" = "arm64" ]; then echo "aarch64"; else echo "x86_64"; fi)-apple-darwin
   wget --quiet -O ${RUST_INSTALLER}.tar.xz https://static.rust-lang.org/dist/${RUST_INSTALLER}.tar.xz
   tar -xf ${RUST_INSTALLER}.tar.xz
   (cd ${RUST_INSTALLER} && sudo ./install.sh)
   ```

4. Download the Redis source

   Download a specific version of the Redis source code archive from GitHub.

   Replace `<version>` with the Redis version, for example: `8.0.0`.

   ```sh
   cd ~/src
   wget -O redis-<version>.tar.gz https://github.com/redis/redis/archive/refs/tags/<version>.tar.gz
   ```

5. Extract the source archive

   Create a directory for the source code and extract the contents into it:

   ```sh
   cd ~/src
   tar xvf redis-<version>.tar.gz
   rm redis-<version>.tar.gz
   ```

6. Build Redis

   ```sh
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

   ```sh
   export LC_ALL=en_US.UTF-8
   export LANG=en_US.UTF-8
   build_dir/bin/redis-server redis-full.conf
   ```

### Build and run Redis with all data structures - macOS 15 (Sequoia)

Support and instructions will be provided at a later date.

### Building Redis - flags and general notes

Redis can be compiled and used on Linux, OSX, OpenBSD, NetBSD, FreeBSD.
We support big endian and little endian architectures, and both 32 bit and 64-bit systems.

It may compile on Solaris derived systems (for instance SmartOS) but our support for this platform is _best effort_ and Redis is not guaranteed to work as well as on Linux, OSX, and \*BSD.

To build Redis with all the data structures (including JSON, time series, Bloom filter, cuckoo filter, count-min sketch, top-k, and t-digest) and with Redis Query Engine, make sure first that all the prerequisites are installed (see build instructions above, per operating system). You need to use the following flag in the make command:

```sh
make BUILD_WITH_MODULES=yes
```

To build Redis with just the core data structures, use:

```sh
make
```

To build with TLS support, you need OpenSSL development libraries (e.g. libssl-dev on Debian/Ubuntu) and the following flag in the make command:

```sh
make BUILD_TLS=yes
```

To build with systemd support, you need systemd development libraries (such as libsystemd-dev on Debian/Ubuntu or systemd-devel on CentOS), and the following flag:

```sh
make USE_SYSTEMD=yes
```

To append a suffix to Redis program names, add the following flag:

```sh
make PROG_SUFFIX="-alt"
```

You can build a 32 bit Redis binary using:

```sh
make 32bit
```

After building Redis, it is a good idea to test it using:

```sh
make test
```

If TLS is built, running the tests with TLS enabled (you will need `tcl-tls` installed):

```sh
./utils/gen-test-certs.sh
./runtest --tls
```

### Fixing build problems with dependencies or cached build options

Redis has some dependencies which are included in the `deps` directory. `make` does not automatically rebuild dependencies even if something in the source code of dependencies changes.

When you update the source code with `git pull` or when code inside the dependencies tree is modified in any other way, make sure to use the following command in order to really clean everything and rebuild from scratch:

```sh
make distclean
```

This will clean: jemalloc, lua, hiredis, linenoise and other dependencies.

Also, if you force certain build options like 32bit target, no C compiler optimizations (for debugging purposes), and other similar build time options, those options are cached indefinitely until you issue a `make distclean`
command.

### Fixing problems building 32 bit binaries

If after building Redis with a 32 bit target you need to rebuild it
with a 64 bit target, or the other way around, you need to perform a `make distclean` in the root directory of the Redis distribution.

In case of build errors when trying to build a 32 bit binary of Redis, try the following steps:

- Install the package libc6-dev-i386 (also try g++-multilib).
- Try using the following command line instead of `make 32bit`:
  `make CFLAGS="-m32 -march=native" LDFLAGS="-m32"`

### Allocator

Selecting a non-default memory allocator when building Redis is done by setting the `MALLOC` environment variable. Redis is compiled and linked against libc malloc by default, except for jemalloc being the default on Linux systems. This default was picked because jemalloc has proven to have fewer fragmentation problems than libc malloc.

To force compiling against libc malloc, use:

```sh
make MALLOC=libc
```

To compile against jemalloc on Mac OS X systems, use:

```sh
make MALLOC=jemalloc
```

### Monotonic clock

By default, Redis will build using the POSIX clock_gettime function as the monotonic clock source. On most modern systems, the internal processor clock can be used to improve performance. Cautions can be found here: http://oliveryang.net/2015/09/pitfalls-of-TSC-usage/

To build with support for the processor's internal instruction clock, use:

```sh
make CFLAGS="-DUSE_PROCESSOR_CLOCK"
```

### Verbose build

Redis will build with a user-friendly colorized output by default.
If you want to see a more verbose output, use the following:

```sh
make V=1
```

### Running Redis with TLS

Please consult the [TLS.md](TLS.md) file for more information on how to use Redis with TLS.

## Code contributions

By contributing code to the Redis project in any form, including sending a pull request via GitHub, a code fragment or patch via private email or public discussion groups, you agree to release your code under the terms of the Redis Software Grant and Contributor License Agreement. Please see the CONTRIBUTING.md file in this source distribution for more information. For security bugs and vulnerabilities, please see SECURITY.md and the description of the ability of users to backport security patches under Redis Open Source 7.4+ under BSDv3. Open Source Redis releases are subject to the following licenses:

1. Version 7.2.x and prior releases are subject to BSDv3. These contributions to the original Redis core project are owned by their contributors and licensed under the 3BSDv3 license as referenced in the REDISCONTRIBUTIONS.txt file. Any copy of that license in this repository applies only to those contributions;

2. Versions 7.4.x to 7.8.x are subject to your choice of RSALv2 or SSPLv1; and

3. Version 8.0.x and subsequent releases are subject to the tri-license RSALv2/SSPLv1/AGPLv3 at your option as referenced in the LICENSE.txt file.

## Redis Trademarks

The purpose of a trademark is to identify the goods and services of a person or company without causing confusion. As the registered owner of its name and logo, Redis accepts certain limited uses of its trademarks, but it has requirements that must be followed as described in its Trademark Guidelines available at: https://redis.io/legal/trademark-policy/.
