# Redis Table Module - Complete Guide

A Redis module that implements SQL-like tables with full CRUD operations, explicit index control, comparison operators, and support for multiple data types.

## Requirements Summary

1. ✅ Create SQL-like tables with namespace
2. ✅ Namespace must be created before tables
3. ✅ Full CRUD operations (CREATE, INSERT, SELECT, UPDATE, DELETE, DROP)
4. ✅ Automatic row ID generation
5. ✅ Index management (auto-create, auto-update, auto-remove)
6. ✅ Explicit index control (define which columns are indexed)
7. ✅ Dynamic table schema modification (ADD/DROP columns and indexes)
8. ✅ TABLE.SCHEMA.VIEW command to display schema
9. ✅ Support for string, integer, float, and date types
10. ✅ Comparison operators: =, >, <, >=, <=
11. ✅ AND/OR operators in WHERE clause
12. ✅ Index validation (equality searches require indexed columns)

## Build

The module includes a fully functional Makefile with the following targets:

```bash
# Build the module
make

# Clean build artifacts
make clean

# Run comprehensive test suite (86 tests)
make test

# Build with debug information
make debug
```

### Manual Build (if needed)
```bash
cd /home/ubuntu/Projects/REDIS/redis/modules/redistable
gcc -Wall -Werror -g -O0 -fPIC -I../../src -c redis_table.c
gcc -shared -o redis_table.so redis_table.o
```

## Run Redis with Module

```bash
# From the redistable directory
cd /home/ubuntu/Projects/REDIS/redis/modules/redistable

# Start Redis with the module loaded
cd ../../..
./src/redis-server --loadmodule modules/redistable/redis_table.so

# In another terminal
./src/redis-cli
```

### Quick Test
```bash
# Build and test the module
cd /home/ubuntu/Projects/REDIS/redis/modules/redistable
make clean && make && make test
```

## Supported Data Types

| Type    | Format          | Example       | Validation                          | Comparison |
|---------|-----------------|---------------|-------------------------------------|------------|
| string  | Any text        | `"Hello"`     | None                                | Lexical    |
| integer | Whole number    | `123`, `-45`  | Digits only, optional +/- prefix    | Numeric    |
| float   | Decimal number  | `123.45`      | Digits + optional decimal point     | Numeric    |
| date    | YYYY-MM-DD      | `2024-01-15`  | Exactly 10 chars, hyphens at 4 & 7  | Lexical    |

## Commands Reference

### TABLE.NAMESPACE.CREATE
Create a namespace (required before creating tables).

**Syntax:**
```
TABLE.NAMESPACE.CREATE <namespace>
```

**Example:**
```bash
TABLE.NAMESPACE.CREATE mydb
```

### TABLE.NAMESPACE.VIEW
Display all namespace:table pairs, optionally filtered by namespace.

**Syntax:**
```
TABLE.NAMESPACE.VIEW [<namespace>]
```

**Examples:**
```bash
# View all tables across all namespaces
TABLE.NAMESPACE.VIEW

# Returns (example):
mydb:employees
mydb:users
testdb:products

# View tables in a specific namespace
TABLE.NAMESPACE.VIEW mydb

# Returns (example):
mydb:employees
mydb:users
```

**How it works:**
1. Scans all `schema:*.*` keys to find tables
2. Optionally filters by namespace if provided
3. Returns sorted list of `namespace:table` pairs

### TABLE.SCHEMA.VIEW
Display table schema with columns, types, and index status.

**Syntax:**
```
TABLE.SCHEMA.VIEW <schema.table>
```

**Example:**
```bash
TABLE.SCHEMA.VIEW mydb.people

# Returns:
1) 1) "FNAME"
   2) "string"
   3) "true"
2) 1) "LNAME"
   2) "string"
   3) "true"
3) 1) "AGE"
   2) "integer"
   3) "false"
4) 1) "SALARY"
   2) "float"
   3) "false"
5) 1) "HIREDATE"
   2) "date"
   3) "true"
```

