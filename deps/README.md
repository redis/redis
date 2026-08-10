This directory contains all Redis dependencies, except for the libc that
should be provided by the operating system.

* **Jemalloc** is our memory allocator, used as replacement for libc malloc on Linux by default. It has good performances and excellent fragmentation behavior. This component is upgraded from time to time.
* **hiredis** is the official C client library for Redis. It is used by redis-cli, redis-benchmark and Redis Sentinel. It is part of the Redis official ecosystem but is developed externally from the Redis repository, so we just upgrade it as needed.
* **linenoise** is a readline replacement. It is developed by the same authors of Redis but is managed as a separated project and updated as needed.
* **lua** is Lua 5.1 with minor changes for security and additional libraries.
* **hdr_histogram** Used for per-command latency tracking histograms.
* **CRoaring** is the C implementation of Roaring bitmaps used by native Redis
  bitmap encodings. Redis vendors the C headers and source from upstream and
  builds it only through the Redis dependency Makefile.

How to upgrade the above dependencies
===

Jemalloc
---

Jemalloc is modified with changes that allow us to implement the Redis
active defragmentation logic. However this feature of Redis is not mandatory
and Redis is able to understand if the Jemalloc version it is compiled
against supports such Redis-specific modifications. So in theory, if you
are not interested in the active defragmentation, you can replace Jemalloc
just following these steps:

1. Remove the jemalloc directory.
2. Substitute it with the new jemalloc source tree.
3. Edit the Makefile located in the same directory as the README you are
   reading, and change the --with-version in the Jemalloc configure script
   options with the version you are using. This is required because otherwise
   Jemalloc configuration script is broken and will not work nested in another
   git repository.

However note that we change Jemalloc settings via the `configure` script of Jemalloc using the `--with-lg-quantum` option, setting it to the value of 3 instead of 4. This provides us with more size classes that better suit the Redis data structures, in order to gain memory efficiency.

If you want to upgrade Jemalloc while also providing support for
active defragmentation, in addition to the above steps you need to perform
the following additional steps:

5. In Jemalloc tree, file `include/jemalloc/jemalloc_macros.h.in`, make sure
   to add `#define JEMALLOC_FRAG_HINT`.
6. Implement the function `je_get_defrag_hint()` inside `src/jemalloc.c`. You
   can see how it is implemented in the current Jemalloc source tree shipped
   with Redis, and rewrite it according to the new Jemalloc internals, if they
   changed, otherwise you could just copy the old implementation if you are
   upgrading just to a similar version of Jemalloc.

#### Updating/upgrading jemalloc

The jemalloc directory is pulled as a subtree from the upstream jemalloc github repo. To update it you should run from the project root:

1. `git subtree pull --prefix deps/jemalloc https://github.com/jemalloc/jemalloc.git <version-tag> --squash`<br>
This should hopefully merge the local changes into the new version.
2. In case any conflicts arise (due to our changes) you'll need to resolve them and commit.
3. Reconfigure jemalloc:<br>
```sh
rm deps/jemalloc/VERSION deps/jemalloc/configure
cd deps/jemalloc
./autogen.sh --with-version=<version-tag>-0-g0
```
4. Update jemalloc's version in `deps/Makefile`: search for "`--with-version=<old-version-tag>-0-g0`" and update it accordingly.
5. Commit the changes (VERSION,configure,Makefile).

Hiredis
---

Hiredis is used by Sentinel, `redis-cli` and `redis-benchmark`. Like Redis, uses the SDS string library, but not necessarily the same version. In order to avoid conflicts, this version has all SDS identifiers prefixed by `hi`.

1. `git subtree pull --prefix deps/hiredis https://github.com/redis/hiredis.git <version-tag> --squash`<br>
This should hopefully merge the local changes into the new version.
2. Conflicts will arise (due to our changes) you'll need to resolve them and commit.

Linenoise
---

Linenoise is rarely upgraded as needed. The upgrade process is trivial since
Redis uses a non modified version of linenoise, so to upgrade just do the
following:

1. Remove the linenoise directory.
2. Substitute it with the new linenoise source tree.

Lua
---

We use Lua 5.1 and no upgrade is planned currently, since we don't want to break
Lua scripts for new Lua features: in the context of Redis Lua scripts the
capabilities of 5.1 are usually more than enough, the release is rock solid,
and we definitely don't want to break old scripts.

So upgrading of Lua is up to the Redis project maintainers and should be a
manual procedure performed by taking a diff between the different versions.

Currently we have at least the following differences between official Lua 5.1
and our version:

