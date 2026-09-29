# litebtree

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
  identified by its root page number.
- **ACID transactions**: inherited from SQLite's rollback journal and WAL
  crash recovery.
- **Concurrency**: in-process multi-threading plus cross-process file
  locking, inherited from SQLite.
- **File format**: same page/cell layout as SQLite, with a different magic
  string so `sqlite3` CLI tools do not mistake it for a SQL database.
- **Build artifact**: `liblitebtree.a` / `liblitebtree.so` (no third-party
  dependencies). Public API functions are prefixed `litebtree_`.

The design and extraction plan is documented in
[docs/kv-extraction-plan.md](docs/kv-extraction-plan.md).

## Status

Implementation in progress. Phase 0 (build skeleton with SQL-free
compilation), Phase 1 (decoupling the btree/pager/journal stack from the
SQL layer) and Phase 2 (KV cell format plus the public API) are done; see
[docs/phase.md](docs/phase.md) for the plan and
[docs/phase0-report.md](docs/phase0-report.md),
[docs/phase1-report.md](docs/phase1-report.md),
[docs/phase2-report.md](docs/phase2-report.md) for the reports.

The KV cell format was subsequently redesigned to use the table-btree
(leaf-data) page family with a byte-string key instead of the index-btree
record format, so keys are always held locally and lookups never touch
overflow pages.  See
[docs/kv-format-redesign.md](docs/kv-format-redesign.md).  With that in
place `make test` passes in full (`t_smoke`, `t_kv`, `t_big`), including
values larger than one page.

## TODO

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
      flipping the pager flag: it refuses WAL transitions inside a
      transaction, closes (and checkpoints) the log when leaving WAL,
      routes MEMORY→WAL through OFF, rewrites the file-header version
      bytes via `sqlite3BtreeSetVersion()`, and then opens the WAL
      connection eagerly.  `btreelite_wal_autocheckpoint()` and
      `btreelite_busy_timeout()` gained real implementations.  The
      acceptance test `test/t_wal.c` covers commits through the log, all
      four checkpoint modes, crash recovery from an uncommitted child
      process, automatic checkpointing, and leaving WAL mode.

## License

Derived from SQLite, which is public domain. This project is likewise
released to the public domain.

```
May you do good and not evil.
May you find forgiveness for yourself and forgive others.
May you share freely, never taking more than you give.
```