**How it works:**
1. Reads `schema:<table>` hash for columns and types
2. Checks `idx:meta:<table>` set for indexed columns
3. Returns `[column, type, indexed]` for each column

### TABLE.SCHEMA.CREATE
Create a table with columns and optional index control.

**Syntax:**
```
TABLE.SCHEMA.CREATE <schema.table> <col:type[:index]> [<col:type[:index]> ...]
```

- `col` - column name
- `type` - `string`, `integer`, `float`, or `date`
- `index` - `true` or `false` (optional, defaults to `true`)

**Examples:**
```bash
# All columns indexed (backward compatible)
TABLE.SCHEMA.CREATE mydb.people FNAME:string LNAME:string AGE:integer COUNTRY:string

# Explicit index control
TABLE.SCHEMA.CREATE mydb.people FNAME:string:true LNAME:string:true AGE:integer:false COUNTRY:string:true

# With new types (float, date)
TABLE.SCHEMA.CREATE mydb.employees EMPID:string:true NAME:string:true SALARY:float:false HIREDATE:date:true
```

### TABLE.SCHEMA.ALTER
Dynamically modify table structure and indexes.

**Syntax:**
```
TABLE.SCHEMA.ALTER <schema.table> ADD COLUMN <col:type[:index]>
TABLE.SCHEMA.ALTER <schema.table> ADD INDEX <col>
TABLE.SCHEMA.ALTER <schema.table> DROP INDEX <col>
```

**Important:** `ADD INDEX` automatically builds indexes for all existing rows!

**Examples:**
```bash
# Add new column
TABLE.SCHEMA.ALTER mydb.people ADD COLUMN EMAIL:string:true
TABLE.SCHEMA.ALTER mydb.people ADD COLUMN SALARY:float:false
TABLE.SCHEMA.ALTER mydb.people ADD COLUMN BIRTHDATE:date:true

# Add index to existing column (builds for all existing rows!)
TABLE.SCHEMA.ALTER mydb.people ADD INDEX AGE

# Remove index (keeps column, deletes all index keys)
TABLE.SCHEMA.ALTER mydb.people DROP INDEX AGE
```

### TABLE.INSERT
Insert a new row with auto-generated ID.

**Syntax:**
```
TABLE.INSERT <schema.table> <col>=<value> [<col>=<value> ...]
```

**Examples:**
```bash
TABLE.INSERT mydb.people FNAME=John LNAME=Doe AGE=30 COUNTRY=USA
# Returns: (integer) 1

TABLE.INSERT mydb.employees EMPID=E001 NAME=John SALARY=50000.50 HIREDATE=2020-01-15
# Returns: (integer) 1
```

### TABLE.SELECT
Query rows with optional WHERE clause and comparison operators.

**Syntax:**
```
TABLE.SELECT <schema.table> [WHERE <col><op><value> (AND|OR <col><op><value> ...)]
```

**Operators:** `=`, `>`, `<`, `>=`, `<=`

**Important:** Equality (`=`) requires indexed columns. Comparison operators work on any column but scan all rows.

**Examples:**
```bash
# Select all rows
TABLE.SELECT mydb.people

# Equality on indexed column (fast)
TABLE.SELECT mydb.people WHERE COUNTRY=USA
TABLE.SELECT mydb.people WHERE FNAME=John

# Comparison operators (scans all rows)
TABLE.SELECT mydb.people WHERE AGE>30
TABLE.SELECT mydb.people WHERE AGE>=25
TABLE.SELECT mydb.people WHERE AGE<40

# Float comparisons
TABLE.SELECT mydb.employees WHERE SALARY>50000.00
TABLE.SELECT mydb.employees WHERE SALARY<=60000.00

# Date comparisons
TABLE.SELECT mydb.employees WHERE HIREDATE>2020-01-01
TABLE.SELECT mydb.employees WHERE HIREDATE>=2020-01-01 AND HIREDATE<=2020-12-31

# Combined conditions
TABLE.SELECT mydb.people WHERE AGE>25 AND COUNTRY=USA
TABLE.SELECT mydb.people WHERE COUNTRY=USA OR COUNTRY=Canada
TABLE.SELECT mydb.people WHERE AGE>=30 OR COUNTRY=France
```

