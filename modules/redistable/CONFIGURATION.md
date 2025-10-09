# Redis Table Module - Configuration Guide

## Overview

The Redis Table Module supports runtime configuration to adjust performance characteristics based on your workload.

---

## Configuration Parameters

### `max_scan_limit`

**Purpose**: Controls the maximum number of rows that can be scanned in a single query operation.

**Default**: 100,000 rows

**Range**: 1,000 to 10,000,000 rows

**When it applies**: 
- `TABLE.SELECT` with comparison operators (>, <, >=, <=)
- `TABLE.UPDATE` with comparison operators
- `TABLE.DELETE` with comparison operators

**When it does NOT apply**:
- Indexed equality searches (`WHERE indexed_col=value`) - These are O(1) lookups
- `TABLE.INSERT` operations
- Schema operations (`CREATE`, `ALTER`, `DROP`)

---

## Configuration Methods

### Method 1: Module Load-Time Configuration (Recommended)

Configure when loading the module:

```bash
redis-server --loadmodule redis_table.so max_scan_limit <value>
```

**Examples:**

```bash
# Default configuration (100K limit)
redis-server --loadmodule redis_table.so

# Analytics workload (500K limit)
redis-server --loadmodule redis_table.so max_scan_limit 500000

# Large-scale batch processing (1M limit)
redis-server --loadmodule redis_table.so max_scan_limit 1000000

# Conservative for shared instances (50K limit)
redis-server --loadmodule redis_table.so max_scan_limit 50000
```

**Verification:**

After starting Redis, the configuration will be logged:
```
[notice] Table module: max_scan_limit set to 500000
```

---

## Use Case Recommendations

### High-Traffic Production (Keep Default: 100K)

**Scenario**: Multi-tenant SaaS, high QPS, shared Redis instance

**Configuration**:
```bash
redis-server --loadmodule redis_table.so
# Uses default 100K limit
```

**Why**:
- ✅ Protects Redis from blocking
- ✅ Predictable query latency
- ✅ Other clients remain responsive
- ✅ Forces good design (use indexes)

---

### Analytics Workloads (Increase: 500K-1M)

**Scenario**: Data analytics, reporting, complex queries on large tables

**Configuration**:
```bash
redis-server --loadmodule redis_table.so max_scan_limit 1000000
```

**Why**:
- ✅ Allows complex analytical queries
- ✅ Reduces "scan limit exceeded" errors
- ✅ Better for read-heavy workloads
- ⚠️ May increase query latency

**Best practices**:
- Run analytics on replica servers
- Schedule during low-traffic periods
- Monitor Redis CPU usage

---

### Batch Processing / Data Migration (Increase: 1M-10M)

**Scenario**: One-time data migration, batch updates, data transformations

**Configuration**:
```bash
redis-server --loadmodule redis_table.so max_scan_limit 10000000
```

**Why**:
- ✅ Handles large datasets
- ✅ Fewer iterations needed
- ✅ Faster migration completion
- ⚠️ Can block Redis for seconds

**Best practices**:
- Use dedicated Redis instance
- Run during maintenance windows
- Monitor progress with logging

---

### Shared / Multi-Tenant (Decrease: 10K-50K)

**Scenario**: Shared hosting, strict SLAs, noisy neighbor prevention

**Configuration**:
```bash
redis-server --loadmodule redis_table.so max_scan_limit 10000
```

**Why**:
- ✅ Strict latency guarantees
- ✅ Prevents abuse
- ✅ Fair resource sharing
- ⚠️ May require more indexes

---

## Monitoring & Troubleshooting

### Check Current Configuration

The configuration is logged when the module loads:

```bash
$ redis-server --loadmodule redis_table.so max_scan_limit 200000
# Look for log line:
# [notice] Table module: max_scan_limit set to 200000
```

### Error: Scan Limit Exceeded

**Error Message**:
```
ERR query scan limit exceeded (max 100000 rows). Use indexed columns or add more specific conditions.
```

**Solutions**:

1. **Add indexes to queried columns** (Best):
   ```bash
   TABLE.SCHEMA.ALTER mydb.users ADD INDEX age
   # Now this works: TABLE.SELECT mydb.users WHERE age=30
   ```

2. **Add more specific WHERE conditions**:
   ```bash
   # Instead of: TABLE.SELECT users WHERE age>25
   # Use: TABLE.SELECT users WHERE dept=Engineering AND age>25
   ```

3. **Increase scan limit** (if appropriate):
   ```bash
   # Restart Redis with higher limit
   redis-server --loadmodule redis_table.so max_scan_limit 500000
   ```

4. **Partition large tables**:
   ```bash
   # Split by date, category, or region
   TABLE.SCHEMA.CREATE mydb.users_2024 ...
   TABLE.SCHEMA.CREATE mydb.users_2023 ...
   ```

---

## Performance Impact

### Scan Limit vs Query Performance

| Limit | Small Tables (<10K) | Medium Tables (10K-100K) | Large Tables (>100K) |
|-------|---------------------|--------------------------|----------------------|
| **10K** | No impact | May hit limit | Will hit limit |
| **100K** (default) | No impact | No impact | May hit limit |
| **500K** | No impact | No impact | Rarely hit limit |
| **1M+** | No impact | No impact | Unlikely to hit limit |

**Performance overhead**: Minimal - just an integer counter check per row

