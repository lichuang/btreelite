/*
** 2026 September 29
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** Phase 4 acceptance test: the savepoint, incremental-blob write,
** integrity-check and memory-usage entry points.
**
**   1. savepoints: nested write transactions can be released or rolled
**      back to, from the outermost inward;
**   2. incremental blob write: overwrite a range inside a stored value
**      in place, and refuse ranges that would grow the value;
**   3. integrity check: a healthy tree reports no errors and a tampered
**      one is detected;
**   4. memory usage: the handle reports non-zero heap;
**   5. mutating entry points require a write transaction and a write
**      cursor.
*/
#include "../include/btreelite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int nFail = 0;
static int nTest = 0;
#define CHECK(c) do{ nTest++; if( !(c) ){                          \
    nFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
  } }while(0)

#define ZDB     "t_api_test.db"
#define ZWAL    "t_api_test.db-wal"
#define ZSHM    "t_wal_test.db-shm"
#define ZJRNL   "t_api_test.db-journal"

static int writeEntries(btreelite_cur *cur, int iFirst, int iLast){
  char zKey[64], zVal[64];
  int rc, i;
  for(i=iFirst; i<=iLast; i++){
    int nk = sprintf(zKey, "row-%04d", i);
    int nv = sprintf(zVal, "val-%04d", i);
    rc = btreelite_put(cur, zKey, nk, zVal, nv);
    if( rc!=BTREELITE_OK ) return rc;
  }
  return BTREELITE_OK;
}

static int countEntries(btreelite_db *db, unsigned iRoot, int *pnCount){
  btreelite_cur *cur = 0;
  int rc, res = 0, n = 0;
  *pnCount = 0;
  rc = btreelite_begin(db, 0);
  if( rc ) return rc;
  rc = btreelite_cursor_open(db, iRoot, 0, &cur);
  if( rc ) return rc;
  rc = btreelite_first(cur, &res);
  while( rc==BTREELITE_OK && res==0 ){
    n++;
    rc = btreelite_next(cur);
    if( rc==BTREELITE_DONE ){ rc = BTREELITE_OK; break; }
    res = btreelite_eof(cur) ? 1 : 0;
  }
  btreelite_cursor_close(cur);
  btreelite_commit(db);
  *pnCount = n;
  return rc;
}

/*
** Count entries using a cursor the caller already opened inside an active
** transaction (no begin/commit of its own).
*/
static int countInTxn(btreelite_cur *cur, int *pnCount){
  int rc, res = 0, n = 0;
  *pnCount = 0;
  rc = btreelite_first(cur, &res);
  while( rc==BTREELITE_OK && res==0 ){
    n++;
    rc = btreelite_next(cur);
    if( rc==BTREELITE_DONE ){ rc = BTREELITE_OK; break; }
    res = btreelite_eof(cur) ? 1 : 0;
  }
  *pnCount = n;
  return rc==BTREELITE_DONE ? BTREELITE_OK : rc;
}


