MSOpenTech Redis on Windows 2.8 Release Notes
=============================================
--[ Redis on Windows 2.8.2400 ] Release date: Jan 21 2016

 - Merged tag 2.8.24 from antirez/2.8
 - [Docs] Added info about the memory working set showed by the task manager.
 - [Comment] Fixed typo.
 - [Docs] Updated README.md.
 - [PR]Add notice for VS2013 without Update 5
 - [Fix] Portability fix for strtol().
 - [Code cleanup] QForkStartup refactoring.
 - [Code cleanup] Comments, code formatting, minor code refactoring.
 - [Code refactoring] Moved global variables initialization in one place.
 - [Design Change] Use dlmalloc for sentinel and persistence off.
 - [Code cleanup] Fixed typo in comments and variable name.
 - [Code cleanup] Macro definition.
 - [Fix] x86 build break.
 - [Code cleanup] Removed unused and duplicated #defines.
 - [Test] Added regression test for replication when AUTH is on.
 - [Setup] Updated the command to push chocolatey packages.
 - [Fix] Wrong return value upon error. Code formatting.
 - [Log] Added Redis version at the top of the crash report.
 - [Fix] Disable replication if persistence is not available.
 - [Code cleanup] Better functions and variables names.
 - [Code cleanup] Error handling using ThrowLastError() method.
 - [Fix] Avoid potential null pointer access.
 - [PR] Documentation: replace argument sign '-' to '--'.
 - [Fix] Remove leftover .dat files from the working dir.
 - [Fix] do_socketSave2() rework for duplicated sockets management.
 - [Code cleanup] Variables/methods name, comments, formatting.
 - [Fix] Windows-specific fixes for replication.
 - [Test] Fixed bad merge.
 - [Portability] Fixes for type 'long'.
 - Merge tag 2.8.23 from antirez/2.8
 - [Code cleanup] Fixed tabs.
 - [Test] Removed Windows-specific workaround. Fixed tabs.
 - [Fix] Duplicated sockets need to be closed properly.
 - [Code cleanup] Renamed WSIOCP_ReceiveDone to WSIOCP_QueueNextRead.
 - [Fix] Sentinel notification-script 2nd argument needs quotes.
 - [PR] Passed STARTUPINFO parameter to CreateProcessA instead of NULL.
 - [Fix] Prevent UnhandledExceptiontHandler from entering a loop.
 - [Code refactoring] IsWindowsVersionAtLeast optimization.
 - [Change] Use VirtualProtect in RejoinCOWPages on Windows 6.2 and higher.

--[ Redis on Windows 2.8.2104 ] Release date: Oct 15 2015

NOTE: When runnig as a sentinel, redis-server.exe is not creating any more
      the RedisQFork_*.dat file.

 - [PR] Updated list of sentinel commands: announce-ip and announce-port. (by @rpannell)
 - [PR] Updated x86 debug and release configurations for all projects. (by @laurencee)
 - [PR] Changed Nuget package structure to support VS 2015 (by @Cybermaxs)
 - [Build] Fixed a build break (regression).
 - [Fix] Sentinel mode doesn't need to create a memory mapped file.

--[ Redis on Windows 2.8.2103 ] Release date: Sep 08 2015

NOTE: Two new features added to the MSI installer:
      - select whether to add the install dir to the PATH env variable.
      - select whether to set the maxmemory and maxheap flags.
      Improved error reporting if the installation fails or if the service
      fails to start: error messages logged to the Application Event Log.

 - [Setup] Added more information in the memory limit dialog.
 - [Setup] Fixed warning for missing properties attributes.
 - [Fix] Missing return code path.
 - [Fix] Reporting error code if listen() fails.
 - [Setup] Added maxheap settings along with the maxmemory settings.
 - [Setup] Added a checkbox to choose whether to add the install dir to PATH.
 - [Fix] Write errors to the Event Log during startup.
 - [Setup] MSI: added max memory config dialog.
 - [Setup] MSI: Redis installation folder added to the PATH environment variable.
 - [Fix] Redis crashes at startup.
 - [Fix] Improved error handling in WSIOCP_Listen().
 - [Fix] Remove temp file after AOF rewrite error.
 - [Fix] Close the file handle after truncating a file.
 - [Fix] replace_rename() had an infinite loop upon error.

--[ Redis on Windows 2.8.2102 ] Release date: Aug 25 2015

IMPORTANT NOTE: the 2.8.2102 release introduces some high impact changes
in the networking layer. Those changes fix some major bugs but since they
are touching significant part of the networking layer, we strongly
suggest to carefully test this release before using it in production.

 - [Fix] Fixed some win32 potential bugs. (By @zeliard)
 - [Cleanup] Code refactoring.
 - [Cleanup] Replaced DebugBreak with ASSERT.
 - [Code optimization] Socket flags moved to socket info structure.
 - [Fix & Cleanup] Socket state moved from aeApiState to RFDMap.
 - [Test] Increased waiting time while/before checking condition.
 - [Fix] Making sure getNextRFDAvailable never reaches MAX_INT.
 - [Fix] Sockets not properly closed.
 - [Test] Added a Windows-specific regression test.
 - [Test] Gitignore log file.
 - [Cleanup] Code refactoring.
 - [Debug] Disabled command line arguments print out.
 - [Fix] Socket state flags were not saved.
 - [Setup] Updated version number and release notes.
 - [Cleanup] Removed code that was commented out.
 - [Portability] Explicit type casting.
 - [Cleanup] Removed comment.
 - [Setup] NuGet description doesn't support Markdown.
 - [Cleanup] Change methods name and signature.
 - [Fix] Redis Server stops accepting connections.
 - [Setup] NuGet description doesn't support Markdown.
 - [Fix] AOF rewrite failure. (Credits to @ppanyukov for investigating the bug)

