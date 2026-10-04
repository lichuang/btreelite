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
** Multiple trees in one file: the acceptance test README and phase.md
** both promise ("one database file can contain many B-trees"), but no
** suite exercised more than one tree at a time.  Covers:
**
**   1. several trees coexist in one file, each with its own entries;
**   2. one write transaction touches every tree at once and commits —
**      or rolls back — as a unit;
**   3. clear_tree empties exactly one tree and leaves the rest intact;
**   4. each tree's keys are confined to it: the same key can map to a
**      different value per tree, and one tree's rows never surface in
**      another's scan;
**   5. all trees read back correctly after close/reopen, and their
**      root page numbers are the handles the caller persists.
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

#define ZDB     "t_trees_test.db"
#define ZWAL    "t_trees_test.db-wal"
#define ZSHM    "t_trees_test.db-shm"
#define ZJRNL   "t_trees_test.db-journal"
#define NTREES  4
#define NROWS   40

/*
** Fill tree iRoot with NROWS rows keyed "row-N" whose values name the
** owning tree ("tree-<id>-value-<N>").
*/
static int fillTree(btreelite_db *db, unsigned iRoot, int iTree){
  btreelite_cur *cur = 0;
  char zKey[64], zVal[96];
  int rc, i;
  rc = btreelite_cursor_open(db, iRoot, 1, &cur);
  if( rc ) return rc;
  for(i=0; i<NROWS; i++){
    int nk = sprintf(zKey, "row-%04d", i);
    int nv = sprintf(zVal, "tree-%d-value-%d", iTree, i);
    rc = btreelite_put(cur, zKey, nk, zVal, nv);
    if( rc!=BTREELITE_OK ) break;
  }
  btreelite_cursor_close(cur);
  return rc;
}

/*
** Verify tree iRoot holds exactly its own NROWS rows with the right
** values, and nothing else.  iRoot==0 means "expect the tree empty".
*/
static int verifyTree(btreelite_db *db, unsigned iRoot, int iTree){
  btreelite_cur *cur = 0;
  char zKey[64], zGot[96];
  int rc, res = 0, n = 0, iErr = 0;
  int iExpect = iTree<0 ? 0 : NROWS;

  rc = btreelite_begin(db, 0);
  if( rc ) return rc;
  rc = btreelite_cursor_open(db, iRoot, 0, &cur);
  if( rc ) return rc;
  rc = btreelite_first(cur, &res);
  while( rc==BTREELITE_OK && res==0 ){
    int nLen = 0;
    int nk, nv;
    char zExpect[96];
    uint32_t nVal = 0;
    nk = sprintf(zKey, "row-%04d", n);
    nv = sprintf(zExpect, "tree-%d-value-%d", iExpect<0?0:iTree, n);
    /* With iTree<0 the tree is expected empty, so any row is an error. */
    if( iTree<0 ){ iErr++; break; }
    if( btreelite_key(cur, zGot, sizeof(zGot), &nLen)!=BTREELITE_OK
     || nLen!=nk || memcmp(zGot, zKey, (size_t)nk)!=0 ){ iErr++; break; }
    if( btreelite_value_size(cur, &nVal)!=BTREELITE_OK
     || nVal!=(uint32_t)nv ){ iErr++; break; }
    if( btreelite_value_read(cur, 0, nVal, zGot)!=BTREELITE_OK
     || memcmp(zGot, zExpect, (size_t)nv)!=0 ){ iErr++; break; }
    n++;
    rc = btreelite_next(cur);
    if( rc==BTREELITE_DONE ){ rc = BTREELITE_OK; break; }
    res = btreelite_eof(cur) ? 1 : 0;
  }
  if( n!=iExpect ) iErr++;
  btreelite_cursor_close(cur);
  btreelite_commit(db);
  return iErr ? BTREELITE_ERROR : BTREELITE_OK;
}

/*
** Count all entries of the tree; *pn receives the total.
*/
static int countAll(btreelite_db *db, unsigned iRoot, int *pn){
  btreelite_cur *cur = 0;
  int rc, res = 0;
  *pn = 0;
  rc = btreelite_begin(db, 0);
  if( rc ) return rc;
  rc = btreelite_cursor_open(db, iRoot, 0, &cur);
  if( rc ) return rc;
  rc = btreelite_first(cur, &res);
  while( rc==BTREELITE_OK && res==0 ){
    (*pn)++;
    rc = btreelite_next(cur);
    if( rc==BTREELITE_DONE ){ rc = BTREELITE_OK; break; }
    res = btreelite_eof(cur) ? 1 : 0;
  }
  btreelite_cursor_close(cur);
  btreelite_commit(db);
  return rc;
}

