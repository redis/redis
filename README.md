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
- Redis can be installed as a Windows Service.

## Redis 2.8 release notes

- Redis on UNIX [release notes](https://raw.githubusercontent.com/antirez/redis/2.8/00-RELEASENOTES)
- Redis on Windows [release notes](https://raw.githubusercontent.com/MSOpenTech/redis/2.8/Redis%20on%20Windows%20Release%20Notes.md)

## How to configure and deploy Redis on Windows

- [Memory Configuration](https://github.com/MSOpenTech/redis/wiki/Memory-Configuration "Memory Configuration")
- [Redis on Windows](https://raw.githubusercontent.com/MSOpenTech/redis/2.8/Redis%20on%20Windows.md "Redis on Windows")
- [Windows Service Documentation](https://raw.githubusercontent.com/MSOpenTech/redis/2.8/Windows%20Service%20Documentation.md "Windows Service Documentation")

## How to build Redis using Visual Studio

You can use the free [Visual Studio Community edition](http://www.visualstudio.com/products/visual-studio-community-vs). If you use Microsoft Visual Studio 2013 (standard), make sure you have updated to Update 5, otherwise you will get a " illegal use of this type as an expression" error.

- Open the solution file msvs\redisserver.sln in Visual Studio, select a build configuration (Debug or Release) and target (x64) then build.

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
- By default the test suite launches 16 parallel tests, but some of the tests may fail when the number of clients is greater than 2. 
  

