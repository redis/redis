Redis on Windows 2.6 prototype
===
## What's new in this release

- This is a port for Windows based on Redis 2.6. The latest version merged in 2.6.12.
- The port is similar to the port of Redis 2.4, including the same background saving technology.
- There is support for x64 version as well as 32 bit versions.
- For the 64 bit version, there is a limit of 2^32 objects in a structure, and a max length of 2^32 for any object
- Version number now 2.6.8-pre2 to indicate prerelease and to enable changing
- Version 2.6.8-pre2 fixes several failures that existed in 2.6.8-pre1. Most of these were related to handling opening and closing of non-blocking sockets.
- The unit/protocol desync test is updated to use nonblocking socket and now works on Windows
- The binaries (unsigned) have been moved to the root to make them easier to find. Previously they were under msvs.
- Signed binaries can be downloaded using Nuget.

##Acknowledgements
Special thanks to Dušan Majkic (https://github.com/dmajkic, https://github.com/dmajkic/redis/) for his project on GitHub that gave us the opportunity to quickly learn some on the intricacies of Redis code. His project also helped us to build our prototype quickly.

## Repo branches
- 2.6: This is the branch for the Windows Redis port based on Redis 2.6.
- 2.4: This branch has the Windows Redis port based on Redis 2.4.

## How to build Redis using Visual Studio

You can use the free Express Edition available at http://www.microsoft.com/visualstudio/en-us/products/2010-editions/visual-cpp-express.

- Open the solution file msvs\redisserver.sln in Visual Studio 10, select platform (win32 or x64) and build.

    This should create the following executables in the msvs\$(Configuration) folder:

    - redis-server.exe
    - redis-benchmark.exe
    - redis-cli.exe
    - redis-check-dump.exe
    - redis-check-aof.exe

For your convenience all binaries are be available in the msvs/bin/release|debug directories.

## RedisWatcher
So far the RedisWatcher is not carried over to 2.6. However this should not be affected by the Redis version, and the code in the 2.4 branch should work with the Redis 2.6 binaries.

## RedisWAInst
So far the RedisWAInst is not carried over to 2.6. However this should not be affected by the Redis version, and the code in the 2.4 branch should work with the Redis 2.6 binaries.

## Release Notes

The branch has been renamed from 2.6_alpha to 2.6 to indicate that the test pass has been completed.

This is a release version of the software.

To run the Redis test suite requires some manual work:

- The tests assume that the binaries are in the src folder, so you need to copy the binaries from the msvs folder to src. 
- The tests make use of TCL. This must be installed separately.
- To run the tests you need to have a Unix shell on your machine. To execute the tests, run the following command: `tclsh8.5.exe tests/test_helper.tcl`. 
  
If a Unix shell is not installed you may see the following error message: "couldn't execute "cat": no such file or directory".

## Known issues
None.
