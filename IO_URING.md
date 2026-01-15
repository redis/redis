# Redis io_uring Support

This document describes the io_uring support implementation in Redis, which provides high-performance asynchronous I/O on Linux systems.

## Overview

io_uring is a modern Linux kernel interface for asynchronous I/O operations that significantly reduces system call overhead and improves performance, especially under high concurrency. This implementation adds io_uring support to Redis at two levels:

1. **Event Loop Backend (ae_iouring.c)**: Uses io_uring for I/O multiplexing (replacing epoll)
2. **Async Connection Type (conn_iouring.c)**: Provides true asynchronous I/O operations for network connections

## Requirements

- **Linux Kernel**: 5.6+ (5.19+ recommended for full feature support)
- **liburing**: Version 2.1+ (2.5+ recommended)
- **Compiler**: GCC or Clang with C11 support

## Installation

### On Rocky Linux 9 / RHEL 9 / CentOS Stream 9

```bash
# Enable CodeReady Builder repository
sudo dnf config-manager --set-enabled crb

# Install liburing development package
sudo dnf install -y liburing-devel

# Verify installation
pkg-config --modversion liburing

# Enable io_uring in the kernel (if disabled)
sudo sysctl -w kernel.io_uring_disabled=0
# Make it persistent across reboots
echo "kernel.io_uring_disabled = 0" | sudo tee -a /etc/sysctl.conf
```

### On Ubuntu 22.04+ / Debian 12+

```bash
# Install liburing development package
sudo apt-get update
sudo apt-get install -y liburing-dev

# Verify installation
pkg-config --modversion liburing
```

### On Other Linux Distributions

If liburing is not available in your distribution's repositories, you can build it from source:

```bash
git clone https://github.com/axboe/liburing.git
cd liburing
./configure
make
sudo make install
sudo ldconfig
```

## Building Redis with io_uring Support

### Standard Build with io_uring

```bash
cd /path/to/redis
make USE_IOURING=yes
```

### Build with io_uring and TLS

```bash
make USE_IOURING=yes BUILD_TLS=yes
```

### Build with io_uring and Modules

```bash
make USE_IOURING=yes BUILD_WITH_MODULES=yes
```

### Verify io_uring Support

Check that Redis is linked with liburing:

```bash
ldd src/redis-server | grep liburing
```

Expected output:
```
liburing.so.2 => /lib64/liburing.so.2 (0x...)
```

Check the event loop backend:

```bash
./src/redis-server --version
./src/redis-cli INFO server | grep multiplexing_api
```

## Running Redis with io_uring

### Start Redis Server

```bash
./src/redis-server
```

### Configuration

Currently, io_uring is automatically used when Redis is compiled with `USE_IOURING=yes`. The event loop backend selection priority is:

1. evport (Solaris)
2. **io_uring** (Linux with USE_IOURING=yes)
3. epoll (Linux)
4. kqueue (BSD/macOS)
5. select (fallback)

## Architecture

### Event Loop Backend (ae_iouring.c)

The io_uring event loop backend provides:

- **Multishot Poll**: Reduces SQE submissions by using persistent poll operations
- **Batch Processing**: Processes multiple events in a single syscall
- **Zero System Calls**: Potential for zero syscall polling with SQPOLL mode (future)

**Key Features:**
- Automatic fallback to epoll if io_uring initialization fails
- Support for both readable and writable events
- Efficient handling of large numbers of concurrent connections

### Async Connection Type (conn_iouring.c)

The io_uring connection type provides:

- **Async Read/Write**: True asynchronous I/O operations
- **Buffered I/O**: Pre-allocated buffers for reduced allocation overhead
- **Batch Submission**: Multiple I/O operations submitted together

**Note**: The async connection type is currently experimental and provides fallback to synchronous I/O when needed.

## Performance Considerations

### When io_uring Provides Benefits

1. **High Concurrency**: 10,000+ concurrent connections
2. **High Throughput**: Large response sizes (e.g., LRANGE of 10,000+ elements)
3. **Pipeline Operations**: Batched command execution
4. **Network-bound Workloads**: When network I/O is the bottleneck

### Expected Performance Improvements

Based on design targets:

| Scenario | Baseline (epoll) | With io_uring | Improvement |
|----------|------------------|---------------|-------------|
| Small requests (GET/SET) | 100% | 110-120% | +10-20% |
| Large responses (LRANGE 10000) | 100% | 130-150% | +30-50% |
| High concurrency (10K+ conns) | 100% | 115-125% | +15-25% |
| Pipeline (1000 commands) | 100% | 120-140% | +20-40% |

