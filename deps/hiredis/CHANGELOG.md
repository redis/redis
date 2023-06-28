## [1.1.0](https://github.com/redis/hiredis/tree/v1.1.0) - (2022-11-15)

Announcing Hiredis v1.1.0 GA with better SSL convenience, new async adapters and a great many bug fixes.

**NOTE**:  Hiredis can now return `nan` in addition to `-inf` and `inf` when returning a `REDIS_REPLY_DOUBLE`. 

## 🐛 Bug Fixes

- Add support for nan in RESP3 double [@filipecosta90](https://github.com/filipecosta90) 
  ([\#1133](https://github.com/redis/hiredis/pull/1133))

## 🧰 Maintenance

- Add an example that calls redisCommandArgv [@michael-grunder](https://github.com/michael-grunder)
  ([\#1140](https://github.com/redis/hiredis/pull/1140))
- fix flag reference [@pata00](https://github.com/pata00) ([\#1136](https://github.com/redis/hiredis/pull/1136))
- Make freeing a NULL redisAsyncContext a no op. [@michael-grunder](https://github.com/michael-grunder) 
  ([\#1135](https://github.com/redis/hiredis/pull/1135))
- CI updates ([@bjosv](https://github.com/redis/bjosv) ([\#1139](https://github.com/redis/hiredis/pull/1139))


## Contributors
We'd like to thank all the contributors who worked on this release!

<a href="https://github.com/bjsov"><img src="https://github.com/bjosv.png" width="32" height="32"></a>
<a href="https://github.com/filipecosta90"><img src="https://github.com/filipecosta90.png" width="32" height="32"></a>
<a href="https://github.com/michael-grunder"><img src="https://github.com/michael-grunder.png" width="32" height="32"></a>
<a href="https://github.com/pata00"><img src="https://github.com/pata00.png" width="32" height="32"></a>

## [1.1.0-rc1](https://github.com/redis/hiredis/tree/v1.1.0-rc1) - (2022-11-06)

Announcing Hiredis v1.1.0-rc1, with better SSL convenience, new async adapters, and a great many bug fixes.

## 🚀 New Features

- Add possibility to prefer IPv6, IPv4 or unspecified [@zuiderkwast](https://github.com/zuiderkwast)
  ([\#1096](https://github.com/redis/hiredis/pull/1096))
- Add adapters/libhv [@ithewei](https://github.com/ithewei) ([\#904](https://github.com/redis/hiredis/pull/904))
- Add timeout support to libhv adapter. [@michael-grunder](https://github.com/michael-grunder) ([\#1109](https://github.com/redis/hiredis/pull/1109))
- set default SSL verification path [@adobeturchenko](https://github.com/adobeturchenko) ([\#928](https://github.com/redis/hiredis/pull/928))
- Introduce .close method for redisContextFuncs [@pizhenwei](https://github.com/pizhenwei) ([\#1094](https://github.com/redis/hiredis/pull/1094))
- Make it possible to set SSL verify mode [@stanhu](https://github.com/stanhu) ([\#1085](https://github.com/redis/hiredis/pull/1085))
- Polling adapter and example [@kristjanvalur](https://github.com/kristjanvalur) ([\#932](https://github.com/redis/hiredis/pull/932))
- Unsubscribe handling in async [@bjosv](https://github.com/bjosv) ([\#1047](https://github.com/redis/hiredis/pull/1047))
- Add timeout support for libuv adapter [@MichaelSuen-thePointer](https://github.com/@MichaelSuenthePointer) ([\#1016](https://github.com/redis/hiredis/pull/1016))

## 🐛 Bug Fixes

- Update for MinGW cross compile [@bit0fun](https://github.com/bit0fun) ([\#1127](https://github.com/redis/hiredis/pull/1127))
- fixed CPP build error with adapters/libhv.h [@mtdxc](https://github.com/mtdxc) ([\#1125](https://github.com/redis/hiredis/pull/1125))
- Fix protocol error
  [@michael-grunder](https://github.com/michael-grunder),
  [@mtuleika-appcast](https://github.com/mtuleika-appcast) ([\#1106](https://github.com/redis/hiredis/pull/1106))
- Use a windows specific keepalive function. [@michael-grunder](https://github.com/michael-grunder) ([\#1104](https://github.com/redis/hiredis/pull/1104))
- Fix CMake config path on Linux. [@xkszltl](https://github.com/xkszltl) ([\#989](https://github.com/redis/hiredis/pull/989))
- Fix potential fault at createDoubleObject [@afcidk](https://github.com/afcidk) ([\#964](https://github.com/redis/hiredis/pull/964))
- Fix some undefined behavior [@jengab](https://github.com/jengab) ([\#1091](https://github.com/redis/hiredis/pull/1091))
- Copy OOM errors to redisAsyncContext when finding subscribe callback [@bjosv](https://github.com/bjosv) ([\#1090](https://github.com/redis/hiredis/pull/1090))
- Maintain backward compatibility with our onConnect callback. [@michael-grunder](https://github.com/michael-grunder) ([\#1087](https://github.com/redis/hiredis/pull/1087))
- Fix PUSH handler tests for Redis >= 7.0.5 [@michael-grunder](https://github.com/michael-grunder) ([\#1121](https://github.com/redis/hiredis/pull/1121))
- fix heap-buffer-overflow [@zhangtaoXT5](https://github.com/zhangtaoXT5) ([\#957](https://github.com/redis/hiredis/pull/957))
- Fix heap-buffer-overflow issue in redisvFormatCommad [@bjosv](https://github.com/bjosv) ([\#1097](https://github.com/redis/hiredis/pull/1097))
- Polling adapter requires sockcompat.h [@michael-grunder](https://github.com/michael-grunder) ([\#1095](https://github.com/redis/hiredis/pull/1095))
- Illumos test fixes, error message difference for bad hostname test. [@devnexen](https://github.com/devnexen) ([\#901](https://github.com/redis/hiredis/pull/901))
- Remove semicolon after do-while in \_EL\_CLEANUP [@sundb](https://github.com/sundb) ([\#905](https://github.com/redis/hiredis/pull/905))
- Stability: Support calling redisAsyncCommand and redisAsyncDisconnect from the onConnected callback [@kristjanvalur](https://github.com/kristjanvalur)
  ([\#931](https://github.com/redis/hiredis/pull/931))
- Fix async connect on Windows [@kristjanvalur](https://github.com/kristjanvalur) ([\#1073](https://github.com/redis/hiredis/pull/1073))
- Fix tests so they work for Redis 7.0 [@michael-grunder](https://github.com/michael-grunder) ([\#1072](https://github.com/redis/hiredis/pull/1072))
- Fix warnings on Win64 [@orgads](https://github.com/orgads) ([\#1058](https://github.com/redis/hiredis/pull/1058))
- Handle push notifications before or after reply. [@yossigo](https://github.com/yossigo) ([\#1062](https://github.com/redis/hiredis/pull/1062))
- Update hiredis sds with improvements found in redis [@bjosv](https://github.com/bjosv) ([\#1045](https://github.com/redis/hiredis/pull/1045))
- Avoid incorrect call to the previous reply's callback [@bjosv](https://github.com/bjosv) ([\#1040](https://github.com/redis/hiredis/pull/1040))
- fix building on AIX and SunOS [\#1031](https://github.com/redis/hiredis/pull/1031) ([@scddev](https://github.com/scddev))
- Allow sending commands after sending an unsubscribe [@bjosv](https://github.com/bjosv) ([\#1036](https://github.com/redis/hiredis/pull/1036))
- Correction for command timeout during pubsub [@bjosv](https://github.com/bjosv) ([\#1038](https://github.com/redis/hiredis/pull/1038))
- Fix adapters/libevent.h compilation for 64-bit Windows [@pbtummillo](https://github.com/pbtummillo) ([\#937](https://github.com/redis/hiredis/pull/937))
- Fix integer overflow when format command larger than 4GB [@sundb](https://github.com/sundb) ([\#1030](https://github.com/redis/hiredis/pull/1030))
- Handle array response during subscribe in RESP3 [@bjosv](https://github.com/bjosv) ([\#1014](https://github.com/redis/hiredis/pull/1014))
- Support PING while subscribing (RESP2) [@bjosv](https://github.com/bjosv) ([\#1027](https://github.com/redis/hiredis/pull/1027))

## 🧰 Maintenance

- CI fixes in preparation of release [@michael-grunder](https://github.com/michael-grunder) ([\#1130](https://github.com/redis/hiredis/pull/1130))
- Add do while(0) (protection for macros [@afcidk](https://github.com/afcidk) [\#959](https://github.com/redis/hiredis/pull/959))
- Fixup of PR734: Coverage of hiredis.c [@bjosv](https://github.com/bjosv) ([\#1124](https://github.com/redis/hiredis/pull/1124))
- CMake corrections for building on Windows [@bjosv](https://github.com/bjosv) ([\#1122](https://github.com/redis/hiredis/pull/1122))
- Install on windows fixes [@bjosv](https://github.com/bjosv) ([\#1117](https://github.com/redis/hiredis/pull/1117))
- Add libhv example to our standard Makefile [@michael-grunder](https://github.com/michael-grunder) ([\#1108](https://github.com/redis/hiredis/pull/1108))
- Additional include directory given by pkg-config [@bjosv](https://github.com/bjosv) ([\#1118](https://github.com/redis/hiredis/pull/1118))
- Use __attribute__ when building with Clang on Windows [@bjosv](https://github.com/bjosv) ([\#1115](https://github.com/redis/hiredis/pull/1115))
- Minor refactor [@michael-grunder](https://github.com/michael-grunder) ([\#1110](https://github.com/redis/hiredis/pull/1110))
- Fix pkgconfig result for hiredis_ssl [@bjosv](https://github.com/bjosv) ([\#1107](https://github.com/redis/hiredis/pull/1107))
- Update documentation to explain redisConnectWithOptions. [@michael-grunder](https://github.com/michael-grunder) ([\#1099](https://github.com/redis/hiredis/pull/1099))
- uvadapter: reduce number of uv_poll_start calls [@noxiouz](https://github.com/noxiouz) ([\#1098](https://github.com/redis/hiredis/pull/1098))
- Regression test for off-by-one parsing error [@bugwz](https://github.com/bugwz) ([\#1092](https://github.com/redis/hiredis/pull/1092))
- CMake: remove dict.c form hiredis_sources [@Lipraxde](https://github.com/Lipraxde) ([\#1055](https://github.com/redis/hiredis/pull/1055))
- Do store command timeout in the context for redisSetTimeout [@catterer](https://github.com/catterer) ([\#593](https://github.com/redis/hiredis/pull/593), [\#1093](https://github.com/redis/hiredis/pull/1093))
- Add GitHub Actions CI workflow for hiredis: Arm, Arm64, 386, windows. [@kristjanvalur](https://github.com/kristjanvalur) ([\#943](https://github.com/redis/hiredis/pull/943))
- CI: bump macOS runner version [@SukkaW](https://github.com/SukkaW) ([\#1079](https://github.com/redis/hiredis/pull/1079))
- Support for generating release notes [@chayim](https://github.com/chayim) ([\#1083](https://github.com/redis/hiredis/pull/1083))
- Improve example for SSL initialization in README.md [@stanhu](https://github.com/stanhu) ([\#1084](https://github.com/redis/hiredis/pull/1084))
- Fix README typos [@bjosv](https://github.com/bjosv) ([\#1080](https://github.com/redis/hiredis/pull/1080))
- fix cmake version [@smmir-cent](https://github.com/@smmircent) ([\#1050](https://github.com/redis/hiredis/pull/1050))
- Use the same name for static and shared libraries [@orgads](https://github.com/orgads) ([\#1057](https://github.com/redis/hiredis/pull/1057))
- Embed debug information in windows static .lib file [@kristjanvalur](https://github.com/kristjanvalur) ([\#1054](https://github.com/redis/hiredis/pull/1054))
- Improved async documentation [@kristjanvalur](https://github.com/kristjanvalur) ([\#1074](https://github.com/redis/hiredis/pull/1074))
- Use official repository for redis package. [@yossigo](https://github.com/yossigo) ([\#1061](https://github.com/redis/hiredis/pull/1061))
- Whitelist hiredis repo path in cygwin [@michael-grunder](https://github.com/michael-grunder) ([\#1063](https://github.com/redis/hiredis/pull/1063))
- CentOS 8 is EOL, switch to RockyLinux [@michael-grunder](https://github.com/michael-grunder) ([\#1046](https://github.com/redis/hiredis/pull/1046))
- CMakeLists.txt: allow building without a C++ compiler [@ffontaine](https://github.com/ffontaine) ([\#872](https://github.com/redis/hiredis/pull/872))
- Makefile: move SSL options into a block and refine rules [@pizhenwei](https://github.com/pizhenwei) ([\#997](https://github.com/redis/hiredis/pull/997))
- Update CMakeLists.txt for more portability [@EricDeng1001](https://github.com/EricDeng1001) ([\#1005](https://github.com/redis/hiredis/pull/1005))
- FreeBSD build fixes + CI [@michael-grunder](https://github.com/michael-grunder) ([\#1026](https://github.com/redis/hiredis/pull/1026))
- Add asynchronous test for pubsub using RESP3 [@bjosv](https://github.com/bjosv) ([\#1012](https://github.com/redis/hiredis/pull/1012))
- Trigger CI failure when Valgrind issues are found [@bjosv](https://github.com/bjosv) ([\#1011](https://github.com/redis/hiredis/pull/1011))
- Move to using make directly in Cygwin [@michael-grunder](https://github.com/michael-grunder) ([\#1020](https://github.com/redis/hiredis/pull/1020))
- Add asynchronous API tests [@bjosv](https://github.com/bjosv) ([\#1010](https://github.com/redis/hiredis/pull/1010))
- Correcting the build target `coverage` for enabled SSL [@bjosv](https://github.com/bjosv) ([\#1009](https://github.com/redis/hiredis/pull/1009))
- GH Actions: Run SSL tests during CI [@bjosv](https://github.com/bjosv) ([\#1008](https://github.com/redis/hiredis/pull/1008))
- GH: Actions - Add valgrind and CMake [@michael-grunder](https://github.com/michael-grunder) ([\#1004](https://github.com/redis/hiredis/pull/1004))
- Add Centos8 tests in GH Actions [@michael-grunder](https://github.com/michael-grunder) ([\#1001](https://github.com/redis/hiredis/pull/1001))
- We should run actions on PRs [@michael-grunder](https://github.com/michael-grunder) (([\#1000](https://github.com/redis/hiredis/pull/1000))
- Add Cygwin test in GitHub actions [@michael-grunder](https://github.com/michael-grunder) ([\#999](https://github.com/redis/hiredis/pull/999))
- Add Windows tests in GitHub actions [@michael-grunder](https://github.com/michael-grunder) ([\#996](https://github.com/redis/hiredis/pull/996))
- Switch to GitHub actions [@michael-grunder](https://github.com/michael-grunder) ([\#995](https://github.com/redis/hiredis/pull/995))
- Minor refactor of CVE-2021-32765 fix. [@michael-grunder](https://github.com/michael-grunder) ([\#993](https://github.com/redis/hiredis/pull/993))
- Remove extra comma from CMake var. [@xkszltl](https://github.com/xkszltl) ([\#988](https://github.com/redis/hiredis/pull/988))
- Add REDIS\_OPT\_PREFER\_UNSPEC [@michael-grunder](https://github.com/michael-grunder) ([\#1101](https://github.com/redis/hiredis/pull/1101))

## Contributors
We'd like to thank all the contributors who worked on this release!

<a href="https://github.com/EricDeng1001"><img src="https://github.com/EricDeng1001.png" width="32" height="32"></a>
<a href="https://github.com/Lipraxde"><img src="https://github.com/Lipraxde.png" width="32" height="32"></a>
<a href="https://github.com/MichaelSuen-thePointer"><img src="https://github.com/MichaelSuen-thePointer.png" width="32" height="32"></a>
<a href="https://github.com/SukkaW"><img src="https://github.com/SukkaW.png" width="32" height="32"></a>
<a href="https://github.com/adobeturchenko"><img src="https://github.com/adobeturchenko.png" width="32" height="32"></a>
<a href="https://github.com/afcidk"><img src="https://github.com/afcidk.png" width="32" height="32"></a>
<a href="https://github.com/bit0fun"><img src="https://github.com/bit0fun.png" width="32" height="32"></a>
<a href="https://github.com/bjosv"><img src="https://github.com/bjosv.png" width="32" height="32"></a>
<a href="https://github.com/bugwz"><img src="https://github.com/bugwz.png" width="32" height="32"></a>
<a href="https://github.com/catterer"><img src="https://github.com/catterer.png" width="32" height="32"></a>
<a href="https://github.com/chayim"><img src="https://github.com/chayim.png" width="32" height="32"></a>
<a href="https://github.com/devnexen"><img src="https://github.com/devnexen.png" width="32" height="32"></a>
<a href="https://github.com/ffontaine"><img src="https://github.com/ffontaine.png" width="32" height="32"></a>
<a href="https://github.com/ithewei"><img src="https://github.com/ithewei.png" width="32" height="32"></a>
<a href="https://github.com/jengab"><img src="https://github.com/jengab.png" width="32" height="32"></a>
<a href="https://github.com/kristjanvalur"><img src="https://github.com/kristjanvalur.png" width="32" height="32"></a>
<a href="https://github.com/michael-grunder"><img src="https://github.com/michael-grunder.png" width="32" height="32"></a>
<a href="https://github.com/noxiouz"><img src="https://github.com/noxiouz.png" width="32" height="32"></a>
<a href="https://github.com/mtdxc"><img src="https://github.com/mtdxc.png" width="32" height="32"></a>
<a href="https://github.com/orgads"><img src="https://github.com/orgads.png" width="32" height="32"></a>
<a href="https://github.com/pbtummillo"><img src="https://github.com/pbtummillo.png" width="32" height="32"></a>
<a href="https://github.com/pizhenwei"><img src="https://github.com/pizhenwei.png" width="32" height="32"></a>
<a href="https://github.com/scddev"><img src="https://github.com/scddev.png" width="32" height="32"></a>
<a href="https://github.com/smmir-cent"><img src="https://github.com/smmir-cent.png" width="32" height="32"></a>
<a href="https://github.com/stanhu"><img src="https://github.com/stanhu.png" width="32" height="32"></a>
<a href="https://github.com/sundb"><img src="https://github.com/sundb.png" width="32" height="32"></a>
<a href="https://github.com/vturchenko"><img src="https://github.com/vturchenko.png" width="32" height="32"></a>
<a href="https://github.com/xkszltl"><img src="https://github.com/xkszltl.png" width="32" height="32"></a>
<a href="https://github.com/yossigo"><img src="https://github.com/yossigo.png" width="32" height="32"></a>
<a href="https://github.com/zhangtaoXT5"><img src="https://github.com/zhangtaoXT5.png" width="32" height="32"></a>
<a href="https://github.com/zuiderkwast"><img src="https://github.com/zuiderkwast.png" width="32" height="32"></a>

## [1.0.2](https://github.com/redis/hiredis/tree/v1.0.2) - (2021-10-07)

Announcing Hiredis v1.0.2, which fixes CVE-2021-32765 but returns the SONAME to the correct value of `1.0.0`.

- [Revert SONAME bump](https://github.com/redis/hiredis/commit/d4e6f109a064690cde64765c654e679fea1d3548)
  ([Michael Grunder](https://github.com/michael-grunder))

## [1.0.1](https://github.com/redis/hiredis/tree/v1.0.1) - (2021-10-04)

<span style="color:red">This release erroneously bumped the SONAME, please use [1.0.2](https://github.com/redis/hiredis/tree/v1.0.2)</span>

Announcing Hiredis v1.0.1, a security release fixing CVE-2021-32765

- Fix for [CVE-2021-32765](https://github.com/redis/hiredis/security/advisories/GHSA-hfm9-39pp-55p2)
  [commit](https://github.com/redis/hiredis/commit/76a7b10005c70babee357a7d0f2becf28ec7ed1e)
  ([Yossi Gottlieb](https://github.com/yossigo))

_Thanks to [Yossi Gottlieb](https://github.com/yossigo) for the security fix and to [Microsoft Security Vulnerability Research](https://www.microsoft.com/en-us/msrc/msvr) for finding the bug._ :sparkling_heart:

## [1.0.0](https://github.com/redis/hiredis/tree/v1.0.0) - (2020-08-03)

Announcing Hiredis v1.0.0, which adds support for RESP3, SSL connections, allocator injection, and better Windows support! :tada:

_A big thanks to everyone who helped with this release.  The following list includes everyone who contributed at least five lines, sorted by lines contributed._ :sparkling_heart:

[Michael Grunder](https://github.com/michael-grunder), [Yossi Gottlieb](https://github.com/yossigo),
[Mark Nunberg](https://github.com/mnunberg), [Marcus Geelnard](https://github.com/mbitsnbites),
[Justin Brewer](https://github.com/justinbrewer), [Valentino Geron](https://github.com/valentinogeron),
[Minun Dragonation](https://github.com/dragonation), [Omri Steiner](https://github.com/OmriSteiner),
[Sangmoon Yi](https://github.com/jman-krafton), [Jinjiazh](https://github.com/jinjiazhang),
[Odin Hultgren Van Der Horst](https://github.com/Miniwoffer), [Muhammad Zahalqa](https://github.com/tryfinally),
[Nick Rivera](https://github.com/heronr), [Qi Yang](https://github.com/movebean),
[kevin1018](https://github.com/kevin1018)

[Full Changelog](https://github.com/redis/hiredis/compare/v0.14.1...v1.0.0)

**BREAKING CHANGES**:

* `redisOptions` now has two timeout fields.  One for connecting, and one for commands.  If you're presently using `options->timeout` you will need to change it to use `options->connect_timeout`. (See [example](https://github.com/redis/hiredis/commit/38b5ae543f5c99eb4ccabbe277770fc6bc81226f#diff-86ba39d37aa829c8c82624cce4f049fbL36))

* Bulk and multi-bulk lengths less than -1 or greater than `LLONG_MAX` are now protocol errors. This is consistent
  with the RESP specification. On 32-bit platforms, the upper bound is lowered to `SIZE_MAX`.

* `redisReplyObjectFunctions.createArray` now takes `size_t` for its length parameter.

**New features:**
- Support for RESP3
  [\#697](https://github.com/redis/hiredis/pull/697),
  [\#805](https://github.com/redis/hiredis/pull/805),
  [\#819](https://github.com/redis/hiredis/pull/819),
  [\#841](https://github.com/redis/hiredis/pull/841)
  ([Yossi Gottlieb](https://github.com/yossigo), [Michael Grunder](https://github.com/michael-grunder))
- Support for SSL connections
  [\#645](https://github.com/redis/hiredis/pull/645),
  [\#699](https://github.com/redis/hiredis/pull/699),
  [\#702](https://github.com/redis/hiredis/pull/702),
  [\#708](https://github.com/redis/hiredis/pull/708),
  [\#711](https://github.com/redis/hiredis/pull/711),
  [\#821](https://github.com/redis/hiredis/pull/821),
  [more](https://github.com/redis/hiredis/pulls?q=is%3Apr+is%3Amerged+SSL)
  ([Mark Nunberg](https://github.com/mnunberg), [Yossi Gottlieb](https://github.com/yossigo))
- Run-time allocator injection
  [\#800](https://github.com/redis/hiredis/pull/800)
  ([Michael Grunder](https://github.com/michael-grunder))
- Improved Windows support (including MinGW and Windows CI)
  [\#652](https://github.com/redis/hiredis/pull/652),
  [\#663](https://github.com/redis/hiredis/pull/663)
  ([Marcus Geelnard](https://www.bitsnbites.eu/author/m/))
- Adds support for distinct connect and command timeouts
  [\#839](https://github.com/redis/hiredis/pull/839),
  [\#829](https://github.com/redis/hiredis/pull/829)
  ([Valentino Geron](https://github.com/valentinogeron))
- Add generic pointer and destructor to `redisContext` that users can use for context.
  [\#855](https://github.com/redis/hiredis/pull/855)
  ([Michael Grunder](https://github.com/michael-grunder))

**Closed issues (that involved code changes):**

- Makefile does not install TLS libraries  [\#809](https://github.com/redis/hiredis/issues/809)
- redisConnectWithOptions should not set command timeout [\#722](https://github.com/redis/hiredis/issues/722), [\#829](https://github.com/redis/hiredis/pull/829) ([valentinogeron](https://github.com/valentinogeron))
- Fix integer overflow in `sdsrange` [\#827](https://github.com/redis/hiredis/issues/827)
- INFO & CLUSTER commands failed when using RESP3 [\#802](https://github.com/redis/hiredis/issues/802)
- Windows compatibility patches [\#687](https://github.com/redis/hiredis/issues/687), [\#838](https://github.com/redis/hiredis/issues/838), [\#842](https://github.com/redis/hiredis/issues/842)
- RESP3 PUSH messages incorrectly use pending callback [\#825](https://github.com/redis/hiredis/issues/825)
- Asynchronous PSUBSCRIBE command fails when using RESP3 [\#815](https://github.com/redis/hiredis/issues/815)
- New SSL API [\#804](https://github.com/redis/hiredis/issues/804), [\#813](https://github.com/redis/hiredis/issues/813)
- Hard-coded limit of nested reply depth [\#794](https://github.com/redis/hiredis/issues/794)
- Fix TCP_NODELAY in Windows/OSX [\#679](https://github.com/redis/hiredis/issues/679), [\#690](https://github.com/redis/hiredis/issues/690), [\#779](https://github.com/redis/hiredis/issues/779), [\#785](https://github.com/redis/hiredis/issues/785),
- Added timers to libev adapter.  [\#778](https://github.com/redis/hiredis/issues/778), [\#795](https://github.com/redis/hiredis/pull/795)
- Initialization discards const qualifier [\#777](https://github.com/redis/hiredis/issues/777)
- \[BUG\]\[MinGW64\] Error setting socket timeout  [\#775](https://github.com/redis/hiredis/issues/775)
- undefined reference to hi_malloc [\#769](https://github.com/redis/hiredis/issues/769)
- hiredis pkg-config file incorrectly ignores multiarch libdir spec'n [\#767](https://github.com/redis/hiredis/issues/767)
- Don't use -G to build shared object on Solaris [\#757](https://github.com/redis/hiredis/issues/757)
- error when make USE\_SSL=1 [\#748](https://github.com/redis/hiredis/issues/748)
- Allow to change SSL Mode [\#646](https://github.com/redis/hiredis/issues/646)
- hiredis/adapters/libevent.h memleak [\#618](https://github.com/redis/hiredis/issues/618)
- redisLibuvPoll crash when server closes the connetion [\#545](https://github.com/redis/hiredis/issues/545)
- about redisAsyncDisconnect question [\#518](https://github.com/redis/hiredis/issues/518)
- hiredis adapters libuv error for help [\#508](https://github.com/redis/hiredis/issues/508)
- API/ABI changes analysis [\#506](https://github.com/redis/hiredis/issues/506)
- Memory leak patch in Redis [\#502](https://github.com/redis/hiredis/issues/502)
- Remove the depth limitation [\#421](https://github.com/redis/hiredis/issues/421)

**Merged pull requests:**

- Move SSL management to a distinct private pointer [\#855](https://github.com/redis/hiredis/pull/855) ([michael-grunder](https://github.com/michael-grunder))
- Move include to sockcompat.h to maintain style [\#850](https://github.com/redis/hiredis/pull/850) ([michael-grunder](https://github.com/michael-grunder))
- Remove erroneous tag and add license to push example [\#849](https://github.com/redis/hiredis/pull/849) ([michael-grunder](https://github.com/michael-grunder))
- fix windows compiling with mingw [\#848](https://github.com/redis/hiredis/pull/848) ([rmalizia44](https://github.com/rmalizia44))
- Some Windows quality of life improvements. [\#846](https://github.com/redis/hiredis/pull/846) ([michael-grunder](https://github.com/michael-grunder))
- Use \_WIN32 define instead of WIN32 [\#845](https://github.com/redis/hiredis/pull/845) ([michael-grunder](https://github.com/michael-grunder))
- Non Linux CI fixes [\#844](https://github.com/redis/hiredis/pull/844) ([michael-grunder](https://github.com/michael-grunder))
- Resp3 oob push support [\#841](https://github.com/redis/hiredis/pull/841) ([michael-grunder](https://github.com/michael-grunder))
- fix \#785: defer TCP\_NODELAY in async tcp connections [\#836](https://github.com/redis/hiredis/pull/836) ([OmriSteiner](https://github.com/OmriSteiner))
- sdsrange overflow fix [\#830](https://github.com/redis/hiredis/pull/830) ([michael-grunder](https://github.com/michael-grunder))
- Use explicit pointer casting for c++ compatibility [\#826](https://github.com/redis/hiredis/pull/826) ([aureus1](https://github.com/aureus1))
- Document allocator injection and completeness fix in test.c [\#824](https://github.com/redis/hiredis/pull/824) ([michael-grunder](https://github.com/michael-grunder))
- Use unique names for allocator struct members [\#823](https://github.com/redis/hiredis/pull/823) ([michael-grunder](https://github.com/michael-grunder))
- New SSL API to replace redisSecureConnection\(\). [\#821](https://github.com/redis/hiredis/pull/821) ([yossigo](https://github.com/yossigo))
- Add logic to handle RESP3 push messages [\#819](https://github.com/redis/hiredis/pull/819) ([michael-grunder](https://github.com/michael-grunder))
- Use standrad isxdigit instead of custom helper function. [\#814](https://github.com/redis/hiredis/pull/814) ([tryfinally](https://github.com/tryfinally))
- Fix missing SSL build/install options. [\#812](https://github.com/redis/hiredis/pull/812) ([yossigo](https://github.com/yossigo))
- Add link to ABI tracker [\#808](https://github.com/redis/hiredis/pull/808) ([michael-grunder](https://github.com/michael-grunder))
- Resp3 verbatim string support [\#805](https://github.com/redis/hiredis/pull/805) ([michael-grunder](https://github.com/michael-grunder))
- Allow users to replace allocator and handle OOM everywhere. [\#800](https://github.com/redis/hiredis/pull/800) ([michael-grunder](https://github.com/michael-grunder))
- Remove nested depth limitation. [\#797](https://github.com/redis/hiredis/pull/797) ([michael-grunder](https://github.com/michael-grunder))
- Attempt to fix compilation on Solaris [\#796](https://github.com/redis/hiredis/pull/796) ([michael-grunder](https://github.com/michael-grunder))
- Support timeouts in libev adapater [\#795](https://github.com/redis/hiredis/pull/795) ([michael-grunder](https://github.com/michael-grunder))
- Fix pkgconfig when installing to a custom lib dir [\#793](https://github.com/redis/hiredis/pull/793) ([michael-grunder](https://github.com/michael-grunder))
- Fix USE\_SSL=1 make/cmake on OSX and CMake tests [\#789](https://github.com/redis/hiredis/pull/789) ([michael-grunder](https://github.com/michael-grunder))
- Use correct libuv call on Windows [\#784](https://github.com/redis/hiredis/pull/784) ([michael-grunder](https://github.com/michael-grunder))
- Added CMake package config and fixed hiredis\_ssl on Windows [\#783](https://github.com/redis/hiredis/pull/783) ([michael-grunder](https://github.com/michael-grunder))
- CMake: Set hiredis\_ssl shared object version. [\#780](https://github.com/redis/hiredis/pull/780) ([yossigo](https://github.com/yossigo))
- Win32 tests and timeout fix [\#776](https://github.com/redis/hiredis/pull/776) ([michael-grunder](https://github.com/michael-grunder))
- Provides an optional cleanup callback for async data. [\#768](https://github.com/redis/hiredis/pull/768) ([heronr](https://github.com/heronr))
- Housekeeping fixes [\#764](https://github.com/redis/hiredis/pull/764) ([michael-grunder](https://github.com/michael-grunder))
- install alloc.h [\#756](https://github.com/redis/hiredis/pull/756) ([ch1aki](https://github.com/ch1aki))
- fix spelling mistakes [\#746](https://github.com/redis/hiredis/pull/746) ([ShooterIT](https://github.com/ShooterIT))
- Free the reply in redisGetReply when passed NULL [\#741](https://github.com/redis/hiredis/pull/741) ([michael-grunder](https://github.com/michael-grunder))
- Fix dead code in sslLogCallback relating to should\_log variable. [\#737](https://github.com/redis/hiredis/pull/737) ([natoscott](https://github.com/natoscott))
- Fix typo in dict.c. [\#731](https://github.com/redis/hiredis/pull/731) ([Kevin-Xi](https://github.com/Kevin-Xi))
- Adding an option to DISABLE\_TESTS [\#727](https://github.com/redis/hiredis/pull/727) ([pbotros](https://github.com/pbotros))
- Update README with SSL support. [\#720](https://github.com/redis/hiredis/pull/720) ([yossigo](https://github.com/yossigo))
- Fixes leaks in unit tests [\#715](https://github.com/redis/hiredis/pull/715) ([michael-grunder](https://github.com/michael-grunder))
- SSL Tests [\#711](https://github.com/redis/hiredis/pull/711) ([yossigo](https://github.com/yossigo))
- SSL Reorganization [\#708](https://github.com/redis/hiredis/pull/708) ([yossigo](https://github.com/yossigo))
- Fix MSVC build. [\#706](https://github.com/redis/hiredis/pull/706) ([yossigo](https://github.com/yossigo))
- SSL: Properly report SSL\_connect\(\) errors. [\#702](https://github.com/redis/hiredis/pull/702) ([yossigo](https://github.com/yossigo))
- Silent SSL trace to stdout by default. [\#699](https://github.com/redis/hiredis/pull/699) ([yossigo](https://github.com/yossigo))
- Port RESP3 support from Redis. [\#697](https://github.com/redis/hiredis/pull/697) ([yossigo](https://github.com/yossigo))
- Removed whitespace before newline [\#691](https://github.com/redis/hiredis/pull/691) ([Miniwoffer](https://github.com/Miniwoffer))
- Add install adapters header files [\#688](https://github.com/redis/hiredis/pull/688) ([kevin1018](https://github.com/kevin1018))
- Remove unnecessary null check before free [\#684](https://github.com/redis/hiredis/pull/684) ([qlyoung](https://github.com/qlyoung))
- redisReaderGetReply leak memory [\#671](https://github.com/redis/hiredis/pull/671) ([movebean](https://github.com/movebean))
- fix timeout code in windows [\#670](https://github.com/redis/hiredis/pull/670) ([jman-krafton](https://github.com/jman-krafton))
- test: fix errstr matching for musl libc [\#665](https://github.com/redis/hiredis/pull/665) ([ghost](https://github.com/ghost))
- Windows: MinGW fixes and Windows Travis builders [\#663](https://github.com/redis/hiredis/pull/663) ([mbitsnbites](https://github.com/mbitsnbites))
- The setsockopt and getsockopt API diffs from BSD socket and WSA one [\#662](https://github.com/redis/hiredis/pull/662) ([dragonation](https://github.com/dragonation))
- Fix Compile Error On Windows \(Visual Studio\) [\#658](https://github.com/redis/hiredis/pull/658) ([jinjiazhang](https://github.com/jinjiazhang))
- Fix NXDOMAIN test case [\#653](https://github.com/redis/hiredis/pull/653) ([michael-grunder](https://github.com/michael-grunder))
- Add MinGW support [\#652](https://github.com/redis/hiredis/pull/652) ([mbitsnbites](https://github.com/mbitsnbites))
- SSL Support [\#645](https://github.com/redis/hiredis/pull/645) ([mnunberg](https://github.com/mnunberg))
- Fix Invalid argument after redisAsyncConnectUnix [\#644](https://github.com/redis/hiredis/pull/644) ([codehz](https://github.com/codehz))
- Makefile: use predefined AR [\#632](https://github.com/redis/hiredis/pull/632) ([Mic92](https://github.com/Mic92))
- FreeBSD  build fix [\#628](https://github.com/redis/hiredis/pull/628) ([devnexen](https://github.com/devnexen))
- Fix errors not propagating properly with libuv.h. [\#624](https://github.com/redis/hiredis/pull/624) ([yossigo](https://github.com/yossigo))
- Update README.md [\#621](https://github.com/redis/hiredis/pull/621) ([Crunsher](https://github.com/Crunsher))
- Fix redisBufferRead documentation [\#620](https://github.com/redis/hiredis/pull/620) ([hacst](https://github.com/hacst))
- Add CPPFLAGS to REAL\_CFLAGS [\#614](https://github.com/redis/hiredis/pull/614) ([thomaslee](https://github.com/thomaslee))
- Update createArray to take size\_t [\#597](https://github.com/redis/hiredis/pull/597) ([justinbrewer](https://github.com/justinbrewer))
- fix common realloc mistake and add null check more [\#580](https://github.com/redis/hiredis/pull/580) ([charsyam](https://github.com/charsyam))
- Proper error reporting for connect failures [\#578](https://github.com/redis/hiredis/pull/578) ([mnunberg](https://github.com/mnunberg))

\* *This Changelog was automatically generated by [github_changelog_generator](https://github.com/github-changelog-generator/github-changelog-generator)*

## [1.0.0-rc1](https://github.com/redis/hiredis/tree/v1.0.0-rc1) - (2020-07-29)

_Note:  There were no changes to code between v1.0.0-rc1 and v1.0.0 so see v1.0.0 for changelog_

### 0.14.1 (2020-03-13)

* Adds safe allocation wrappers (CVE-2020-7105, #747, #752) (Michael Grunder)

### 0.14.0 (2018-09-25)
**BREAKING CHANGES**:

* Change `redisReply.len` to `size_t`, as it denotes the the size of a string

  User code should compare this to `size_t` values as well.
  If it was used to compare to other values, casting might be necessary or can be removed, if casting was applied before.

* Make string2ll static to fix conflict with Redis (Tom Lee [c3188b])
* Use -dynamiclib instead of -shared for OSX (Ryan Schmidt [a65537])
* Use string2ll from Redis w/added tests (Michael Grunder [7bef04, 60f622])
* Makefile - OSX compilation fixes (Ryan Schmidt [881fcb, 0e9af8])
* Remove redundant NULL checks (Justin Brewer [54acc8, 58e6b8])
* Fix bulk and multi-bulk length truncation (Justin Brewer [109197])
* Fix SIGSEGV in OpenBSD by checking for NULL before calling freeaddrinfo (Justin Brewer [546d94])
* Several POSIX compatibility fixes (Justin Brewer [bbeab8, 49bbaa, d1c1b6])
* Makefile - Compatibility fixes (Dimitri Vorobiev [3238cf, 12a9d1])
* Makefile - Fix make install on FreeBSD (Zach Shipko [a2ef2b])
* Makefile - don't assume $(INSTALL) is cp (Igor Gnatenko [725a96])
* Separate side-effect causing function from assert and small cleanup (amallia [b46413, 3c3234])
* Don't send negative values to `__redisAsyncCommand` (Frederik Deweerdt [706129])
* Fix leak if setsockopt fails (Frederik Deweerdt [e21c9c])
* Fix libevent leak (zfz [515228])
* Clean up GCC warning (Ichito Nagata [2ec774])
* Keep track of errno in `__redisSetErrorFromErrno()` as snprintf may use it (Jin Qing [25cd88])
* Solaris compilation fix (Donald Whyte [41b07d])
* Reorder linker arguments when building examples (Tustfarm-heart [06eedd])
* Keep track of subscriptions in case of rapid subscribe/unsubscribe (Hyungjin Kim [073dc8, be76c5, d46999])
* libuv use after free fix (Paul Scott [cbb956])
* Properly close socket fd on reconnect attempt (WSL [64d1ec])
* Skip valgrind in OSX tests (Jan-Erik Rediger [9deb78])
* Various updates for Travis testing OSX (Ted Nyman [fa3774, 16a459, bc0ea5])
* Update libevent (Chris Xin [386802])
* Change sds.h for building in C++ projects (Ali Volkan ATLI [f5b32e])
* Use proper format specifier in redisFormatSdsCommandArgv (Paulino Huerta, Jan-Erik Rediger [360a06, 8655a6])
* Better handling of NULL reply in example code (Jan-Erik Rediger [1b8ed3])
* Prevent overflow when formatting an error (Jan-Erik Rediger [0335cb])
* Compatibility fix for strerror_r (Tom Lee [bb1747])
* Properly detect integer parse/overflow errors (Justin Brewer [93421f])
* Adds CI for Windows and cygwin fixes (owent, [6c53d6, 6c3e40])
* Catch a buffer overflow when formatting the error message
* Import latest upstream sds. This breaks applications that are linked against the old hiredis v0.13
* Fix warnings, when compiled with -Wshadow
* Make hiredis compile in Cygwin on Windows, now CI-tested
* Bulk and multi-bulk lengths less than -1 or greater than `LLONG_MAX` are now
  protocol errors. This is consistent with the RESP specification. On 32-bit
  platforms, the upper bound is lowered to `SIZE_MAX`.

* Remove backwards compatibility macro's

This removes the following old function aliases, use the new name now:

| Old                         | New                    |
| --------------------------- | ---------------------- |
| redisReplyReaderCreate      | redisReaderCreate      |
| redisReplyReaderCreate      | redisReaderCreate      |
| redisReplyReaderFree        | redisReaderFree        |
| redisReplyReaderFeed        | redisReaderFeed        |
| redisReplyReaderGetReply    | redisReaderGetReply    |
| redisReplyReaderSetPrivdata | redisReaderSetPrivdata |
| redisReplyReaderGetObject   | redisReaderGetObject   |
| redisReplyReaderGetError    | redisReaderGetError    |

* The `DEBUG` variable in the Makefile was renamed to `DEBUG_FLAGS`

Previously it broke some builds for people that had `DEBUG` set to some arbitrary value,
due to debugging other software.
By renaming we avoid unintentional name clashes.

Simply rename `DEBUG` to `DEBUG_FLAGS` in your environment to make it working again.

### 0.13.3 (2015-09-16)

* Revert "Clear `REDIS_CONNECTED` flag when connection is closed".
* Make tests pass on FreeBSD (Thanks, Giacomo Olgeni)


If the `REDIS_CONNECTED` flag is cleared,
the async onDisconnect callback function will never be called.
This causes problems as the disconnect is never reported back to the user.

### 0.13.2 (2015-08-25)

* Prevent crash on pending replies in async code (Thanks, @switch-st)
* Clear `REDIS_CONNECTED` flag when connection is closed (Thanks, Jerry Jacobs)
* Add MacOS X addapter (Thanks, @dizzus)
* Add Qt adapter (Thanks, Pietro Cerutti)
* Add Ivykis adapter (Thanks, Gergely Nagy)

All adapters are provided as is and are only tested where possible.

### 0.13.1 (2015-05-03)

This is a bug fix release.
The new `reconnect` method introduced new struct members, which clashed with pre-defined names in pre-C99 code.
Another commit forced C99 compilation just to make it work, but of course this is not desirable for outside projects.
Other non-C99 code can now use hiredis as usual again.
Sorry for the inconvenience.

* Fix memory leak in async reply handling (Salvatore Sanfilippo)
* Rename struct member to avoid name clash with pre-c99 code (Alex Balashov, ncopa)

### 0.13.0 (2015-04-16)

This release adds a minimal Windows compatibility layer.
The parser, standalone since v0.12.0, can now be compiled on Windows
(and thus used in other client libraries as well)

* Windows compatibility layer for parser code (tzickel)
* Properly escape data printed to PKGCONF file (Dan Skorupski)
* Fix tests when assert() undefined (Keith Bennett, Matt Stancliff)
* Implement a reconnect method for the client context, this changes the structure of `redisContext` (Aaron Bedra)

### 0.12.1 (2015-01-26)

* Fix `make install`: DESTDIR support, install all required files, install PKGCONF in proper location
* Fix `make test` as 32 bit build on 64 bit platform

### 0.12.0 (2015-01-22)

* Add optional KeepAlive support

* Try again on EINTR errors

* Add libuv adapter

* Add IPv6 support

* Remove possibility of multiple close on same fd

* Add ability to bind source address on connect

* Add redisConnectFd() and redisFreeKeepFd()

* Fix getaddrinfo() memory leak

* Free string if it is unused (fixes memory leak)

* Improve redisAppendCommandArgv performance 2.5x

* Add support for SO_REUSEADDR

* Fix redisvFormatCommand format parsing

* Add GLib 2.0 adapter

* Refactor reading code into read.c

* Fix errno error buffers to not clobber errors

* Generate pkgconf during build

* Silence _BSD_SOURCE warnings

* Improve digit counting for multibulk creation


### 0.11.0

* Increase the maximum multi-bulk reply depth to 7.

* Increase the read buffer size from 2k to 16k.

* Use poll(2) instead of select(2) to support large fds (>= 1024).

### 0.10.1

* Makefile overhaul. Important to check out if you override one or more
  variables using environment variables or via arguments to the "make" tool.

* Issue #45: Fix potential memory leak for a multi bulk reply with 0 elements
  being created by the default reply object functions.

* Issue #43: Don't crash in an asynchronous context when Redis returns an error
  reply after the connection has been made (this happens when the maximum
  number of connections is reached).

### 0.10.0

* See commit log.
