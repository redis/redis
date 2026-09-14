# CRoaring

[![Ubuntu-CI](https://github.com/RoaringBitmap/CRoaring/actions/workflows/ubuntu-noexcept-ci.yml/badge.svg)](https://github.com/RoaringBitmap/CRoaring/actions/workflows/ubuntu-noexcept-ci.yml) [![VS18-CI](https://github.com/RoaringBitmap/CRoaring/actions/workflows/vs18-ci.yml/badge.svg)](https://github.com/RoaringBitmap/CRoaring/actions/workflows/vs18-ci.yml)
[![Fuzzing Status](https://oss-fuzz-build-logs.storage.googleapis.com/badges/croaring.svg)](https://bugs.chromium.org/p/oss-fuzz/issues/list?sort=-opened&can=1&q=proj:croaring)

[![Doxygen Documentation](https://img.shields.io/badge/docs-doxygen-green.svg)](http://roaringbitmap.github.io/CRoaring/)



Portable Roaring bitmaps in C (and C++) with full support for your favorite compiler (GNU GCC, LLVM's clang, Visual Studio, Apple Xcode, Intel oneAPI). Included in the [Awesome C](https://github.com/kozross/awesome-c) list of open source C software.

# Table of Contents

- [Introduction](#introduction)
- [Objective](#objective)
- [Requirements](#requirements)
- [Quick Start](#quick-start)
- [How to use the library?](#how-to-use-the-library)
  - [The C API](#the-c-api)
  - [The C++ API](#the-c-api-1)
- [Packages](#packages)
- [Using Roaring as a CPM dependency](#using-roaring-as-a-cpm-dependency)
- [Using as a CMake dependency with FetchContent](#using-as-a-cmake-dependency-with-fetchcontent)
- [Amalgamating](#amalgamating)
- [API](#api)
  - [Main API functions](#main-api-functions)
  - [C++ API functions](#c-api-functions)
- [Dealing with large volumes of data](#dealing-with-large-volumes-of-data)
- [Running microbenchmarks](#running-microbenchmarks)
- [Custom memory allocators](#custom-memory-allocators)
- [Example (C)](#example-c)
- [Compressed 64-bit Roaring bitmaps (C)](#compressed-64-bit-roaring-bitmaps-c)
- [Conventional bitsets (C)](#conventional-bitsets-c)
- [Example (C++)](#example-c-1)
- [Building with cmake (Linux and macOS, Visual Studio users should see below)](#building-with-cmake-linux-and-macos-visual-studio-users-should-see-below)
- [Building (Visual Studio under Windows)](#building-visual-studio-under-windows)
  - [Usage (Using conan)](#usage-using-conan)
  - [Usage (Using vcpkg on Windows, Linux and macOS)](#usage-using-vcpkg-on-windows-linux-and-macos)
- [SIMD-related throttling](#simd-related-throttling)
- [Thread safety](#thread-safety)
- [How to best aggregate bitmaps?](#how-to-best-aggregate-bitmaps)
- [Wrappers for Roaring Bitmaps](#wrappers-for-roaring-bitmaps)
- [Mailing list/discussion group](#mailing-listdiscussion-group)
- [Contributing](#contributing)
- [References about Roaring](#references-about-roaring)

# Introduction

Bitsets, also called bitmaps, are commonly used as fast data structures. Unfortunately, they can use too much memory.
 To compensate, we often use compressed bitmaps.

Roaring bitmaps are compressed bitmaps which tend to outperform conventional compressed bitmaps such as WAH, EWAH or Concise.
They are used by several major systems such as [Apache Lucene][lucene] and derivative systems such as [Solr][solr] and
[Elasticsearch][elasticsearch], [Metamarkets' Druid][druid], [LinkedIn Pinot][pinot], [Netflix Atlas][atlas], [Apache Spark][spark], [OpenSearchServer][opensearchserver], [Cloud Torrent][cloudtorrent], [Whoosh][whoosh], [InfluxDB](https://www.influxdata.com), [Pilosa][pilosa], [Bleve](http://www.blevesearch.com), [Microsoft Visual Studio Team Services (VSTS)][vsts], and eBay's [Apache Kylin][kylin]. The CRoaring library is used in several systems such as [Apache Doris](http://doris.incubator.apache.org), [ClickHouse](https://github.com/ClickHouse/ClickHouse), [Redpanda](https://github.com/redpanda-data/redpanda), [YDB](https://ydb.tech), [Alibaba Tair](https://www.alibabacloud.com/help/en/redis/developer-reference/tairroaring-command), [clice](https://github.com/clice-io/clice), [biscuit](https://github.com/CrystallineCore/Biscuit) and [StarRocks](https://github.com/StarRocks/starrocks). The YouTube SQL Engine, [Google Procella](https://research.google/pubs/pub48388/), uses Roaring bitmaps for indexing.

We published a peer-reviewed article on the design and evaluation of this library:

- Roaring Bitmaps: Implementation of an Optimized Software Library, Software: Practice and Experience 48 (4), 2018 [arXiv:1709.07821](https://arxiv.org/abs/1709.07821)

[lucene]: https://lucene.apache.org/
[solr]: https://lucene.apache.org/solr/
[elasticsearch]: https://www.elastic.co/products/elasticsearch
[druid]: http://druid.io/
[spark]: https://spark.apache.org/
[opensearchserver]: http://www.opensearchserver.com
[cloudtorrent]: https://github.com/jpillora/cloud-torrent
[whoosh]: https://bitbucket.org/mchaput/whoosh/wiki/Home
[pilosa]: https://www.pilosa.com/
[kylin]: http://kylin.apache.org/
[pinot]: http://github.com/linkedin/pinot/wiki
[vsts]: https://www.visualstudio.com/team-services/
[atlas]: https://github.com/Netflix/atlas

Roaring bitmaps are found to work well in many important applications:

> Use Roaring for bitmap compression whenever possible. Do not use other bitmap compression methods ([Wang et al., SIGMOD 2017](http://db.ucsd.edu/wp-content/uploads/2017/03/sidm338-wangA.pdf))


[There is a serialized format specification for interoperability between implementations](https://github.com/RoaringBitmap/RoaringFormatSpec/). Hence, it is possible to serialize a Roaring Bitmap from C++, read it in Java, modify it, serialize it back and read it in Go and Python.

# Objective

The primary goal of the CRoaring is to provide a high performance low-level implementation that fully take advantage
of the latest hardware. Roaring bitmaps are already available on a variety of platform through Java, Go, Rust... implementations. CRoaring is a library that seeks to achieve superior performance by staying close to the latest hardware.


(c) 2016-... The CRoaring authors.



# Requirements

- Linux, macOS, FreeBSD, Windows (MSYS2 and Microsoft Visual studio).
- We test the library with ARM, x64/x86 and POWER processors. We support big endian systems.
- Recent C compiler supporting the C11 standard (GCC 7 or better, LLVM 8 or better (clang), Xcode 11 or better, Microsoft Visual Studio 2022 or better, Intel oneAPI Compiler 2023.2 or better), there is also an optional C++ class that requires a C++ compiler supporting the C++11 standard. We support [Fil-C, the memory-safe C/C++ compiler](https://fil-c.org).
- CMake (to contribute to the project, users can rely on amalgamation/unity builds if they do not wish to use CMake).
- The CMake system assumes that git is available.
- Under x64 systems, the library provides runtime dispatch so that optimized functions are called based on the detected CPU features. It works with GCC, clang (version 9 and up) and Visual Studio (2017 and up). Other systems (e.g., ARM) do not need runtime dispatch.



# Quick Start

The CRoaring library can be amalgamated into a single source file that makes it easier
for integration into other projects. Moreover, by making it possible to compile
all the critical code into one compilation unit, it can improve the performance. For
the rationale, please see the [SQLite documentation](https://www.sqlite.org/amalgamation.html),
or the corresponding [Wikipedia entry](https://en.wikipedia.org/wiki/Single_Compilation_Unit).
Users who choose this route, do not need to rely on CRoaring's build system (based on CMake).

We offer amalgamated files as part of each release.

Linux or macOS users might follow the following instructions if they have a recent C or C++ compiler installed and a standard utility (`wget`).


 1. Pull the library in a directory
    ```
    wget https://github.com/RoaringBitmap/CRoaring/releases/download/v2.1.0/roaring.c
    wget https://github.com/RoaringBitmap/CRoaring/releases/download/v2.1.0/roaring.h
    wget https://github.com/RoaringBitmap/CRoaring/releases/download/v2.1.0/roaring.hh
    ```
 2. Create a new file named `demo.c` with this content:
    ```C
    #include <stdio.h>
    #include <stdlib.h>
    #include "roaring.c"
    int main() {
        roaring_bitmap_t *r1 = roaring_bitmap_create();
        for (uint32_t i = 100; i < 1000; i++) roaring_bitmap_add(r1, i);
        printf("cardinality = %d\n", (int) roaring_bitmap_get_cardinality(r1));
        roaring_bitmap_free(r1);

        bitset_t *b = bitset_create();
        for (int k = 0; k < 1000; ++k) {
                bitset_set(b, 3 * k);
        }
        printf("%zu \n", bitset_count(b));
        bitset_free(b);
        return EXIT_SUCCESS;
    }
    ```
    [You can try it out on Godbolt](https://godbolt.org/z/Y6oMGEo97).
 2. Create a new file named `demo.cpp` with this content:
    ```C++
    #include <iostream>
    #include "roaring.hh" // the amalgamated roaring.hh includes roaring64map.hh
    #include "roaring.c"
    int main() {
        roaring::Roaring r1;
        for (uint32_t i = 100; i < 1000; i++) {
            r1.add(i);
        }
        std::cout << "cardinality = " << r1.cardinality() << std::endl;

        roaring::Roaring64Map r2;
        for (uint64_t i = 18000000000000000100ull; i < 18000000000000001000ull; i++) {
            r2.add(i);
        }
        std::cout << "cardinality = " << r2.cardinality() << std::endl;
        return 0;
    }
    ```
 2. Compile
    ```
    cc -o demo demo.c
    c++ -std=c++11 -o demopp demo.cpp
    ```
 3. `./demo`
    ```
    cardinality = 900
    1000
    ```
 4. `./demopp`
    ```
    cardinality = 900
    cardinality = 900
    ```



# How to use the library?

The library offers both a C API (`roaring.h` for 32-bit bitmaps, `roaring64.h`
for 64-bit bitmaps) and a C++ API (`roaring.hh` and `roaring64map.hh`). The two
short programs below cover the most common operations: creating a bitmap, adding
values, querying it, combining bitmaps with set operations, and iterating over
the values. Both programs are part of our test suite, so they are guaranteed to
compile and run.

## The C API

Each bitmap you create with `roaring_bitmap_create()` (or that is returned by a
set operation such as `roaring_bitmap_and()`) must be released with
`roaring_bitmap_free()`.

```c
#include <roaring/roaring.h>
#include <roaring/roaring64.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    // --- 32-bit bitmaps ---
    // Create an empty bitmap and add a few values.
    roaring_bitmap_t *bitmap = roaring_bitmap_create();
    roaring_bitmap_add(bitmap, 1);
    roaring_bitmap_add(bitmap, 100);
    roaring_bitmap_add(bitmap, 1000);
    roaring_bitmap_add_range(bitmap, 10, 20);  // adds the half-open range [10, 20)

    // Query the bitmap.
    assert(roaring_bitmap_contains(bitmap, 100));
    assert(!roaring_bitmap_contains(bitmap, 50));
    printf("32-bit cardinality = %d\n",
           (int)roaring_bitmap_get_cardinality(bitmap));

    // Optionally compress runs of consecutive values for a smaller footprint.
    roaring_bitmap_run_optimize(bitmap);

    // Set operations return a new bitmap that you own and must free.
    roaring_bitmap_t *other = roaring_bitmap_from(100, 1000, 5000);
    roaring_bitmap_t *intersection = roaring_bitmap_and(bitmap, other);
    assert(roaring_bitmap_get_cardinality(intersection) == 2);  // {100, 1000}

    // Iterate over the values in sorted (increasing) order.
    roaring_uint32_iterator_t *it = roaring_iterator_create(bitmap);
    while (it->has_value) {
        // do something with it->current_value
        roaring_uint32_iterator_advance(it);
    }
    roaring_uint32_iterator_free(it);

    roaring_bitmap_free(intersection);
    roaring_bitmap_free(other);
    roaring_bitmap_free(bitmap);

    // --- 64-bit bitmaps (same ideas, but with 64-bit values) ---
    roaring64_bitmap_t *big = roaring64_bitmap_create();
    roaring64_bitmap_add(big, 1);
    roaring64_bitmap_add(big, 0xFFFFFFFFFFULL);  // a value beyond 32 bits
    assert(roaring64_bitmap_contains(big, 0xFFFFFFFFFFULL));
    printf("64-bit cardinality = %d\n",
           (int)roaring64_bitmap_get_cardinality(big));
    roaring64_bitmap_free(big);

    return EXIT_SUCCESS;
}
```

## The C++ API

The C++ classes (`roaring::Roaring` and `roaring::Roaring64Map`) wrap the C API
and manage memory for you: the destructor frees the bitmap, and set operations
are exposed as operators (`&`, `|`, `^`, `-`). No explicit `free` is required.

```cpp
#include <roaring/roaring.hh>
#include <roaring/roaring64map.hh>

#include <cassert>
#include <iostream>

using namespace roaring;

int main() {
    // --- 32-bit bitmaps ---
    Roaring r;
    r.add(1);
    r.add(100);
    r.add(1000);
    r.addRange(10, 20);  // adds the half-open range [10, 20)

    assert(r.contains(100));
    assert(!r.contains(50));
    std::cout << "32-bit cardinality = " << r.cardinality() << std::endl;

    // Construct a bitmap directly from a list of values.
    Roaring other = Roaring::bitmapOfList({100, 1000, 5000});

    // Operators return new bitmaps; their memory is managed for you.
    Roaring intersection = r & other;
    assert(intersection.cardinality() == 2);  // {100, 1000}

    // Range-based iteration visits the values in sorted (increasing) order.
    uint64_t sum = 0;
    for (uint32_t value : r) {
        sum += value;
    }
    std::cout << "sum of values = " << sum << std::endl;

    // --- 64-bit bitmaps ---
    Roaring64Map big;
    big.add(uint64_t(1));
    big.add(uint64_t(0xFFFFFFFFFFULL));  // a value beyond 32 bits
    assert(big.contains(uint64_t(0xFFFFFFFFFFULL)));
    std::cout << "64-bit cardinality = " << big.cardinality() << std::endl;

    return EXIT_SUCCESS;
}
```

For more extensive, fully commented examples (serialization, bulk operations,
copy-on-write, aggregating many bitmaps, etc.), see the [Example (C)](#example-c)
and [Example (C++)](#example-c-1) sections below.

Packages
------

[![Packaging status](https://repology.org/badge/vertical-allrepos/croaring.svg)](https://repology.org/project/croaring/versions)

# Using Roaring as a CPM dependency


If you like CMake and CPM, you can add just a few lines in your `CMakeLists.txt` file to grab a `CRoaring` release. [See our CPM demonstration for further details](https://github.com/RoaringBitmap/CPMdemo).



```CMake
cmake_minimum_required(VERSION 3.10)
project(roaring_demo
  LANGUAGES CXX C
)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

add_executable(hello hello.cpp)
# You can add CPM.cmake like so:
# mkdir -p cmake
# wget -O cmake/CPM.cmake https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/get_cpm.cmake
include(cmake/CPM.cmake)
CPMAddPackage(
  NAME roaring
  GITHUB_REPOSITORY "RoaringBitmap/CRoaring"
  GIT_TAG v2.0.4
  OPTIONS "ENABLE_ROARING_TESTS OFF"
)

target_link_libraries(hello roaring::roaring)
```


# Using as a CMake dependency with FetchContent

If you like CMake, you can add just a few lines in your `CMakeLists.txt` file to grab a `CRoaring` release. [See our demonstration for further details](https://github.com/RoaringBitmap/croaring_cmake_demo_single_file).

If you installed the CRoaring library locally, you may use it with CMake's `find_package` function as in this example:

```CMake
cmake_minimum_required(VERSION 3.15)

project(test_roaring_install VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(roaring REQUIRED)

file(WRITE main.cpp "
#include <iostream>
#include \"roaring/roaring.hh\"
int main() {
  roaring::Roaring r1;
  for (uint32_t i = 100; i < 1000; i++) {
    r1.add(i);
  }
  std::cout << \"cardinality = \" << r1.cardinality() << std::endl;
  return 0;
}")

add_executable(repro main.cpp)
target_link_libraries(repro PUBLIC roaring::roaring)
```


# Amalgamating

To generate the amalgamated files yourself, you can invoke a bash script...

```bash
./amalgamation.sh
```

If you prefer a silent output, you can use the following command to redirect ``stdout`` :

```bash
./amalgamation.sh > /dev/null
```


(Bash shells are standard under Linux and macOS. Bash shells are available under Windows as part of the  [GitHub Desktop](https://desktop.github.com/) under the name ``Git Shell``. So if you have cloned the ``CRoaring`` GitHub repository from within the GitHub Desktop, you can right-click on ``CRoaring``, select ``Git Shell`` and then enter the above commands.)

It is not necessary to invoke the script in the CRoaring directory. You can invoke
it from any directory where you want the amalgamation files to be written.

It will generate three files for C users: ``roaring.h``, ``roaring.c`` and ``amalgamation_demo.c``... as well as some brief instructions. The ``amalgamation_demo.c`` file is a short example, whereas ``roaring.h`` and ``roaring.c`` are "amalgamated" files (including all source and header files for the project). This means that you can simply copy the files ``roaring.h`` and ``roaring.c`` into your project and be ready to go! No need to produce a library! See the ``amalgamation_demo.c`` file.

# API

The C interface is found in the files

- [roaring.h](https://github.com/RoaringBitmap/CRoaring/blob/master/include/roaring/roaring.h),
- [roaring64.h](https://github.com/RoaringBitmap/CRoaring/blob/master/include/roaring/roaring64.h).

We also have a C++ interface:

- [roaring.hh](https://github.com/RoaringBitmap/CRoaring/blob/master/cpp/roaring/roaring.hh),
- [roaring64map.hh](https://github.com/RoaringBitmap/CRoaring/blob/master/cpp/roaring/roaring64map.hh).


# Main API functions

Below is an overview of the main functions provided by CRoaring in C, covering both 32-bit (`roaring.h`) and 64-bit (`roaring64.h`) bitmaps. For more details, see the header files in `include/roaring/` or the Doxygen documentation.

## Creation and Destruction
- `roaring_bitmap_t *roaring_bitmap_create(void);`  
  Create a new empty 32-bit bitmap.
- `roaring64_bitmap_t *roaring64_bitmap_create(void);`  
  Create a new empty 64-bit bitmap.
- `void roaring_bitmap_free(roaring_bitmap_t *r);`  
  Free a 32-bit bitmap.
- `void roaring64_bitmap_free(roaring64_bitmap_t *r);`  
  Free a 64-bit bitmap.

## Adding and Removing Values
- `void roaring_bitmap_add(roaring_bitmap_t *r, uint32_t x);`  
  Add value `x` to a 32-bit bitmap.
- `void roaring64_bitmap_add(roaring64_bitmap_t *r, uint64_t x);`  
  Add value `x` to a 64-bit bitmap.
- `void roaring_bitmap_remove(roaring_bitmap_t *r, uint32_t x);`  
  Remove value `x` from a 32-bit bitmap.
- `void roaring64_bitmap_remove(roaring64_bitmap_t *r, uint64_t x);`  
  Remove value `x` from a 64-bit bitmap.

## Queries and Cardinality
- `bool roaring_bitmap_contains(const roaring_bitmap_t *r, uint32_t x);`  
  Check if `x` is present in a 32-bit bitmap.
- `bool roaring64_bitmap_contains(const roaring64_bitmap_t *r, uint64_t x);`  
  Check if `x` is present in a 64-bit bitmap.
- `uint64_t roaring_bitmap_get_cardinality(const roaring_bitmap_t *r);`  
  Get the number of elements in a 32-bit bitmap.
- `uint64_t roaring64_bitmap_get_cardinality(const roaring64_bitmap_t *r);`  
  Get the number of elements in a 64-bit bitmap.

## Iteration
- `bool roaring_iterate(const roaring_bitmap_t *r, roaring_iterator iterator, void *param);`  
  Iterate over all values in a 32-bit bitmap, calling `iterator` for each value.
- `bool roaring64_bitmap_iterate(const roaring64_bitmap_t *r, roaring_iterator64 iterator, void *param);`  
  Iterate over all values in a 64-bit bitmap.

## Set Operations
- `roaring_bitmap_t *roaring_bitmap_and(const roaring_bitmap_t *r1, const roaring_bitmap_t *r2);`  
  Intersection (AND) of two 32-bit bitmaps.
- `roaring64_bitmap_t *roaring64_bitmap_and(const roaring64_bitmap_t *r1, const roaring64_bitmap_t *r2);`  
  Intersection (AND) of two 64-bit bitmaps.
- `roaring_bitmap_t *roaring_bitmap_or(const roaring_bitmap_t *r1, const roaring_bitmap_t *r2);`  
  Union (OR) of two 32-bit bitmaps.
- `roaring64_bitmap_t *roaring64_bitmap_or(const roaring64_bitmap_t *r1, const roaring64_bitmap_t *r2);`  
  Union (OR) of two 64-bit bitmaps.
- `roaring_bitmap_t *roaring_bitmap_xor(const roaring_bitmap_t *r1, const roaring_bitmap_t *r2);`  
  Symmetric difference (XOR) of two 32-bit bitmaps.
- `roaring64_bitmap_t *roaring64_bitmap_xor(const roaring64_bitmap_t *r1, const roaring64_bitmap_t *r2);`  
  Symmetric difference (XOR) of two 64-bit bitmaps.
- `roaring_bitmap_t *roaring_bitmap_andnot(const roaring_bitmap_t *r1, const roaring_bitmap_t *r2);`  
  Difference (r1 \ r2) for 32-bit bitmaps.
- `roaring64_bitmap_t *roaring64_bitmap_andnot(const roaring64_bitmap_t *r1, const roaring64_bitmap_t *r2);`  
  Difference (r1 \ r2) for 64-bit bitmaps.

## Serialization and Deserialization
- `size_t roaring_bitmap_portable_size_in_bytes(const roaring_bitmap_t *r);`  
  Get the number of bytes required to serialize a 32-bit bitmap.
- `size_t roaring64_bitmap_portable_size_in_bytes(const roaring64_bitmap_t *r);`  
  Get the number of bytes required to serialize a 64-bit bitmap.
- `size_t roaring_bitmap_portable_serialize(const roaring_bitmap_t *r, char *buf);`  
  Serialize a 32-bit bitmap to a buffer (portable format).
- `size_t roaring64_bitmap_portable_serialize(const roaring64_bitmap_t *r, char *buf);`  
  Serialize a 64-bit bitmap to a buffer (portable format).
- `roaring_bitmap_t *roaring_bitmap_portable_deserialize(const char *buf);`  
  Deserialize a 32-bit bitmap from a buffer. This is unsafe: it assumes `buf` points to a valid serialized bitmap and may read out of bounds otherwise. Prefer the safe variant below for untrusted input.
- The 64-bit API does not provide an unsafe deserializer; use `roaring64_bitmap_portable_deserialize_safe` (below) instead.
- `roaring_bitmap_t *roaring_bitmap_portable_deserialize_safe(const char *buf, size_t maxbytes);`  
  Safe deserialization of a 32-bit bitmap (will not read past `maxbytes`). If you are loading data from an untrusted source, you should call `roaring_bitmap_internal_validate` prior to using the `roaring_bitmap_t`.
- `roaring64_bitmap_t *roaring64_bitmap_portable_deserialize_safe(const char *buf, size_t maxbytes);`  
  Safe deserialization of a 64-bit bitmap (will not read past `maxbytes`).  If you are loading data from an untrusted source, you should call `roaring64_bitmap_internal_validate` prior to using the `roaring64_bitmap_t`.
- `size_t roaring_bitmap_portable_deserialize_size(const char *buf, size_t maxbytes);`  
  Get the size of a serialized 32-bit bitmap (returns 0 if invalid).
- `size_t roaring64_bitmap_portable_deserialize_size(const char *buf, size_t maxbytes);`  
  Get the size of a serialized 64-bit bitmap (returns 0 if invalid).

## Validation
- `bool roaring_bitmap_internal_validate(const roaring_bitmap_t *r, const char **reason);`  
  Validate the internal structure of a 32-bit bitmap. Returns `true` if valid, `false` otherwise. If invalid, `reason` points to a string describing the problem.
- `bool roaring64_bitmap_internal_validate(const roaring64_bitmap_t *r, const char **reason);`  
  Validate the internal structure of a 64-bit bitmap.

## Notes
- All memory allocated by the library must be freed using the corresponding `free` function.
- The portable serialization format is cross-platform and can be shared between different languages and architectures.
- Always validate bitmaps deserialized from untrusted sources before using them.



# C++ API functions

The C++ interface is provided via the `roaring.hh` (32-bit) and `roaring64map.hh` (64-bit) headers. These offer a modern, type-safe, and convenient API for manipulating Roaring bitmaps in C++.

## Main Classes
- `roaring::Roaring` — 32-bit Roaring bitmap
- `roaring::Roaring64Map` — 64-bit Roaring bitmap

## Common Methods (32-bit and 64-bit)
- `Roaring()` / `Roaring64Map()`
  - Construct an empty bitmap.
- `Roaring(std::initializer_list<uint32_t> values)`
  - Construct from a list of values.
- `void add(uint32_t x)` / `void add(uint64_t x)`
  - Add a value to the bitmap.
- `void remove(uint32_t x)` / `void remove(uint64_t x)`
  - Remove a value from the bitmap.
- `bool contains(uint32_t x) const` / `bool contains(uint64_t x) const`
  - Check if a value is present.
- `uint64_t cardinality() const`
  - Get the number of elements in the bitmap.
- `bool isEmpty() const`
  - Check if the bitmap is empty.
- `void clear()`
  - Remove all elements.
- `void runOptimize()`
  - Convert internal containers to run containers for better compression.
- `void setCopyOnWrite(bool enable)`
  - Enable or disable copy-on-write mode for fast/shallow copies.
- `bool operator==(const Roaring&) const` / `bool operator==(const Roaring64Map&) const`
  - Equality comparison.
- `void swap(Roaring&)` / `void swap(Roaring64Map&)`
  - Swap contents with another bitmap.

## Set Operations
- `Roaring operator|(const Roaring&) const` / `Roaring64Map operator|(const Roaring64Map&) const`
  - Union (OR)
- `Roaring operator&(const Roaring&) const` / `Roaring64Map operator&(const Roaring64Map&) const`
  - Intersection (AND)
- `Roaring operator^(const Roaring&) const` / `Roaring64Map operator^(const Roaring64Map&) const`
  - Symmetric difference (XOR)
- `Roaring operator-(const Roaring&) const` / `Roaring64Map operator-(const Roaring64Map&) const`
  - Difference
- In-place versions: `operator|=`, `operator&=`, `operator^=`, `operator-=`

## Iteration
- `Roaring::const_iterator` / `Roaring64Map::const_iterator`
  - Standard C++ iterator support: `begin()`, `end()`
- `void iterate(function, void* param)`
  - Call a function for each value (C-style callback).

## Serialization and Deserialization
- `size_t getSizeInBytes() const`
  - Get the size in bytes for serialization.
- `void write(char* buf) const`
  - Serialize the bitmap to a buffer.
- `static Roaring read(const char* buf, bool portable = true)`
  - Deserialize a bitmap from a buffer.
- `static Roaring readSafe(const char* buf, size_t maxbytes, bool portable = true)`
  - Safe deserialization (will not read past `maxbytes`).

## Bulk Operations
- `void addMany(size_t n, const uint32_t* values)` / `void addMany(size_t n, const uint64_t* values)`
  - Add many values at once.
- `void toUint32Array(uint32_t* out) const` / `void toUint64Array(uint64_t* out) const`
  - Export all values to an array.

## Example Usage
```cpp
#include "roaring/roaring.hh"
using namespace roaring;

Roaring r1;
r1.add(42);
if (r1.contains(42)) {
    // ...
}
Roaring r2 = Roaring::bitmapOf(3, 1, 2, 3);
Roaring r3 = r1 | r2;
for (auto v : r3) {
    // iterate over values
}
```

For 64-bit values, use `#include "roaring64map.hh"` and the `Roaring64Map` class, which has a similar API.


# Dealing with large volumes of data

Some users have to deal with large volumes of data. It  may be important for these users to be aware of the `addMany` (C++) `roaring_bitmap_or_many` (C) functions as it is much faster and economical to add values in batches when possible. Furthermore, calling periodically the `runOptimize` (C++) or `roaring_bitmap_run_optimize` (C) functions may help.


# Running microbenchmarks

We have microbenchmarks constructed with the Google Benchmarks.
Under Linux or macOS, you may run them as follows:

```
cmake -B build -D ENABLE_ROARING_MICROBENCHMARKS=ON
cmake --build build
./build/microbenchmarks/bench
```

By default, the benchmark tools picks one data set (e.g., `CRoaring/benchmarks/realdata/census1881`).
We have several data sets and you may pick others:

```
./build/microbenchmarks/bench benchmarks/realdata/wikileaks-noquotes
```

You may disable some functionality for the purpose of benchmarking. For example, assuming you
have an x64 processor, you could benchmark the code without AVX-512 even if both your processor
and compiler supports it:

```
cmake -B buildnoavx512 -D ROARING_DISABLE_AVX512=ON -D ENABLE_ROARING_MICROBENCHMARKS=ON
cmake --build buildnoavx512
./buildnoavx512/microbenchmarks/bench
```

You can benchmark without AVX or AVX-512 as well:

```
cmake -B buildnoavx -D ROARING_DISABLE_AVX=ON -D ENABLE_ROARING_MICROBENCHMARKS=ON
cmake --build buildnoavx
./buildnoavx/microbenchmarks/bench
```

Please see `microbenchmarks/README.md` for more details.

# Custom memory allocators
For general users, CRoaring would apply default allocator without extra codes. But global memory hook is also provided for those who want a custom memory allocator. Here is an example:
```C
#include <roaring.h>

int main(){
    // define with your own memory hook
    roaring_memory_t my_hook{my_malloc, my_free ...};
    // initialize global memory hook
    roaring_init_memory_hook(my_hook);
    // write you code here
    ...
}
```

By default we use:
```C
static roaring_memory_t global_memory_hook = {
    .malloc = malloc,
    .realloc = realloc,
    .calloc = calloc,
    .free = free,
    .aligned_malloc = roaring_bitmap_aligned_malloc,
    .aligned_free = roaring_bitmap_aligned_free,
};
```

We require that the `free`/`aligned_free` functions follow the C
convention where `free(NULL)`/`aligned_free(NULL)` have no effect.


# Example (C)


This example assumes that CRoaring has been build and that you are linking against the corresponding library. By default, CRoaring will install its header files in a `roaring` directory. If you are working from the amalgamation script, you may add the line `#include "roaring.c"` if you are not linking against a prebuilt CRoaring library and replace `#include <roaring/roaring.h>` by `#include "roaring.h"`.

```c
#include <roaring/roaring.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

bool roaring_iterator_sumall(uint32_t value, void *param) {
    *(uint32_t *)param += value;
    return true;  // iterate till the end
}

int main() {
    // create a new empty bitmap
    roaring_bitmap_t *r1 = roaring_bitmap_create();
    // then we can add values
    for (uint32_t i = 100; i < 1000; i++) roaring_bitmap_add(r1, i);
    // check whether a value is contained
    assert(roaring_bitmap_contains(r1, 500));
    // compute how many bits there are:
    uint32_t cardinality = roaring_bitmap_get_cardinality(r1);
    printf("Cardinality = %d \n", cardinality);

    // if your bitmaps have long runs, you can compress them by calling
    // run_optimize
    uint32_t expectedsizebasic = roaring_bitmap_portable_size_in_bytes(r1);
    roaring_bitmap_run_optimize(r1);
    uint32_t expectedsizerun = roaring_bitmap_portable_size_in_bytes(r1);
    printf("size before run optimize %d bytes, and after %d bytes\n",
           expectedsizebasic, expectedsizerun);

    // create a new bitmap containing the values {1,2,3,5,6}
    roaring_bitmap_t *r2 = roaring_bitmap_from(1, 2, 3, 5, 6);
    roaring_bitmap_printf(r2);  // print it

    // we can also create a bitmap from a pointer to 32-bit integers
    uint32_t somevalues[] = {2, 3, 4};
    roaring_bitmap_t *r3 = roaring_bitmap_of_ptr(3, somevalues);

    // we can also go in reverse and go from arrays to bitmaps
    uint64_t card1 = roaring_bitmap_get_cardinality(r1);
    uint32_t *arr1 = (uint32_t *)malloc(card1 * sizeof(uint32_t));
    assert(arr1 != NULL);
    roaring_bitmap_to_uint32_array(r1, arr1);
    roaring_bitmap_t *r1f = roaring_bitmap_of_ptr(card1, arr1);
    free(arr1);
    assert(roaring_bitmap_equals(r1, r1f));  // what we recover is equal
    roaring_bitmap_free(r1f);

    // we can go from arrays to bitmaps from "offset" by "limit"
    size_t offset = 100;
    size_t limit = 1000;
    uint32_t *arr3 = (uint32_t *)malloc(limit * sizeof(uint32_t));
    assert(arr3 != NULL);
    roaring_bitmap_range_uint32_array(r1, offset, limit, arr3);
    free(arr3);

    // we can copy and compare bitmaps
    roaring_bitmap_t *z = roaring_bitmap_copy(r3);
    assert(roaring_bitmap_equals(r3, z));  // what we recover is equal
    roaring_bitmap_free(z);

    // we can compute union two-by-two
    roaring_bitmap_t *r1_2_3 = roaring_bitmap_or(r1, r2);
    roaring_bitmap_or_inplace(r1_2_3, r3);

    // we can compute a big union
    const roaring_bitmap_t *allmybitmaps[] = {r1, r2, r3};
    roaring_bitmap_t *bigunion = roaring_bitmap_or_many(3, allmybitmaps);
    assert(
        roaring_bitmap_equals(r1_2_3, bigunion));  // what we recover is equal
    // can also do the big union with a heap
    roaring_bitmap_t *bigunionheap =
        roaring_bitmap_or_many_heap(3, allmybitmaps);
    assert(roaring_bitmap_equals(r1_2_3, bigunionheap));

    roaring_bitmap_free(r1_2_3);
    roaring_bitmap_free(bigunion);
    roaring_bitmap_free(bigunionheap);

    // we can compute intersection two-by-two
    roaring_bitmap_t *i1_2 = roaring_bitmap_and(r1, r2);
    roaring_bitmap_free(i1_2);

    // we can write a bitmap to a pointer and recover it later
    uint32_t expectedsize = roaring_bitmap_portable_size_in_bytes(r1);
    char *serializedbytes = malloc(expectedsize);
    // When serializing data to a file, we recommend that you also use
    // checksums so that, at deserialization, you can be confident
    // that you are recovering the correct data.
    roaring_bitmap_portable_serialize(r1, serializedbytes);
    // Note: it is expected that the input follows the specification
    // https://github.com/RoaringBitmap/RoaringFormatSpec
    // otherwise the result may be unusable.
    // The 'roaring_bitmap_portable_deserialize_safe' function will not read
    // beyond expectedsize bytes.
    // We also recommend that you use checksums to check that serialized data corresponds
    // to the serialized bitmap. The CRoaring library does not provide checksumming.
    roaring_bitmap_t *t = roaring_bitmap_portable_deserialize_safe(serializedbytes, expectedsize);
    if(t == NULL) { return EXIT_FAILURE; }
    const char *reason = NULL;
    // If your input came from an untrusted source, then you need to validate the
    // resulting bitmap. Failing to do so could lead to undefined behavior, crashes and so forth.
    if (!roaring_bitmap_internal_validate(t, &reason)) {
        return EXIT_FAILURE;
    }
    // At this point, the bitmap is safe.
    assert(roaring_bitmap_equals(r1, t));  // what we recover is equal
    roaring_bitmap_free(t);
    // we can also check whether there is a bitmap at a memory location without
    // reading it
    size_t sizeofbitmap =
        roaring_bitmap_portable_deserialize_size(serializedbytes, expectedsize);
    assert(sizeofbitmap ==
           expectedsize);  // sizeofbitmap would be zero if no bitmap were found
    // We can also read the bitmap "safely" by specifying a byte size limit.
    // The 'roaring_bitmap_portable_deserialize_safe' function will not read
    // beyond expectedsize bytes.
    // We also recommend that you use checksums to check that serialized data corresponds
    // to the serialized bitmap. The CRoaring library does not provide checksumming.
    t = roaring_bitmap_portable_deserialize_safe(serializedbytes, expectedsize);
    if(t == NULL) {
        printf("Problem during deserialization.\n");
        // We could clear any memory and close any file here.
        return EXIT_FAILURE;
    }
    // We can validate the bitmap we recovered to make sure it is proper.
    // If the data came from an untrusted source, you should call
    // roaring_bitmap_internal_validate.
    const char *reason_failure = NULL;
    if (!roaring_bitmap_internal_validate(t, &reason_failure)) {
        printf("safely deserialized invalid bitmap: %s\n", reason_failure);
        // We could clear any memory and close any file here.
        return EXIT_FAILURE;
    }
    assert(roaring_bitmap_equals(r1, t));  // what we recover is equal
    roaring_bitmap_free(t);

    free(serializedbytes);

    // we can iterate over all values using custom functions
    uint32_t counter = 0;
    roaring_iterate(r1, roaring_iterator_sumall, &counter);

    // we can also create iterator structs
    counter = 0;
    roaring_uint32_iterator_t *i = roaring_iterator_create(r1);
    while (i->has_value) {
        counter++;  // could use    i->current_value
        roaring_uint32_iterator_advance(i);
    }
    // you can skip over values and move the iterator with
    // roaring_uint32_iterator_move_equalorlarger(i,someintvalue)

    roaring_uint32_iterator_free(i);
    // roaring_bitmap_get_cardinality(r1) == counter

    // for greater speed, you can iterate over the data in bulk
    i = roaring_iterator_create(r1);
    uint32_t buffer[256];
    while (1) {
        uint32_t ret = roaring_uint32_iterator_read(i, buffer, 256);
        for (uint32_t j = 0; j < ret; j++) {
            counter += buffer[j];
        }
        if (ret < 256) {
            break;
        }
    }
    roaring_uint32_iterator_free(i);

    roaring_bitmap_free(r1);
    roaring_bitmap_free(r2);
    roaring_bitmap_free(r3);
    return EXIT_SUCCESS;
}
```

# Compressed 64-bit Roaring bitmaps (C)


We also support efficient 64-bit compressed bitmaps in C:

```c++
  roaring64_bitmap_t *r2 = roaring64_bitmap_create();
  for (uint64_t i = 100; i < 1000; i++) roaring64_bitmap_add(r2, i);
  printf("cardinality (64-bit) = %d\n", (int) roaring64_bitmap_get_cardinality(r2));
  roaring64_bitmap_free(r2);
```

The API is similar to the conventional 32-bit bitmaps. Please see
the header file `roaring64.h` (compare with `roaring.h`).

# Conventional bitsets (C)

We support convention bitsets (uncompressed) as part of the library.

Simple example:

```C
bitset_t * b = bitset_create();
bitset_set(b,10);
bitset_get(b,10);// returns true
bitset_free(b); // frees memory
```

More advanced example:

```C
bitset_t *b = bitset_create();
for (int k = 0; k < 1000; ++k) {
    bitset_set(b, 3 * k);
}
// We have bitset_count(b) == 1000.
// We have bitset_get(b, 3) is true
// You can iterate through the values:
size_t k = 0;
for (size_t i = 0; bitset_next_set_bit(b, &i); i++) {
    // You will have i == k
    k += 3;
}
// We support a wide range of operations on two bitsets such as
// bitset_inplace_symmetric_difference(b1,b2);
// bitset_inplace_symmetric_difference(b1,b2);
// bitset_inplace_difference(b1,b2);// should make no difference
// bitset_inplace_union(b1,b2);
// bitset_inplace_intersection(b1,b2);
// bitsets_disjoint
// bitsets_intersect
```

In some instances, you may want to convert a Roaring bitmap into a conventional (uncompressed) bitset.
Indeed, bitsets have advantages such as higher query performances in some cases. The following code
illustrates how you may do so:

```C
roaring_bitmap_t *r1 = roaring_bitmap_create();
for (uint32_t i = 100; i < 100000; i+= 1 + (i%5)) {
     roaring_bitmap_add(r1, i);
}
for (uint32_t i = 100000; i < 500000; i+= 100) {
     roaring_bitmap_add(r1, i);
}
roaring_bitmap_add_range(r1, 500000, 600000);
bitset_t * bitset = bitset_create();
bool success = roaring_bitmap_to_bitset(r1, bitset);
assert(success); // could fail due to memory allocation.
assert(bitset_count(bitset) == roaring_bitmap_get_cardinality(r1));
// You can then query the bitset:
for (uint32_t i = 100; i < 100000; i+= 1 + (i%5)) {
    assert(bitset_get(bitset,i));
}
for (uint32_t i = 100000; i < 500000; i+= 100) {
    assert(bitset_get(bitset,i));
}
// you must free the memory:
bitset_free(bitset);
roaring_bitmap_free(r1);
```

You should be aware that a convention bitset (`bitset_t *`) may use much more
memory than a Roaring bitmap in some cases. You should run benchmarks to determine
whether the conversion to a bitset has performance benefits in your case.

# Example (C++)


This example assumes that CRoaring has been build and that you are linking against the corresponding library. By default, CRoaring will install its header files in a `roaring` directory so you may need to replace `#include "roaring.hh"` by `#include <roaring/roaring.hh>`. If you are working from the amalgamation script, you may add the line `#include "roaring.c"` if you are not linking against a CRoaring prebuilt library.

```c++
#include <iostream>

#include "roaring.hh"

using namespace roaring;

int main() {
    Roaring r1;
    for (uint32_t i = 100; i < 1000; i++) {
        r1.add(i);
    }

    // check whether a value is contained
    assert(r1.contains(500));

    // compute how many bits there are:
    uint32_t cardinality = r1.cardinality();

    // if your bitmaps have long runs, you can compress them by calling
    // run_optimize
    uint32_t size = r1.getSizeInBytes();
    r1.runOptimize();

    // you can enable "copy-on-write" for fast and shallow copies
    r1.setCopyOnWrite(true);

    uint32_t compact_size = r1.getSizeInBytes();
    std::cout << "size before run optimize " << size << " bytes, and after "
              << compact_size << " bytes." << std::endl;

    // create a new bitmap with varargs
    Roaring r2 = Roaring::bitmapOf(5, 1, 2, 3, 5, 6);

    r2.printf();
    printf("\n");

    // create a new bitmap with initializer list
    Roaring r2i = Roaring::bitmapOfList({1, 2, 3, 5, 6});

    assert(r2i == r2);

    // we can also create a bitmap from a pointer to 32-bit integers
    const uint32_t values[] = {2, 3, 4};
    Roaring r3(3, values);

    // we can also go in reverse and go from arrays to bitmaps
    uint64_t card1 = r1.cardinality();
    uint32_t *arr1 = new uint32_t[card1];
    r1.toUint32Array(arr1);
    Roaring r1f(card1, arr1);
    delete[] arr1;

    // bitmaps shall be equal
    assert(r1 == r1f);

    // we can copy and compare bitmaps
    Roaring z(r3);
    assert(r3 == z);

    // we can compute union two-by-two
    Roaring r1_2_3 = r1 | r2;
    r1_2_3 |= r3;

    // we can compute a big union
    const Roaring *allmybitmaps[] = {&r1, &r2, &r3};
    Roaring bigunion = Roaring::fastunion(3, allmybitmaps);
    assert(r1_2_3 == bigunion);

    // we can compute intersection two-by-two
    Roaring i1_2 = r1 & r2;

    // we can write a bitmap to a pointer and recover it later
    uint32_t expectedsize = r1.getSizeInBytes();
    char *serializedbytes = new char[expectedsize];
    r1.write(serializedbytes);
    // readSafe will not overflow, but the resulting bitmap
    // is only valid and usable if the input follows the
    // Roaring specification: https://github.com/RoaringBitmap/RoaringFormatSpec/
    Roaring t = Roaring::readSafe(serializedbytes, expectedsize);
    assert(r1 == t);
    delete[] serializedbytes;

    // we can iterate over all values using custom functions
    uint32_t counter = 0;
    r1.iterate(
        [](uint32_t value, void *param) {
            *(uint32_t *)param += value;
            return true;
        },
        &counter);

    // we can also iterate the C++ way
    counter = 0;
    for (Roaring::const_iterator i = t.begin(); i != t.end(); i++) {
        ++counter;
    }
    // counter == t.cardinality()

    // we can move iterators to skip values
    const uint32_t manyvalues[] = {2, 3, 4, 7, 8};
    Roaring rogue(5, manyvalues);
    Roaring::const_iterator j = rogue.begin();
    j.move_equalorlarger(4);  // *j == 4
    return EXIT_SUCCESS;
}

```



# Building with cmake (Linux and macOS, Visual Studio or OpenHarmony users should see below)

CRoaring follows the standard cmake workflow. Starting from the root directory of
the project (CRoaring), you can do:

```
mkdir -p build
cd build
cmake ..
cmake --build .
# follow by 'ctest' if you want to test.
# you can also type 'make install' to install the library on your system
# C header files typically get installed to /usr/local/include/roaring
# whereas C++ header files get installed to /usr/local/include/roaring
```
(You can replace the ``build`` directory with any other directory name.)
By default all tests are built on all platforms, to skip building and running tests add `` -DENABLE_ROARING_TESTS=OFF `` to the command line.

As with all ``cmake`` projects, you can specify the compilers you wish to use by adding (for example) ``-DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++`` to the ``cmake`` command line.

If you are using clang or gcc and you know your target architecture,  you can set the architecture by specifying `-DROARING_ARCH=arch`. For example, if you have many server but the oldest server is running the Intel `haswell` architecture, you can specify -`DROARING_ARCH=haswell`. In such cases, the produced binary will be optimized for processors having the characteristics of a haswell process and may not run on older architectures. You can find out the list of valid architecture values by typing `man gcc`.

 ```
 mkdir -p build_haswell
 cd build_haswell
 cmake -DROARING_ARCH=haswell ..
 cmake --build .
 ```

For a debug release, starting from the root directory of the project (CRoaring), try

```
mkdir -p debug
cd debug
cmake -DCMAKE_BUILD_TYPE=Debug -DROARING_SANITIZE=ON ..
ctest
```


To check that your code abides by the style convention (make sure that ``clang-format`` is installed):

```
./tools/clang-format-check.sh
```

To reformat your code according to the style convention (make sure that ``clang-format`` is installed):

```
./tools/clang-format.sh
```

# Building (Visual Studio under Windows)

We are assuming that you have a common Windows PC with at least Visual Studio 2015, and an x64 processor.

To build with at least Visual Studio 2015 from the command line:
- Grab the CRoaring code from GitHub, e.g., by cloning it using [GitHub Desktop](https://desktop.github.com/).
- Install [CMake](https://cmake.org/download/). When you install it, make sure to ask that ``cmake`` be made available from the command line.
- Create a subdirectory within CRoaring, such as ``VisualStudio``.
- Using a shell, go to this newly created directory. For example, within GitHub Desktop, you can right-click on  ``CRoaring`` in your GitHub repository list, and select ``Open in Git Shell``, then type ``cd VisualStudio`` in the newly created shell.
- Type ``cmake -DCMAKE_GENERATOR_PLATFORM=x64 ..`` in the shell while in the ``VisualStudio`` repository. (Alternatively, if you want to build a static library, you may use the command line ``cmake -DCMAKE_GENERATOR_PLATFORM=x64 -DROARING_BUILD_STATIC=ON  ..``.)
- This last command created a Visual Studio solution file in the newly created directory (e.g., ``RoaringBitmap.sln``). Open this file in Visual Studio. You should now be able to build the project and run the tests. For example, in the ``Solution Explorer`` window (available from the ``View`` menu), right-click ``ALL_BUILD`` and select ``Build``. To test the code, still in the ``Solution Explorer`` window, select ``RUN_TESTS`` and select ``Build``.

To build with at least Visual Studio 2017 directly in the IDE:
- Grab the CRoaring code from GitHub, e.g., by cloning it using [GitHub Desktop](https://desktop.github.com/).
- Select the ``Visual C++ tools for CMake`` optional component when installing the C++ Development Workload within Visual Studio.
- Within Visual Studio use ``File > Open > Folder...`` to open the CRoaring folder.
- Right click on ``CMakeLists.txt`` in the parent directory within ``Solution Explorer`` and select ``Build`` to build the project.
- For testing, in the Standard toolbar, drop the ``Select Startup Item...`` menu and choose one of the tests. Run the test by pressing the button to the left of the dropdown.


We have optimizations specific to AVX2 and AVX-512 in the code, and they are turned dynamically based on the detected hardware at runtime.


## Usage (Using `conan`)

You can install pre-built binaries for `roaring` or build it from source using [Conan](https://conan.io/). Use the following command to install latest version:

```
conan install --requires="roaring/[*]" --build=missing
```

For detailed instructions on how to use Conan, please refer to the [Conan documentation](https://docs.conan.io/2/).

The `roaring` Conan recipe is kept up to date by Conan maintainers and community contributors.
If the version is out of date, please [create an issue or pull request](https://github.com/conan-io/conan-center-index) on the ConanCenterIndex repository.


## Usage (Using `vcpkg` on Windows, Linux and macOS)

[vcpkg](https://github.com/Microsoft/vcpkg) users on Windows, Linux and macOS can download and install `roaring` with one single command from their favorite shell.

On Linux and macOS:

```
$ ./vcpkg install roaring
```

will build and install `roaring` as a static library.

On Windows (64-bit):

```
.\vcpkg.exe install roaring:x64-windows
```

will build and install `roaring` as a shared library.

```
.\vcpkg.exe install roaring:x64-windows-static
```

will build and install `roaring` as a static library.

These commands will also print out instructions on how to use the library from MSBuild or CMake-based projects.

If you find the version of `roaring` shipped with `vcpkg` is out-of-date, feel free to report it to `vcpkg` community either by submiting an issue or by creating a PR.


# Building (OpenHarmony)

To build with OpenHarmony SDK please see the [OpenHarmony Cross Compile Guide](https://gitcode.com/openharmony-sig/tpc_c_cplusplus/blob/master/Cross-Compilation Guide for Open-Source Third-Party Libraries in OpenHarmony_en.md)

# SIMD-related throttling

Our AVX2 code does not use floating-point numbers or multiplications, so it is not subject to turbo frequency throttling on many-core Intel processors.

Our AVX-512 code is only enabled on recent hardware (Intel Ice Lake or better and AMD Zen 4) where SIMD-specific frequency throttling is not observed.

# Thread safety

Like, for example, STL containers, the CRoaring library has no built-in thread support. Thus whenever you modify a bitmap in one thread, it is unsafe to query it in others. However, you can safely copy a bitmap and use both copies in concurrently.

If you use  "copy-on-write" (default to disabled), then you should pass copies to the different threads. They will create shared containers, and for shared containers, we use reference counting with an atomic counter.



To summarize:
- If you do not use copy-on-write, you can access concurrent the same bitmap safely as long as you do not modify it. If you plan on modifying it, you should pass different copies to the different threads.
- If you use copy-on-write, you should always pass copies to the different threads. The copies and then lightweight (shared containers).

Thus the following pattern where you copy bitmaps and pass them to different threads is safe with or without COW:

```C
    roaring_bitmap_set_copy_on_write(r1, true);
    roaring_bitmap_set_copy_on_write(r2, true);
    roaring_bitmap_set_copy_on_write(r3, true);

    roaring_bitmap_t * r1a = roaring_bitmap_copy(r1);
    roaring_bitmap_t * r1b = roaring_bitmap_copy(r1);

    roaring_bitmap_t * r2a = roaring_bitmap_copy(r2);
    roaring_bitmap_t * r2b = roaring_bitmap_copy(r2);

    roaring_bitmap_t * r3a = roaring_bitmap_copy(r3);
    roaring_bitmap_t * r3b = roaring_bitmap_copy(r3);

    roaring_bitmap_t *rarray1[3] = {r1a, r2a, r3a};
    roaring_bitmap_t *rarray2[3] = {r1b, r2b, r3b};
    std::thread thread1(run, rarray1);
    std::thread thread2(run, rarray2);
```

# How to best aggregate bitmaps?

Suppose you want to compute the union (OR) of many bitmaps. How do you proceed? There are many
different strategies.

You can use `roaring_bitmap_or_many(bitmapcount, bitmaps)` or `roaring_bitmap_or_many_heap(bitmapcount, bitmaps)` or you may
even roll your own aggregation:

```C
roaring_bitmap_t *answer = roaring_bitmap_copy(bitmaps[0]);
for (size_t i = 1; i < bitmapcount; i++) {
  roaring_bitmap_or_inplace(answer, bitmaps[i]);
}
```

All of them will work but they have different performance characteristics. The `roaring_bitmap_or_many_heap` should
probably only be used if, after benchmarking, you find that it is faster by a good margin: it uses more memory.

The `roaring_bitmap_or_many` is meant as a good default. It works by trying to delay work as much as possible.
However, because it delays computations, it also does not optimize the format as the computation runs. It might
thus fail to see some useful pattern in the data such as long consecutive values.

The approach based on repeated calls to `roaring_bitmap_or_inplace`
is also fine, and might even be faster in some cases. You can expect it to be faster if, after
a few calls, you get long sequences of consecutive values in the answer. That is, if the
final answer is all integers in the range [0,1000000), and this is apparent quickly, then the
later `roaring_bitmap_or_inplace` will be very fast.

You should benchmark these alternatives on your own data to decide what is best.

# Wrappers for Roaring Bitmaps

This page lists several community-contributed wrappers for the Roaring Bitmap library, enabling its use in various programming languages and environments.

## Python

Tom Cornebize developed a Python wrapper, **PyRoaringBitMap**, which can be found at [https://github.com/Ezibenroc/PyRoaringBitMap](https://github.com/Ezibenroc/PyRoaringBitMap).

Installation is straightforward using pip:

```
pip install pyroaring
```

## JavaScript (Node.js)

Salvatore Previti created a Node.js wrapper, **roaring-node**, available at [https://github.com/SalvatorePreviti/roaring-node](https://github.com/SalvatorePreviti/roaring-node).

You can install it via npm with the following command:

```
npm install roaring
```

## Swift

Jérémie Piotte authored the [Swift wrapper](https://github.com/RoaringBitmap/SwiftRoaring).

## C\#

There is a C\# wrapper, **CRoaring.Net**, located at [https://github.com/k-wojcik/Roaring.Net](https://github.com/k-wojcik/Roaring.Net). This wrapper is compatible with Windows and Linux on x64 processors.

## Go (Golang)

A Go wrapper is available at the official RoaringBitmap GitHub organization: [https://github.com/RoaringBitmap/gocroaring](https://github.com/RoaringBitmap/gocroaring).

## Rust

Saulius Grigaliunas developed a Rust wrapper, **croaring-rs**, which can be found at [https://github.com/saulius/croaring-rs](https://github.com/saulius/croaring-rs).

## D

Yuce Tekol created a D wrapper, **droaring**, available at [https://github.com/yuce/droaring](https://github.com/yuce/droaring).

## Redis Module

Antonio Guilherme Ferreira Viggiano wrote a Redis Module integrating Roaring Bitmaps, available at [https://github.com/aviggiano/redis-roaring](https://github.com/aviggiano/redis-roaring).

## Zig

Justin Whear contributed a Zig wrapper, located at [https://github.com/jwhear/roaring-zig](https://github.com/jwhear/roaring-zig).


# Mailing list/discussion group

https://groups.google.com/forum/#!forum/roaring-bitmaps

# Contributing

When contributing a change to the project, please run `tools/run-clangcldocker.sh` after making any changes if you have docker and bash. A github action runs on all PRs to ensure formatting is consistent with this.

If you are using AI, please review our [AI usage policy](AI_USAGE_POLICY.md).

For large PRs, prefer smaller incremental PRs or request staged review.

Contributions are licensed under the project’s license. Ensure your work complies and does not infringe on third-party rights.

A compiler or static-analyzer warning is not a bug. Do not report such cases as bugs. We do accept pull requests if you want to silence warnings issued by code analyzers, however.

# Stars


[![Star History Chart](https://api.star-history.com/svg?repos=RoaringBitmap/CRoaring&type=Date)](https://www.star-history.com/#RoaringBitmap/CRoaring&Date)

# References about Roaring

- Daniel Lemire, Owen Kaser, Nathan Kurz, Luca Deri, Chris O'Hara, François Saint-Jacques, Gregory Ssi-Yan-Kai, Roaring Bitmaps: Implementation of an Optimized Software Library, Software: Practice and Experience Volume 48, Issue 4 April 2018 Pages 867-895 [arXiv:1709.07821](https://arxiv.org/abs/1709.07821)
-  Samy Chambi, Daniel Lemire, Owen Kaser, Robert Godin,
Better bitmap performance with Roaring bitmaps,
Software: Practice and Experience Volume 46, Issue 5, pages 709–719, May 2016  [arXiv:1402.6407](http://arxiv.org/abs/1402.6407)
- Daniel Lemire, Gregory Ssi-Yan-Kai, Owen Kaser, Consistently faster and smaller compressed bitmaps with Roaring, Software: Practice and Experience Volume 46, Issue 11, pages 1547-1569, November 2016 [arXiv:1603.06549](http://arxiv.org/abs/1603.06549)
- Samy Chambi, Daniel Lemire, Robert Godin, Kamel Boukhalfa, Charles Allen, Fangjin Yang, Optimizing Druid with Roaring bitmaps, IDEAS 2016, 2016. http://r-libre.teluq.ca/950/
