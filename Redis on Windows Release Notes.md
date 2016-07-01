MSOpenTech Redis on Windows 3.0 Release Notes
=============================================
--[ Redis on Windows 3.0.504 ] Release date: Jul 01 2016

 - [Fix] Use overlapped sockets for cluster failover communication.
 - [Portability] strtol and strtoul fixes.
 - [Docs] Added Microsoft Open Source Code of Conduct.

--[ Redis on Windows 3.0.503 ] Release date: Jun 21 2016

 - [Fix] Possible AV during background save.

--[ Redis on Windows 3.0.502 ] Release date: Jun 21 2016

 - [PR] Fixed pointer overflow crash when using bgsave under rare circumstances. (by @Harachie)
 - [PR] Update msvs documentation to correct maxmemory-policy. (by @andyvan)
 - [Setup] Fixed the NETWORK SERVICE account issue on Windows 10.
 - [PR] Compare empty string for command line extract save. (by @zhumingjian )
 - [PR] Fix: bug on extracting sub-params. (by @zeliard)
 - [PR] Fix: string (handle value) to a unsigned 64bit number for LLP64 OS. (by @zeliard)
 - [PR] Fix: add break stmt in switch-case. (by @zeliard)
 - [Fix] 'Infinity' parsing.
 - [Fix] Linking error on some platforms using VS2015.
 - [PR] No separate NuGet download anymore on shields.io (by @jamesmanning )
 - [PR] Fix building problems in MSVS2015. (by @CAIQT)
 - [Docs] Fixed wrong value for redis install example.

--[ Redis on Windows 3.0.501 ] Release date: Jan 15 2016

 - [Docs] Single dash replaced with double dash for service cmd parameters.
 - [PR] Update Redis on Windows.md  (by @ammills01)
 - [Fix] Enabled jemalloc thread safety.
 - [Code cleanup] Better expression grouping.
 - [Docs] Added info about the memory working set showed by the task manager.
 - [Fix] Portability fix for strtol.
 - [Docs] Updated README.md.
 - [PR] Add notice for VS2013 without Update 5 (by @gimmemoore)

--[ Redis on Windows 3.0.500 ] Release date: Dec 07 2015

 - [Release] 3.0.500 stable.

--[ Redis on Windows 3.0.500-rc2 ] Release date: Dec 03 2015

 - [Docs] Updated the README.
 - [Test] Added regression test for replication when AUTH is on.
 - [Fix] Replication I/O bug when AUTH is enabled.
 - [Fix] FreeHeapBlock should check if the addr is in the redis heap.
 - [Fix] Disable replication if persistence is not available.
 - [Setup] Updated the command to push the chocolatey package.
 - Removed the HiredisExample project since it will be placed in the stand-alone hiredis repository.
 - [Debug] Added Redis version at the top of the crash report.
 - [Build] Added platform in the destination folder path for the x86 build.
 - [Fix] 32 bit support.
 - [PR] Unable to build Redis 3.0 on 32 bit. (by @Jens-G)
 - [PR] Switching 3.0 to x86 results in LNK errors. (by @Jens-G)
 - [Comment] Fixed comment.
 - [PR] replace argument sign '-' to '--'. (by @Hawkeyes0)
 - [Fix] Duplicated sockets management for diskless replication.
 - [Code cleanup] Code refactoring, formatting, comments, error logging.

--[ Redis on Windows 3.0.500-rc1 ] Release date: Nov 12 2015

 - [Fix] Improved the error reporting on startup errors.
 - [Code cleanup] Event log code refactoring. Code formatting.
 - [Code cleanup] Fixed tabs.
 - [Code cleanup] Renamed WSIOCP_ReceiveDone to WSIOCP_QueueNextRead.
 - [Code refactoring] IsWindowsVersionAtLeast optimization.
 - [Fix] Sentinel notification-script 2nd argument needs quotes.
 - [PR] Passed STARTUPINFO parameter to CreateProcessA instead of NULL. (by @flavius-m)
 - [Test] Removed a Windows-specific workaround.
 - [Fix] Duplicated sockets need to be closed properly.
 - [Fix] Windows-specific fixes for the 3.0.5 merge.
 - Merged tag 3.0.5 from antirez/3.0
 - [Sample] Removed the maxheap flag from the configuration samples.
 - [Fix] Reporting the error code if listen() fails.
 - [Tools] Changed the ReleasePackagingTool output folder.
 - [Setup] Added the max memory dialog.
 - [Build] Unified output folders for the jemalloc project.
 - [PR] Updated list of sentinel commands: announce-ip and announce-port. (by @rpannell)
 - [PR] Updated x86 debug and release configurations for all projects. (by @laurencee)
 - [PR] Changed Nuget package structure to support VS 2015. (by @Cybermaxs)
 - [Seup] Updated nuget and chocolatey setup files.