int main(void){
  btreelite_db *db = 0;
  btreelite_cur *cur = 0;
  unsigned aRoot[NTREES];
  int rc, n, it, i;

  for(it=0; it<NTREES; it++) aRoot[it] = 0;
  unlink(ZDB); unlink(ZWAL); unlink(ZSHM); unlink(ZJRNL);

  /* ---------------------------------------------------------------- */
  /* 1. Create NTREES trees in one write transaction; each gets a      */
  /*    distinct root page and each can hold its own entries.          */
  /* ---------------------------------------------------------------- */
  rc = btreelite_open(ZDB, &db);
  CHECK( rc==BTREELITE_OK );
  if( rc ) return 1;
  rc = btreelite_begin(db, 1);
  CHECK( rc==BTREELITE_OK );
  for(it=0; it<NTREES; it++){
    unsigned iRoot = 0;
    rc = btreelite_create_tree(db, &iRoot);
    CHECK( rc==BTREELITE_OK );
    CHECK( iRoot>0 );
    /* The roots must be distinct pages. */
    for(i=0; i<it; i++){
      CHECK( iRoot!=aRoot[i] );
    }
    aRoot[it] = iRoot;
  }
  rc = btreelite_commit(db);
  CHECK( rc==BTREELITE_OK );

  /* ---------------------------------------------------------------- */
  /* 2. One write transaction fills every tree; commit makes it a      */
  /*    unit: afterwards all trees are complete.                       */
  /* ---------------------------------------------------------------- */
  rc = btreelite_begin(db, 1);
  CHECK( rc==BTREELITE_OK );
  for(it=0; it<NTREES; it++){
    rc = fillTree(db, aRoot[it], it);
    CHECK( rc==BTREELITE_OK );
  }
  rc = btreelite_commit(db);
  CHECK( rc==BTREELITE_OK );
  for(it=0; it<NTREES; it++){
    rc = verifyTree(db, aRoot[it], it);
    CHECK( rc==BTREELITE_OK );
  }

  /* ---------------------------------------------------------------- */
  /* 3. Keys are confined to their tree: the same key maps to a        */
  /*    different value in every tree, and no tree leaks rows into     */
  /*    another's scan.                                                */
  /* ---------------------------------------------------------------- */
  {
    for(it=0; it<NTREES; it++){
      rc = btreelite_begin(db, 0);
      CHECK( rc==BTREELITE_OK );
      rc = btreelite_cursor_open(db, aRoot[it], 0, &cur);
      CHECK( rc==BTREELITE_OK );
      rc = btreelite_get(cur, "row-0007", 8);
      CHECK( rc==BTREELITE_OK );
      {
        char zGot[96];
        uint32_t nVal = 0;
        char zExpect[96];
        int nv = sprintf(zExpect, "tree-%d-value-7", it);
        btreelite_value_size(cur, &nVal);
        CHECK( nVal==(uint32_t)nv );
        rc = btreelite_value_read(cur, 0, nVal, zGot);
        CHECK( rc==BTREELITE_OK );
        CHECK( memcmp(zGot, zExpect, (size_t)nv)==0 );
      }
      btreelite_cursor_close(cur);
      btreelite_commit(db);
    }
    /* An empty tree reports no rows at all. */
    {
      unsigned iRoot = 0;
      rc = btreelite_begin(db, 1);
      CHECK( rc==BTREELITE_OK );
      rc = btreelite_create_tree(db, &iRoot);
      CHECK( rc==BTREELITE_OK );
      CHECK( btreelite_commit(db)==BTREELITE_OK );
      rc = verifyTree(db, iRoot, -1);
      CHECK( rc==BTREELITE_OK );
    }
  }

  /* ---------------------------------------------------------------- */
  /* 4. One write transaction rolled back leaves every tree as it was  */
  /*    before the transaction started.                                */
  /* ---------------------------------------------------------------- */
  {
    rc = btreelite_begin(db, 1);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_cursor_open(db, aRoot[1], 1, &cur);
    CHECK( rc==BTREELITE_OK );
    {
      char zKey[64];
      int i;
      /* Overwrite and add rows, then a fresh row in another tree. */
      for(i=0; i<10; i++){
        int nk = sprintf(zKey, "row-%04d", i);
        btreelite_put(cur, zKey, nk, "DIRTY", 5);
      }
    }
    btreelite_cursor_close(cur);
    rc = btreelite_cursor_open(db, aRoot[2], 1, &cur);
    CHECK( rc==BTREELITE_OK );
    {
      char zKey[64];
      int nk = sprintf(zKey, "row-%04d", NROWS);   /* new key */
      btreelite_put(cur, zKey, nk, "DIRTY-2", 7);
    }
    btreelite_cursor_close(cur);
    rc = btreelite_rollback(db);
    CHECK( rc==BTREELITE_OK );
    for(it=0; it<NTREES; it++){
      rc = verifyTree(db, aRoot[it], it);
      CHECK( rc==BTREELITE_OK );
    }
  }

  /* ---------------------------------------------------------------- */
  /* 5. clear_tree empties exactly one tree; its siblings stay full.   */
  /* ---------------------------------------------------------------- */
  {
    rc = btreelite_begin(db, 1);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_clear_tree(db, aRoot[0]);
    CHECK( rc==BTREELITE_OK );
    CHECK( btreelite_commit(db)==BTREELITE_OK );
    rc = verifyTree(db, aRoot[0], -1);
    CHECK( rc==BTREELITE_OK );
    for(it=1; it<NTREES; it++){
      rc = verifyTree(db, aRoot[it], it);
      CHECK( rc==BTREELITE_OK );
    }
  }

  /* ---------------------------------------------------------------- */
  /* 6. Close and reopen: root page numbers survive as handles, and    */
  /*    every tree's contents come back.                               */
  /* ---------------------------------------------------------------- */
  {
    btreelite_close(db);
    db = 0;
    rc = btreelite_open(ZDB, &db);
    CHECK( rc==BTREELITE_OK );
    CHECK( btreelite_page_count(db)>0 );
    /* The cleared tree is still empty; the others still hold their rows. */
    rc = verifyTree(db, aRoot[0], -1);
    CHECK( rc==BTREELITE_OK );
    for(it=1; it<NTREES; it++){
      rc = verifyTree(db, aRoot[it], it);
      CHECK( rc==BTREELITE_OK );
    }
    rc = countAll(db, aRoot[3], &n);
    CHECK( rc==BTREELITE_OK );
    CHECK( n==NROWS );
    btreelite_close(db);
  }

  btreelite_free(0);   /* harmless */
  unlink(ZDB); unlink(ZWAL); unlink(ZSHM); unlink(ZJRNL);

  printf("%d checks, %d failures\n", nTest, nFail);
  return nFail ? 1 : 0;
}