Redis on Windows 2.8.4
===
## What's new in this release

- This is a port for Windows based on Redis 2.8. The latest version merged in 2.8.4.
- There is support for the x64 version. We have dropped support for the 32-bit version.
- The binaries (unsigned) have been moved to a zip file in the \bin folder to make them easier to find. The Release build automatically updates the
  zip file.
- Signed binaries can be downloaded using Nuget and Chocolatey.
- There is a replacement for the UNIX fork() API that simulates the copy-on-write behavior using a memory mapped file.
- Because Redis makes some assumptions about the values of File Descriptors, we have built a virtual file descriptor mapping layer. 
- We are moving towards moving all Windows-specific changes into the Win32_Interop library.

## Repo branches
- 2.8.4.msopen: This is the branch for the Windows Redis port based on Redis 2.8
- 2.6: This is the branch for the Windows Redis port based on Redis 2.6.
- 2.4: This branch has the Windows Redis port based on Redis 2.4.

## How to build Redis using Visual Studio

You can use the free Visual Studio Express 2013 for Windows Desktop edition available at http://www.microsoft.com/en-us/download/details.aspx?id=40787.

- Open the solution file msvs\redisserver.sln in Visual Studio 2013, select a build configuration (Debug or Release) and build.

    This should create the following executables in the msvs\x64\$(Configuration) folder:

    - redis-server.exe
    - redis-benchmark.exe
    - redis-cli.exe
    - redis-check-dump.exe
    - redis-check-aof.exe

## Release Notes

This is a production-ready version of the software.

To run the Redis test suite requires some manual work:

- The tests assume that the binaries are in the src folder. Use mklink to create a symbolic link to the files in the msvs\x64\Debug|Release folders. 
- The tests make use of TCL. This must be installed separately.
- To run the tests you need to have a Unix shell on your machine. To execute the tests, run the following command: `tclsh8.5.exe tests/test_helper.tcl`. 
- Because of the swap space requirements for the quasi-fork implementation, running too many instances of redis will exhaust swap space. Some of the unit 
  tests launch multiple instance of redis (i.e., 1 master + 3 slaves). By default the test suite launches 16 parallel tests. This will cause failures
  because there is not enough swap sapce configured by default. Either limit the number of test instances with the '--clients 1' flag, or increase the
  default system swap space significantly (increase by (4 x physical memory) for each additional test client). 
  
If a Unix shell is not installed you may see the following error message: "couldn't execute "cat": no such file or directory". 

## Known issues
None.