**Note**: Actual performance depends on workload, hardware, and kernel version.

## Troubleshooting

### io_uring Initialization Fails

**Error**: `Failed to initialize io_uring for async I/O: Operation not permitted`

**Cause**: io_uring may be disabled in the kernel.

**Solutions**:
1. Check if io_uring is disabled:
   ```bash
   sysctl kernel.io_uring_disabled
   ```
   If the value is `1` or `2`, io_uring is disabled.

2. Enable io_uring:
   ```bash
   sudo sysctl -w kernel.io_uring_disabled=0
   ```

3. Make it permanent by adding to `/etc/sysctl.conf`:
   ```bash
   echo "kernel.io_uring_disabled = 0" | sudo tee -a /etc/sysctl.conf
   ```

4. Check kernel version: `uname -r` (should be 5.6+)
5. Verify liburing installation: `ldconfig -p | grep liburing`

### Redis Falls Back to epoll

If Redis falls back to epoll, check:

```bash
# Check if Redis was compiled with io_uring support
strings src/redis-server | grep -i uring

# Check for HAVE_IOURING define
grep HAVE_IOURING src/config.h

# Verify build flags
make USE_IOURING=yes V=1 2>&1 | grep "USE_IOURING"
```

### Performance Not as Expected

1. **Verify io_uring is Active**:
   ```bash
   ./src/redis-cli INFO server | grep multiplexing_api
   ```
   Should show: `multiplexing_api:io_uring`

2. **Check Kernel Version**:
   ```bash
   uname -r
   ```
   Kernel 5.19+ provides the best io_uring support

3. **Monitor System Calls**:
   ```bash
   strace -c -p $(pgrep redis-server)
   ```
   With io_uring, you should see significantly fewer `epoll_wait` calls

## Limitations and Known Issues

1. **Linux Only**: io_uring is Linux-specific and not available on other platforms
2. **Kernel Version**: Requires Linux 5.6+; some features need 5.19+
3. **Experimental Status**: The async connection type (conn_iouring.c) is experimental
4. **TLS Compatibility**: TLS connections still use standard socket operations
5. **SQPOLL Mode**: Not yet implemented (would require CAP_SYS_NICE capability)

## Future Enhancements

Planned improvements for io_uring support:

1. **Fixed Buffers (IORING_REGISTER_BUFFERS)**: Pre-registered memory for zero-copy I/O
2. **Zero-copy Send (IORING_OP_SEND_ZC)**: For large responses
3. **Multishot Receive**: Single SQE producing multiple CQEs
4. **SQPOLL Mode**: Kernel-side polling for even lower latency
5. **IO Thread Integration**: Per-thread io_uring instances for better scalability
6. **Buffer Pools**: Automatic buffer management for async operations

## Technical Details

### File Structure

- `src/ae_iouring.c`: Event loop backend using io_uring for polling
- `src/conn_iouring.c`: Connection type for async I/O operations
- `src/conn_iouring.h`: Header file for io_uring connection API
- `src/config.h`: HAVE_IOURING feature detection
- `src/ae.c`: Backend selection logic
- `src/connection.c`: Connection type registration

### Build System Changes

- `src/Makefile`: Added USE_IOURING flag and liburing linking
- `src/config.h`: Added HAVE_IOURING conditional compilation

### API Extensions

The io_uring implementation adds these new APIs:

```c
/* Initialize io_uring subsystem */
int iouringInit(void);

/* Cleanup io_uring subsystem */
void iouringCleanup(void);

/* Submit async read/write requests */
int connIoUringAsyncRead(connection *conn);
int connIoUringAsyncWrite(connection *conn, const void *data, size_t len);

/* Batch submission and completion processing */
int connIoUringSubmit(void);
int connIoUringProcessCompletions(void);
```

## References

- [io_uring Documentation](https://kernel.dk/io_uring.pdf)
- [liburing GitHub](https://github.com/axboe/liburing)
- [Linux Kernel io_uring](https://kernel.org/doc/html/latest/io_uring.html)

## Contributing

If you encounter issues or have suggestions for io_uring support:

1. Check that you're using a supported kernel version (5.6+)
2. Verify liburing is properly installed
3. Report issues with:
   - Kernel version (`uname -r`)
   - liburing version (`pkg-config --modversion liburing`)
   - Build output (`make USE_IOURING=yes V=1`)
   - Redis INFO output

## License

The io_uring support code is licensed under the same terms as Redis:
- Redis Source Available License 2.0 (RSALv2), or
- Server Side Public License v1 (SSPLv1), or
- GNU Affero General Public License v3 (AGPLv3)
