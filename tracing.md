USDT Support -- Work In Progress
===============================

This document is intended to be a helpful overview for tracing Redis through
optional built-in static tracepoints.

Getting Started
---------------

### Building

To build with USDT support you'll need either Dtrace or the Systemtap USDT
development libraries (e.g. systemtap-sdt-dev on Debian/Ubuntu). On Solaris,
BSD, etc the usual Dtrace faculties most of these distributions ship with
should be fine.

Run with `make BUILD_USDT=yes`.

### Tests

FIXME would need dtrace and bpftrace test cases?

### Manually checking for tracepoints

On Linux, inspect the elf notes of the Redis server:

```
$ readelf --notes src/redis-server

Displaying notes found in: .note.ABI-tag
  Owner                 Data size       Description
  GNU                  0x00000010       NT_GNU_ABI_TAG (ABI version tag)
    OS: Linux, ABI: 3.2.0

Displaying notes found in: .note.stapsdt
  Owner                 Data size       Description
  stapsdt              0x00000036       NT_STAPSDT (SystemTap probe descriptors)
    Provider: redis
    Name: call__start
    Location: 0x0000000000044400, Base: 0x000000000018f970, Semaphore: 0x00000000001d10b2
    Arguments: -4@80(%r12)
  stapsdt              0x00000034       NT_STAPSDT (SystemTap probe descriptors)
    Provider: redis
    Name: call__end
    Location: 0x0000000000044410, Base: 0x000000000018f970, Semaphore: 0x00000000001d10b4
    Arguments: -4@80(%r12)
```

Adding new probes
-----------------------

New probes are added in the dtrace macro format:

```c
   /**
    * Fired before a command is called
    * @param id the command
    */
   probe my__probe(int my_arg);
```

This specifies the tracepoint signature, which will be used to generate a
header that defines two macros:

```c
REDIS_MY_PROBE(arg0);
REDIS_MY_PROBE_ENABLED();
```

A stub definition for each is added to `src/trace.h`, so that the code will
nop if tracepoints aren't enabled.

Within `src/trace.h`, a wrapper definition should also be created:

```c
#define TRACE_MY_PROBE(arg0)\
  REDIS_USDT_PROBE_HOOK(MY_PROBE, arg0)
```

This will ensure that all tracepoints are wrapped for efficiency purposes, and
allows them to be called idiomatically from within the Redis source code, like
`TRACE_##name(...)`, such as:

```c
a = 1
TRACE_MY_PROBE(a);
```

These macros can be placed within the C source code at strategic locations,
which will allow instrumentation to be placed within arbitrary contexts of the
source code, for any file with access to the scope of `src/trace.h`.

If collecting an argument would be expensive, then it should be additionally
guarded with:

```c
if(UNLIKELY(REDIS_MY_PROBE_ENABLED())
{
...
// expensive code to get an expensive_arg
...

  TRACE_CALL_START(expensive_arg);
}
```

On Linux a maximum of only 6 arguments is supported so as a best practice is
to not exceed 6 arguments to a tracepoint definition.

Performance
==========

Well-placed USDT tracepoints can help to solve performance problems. There is
a cost associated with executing a USDT tracepoint that should be considered
when tracing them.

Calls to a tracepoint are done like method calls, but should have no overhead
if all values emitted to the tracepoint are already available / computed.

In some cases where overhead is necessary, the long-term benefit of performance
gains may outweigh the short-term risk of performance losses while Redis is
instrumented.

Userspace Overhead
---------------

To minimize userspace overhead, the UNLIKELY macro is used to assist branch
prediction and prevent instruction pipeline flushing. All TRACE macro calls
are wrapped in an `UNLIKELY(REDIS_##name##_ENABLED())` check by default.
This helps to ensure that if a tracepoint isn't attached to, it won't affect
performance at all.

If data for a tracepoint would be expensive, conditionally gathering this data
can be gated by this same sort of check. This can allow for flexible data
collection, but can also make the cost of executing a probe measurable,
especially if the code being probed runs frequently.

Kernel overhead
---------------

Each time a tracepoint is executed with a probe attached to it, the Kernel will
run code in order to collect this information for a debugger like dtrace, gdb,
or bpftrace.

On Linux, this is easy to model, examining the disassembly of an eBPF probe
shows the instructions that would be executed on each tracepoint probe
invocation.

In general, the overhead should be lower than other methods to obtain this
data.


To-Do List
==========

Connection tracing
-----------------------

Base this off of memcached connection tracepoints


Trace command processing
-----------------------

