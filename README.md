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

- [x] **Large values spanning overflow pages** — fixed. The overflow path
      itself was correct; the defect was in `putVarint64()` (`src/util.c`),
      which omitted the `buf[0] &= 0x7f` step that clears the continuation
      bit of a multi-byte varint's most-significant byte. Any value of
      `16384` bytes or more therefore encoded its `nValue` header as an
      extra continuation byte (`81 80 80` instead of `81 80 00`), so the
      leaf parser read a corrupt key length and the lookup missed. The
      200 KB `t_big` test passes now.
- [ ] **Remove the remaining autovacuum scaffolding** — `SQLITE_OMIT_AUTOVACUUM`
      is defined, so this is unused-symbol and warning cleanup only.
- [ ] **Remove the intkey-only code paths** — `btreeParseCellPtr`,
      `sqlite3BtreeTableMoveto`, `BTREE_PREFORMAT`,
      `sqlite3BtreeTransferRow` and friends have no callers in a KV-only
      build. Deletion is mechanical (about 800 lines).
- [x] **Replace the file magic string** — done.  The file header now begins
      with `"btreelite fmt 1"` instead of `"SQLite format 3"`, so the
      `sqlite3` CLI and library reject a btreelite file with
      "file is not a database", and vice versa.

## License

Derived from SQLite, which is public domain. This project is likewise
released to the public domain.

```
May you do good and not evil.
May you find forgiveness for yourself and forgive others.
May you share freely, never taking more than you give.
```