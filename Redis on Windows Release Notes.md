MSOpenTech Redis on Windows 3.0 Release Notes
=============================================

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