### TABLE.UPDATE
Update rows matching WHERE clause.

**Syntax:**
```
TABLE.UPDATE <schema.table> WHERE <cond> (AND|OR <cond> ...) SET <col>=<value> [<col>=<value> ...]
```

**Examples:**
```bash
TABLE.UPDATE mydb.people WHERE FNAME=John SET AGE=31
TABLE.UPDATE mydb.people WHERE AGE=30 AND COUNTRY=Canada SET COUNTRY=France
TABLE.UPDATE mydb.employees WHERE EMPID=E001 SET SALARY=52000.75 HIREDATE=2020-02-01
```

### TABLE.DELETE
Delete rows matching WHERE clause.

**Syntax:**
```
TABLE.DELETE <schema.table> [WHERE <cond> (AND|OR <cond> ...)]
```

**Examples:**
```bash
TABLE.DELETE mydb.people WHERE COUNTRY=France
TABLE.DELETE mydb.people WHERE AGE>40
TABLE.DELETE mydb.people WHERE FNAME=Jane OR FNAME=Bob
```

### TABLE.DROP
Drop a table (removes table schema, all rows, indexes, and metadata).

**Syntax:**
```
TABLE.DROP <schema.table> FORCE
```

**Important:** The `FORCE` parameter is required to confirm the irreversible deletion. Without it, the command will fail with a warning message.

**Examples:**
```bash
# Without FORCE - will fail with warning
TABLE.DROP mydb.people
# Returns: (error) ERR This operation is irreversible, use FORCE parameter to remove the table

# With FORCE - will succeed
TABLE.DROP mydb.people FORCE
# Returns: OK
```

### TABLE.HELP
Display help text.

**Syntax:**
```
TABLE.HELP
```

## Complete Example

