MSOpenTech Redis 2.8.17 Release Notes
=====================================

Welcome to the binary release of Redis from Microsoft Open Technologies, Inc.

What is Redis?
--------------

Redis is an open source, high performance, key-value store. Values may contain strings, hashes, lists, sets and sorted sets. Redis has been developed primarily for UNIX-like operating systems.

Porting Goals
-------------

Our goal is to provide a version of Redis that runs on Windows with a performance essentially equal to the performance of Redis on an equivalent UNIX machine.

What is new with the 2.8.17 release
-----------------------------------

Our last official release was 2.8.12. We have merged in the changes up to 2.8.17. Please see the [release notes for the UNIX 2.8 branch](http://download.redis.io/redis-stable/00-RELEASENOTES) to understand how this impacts Redis functionality.

### Network layer changes

There have been significant changes to the networking layer for this version. Likely there will be a few weeks before there is another official (Chocolatey and Nuget) release. Most of these changes target IPv6.

### persistence-available flag 

If Redis is to be used as an in-memory-only cache without any kind of persistence, then the fork() mechanism used by the background AOF/RDB persistence is unnecessary. As an optimization, all persistence can be turned off in the Windows version of Redis in this scenario. This will disable the creation of the memory mapped heap file, redirect heap allocations to the system heap allocator, and disable commands that would otherwise cause fork() operations: BGSAVE and BGREWRITEAOF. This flag may not be combined with any of the other flags that configure AOF and RDB operations.

persistence-available [(yes)|no]

How to develop for Redis
------------------------

You will need a client library for accessing Redis. There are a wide variety of client libraries available as listed at <http://redis.io/clients>.

