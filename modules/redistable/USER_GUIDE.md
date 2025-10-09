---
title: "Redis Table Module - User Guide"
author: "Raphael Drai"
author-email: "raphael.drai@gmail.com"
date: "October 2025"
geometry: margin=1in
toc: true
toc-depth: 3
numbersections: true
colorlinks: true
---

\newpage

# Introduction

The **Redis Table Module** is a powerful extension that brings SQL-like table functionality to Redis. It enables you to create structured tables with namespaces and table schemas, perform CRUD operations, manage indexes, and query data using familiar SQL-like syntax—all within Redis.

## Key Features

- **SQL-like Tables**: Create tables with defined table schemas and data types
- **Full CRUD Operations**: CREATE, INSERT, SELECT, UPDATE, DELETE, DROP
- **Multiple Data Types**: Support for string, integer, float, and date types
- **Flexible Indexing**: Control which columns are indexed for optimal performance
- **Comparison Operators**: Query data using =, >, <, >=, <= operators
- **Logical Operators**: Combine conditions with AND/OR
- **Dynamic Schema**: Add or remove columns and indexes on the fly
- **Auto-increment IDs**: Automatic row ID generation
- **Schema Introspection**: View table structure and index information

## Use Cases

- **Structured Data Storage**: Store relational data in Redis
- **Fast Lookups**: Leverage Redis speed with indexed queries
- **Caching Layer**: Cache database tables in Redis
- **Session Management**: Store user sessions with structured data
- **Analytics**: Store and query time-series or event data

\newpage

# Getting Started

## Prerequisites

- Redis server (version 6.0 or higher recommended)
- GCC compiler for building the module
- Basic familiarity with Redis CLI

## Installation

### Step 1: Build the Module

The module includes a fully functional Makefile with comprehensive targets:

```bash
cd /home/ubuntu/Projects/REDIS/redis/modules/redistable

# Build the module
make

# Clean build artifacts
make clean

# Run comprehensive test suite (93 tests)
make test

# Build with debug information
make debug
```

**Manual build (if needed)**:
```bash
cd /home/ubuntu/Projects/REDIS/redis/modules/redistable
gcc -Wall -Werror -g -O0 -fPIC -I../../src -c redis_table.c
gcc -shared -o redis_table.so redis_table.o
```

### Step 2: Start Redis with the Module

**Basic (default configuration):**
```bash
cd /home/ubuntu/Projects/REDIS/redis
./src/redis-server --loadmodule modules/redistable/redis_table.so
```

**With custom scan limit:**
```bash
# For analytics workloads (500K row scan limit)
./src/redis-server --loadmodule modules/redistable/redis_table.so max_scan_limit 500000

# For large-scale production (1M row scan limit)
./src/redis-server --loadmodule modules/redistable/redis_table.so max_scan_limit 1000000
```

**Configuration Parameters:**
- `max_scan_limit` - Maximum rows to scan per query (default: 100,000)
  - **Min**: 1,000
  - **Max**: 10,000,000
  - **Default**: 100,000

### Step 3: Connect to Redis

In another terminal:

```bash
cd /home/ubuntu/Projects/REDIS/redis
./src/redis-cli
```

### Step 4: Verify Installation

```bash
TABLE.HELP
```

If you see the help text, the module is successfully loaded!

### Step 5: Quick Test

Run the comprehensive test suite to verify everything works:

```bash
# From the redistable directory
cd /home/ubuntu/Projects/REDIS/redis/modules/redistable
make test
```

**Expected output**: All 86 tests should pass successfully, covering:
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

\newpage

# Data Types

The Redis Table Module supports four data types, each with specific validation rules and comparison behavior.

## String Type

**Format**: Any text value

**Examples**:
```
"Hello World"
"user@example.com"
"Product-123"
```

**Validation**: None (accepts any text)

**Comparison**: Lexical (alphabetical order)

**Use Cases**: Names, emails, IDs, descriptions

---

## Integer Type

**Format**: Whole numbers (positive or negative)

**Examples**:
```
123
-45
0
999999
```

**Validation**: Must contain only digits with optional +/- prefix

**Comparison**: Numeric

**Use Cases**: Ages, quantities, counters, IDs

---

## Float Type

**Format**: Decimal numbers

**Examples**:
```
123.45
-99.99
0.001
1000.0
```

**Validation**: Digits with optional decimal point

**Comparison**: Numeric

**Use Cases**: Prices, measurements, percentages, ratings

---

## Date Type

**Format**: YYYY-MM-DD (ISO 8601 date format)

**Examples**:
```
2024-01-15
2023-12-31
2025-06-01
```

**Validation**: 
- Exactly 10 characters
- Hyphens at positions 4 and 7
- Format: YYYY-MM-DD

**Comparison**: Lexical (works correctly for ISO dates)

**Use Cases**: Birthdates, hire dates, event dates, timestamps

\newpage

# Command Reference

## Schema Management

### TABLE.NAMESPACE.CREATE

Create a namespace for organizing tables.

**Syntax**:
```
TABLE.NAMESPACE.CREATE <namespace_name>
```

**Parameters**:
- `namespace_name`: Name of the namespace to create

**Returns**: `OK` on success

**Example**:
```bash
TABLE.NAMESPACE.CREATE mydb
```

**Output**:
```
OK
```

**Notes**:
- A namespace must be created before creating tables
- Namespace names are case-sensitive
- Duplicate namespaces will return an error

---

### TABLE.NAMESPACE.VIEW

Display all tables across all namespaces or filter by a specific namespace.

**Syntax**:
```
TABLE.NAMESPACE.VIEW [<namespace>]
```

**Parameters**:
- `namespace`: Optional - Filter results to show only tables in this namespace

**Returns**: Array of `namespace:table` strings, sorted alphabetically

**Examples**:

**View all tables in all namespaces**:
```bash
TABLE.NAMESPACE.VIEW
```

**Output**:
```
1) "mydb:employees"
2) "mydb:users"
3) "shop:products"
```

