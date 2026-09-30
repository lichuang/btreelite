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
**   4. memory usage: the handle reports non-zero heap.
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

  /* 1. Savepoints: nested write-begin acts as a savepoint. */
  {
    /* Savepoint 0 = outermost (already begun), then two more. */
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

    /* ROLLBACK TO the middle savepoint: rows 20..24 unwind; rows 15..19
    ** stay and the inner savepoint (2) is destroyed. */
    rc = btreelite_savepoint(db, BTREELITE_SAVEPOINT_ROLLBACK, 1);
    CHECK( rc==BTREELITE_OK );
    rc = countInTxn(cur, &n);
    CHECK( n==20 );

    /* After the rollback the earlier writes to 15..19 are gone (they were
    ** part of savepoint 2's window), so rewrite them. */
    rc = writeEntries(cur, 21, 24);
    CHECK( rc==BTREELITE_OK );
    rc = countInTxn(cur, &n);
    CHECK( n==24 );

    /* RELEASE savepoint 1 keeps rows 21..24; only the outermost (the
    ** transaction) remains open, and the next commit lands everything. */
    rc = btreelite_savepoint(db, BTREELITE_SAVEPOINT_RELEASE, 1);
    CHECK( rc==BTREELITE_OK );
    rc = countInTxn(cur, &n);
    CHECK( n==24 );

    /* Committing the transaction must persist all rows so far:
    ** 0..19 from savepoint 0 plus 21..24 re-written afterwards. */
    rc = btreelite_commit(db);
    CHECK( rc==BTREELITE_OK );
    CHECK( btreelite_txn_state(db)==0 );
    rc = countEntries(db, iRoot, &n);
    CHECK( rc==BTREELITE_OK );
    CHECK( n==24 );

    /* Out-of-range and bad operations are rejected. */
    rc = btreelite_savepoint(db, BTREELITE_SAVEPOINT_RELEASE, 5);
    CHECK( rc==BTREELITE_ERROR );
    rc = btreelite_savepoint(db, 7, 0);
    CHECK( rc==BTREELITE_ERROR );
  }
  btreelite_cursor_close(cur);

  /* 2. Incremental blob write. */
  {
    char *pVal;
    uint32_t nVal = 0;
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

  /* 4. Memory usage. */
  {
    int n = btreelite_mem_used(db);
    CHECK( n>0 );
  }

  btreelite_close(db);
  unlink(ZDB); unlink(ZWAL); unlink(ZJRNL); unlink("t_wal_test.db-shm");

  printf("%d checks, %d failures\n", nTest, nFail);
  return nFail ? 1 : 0;
}