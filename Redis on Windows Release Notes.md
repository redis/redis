MSOpenTech Redis on Windows 3.0 Release Notes
=============================================

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