**View tables in a specific namespace**:
```bash
TABLE.NAMESPACE.VIEW mydb
```

**Output**:
```
1) "mydb:employees"
2) "mydb:users"
```

**Notes**:
- Returns empty array if no tables exist
- Results are sorted by namespace, then by table name
- Only shows actual tables, not namespace markers

---

### TABLE.SCHEMA.VIEW

Display the structure of a table including columns, data types, and index status.

**Syntax**:
```
TABLE.SCHEMA.VIEW <namespace.table>
```

**Parameters**:
- `namespace.table`: Fully qualified table name

**Returns**: Array of [column_name, data_type, is_indexed]

**Example**:
```bash
TABLE.SCHEMA.VIEW mydb.employees
```

**Output**:
```
1) 1) "EMPID"
   2) "string"
   3) "true"
2) 1) "NAME"
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

**Notes**:
- Third element shows if column is indexed (`true`/`false`)
- Indexed columns support fast equality (=) searches
- Non-indexed columns require full table scans for queries

\newpage

## Table Operations

### TABLE.SCHEMA.CREATE

Create a new table with defined columns and optional index control.

**Syntax**:
```
TABLE.SCHEMA.CREATE <namespace.table> <col:type[:index]> [<col:type[:index]> ...]
```

**Parameters**:
- `namespace.table`: Fully qualified table name
- `col`: Column name
- `type`: Data type (string, integer, float, date)
- `index`: Optional - `true` or `false` (defaults to `true`)

**Returns**: `OK` on success

**Examples**:

**Basic table (all columns indexed by default)**:
```bash
TABLE.SCHEMA.CREATE mydb.users NAME:string EMAIL:string AGE:integer
```

**Explicit index control**:
```bash
TABLE.SCHEMA.CREATE mydb.products 
  ID:string:true 
  NAME:string:true 
  PRICE:float:false 
  STOCK:integer:false
```

**All data types**:
```bash
TABLE.SCHEMA.CREATE mydb.employees 
  EMPID:string:true 
  NAME:string:true 
  AGE:integer:false 
  SALARY:float:false 
  HIREDATE:date:true
```

**Notes**:
- Namespace must exist before creating table
- Column names are case-sensitive
- Indexed columns enable fast equality searches
- Non-indexed columns save memory but require table scans

---

### TABLE.SCHEMA.ALTER

Modify table structure by adding columns or managing indexes.

**Syntax**:
```
TABLE.SCHEMA.ALTER <namespace.table> ADD COLUMN <col:type[:index]>
TABLE.SCHEMA.ALTER <namespace.table> ADD INDEX <column>
TABLE.SCHEMA.ALTER <namespace.table> DROP INDEX <column>
```

**Parameters**:
- `namespace.table`: Fully qualified table name
- `col`: Column name
- `type`: Data type
- `index`: Optional - `true` or `false`
- `column`: Existing column name

**Returns**: `OK` on success

**Examples**:

**Add new column**:
```bash
TABLE.SCHEMA.ALTER mydb.users ADD COLUMN CITY:string:true
TABLE.SCHEMA.ALTER mydb.users ADD COLUMN BALANCE:float:false
```

**Add index to existing column** (builds indexes for all existing rows):
```bash
TABLE.SCHEMA.ALTER mydb.users ADD INDEX AGE
```

**Remove index** (keeps column, removes index keys):
```bash
TABLE.SCHEMA.ALTER mydb.users DROP INDEX CITY
```

**Notes**:
- `ADD INDEX` automatically creates indexes for all existing rows
- This can be slow for large tables
- `DROP INDEX` removes all index keys but keeps the column
- Adding indexes enables equality (=) searches on that column

---

### TABLE.DROP

Delete a table and all its data, indexes, and metadata.

**Syntax**:
```
TABLE.DROP <namespace.table> FORCE
```

**Parameters**:
- `namespace.table`: Fully qualified table name
- `FORCE`: Required parameter to confirm the irreversible deletion

**Returns**: `OK` on success

**Examples**:
```bash
# Without FORCE - will fail with warning
TABLE.DROP mydb.users
# Returns: (error) ERR This operation is irreversible, use FORCE parameter to remove the table

# With FORCE - will succeed
TABLE.DROP mydb.users FORCE
# Returns: OK
```

**Notes**:
- This operation is irreversible
- The `FORCE` parameter is required to prevent accidental deletions
- All rows, indexes, and table schema information are deleted
- The namespace itself is not deleted (only the table)

\newpage

## Data Manipulation

### TABLE.INSERT

Insert a new row with auto-generated ID.

**Syntax**:
```
TABLE.INSERT <namespace.table> <col>=<value> [<col>=<value> ...]
```

**Parameters**:
- `namespace.table`: Fully qualified table name
- `col=value`: Column-value pairs

**Returns**: Row ID (integer)

**Examples**:

**Basic insert**:
```bash
TABLE.INSERT mydb.users NAME=John EMAIL=john@example.com AGE=30
```

**Output**:
```
(integer) 1
```

**All data types**:
```bash
TABLE.INSERT mydb.employees 
  EMPID=E001 
  NAME=Alice 
  AGE=28 
  SALARY=55000.50 
  HIREDATE=2023-01-15
