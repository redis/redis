<!-- MarkdownTOC -->

- [Introduction](#introduction)
- [Features](#features)
- [Details](#details)
    - [xslaveof command](#xslaveof-command)
    - [force full sync](#force-full-sync)
    - [slave can replicate all commands to it's slaves](#slave-can-replicate-all-commands-to-its-slaves)
    - [enhanced partial sync](#enhanced-partial-sync)

<!-- /MarkdownTOC -->


<a name="introduction"></a>
# Introduction
XRedis is [ctrip](http://www.ctrip.com/) redis branch. Ctrip is a leading provider of travel services including accommodation reservation, transportation ticketing, packaged tours and corporate travel management.

<a name="features"></a>
# Features
* all features of redis 4.0.8 are inherited (from xredis 1.0.1)
* xslaveof command support
* force full sync
* enhanced partial sync

<a name="details"></a>
# Details

<a name="xslaveof-command"></a>
## xslaveof command

Suppose that redis slave is connectted to redis master(`ip1 port1`). When command `slaveof ip2 port2` is sent to this slave, redis will do the following:

Slave try partial resynchronization at the cron time(one time per second by default).

`xslaveof ip port` is a promotion for `slaveof`. Using the new command, slave will try partial resynchronization as soon as possible

<a name="force-full-sync"></a>
## force full sync
    * 命令 `refullsync`
    force all slaves reconnect itself, and fullsync with slaves
<a name="slave-can-replicate-all-commands-to-its-slaves"></a>
## slave can replicate all commands to it's slaves
**WARN: dangerous, you have to know what you are doing when using this command**

    config set slave-repl-all yes
    config set slave-repl-all no

1. Config remain in memory, never persist to disk
2. Cofig will automatically become yes when server become master

<a name="enhanced-partial-sync"></a>
## enhanced partial sync
- Do not free replication log right after slave become master

Normally if master has no longer connected slave for `repl-backlog-ttl` seconds, it will free replication log.  
When slave becomes master, we hope that redis will restart the timer. Actually offical redis doesn't do this. So it may free replication log immediately.

- Whatever fail message master returns, slave will always try partial sync.







