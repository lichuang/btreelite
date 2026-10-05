/*
** btreelite.h — Public API for the btreelite key-value storage engine.
**
** btreelite is a standalone key-value storage engine derived from SQLite's
** storage subsystem (btree + pager + WAL) with the SQL layer removed.
**
** Keys are arbitrary byte strings ordered by binary comparison (memcmp).
** Values are arbitrary byte strings.  One database file can contain many
** independent btrees, each identified by its root page number.
**
** ACID transactions, rollback-journal and WAL crash recovery, and
** cross-process file locking are all inherited from SQLite unchanged.
**
** Threading: a btreelite_db handle and every cursor opened on it are for
** use by one thread at a time (btreelite serializes access with its
** internal mutex, so a second thread's calls return BTREELITE_BUSY rather
** than corrupting state, but a contended handle is still program-error
** territory).  Distinct handles -- distinct files, or each thread's own
** ":memory:" database -- may be used from distinct threads concurrently.
**
** The source is public domain.  Derived from SQLite, which is likewise
** public domain.
*/
#ifndef BTREELITE_H
#define BTREELITE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes are the standard SQLite result codes (SQLITE_OK == 0,
** SQLITE_BUSY, SQLITE_CORRUPT, SQLITE_NOMEM, ...).  Define the subset the
** public API can actually return here so that applications do not need
** sqlite3.h.
*/
#define BTREELITE_OK             0   /* Successful result */
#define BTREELITE_ERROR          1   /* Generic error */
#define BTREELITE_BUSY           5   /* The database file is locked */
#define BTREELITE_READONLY       8   /* Attempt to write a readonly database */
#define BTREELITE_IOERR         10   /* Some kind of disk I/O error occurred */
#define BTREELITE_CORRUPT       11   /* The database disk image is malformed */
#define BTREELITE_FULL          13   /* Insertion failed because database is full */
#define BTREELITE_CANTOPEN      14   /* Unable to open the database file */
#define BTREELITE_NOTFOUND      12   /* (query-only) No such key */
#define BTREELITE_NOMEM         7    /* Out of memory */
#define BTREELITE_INTERRUPT     9    /* Operation terminated by interrupt */
#define BTREELITE_BUSY_SNAPSHOT 517  /* WAL: database changed under us */
#define BTREELITE_TOOBIG        18   /* Key exceeds BTREELITE_MAX_KEY */
#define BTREELITE_DONE          101  /* Cursor is at the end/beginning */

/*
** Maximum key length, in bytes.  Keys are stored inline on the btree page
** (they never spill to overflow pages), so their length is bounded by the
** free space in a leaf cell.  A key longer than this is rejected with
** BTREELITE_TOOBIG rather than corrupting the page.
*/
#define BTREELITE_MAX_KEY 255

typedef struct btreelite_db btreelite_db;     /* opaque database handle */
typedef struct btreelite_cur btreelite_cur;   /* opaque cursor handle */

/*
** Checkpoint modes (same numbering as SQLITE_CHECKPOINT_*).
*/
#define BTREELITE_CHECKPOINT_PASSIVE  0
#define BTREELITE_CHECKPOINT_FULL     1
#define BTREELITE_CHECKPOINT_RESTART  2
#define BTREELITE_CHECKPOINT_TRUNCATE 3

/*
** Savepoint operations.
*/
#define BTREELITE_SAVEPOINT_RELEASE   1
#define BTREELITE_SAVEPOINT_ROLLBACK  2

/* ---------------------------------------------------------------------- */
/* Database lifecycle                                                     */
/* ---------------------------------------------------------------------- */

/*
** Open the database file named zPath.  If zPath is NULL, an in-memory
** database is created that is destroyed by btreelite_close().
**
** On success *ppDb receives the handle and BTREELITE_OK is returned.
** Otherwise *ppDb is NULL and an error code is returned.
*/
int btreelite_open(const char *zPath, btreelite_db **ppDb);

/* Close the database.  Active transactions are rolled back. */
void btreelite_close(btreelite_db *p);

