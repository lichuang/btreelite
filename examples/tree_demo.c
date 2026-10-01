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
** btreelite demo 3: many trees, in-memory databases, and maintenance.
**
** Shows that one file can hold several independent trees distinguished by
** their root page number, that a NULL path opens a throwaway in-memory
** database, and how to run the maintenance entry points: a WAL
** checkpoint, an automatic-checkpoint threshold, and an integrity check.
**
** Build (from the repository root):
**     make examples
** Run:
**     ./examples/tree_demo
*/
#include "btreelite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_FILE "tree_demo.db"

static void check(const char *zWhat, int rc){
  if( rc!=BTREELITE_OK ){
    fprintf(stderr, "%s failed: rc=%d\n", zWhat, rc);
    exit(1);
  }
}

static void fillTree(btreelite_db *db, unsigned iRoot, int iTag, int n){
  btreelite_cur *cur = 0;
  char zKey[32], zVal[64];
  int i;
  check("cursor_open", btreelite_cursor_open(db, iRoot, 1, &cur));
  for(i=0; i<n; i++){
    int nKey = sprintf(zKey, "k-%04d", i);
    int nVal = sprintf(zVal, "tree-%d entry-%d", iTag, i);
    check("put", btreelite_put(cur, zKey, nKey, zVal, nVal));
  }
  btreelite_cursor_close(cur);
}

/*
** Read back one entry and report whether its value names tree iTag.
*/
static void expectValue(btreelite_db *db, unsigned iRoot, int iTag, int i){
  btreelite_cur *cur = 0;
  char zKey[32], zWant[64], zGot[64];
  int nKey = sprintf(zKey, "k-%04d", i);
  int nWant = sprintf(zWant, "tree-%d entry-%d", iTag, i);
  uint32_t nGot = 0;
  check("cursor_open", btreelite_cursor_open(db, iRoot, 0, &cur));
  check("get", btreelite_get(cur, zKey, nKey));
  check("value_size", btreelite_value_size(cur, &nGot));
  check("value_read", btreelite_value_read(cur, 0, nGot, zGot));
  printf("  root %u: %s -> %.*s\n", iRoot, zKey, (int)nGot, zGot);
  if( (int)nGot!=nWant || memcmp(zGot, zWant, nWant)!=0 ){
    fprintf(stderr, "  WRONG VALUE for root %u\n", iRoot);
    exit(1);
  }
  btreelite_cursor_close(cur);
}

int main(void){
  btreelite_db *db = 0;
  unsigned aRoot[3];
  int rc, i;

  unlink(DB_FILE);
  check("open", btreelite_open(DB_FILE, &db));

  /* --- Three independent trees in one file -------------------------- */
  check("begin", btreelite_begin(db, 1));
  for(i=0; i<3; i++){
    check("create_tree", btreelite_create_tree(db, &aRoot[i]));
  }
  printf("three trees rooted at pages %u, %u, %u\n",
         aRoot[0], aRoot[1], aRoot[2]);
  for(i=0; i<3; i++){
    fillTree(db, aRoot[i], i, 20);
  }
  check("commit", btreelite_commit(db));

  /* The same key exists in every tree, with a different value. */
  check("begin(ro)", btreelite_begin(db, 0));
  for(i=0; i<3; i++){
    expectValue(db, aRoot[i], i, 7);
  }
  check("commit(ro)", btreelite_commit(db));

  /* Clearing one tree leaves its siblings intact. */
  check("begin", btreelite_begin(db, 1));
  check("clear_tree", btreelite_clear_tree(db, aRoot[1]));
  check("commit", btreelite_commit(db));
  printf("cleared the tree at page %u\n", aRoot[1]);

  /* --- Integrity check and WAL maintenance -------------------------- */
  for(i=0; i<3; i++){
    char *zOut = 0;
    int nErr = 0;
    check("integrity_check",
          btreelite_integrity_check(db, aRoot[i], 10, &nErr, &zOut));
    printf("integrity of tree %u: %d errors\n", aRoot[i], nErr);
    if( zOut ) btreelite_free(zOut);
  }
  {
    int nLog = -1, nCkpt = -1;
    check("checkpoint",
          btreelite_checkpoint(db, BTREELITE_CHECKPOINT_TRUNCATE,
                               &nLog, &nCkpt));
    printf("checkpoint(TRUNCATE): %d frames in the log, %d checkpointed\n",
           nLog, nCkpt);
  }
  /* Ask for an automatic checkpoint once the log reaches 8 frames. */
  btreelite_wal_autocheckpoint(db, 8);
  printf("autocheckpoint threshold set to 8 frames\n");
  btreelite_close(db);

  /* --- A throwaway in-memory database -------------------------------- */
  {
    btreelite_db *mem = 0;
    btreelite_cur *cur = 0;
    uint32_t nVal = 0;
    unsigned iRoot = 0;
    check("open(NULL)", btreelite_open(0, &mem));
    check("begin", btreelite_begin(mem, 1));
    check("create_tree", btreelite_create_tree(mem, &iRoot));
    check("cursor_open", btreelite_cursor_open(mem, iRoot, 1, &cur));
    check("put", btreelite_put(cur, "temp", 4, "in memory only", 14));
    check("get", btreelite_get(cur, "temp", 4));
    check("value_size", btreelite_value_size(cur, &nVal));
    printf("in-memory entry length: %u\n", nVal);
    btreelite_cursor_close(cur);
    /* No commit: closing discards the whole database anyway. */
    btreelite_close(mem);
    printf("(in-memory database is gone after close)\n");
    rc = 0; (void)rc;
  }

  unlink(DB_FILE);
  puts("tree_demo: done");
  return 0;
}
