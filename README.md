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
- **ACID transactions**: inherited from SQLite's write-ahead log.  WAL is
  the default (and durable `synchronous=FULL`, matching SQLite's default);
  `btreelite_journal_mode()` exposes only WAL and MEMORY, and an in-memory
  database or an environment that cannot host the WAL shared-memory index
  falls back to MEMORY automatically.
- **Concurrency**: in-process multi-threading plus cross-process file
  locking, inherited from SQLite.
- **File format**: same page/cell layout as SQLite, with a different magic
  string so `sqlite3` CLI tools do not mistake it for a SQL database.
- **Build artifact**: `libbtreelite.a` (no third-party
  dependencies). Public API functions are prefixed `btreelite_`.

The design and phased extraction plan is documented in
[docs/phase.md](docs/phase.md).

## Status

Phase 0 (build skeleton with SQL-free compilation), Phase 1 (decoupling the
btree/pager/journal stack from the SQL layer), Phase 2 (KV cell format plus
the public API), Phase 3 (WAL seam: mode transitions, checkpointing,
autocheckpoint, busy timeout) and Phase 4 (the last declared APIs:
savepoints, incremental blob write, integrity check, memory accounting) are
complete; see
[docs/phase.md](docs/phase.md) for the plan and
[docs/phase0-report.md](docs/phase0-report.md),
[docs/phase1-report.md](docs/phase1-report.md),
[docs/phase2-report.md](docs/phase2-report.md) for the reports.

The KV cell format was subsequently redesigned to use the table-btree
(leaf-data) page family with a byte-string key instead of the index-btree
record format, so keys are always held locally and lookups never touch
overflow pages.  See
[docs/kv-format-design.md](docs/kv-format-design.md) (Chinese original:
[docs/kv-format-design-cn.md](docs/kv-format-design-cn.md)).  With that in
place `make test` passes in full across eight suites (`t_smoke`, `t_kv`,
`t_big`, `t_wal`, `t_api`, `t_proc`, `t_dur`, `t_trees`).

## Benchmark: btreelite (KV) vs SQLite (SQL)

The extraction plan calls for a KV-vs-SQL throughput comparison.  `make
bench` builds and runs it, producing two binaries from the *one* workload
source, `test/bench.c`:

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

Measured on macOS (arm64), rows/s:

| Rows inserted first | Phase | SQLite (SQL) | btreelite (KV) | KV vs SQL |
|---|---|---|---|---|
| 100,000 | lookup | 793,323 | **4,774,637** | **6.0x faster** |
| 100,000 | insert | **2,115,328** | 1,615,483 | 1.31x slower |
| 100,000 | overwrite | 2,260,756 | **3,290,231** | **1.46x faster** |
| 100,000 | delete | 2,362,837 | **3,328,119** | **1.41x faster** |
| 1,000,000 | lookup | 791,496 | **4,418,464** | **5.6x faster** |
| 1,000,000 | insert | **2,013,644** | 1,626,638 | 1.24x slower |
| 1,000,000 | overwrite | 2,279,161 | **3,186,753** | **1.40x faster** |
| 1,000,000 | delete | 2,614,988 | **3,431,144** | **1.31x faster** |

How to read it (and where the plan's "KV slightly faster" holds or not):

- **Lookups are the story.**  5.6–6.0x: a KV search is a `memcmp` against a
  byte-string key held locally on the page — no record encoding, no
  VDBE-style dispatch.  This is the return on the KV cell redesign.
- **Overwrites and deletes run 1.3–1.5x faster**, for the same reason:
  no record decode on the way to the payload.
- **Sequential inserts are the one exception** (about 1.25x slower):
  the SQL side inserts via rowid append (`BTREE_APPEND`), which lands
  each new row at the end of the tree in O(1); the KV side bisects for
  every key.  Appending-like traffic (monotone keys) is where the KV
  insert path is worth an explicit fast path, if it ever matters.

## Journal modes

btreelite exposes exactly two journal modes; the rest of SQLite's are
deliberately omitted.

| Mode | Role in btreelite |
|---|---|
| `BTREELITE_JOURNAL_WAL` | The default for every file-backed database. |
| `BTREELITE_JOURNAL_MEMORY` | Fallback for in-memory databases and for environments that cannot host the WAL shared-memory index. |

### Why WAL is the default (a deliberate departure from SQLite)

SQLite's own default is `journal_mode=DELETE`; WAL is only entered after an
explicit `PRAGMA journal_mode=WAL`, though once set it is *persistent* (the
choice is recorded in the file header).  btreelite instead turns WAL on at
`btreelite_open()`, because for a file-backed database WAL is a strict
superset of every rollback-journal mode:

| Property | DELETE / TRUNCATE / PERSIST | WAL |
|---|---|---|
| Atomic commit and rollback | yes | yes |
| Crash safety (no corruption) | yes | yes |
| Durable at `synchronous=FULL` | yes | yes |
| Readers not blocked by a writer | **no** (writer takes EXCLUSIVE) | **yes** (snapshot isolation) |
| Write amplification | 2x (read old page, then write) | 1x (sequential append) |
| `fsync()` per commit | more | fewer |
| Survives reopen as the same mode | no (resets to DELETE) | yes (persistent) |
| `-shm` shared memory required | no | **yes** |
| Usable on a network filesystem | yes | **no** |

DELETE, TRUNCATE and PERSIST are the *same* capability — a disk rollback
journal — differing only in how the journal is finalized on commit (deleted,
truncated to zero, or header zeroed).  That is a tuning knob, not a feature,
so omitting them costs nothing.  `OFF` is omitted because it disables atomic
commit entirely (ROLLBACK is undefined and ordinary writes can corrupt the
file).  `MEMORY` is the one mode WAL cannot replace — it is the only option
with no on-disk journal — so it is kept as the automatic fallback.

### Consequence: WAL is sticky

Because WAL state lives in the file header (`version=2`), the first time a
database is opened by btreelite it is converted to WAL and stays that way —
including when it is later opened by stock SQLite.  The `-wal` and `-shm`
companion files should be expected alongside the database.

### Risk: the MEMORY fallback is not crash-safe

When a **file-backed** database runs where WAL is unavailable (a VFS with no
`-shm` support, or a `nolock` URI), btreelite silently falls back to MEMORY.
MEMORY keeps `ROLLBACK` working but stores the rollback journal in RAM, so a
crash or power loss in the middle of a transaction will very likely corrupt
the database (this is SQLite's documented behavior, not a btreelite
regression).  Applications that need crash safety on such filesystems should
not use btreelite, or must ensure the environment can host the WAL.  The
same applies to in-memory databases, where there is nothing to recover
anyway.

## TODO

- [x] **Phase 4 — the remaining declared-but-unimplemented APIs** — done.
      `btreelite_savepoint()` is implemented (nested `btreelite_begin()`
      acts as the savepoint; releasing the outermost one commits the
      transaction), as are `btreelite_value_write()` (an incremental
      in-place value write that refuses ranges which would grow the value),
      `btreelite_integrity_check()` and `btreelite_mem_used()`.  The
      underlying integrity-check and incremental-blob code is now compiled
      in: the `SQLITE_OMIT_INTEGRITY_CHECK 0` / `SQLITE_OMIT_INCRBLOB 0`
      pseudo-definitions were removed — a macro defined to `0` still counts
      as defined, so both features had been silently compiled out.  The KV
      tree integrity checker no longer applies the rowid-ordering rule,
      since a KV cell's `CellInfo.nKey` holds the key length, not a rowid.
      The acceptance test `test/t_api.c` covers nested savepoints,
      incremental blob writes, integrity checks and memory accounting.
      Every function declared in `include/btreelite.h` now links.
- [x] **In-memory databases (`btreelite_open(NULL)` / `":memory:"`)** —
      fixed.  The third `SQLITE_OMIT_MEMORYDB 0` pseudo-definition had forced
      `isMemdb` to false, so these opens silently created a *literal* file
      named `":memory:"` instead of an in-memory database.  With the macro
      removed both forms open a true memory database (journal mode MEMORY,
      no files created, and the data does not survive `btreelite_close()`)
      as the header documents.
- [x] **Phase 4 concurrency acceptance** — done.  `test/t_proc.c` runs
      *separate processes* (fork + execl, since POSIX record locks cannot be
      exercised by a plain fork): a second process holding a write
      transaction excludes the first (`SQLITE_BUSY` with a zero timeout); a
      WAL writer commits while a parent reader keeps its snapshot
      (reader/writer concurrency), with the new rows visible to a fresh
      reader afterwards; and `btreelite_busy_timeout()` makes a blocked
      writer wait out the peer's hold instead of failing instantly — the
      parent got the lock, wrote and committed after the child released it.
- [x] **`sqlite3BtreeIntegerKey()` dead function** — removed.  It had no
      callers: the KV accessors (`sqlite3BtreeKvKey` et al.) superseded it.
- [x] **Phase 4 durability acceptance** — done.  `btreelite_synchronous()`
      exposes the three crash-safety levels (OFF/NORMAL/FULL) and returns
      the previous level; the default is FULL, matching SQLite.  The
      durability test mixes all three levels across three write batches
      and verifies every row reads back afterwards — changing synchronous
      changes durability, never correctness.  `btreelite_mmap_limit()`
      enables the memory-mapped read path, exercised both on a fresh
      transaction and after a close/reopen cycle that re-establishes the
      mapping.  A crash in the middle of a child's uncommitted transaction
      was verified to lose nothing committed before it (FULL).  See
      `test/t_dur.c`.
- [x] **Multiple trees in one file (acceptance)** — done.  `test/t_trees.c`
      creates several trees in one file, each with a distinct root page, and
      verifies they coexist: one write transaction fills and commits every
      tree (and a rolled-back transaction rewrites two trees and leaves both
      exactly as before), the same key maps to a different value in each
      tree, `btreelite_clear_tree()` empties exactly one tree and its
      siblings stay full, and after a close/reopen cycle every tree's rows
      come back through the caller-persisted root page numbers.

- [x] **Large values spanning overflow pages (varint encoding)** — fixed.
      The overflow path itself was correct; the defect was in
      `putVarint64()` (`src/util.c`), which omitted the `buf[0] &= 0x7f`
      step that clears the continuation bit of a multi-byte varint's
      most-significant byte. Any value of `16384` bytes or more therefore
      encoded its `nValue` header as an extra continuation byte
      (`81 80 80` instead of `81 80 00`), so the leaf parser read a corrupt
      key length and the lookup missed. The 200 KB `t_big` test passes now.
- [x] **Lookup misses when a small value shares a leaf with an over-maxLeaf
      value** — fixed.  The `balance_quick()` fast path built the interior
      divider by copying the *intkey* varint from the right-most cell (it
      skipped the `nPayload` varint and then copied the next one).  For a KV
      cell `[nValue][nKeyLen][key][value]` that copied `nKeyLen` instead of
      the key, so the divider carried a garbage key and lookups descended
      past the small key.  Only leaves with a single trailing overflow cell
      (exactly the `<= maxLeaf` + overflow neighbour shape) took that path,
      which is why values `4060 + 16384` missed while `4062 + 16384` did not.
      Fix: btreelite KV pages now skip the `balance_quick()` fast path and
      route to `balance_nonroot()`, which builds the divider from the real KV
      key (as the redesign doc §4.7 had prescribed).  `t_big` gained a
      regression case covering values straddling `maxLeaf`; it fails without
      the fix and passes with it.
- [x] **Remove the remaining autovacuum scaffolding** — done.  The
      `invalidateAllOverflowCache` helper, the `btreeHeap*` integrity-check
      min-heap, and the autovacuum pointer-map arm of `balance_nonroot` are
      now wrapped in the same `SQLITE_OMIT_AUTOVACUUM` /
      `SQLITE_OMIT_INTEGRITY_CHECK` guards as their callers, and an unused
      `pVfs` in `pager_playback` (left over from the removed super-journal
      replay) was dropped.  A clean build now produces no warnings.
- [x] **Remove the intkey-only code paths** — done.  `decodeFlags` now accepts
      only the two KV page types (0x05, 0x0d) and everything else is
      `CORRUPT`; the table/index cell parsers (`btreeParseCellPtr`,
      `btreeParseCellPtrNoPayload`, `btreeParseCellPtrIndex`), their
      `cellSizePtr*` counterparts, `sqlite3BtreeTableMoveto`,
      `sqlite3BtreeIndexMoveto`, `indexCellCompare`, `sqlite3BtreeTransferRow`
      and the `BTREE_PREFORMAT` / `UnpackedRecord`-based moveto path were
      deleted, along with the record-comparator stubs in
      `btreelite_compat.c`.  About 1,300 lines gone; `make test` still passes.
- [x] **Replace the file magic string** — done.  The file header now begins
      with `"btreelite fmt 1"` instead of `"SQLite format 3"`, so the
      `sqlite3` CLI and library reject a btreelite file with
      "file is not a database", and vice versa.
- [x] **Large values spanning overflow pages (varint encoding)** — fixed.
      The overflow path itself was correct; the defect was in
      `putVarint64()` (`src/util.c`), which omitted the `buf[0] &= 0x7f`
      step that clears the continuation bit of a multi-byte varint's
      most-significant byte. Any value of `16384` bytes or more therefore
      encoded its `nValue` header as an extra continuation byte
      (`81 80 80` instead of `81 80 00`), so the leaf parser read a corrupt
      key length and the lookup missed. The 200 KB `t_big` test passes now.
- [x] **Lookup misses when a small value shares a leaf with an over-maxLeaf
      value** — fixed.  The `balance_quick()` fast path built the interior
      divider by copying the *intkey* varint from the right-most cell (it
      skipped the `nPayload` varint and then copied the next one).  For a KV
      cell `[nValue][nKeyLen][key][value]` that copied `nKeyLen` instead of
      the key, so the divider carried a garbage key and lookups descended
      past the small key.  Only leaves with a single trailing overflow cell
      (exactly the `<= maxLeaf` + overflow neighbour shape) took that path,
      which is why values `4060 + 16384` missed while `4062 + 16384` did not.
      Fix: btreelite KV pages now skip the `balance_quick()` fast path and
      route to `balance_nonroot()`, which builds the divider from the real KV
      key (as the redesign doc §4.7 had prescribed).  `t_big` gained a
      regression case covering values straddling `maxLeaf`; it fails without
      the fix and passes with it.
- [x] **Remove the remaining autovacuum scaffolding** — done.  The
      `invalidateAllOverflowCache` helper, the `btreeHeap*` integrity-check
      min-heap, and the autovacuum pointer-map arm of `balance_nonroot` are
      now wrapped in the same `SQLITE_OMIT_AUTOVACUUM` /
      `SQLITE_OMIT_INTEGRITY_CHECK` guards as their callers, and an unused
      `pVfs` in `pager_playback` (left over from the removed super-journal
      replay) was dropped.  A clean build now produces no warnings.
- [x] **Remove the intkey-only code paths** — done.  `decodeFlags` now accepts
      only the two KV page types (0x05, 0x0d) and everything else is
      `CORRUPT`; the table/index cell parsers (`btreeParseCellPtr`,
      `btreeParseCellPtrNoPayload`, `btreeParseCellPtrIndex`), their
      `cellSizePtr*` counterparts, `sqlite3BtreeTableMoveto`,
      `sqlite3BtreeIndexMoveto`, `indexCellCompare`, `sqlite3BtreeTransferRow`
      and the `BTREE_PREFORMAT` / `UnpackedRecord`-based moveto path were
      deleted, along with the record-comparator stubs in
      `btreelite_compat.c`.  About 1,300 lines gone; `make test` still passes.
- [x] **Replace the file magic string** — done.  The file header now begins
      with `"btreelite fmt 1"` instead of `"SQLite format 3"`, so the
      `sqlite3` CLI and library reject a btreelite file with
      "file is not a database", and vice versa.
- [x] **Wire up WAL (Phase 3)** — done.  `btreelite_journal_mode()` now
      performs the full `PRAGMA journal_mode` transition instead of only
      flipping the pager flag: it refuses mode changes inside a transaction,
      closes (and checkpoints) the log when leaving WAL, reroutes MEMORY→WAL
      through OFF, rewrites the file-header version bytes via
      `sqlite3BtreeSetVersion()`, and then opens the WAL connection eagerly.
      `btreelite_wal_autocheckpoint()` and `btreelite_busy_timeout()` gained
      real implementations.  The journal-mode surface was then narrowed to
      WAL (the default) and MEMORY (the fallback); see the *Journal modes*
      section above.  The acceptance test `test/t_wal.c` covers commits
      through the log, all four checkpoint modes, crash recovery from an
      uncommitted child process, automatic checkpointing, and leaving WAL
      mode.

## License

Derived from SQLite, which is public domain. This project is likewise
released to the public domain.

```
May you do good and not evil.
May you find forgiveness for yourself and forgive others.
May you share freely, never taking more than you give.
```