/* ---------------------------------------------------------------------- */
/* Trees                                                                  */
/* ---------------------------------------------------------------------- */

/*
** Create a new empty tree inside the database.  On success *piRoot receives
** the root page number of the new tree, which the application is responsible
** for persisting.  A write transaction must be open.
*/
int btreelite_create_tree(btreelite_db *p, unsigned *piRoot);

/*
** Drop every entry of the tree rooted at iRoot.  The root page itself
** survives (the tree becomes empty).  A write transaction must be open.
*/
int btreelite_clear_tree(btreelite_db *p, unsigned iRoot);

/* Return the total number of pages in the database file. */
unsigned btreelite_page_count(btreelite_db *p);

/* ---------------------------------------------------------------------- */
/* Transactions                                                           */
/* ---------------------------------------------------------------------- */

/*
** Begin a transaction.  wrflag: 0 = read-only, 1 = write, 2 = write with
** exclusive access (BEGIN EXCLUSIVE semantics).
*/
int btreelite_begin(btreelite_db *p, int wrflag);

/* Commit the current write transaction (two-phase internally). */
int btreelite_commit(btreelite_db *p);

/* Roll back the current write transaction. */
int btreelite_rollback(btreelite_db *p);

/*
** Release (BTREELITE_SAVEPOINT_RELEASE) or roll back to
** (BTREELITE_SAVEPOINT_ROLLBACK) the savepoint identified by iSavepoint,
** counting from 0 for the outermost savepoint.  Savepoints are created
** implicitly as nested btreelite_begin() calls within a write transaction.
**
** ROLLBACK TO i undoes everything written after savepoint i was
** established and keeps i open; savepoints nested inside i are destroyed.
** RELEASE i merges i's changes into its parent and destroys i together
** with everything nested inside it.
**
** Savepoint 0 is the write transaction itself: RELEASE 0 commits the
** transaction, and ROLLBACK TO 0 undoes everything the transaction has
** written while leaving the transaction open.
*/
int btreelite_savepoint(btreelite_db *p, int op, int iSavepoint);

/* Query the transaction state: 0=none, 1=read, 2=write. */
int btreelite_txn_state(btreelite_db *p);

/*
** Request that the current integrity check abort with BTREELITE_INTERRUPT.
** The interrupt flag is consulted only by btreelite_integrity_check
** (and the underlying whole-tree count): set it before, or concurrently
** with, such a check.  get/put/next traffic does not poll the flag and is
** not interruptible.
*/
void btreelite_set_interrupt(btreelite_db *p);

/* Configure busy timeout in milliseconds (0 = default, negative = none). */
void btreelite_busy_timeout(btreelite_db *p, int ms);

/*
** Synchronization levels for btreelite_synchronous().  Values match
** SQLite's PRAGMA synchronous.
**
** In WAL (the default) mode OFF issues no fsync() at all; NORMAL syncs the
** log at checkpoints only, so a power loss may roll back the last few
** committed transactions but cannot corrupt the database; FULL additionally
** syncs after every commit, making transactions durable.  The btreelite
** default is FULL, matching SQLite.
*/
#define BTREELITE_SYNC_OFF     0   /* no fsync: fastest, not crash-safe */
#define BTREELITE_SYNC_NORMAL  1   /* fsync at checkpoint only */
#define BTREELITE_SYNC_FULL    2   /* fsync on every commit (the default) */

/*
** Select the synchronization level (a BTREELITE_SYNC_* value) used when
** committing transactions and checkpoints.  Returns the previously set
** level.  The default is BTREELITE_SYNC_FULL, matching SQLite's default.
*/
int btreelite_synchronous(btreelite_db *p, int level);

/*
** Set the number of bytes of a file-backed database that may be memory
** mapped (0 disables mmap, the default).  Reading pages through the mapped
** region avoids read() calls entirely; this affects read performance only,
** and never changes the file contents.  Passing a negative value makes the
** setting a no-op.
*/
void btreelite_mmap_limit(btreelite_db *p, long nLimit);

