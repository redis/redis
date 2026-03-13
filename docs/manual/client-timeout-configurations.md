# Redis Client Timeout Configurations

This guide provides examples and best practices for configuring timeouts in Redis client libraries across different programming languages.

## Overview

Proper timeout configuration is crucial for building reliable applications with Redis. This document covers:
- Connection timeouts
- Socket timeouts
- Command timeouts
- Keepalive settings

## Python Example (redis-py)

```python
import redis

# Basic timeout configuration
redis_client = redis.Redis(
    host='localhost',
    port=6379,
    socket_timeout=5,  # Overall socket timeout in seconds
    socket_connect_timeout=2,  # Connection timeout in seconds
    socket_keepalive=True  # Enable TCP keepalive
)

# Configuration with retry logic
from redis.retry import Retry
from redis.backoff import ExponentialBackoff
from redis.exceptions import TimeoutError, ConnectionError

retry = Retry(ExponentialBackoff(), 3)
redis_client = redis.Redis(
    host='localhost',
    port=6379,
    socket_timeout=5,
    retry=retry
)
```

## Node.js Example (node-redis)

```javascript
const redis = require('redis');

// Basic timeout configuration
const client = redis.createClient({
  socket: {
    host: 'localhost',
    port: 6379,
    connectTimeout: 2000,  // Connection timeout in milliseconds
    timeout: 5000,        // Operation timeout in milliseconds
    keepAlive: 1000,      // TCP keepalive in milliseconds
    noDelay: true         // Disable Nagle's algorithm
  }
});

// With retry strategy
const client = redis.createClient({
  socket: {
    host: 'localhost',
    port: 6379,
    connectTimeout: 2000,
    timeout: 5000
  },
  retry_strategy: function(options) {
    if (options.error && options.error.code === 'ECONNREFUSED') {
      return new Error('Server refused connection');
    }
    if (options.total_retry_time > 1000 * 60 * 60) {
      return new Error('Retry time exhausted');
    }
    if (options.attempt > 10) {
      return undefined;
    }
    return Math.min(options.attempt * 100, 3000);
  }
});
```

## Java Example (Jedis)

```java
import redis.clients.jedis.Jedis;
import redis.clients.jedis.JedisPool;
import redis.clients.jedis.JedisPoolConfig;

// Basic configuration
JedisPoolConfig poolConfig = new JedisPoolConfig();
poolConfig.setMaxTotal(100);
poolConfig.setMaxIdle(20);
poolConfig.setMinIdle(10);

// Timeout configuration (all values in milliseconds)
int connectionTimeout = 2000;  // Connection timeout
int socketTimeout = 5000;      // Socket timeout

JedisPool pool = new JedisPool(
    poolConfig,
    "localhost",
    6379,
    connectionTimeout,
    socketTimeout
);

// Using the pool with timeouts
try (Jedis jedis = pool.getResource()) {
    jedis.setex("key", 60, "value");
} catch (JedisConnectionException e) {
    // Handle timeout errors
}
```

## Best Practices

1. **Connection Timeouts**
   - Set shorter timeouts for initial connections (1-3 seconds)
   - Consider your network latency when setting this value
   - Implement retry logic for connection failures

2. **Socket Timeouts**
   - Set based on expected operation time
   - Consider using different timeouts for read/write operations
   - Default: 5-10 seconds for general operations

3. **Keepalive Settings**
   - Enable for long-lived connections
   - Consider your infrastructure requirements
   - Useful for connections through firewalls/proxies

4. **Error Handling**
   - Always implement proper error handling for timeout scenarios
   - Consider implementing circuit breakers for repeated timeouts
   - Log timeout occurrences for monitoring

## Common Issues and Solutions

1. **Connection Timeouts**
   ```
   Problem: Redis connection timeout after 2000 milliseconds
   Solution: Increase connect_timeout or check network issues
   ```

2. **Operation Timeouts**
   ```
   Problem: Socket timeout during large operation
   Solution: Adjust socket_timeout based on operation size
   ```

3. **Idle Connection Drops**
   ```
   Problem: Connection dropped after period of inactivity
   Solution: Enable keepalive with appropriate interval
   ```

## When to Adjust Timeouts

1. **Increase Timeouts When:**
   - Dealing with large datasets
   - Network latency is high
   - Bulk operations are common

2. **Decrease Timeouts When:**
   - Quick failure detection is needed
   - Resources are limited
   - Operations should be quick

## Monitoring Recommendations

Monitor these metrics for timeout-related issues:
- Connection failure rates
- Operation latency
- Timeout frequency
- Retry attempts

For more information about client configurations, see the [Redis documentation](https://redis.io/topics/clients).