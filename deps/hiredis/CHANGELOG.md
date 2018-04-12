### 0.11.0

* Increase the maximum multi-bulk reply depth to 7.

* Increase the read buffer size from 2k to 16k.

* Use poll(2) instead of select(2) to support large fds (>= 1024).

### 0.10.1

* Makefile overhaul. Important to check out if you override one or more
  variables using environment variables or via arguments to the "make" tool.

* Issue #45: Fix potential memory leak for a multi bulk reply with 0 elements
  being created by the default reply object functions.

* Issue #43: Don't crash in an asynchronous context when Redis returns an error
  reply after the connection has been made (this happens when the maximum
  number of connections is reached).

### 0.10.0

* See commit log.