```

**Output**:
```
(integer) 1
```

**Notes**:
- Row IDs are auto-incremented starting from 1
- All columns must match the defined data types
- Invalid data types will return an error
- Indexes are automatically created for indexed columns

---

### TABLE.SELECT

Query rows with optional filtering using WHERE clause.

**Syntax**:
```
TABLE.SELECT <namespace.table> [WHERE <condition> [AND|OR <condition> ...]]
```

**Condition Format**:
```
<column><operator><value>
```

**Operators**:
- `=` - Equals (requires indexed column)
- `>` - Greater than
- `<` - Less than
- `>=` - Greater than or equal
- `<=` - Less than or equal

**Logical Operators**: `AND`, `OR`

**Returns**: Array of matching rows

**Examples**:

**Select all rows**:
```bash
TABLE.SELECT mydb.users
```

**Equality search (indexed column)**:
```bash
TABLE.SELECT mydb.users WHERE NAME=John
```

**Comparison operators**:
```bash
TABLE.SELECT mydb.users WHERE AGE>25
TABLE.SELECT mydb.users WHERE AGE>=30
TABLE.SELECT mydb.users WHERE AGE<40
```

**Float comparisons**:
```bash
TABLE.SELECT mydb.employees WHERE SALARY>50000.00
TABLE.SELECT mydb.employees WHERE SALARY<=60000.00
```

**Date comparisons**:
```bash
TABLE.SELECT mydb.employees WHERE HIREDATE>2023-01-01
TABLE.SELECT mydb.employees WHERE HIREDATE>=2023-01-01
```

**Combined conditions (AND)**:
```bash
TABLE.SELECT mydb.users WHERE AGE>25 AND NAME=John
TABLE.SELECT mydb.employees WHERE SALARY>=50000 AND HIREDATE>2023-01-01
```

**Combined conditions (OR)**:
```bash
TABLE.SELECT mydb.users WHERE NAME=John OR NAME=Jane
TABLE.SELECT mydb.employees WHERE SALARY>60000 OR HIREDATE<2020-01-01
```

**Date range query**:
```bash
TABLE.SELECT mydb.employees 
  WHERE HIREDATE>=2023-01-01 AND HIREDATE<=2023-12-31
```

**Notes**:
- Equality (=) requires indexed columns for fast lookup
- Comparison operators (>, <, >=, <=) work on any column but scan all rows
- Multiple conditions can be combined with AND/OR
- String comparisons are lexical (alphabetical)
- Numeric comparisons work correctly for integers and floats
- Date comparisons work correctly with YYYY-MM-DD format

---

### TABLE.UPDATE

Update rows matching WHERE clause.

**Syntax**:
```
TABLE.UPDATE <namespace.table> WHERE <condition> [AND|OR <condition> ...] 
  SET <col>=<value> [<col>=<value> ...]
```

**Parameters**:
- `namespace.table`: Fully qualified table name
- `WHERE`: Condition clause (same as SELECT)
- `SET`: Column-value pairs to update

**Returns**: Number of rows updated (integer)

**Examples**:

**Update single field**:
```bash
TABLE.UPDATE mydb.users WHERE NAME=John SET AGE=31
```

**Output**:
```
(integer) 1
```

**Update multiple fields**:
```bash
TABLE.UPDATE mydb.employees WHERE EMPID=E001 
  SET SALARY=57000.00 HIREDATE=2023-02-01
```

**Update with comparison**:
```bash
TABLE.UPDATE mydb.users WHERE AGE>30 SET STATUS=senior
```

**Update with combined conditions**:
```bash
TABLE.UPDATE mydb.employees 
  WHERE SALARY<50000 AND HIREDATE<2020-01-01 
  SET SALARY=50000.00
```

**Notes**:
- Indexes are automatically updated
- WHERE clause is required
- Updated values must match column data types
- Returns count of affected rows

---

### TABLE.DELETE

Delete rows matching WHERE clause.

**Syntax**:
```
TABLE.DELETE <namespace.table> [WHERE <condition> [AND|OR <condition> ...]]
```

**Parameters**:
- `namespace.table`: Fully qualified table name
- `WHERE`: Optional condition clause

**Returns**: Number of rows deleted (integer)

**Examples**:

**Delete specific rows**:
```bash
TABLE.DELETE mydb.users WHERE NAME=John
```

**Output**:
```
(integer) 1
```

**Delete with comparison**:
```bash
TABLE.DELETE mydb.users WHERE AGE>65
```

**Delete with combined conditions**:
```bash
TABLE.DELETE mydb.employees 
  WHERE SALARY<30000 OR HIREDATE<2015-01-01
```

**Notes**:
- Indexes are automatically cleaned up
- WHERE clause is optional (omitting it deletes all rows)
- Returns count of deleted rows
- Deleted row IDs are not reused

\newpage

# Practical Examples

## Example 1: User Management System

### Setup

```bash
# Create namespace
TABLE.NAMESPACE.CREATE userdb

# Create users table
TABLE.SCHEMA.CREATE userdb.users 
  USERID:string:true 
  USERNAME:string:true 
  EMAIL:string:true 
  AGE:integer:false 
  JOINDATE:date:true
```

### Insert Users

```bash
TABLE.INSERT userdb.users 
  USERID=U001 USERNAME=alice EMAIL=alice@example.com 
  AGE=28 JOINDATE=2023-01-15

TABLE.INSERT userdb.users 
  USERID=U002 USERNAME=bob EMAIL=bob@example.com 
  AGE=35 JOINDATE=2022-06-20

TABLE.INSERT userdb.users 
  USERID=U003 USERNAME=charlie EMAIL=charlie@example.com 
  AGE=42 JOINDATE=2021-11-10
```

### Query Users

```bash
# Find user by username
TABLE.SELECT userdb.users WHERE USERNAME=alice

# Find users who joined in 2023
TABLE.SELECT userdb.users WHERE JOINDATE>=2023-01-01

# Find users over 30
TABLE.SELECT userdb.users WHERE AGE>30

# Find young users who joined recently
TABLE.SELECT userdb.users 
  WHERE AGE<30 AND JOINDATE>=2023-01-01
```

### Update User

```bash
# Update user's age
TABLE.UPDATE userdb.users WHERE USERID=U001 SET AGE=29
```

### Delete User

```bash
# Remove inactive old accounts
TABLE.DELETE userdb.users WHERE JOINDATE<2020-01-01
```

---

## Example 2: E-Commerce Product Catalog

### Setup

```bash
# Create namespace
TABLE.NAMESPACE.CREATE shop