1. Makefile is modified to allow a different compiler than GCC.
2. We have the implementation source code, and directly link to the following external libraries: `lua_cjson.o`, `lua_struct.o`, `lua_cmsgpack.o` and `lua_bit.o`.
3. There is a security fix in `ldo.c`, line 498: The check for `LUA_SIGNATURE[0]` is removed in order to avoid direct bytecode execution.

Hdr_Histogram
---

Updated source can be found here: https://github.com/HdrHistogram/HdrHistogram_c
We use a customized version based on master branch commit e4448cf6d1cd08fff519812d3b1e58bd5a94ac42.
1. Compare all changes under /hdr_histogram directory to upstream master commit e4448cf6d1cd08fff519812d3b1e58bd5a94ac42
2. Copy updated files from newer version onto files in /hdr_histogram.
3. Apply the changes from 1 above to the updated files.

CRoaring
---

Updated source can be found here: https://github.com/RoaringBitmap/CRoaring
Redis currently vendors CRoaring v4.7.2.

Redis bitmap persistence contract:

* `RDB_TYPE_BITMAP` stores the bitmap's logical byte length followed by an RDB
  string containing CRoaring's 64-bit portable serialization. The logical
  length is separate because trailing zero bits are observable through Redis
  bitmap commands but are not represented by Roaring containers.
* The CRoaring portable blob follows the RoaringFormatSpec, uses canonical
  little-endian fields on every architecture, and is shared by RDB snapshots,
  DUMP/RESTORE, and the RDB payloads used by AOF persistence.
* Persistence never expands a sparse bitmap to its logical string length. A
  bitmap with a high set-bit offset therefore remains proportional to its
  resident Roaring containers instead of its highest bit.
* Externally exposed dense raw-byte materialization is limited to
  `DEBUG BITMAP-RAW` and by `proto-max-bulk-len`. The internal
  mixed-representation BITOP optimization also materializes bounded raw
  buffers, but has its own 1 MiB aggregate limit and does not affect the
  persistence format.

1. Replace `deps/croaring/include` with upstream `include`.
2. Replace `deps/croaring/src` with upstream C source/header files from `src`;
   Redis does not use upstream CMake files.
3. Update `deps/croaring/LICENSE`, `AUTHORS`, `README.md`, and `SECURITY.md`.
4. Check whether upstream added, removed, or renamed C sources and mirror the
   source list in `deps/croaring/Makefile`.
5. Re-apply the local Redis changes below unless upstream has independently
   fixed them; they exist to keep CI green on platforms upstream does not
   exercise the same way.

Local changes compared to pristine upstream v4.7.2:

In `deps/croaring/Makefile`:

* CRoaring is compiled with hidden symbol visibility so Redis' Linux
  `-rdynamic` link does not expose the vendored symbols to modules.
* The dependency build tracks its effective compiler flags and rebuilds when
  they change. It also disables x86 and NEON implementation headers because
  Redis builds only CRoaring's portable implementation.
* The `test` target forces the non-atomic refcount implementation and runs the
  regression test in `tests/refcount_none.c`.

In `include/roaring/portability.h`:

* Added a `__has_include` polyfill (`#ifndef __has_include` /
  `#define __has_include(x) 0`) for compilers without the builtin.
* Replaced upstream's malformed `#ifndef !defined(__BYTE_ORDER__) || ...`
  guard and reworked the endian/byteswap include chain around it. Redis' copy
  also recognizes the non-GNU
  `__BYTE_ORDER`, `_BYTE_ORDER`, and `BYTE_ORDER` macro families and errors at
  build time when CRoaring cannot determine target endianness.
* Added a `CROARING_ATOMIC_IMPL_GCC` fallback using `__sync` builtins for
  toolchains without C11 atomics.
* Fixed the non-atomic fallback's refcount decrement to report when the
  dereferenced counter reaches zero. Keep this patch until an upstream release
  contains the same fix.
* Gated `CROARING_ALLOW_UNALIGNED` to
  `defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 5)`.

In `src/roaring64.c` and the added `include/roaring/roaring64_internal.h`:

* Moved the private `struct roaring64_bitmap_s` definition out of
  `roaring64.c` into the new shared internal header (plus small leaf-decoding
  helpers) so `src/bitroar.c` can walk every allocation behind a 64-bit bitmap
  for MEMORY USAGE accounting, fork-child page dismissal and
  active defragmentation, exactly like it does for the 32-bit
  `roaring_bitmap_t` whose layout upstream exposes publicly. The long-term
  plan is to propose an allocation-visitor/relocation helper API to upstream
  CRoaring so future version bumps do not depend on this internal header.
