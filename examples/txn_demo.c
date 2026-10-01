/*
** 2026 September 30
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
** btreelite demo 2: transactions and savepoints.
**
** Shows atomicity (a rolled back write leaves nothing behind), nested
** savepoints (roll back to a named depth without ending the transaction),
** and the durability controls.
**
** Build (from the repository root):
**     make examples
** Run:
**     ./examples/txn_demo
*/
#include "btreelite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_FILE "txn_demo.db"

static void check(const char *zWhat, int rc){
  if( rc!=BTREELITE_OK ){
    fprintf(stderr, "%s failed: rc=%d\n", zWhat, rc);
    exit(1);
  }
}

/*
** Count the entries in tree iRoot (opens its own read transaction).
*/
static int countEntries(btreelite_db *db, unsigned iRoot){
  btreelite_cur *cur = 0;
  int rc, res = 0, n = 0;
  check("cursor_open", btreelite_cursor_open(db, iRoot, 0, &cur));
  check("first", btreelite_first(cur, &res));
  while( res==0 ){
    n++;
    rc = btreelite_next(cur);
    if( rc==BTREELITE_DONE ) break;
    check("next", rc);
    res = btreelite_eof(cur) ? 1 : 0;
  }
  btreelite_cursor_close(cur);
  return n;
}

/*
** Count entries using a cursor the caller already opened in a write txn.
*/
static int countWith(btreelite_cur *cur){
  int rc, res = 0, n = 0;
  check("first", btreelite_first(cur, &res));
  while( res==0 ){
    n++;
    rc = btreelite_next(cur);
    if( rc==BTREELITE_DONE ) break;
    check("next", rc);
    res = btreelite_eof(cur) ? 1 : 0;
  }
  return n;
}

static int putRange(btreelite_cur *cur, int iFirst, int iLast){
  char zKey[32], zVal[64];
  int i;
  for(i=iFirst; i<=iLast; i++){
    int nKey = sprintf(zKey, "row-%04d", i);
    int nVal = sprintf(zVal, "value-%04d", i);
    int rc = btreelite_put(cur, zKey, nKey, zVal, nVal);
    if( rc!=BTREELITE_OK ) return rc;
  }
  return BTREELITE_OK;
}

int main(void){
  btreelite_db *db = 0;
  btreelite_cur *cur = 0;
  unsigned iRoot = 0;

  unlink(DB_FILE);

  check("open", btreelite_open(DB_FILE, &db));

  /* --- Durability knobs --------------------------------------------- */
  printf("synchronous: OFF=%d NORMAL=%d FULL=%d\n",
         BTREELITE_SYNC_OFF, BTREELITE_SYNC_NORMAL, BTREELITE_SYNC_FULL);
  printf("previous synchronous level: %d (FULL by default)\n",
         btreelite_synchronous(db, BTREELITE_SYNC_FULL));
  btreelite_mmap_limit(db, 1<<20);   /* allow up to 1 MB to be mapped */

  /* --- Atomicity: a rolled back transaction changes nothing --------- */
  check("begin", btreelite_begin(db, 1));
  check("create_tree", btreelite_create_tree(db, &iRoot));
  check("cursor_open", btreelite_cursor_open(db, iRoot, 1, &cur));
  check("put(0..19)", putRange(cur, 0, 19));
  check("commit", btreelite_commit(db));
  printf("committed 20 rows\n");

  check("begin", btreelite_begin(db, 1));
  check("put(20..29)", putRange(cur, 20, 29));
  check("rollback", btreelite_rollback(db));
  check("begin(ro)", btreelite_begin(db, 0));
  printf("after rollback the tree still has %d rows\n",
         countEntries(db, iRoot));
  check("commit(ro)", btreelite_commit(db));

  /* --- Savepoints: nested writes, partial rollback ------------------ */
  check("begin", btreelite_begin(db, 1));
  check("put(20..24)", putRange(cur, 20, 24));     /* savepoint 0 window */

  check("begin(nested 1)", btreelite_begin(db, 1));
  check("put(25..29)", putRange(cur, 25, 29));     /* savepoint 1 window */
  printf("inside the transaction: %d rows visible\n", countWith(cur));

  check("begin(nested 2)", btreelite_begin(db, 1));
  check("put(30..34)", putRange(cur, 30, 34));     /* savepoint 2 window */
  printf("after the deepest writes: %d rows\n", countWith(cur));

  /* Roll back to savepoint 1: rows 30..34 vanish, 0..29 survive. */
  check("savepoint(ROLLBACK,1)",
        btreelite_savepoint(db, BTREELITE_SAVEPOINT_ROLLBACK, 1));
  printf("after ROLLBACK TO savepoint 1: %d rows\n", countWith(cur));

  /* Release savepoint 0 (the outermost): commits the transaction. */
  check("savepoint(RELEASE,0)",
        btreelite_savepoint(db, BTREELITE_SAVEPOINT_RELEASE, 0));
  printf("transaction state after RELEASE: %d (0=none)\n",
         btreelite_txn_state(db));
  check("begin(ro)", btreelite_begin(db, 0));
  printf("committed total: %d rows\n", countEntries(db, iRoot));
  check("commit(ro)", btreelite_commit(db));

  btreelite_cursor_close(cur);
  btreelite_close(db);

  /* --- Durability across reopen ------------------------------------- */
  check("reopen", btreelite_open(DB_FILE, &db));
  check("begin(ro)", btreelite_begin(db, 0));
  printf("after reopen: %d rows\n", countEntries(db, iRoot));
  check("commit(ro)", btreelite_commit(db));
  {
    char *zOut = 0;
    int nErr = 0;
    check("integrity_check", btreelite_integrity_check(db, iRoot, 10,
                                                       &nErr, &zOut));
    printf("integrity of the committed tree: %d errors\n", nErr);
    if( zOut ) btreelite_free(zOut);
  }
  printf("journal mode is WAL: %d (BTREELITE_JOURNAL_WAL=%d)\n",
         btreelite_journal_mode(db, BTREELITE_JOURNAL_WAL),
         BTREELITE_JOURNAL_WAL);
  btreelite_close(db);

  unlink(DB_FILE);
  puts("txn_demo: done");
  return 0;
}