# Create products table
TABLE.SCHEMA.CREATE shop.products 
  SKU:string:true 
  NAME:string:true 
  CATEGORY:string:true 
  PRICE:float:false 
  STOCK:integer:false 
  ADDED:date:true
```

### Add Products

```bash
TABLE.INSERT shop.products 
  SKU=PROD001 NAME=Laptop CATEGORY=Electronics 
  PRICE=999.99 STOCK=50 ADDED=2024-01-10

TABLE.INSERT shop.products 
  SKU=PROD002 NAME=Mouse CATEGORY=Electronics 
  PRICE=29.99 STOCK=200 ADDED=2024-01-15

TABLE.INSERT shop.products 
  SKU=PROD003 NAME=Desk CATEGORY=Furniture 
  PRICE=299.99 STOCK=30 ADDED=2024-02-01

TABLE.INSERT shop.products 
  SKU=PROD004 NAME=Chair CATEGORY=Furniture 
  PRICE=199.99 STOCK=45 ADDED=2024-02-05
```

### Query Products

```bash
# Find all electronics
TABLE.SELECT shop.products WHERE CATEGORY=Electronics

# Find products under $100
TABLE.SELECT shop.products WHERE PRICE<100.00

# Find low stock items
TABLE.SELECT shop.products WHERE STOCK<50

# Find expensive furniture
TABLE.SELECT shop.products 
  WHERE CATEGORY=Furniture AND PRICE>250.00

# Find recently added products
TABLE.SELECT shop.products WHERE ADDED>=2024-02-01
```

### Update Inventory

```bash
# Reduce stock after sale
TABLE.UPDATE shop.products WHERE SKU=PROD001 SET STOCK=45

# Price adjustment
TABLE.UPDATE shop.products WHERE CATEGORY=Electronics 
  SET PRICE=899.99
```

### Remove Products

```bash
# Remove out-of-stock items
TABLE.DELETE shop.products WHERE STOCK=0
```

---

## Example 3: Employee Management

### Setup

```bash
# Create namespace
TABLE.NAMESPACE.CREATE company

# Create employees table
TABLE.SCHEMA.CREATE company.employees 
  EMPID:string:true 
  FNAME:string:true 
  LNAME:string:true 
  DEPT:string:true 
  SALARY:float:false 
  AGE:integer:false 
  HIREDATE:date:true
```

### Add Employees

```bash
TABLE.INSERT company.employees 
  EMPID=E001 FNAME=John LNAME=Doe DEPT=Engineering 
  SALARY=75000.50 AGE=32 HIREDATE=2020-03-15

TABLE.INSERT company.employees 
  EMPID=E002 FNAME=Jane LNAME=Smith DEPT=Marketing 
  SALARY=68000.00 AGE=29 HIREDATE=2021-07-01

TABLE.INSERT company.employees 
  EMPID=E003 FNAME=Bob LNAME=Johnson DEPT=Engineering 
  SALARY=82000.75 AGE=38 HIREDATE=2019-01-20

TABLE.INSERT company.employees 
  EMPID=E004 FNAME=Alice LNAME=Williams DEPT=Sales 
  SALARY=71000.25 AGE=35 HIREDATE=2020-11-10
```

### Query Employees

```bash
# Find all engineers
TABLE.SELECT company.employees WHERE DEPT=Engineering

# Find high earners
TABLE.SELECT company.employees WHERE SALARY>70000.00

# Find employees hired in 2020
TABLE.SELECT company.employees 
  WHERE HIREDATE>=2020-01-01 AND HIREDATE<=2020-12-31

# Find senior engineers
TABLE.SELECT company.employees 
  WHERE DEPT=Engineering AND AGE>35

# Find employees by name
TABLE.SELECT company.employees WHERE LNAME=Smith
```

### Salary Adjustments

```bash
# Give raise to engineering department
TABLE.UPDATE company.employees WHERE DEPT=Engineering 
  SET SALARY=80000.00

# Adjust specific employee
TABLE.UPDATE company.employees WHERE EMPID=E002 
  SET SALARY=72000.00
```

### Dynamic Index Management

```bash
# Initially, AGE is not indexed
# Add index for better performance on age queries
TABLE.SCHEMA.ALTER company.employees ADD INDEX AGE

# Now equality search on AGE is fast
TABLE.SELECT company.employees WHERE AGE=32

# View updated table schema
TABLE.SCHEMA.VIEW company.employees
```

---

## Example 4: Event Tracking System

### Setup

```bash
# Create namespace
TABLE.NAMESPACE.CREATE events

# Create events table
TABLE.SCHEMA.CREATE events.log 
  EVENTID:string:true 
  TYPE:string:true 
  USER:string:true 
  TIMESTAMP:date:true 
  VALUE:integer:false
```

### Log Events

```bash
TABLE.INSERT events.log 
  EVENTID=EVT001 TYPE=login USER=alice 
  TIMESTAMP=2024-03-01 VALUE=1

TABLE.INSERT events.log 
  EVENTID=EVT002 TYPE=purchase USER=bob 
  TIMESTAMP=2024-03-02 VALUE=150

TABLE.INSERT events.log 
  EVENTID=EVT003 TYPE=login USER=charlie 
  TIMESTAMP=2024-03-02 VALUE=1

TABLE.INSERT events.log 
  EVENTID=EVT004 TYPE=purchase USER=alice 
  TIMESTAMP=2024-03-03 VALUE=200
```

### Query Events

```bash
# Find all login events
TABLE.SELECT events.log WHERE TYPE=login

# Find events by user
TABLE.SELECT events.log WHERE USER=alice

# Find events in date range
TABLE.SELECT events.log 
  WHERE TIMESTAMP>=2024-03-01 AND TIMESTAMP<=2024-03-02

# Find high-value purchases
TABLE.SELECT events.log 
  WHERE TYPE=purchase AND VALUE>100

# Find user's purchases
TABLE.SELECT events.log 
  WHERE USER=alice AND TYPE=purchase