/* ---------------------------------------------------------------------- */
/* Cursors                                                                */
/* ---------------------------------------------------------------------- */

/*
** Open a cursor on the tree rooted at iRoot.  wrFlag must be 0 for a
** read-only cursor or 1 for a read-write cursor (a write transaction must
** be open for the latter).  *ppCur receives the cursor; it must be released
** with btreelite_cursor_close().
**
** Root pages come from btreelite_create_tree() and the application keeps
** the numbers on its own.  A root number that was never obtained that way
** (or that no longer refers to a tree) still opens without error, but the
** first cursor operation fails with BTREELITE_CORRUPT and every later
** operation fails -- treat the returned error and destroy the cursor.
*/
int btreelite_cursor_open(btreelite_db *p, unsigned iRoot, int wrFlag,
                          btreelite_cur **ppCur);

/* Close a cursor.  A NULL pointer is a harmless no-op. */
void btreelite_cursor_close(btreelite_cur *c);

/* ---------------------------------------------------------------------- */
/* CRUD                                                                   */
/* ---------------------------------------------------------------------- */

/*
** Position cursor c on the entry whose key equals (k,nK).
** Returns BTREELITE_OK if found, BTREELITE_NOTFOUND if absent,
** BTREELITE_TOOBIG if nK exceeds BTREELITE_MAX_KEY.
*/
int btreelite_get(btreelite_cur *c, const void *k, int nK);

/*
** Insert or overwrite the entry (k,nK) with value (v,nV).
** nV may be 0 (value is an empty string).  Returns BTREELITE_TOOBIG if nK
** exceeds BTREELITE_MAX_KEY.  A write transaction must be open and c must
** be a write cursor; otherwise BTREELITE_ERROR is returned.
*/
int btreelite_put(btreelite_cur *c, const void *k, int nK,
                  const void *v, int nV);

/*
** Delete the entry whose key equals (k,nK).  A missing key is not an error.
** A write transaction must be open and c must be a write cursor.
*/
int btreelite_del(btreelite_cur *c, const void *k, int nK);

/* ---------------------------------------------------------------------- */
/* Cursor movement                                                        */
/* ---------------------------------------------------------------------- */

/*
** Move the cursor to the first / last entry.  *pRes is 0 if the cursor
** points to an entry, 1 if the tree is empty.
*/
int btreelite_first(btreelite_cur *c, int *pRes);
int btreelite_last(btreelite_cur *c, int *pRes);

/*
** Advance / retreat the cursor.  Return BTREELITE_OK on a valid entry,
** BTREELITE_DONE when moving past the last / first entry.
*/
int btreelite_next(btreelite_cur *c);
int btreelite_prev(btreelite_cur *c);

/*
** Move the cursor to the first entry whose key is greater than or equal
** to (k,nK).  *pRes is 0 if such an entry exists, 1 if (k,nK) sorts after
** every entry in the tree (cursor is then at "end").
*/
int btreelite_seek(btreelite_cur *c, const void *k, int nK);

/* Non-zero if the cursor is not pointing at an entry. */
int btreelite_eof(btreelite_cur *c);

/* ---------------------------------------------------------------------- */
/* Reading the current entry                                              */
/* ---------------------------------------------------------------------- */

/*
** Copy the key of the current entry into buf (up to nMax bytes).
** The full key length is stored in *pnKey (may exceed nMax).
** Returns BTREELITE_OK on success, BTREELITE_ERROR if the cursor is EOF.
*/
int btreelite_key(btreelite_cur *c, void *buf, int nMax, int *pnLen);

/*
** Copy up to nMax bytes of the value of the current entry into buf,
** starting at offset.  The full value length is stored in *pnVal.
** (Equivalent of sqlite3_blob_read; handles values spanning overflow
** pages transparently.)
*/
int btreelite_value_read(btreelite_cur *c, uint32_t offset, uint32_t amt,
                         void *pBuf);