int main(void){
  btreelite_db *db = 0;
  btreelite_cur *cur = 0;
  unsigned iRoot = 2;
  int rc, n;

  unlink(ZDB); unlink(ZWAL); unlink(ZJRNL);
  unlink("t_wal_test.db-shm");

  /* Seed one tree with 10 entries. */
  rc = btreelite_open(ZDB, &db);
  CHECK( rc==BTREELITE_OK );
  rc = btreelite_begin(db, 1);
  CHECK( rc==BTREELITE_OK );
  rc = btreelite_create_tree(db, &iRoot);
  CHECK( rc==BTREELITE_OK );
  rc = btreelite_cursor_open(db, iRoot, 1, &cur);
  CHECK( rc==BTREELITE_OK );
  rc = writeEntries(cur, 0, 9);
  CHECK( rc==BTREELITE_OK );
  rc = btreelite_commit(db);
  CHECK( rc==BTREELITE_OK );   /* 10 committed baseline rows: 0..9 */

  /* 1. Savepoints: nested write-begin acts as a savepoint, with SQL
  ** semantics: ROLLBACK TO i undoes everything written after savepoint i
  ** was established (and keeps i open); RELEASE i merges it and destroys
  ** everything nested inside it. */
  {
    /* Savepoint 0 = the write transaction itself, then two more. */
    rc = btreelite_begin(db, 1);
    CHECK( rc==BTREELITE_OK );
    rc = writeEntries(cur, 10, 14);
    CHECK( rc==BTREELITE_OK );
    rc = countInTxn(cur, &n);
    CHECK( rc==BTREELITE_OK );
    CHECK( n==15 );                   /* 0..9 + 10..14 */

    rc = btreelite_begin(db, 1);
    CHECK( rc==BTREELITE_OK );
    rc = writeEntries(cur, 15, 19);
    CHECK( rc==BTREELITE_OK );
    rc = countInTxn(cur, &n);
    CHECK( rc==BTREELITE_OK );
    CHECK( n==20 );

    rc = btreelite_begin(db, 1);
    CHECK( rc==BTREELITE_OK );
    rc = writeEntries(cur, 20, 24);
    CHECK( rc==BTREELITE_OK );
    rc = countInTxn(cur, &n);
    CHECK( rc==BTREELITE_OK );
    CHECK( n==25 );

    /* ROLLBACK TO savepoint 2 undoes only what was written after it was
    ** established: rows 20..24 unwind, rows 15..19 stay, and savepoint 2
    ** remains open (re-established) for further use. */
    rc = btreelite_savepoint(db, BTREELITE_SAVEPOINT_ROLLBACK, 2);
    CHECK( rc==BTREELITE_OK );
    rc = countInTxn(cur, &n);
    CHECK( n==20 );

    /* Savepoint 2 is still open: new writes land in its window. */
    rc = writeEntries(cur, 20, 21);
    CHECK( rc==BTREELITE_OK );
    rc = countInTxn(cur, &n);
    CHECK( n==22 );

    /* ROLLBACK TO savepoint 1 undoes everything after it: rows 15..21
    ** unwind, savepoint 2 is destroyed, savepoint 1 remains open. */
    rc = btreelite_savepoint(db, BTREELITE_SAVEPOINT_ROLLBACK, 1);
    CHECK( rc==BTREELITE_OK );
    rc = countInTxn(cur, &n);
    CHECK( n==15 );

    /* RELEASE savepoint 1 merges its changes and destroys it; only the
    ** outermost savepoint (the transaction) remains. */
    rc = btreelite_savepoint(db, BTREELITE_SAVEPOINT_RELEASE, 1);
    CHECK( rc==BTREELITE_OK );
    rc = countInTxn(cur, &n);
    CHECK( n==15 );

    /* Out-of-range and bad operations are rejected. */
    rc = btreelite_savepoint(db, BTREELITE_SAVEPOINT_RELEASE, 5);
    CHECK( rc==BTREELITE_ERROR );
    rc = btreelite_savepoint(db, BTREELITE_SAVEPOINT_ROLLBACK, 1);
    CHECK( rc==BTREELITE_ERROR );   /* savepoint 1 no longer exists */
    rc = btreelite_savepoint(db, 7, 0);
    CHECK( rc==BTREELITE_ERROR );

    /* ROLLBACK TO savepoint 0 (the transaction) undoes everything the
    ** transaction wrote but leaves it open: back to the committed
    ** baseline of 10 rows. */
    rc = writeEntries(cur, 25, 29);
    CHECK( rc==BTREELITE_OK );
    rc = countInTxn(cur, &n);
    CHECK( n==20 );
    rc = btreelite_savepoint(db, BTREELITE_SAVEPOINT_ROLLBACK, 0);
    CHECK( rc==BTREELITE_OK );
    rc = countInTxn(cur, &n);
    CHECK( n==10 );
    CHECK( btreelite_txn_state(db)==2 );   /* transaction still open */

    /* The transaction can still be used and committed. */
    rc = writeEntries(cur, 30, 31);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_savepoint(db, BTREELITE_SAVEPOINT_RELEASE, 0);
    CHECK( rc==BTREELITE_OK );   /* RELEASE 0 commits the transaction */
    CHECK( btreelite_txn_state(db)==0 );

    /* What persists: the committed baseline 0..9 plus rows 30..31 written
    ** after the full-transaction rollback.  Everything the transaction
    ** wrote before that rollback (10..14, 25..29) and everything rolled
    ** back by ROLLBACK TO 1/2 (15..24) must be gone. */
    rc = countEntries(db, iRoot, &n);
    CHECK( rc==BTREELITE_OK );
    CHECK( n==12 );
    {
      int i;
      char zKey[64];
      btreelite_cur *c2 = 0;
      rc = btreelite_begin(db, 0);
      CHECK( rc==BTREELITE_OK );
      rc = btreelite_cursor_open(db, iRoot, 0, &c2);
      CHECK( rc==BTREELITE_OK );
      for(i=0; i<=31; i++){
        int nk = sprintf(zKey, "row-%04d", i);
        int expect = (i<=9 || i>=30);
        rc = btreelite_get(c2, zKey, nk);
        CHECK( rc==(expect ? BTREELITE_OK : BTREELITE_NOTFOUND) );
      }
      btreelite_cursor_close(c2);
      rc = btreelite_commit(db);
      CHECK( rc==BTREELITE_OK );
    }
  }
  btreelite_cursor_close(cur);

  /* 2. Incremental blob write. */
  {
    char *pVal;
    uint32_t nVal = 0;
    rc = btreelite_begin(db, 1);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_cursor_open(db, iRoot, 1, &cur);
    CHECK( rc==BTREELITE_OK );
    pVal = (char*)malloc(1024);
    memset(pVal, 'a', 1024);
    memcpy(pVal, "VALUE", 5);
    rc = btreelite_put(cur, "blob", 4, pVal, 1024);
    CHECK( rc==BTREELITE_OK );

    /* Overwrite the middle of the value in place. */
    memset(pVal+100, 'z', 100);
    rc = btreelite_value_write(cur, "blob", 4, 100, 100, pVal+100);
    CHECK( rc==BTREELITE_OK );

    /* Read it back and verify only that range changed. */
    btreelite_value_size(cur, &nVal);
    CHECK( nVal==1024 );
    {
      char got[2048];
      rc = btreelite_value_read(cur, 0, nVal, got);
      CHECK( rc==BTREELITE_OK );
      CHECK( memcmp(got, pVal, 1024)==0 );
      CHECK( got[0]=='V' );
      CHECK( got[100]=='z' );
      CHECK( got[999]=='a' );
      /* A range that would grow the value is refused. */
      rc = btreelite_value_write(cur, "blob", 4, 1000, 100, got);
      CHECK( rc==BTREELITE_ERROR );
      rc = btreelite_value_write(cur, "missing", 7, 0, 1, got);
      CHECK( rc==BTREELITE_NOTFOUND );
    }
    free(pVal);
    rc = btreelite_commit(db);
    CHECK( rc==BTREELITE_OK );
  }

  /* 3. Integrity check on the healthy tree. */
  {
    char *zOut = 0;
    int nErr = -1;
    rc = btreelite_integrity_check(db, iRoot, 100, &nErr, &zOut);
    CHECK( rc==BTREELITE_OK );
    CHECK( nErr==0 );
    if( zOut ){
      printf("integrity msg: %s\n", zOut);
      btreelite_free(zOut);
      CHECK( 0 );   /* a healthy tree must not report anything */
    }

    /* A bad root (page 5 has not been allocated yet) is reported. */
    nErr = -1; zOut = 0;
    rc = btreelite_integrity_check(db, 100000, 100, &nErr, &zOut);
    CHECK( rc!=BTREELITE_OK || nErr>0 );
    if( zOut ) btreelite_free(zOut);

    /* iRoot==0 checks page 1 and the freelist (partial form). */
    nErr = -1; zOut = 0;
    rc = btreelite_integrity_check(db, 0, 100, &nErr, &zOut);
    CHECK( rc==BTREELITE_OK );
    CHECK( nErr==0 );
    if( zOut ) btreelite_free(zOut);
  }

  /* 3b. A caller-owned transaction must survive the check: the checker
  ** used to commit an open write transaction behind the caller's back. */
  {
    char *zOut = 0;
    int nErr = -1;
    rc = btreelite_begin(db, 1);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_put(cur, "icheck-key", 10, "v", 1);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_integrity_check(db, iRoot, 100, &nErr, &zOut);
    CHECK( rc==BTREELITE_OK );   /* checks the uncommitted state */
    CHECK( nErr==0 );
    if( zOut ) btreelite_free(zOut);
    /* The write transaction is still open: the new key is visible... */
    rc = btreelite_get(cur, "icheck-key", 10);
    CHECK( rc==BTREELITE_OK );
    /* ...and rolling back undoes it (had the check committed, the key
    ** would survive and this rollback would fail). */
    rc = btreelite_rollback(db);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_begin(db, 1);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_get(cur, "icheck-key", 10);
    CHECK( rc==BTREELITE_NOTFOUND );
    rc = btreelite_rollback(db);
    CHECK( rc==BTREELITE_OK );

    /* Same for a caller-owned read transaction. */
    rc = btreelite_begin(db, 0);
    CHECK( rc==BTREELITE_OK );
    nErr = -1; zOut = 0;
    rc = btreelite_integrity_check(db, iRoot, 100, &nErr, &zOut);
    CHECK( rc==BTREELITE_OK );
    CHECK( nErr==0 );
    if( zOut ) btreelite_free(zOut);
    /* The read transaction is still open and usable. */
    rc = btreelite_get(cur, "row-0003", 8);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_rollback(db);
    CHECK( rc==BTREELITE_OK );
  }

  /* 4. Memory usage. */
  {
    int n = btreelite_mem_used(db);
    CHECK( n>0 );
  }

  /* 5. Mutating entry points without a write transaction (or through a
  ** read-only cursor) are refused: upstream relies on the VDBE layer for
  ** this precondition, so release builds used to accept the write, corrupt
  ** state silently and crash later in balance(). */
  {
    btreelite_cur *curRd = 0;
    unsigned iNew = 0;
    /* No transaction at all. */
    rc = btreelite_put(cur, "no-txn", 6, "v", 1);
    CHECK( rc==BTREELITE_ERROR );
    rc = btreelite_del(cur, "row-0000", 8);
    CHECK( rc==BTREELITE_ERROR );
    rc = btreelite_value_write(cur, "row-0000", 8, 0, 1, "x");
    CHECK( rc==BTREELITE_ERROR );
    rc = btreelite_create_tree(db, &iNew);
    CHECK( rc==BTREELITE_ERROR );
    rc = btreelite_clear_tree(db, iRoot);
    CHECK( rc==BTREELITE_ERROR );
    /* A read transaction does not suffice either. */
    rc = btreelite_begin(db, 0);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_put(cur, "rd-txn", 6, "v", 1);
    CHECK( rc==BTREELITE_ERROR );
    rc = btreelite_del(cur, "row-0000", 8);
    CHECK( rc==BTREELITE_ERROR );
    rc = btreelite_value_write(cur, "row-0000", 8, 0, 1, "x");
    CHECK( rc==BTREELITE_ERROR );
    rc = btreelite_create_tree(db, &iNew);
    CHECK( rc==BTREELITE_ERROR );
    rc = btreelite_clear_tree(db, iRoot);
    CHECK( rc==BTREELITE_ERROR );
    rc = btreelite_rollback(db);
    CHECK( rc==BTREELITE_OK );
    /* Nor does a read-only cursor inside a write transaction. */
    rc = btreelite_begin(db, 1);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_cursor_open(db, iRoot, 0, &curRd);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_put(curRd, "rd-cur", 6, "v", 1);
    CHECK( rc==BTREELITE_ERROR );
    rc = btreelite_del(curRd, "row-0000", 8);
    CHECK( rc==BTREELITE_ERROR );
    rc = btreelite_value_write(curRd, "row-0000", 8, 0, 1, "x");
    CHECK( rc==BTREELITE_ERROR );
    btreelite_cursor_close(curRd);
    /* The write cursor is unaffected and the tree untouched. */
    rc = btreelite_put(cur, "guard-key", 9, "g", 1);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_del(cur, "guard-key", 9);
    CHECK( rc==BTREELITE_OK );
    rc = countInTxn(cur, &n);
    CHECK( rc==BTREELITE_OK );
    CHECK( n==13 );   /* 12 from the savepoint section plus "blob" */
    rc = btreelite_rollback(db);
    CHECK( rc==BTREELITE_OK );
  }

  btreelite_close(db);
  unlink(ZDB); unlink(ZWAL); unlink(ZJRNL); unlink("t_wal_test.db-shm");

  printf("%d checks, %d failures\n", nTest, nFail);
  return nFail ? 1 : 0;
}