```

\newpage

# Performance Guide

## Understanding Indexes

### Indexed Columns

**Performance**: O(1) - Constant time lookup

**How it works**:
- Creates a Redis set for each unique value
- Fast equality (=) searches
- Automatic index maintenance on INSERT/UPDATE/DELETE

**When to use**:
- Frequently queried columns
- High selectivity (many unique values)
- Equality searches

**Storage overhead**:
- One Redis set per unique value
- Example: 1000 users with 10 departments = 10 index sets

**Example**:
```bash
# Fast - uses index
TABLE.SELECT mydb.users WHERE USERID=U001
TABLE.SELECT mydb.users WHERE DEPT=Engineering
```

---

### Non-Indexed Columns

**Performance**: O(N) - Linear time (full table scan)

**How it works**:
- Scans all rows in the table
- Compares each value against condition
- No index overhead

**When to use**:
- Infrequently queried columns
- Small tables (< 1000 rows)
- Comparison operators (>, <, >=, <=)

**Storage overhead**: None

**Example**:
```bash
# Slow - scans all rows
TABLE.SELECT mydb.users WHERE AGE>30
TABLE.SELECT mydb.users WHERE SALARY>50000
```

---

## Indexing Best Practices

### 1. Index Selective Columns

**Good candidates**:
- User IDs
- Email addresses
- Product SKUs
- Department names
- Status codes

**Poor candidates**:
- Boolean flags (only 2 values)
- Age (limited range)
- Gender (low cardinality)

---

### 2. Don't Over-Index

**Problem**: Every index:
- Consumes memory
- Slows down INSERT operations
- Slows down UPDATE operations
- Requires maintenance

**Solution**: Only index columns you frequently query with `=`

---

### 3. Use Comparison Operators Wisely

**Comparison operators** (>, <, >=, <=) always scan all rows, even on indexed columns.

**Example**:
```bash
# Both scan all rows (same performance)
TABLE.SELECT mydb.users WHERE AGE>30        # AGE indexed
TABLE.SELECT mydb.users WHERE SALARY>50000  # SALARY not indexed
```

**Best practice**: Use comparison operators on small tables or infrequent queries.

---

### 4. Dynamic Index Management

Start with minimal indexes and add them based on query patterns.

**Example workflow**:
```bash
# Create table with minimal indexes
TABLE.SCHEMA.CREATE mydb.users 
  USERID:string:true 
  NAME:string:false 
  AGE:integer:false 
  DEPT:string:false

# Monitor query patterns
# If you frequently query by DEPT, add index
TABLE.SCHEMA.ALTER mydb.users ADD INDEX DEPT

# If DEPT queries become rare, remove index
TABLE.SCHEMA.ALTER mydb.users DROP INDEX DEPT
```

---

### 5. Index Building Considerations

**ADD INDEX** scans all existing rows to build indexes.

**Performance impact**:
- Small table (< 1000 rows): Instant
- Medium table (1000-10000 rows): Seconds
- Large table (> 10000 rows): Minutes

**Best practice**: Add indexes during low-traffic periods.

---

## Query Optimization

### Fast Queries (Indexed)

```bash
# O(1) - Instant lookup
TABLE.SELECT mydb.users WHERE USERID=U001
TABLE.SELECT mydb.products WHERE SKU=PROD123
TABLE.SELECT mydb.employees WHERE DEPT=Engineering
```

---

### Slow Queries (Full Scan)

```bash
# O(N) - Scans all rows
TABLE.SELECT mydb.users WHERE AGE>30
TABLE.SELECT mydb.products WHERE PRICE<100
TABLE.SELECT mydb.employees WHERE SALARY>50000
```

---

### Mixed Queries

```bash
# Fast indexed lookup + filter
TABLE.SELECT mydb.employees 
  WHERE DEPT=Engineering AND SALARY>70000

# Explanation:
# 1. Fast lookup: DEPT=Engineering (indexed)
# 2. Filter results: SALARY>70000 (comparison)
```

---

## Memory Optimization

### Estimate Index Size

**Formula**:
```
Index memory = (number of unique values) × (average set size)
```

**Example**:
- 10,000 employees
- 20 departments
- Each department has ~500 employees

**Index size**: 20 sets × 500 entries = 10,000 entries

---

### Monitor Memory Usage

```bash
# Check Redis memory
redis-cli INFO memory

# Check specific table keys
redis-cli KEYS "idx:mydb.users:*"

# Count index sets
redis-cli KEYS "idx:mydb.users:DEPT:*" | wc -l
```

---

### Reduce Memory Usage

**Strategies**:
1. Drop unused indexes
2. Use shorter column names
3. Use shorter values where possible
4. Archive old data

**Example**:
```bash
# Remove index from rarely queried column
TABLE.SCHEMA.ALTER mydb.users DROP INDEX MIDDLENAME
```

\newpage

# Troubleshooting

## Common Errors

### Error: "ERR namespace does not exist"

**Cause**: Attempting to create a table before creating the namespace.

**Solution**:
```bash
# Create namespace first
TABLE.NAMESPACE.CREATE mydb

# Then create table
TABLE.SCHEMA.CREATE mydb.users NAME:string AGE:integer
```

---

### Error: "ERR table schema does not exist"

**Cause**: Referencing a table that doesn't exist.

**Solution**:
```bash
# Check if table exists
TABLE.SCHEMA.VIEW mydb.users

# Create table if needed
TABLE.SCHEMA.CREATE mydb.users NAME:string AGE:integer
```

---

### Error: "ERR table schema already exists"

**Cause**: Attempting to create a table that already exists.

**Solution**:
```bash
# Drop existing table first
TABLE.DROP mydb.users FORCE

# Then create new table
TABLE.SCHEMA.CREATE mydb.users NAME:string AGE:integer EMAIL:string
```

---

### Error: "ERR search cannot be done on non-indexed column"

**Cause**: Using equality (=) operator on a non-indexed column.

**Solution**:
```bash
# Option 1: Add index to the column
TABLE.SCHEMA.ALTER mydb.users ADD INDEX AGE

