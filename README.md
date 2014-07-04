Redis on Windows 
===

## Redis 2.8 Branch

- This is a port for Windows based on Redis 2.8. The latest version merged in 2.8.12.
- There is support for the 64-bit version. We have dropped official support for the 32-bit version, though you can build it from source if desired.
- The binaries (unsigned) have been moved to a zip file in the \bin folder to make them easier to find. The Release build automatically updates the
  zip file.
- Signed binaries can be downloaded using Nuget and Chocolatey.
- There is a replacement for the UNIX fork() API that simulates the copy-on-write behavior using a memory mapped file.
- Because Redis makes some assumptions about the values of File Descriptors, we have built a virtual file descriptor mapping layer. 
- We are moving towards moving all Windows-specific changes into the Win32_Interop library.
- Redis can be installed as a Windows Service.

## What's new since 2.8.9

- See the Redis release notes: http://download.redis.io/redis-stable/00-RELEASENOTES
- The Windows codebase has a few bug fixes and better error messages for our fork() API replacement.

## Important: More documentation is available

Please read the documentation in msvs\setups\documentation. This is the documentation that is bundled with the binaries, and contains vital information  
about configuring and deploying Redis on Windows.

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
  'tclsh8.5.exe tests/test_helper.tcl --clients <n>`. If a Unix shell is not installed you may see the following error message: "couldn't execute "cat": no 
  such file or directory".
- By default the test suite launches 16 parallel tests. I will get time out errors on an iCore 7-2620m@2.7Ghz with some of the tests when the number of clients 
  is greater than 6. 
  
 

## Known issues

None.
