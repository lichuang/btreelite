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
** btreelite demo 1: the plain key-value API.
**
** Shows the smallest useful lifecycle: open a file, create a tree, put /
** get / delete entries, walk the tree forwards and backwards, do a range
** seek, and store a value far larger than one page (it spills onto
** overflow pages and is read back through the streaming value API).
**
** Build (from the repository root):
**     make examples
** Run:
**     ./examples/kv_demo
*/
#include "btreelite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_FILE "kv_demo.db"

static void die(const char *zWhat, int rc){
  fprintf(stderr, "%s failed: rc=%d\n", zWhat, rc);
  exit(1);
}

static void check(const char *zWhat, int rc){
  if( rc!=BTREELITE_OK ) die(zWhat, rc);
}

/*
** Print an entry's key and the first bytes of its value.
*/
static void show(btreelite_cur *cur){
  char zKey[256], zVal[64];
  int nKey = 0;
  uint32_t nVal = 0;
  int nShow;
  check("key", btreelite_key(cur, zKey, sizeof(zKey), &nKey));
  check("value_size", btreelite_value_size(cur, &nVal));
  nShow = (int)(nVal < sizeof(zVal) ? nVal : sizeof(zVal));
  if( nShow ) check("value_read", btreelite_value_read(cur, 0, nShow, zVal));
  printf("  %-16.*s -> %.*s%s\n", nKey, zKey, nShow, zVal,
         nVal > (uint32_t)nShow ? "..." : "");
}

int main(void){
  btreelite_db *db = 0;
  btreelite_cur *cur = 0;
  unsigned iRoot = 0;
  int rc, i, res = 0;

  unlink(DB_FILE);

  /* --- Open a database and create a tree ---------------------------- */
  check("open", btreelite_open(DB_FILE, &db));
  printf("opened %s, page size/count: %u pages\n",
         DB_FILE, btreelite_page_count(db));

  check("begin", btreelite_begin(db, 1));
  check("create_tree", btreelite_create_tree(db, &iRoot));
  printf("created tree rooted at page %u\n", iRoot);

  /* --- Insert a handful of entries in one transaction --------------- */
  check("cursor_open", btreelite_cursor_open(db, iRoot, 1, &cur));
  for(i=0; i<10; i++){
    char zKey[32], zVal[64];
    int nKey = sprintf(zKey, "key-%03d", i);
    int nVal = sprintf(zVal, "value for entry %d", i);
    check("put", btreelite_put(cur, zKey, nKey, zVal, nVal));
  }
  check("commit", btreelite_commit(db));
  printf("inserted 10 entries and committed\n");

  /* --- Point lookup ------------------------------------------------- */
  check("begin(ro)", btreelite_begin(db, 0));
  check("get", btreelite_get(cur, "key-007", 7));
  show(cur);
  rc = btreelite_get(cur, "no-such-key", 11);
  printf("lookup of a missing key: rc=%d (NOTFOUND=%d)\n",
         rc, BTREELITE_NOTFOUND);
  check("commit(ro)", btreelite_commit(db));

  /* --- Forward scan ------------------------------------------------- */
  printf("forward scan:\n");
  check("begin(ro)", btreelite_begin(db, 0));
  check("first", btreelite_first(cur, &res));
  while( res==0 ){
    show(cur);
    rc = btreelite_next(cur);
    if( rc==BTREELITE_DONE ) break;
    check("next", rc);
    res = btreelite_eof(cur) ? 1 : 0;
  }

  /* --- Range seek: first entry >= "key-005" ------------------------- */
  check("seek", btreelite_seek(cur, "key-005", 7));
  printf("first entry >= key-005:\n");
  show(cur);
  check("commit(ro)", btreelite_commit(db));

  /* --- Delete, then confirm it is gone ------------------------------ */
  check("begin(rw)", btreelite_begin(db, 1));
  check("del", btreelite_del(cur, "key-005", 7));
  check("commit", btreelite_commit(db));
  check("begin(ro)", btreelite_begin(db, 0));
  rc = btreelite_get(cur, "key-005", 7);
  printf("after delete, lookup rc=%d\n", rc);
  check("commit(ro)", btreelite_commit(db));

  /* --- A value larger than one page: overflow pages ----------------- */
  {
    const size_t nBig = 200*1024;
    char *zBig = (char*)malloc(nBig);
    char *zOut = (char*)malloc(nBig);
    uint32_t nOut = 0;
    memset(zBig, 'x', nBig);
    memcpy(zBig, "BIG-VALUE-HEAD", 14);

    check("begin(rw)", btreelite_begin(db, 1));
    check("put(big)", btreelite_put(cur, "big", 3, zBig, (int)nBig));
    check("commit", btreelite_commit(db));

    check("begin(ro)", btreelite_begin(db, 0));
    check("get(big)", btreelite_get(cur, "big", 3));
    check("value_size(big)", btreelite_value_size(cur, &nOut));
    check("value_read(big)", btreelite_value_read(cur, 0, nOut, zOut));
    printf("stored a %u-byte value; read back %s\n", nOut,
           memcmp(zOut, zBig, nBig)==0 ? "identically" : "WRONG");
    check("commit(ro)", btreelite_commit(db));
    free(zBig); free(zOut);
  }

  btreelite_cursor_close(cur);
  btreelite_close(db);
  unlink(DB_FILE);
  puts("kv_demo: done");
  return 0;
}
