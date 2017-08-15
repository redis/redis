### 1.0.0 (unreleased)

**Fixes**:

* Catch a buffer overflow when formatting the error message
* Import latest upstream sds. This breaks applications that are linked against the old hiredis v0.13
* Fix warnings, when compiled with -Wshadow
* Make hiredis compile in Cygwin on Windows, now CI-tested

**BREAKING CHANGES**:

* Change `redisReply.len` to `size_t`, as it denotes the the size of a string

User code should compare this to `size_t` values as well.
If it was used to compare to other values, casting might be necessary or can be removed, if casting was applied before.

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

* Remove possiblity of multiple close on same fd

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