# Option 2: Use comparison operator instead
TABLE.SELECT mydb.users WHERE AGE>=30 AND AGE<=30
```

---

### Error: "ERR invalid column or type"

**Cause**: Invalid data type for a column.

**Solution**:
```bash
# Check table schema
TABLE.SCHEMA.VIEW mydb.users

# Ensure values match column types
# Wrong: TABLE.INSERT mydb.users AGE=abc
# Right: TABLE.INSERT mydb.users AGE=30
```

---

### Error: "ERR column does not exist"

**Cause**: Referencing a column that's not in the table schema.

**Solution**:
```bash
# View table schema
TABLE.SCHEMA.VIEW mydb.users

# Add missing column
TABLE.SCHEMA.ALTER mydb.users ADD COLUMN EMAIL:string:true
```

---

### Error: "ERR format: <col:type> or <col:type:index>"

**Cause**: Invalid syntax in TABLE.CREATE.

**Solution**:
```bash
# Wrong: TABLE.SCHEMA.CREATE mydb.users NAME string
# Right: TABLE.SCHEMA.CREATE mydb.users NAME:string

# Wrong: TABLE.SCHEMA.CREATE mydb.users NAME:string:yes
# Right: TABLE.SCHEMA.CREATE mydb.users NAME:string:true
```

---

## Debugging Tips

### Inspect Underlying Keys

```bash
# View all keys for a table
redis-cli KEYS "*mydb.users*"

# View table schema
redis-cli HGETALL schema:mydb.users

# View index metadata
redis-cli SMEMBERS idx:meta:mydb.users

# View row IDs
redis-cli SMEMBERS rows:mydb.users

# View specific row
redis-cli HGETALL mydb.users:1

# View index for a value
redis-cli SMEMBERS idx:mydb.users:DEPT:Engineering
```

---

### Verify Index Integrity

```bash
# List all index keys for a column
redis-cli KEYS "idx:mydb.users:DEPT:*"

# Check index contents
redis-cli SMEMBERS idx:mydb.users:DEPT:Engineering

# Verify row exists
redis-cli HGETALL mydb.users:1
```

---

### Test Queries Manually

```bash
# Enable verbose mode
redis-cli --verbose

# Test query
redis-cli TABLE.SELECT mydb.users WHERE DEPT=Engineering

# Check result
```

---

## Performance Issues

### Slow Queries

**Problem**: Queries taking too long.

**Diagnosis**:
```bash
# Check table size
redis-cli SCARD rows:mydb.users

# Check if column is indexed
redis-cli SMEMBERS idx:meta:mydb.users
```

**Solutions**:
1. Add index to frequently queried columns
2. Use indexed columns in WHERE clause
3. Reduce table size (archive old data)
4. Optimize query conditions

---

### High Memory Usage

**Problem**: Redis using too much memory.

**Diagnosis**:
```bash
# Check memory
redis-cli INFO memory

# Count index keys
redis-cli KEYS "idx:*" | wc -l

# Check specific table
redis-cli KEYS "idx:mydb.users:*" | wc -l
```

**Solutions**:
1. Drop unused indexes
2. Archive old data
3. Use shorter column names
4. Reduce number of indexed columns

---

### Slow INSERT/UPDATE

**Problem**: Write operations are slow.

**Cause**: Too many indexed columns.

**Solution**:
```bash
# Remove unnecessary indexes
TABLE.SCHEMA.ALTER mydb.users DROP INDEX MIDDLENAME
TABLE.SCHEMA.ALTER mydb.users DROP INDEX NICKNAME
```

\newpage

# Testing Framework

## Comprehensive Test Suite

The Redis Table Module includes a robust testing framework with **86 comprehensive tests** covering all functionality:

### Test Categories

| Category | Tests | Coverage |
|----------|-------|----------|
| Namespace Management | 4 | CREATE, VIEW, validation |
| Table Creation & Schema | 9 | CREATE, duplicate handling, validation |
| Data Insertion | 9 | All data types, validation, error handling |
| Data Selection | 3 | Basic queries, indexed/non-indexed columns |
| Comparison Operators | 8 | >, <, >=, <= for all data types |
| Logical Operators | 2 | AND, OR combinations |
| Table Alteration | 7 | ADD COLUMN, ADD/DROP INDEX |
| Data Updates | 5 | WHERE clauses, type validation |
| Data Deletion | 3 | WHERE clauses, index cleanup |
| Table Dropping | 6 | FORCE parameter, validation |
| Edge Cases | 6 | Error conditions, boundary cases |
| Help Command | 3 | Documentation completeness |
| Index Maintenance | 3 | Automatic index updates |
| Complex Scenarios | 6 | Real-world usage patterns |

### Running Tests

**Full test suite**:
```bash
cd /home/ubuntu/Projects/REDIS/redis/modules/redistable
make test
```

**Manual test execution**:
```bash
cd tests
./run_tests.sh
```

### Test Output

**Successful run**:
```
========================================
Redis Table Module - Test Runner
========================================

Starting Redis server with table module...
Redis server started successfully

Cleaning database...
Running test suite...

=== TEST SUITE 1: Namespace Management ===
Test 1: Create namespace
✓ PASS: Namespace creation should return OK
...

========================================
Test Summary
========================================
Passed: 86
Failed: 0
Total:  86
========================================
All tests completed successfully!
```

### Test Architecture

**Test Components**:
- `run_tests.sh`: Main test runner (starts/stops Redis)
- `test_redis_table.sh`: Comprehensive test suite
- `TESTING.md`: Detailed testing documentation

**Test Process**:
1. Builds module if needed
2. Starts Redis server with module loaded
3. Runs all 86 tests sequentially
4. Cleans up and stops Redis server
5. Reports pass/fail summary

### Debugging Failed Tests

**Individual test debugging**:
```bash
# Start Redis manually
cd /home/ubuntu/Projects/REDIS/redis
./src/redis-server --loadmodule modules/redistable/redis_table.so