/* Total byte length of the value of the current entry. */
int btreelite_value_size(btreelite_cur *c, uint32_t *pnVal);

/*
** Return a pointer to as many bytes of the value as are stored locally on
** the current page (fast path for small values).  The number of valid
** bytes is stored in *pAmt.  The pointer is only valid until the next
** btreelite_* call on this cursor.
*/
const void *btreelite_value_fetch(btreelite_cur *c, int *pAmt);

/*
** Overwrite a range of the value of the current entry in place.
** The cursor must point at an entry whose key equals (k,nK) and the range
** [offset, offset+amt) must lie inside the existing value.  (The
** incremental-blob-write mechanism of SQLite.)  A write transaction must
** be open and c must be a write cursor.
*/
int btreelite_value_write(btreelite_cur *c, const void *k, int nK,
                          uint32_t offset, uint32_t amt, const void *z);

/* ---------------------------------------------------------------------- */
/* WAL and maintenance                                                    */
/* ---------------------------------------------------------------------- */

/*
** Run a WAL checkpoint.  eMode is one of the BTREELITE_CHECKPOINT_*
** constants.  pnLog/pnCkpt (if non-NULL) receive the number of frames in
** the WAL and the number of frames backfilled into the database.
*/
int btreelite_checkpoint(btreelite_db *p, int eMode, int *pnLog, int *pnCkpt);

/*
** Run an integrity check over the tree rooted at iRoot (or over the whole
** file if iRoot is 0).  Returns BTREELITE_OK if no corruption was found,
** BTREELITE_CORRUPT otherwise with *pnErr error messages written into a
** string allocated by the library and returned in *pzOut (free with
** btreelite_free()).
**
** If no transaction is open, the check runs in a read transaction that is
** opened and closed by this call.  If the caller already holds a read or
** write transaction, the check runs inside it (seeing uncommitted changes)
** and the caller's transaction is left untouched.
*/
int btreelite_integrity_check(btreelite_db *p, unsigned iRoot, int mxErr,
                              int *pnErr, char **pzOut);

/*
** Journal modes for btreelite_journal_mode().  btreelite offers exactly two:
**
**   BTREELITE_JOURNAL_WAL     write-ahead logging: crash-safe, durable
**                             (synchronous=FULL by default) and the default
**                             for every file-backed database;
**   BTREELITE_JOURNAL_MEMORY  the rollback journal is held in RAM.  This is
**                             used automatically for in-memory databases and
**                             when the environment cannot support WAL, and it
**                             is NOT crash-safe.
**
** Values match SQLite's PRAGMA journal_mode.  A WAL transition must be made
** outside a transaction.
*/
#define BTREELITE_JOURNAL_MEMORY   4   /* In-memory journal: not crash-safe */
#define BTREELITE_JOURNAL_WAL      5   /* Write-ahead log (the default) */

/*
** Select the journal mode at run time.  eMode must be BTREELITE_JOURNAL_WAL
** or BTREELITE_JOURNAL_MEMORY; any other value is rejected with
** BTREELITE_ERROR.  Returns the resulting mode, which is MEMORY rather than
** WAL when the database is in memory or the environment cannot support WAL.
*/
int btreelite_journal_mode(btreelite_db *p, int eMode);

/* Enable or disable automatic checkpointing after every N frames. */
void btreelite_wal_autocheckpoint(btreelite_db *p, int nFrames);

/* ---------------------------------------------------------------------- */
/* Memory                                                                 */
/* ---------------------------------------------------------------------- */

/*
** Free a string returned by btreelite_integrity_check() (or any other
** buffer the library has allocated on the caller's behalf).
*/
void btreelite_free(void *p);

/*
** Return the number of bytes of heap currently acquired through the
** library's allocator.  The counter is process-wide (malloc.c tracks a
** single running total for all handles), so with several open databases
** the number returned for any one handle is the sum over all of them.
*/
int btreelite_mem_used(btreelite_db *p);

#ifdef __cplusplus
}
#endif
#endif /* BTREELITE_H */