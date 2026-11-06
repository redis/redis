Redis Test Suite
================

The normal execution mode of the test suite involves starting and manipulating
local `redis-server` instances, inspecting process state, log files, etc.

The test suite also supports execution against an external server, which is
enabled using the `--host` and `--port` parameters. When executing against an
external server, tests tagged `external:skip` are skipped.

There are additional runtime options that can further adjust the test suite to
match different external server configurations:

| Option               | Impact                                                   |
| -------------------- | -------------------------------------------------------- |
| `--singledb`         | Only use database 0, don't assume others are supported. |
| `--ignore-encoding`  | Skip all checks for specific encoding.  |
| `--ignore-digest`    | Skip key value digest validations. |
| `--cluster-mode`     | Run in strict Redis Cluster compatibility mode. |
| `--large-memory`     | Enables tests that consume more than 100mb |

Tags
----

Tags are applied to tests to classify them according to the subsystem they test,
but also to indicate compatibility with different run modes and required
capabilities.

Tags can be applied in different context levels:
* `start_server` context
* `tags` context that bundles several tests together
* A single test context.

The following compatibility and capability tags are currently used:

| Tag                       | Indicates |
| ---------------------     | --------- |
| `external:skip`           | Not compatible with external servers. |
| `cluster:skip`            | Not compatible with `--cluster-mode`. |
| `large-memory`            | Test that requires more than 100mb |
| `tls:skip`                | Not compatible with `--tls`. |
| `tsan:skip`               | Not compatible with running under thread sanitizer. |
| `needs:repl`              | Uses replication and needs to be able to `SYNC` from server. |
| `needs:debug`             | Uses the `DEBUG` command or other debugging focused commands (like `OBJECT REFCOUNT`). |
| `needs:pfdebug`           | Uses the `PFDEBUG` command. |
| `needs:config-maxmemory`  | Uses `CONFIG SET` to manipulate memory limit, eviction policies, etc. |
| `needs:config-resetstat`  | Uses `CONFIG RESETSTAT` to reset statistics. |
| `needs:reset`             | Uses `RESET` to reset client connections. |
| `needs:save`              | Uses `SAVE` or `BGSAVE` to create an RDB file. |

When using an external server (`--host` and `--port`), filtering using the
`external:skip` tags is done automatically.

When using `--cluster-mode`, filtering using the `cluster:skip` tag is done
automatically.

When not using `--large-memory`, filtering using the `largemem:skip` tag is done
automatically.

In addition, it is possible to specify additional configuration. For example, to
run tests on a server that does not permit `SYNC` use:

    ./runtest --host <host> --port <port> --tags -needs:repl