# Run specific commands
./src/redis-cli TABLE.NAMESPACE.CREATE testdb
./src/redis-cli TABLE.SCHEMA.CREATE testdb.users NAME:string AGE:integer
```

**Check test logs**:
```bash
# View test output in detail
cd /home/ubuntu/Projects/REDIS/redis/modules/redistable
make test 2>&1 | tee test_output.log
```

\newpage

# Advanced Topics

## Data Model Internals

The Redis Table Module uses the following key patterns:

### Namespace Keys

```
schema:<namespace>
```
**Type**: String  
**Value**: "1"  
**Purpose**: Marks namespace existence

**Example**:
```bash
GET schema:mydb
# Returns: "1"
```

---

### Table Schema Keys

```
schema:<namespace>.<table>
```
**Type**: Hash  
**Fields**: column => type  
**Purpose**: Stores table structure

**Example**:
```bash
HGETALL schema:mydb.users
# Returns:
# NAME => string
# AGE => integer
# EMAIL => string
```

---

### Index Metadata Keys

```
idx:meta:<namespace>.<table>
```
**Type**: Set  
**Members**: Indexed column names  
**Purpose**: Tracks which columns are indexed

**Example**:
```bash
SMEMBERS idx:meta:mydb.users
# Returns: NAME, EMAIL (indexed columns)
```

---

### Row Keys

```
<namespace>.<table>:<id>
```
**Type**: Hash  
**Fields**: column => value  
**Purpose**: Stores row data

**Example**:
```bash
HGETALL mydb.users:1
# Returns:
# NAME => John
# AGE => 30
# EMAIL => john@example.com
```

---

### Row ID Counter

```
table:<namespace>.<table>:id
```
**Type**: String (integer)  
**Purpose**: Auto-increment counter for row IDs

**Example**:
```bash
GET table:mydb.users:id
# Returns: "5" (next ID will be 6)
```

---

### Row ID Set

```
rows:<namespace>.<table>
```
**Type**: Set  
**Members**: All row IDs  
**Purpose**: Track all rows in table

**Example**:
```bash
SMEMBERS rows:mydb.users
# Returns: 1, 2, 3, 4, 5
```

---

### Index Keys

```
idx:<namespace>.<table>:<column>:<value>
```
**Type**: Set  
**Members**: Row IDs with this value  
**Purpose**: Fast equality lookups

**Example**:
```bash
SMEMBERS idx:mydb.users:DEPT:Engineering
# Returns: 1, 3, 5 (row IDs of engineers)
```

---

## Migration Guide

### From V1 to V2

The unified module is backward compatible with V1 syntax.

**V1 Syntax** (all columns indexed):
```bash
TABLE.CREATE mydb.users NAME:string AGE:integer
```

**V2 Equivalent**:
```bash
TABLE.CREATE mydb.users NAME:string:true AGE:integer:true
```

**Migration steps**:
1. V1 tables work unchanged
2. Use `TABLE.ALTER DROP INDEX` to optimize
3. New tables can use explicit index control

---

## Limitations

### Performance Considerations

1. **Configurable scan limit per query operation**
   - Default: 100,000 rows (configurable from 1,000 to 10,000,000)
   - Prevents Redis from blocking on huge datasets during WHERE clause evaluation
   - This is a scan limit, not a result limit
   - Error: "ERR query scan limit exceeded (max 100000 rows)"
   - See Configuration section for tuning guidance

2. **Non-blocking schema operations**
   - `TABLE.NAMESPACE.VIEW` and `TABLE.SCHEMA.ALTER DROP INDEX` use SCAN command
   - Cursor-based iteration prevents Redis blocking
   - Safe for production use with any number of tables or indexes

### Concurrency Considerations

⚠️ **Known Limitation (will be fixed in v2.2):**

**Race Condition: DROP INDEX During Active Queries**

**Problem**: The current implementation removes index metadata before deleting all index keys, creating a race window.

**Example Scenario**:
```bash
# Terminal 1: Start query
TABLE.SELECT users WHERE age=30  
# Checks: is age indexed? → YES
# Fetches from: idx:users:age:30

# Terminal 2: During query execution
TABLE.SCHEMA.ALTER users DROP INDEX age
# Immediately: SREM idx:meta:users age  (metadata removed)
# Then: SCAN and DEL idx:users:age:*    (keys deleted incrementally)

