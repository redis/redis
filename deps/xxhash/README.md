
xxHash - Extremely fast hash algorithm
======================================

xxHash is an Extremely fast Hash algorithm, processing at RAM speed limits.
Code is highly portable, and produces hashes identical across all platforms (little / big endian).
The library includes the following algorithms :
- XXH32 : generates 32-bit hashes, using 32-bit arithmetic
- XXH64 : generates 64-bit hashes, using 64-bit arithmetic
- XXH3 (since `v0.8.0`): generates 64 or 128-bit hashes, using vectorized arithmetic.
  The 128-bit variant is called XXH128.

All variants successfully complete the [SMHasher](https://code.google.com/p/smhasher/wiki/SMHasher) test suite
which evaluates the quality of hash functions (collision, dispersion and randomness).
Additional tests, which evaluate more thoroughly speed and collision properties of 64-bit hashes, [are also provided](https://github.com/Cyan4973/xxHash/tree/dev/tests).

|Branch      |Status   |
|------------|---------|
|release     | [![Build Status](https://github.com/Cyan4973/xxHash/actions/workflows/ci.yml/badge.svg?branch=release)](https://github.com/Cyan4973/xxHash/actions?query=branch%3Arelease+) |
|dev         | [![Build Status](https://github.com/Cyan4973/xxHash/actions/workflows/ci.yml/badge.svg?branch=dev)](https://github.com/Cyan4973/xxHash/actions?query=branch%3Adev+) |


Benchmarks
-------------------------

The benchmarked reference system uses an Intel i7-9700K cpu, and runs Ubuntu x64 20.04.
The [open source benchmark program] is compiled with `clang` v10.0 using `-O3` flag.

| Hash Name     | Width | Bandwidth (GB/s) | Small Data Velocity | Quality | Comment |
| ---------     | ----- | ---------------- | ----- | --- | --- |
| __XXH3__ (SSE2) |  64 | 31.5 GB/s        | 133.1 | 10
| __XXH128__ (SSE2) | 128 | 29.6 GB/s      | 118.1 | 10
| _RAM sequential read_ | N/A | 28.0 GB/s  |   N/A | N/A | _for reference_
| City64        |    64 | 22.0 GB/s        |  76.6 | 10
| T1ha2         |    64 | 22.0 GB/s        |  99.0 |  9 | Slightly worse [collisions]
| City128       |   128 | 21.7 GB/s        |  57.7 | 10
| __XXH64__     |    64 | 19.4 GB/s        |  71.0 | 10
| SpookyHash    |    64 | 19.3 GB/s        |  53.2 | 10
| Mum           |    64 | 18.0 GB/s        |  67.0 |  9 | Slightly worse [collisions]
| __XXH32__     |    32 |  9.7 GB/s        |  71.9 | 10
| City32        |    32 |  9.1 GB/s        |  66.0 | 10
| Murmur3       |    32 |  3.9 GB/s        |  56.1 | 10
| SipHash       |    64 |  3.0 GB/s        |  43.2 | 10
| FNV64         |    64 |  1.2 GB/s        |  62.7 |  5 | Poor avalanche properties
| Blake2        |   256 |  1.1 GB/s        |   5.1 | 10 | Cryptographic
| SHA1          |   160 |  0.8 GB/s        |   5.6 | 10 | Cryptographic but broken
| MD5           |   128 |  0.6 GB/s        |   7.8 | 10 | Cryptographic but broken

[open source benchmark program]: https://github.com/Cyan4973/xxHash/tree/release/tests/bench
[collisions]: https://github.com/Cyan4973/xxHash/wiki/Collision-ratio-comparison#collision-study

note 1: Small data velocity is a _rough_ evaluation of algorithm's efficiency on small data. For more detailed analysis, please refer to next paragraph.

note 2: some algorithms feature _faster than RAM_ speed. In which case, they can only reach their full speed potential when input is already in CPU cache (L3 or better). Otherwise, they max out on RAM speed limit.

### Small data

Performance on large data is only one part of the picture.
Hashing is also very useful in constructions like hash tables and bloom filters.
In these use cases, it's frequent to hash a lot of small data (starting at a few bytes).
Algorithm's performance can be very different for such scenarios, since parts of the algorithm,
such as initialization or finalization, become fixed cost.
The impact of branch mis-prediction also becomes much more present.

XXH3 has been designed for excellent performance on both long and small inputs,
which can be observed in the following graph:

![XXH3, latency, random size](https://user-images.githubusercontent.com/750081/61976089-aedeab00-af9f-11e9-9239-e5375d6c080f.png)

For a more detailed analysis, please visit the wiki :
https://github.com/Cyan4973/xxHash/wiki/Performance-comparison#benchmarks-concentrating-on-small-data-

Quality
-------------------------

Speed is not the only property that matters.
Produced hash values must respect excellent dispersion and randomness properties,
so that any sub-section of it can be used to maximally spread out a table or index,
as well as reduce the amount of collisions to the minimal theoretical level, following the [birthday paradox].

`xxHash` has been tested with Austin Appleby's excellent SMHasher test suite,
and passes all tests, ensuring reasonable quality levels.
It also passes extended tests from [newer forks of SMHasher], featuring additional scenarios and conditions.

Finally, xxHash provides its own [massive collision tester](https://github.com/Cyan4973/xxHash/tree/dev/tests/collisions),
able to generate and compare billions of hashes to test the limits of 64-bit hash algorithms.
On this front too, xxHash features good results, in line with the [birthday paradox].
A more detailed analysis is documented [in the wiki](https://github.com/Cyan4973/xxHash/wiki/Collision-ratio-comparison).

[birthday paradox]: https://en.wikipedia.org/wiki/Birthday_problem
[newer forks of SMHasher]: https://github.com/rurban/smhasher


### Build modifiers

The following macros can be set at compilation time to modify `libxxhash`'s behavior. They are generally disabled by default.

- `XXH_INLINE_ALL`: Make all functions `inline`, implementation is directly included within `xxhash.h`.
                    Inlining functions is beneficial for speed, notably for small keys.
                    It's _extremely effective_ when key's length is expressed as _a compile time constant_,
                    with performance improvements observed in the +200% range .
                    See [this article](https://fastcompression.blogspot.com/2018/03/xxhash-for-small-keys-impressive-power.html) for details.
- `XXH_PRIVATE_API`: same outcome as `XXH_INLINE_ALL`. Still available for legacy support.
                     The name underlines that `XXH_*` symbol names will not be exported.
- `XXH_STATIC_LINKING_ONLY`: gives access to internal state declaration, required for static allocation.
                             Incompatible with dynamic linking, due to risks of ABI changes.
- `XXH_NAMESPACE`: Prefixes all symbols with the value of `XXH_NAMESPACE`.
                   This macro can only use compilable character set.
                   Useful to evade symbol naming collisions,
                   in case of multiple inclusions of xxHash's source code.
                   Client applications still use the regular function names,
                   as symbols are automatically translated through `xxhash.h`.
- `XXH_FORCE_ALIGN_CHECK`: Use a faster direct read path when input is aligned.
                           This option can result in dramatic performance improvement on architectures unable to load memory from unaligned addresses
                           when input to hash happens to be aligned on 32 or 64-bit boundaries.
                           It is (slightly) detrimental on platform with good unaligned memory access performance (same instruction for both aligned and unaligned accesses).
                           This option is automatically disabled on `x86`, `x64` and `aarch64`, and enabled on all other platforms.
- `XXH_FORCE_MEMORY_ACCESS`: The default method `0` uses a portable `memcpy()` notation.
                             Method `1` uses a gcc-specific `packed` attribute, which can provide better performance for some targets.
                             Method `2` forces unaligned reads, which is not standard compliant, but might sometimes be the only way to extract better read performance.
                             Method `3` uses a byteshift operation, which is best for old compilers which don't inline `memcpy()` or big-endian systems without a byteswap instruction.
- `XXH_CPU_LITTLE_ENDIAN`: By default, endianness is determined by a runtime test resolved at compile time.
                           If, for some reason, the compiler cannot simplify the runtime test, it can cost performance.
                           It's possible to skip auto-detection and simply state that the architecture is little-endian by setting this macro to 1.
                           Setting it to 0 states big-endian.
- `XXH_ENABLE_AUTOVECTORIZE`: Auto-vectorization may be triggered for XXH32 and XXH64, depending on cpu vector capabilities and compiler version.
                              Note: auto-vectorization tends to be triggered more easily with recent versions of `clang`.
                              For XXH32, SSE4.1 or equivalent (NEON) is enough, while XXH64 requires AVX512.
                              Unfortunately, auto-vectorization is generally detrimental to XXH performance.
                              For this reason, the xxhash source code tries to prevent auto-vectorization by default.
                              That being said, systems evolve, and this conclusion is not forthcoming.
                              For example, it has been reported that recent Zen4 cpus are more likely to improve performance with vectorization.
                              Therefore, should you prefer or want to test vectorized code, you can enable this flag:
                              it will remove the no-vectorization protection code, thus making it more likely for XXH32 and XXH64 to be auto-vectorized.
- `XXH32_ENDJMP`: Switch multi-branch finalization stage of XXH32 by a single jump.
                  This is generally undesirable for performance, especially when hashing inputs of random sizes.
                  But depending on exact architecture and compiler, a jump might provide slightly better performance on small inputs. Disabled by default.
- `XXH_IMPORT`: MSVC specific: should only be defined for dynamic linking, as it prevents linkage errors.
- `XXH_NO_STDLIB`: Disable invocation of `<stdlib.h>` functions, notably `malloc()` and `free()`.
                   `libxxhash`'s `XXH*_createState()` will always fail and return `NULL`.
                   But one-shot hashing (like `XXH32()`) or streaming using statically allocated states
                   still work as expected.
                   This build flag is useful for embedded environments without dynamic allocation.
- `XXH_memcpy`, `XXH_memset`, `XXH_memcmp` : redirect `memcpy()`, `memset()` and `memcmp()` to some user-selected symbol at compile time.
                   Redirecting all 3 removes the need to include `<string.h>` standard library.
- `XXH_NO_EXTERNC_GUARD`: When `xxhash.h` is compiled in C++ mode, removes the `extern "C" { .. }` block guard.
- `XXH_DEBUGLEVEL` : When set to any value >= 1, enables `assert()` statements.
                     This (slightly) slows down execution, but may help finding bugs during debugging sessions.

#### Binary size control
- `XXH_NO_XXH3` : removes symbols related to `XXH3` (both 64 & 128 bits) from generated binary.
                  `XXH3` is by far the largest contributor to `libxxhash` size,
                  so it's useful to reduce binary size for applications which do not employ `XXH3`.
- `XXH_NO_LONG_LONG`: removes compilation of algorithms relying on 64-bit `long long` types
                      which include `XXH3` and `XXH64`.
                      Only `XXH32` will be compiled.
                      Useful for targets (architectures and compilers) without 64-bit support.
- `XXH_NO_STREAM`: Disables the streaming API, limiting the library to single shot variants only.
- `XXH_NO_INLINE_HINTS`: By default, xxHash uses `__attribute__((always_inline))` and `__forceinline` to improve performance at the cost of code size.
                         Defining this macro to 1 will mark all internal functions as `static`, allowing the compiler to decide whether to inline a function or not.
                         This is very useful when optimizing for smallest binary size,
                         and is automatically defined when compiling with `-O0`, `-Os`, `-Oz`, or `-fno-inline` on GCC and Clang.
                         It may also be required to successfully compile using `-Og`, depending on compiler version.
- `XXH_SIZE_OPT`: `0`: default, optimize for speed
                  `1`: default for `-Os` and `-Oz`: disables some speed hacks for size optimization
                  `2`: makes code as small as possible, performance may cry

#### Build modifiers specific for XXH3
- `XXH_VECTOR` : manually select a vector instruction set (default: auto-selected at compilation time). Available instruction sets are `XXH_SCALAR`, `XXH_SSE2`, `XXH_AVX2`, `XXH_AVX512`, `XXH_NEON` and `XXH_VSX`. Compiler may require additional flags to ensure proper support (for example, `gcc` on x86_64 requires `-mavx2` for `AVX2`, or `-mavx512f` for `AVX512`).
- `XXH_PREFETCH_DIST` : select prefetching distance. For close-to-metal adaptation to specific hardware platforms. XXH3 only.
- `XXH_NO_PREFETCH` : disable prefetching. Some platforms or situations may perform better without prefetching. XXH3 only.

#### Build modifiers for `xxhsum` CLI
- `XXH_1ST_SPEED_TARGET` : select an initial speed target, expressed in MB/s, for the first speed test in benchmark mode. Benchmark will adjust the target at subsequent iterations, but the first test is made "blindly" by targeting this speed. Currently conservatively set to 10 MB/s, to support very slow (emulated) platforms.

#### Makefile variables
When compiling the Command Line Interface `xxhsum` using `make`, the following environment variables can also be set :
- `DISPATCH=1` : use `xxh_x86dispatch.c`, select at runtime between `scalar`, `sse2`, `avx2` or `avx512` instruction set. This option is only valid for `x86`/`x64` systems. It is enabled by default when target `x86`/`x64` is detected. It can be forcefully turned off using `DISPATCH=0`.
- `LIBXXH_DISPATCH=1` : same idea, implemented a runtime vector extension detector, but within `libxxhash`. This parameter is disabled by default. When enabled (only valid for `x86`/`x64` systems), new symbols published in `xxh_x86dispatch.h` become accessible. At the time of this writing, it's required to include `xxh_x86dispatch.h` in order to access the symbols with runtime vector extension detection.
- `NODE_JS=1` : When compiling `xxhsum` for Node.js with Emscripten, this links the `NODERAWFS` library for unrestricted filesystem access and patches `isatty` to make the command line utility correctly detect the terminal. This does make the binary specific to Node.js.

### Building xxHash - Using vcpkg

You can download and install xxHash using the [vcpkg](https://github.com/Microsoft/vcpkg) dependency manager:

    git clone https://github.com/Microsoft/vcpkg.git
    cd vcpkg
    ./bootstrap-vcpkg.sh
    ./vcpkg integrate install
    ./vcpkg install xxhash

The xxHash port in vcpkg is kept up to date by Microsoft team members and community contributors. If the version is out of date, please [create an issue or pull request](https://github.com/Microsoft/vcpkg) on the vcpkg repository.

### Example

The simplest example calls xxhash 64-bit variant as a one-shot function
generating a hash value from a single buffer, and invoked from a C/C++ program:

```C
#include "xxhash.h"

    (...)
    XXH64_hash_t hash = XXH64(buffer, size, seed);
}
```

Streaming variant is more involved, but makes it possible to provide data incrementally:

```C
#include "stdlib.h"   /* abort() */
#include "xxhash.h"


XXH64_hash_t calcul_hash_streaming(FileHandler fh)
{
    /* create a hash state */
    XXH64_state_t* const state = XXH64_createState();
    if (state==NULL) abort();

    size_t const bufferSize = SOME_SIZE;
    void* const buffer = malloc(bufferSize);
    if (buffer==NULL) abort();

    /* Initialize state with selected seed */
    XXH64_hash_t const seed = 0;   /* or any other value */
    if (XXH64_reset(state, seed) == XXH_ERROR) abort();

    /* Feed the state with input data, any size, any number of times */
    (...)
    while ( /* some data left */ ) {
        size_t const length = get_more_data(buffer, bufferSize, fh);
        if (XXH64_update(state, buffer, length) == XXH_ERROR) abort();
        (...)
    }
    (...)

    /* Produce the final hash value */
    XXH64_hash_t const hash = XXH64_digest(state);

    /* State could be re-used; but in this example, it is simply freed  */
    free(buffer);
    XXH64_freeState(state);

    return hash;
}
```


### License

The library files `xxhash.c` and `xxhash.h` are BSD licensed.
The utility `xxhsum` is GPL licensed.


### Other programming languages

Beyond the C reference version,
xxHash is also available from many different programming languages,
thanks to great contributors.
They are [listed here](http://www.xxhash.com/#other-languages).


### Packaging status

Many distributions bundle a package manager
which allows easy xxhash installation as both a `libxxhash` library
and `xxhsum` command line interface.

[![Packaging status](https://repology.org/badge/vertical-allrepos/xxhash.svg)](https://repology.org/project/xxhash/versions)


### Special Thanks

- Takayuki Matsuoka, aka @t-mat, for creating `xxhsum -c` and great support during early xxh releases
- Mathias Westerdahl, aka @JCash, for introducing the first version of `XXH64`
- Devin Hussey, aka @easyaspi314, for incredible low-level optimizations on `XXH3` and `XXH128`
