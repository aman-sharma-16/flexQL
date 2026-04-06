# FlexQL C++ Project

This project provides a small SQL server with a TCP client API, persistent on-disk storage, and a benchmark target for exercising the public `flexql_*` API.

## Files

- `flexql.h`: public client API
- `flexql.cpp`: client-side socket implementation
- `database.h` / `database.cpp`: SQL parser, execution engine, persistence, indexes, and caches
- `indexes.h`: primary-key index helper
- `server.cpp`: multithreaded TCP server
- `client.cpp`: interactive REPL client
- `benchmark_flexql.cpp`: unit tests plus insert benchmark
- `Makefile`: build targets for server, client, and benchmark

## Build

```bash
make
```

## Run

Terminal 1:

```bash
./flexql-server 9000
```

Terminal 2:

```bash
./flexql-client 127.0.0.1 9000
```

Benchmark:

```bash
./benchmark_flexql 100000
```

## Supported SQL

### Database management

```sql
CREATE DATABASE APPDB;
CREATE DATABASE IF NOT EXISTS APPDB;
USE APPDB;
```

### Table creation

```sql
CREATE TABLE STUDENT (
    ID INT PRIMARY KEY NOT NULL,
    FIRST_NAME VARCHAR(64) NOT NULL,
    LAST_NAME VARCHAR(64) NOT NULL,
    EMAIL VARCHAR(128) NOT NULL,
    CREATED_AT DATETIME
);

CREATE TABLE IF NOT EXISTS ENROLLMENT (
    ID INT PRIMARY KEY NOT NULL,
    STUDENT_ID INT NOT NULL,
    COURSE VARCHAR(64) NOT NULL
);
```

### Insert

Single row:

```sql
INSERT INTO STUDENT VALUES (1, 'John', 'Doe', 'john@gmail.com', '2026-03-22 10:00:00');
```

Batch insert:

```sql
INSERT INTO STUDENT VALUES
    (2, 'Alice', 'Smith', 'alice@gmail.com', '2026-03-22 10:01:00'),
    (3, 'Bob', 'Taylor', 'bob@gmail.com', '2026-03-22 10:02:00'),
    (4, 'Carol', 'Lee', 'carol@gmail.com', '2026-03-22 10:03:00');
```

Expiring rows:

```sql
INSERT INTO STUDENT VALUES (5, 'Temp', 'User', 'temp@gmail.com', '2026-03-22 10:05:00') EXPIRES '2026-03-22 11:00:00';
INSERT INTO STUDENT VALUES (6, 'TTL', 'User', 'ttl@gmail.com', '2026-03-22 10:06:00') TTL 60;
```

### Select

```sql
SELECT * FROM STUDENT;
SELECT FIRST_NAME, EMAIL FROM STUDENT;
SELECT * FROM STUDENT WHERE ID = 1;
SELECT * FROM STUDENT WHERE FIRST_NAME = 'Alice';
```

### Inner join

```sql
SELECT *
FROM STUDENT
INNER JOIN ENROLLMENT
ON STUDENT.ID = ENROLLMENT.STUDENT_ID;

SELECT STUDENT.FIRST_NAME, ENROLLMENT.COURSE
FROM STUDENT
INNER JOIN ENROLLMENT
ON STUDENT.ID = ENROLLMENT.STUDENT_ID
WHERE ENROLLMENT.COURSE = 'DBMS';
```

## Persistence And Fault Tolerance

- Data lives under `flexql_data/<DATABASE_NAME>/`
- Each table uses a durable schema file (`*.schema`) and append-only row file (`*.rows`)
- New tables use a compact binary row format to reduce write amplification and speed up large inserts; older text row files remain readable
- Schema writes use atomic temp-file replacement
- Row appends are acknowledged only after the row file is flushed to disk
- The server rebuilds table metadata and primary-key indexes from disk on restart
- RAM is used only as an acceleration layer; disk remains the durable source of truth

## Performance Notes

- The benchmark uses adaptive batching: `5000` rows for small runs, `50000` rows for million-scale runs, and `100000` rows for multi-million runs such as 10M or 15M ingestion
- Large benchmark tables use integer-heavy schemas for `ID`, `BALANCE`, and `EXPIRES_AT`, which reduces parsing and normalization overhead
- Benchmark row payloads are intentionally compact for large-ingest tests so the measurement tracks database ingestion overhead more than string bloat
- Table append file descriptors are kept open to avoid repeated open/close costs during insert-heavy workloads
- Row metadata stored in RAM is intentionally small so very large persistent tables can still be tracked efficiently in memory
- Insert operations no longer populate the row cache eagerly; the row cache is filled on demand by reads, which avoids extra lock/copy work during bulk loads
- New tables are stored in a binary row format, which reduces serialization CPU, disk bytes written, and readback overhead
- Recently materialized rows are cached in RAM, which speeds repeated reads and joins without changing durability guarantees
- `SELECT` results are also cached with a small LRU result cache
- Primary-key equality lookups use an in-memory hash index

## Current Limits

- One `WHERE` condition is supported
- `INNER JOIN` supports one equality join condition
- `UPDATE`, `DELETE`, transactions, and secondary indexes are not implemented
- Expired rows are filtered at read time and remain in the append-only row file
