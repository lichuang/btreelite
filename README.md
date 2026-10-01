# btreelite

A standalone key-value storage engine extracted from SQLite's storage
subsystem — the B-tree layer, the pager, and WAL — with the SQL layer
removed entirely.

## What it is

- **Origin**: derived from SQLite 3.54.0 (public domain). The B-tree, pager,
  and WAL code are reused with only interface-level shims; no algorithmic
  changes.
- **Model**: plain key-value semantics. Keys are byte strings ordered by
  binary comparison (memcmp). Values are arbitrary byte strings.
- **Multiple trees**: one database file can contain many B-trees, each
  identified by its root page number.  There is no catalog of tree roots:
  `btreelite_create_tree()` hands back the root page and the application is
  responsible for persisting it wherever it tracks its keys; a root page
  number is the handle for every later `cursor_open`/`clear_tree`.  Page 1
  hosts the file-format header (its first 100 bytes) and doubles as the
  always-empty internal tree root of the format; keep it reserved.
- **In-memory databases**: `btreelite_open(NULL)` (or `":memory:"`) opens a
  heap-resident database that vanishes at `btreelite_close()` — commits are
  real transactions but nothing survives the close, and it runs in the
  MEMORY journal mode because a WAL needs a file.  Handy as scratch space
  or for tests.
- **ACID transactions**: inherited from SQLite's write-ahead log, on by
  default and durable (`synchronous=FULL`, matching SQLite's default).
- **Concurrency**: in-process multi-threading plus cross-process file
  locking, inherited from SQLite.
- **File format**: same page/cell layout as SQLite, with a different magic
  string so `sqlite3` CLI tools do not mistake it for a SQL database.
- **Build artifact**: `libbtreelite.a` (no third-party
  dependencies). Public API functions are prefixed `btreelite_`.

The on-disk format uses the table-btree (leaf-data) page family with a
byte-string key instead of the index-btree record format, so keys are always
held locally and lookups never touch overflow pages.  It is specified in
[KV storage format design](docs/kv-format-design.md) (中文:
[KV 存储格式设计](docs/kv-format-design-cn.md)); the journal-mode design and
its durability trade-offs are in [design notes](docs/design.md).

## Benchmark: btreelite (KV) vs SQLite (SQL)

`make bench` measures KV throughput against SQL on the same workload,
building two binaries from the one source `test/bench.c`:

- **`bench_kv`** — the same file compiled against `libbtreelite.a` and
  driven through the public `btreelite_*` API on byte-string keys.
- **`bench_sql`** — the same file compiled with `-DBENCH_SQL` against the
  system `libsqlite3`, driven through prepared statements over an
  `INTEGER PRIMARY KEY` table with blob values.

Sharing one source is the point: both engines execute *literally the same*
workload code, so the comparison measures the engine and its interface, not
a difference in the driver.  The workload is four phases — point lookups,
sequential inserts, in-place overwrites and deletes, each timed separately
and one transaction per batch — on byte-string rowids ("row-NNNNNNNN")
for KV and the same numbers as integer keys for SQL.  Both engines run at
WAL and `synchronous=FULL`, so the comparison is between the storage
machinery and the interfaces, not between durability settings.
`make bench BENCHARG="<rows-per-batch> <batches>"` scales it; the default
is 1,000 rows per batch in 10 batches (10,000 rows total).

Measured on macOS (arm64), Apple M1 Max, 64 GB RAM, rows/s:

| Rows inserted first | Phase | SQLite (SQL) | btreelite (KV) | KV vs SQL |
|---|---|---|---|---|
| 100,000 | lookup | 810,202 | **4,689,332** | **5.8x faster** |
| 100,000 | insert | **2,148,828** | 2,221,630 | on par |
| 100,000 | overwrite | 2,263,416 | **3,345,601** | **1.48x faster** |
| 100,000 | delete | 2,465,058 | **3,443,763** | **1.40x faster** |
| 1,000,000 | lookup | 813,324 | **4,465,422** | **5.5x faster** |
| 1,000,000 | insert | 2,082,284 | 2,060,594 | on par |
| 1,000,000 | overwrite | 2,322,395 | **3,131,567** | **1.35x faster** |
| 1,000,000 | delete | 2,749,421 | **3,563,157** | **1.30x faster** |

## License

Derived from SQLite, which is public domain. This project is likewise
released to the public domain.

```
May you do good and not evil.
May you find forgiveness for yourself and forgive others.
May you share freely, never taking more than you give.
```