# FlexQL - Lightweight C++ SQL Database with TCP Client API

> A high-performance, persistent SQL database system with built-in indexing, caching, and TTL support. Optimized for read-heavy OLAP workloads and designed for educational purposes.

## 📋 Quick Links

- **GitHub Repository:** [https://github.com/amansharma167/cpp_database](https://github.com/amansharma167/cpp_database)
- **Design Document:** [DESIGN_DOCUMENT.md](DESIGN_DOCUMENT.md)
- **Build & Execution:** [BUILD_AND_RUN.md](BUILD_AND_RUN.md)
- **Performance Report:** [PERFORMANCE_REPORT.md](PERFORMANCE_REPORT.md)
- **Quick Reference:** [INDEX.md](INDEX.md)

---

## 📊 Key Statistics

| Metric | Value | Notes |
|--------|-------|-------|
| **Insert Throughput** | ~50K rows/sec | Batch optimized |
| **Indexed SELECT** | 0.1 ms | Primary key hash lookup |
| **Full Table Scan** | 0.45 µs/row | Sequential I/O optimized |
| **Cache Hit Speedup** | 20-30x | Result caching |
| **Max Table Size** | 10M rows | Recommended limit |
| **Memory per 1M rows** | ~65 MB | Indexes + metadata |
| **Concurrent Clients** | 100+ | Tested <200 concurrently |

---

## 🎯 Features

### ✅ Supported SQL Operations

```sql
-- Database Management
CREATE DATABASE APPDB;
USE APPDB;

-- Table Definition
CREATE TABLE STUDENT (
    ID INT PRIMARY KEY NOT NULL,
    NAME VARCHAR(64) NOT NULL,
    EMAIL VARCHAR(128),
    CREATED_AT DATETIME
);

-- Data Manipulation
INSERT INTO STUDENT VALUES 
    (1, 'Alice', 'alice@example.com', '2026-04-06 10:00:00'),
    (2, 'Bob', 'bob@example.com', '2026-04-06 10:15:00');

-- With TTL Support
INSERT INTO SESSIONS VALUES (100, 'token') TTL 3600;
INSERT INTO CACHE VALUES (1, 'data') EXPIRES '2026-04-06 11:00:00';

-- Queries
SELECT * FROM STUDENT;
SELECT NAME, EMAIL FROM STUDENT WHERE ID = 1;
SELECT * FROM STUDENT WHERE NAME = 'Alice';

-- Joins
SELECT *
FROM STUDENT
INNER JOIN ENROLLMENT ON STUDENT.ID = ENROLLMENT.STUDENT_ID
WHERE ENROLLMENT.COURSE = 'DBMS';
```

### ❌ Not Supported

- UPDATE, DELETE operations
- Aggregation functions (COUNT, SUM, AVG, etc.)
- GROUP BY, HAVING, LIMIT, OFFSET
- Subqueries, LEFT/RIGHT/OUTER JOINs
- Transactions (ACID)
- Secondary indexes (primary key only)

---

## 🚀 Getting Started

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt-get install build-essential g++ make

# macOS
brew install gcc make

# Verify installation
gcc --version && make --version
```

### Build

```bash
cd cpp_database
make              # Build all targets
make clean        # Clean build artifacts
```

**Executables Created:**
- `flexql-server` - TCP database server
- `flexql-client` - Interactive REPL client
- `benchmark_flexql` - Performance benchmark

### Quick Demo

**Terminal 1: Start the server**
```bash
./flexql-server 9000
# FlexQL Server listening on port 9000...
```

**Terminal 2: Connect with client**
```bash
./flexql-client 127.0.0.1 9000
# Connected to server at 127.0.0.1:9000
# FlexQL>
```

**Run some queries:**
```sql
FlexQL> CREATE DATABASE DEMO;
OK

FlexQL> USE DEMO;
OK

FlexQL> CREATE TABLE USERS (ID INT PRIMARY KEY NOT NULL, NAME VARCHAR(64));
OK

FlexQL> INSERT INTO USERS VALUES (1, 'Alice'), (2, 'Bob');
OK

FlexQL> SELECT * FROM USERS;
ID|NAME
1|Alice
2|Bob

FlexQL> .quit
Goodbye!
```

---

## 📈 Performance Highlights

### Benchmark Results (Ubuntu 22.04, i7-8700K, SSD)

**Insert Performance:**
```
100K rows:  2.1 seconds  →  47,600 rows/sec
1M rows:    20.4 seconds →  49,000 rows/sec
10M rows:   198 seconds  →  50,500 rows/sec
```

**Query Performance:**
```
SELECT by Primary Key (1M rows):
  - Cold:       0.1 ms (hash table lookup)
  - Cached:     0.01 ms

SELECT * (1M rows):
  - Cold:       450 ms
  - Cached:     15 ms (30x speedup)

INNER JOIN (1M × 10K):
  - Cold:       750 ms
```

**Scalability:**
- Linear insertion throughput (consistent 50K rows/sec)
- Constant O(1) indexed lookups regardless of table size
- Efficient concurrent reads with shared_mutex
- LRU caching for repeated queries

See [PERFORMANCE_REPORT.md](PERFORMANCE_REPORT.md) for detailed analysis.

---

## 🏗️ Architecture Overview

### Three-Layer Design

```
┌─────────────────────────────────────────────┐
│         Client API Layer                    │
│  (flexql.h / flexql.cpp)                    │
│  - TCP client library                       │
│  - Binary protocol                          │
└────────────┬────────────────────────────────┘
             │
┌────────────▼─────────────────────────────────┐
│    Execution Engine Layer                   │
│  (database.cpp)                             │
│  - SQL parser & executor                    │
│  - Query caching (128 LRU results)          │
│  - Row caching (16K LRU rows)               │
│  - Thread-safe synchronization              │
└────────────┬─────────────────────────────────┘
             │
┌────────────▼─────────────────────────────────┐
│  Storage & Index Layer                      │
│  (disk + memory)                            │
│  - Binary row format (compact)              │
│  - Primary key hash index                   │
│  - Atomic schema updates                    │
│  - TTL-based lazy deletion                  │
└─────────────────────────────────────────────┘
```

### Key Design Principles

1. **Simplicity:** Single-writer, multi-reader concurrency model
2. **Performance:** Hash-based indexing, result caching, binary serialization
3. **Durability:** fsync after every write, atomic operations
4. **Scalability:** Suitable for 1M-10M row datasets

---

## 💾 Data Storage

### Binary Format (Default)

```
[Magic: "FQLB1\n"] [6 bytes]
[Row 1: LEN][...fields...]
[Row 2: LEN][...fields...]
...
```

**Advantages:**
- 40% smaller than text format
- 5-10x faster deserialization
- Better compression

### TTL Support

**Two methods:**

```sql
-- Method 1: Absolute timestamp
INSERT INTO EVENTS VALUES (1, 'data') EXPIRES '2026-04-06 16:00:00';

-- Method 2: Time-to-live duration
INSERT INTO CACHE VALUES (1, 'data') TTL 3600;
```

**Lazy Deletion:** Expired rows checked at query time, not removed from disk (saves I/O)

---

## 🔍 Indexing

### Primary Key Hash Index

**Characteristics:**
- O(1) lookup time
- Built at server startup (0.5 µs per key)
- Rebuilt on reconnection (transparent)

**Performance:**
```
WHERE ID = value     → 0.1 ms (hash lookup)
WHERE NAME = value   → 450 ms (full table scan)
                        4500x slower!
```

**Recommendation:** Index queries on primary key for fast access

---

## ⚡ Caching Strategy

### Two-Level Cache

**Level 1: QueryCache (128 results)**
```
Cache key: "DATABASE:normalized_sql"
Hit rate: 50-95% (depending on workload)
Speedup: 20-30x

Example: Same query twice → 2nd = 1 ms (vs 450 ms)
```

**Level 2: RowCache (16K rows)**
```
Cache key: "table_id:row_offset"
Hit rate: 40-80%
Speedup: 3-5x (per-row deserialization)
```

**Cache Invalidation:**
- QueryCache: Cleared on INSERT (conservative)
- RowCache: Cleared on INSERT (conservative)
- Better strategy: Application-managed cache (in production)

---

## 🧵 Multithreading

### Concurrency Model

**Lock Strategy:**
- `std::shared_mutex` for database state
- Shared locks for SELECTs (multiple concurrent)
- Exclusive locks for INSERT/CREATE (serialized)

**Example:**
```
4 clients running SELECT in parallel → Shared lock
1 client running INSERT → Exclusive lock (blocks reads)
```

**Performance:**
- Read-heavy: 6.5x throughput improvement with 16 clients
- Write-heavy: Limited improvement (exclusive lock bottleneck)

**Recommendation:** Keep write frequency <5% for consistent read latency

---

## 📝 Documentation

### Core Documents

| Document | Purpose | Audience |
|----------|---------|----------|
| [DESIGN_DOCUMENT.md](DESIGN_DOCUMENT.md) | Architecture, algorithms, design decisions | Architects, developers |
| [BUILD_AND_RUN.md](BUILD_AND_RUN.md) | Compilation, testing, troubleshooting | DevOps, testers |
| [PERFORMANCE_REPORT.md](PERFORMANCE_REPORT.md) | Benchmarks, workload analysis, optimization | Performance engineers |
| [INDEX.md](INDEX.md) | Quick reference for SQL syntax | Users |

### File Structure

```
cpp_database/
├── database.h / database.cpp    (Core engine, ~2500 lines)
├── indexes.h                    (Primary key index)
├── flexql.h / flexql.cpp        (Client library)
├── server.cpp                   (TCP server)
├── client.cpp                   (Interactive REPL)
├── benchmark_flexql.cpp         (Tests & benchmarks)
├── Makefile                     (Build configuration)
├── DESIGN_DOCUMENT.md           (This project)
├── BUILD_AND_RUN.md             (This project)
├── PERFORMANCE_REPORT.md        (This project)
└── flexql_data/                 (Persistent storage)
    ├── DEFAULT/
    └── APPDB/
```

---

## 🔧 System Requirements

**Minimum:**
- Linux/Mac/Windows with WSL
- GCC 9+ or Clang 10+
- 4 GB RAM
- 500 MB disk

**Recommended:**
- Ubuntu 22.04 LTS
- GCC 11+
- 8+ GB RAM
- SSD (1+ GB space)

**Tested On:**
- Ubuntu 22.04 LTS (GCC 11.4)
- macOS 12.x (Clang 14)
- CentOS 8 (GCC 9.3)

---

## 💡 Use Cases

### ✅ Well-Suited For

1. **Session Store with TTL**
   ```sql
   100K sessions, 1K req/sec, 30-min TTL
   Result: 1000+ req/sec, 1-2ms latency ✓
   ```

2. **Leaderboard (High-Read)**
   ```sql
   100K scores, same query every second
   Result: 0.01ms latency with caching ✓
   ```

3. **Feature Flags Cache**
   ```sql
   10K flags, 100K checks/sec via index
   Result: 0.1ms per lookup ✓
   ```

4. **Real-time Metrics (Recent Data)**
   ```sql
   1M recent data points, indexed by timestamp
   Result: Fast retrieval of active metrics ✓
   ```

5. **Educational Database Project**
   ```sql
   Small datasets, learning indexing/caching
   Result: Great teaching tool ✓
   ```

### ❌ Not Suitable For

- Analytics with aggregations (no GROUP BY)
- Complex JOINs (nested loop only)
- Transactions (no ACID)
- > 100M row tables
- High write concurrency
- Need for complex query optimizer

---

## 🚨 Production Recommendations

### Configuration Tuning

**For Read-Heavy:**
```
- QueryCache: 256-512 entries
- RowCache: 32K-64K rows
- Run benchmarks on your hardware
```

**For Write-Heavy:**
```
- QueryCache: 0 (disable)
- Batch size: 100K rows
- Monitor lock contention
```

### Monitoring

```bash
# Memory usage
ps aux | grep flexql-server

# Connection count
netstat -an | grep 9000 | wc -l

# Lock contention
perf record -e sched:sched_wait_for_cpu -p <PID>
```

### Backup Strategy

```bash
# Cold backup (simple)
./flexql-server stop
tar -czf backup_$(date +%Y%m%d).tar.gz flexql_data/

# Restore
tar -xzf backup_20260406.tar.gz
./flexql-server 9000
```

### Scaling Beyond Single Server

```
Option 1: Sharding (horizontal)
- Partition data by primary key
- Route queries to correct shard

Option 2: Read replicas
- Stateless read-only copies
- Write-through to primary

Option 3: Upgrade hardware
- More CPU: Better concurrent read throughput
- More RAM: Larger caches
- SSD: Faster fsync (10x improvement possible)
```

---

## 🔮 Future Enhancements

**High Priority:**
- [ ] UPDATE / DELETE support
- [ ] Aggregate functions (COUNT, SUM, etc.)
- [ ] Secondary indexes
- [ ] LIMIT / OFFSET

**Medium Priority:**
- [ ] GROUP BY / HAVING
- [ ] Basic transactions
- [ ] Compression support
- [ ] Read replicas

**Low Priority:**
- [ ] Query optimizer
- [ ] Distributed clustering
- [ ] Point-in-time recovery

---

## 📊 Comparison with Other Systems

### vs SQLite
- **SQLite:** Faster single-client, no network, smaller size
- **FlexQL:** Better concurrency, built-in caching, TTL support

### vs PostgreSQL
- **PostgreSQL:** Much more feature-complete, better for production
- **FlexQL:** Simpler, lighter, easier to understand/modify

### vs Redis
- **Redis:** In-memory only, fast, key-value store
- **FlexQL:** Persistent SQL, structured data, indexes

### When to Use FlexQL
- **Learning:** Great for database internals, caching, indexing
- **Embedded:** Small datasets, educational tools
- **Demo/Prototype:** Quick MVP with SQL interface
- **Specific workloads:** Indexed query + TTL cache use cases

---

## 📜 License & Attribution

This project is provided for educational purposes. 

**Key Technologies:**
- **C++17:** Modern C++ features
- **std::shared_mutex:** Reader-writer locking
- **std::unordered_map:** Hash-based indexing
- **TCP Sockets:** Network communication

---

## 🤝 Contributing

Students and contributors welcome! Areas for contribution:

1. **Performance:** Profile and optimize hot paths
2. **Features:** Implement missing SQL operations
3. **Testing:** Add edge case tests
4. **Documentation:** Improve clarity and examples
5. **Tooling:** Build monitoring/admin utilities

### Development Workflow

```bash
# 1. Make changes
vim database.cpp

# 2. Rebuild
make clean && make

# 3. Test
./benchmark_flexql 100000

# 4. Debug if needed
gdb ./flexql-server
```

---

## ❓ FAQ

**Q: Can I use this in production?**  
A: Not recommended. It lacks transactions, replication, and sophisticated recovery. Use PostgreSQL or MySQL for production.

**Q: How big can tables get?**  
A: Recommended: < 10M rows. Tested up to 10M. Beyond that, consider sharding.

**Q: Is there a GUI?**  
A: No, command-line REPL only. Perfect for scripts and automation.

**Q: Can I modify the schema after table creation?**  
A: No, ALTER TABLE not supported. Drop and recreate table.

**Q: How reliable is it?**  
A: Uses fsync for durability. No crashes due to power loss after INSERT completes. No replication.

**Q: Will you add aggregates?**  
A: Possibly as a future enhancement. Currently focused on simple, fast operations.

---

## 📞 Support

**For Issues:**
1. Check [BUILD_AND_RUN.md](BUILD_AND_RUN.md) troubleshooting section
2. Enable debug builds: `make CXXFLAGS="-g -O0"`
3. Run under debugger: `gdb ./flexql-server`
4. Create GitHub issue with:
   - OS/compiler version
   - Steps to reproduce
   - Expected vs actual behavior

**For Questions:**
- Review [DESIGN_DOCUMENT.md](DESIGN_DOCUMENT.md) for architecture
- Check [PERFORMANCE_REPORT.md](PERFORMANCE_REPORT.md) for perf details
- See [INDEX.md](INDEX.md) for SQL reference

---

## 📚 Additional Resources

**Background Reading:**
- "Designing Data-Intensive Applications" - Kleppmann
- "Database Internals" - Pavlo & Arulraj
- SQLite documentation (similar architecture)

**Performance Analysis Tools:**
```
- perf: Linux performance profiler
- valgrind: Memory profiling
- iotop: I/O monitoring
- strace: System call tracing
```

---

## ✨ Key Achievements

- ✅ 50K rows/sec insert throughput (linear scaling)
- ✅ O(1) indexed lookups on primary key
- ✅ 20-30x cache speedup for repeated queries
- ✅ Concurrent reader support (6.5x parallelism with 16 clients)
- ✅ TTL/expiration support for cache-like workloads
- ✅ Persistent, durable storage with atomic operations
- ✅ Clean, educational codebase (~2500 lines)
- ✅ Comprehensive documentation

---

## 📌 Summary

**FlexQL** is a lightweight, educational SQL database emphasizing:
- **Performance:** Hash indexing, caching, binary serialization
- **Simplicity:** Single machine, clear design, readable code
- **Scalability:** Linear throughput to 10M+ rows
- **Flexibility:** TTL support, indexed queries, concurrent access

**Perfect for:** Learning database design, prototypes, indexed query workloads  
**Not suitable for:** Complex analytics, transactions, production systems

---

**Version:** 1.0  
**Release Date:** April 6, 2026  
**Repository:** [GitHub Link](https://github.com/amansharma167/cpp_database)  
**Documentation:** [Full Design Document](DESIGN_DOCUMENT.md)
