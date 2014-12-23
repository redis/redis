[![Windows Status](http://img.shields.io/appveyor/ci/MSOpenTech-lab/redis.svg?style=flat-square)](https://ci.appveyor.com/project/MSOpenTech-lab/redis)

[![NuGet version](http://img.shields.io/nuget/v/redis-64.svg?style=flat-square)](http://www.nuget.org/packages/redis-64/)
[![NuGet downloads](http://img.shields.io/nuget/dt/redis-64.svg?style=flat-square)](http://www.nuget.org/packages/redis-64/)

[![Chocolatey version](http://img.shields.io/chocolatey/v/redis-64.svg?style=flat-square)](http://www.chocolatey.org/packages/redis-64/)
[![Chocolatey downloads](http://img.shields.io/chocolatey/dt/redis-64.svg?style=flat-square)](http://www.chocolatey.org/packages/redis-64/)

Redis on Windows 
===

## Redis 2.8 Branch

- This is a port for Windows based on Redis 2.8.
- There is support for the 64-bit version. We have dropped official support for the 32-bit version, though you can build it from source if desired.
- You can download the latest binaries (unsigned) from [http://github.com/MSOpenTech/redis/releases](http://github.com/MSOpenTech/redis/releases "Release page"). For releases prior to 2.8.17.1, the binaries can found in a zip file inside the source archive, under the bin/release folder.
- Signed binaries can be downloaded using Nuget and Chocolatey.
- There is a replacement for the UNIX fork() API that simulates the copy-on-write behavior using a memory mapped file.
- Because Redis makes some assumptions about the values of File Descriptors, we have built a virtual file descriptor mapping layer. 
- We are moving towards moving all Windows-specific changes into the Win32_Interop library.
- Redis can be installed as a Windows Service.

## What's new since 2.8.12

- See the Redis release notes: http://download.redis.io/redis-stable/00-RELEASENOTES

## Important: More documentation is available

Please read the documentation in msvs\setups\documentation. This is the documentation that is bundled with the binaries, and contains vital information about configuring and deploying Redis on Windows.

## How to build Redis using Visual Studio

You can use the free Visual Studio Express 2013 for Windows Desktop edition available at http://www.microsoft.com/en-us/download/details.aspx?id=40787.

- Open the solution file msvs\redisserver.sln in Visual Studio 2013, select a build configuration (Debug or Release) and target (Win32 or x64) then build.

    This should create the following executables in the msvs\$(Target)\$(Configuration) folder:

    - redis-server.exe
    - redis-benchmark.exe
    - redis-cli.exe
    - redis-check-dump.exe
    - redis-check-aof.exe

## Unit Testing

To run the Redis test suite requires some manual work:

- The tests assume that the binaries are in the src folder. Use mklink to create a symbolic link to the files in the msvs\x64\Debug|Release folders. You will
  need symbolic links for src\redis-server, src\redis-benchmark, src\redis-check-aof, src\redis-check-dump, src\redis-cli, and src\redis-sentinel.
- The tests make use of TCL. This must be installed separately.
- To run the tests you need to have a Unix shell on your machine, or MinGW tools in your path. To execute the tests, run the following command: 
  "tclsh8.5.exe tests/test_helper.tcl --clients N", where N is the number of parallel clients . If a Unix shell is not installed you may see the 
  following error message: "couldn't execute "cat": no such file or directory".
- By default the test suite launches 16 parallel tests. I will get time out errors on an iCore 7-2620m@2.7Ghz with some of the tests when the number of clients 
  is greater than 6. 
  
## Known issues

Problem:
On versions of Windows prior to Windows 8/Server 2012, when an AOF or RDB operation is complete and the child process is being rejoined with the parent, it is 
possible for Redis to unexpectedly terminate.

Cause:
The PAGE_REVERT_TO_FILE_MAP flag is not usable in VirtualProtect() in earlier versions of the OS. This flag allows for removing copy on write(COW) pages from 
a memory mapped view without unmapping the view. In prior versions of the OS, the only way to purge COW pages is to unmap and then remap the memory mapped view. 
Between the unmapping and remapping operations a third party process(e.g., an anti-virus program) could allocate virtual memory occupied by the memory mapped view. 
As this view is used for the Redis heap, failure to remap is a fatal error. (See RejoinCOWPages() in src\Win32_Interop\Win32_QFork.cpp)

Solution:
If you encounter this problem, host Redis on Windows 8/Server 2012 or newer.

