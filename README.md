[![Windows Status](http://img.shields.io/appveyor/ci/MSOpenTech-lab/redis.svg?style=flat-square)](https://ci.appveyor.com/project/MSOpenTech-lab/redis) [![NuGet version](http://img.shields.io/nuget/v/redis-64.svg?style=flat-square)](http://www.nuget.org/packages/redis-64/) [![Chocolatey version](http://img.shields.io/chocolatey/v/redis-64.svg?style=flat-square)](http://www.chocolatey.org/packages/redis-64/) [![Chocolatey downloads](http://img.shields.io/chocolatey/dt/redis-64.svg?style=flat-square)](http://www.chocolatey.org/packages/redis-64/)

This project is no longer being actively maintained. If you are looking for a Windows version of Redis, you may want to check out [Memurai](https://www.memurai.com/). Please note that Microsoft is not officially endorsing this product in any way.

## Redis on Windows 

- This is a port for Windows based on [Redis](https://github.com/antirez/redis).
- We officially support the 64-bit version only. Although you can build the 32-bit version from source if desired.
- You can download the latest unsigned binaries and the unsigned MSI installer from the [release page](http://github.com/MSOpenTech/redis/releases "Release page").
- For releases prior to 2.8.17.1, the binaries can found in a zip file inside the source archive, under the bin/release folder.
- Signed binaries are available through [NuGet](https://www.nuget.org/packages/Redis-64/) and [Chocolatey](https://chocolatey.org/packages/redis-64).
- Redis can be installed as a Windows Service.

## Windows-specific changes
- There is a replacement for the UNIX fork() API that simulates the copy-on-write behavior using a memory mapped file on 2.8. Version 3.0 is using a similar behavior but dropped the memory mapped file in favor of the system paging file.
- In 3.0 we switch the default memory allocator from dlmalloc to jemalloc that is supposed to do a better job at managing the heap fragmentation.
- Because Redis makes some assumptions about the values of file descriptors, we have built a virtual file descriptor mapping layer. 

## Redis release notes

There are two current active branches: 2.8 and 3.0.

- Redis on UNIX [2.8 release notes](https://raw.githubusercontent.com/antirez/redis/2.8/00-RELEASENOTES)
- Redis on Windows [2.8 release notes](https://raw.githubusercontent.com/MSOpenTech/redis/2.8/Redis%20on%20Windows%20Release%20Notes.md)
- Redis on UNIX [3.0 release notes](https://raw.githubusercontent.com/antirez/redis/3.0/00-RELEASENOTES)
- Redis on Windows [3.0 release notes](https://raw.githubusercontent.com/MSOpenTech/redis/3.0/Redis%20on%20Windows%20Release%20Notes.md)

## How to configure and deploy Redis on Windows

- [Memory Configuration for 2.8](https://github.com/MSOpenTech/redis/wiki/Memory-Configuration "Memory Configuration")
- [Memory Configuration for 3.0](https://github.com/MSOpenTech/redis/wiki/Memory-Configuration-For-Redis-3.0 "Memory Configuration")
- [Windows Service Documentation](https://raw.githubusercontent.com/MSOpenTech/redis/3.0/Windows%20Service%20Documentation.md "Windows Service Documentation")
- [Redis on Windows](https://raw.githubusercontent.com/MSOpenTech/redis/2.8/Redis%20on%20Windows.md "Redis on Windows")
- [Windows Service Documentation](https://raw.githubusercontent.com/MSOpenTech/redis/2.8/Windows%20Service%20Documentation.md "Windows Service Documentation")

## How to build Redis using Visual Studio

You can use the free [Visual Studio 2013 Community Edition](http://www.visualstudio.com/products/visual-studio-community-vs). Regardless which Visual Studio edition you use, make sure you have updated to Update 5, otherwise you will get a "illegal use of this type as an expression" error.

- Open the solution file msvs\redisserver.sln in Visual Studio, select a build configuration (Debug or Release) and target (x64) then build.

    This should create the following executables in the msvs\$(Target)\$(Configuration) folder:

    - redis-server.exe
    - redis-benchmark.exe
    - redis-cli.exe
    - redis-check-dump.exe
    - redis-check-aof.exe

## Testing

To run the Redis test suite some manual work is required:

- The tests assume that the binaries are in the src folder. Use mklink to create a symbolic link to the files in the msvs\x64\Debug|Release folders. You will
  need symbolic links for src\redis-server, src\redis-benchmark, src\redis-check-aof, src\redis-check-dump, src\redis-cli, and src\redis-sentinel.
- The tests make use of TCL. This must be installed separately.
- To run the cluster tests against 3.0, Ruby On Windows is required.
- To run the tests you need to have a Unix shell on your machine, or MinGW tools in your path. To execute the tests, run the following command: 
  "tclsh8.5.exe tests/test_helper.tcl --clients N", where N is the number of parallel clients . If a Unix shell is not installed you may see the 
  following error message: "couldn't execute "cat": no such file or directory".
- By default the test suite launches 16 parallel tests, but 2 is the suggested number. 
  
## Code of Conduct

This project has adopted the [Microsoft Open Source Code of Conduct](https://opensource.microsoft.com/codeofconduct/). For more information see the [Code of Conduct FAQ](https://opensource.microsoft.com/codeofconduct/faq/) or contact [opencode@microsoft.com](mailto:opencode@microsoft.com) with any additional questions or comments.
