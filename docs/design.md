# btreelite design notes

Design topics that are too detailed for the README live here.  For the
on-disk format see [KV storage format design](kv-format-design.md) (中文:
[KV 存储格式设计](kv-format-design-cn.md)).

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

## Benchmark interpretation

The README carries the measured KV-vs-SQL numbers.  How to read them:

- **Lookups are the story.**  5.5–5.8x: a KV search is a `memcmp` against a
  byte-string key held locally on the page — no record encoding, no
  VDBE-style dispatch.  This is the return on the KV cell format.
- **Overwrites and deletes run 1.30–1.48x faster**, for the same reason:
  no record decode on the way to the payload.
- **Inserts are on par, not behind.**  `btreelite_put` hands the btree the
  `BTREE_APPEND` search hint, which is what SQLite's own `OPFLAG_APPEND`
  does for auto-generated rowids: the locate is biased toward the highest
  cell, so monotone key traffic -- the common KV case -- lands on the
  append fast path.  The hint is safe for keys in any order (it only
  changes the first probe of the binary search, and the insertion logic is
  symmetric in the returned seek result); measured throughput for sorted,
  descending and random key orders with the hint on: ascending +29%,
  descending and random within noise of the unhinted baseline.
  `put` keeps a final reposition afterwards, because "cursor at the stored
  entry" is what the value accessors (`value_size` / `value_read` /
  `value_fetch`) are built on and `sqlite3BtreeInsert` leaves the cursor in
  an arbitrary state once a balance has run.
