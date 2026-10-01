# btreelite examples

Small, self-contained programs that demonstrate the public API.  Each one
links against `libbtreelite.a` and creates its database, runs, prints what
it does, and removes the file on the way out.

Build them all from the repository root:

```sh
make examples
```

and run each:

```sh
./examples/kv_demo
./examples/txn_demo
./examples/tree_demo
```

| Program | What it demonstrates |
|---|---|
| `kv_demo.c` | The core key-value lifecycle: open, create a tree, `put`/`get`/`del`, a forward scan, a range `seek`, and a value larger than one page (overflow pages read back through the streaming value API). |
| `txn_demo.c` | Transactions: atomicity across a rollback, nested savepoints with a partial `ROLLBACK TO`, `RELEASE` of the outermost savepoint to commit, the `synchronous` / `mmap` knobs, and durability across a reopen. |
| `tree_demo.c` | Several independent trees in one file (each identified by its root page), `clear_tree` on one tree only, `integrity_check`, a WAL `checkpoint`, the automatic-checkpoint threshold, and an in-memory database via `btreelite_open(NULL)`. |

To build a single one by hand:

```sh
cc -Iinclude -Isrc examples/kv_demo.c libbtreelite.a -o kv_demo
```

See `include/btreelite.h` for the full API and `README.md` at the repository
root for what the engine is and how it performs.