```bash
FLUSHALL

# 1. Create namespace
TABLE.NAMESPACE.CREATE mydb

# 2. Create table with explicit index control and new types
TABLE.SCHEMA.CREATE mydb.employees EMPID:string:true FNAME:string:true LNAME:string:true AGE:integer:false SALARY:float:false DEPT:string:true HIREDATE:date:true

# 3. View all tables in all namespaces
TABLE.NAMESPACE.VIEW

# 4. View tables in specific namespace
TABLE.NAMESPACE.VIEW mydb

# 5. View table schema
TABLE.SCHEMA.VIEW mydb.employees

# 6. Insert data
TABLE.INSERT mydb.employees EMPID=E001 FNAME=John LNAME=Doe AGE=30 SALARY=50000.50 DEPT=Engineering HIREDATE=2020-01-15
TABLE.INSERT mydb.employees EMPID=E002 FNAME=Jane LNAME=Smith AGE=28 SALARY=55000.75 DEPT=Marketing HIREDATE=2021-03-20
TABLE.INSERT mydb.employees EMPID=E003 FNAME=Bob LNAME=Johnson AGE=35 SALARY=60000.00 DEPT=Engineering HIREDATE=2019-06-10
TABLE.INSERT mydb.employees EMPID=E004 FNAME=Alice LNAME=Williams AGE=32 SALARY=58000.25 DEPT=Sales HIREDATE=2020-11-05

# 7. Fast indexed searches (=)
TABLE.SELECT mydb.employees WHERE DEPT=Engineering
TABLE.SELECT mydb.employees WHERE FNAME=Jane
TABLE.SELECT mydb.employees WHERE HIREDATE=2020-01-15

# 8. Comparison searches (scans all rows)
TABLE.SELECT mydb.employees WHERE AGE>30
TABLE.SELECT mydb.employees WHERE SALARY>=55000.00
TABLE.SELECT mydb.employees WHERE HIREDATE>2020-01-01

# 9. Date range queries
TABLE.SELECT mydb.employees WHERE HIREDATE>=2020-01-01 AND HIREDATE<=2020-12-31

# 10. Float comparisons
TABLE.SELECT mydb.employees WHERE SALARY>55000.00
TABLE.SELECT mydb.employees WHERE SALARY<=58000.00

# 11. Combined conditions
TABLE.SELECT mydb.employees WHERE AGE>28 AND DEPT=Engineering
TABLE.SELECT mydb.employees WHERE SALARY>=55000.00 OR DEPT=Sales

# 12. Add index to existing column (builds for all 4 rows!)
TABLE.SCHEMA.ALTER mydb.employees ADD INDEX AGE

# 13. View updated table schema
TABLE.SCHEMA.VIEW mydb.employees
# Now AGE shows indexed=true

# 14. Now equality search on AGE works (fast)
TABLE.SELECT mydb.employees WHERE AGE=30

# 15. Update with new types
TABLE.UPDATE mydb.employees WHERE EMPID=E001 SET SALARY=52000.75 AGE=31 HIREDATE=2020-02-01

# 16. Delete rows
TABLE.DELETE mydb.employees WHERE AGE>35

# 17. Add new columns
TABLE.SCHEMA.ALTER mydb.employees ADD COLUMN CITY:string:true
TABLE.SCHEMA.ALTER mydb.employees ADD COLUMN BONUS:float:false
TABLE.SCHEMA.ALTER mydb.employees ADD COLUMN REVIEWDATE:date:false

# 18. Remove index
TABLE.SCHEMA.ALTER mydb.employees DROP INDEX LNAME

# 19. Drop table (requires FORCE parameter)
TABLE.DROP mydb.employees FORCE
```

## Data Model

The module uses the following Redis keys:

```
schema:<namespace>                     - Namespace marker (string "1")
schema:<namespace>.<table>             - Table schema hash (col => type)
idx:meta:<namespace>.<table>           - Index metadata set (indexed column names)
<namespace>.<table>:<id>               - Row hash
table:<namespace>.<table>:id           - Auto-increment counter
rows:<namespace>.<table>               - Set of all row IDs
idx:<namespace>.<table>:<col>:<value>  - Index set (only for indexed columns)
```

## Inspecting Underlying Keys

```bash
# Namespace marker
GET schema:mydb
# Returns: "1"

# Table schema
HGETALL schema:mydb.employees
# Returns: EMPID => string, FNAME => string, ...

# Index metadata
SMEMBERS idx:meta:mydb.employees
# Returns: EMPID, FNAME, LNAME, DEPT, HIREDATE (indexed columns)

# Row IDs
SMEMBERS rows:mydb.employees
# Returns: 1, 2, 3, 4

# Specific row
HGETALL mydb.employees:1
# Returns: EMPID => E001, FNAME => John, ...

# Index keys
KEYS idx:mydb.employees:DEPT:*
# Returns: idx:mydb.employees:DEPT:Engineering, idx:mydb.employees:DEPT:Marketing, ...

SMEMBERS idx:mydb.employees:DEPT:Engineering
# Returns: 1, 3 (row IDs)
```

## Performance Considerations

### Indexed Columns (= operator)
- **Fast** - O(1) lookup via Redis sets
- Use for frequently queried columns
- Adds storage overhead (one set per unique value)

### Non-Indexed Columns (comparison operators)
- **Slow** - O(N) full table scan
- Use for infrequent queries or small tables
- No storage overhead

### Index Building
- `ADD INDEX` scans all existing rows: O(N)
- Do this during low-traffic periods for large tables

### Best Practices
1. **Index selective columns** - High cardinality, frequently queried
2. **Don't index everything** - Indexes consume memory and slow writes
3. **Use comparison operators sparingly** - They scan all rows
4. **Add indexes dynamically** - Use TABLE.ALTER based on query patterns
5. **Drop unused indexes** - Free up memory

