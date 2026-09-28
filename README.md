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

## TODO

- [ ] **Large values spanning overflow pages** — `put`/`get` of a value
      larger than one page returns `CORRUPT`. The insert itself succeeds;
      the failure is in the following `IndexMoveto` reposition over the
      overflow chain (btree.c around the accessPayload/malloc branch,
      page 2). Suspected cause: the overflow-chain page numbers and
      `BtShared.nPage` are transiently inconsistent after a balance
      reorder. Fix requires auditing the `nPage` maintenance ordering
      across `fillInCell` and `balance_nonroot`.
- [ ] **Remove the remaining autovacuum scaffolding** — `SQLITE_OMIT_AUTOVACUUM`
      is defined, so this is unused-symbol and warning cleanup only.
- [ ] **Remove the intkey-only code paths** — `btreeParseCellPtr`,
      `sqlite3BtreeTableMoveto`, `BTREE_PREFORMAT`,
      `sqlite3BtreeTransferRow` and friends have no callers in a KV-only
      build. Deletion is mechanical (about 800 lines).
- [ ] **Replace the file magic string** — the header still carries SQLite's
      `"SQLite format 3"`; switch it to a project-specific string once the
      KV cell format is frozen, so `sqlite3` CLI tools reject the file.

## License

Derived from SQLite, which is public domain. This project is likewise
released to the public domain.

```
May you do good and not evil.
May you find forgiveness for yourself and forgive others.
May you share freely, never taking more than you give.
```