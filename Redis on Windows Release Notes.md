MSOpenTech Redis on Windows 2.8 Release Notes
=============================================

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