--[ Redis on Windows 2.8.2101 ] Release date: Jul 15 2015

 - [Fix] deleting char** correctly (@zeliard) 
 - [Fix] Fork code for background processing fixes and code refactoring.
 - [Fix] BeginForkOperation_Aof()/_Rdb()/_Socket() and BeginForkOperation() code
         refactoring.
 - [Fix] rewriteAppendOnlyFileBackground() code refactoring to minimize the code
         changes for WIN32.
  -[Fix] rewriteAppendOnlyFileBackground() must update the latency monitor, the
         fork stats and replicationScriptCacheFlush().
 - [Fix] rdbSaveBackground() code refactoring to minimize the code changes for
         WIN32.
 - [Fix] rdbSaveBackground() must update the latency monitor and the fork stats.
 - [Fix] memory leak in rdbSaveToSlavesSockets().
 - [Fix] properly releasing resources in rdbSaveToSlavesSockets().
 - [Fix] QForkChildInit() not setting the operationFailed event in case of
         exception.
 - [Fix] QForkChildInit() AV in catch() statement.
 - [Setup] Updated the scripts to create/push the NuGet/Chocolatey packages.
 - [Setup] EventLog.dll excluded from NuGet package.
 - [Fix] The stack trace was not logged when Redis is running as a service.
 - [Setup] Move ReleasePackagingTool to its own solution.
 - [WinPort] Explicit cast.
 - [Cleanup] Removed non-existent file from hiredis project.
 - [Cleanup] Minor change to method signature.
 - [Cleanup] Changed variable type to match function return value.
 - [Cleanup] Removed useless platform definition from RedisServer.sln
 - [Cleanup] Removed unused include.
 - [Fix] UnhandledExceptiontHandler internal exception handling.
 - [Fix] RFDMap was not thread safe.
 - [Change] Removed extra space allocated at the end of the mmap file.
 - [Cleanup] Tabs->spaces, formatting.
 - [Fix] aeWinQueueAccept wrong return value.
 - [Change] Removed the forkedProcessReady event.
 - [Change] Rolled back "Workaround for getpeername() issue".
 - [Cleanup] Code cleanup: variables initialization and validation.
 - [Fix] Socket blocking state.
 - [Fix] Memory corruption. Merged fix from Azure fork (by Mike Montwill).
 - [Fix] Added API mapping for fclose() and fileno().
 - [Setup] Updated the release number and the release notes (2.8.2101).
 - [Fix] Bug report fixes.
 - [Setup] Nuget/Chocolatey packages update.
 - [Fix] Memory was not properly released at the end of the memory test.
 - [Change] Hard-coded memory test loops changed from 50 to 5.

--[ Redis on Windows 2.8.21 ] Release date: Jun 24 2015

 - Merged Redis 2.8.21 [https://raw.githubusercontent.com/antirez/redis/2.8/00-RELEASENOTES]
 - Fixes for 64-bit portability.
 - Fixed rejoin pages on COW race condition.
 - Fixed AOF truncation.
 - Fixed crash when the 'save' flag is set and the 'persistence-available' flag is set to 'no'.
 - Logging a BUG REPORT (stack trace and server info) when Redis crashes.
 - Restored native Redis command arguments: -h, --help, -version, --version, --test-memory.
 - Install symbols for redis binaries.
 - Prevent firewall exception from being added if the Windows Firewall Windows service is stopped. (NickMRamirez)
 - Fix wrong pointer castings for x64 support. (zeliard)
 - Fix pointer casting for supporting a 64bit case. (zeliard)
 - Fix wrong memset argument. (zeliard)

--[ Redis on Windows 2.8.19.1 ] Release date: May 04 2015

 - Added an MSI installer, in addition to the .zip distribution
 - Various bug fixes, including:
     #167
     #228

--[ Redis on Windows 2.8.19 ] Release date: Feb 25 2015

 - Workaround for getpeername() issue, which affected sentinel failover over ipv6.
 - Miscellaneous bug fixes.

--[ Redis on Windows 2.8.17.4 ] Release date: Feb 02 2015

 - Fix AV in AllocHeapBlock.

--[ Redis on Windows 2.8.17.3 ] Release date: Dec 26 2014

 - Fix redis-cli pipe mode.

--[ Redis on Windows 2.8.17.2 ] Release date: Dec 23 2014

 - Moved binaries out of the repository, to the Releases page.
 - Miscellaneous bug fixes.

--[ Redis on Windows 2.8.17.1 ] Release date: Dec 16 2014

 - Move release binaries to release page.
 - Adopting the suggestions at #172.
 - We no longer commit the binaries into the repo.
 - Instead, we create periodic releases on the release page.

--[ Redis on Windows 2.8.12 ] Release date: Sep 4 2014

--[ Redis on Windows 2.8.9 ] Release date: Jun 26 2014

--[ Redis on Windows 2.6.14 ] Release date: May 20 2014

--[ Redis on Windows 2.8.4 ] Release date: May 20 2014

--[ Redis on Windows 2.6.8 ] Release date: May 6 2013

--[ Redis on Windows 2.4.6 ] Release date: Feb 10 2012