---

## Configuration Validation

### Valid Configurations

```bash
# Minimum (1K)
redis-server --loadmodule redis_table.so max_scan_limit 1000

# Default (100K) - explicit
redis-server --loadmodule redis_table.so max_scan_limit 100000

# Maximum (10M)
redis-server --loadmodule redis_table.so max_scan_limit 10000000
```

### Invalid Configurations

```bash
# Too low (< 1000) - will use default 100K
redis-server --loadmodule redis_table.so max_scan_limit 500
# Warning logged, uses default

# Too high (> 10M) - will use default 100K
redis-server --loadmodule redis_table.so max_scan_limit 50000000
# Warning logged, uses default

# Invalid value - will use default 100K
redis-server --loadmodule redis_table.so max_scan_limit abc
# Warning logged, uses default
```

---

## Concurrency Considerations

⚠️ **Known Limitation (to be fixed in v2.2):**

### DROP INDEX Race Condition

**Issue**: Concurrent `DROP INDEX` operations may cause queries to return incorrect results.

**Technical Details**:
```
Step 1: DROP INDEX removes metadata (SREM - atomic, fast)
Step 2: DROP INDEX deletes index keys (SCAN loop - non-atomic, slow)

If a query starts between Step 1 and Step 2:
- Query checks: is column indexed? → NO (metadata already removed)
- Query attempts: index lookup → index keys being deleted → EMPTY result ❌
```

**Impact**:
- Queries return empty set instead of falling back to full scan
- No error raised (silent failure)
- Race window proportional to index size (milliseconds to seconds)

**Production Recommendations**:

1. **Schema Change Protocol**:
   ```bash
   # DO NOT run schema changes during peak traffic
   # Use maintenance windows for DROP INDEX operations
   
   # Good practice:
   - Schedule during low-traffic periods
   - Announce schema changes to application team
   - Monitor query results after schema changes
   ```

2. **Application-Level Coordination**:
   ```python
   # Python example
   with app.schema_lock:
       redis.execute_command("TABLE.SCHEMA.ALTER", "users", "DROP", "INDEX", "age")
   ```

3. **Monitoring**:
   ```bash
   # Watch for unexpected empty results
   # Alert on schema changes during business hours
   # Log all DROP INDEX operations
   ```

4. **Alternative**: Keep indexes instead of dropping them
   - Indexes have minimal overhead if not queried
   - Only drop if truly necessary (storage constraints)

**Safe Operations**:
- ✅ `ADD INDEX` - Safe (builds incrementally, atomic visibility)
- ✅ Query operations - Safe (read-only)
- ⚠️ `DROP INDEX` - Use during maintenance windows

**Planned Fix (v2.2)**:
- Reverse deletion order OR soft-delete tombstone pattern
- Will ensure queries either use index correctly or fall back to full scan

---

## Best Practices

### 1. Start Conservative, Increase as Needed

```bash
# Start with default
redis-server --loadmodule redis_table.so

# Monitor for "scan limit exceeded" errors
# Increase only if needed for your workload
```

### 2. Different Limits for Different Environments

```bash
# Development: Higher limit for convenience
dev: redis-server --loadmodule redis_table.so max_scan_limit 500000

# Staging: Match production
staging: redis-server --loadmodule redis_table.so max_scan_limit 100000

# Production: Conservative default
prod: redis-server --loadmodule redis_table.so
```

### 3. Use Indexes for Production Queries

```bash
# ❌ Slow - requires scan
TABLE.SELECT users WHERE age>30

# ✅ Fast - uses index
TABLE.SCHEMA.ALTER users ADD INDEX age
TABLE.SELECT users WHERE age=30  # O(1) lookup
```

### 4. Monitor Redis Performance

```bash
# Check Redis CPU usage
redis-cli INFO cpu

# Check slow queries
redis-cli SLOWLOG GET 10

# Monitor command stats
redis-cli INFO commandstats
```

---

## Quick Reference

| Use Case | Recommended Limit | Command |
|----------|------------------|---------|
| **Production (high traffic)** | 100K (default) | `--loadmodule redis_table.so` |
| **Analytics** | 500K-1M | `--loadmodule redis_table.so max_scan_limit 500000` |
| **Batch processing** | 1M-10M | `--loadmodule redis_table.so max_scan_limit 5000000` |
| **Shared hosting** | 10K-50K | `--loadmodule redis_table.so max_scan_limit 50000` |
| **Development** | 500K | `--loadmodule redis_table.so max_scan_limit 500000` |

---

## FAQs

**Q: Can I change the limit without restarting Redis?**
A: No, the configuration is set at module load time and requires a restart to change.

**Q: What happens if I exceed the limit?**
A: The query returns an error: `ERR query scan limit exceeded`. No data is modified.

**Q: Does this affect indexed queries?**
A: No. Indexed equality searches (`WHERE col=value`) are O(1) lookups and don't scan rows.

**Q: What's the performance overhead?**
A: Minimal - just an integer comparison per row scanned (~nanoseconds).

**Q: Can I set different limits for different tables?**
A: No, the limit is global for the entire module instance.

**Q: What if I need to scan more than 10M rows?**
A: Consider:
  - Adding indexes
  - Partitioning tables
  - Using external analytics tools
  - Running on a dedicated Redis instance

---

**Version**: 2.1
**Last Updated**: 2025-10-09