## Testing

The module includes a comprehensive test suite with 86 tests covering all functionality:

```bash
# Run all tests (recommended)
make test

# Or run tests manually
cd tests
./run_tests.sh
```

### Test Coverage
- ✅ Namespace management (4 tests)
- ✅ Table creation and schema operations (9 tests) 
- ✅ Data insertion with type validation (9 tests)
- ✅ Data selection and querying (3 tests)
- ✅ Comparison operators (8 tests)
- ✅ Logical operators (2 tests)
- ✅ Table alteration (7 tests)
- ✅ Data updates (5 tests)
- ✅ Data deletion (3 tests)
- ✅ Table dropping (6 tests)
- ✅ Edge cases and error handling (6 tests)
- ✅ Help command (3 tests)
- ✅ Index maintenance (3 tests)
- ✅ Complex scenarios (6 tests)

**All 86 tests pass successfully!**

Test new features manually:
```bash
# Test float
TABLE.SCHEMA.CREATE test.data VALUE:float:true
TABLE.INSERT test.data VALUE=123.45
TABLE.SELECT test.data WHERE VALUE>100.00

# Test date
TABLE.SCHEMA.CREATE test.events DATE:date:true
TABLE.INSERT test.events DATE=2024-01-15
TABLE.SELECT test.events WHERE DATE>=2024-01-01

# Test table schema view
TABLE.SCHEMA.VIEW test.data

# Test index building
TABLE.SCHEMA.CREATE test.users AGE:integer:false
TABLE.INSERT test.users AGE=30
TABLE.INSERT test.users AGE=25
TABLE.SCHEMA.ALTER test.users ADD INDEX AGE
TABLE.SELECT test.users WHERE AGE=30  # Now works!
```

## Error Messages

- `ERR namespace does not exist` - Create namespace first with TABLE.NAMESPACE.CREATE
- `ERR table schema does not exist` - Table doesn't exist
- `ERR table schema already exists` - Table name conflict
- `ERR invalid column or type` - Column doesn't exist or type mismatch
- `ERR search cannot be done on non-indexed column` - Use = only on indexed columns
- `ERR column does not exist` - Column not in table schema (when adding index)
- `ERR format: <col:type> or <col:type:index>` - Invalid CREATE syntax
- `ERR index must be 'true' or 'false'` - Invalid index value

## Limitations

- Maximum 1000 rows per WHERE clause (array limit in filter functions)
- Comparison operators require full table scan
- No compound indexes (index per column only)
- No LIKE/pattern matching
- No JOIN operations
- No transactions
- Date format must be YYYY-MM-DD (no time component)
- Float stored as string (precision limited by string conversion)

## Migration from V1

V1 tables (all columns auto-indexed) work with the unified module:
- Old syntax (without `:index`) defaults to `indexed=true`
- All V1 commands work unchanged
- Can use TABLE.ALTER to drop indexes on V1 tables

## Summary

The unified Redis Table Module provides:
- ✅ **Full CRUD operations** - CREATE, INSERT, SELECT, UPDATE, DELETE, DROP
- ✅ **Explicit index control** - Define which columns are indexed
- ✅ **4 data types** - string, integer, float, date
- ✅ **Comparison operators** - =, >, <, >=, <=
- ✅ **Logical operators** - AND, OR
- ✅ **Dynamic table schema** - Add/remove columns and indexes
- ✅ **Table schema introspection** - TABLE.SCHEMA.VIEW
- ✅ **Auto-index building** - ADD INDEX creates indexes for existing data
- ✅ **Auto-index cleanup** - DELETE/UPDATE maintain indexes automatically
- ✅ **Backward compatible** - V1 syntax still works

Use this module when you need SQL-like tables in Redis with fine-grained control over indexing and support for multiple data types.