--[ Redis on Windows 3.0.300-beta1 ] Release date: Oct 14 2015

 - [Change] Switched from dlmalloc to jemalloc.
 - [Change] Child process can allocate memory from the system heap.
 - [Build] Removed the proprocessor defs: _WIN32IOCP, WIN32_IOCP.
 - [Change] Heap allocation on demand.
 - [Change] Removed the memory mapped file.
 - [Cleanup] Comments and code formatting/cleanup/consistency.
 - [Code cleanup] Minor code changes preparatory for jemalloc support.
 - [Change] Sentinel mode doesn't require a memory mapped file.
 - [Cleanup] Code refactoring, fixed typos, formatting.
 - [New] Added jemalloc-win project.
 - [Cleanup] Code refactoring (some from azure porting).
 - [Fix] Redis crashes at startup.

--[ Redis on Windows 3.0.300-alpha3 ] Release date: Sep 02 2015

 - [Setup] Updated version from 3.0.300-alpha2 to 3.0.300-alpha3.
 - [Fix] Error handling and cleanup after an AOF rewrite error.
 - [Fix] Made stack trace report more robust.
 - [Fix] replace_rename infinite loop upon error.
 - [Cleanup] Code refactoring, hiredis isolation (work in progress).
 - [Cleanup] Code refactoring.
 - [Fix] Ported fixes from 2.8.
 - [Cleanup] Replaced aeWin prefix with WSIOCP.
 - [Fix] Made getNextRFDAvailable more robust.
 - [Fix] Removed aeWinCloseSocket. Sockets were not closed properly.
 - [Fix] Optimized socket flag management.
 - [Debug] Added custom ASSERT macro.


--[ Redis on Windows 3.0.300-alpha2 ] Release date: Aug 20 2015

 - [Cleanup] Code formatting.
 - [Test] Added a Windows-specific regression test.
 - [Test] Increated the waiting time before checking the server status.
 - [Fix] Socket flags not saved.
 - [Cleanup] Code refacoring.
 - [Test] Increased the waiting time for some conditions.
 - [Test] Removed Windows specific code.
 - [Test] Set maxheap to 150mb to run the cluster tests.
 - [Fix] getpeername fails to retrieve the master ip address.
 - [Cleanup] Code refactoring, comments.
 - [Cleanup] Code refactoring to reduce dependencies between projects.
 - [Fix] Socket state moved from aeApiState to RFDMap.
 - [Cleanup] Code refactoring.
 - [Cleanup] Removed WSACleanup mapping.
 - [Setup] NuGet description doesn't support Markdown.
 - [Portability] Explicit type casting.
 - [Fix] Redis Server stops accepting connections.
 - [Fix] Set pipe to non-blocking. If the child process has already exite
 - [Fix] AOF rewrite not working.
 - [Setup] Release number.
 - Merged tag 3.0.3 from antirez/3.0 into 3.0


--[ Redis on Windows 3.0.100-alpha1 ] Release date: Jul 22 2015

 - First alpha based on Redis 3.0.1 [https://raw.githubusercontent.com/antirez/redis/10323dc5feb2adc10c4d62c7d667fd45923d6a57/00-RELEASENOTES]
 - Portability fixes: long -> PORT_LONG, unsigned long -> PORT_ULONG etc.
 - fcntl in WIN32 implementation doesn't support default arg.
 - Squashed 2.8 fixes since the 3.0 initial merge.
 - WIN32 portability fixes.
 - Removed the forkedProcessReady event. 
 - [Change] Rolled back "Workaround for getpeername() issue". 
 - [Fix] Memory corruption. Merged fix from Azure fork (by Mike Montwill). 
 - [Change/Fix] Added API mapping for fclose/fileno.
 - 3.0 fixes: passing pipes from parent to child plus fixes from Azure.
 - [Fix] Portability fixes taken from Azure.
 - [Test] Portability fix to support Ruby.
 - [Fix] Workaround for VirtualProtect failing while running the cluster tests.
 - [Fix] Fixed some win32 potential bugs (by @zeliard)