# Terminal 1: Query result
# If index fetch happens after metadata removed → Empty result ❌
# Should either: use index correctly OR fall back to full scan
```

**Impact**:
- **Incorrect Results**: Queries may return empty set instead of actual data
- **No Error**: Silent failure (query succeeds but returns wrong data)
- **Race Window**: Milliseconds to seconds depending on index size

**When It Happens**:
- Concurrent `DROP INDEX` and `SELECT/UPDATE/DELETE` operations
- Multiple clients modifying same table schema
- Schema changes during active query workload

**Workarounds**:
1. **Maintenance Windows**: Run schema changes during low-traffic periods
2. **Application Coordination**: Use application-level locking for schema changes
3. **Monitoring**: Watch for unexpected empty query results after schema operations
4. **Avoid DROP INDEX**: Consider keeping indexes (minimal overhead if not queried)

**Planned Fix (v2.2)**:
- **Option A**: Reverse deletion order (delete keys first, metadata last)
- **Option B**: Soft-delete with tombstone pattern (mark index as "deleting")
- **Option C**: Atomic operation with proper locking

**Current Recommendation**: 
- ✅ `ADD INDEX` is safe (builds incrementally, visible only when complete)
- ⚠️ `DROP INDEX` should be done during maintenance windows
- ✅ Query operations are safe (read-only, no schema modification)

### Data Constraints

3. **Maximum 64 characters** for namespace and table names
   - Error: "ERR incorrect namespace/table name, it exceeds the limit of 64 characters"

4. **Date format**: YYYY-MM-DD only (no time component)

5. **Float precision**: Limited by string conversion

### Query Limitations

6. **No compound indexes** (one index per column)
7. **No LIKE/pattern matching**
8. **No JOIN operations**
9. **No transactions**
10. **Comparison operators** (>, <, >=, <=) require full table scan (up to 100K limit)
11. **Equality (=) search** requires indexed columns

### Workarounds

**Large datasets**:
- Use indexed columns for equality searches
- Add more specific WHERE conditions to reduce scan size
- Consider partitioning data across multiple tables

**Pattern matching**:
- Implement application-level filtering after SELECT
- Consider using Redis Search module for advanced patterns

**Joins**:
- Perform multiple queries and join in application
- Denormalize data into single table when possible

\newpage

# Appendix

## Complete Command Summary

| Command | Purpose | Example |
|---------|---------|---------|
| `TABLE.NAMESPACE.CREATE` | Create namespace | `TABLE.NAMESPACE.CREATE mydb` |
| `TABLE.SCHEMA.VIEW` | View table structure | `TABLE.SCHEMA.VIEW mydb.users` |
| `TABLE.SCHEMA.CREATE` | Create table | `TABLE.SCHEMA.CREATE mydb.users NAME:string AGE:integer` |
| `TABLE.SCHEMA.ALTER` | Modify table | `TABLE.SCHEMA.ALTER mydb.users ADD COLUMN EMAIL:string` |
| `TABLE.INSERT` | Insert row | `TABLE.INSERT mydb.users NAME=John AGE=30` |
| `TABLE.SELECT` | Query rows | `TABLE.SELECT mydb.users WHERE AGE>25` |
| `TABLE.UPDATE` | Update rows | `TABLE.UPDATE mydb.users WHERE NAME=John SET AGE=31` |
| `TABLE.DELETE` | Delete rows | `TABLE.DELETE mydb.users WHERE AGE>65` |
| `TABLE.DROP` | Drop table | `TABLE.DROP mydb.users` |
| `TABLE.HELP` | Show help | `TABLE.HELP` |

---

## Operator Reference

### Comparison Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `=` | Equals | `WHERE AGE=30` |
| `>` | Greater than | `WHERE AGE>30` |
| `<` | Less than | `WHERE AGE<30` |
| `>=` | Greater or equal | `WHERE AGE>=30` |
| `<=` | Less or equal | `WHERE AGE<=30` |

### Logical Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `AND` | Both conditions true | `WHERE AGE>25 AND DEPT=Sales` |
| `OR` | Either condition true | `WHERE AGE>65 OR AGE<18` |

---

## Data Type Reference

| Type | Format | Example | Validation |
|------|--------|---------|------------|
| `string` | Any text | `"Hello"` | None |
| `integer` | Whole number | `123`, `-45` | Digits, optional +/- |
| `float` | Decimal | `123.45` | Digits, optional decimal |
| `date` | YYYY-MM-DD | `2024-01-15` | Exactly 10 chars, hyphens at 4 & 7 |

---

## Quick Reference Card

### Create and Populate Table

```bash
TABLE.NAMESPACE.CREATE mydb
TABLE.SCHEMA.CREATE mydb.users NAME:string:true AGE:integer:false
TABLE.INSERT mydb.users NAME=Alice AGE=28
TABLE.INSERT mydb.users NAME=Bob AGE=35
```

### Query Data

```bash
TABLE.SELECT mydb.users
TABLE.SELECT mydb.users WHERE NAME=Alice
TABLE.SELECT mydb.users WHERE AGE>30
```

### Update and Delete

```bash
TABLE.UPDATE mydb.users WHERE NAME=Alice SET AGE=29
TABLE.DELETE mydb.users WHERE AGE>65
```

### Manage Indexes

```bash
TABLE.ALTER mydb.users ADD INDEX AGE
TABLE.ALTER mydb.users DROP INDEX AGE
TABLE.SCHEMA.VIEW mydb.users
```

### Clean Up

```bash
TABLE.DROP mydb.users FORCE
```

---

### Build System

The module includes a comprehensive Makefile with the following targets:

| Target | Purpose | Description |
|--------|---------|-------------|
| `make` | Build module | Compiles `redis_table.so` from source |
| `make clean` | Clean artifacts | Removes `.o` and `.so` files |
| `make test` | Run tests | Executes all 86 tests with Redis server |
| `make debug` | Debug build | Builds with debug symbols and info |
| `make install` | Install module | Copies to system module directory |

### Additional Resources

### Documentation
- **README.md**: Complete technical documentation with updated build instructions
- **TESTING.md**: Testing guide and test suite documentation
- **USER_GUIDE.md**: This comprehensive user guide

### Support
- Check the module source code for implementation details
- Review test scripts for usage examples (86 comprehensive tests)
- Inspect Redis keys for debugging
- Use `make test` to verify functionality

### Best Practices
1. Always create namespace before tables
2. Index only frequently queried columns
3. Use comparison operators sparingly
4. Monitor memory usage
5. Test queries before production use
6. Run `make test` after any changes
7. Use `make clean && make` for clean builds

---

**Document Version**: 2.0  
**Module Version**: 2.0  
**Last Updated**: October 2025  
**Build System**: Fully functional Makefile with comprehensive testing

---

\newpage

# Index

- **ADD COLUMN**, 13
- **ADD INDEX**, 13, 28
- **Comparison Operators**, 15, 27
- **Data Types**, 6
- **Date Type**, 7
- **DELETE**, 17
- **DROP INDEX**, 13
- **DROP TABLE**, 13
- **Float Type**, 7
- **Indexes**, 26
- **INSERT**, 15
- **Integer Type**, 6
- **Logical Operators**, 16
- **Performance**, 26
- **Schema**, 8
- **SELECT**, 15
- **String Type**, 6
- **Testing Framework**, 32
- **Troubleshooting**, 35
- **UPDATE**, 17

