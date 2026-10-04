/*
** 2026 October 4
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
** White-box test: sqlite3BtreeInsert() with the BTREE_SAVEPOSITION flag.
** The public API never passes that flag, so this test drives the btree
** layer directly.  After a balance, such an insert must leave the cursor
** restorable: the KV key is a byte string and has to be copied out like
** saveCursorKey() does.  (Regression: the copy used to be conditional on
** pCur->pKeyInfo, which is always NULL for a KV cursor, so the cursor
** was left with pKey==NULL and nKey>0 -- and the next position restore
** compared against a NULL key.)
**
**   1. inserts with BTREE_SAVEPOSITION survive balances and the cursor
**      restores cleanly after every insert;
**   2. the tree content is exactly the inserted set afterwards.
*/
#include "btreeliteInt.h"
#include "btree.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int nFail = 0;
static int nTest = 0;
#define CHECK(c) do{ nTest++; if( !(c) ){                          \
    nFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
  } }while(0)

#define ZDB   "t_savepos_test.db"
#define ZWAL  "t_savepos_test.db-wal"
#define NROW  1200

int main(void){
  sqlite3 env;
  Btree *pBt = 0;
  BtCursor *pCur = 0;
  Pgno iRoot = 0;
  int rc, i;
  int vfsFlags = SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_MAIN_DB;

  unlink(ZDB); unlink(ZWAL);

  /* Minimal connection environment, mirroring btreelite_open(). */
  rc = sqlite3_initialize();
  CHECK( rc==SQLITE_OK );
  memset(&env, 0, sizeof(env));
  env.mutex = sqlite3MutexAlloc(SQLITE_MUTEX_RECURSIVE);
  env.aDb = (Db*)sqlite3MallocZero(sizeof(Db));
  env.nDb = 1;
  env.errMask = 0xff;
  sqlite3_mutex_enter(env.mutex);
  rc = sqlite3BtreeOpen(sqlite3_vfs_find(0), ZDB, &env, &pBt, 0, vfsFlags);
  CHECK( rc==SQLITE_OK );
  env.aDb[0].pBt = pBt;

  pCur = (BtCursor*)sqlite3Malloc(sqlite3BtreeCursorSize());
  CHECK( pCur!=0 );
  sqlite3BtreeCursorZero(pCur);

  rc = sqlite3BtreeBeginTrans(pBt, 1, 0);
  CHECK( rc==SQLITE_OK );
  rc = sqlite3BtreeCreateTable(pBt, &iRoot, BTREE_BLOBKEY);
  CHECK( rc==SQLITE_OK );
  rc = sqlite3BtreeCursor(pBt, iRoot, BTREE_WRCSR, 0, pCur);
  CHECK( rc==SQLITE_OK );

  /* 1. Insert ascending keys with BTREE_SAVEPOSITION.  The tree splits
  ** many times over 1200 rows, so the save-the-key branch in
  ** sqlite3BtreeInsert runs over and over; the BtreeNext() call after
  ** each insert forces a position restore through the saved key. */
  for(i=0; i<NROW; i++){
    BtreePayload x;
    char zKey[32], zVal[32];
    int nk = sprintf(zKey, "key-%06d", i);
    int nv = sprintf(zVal, "val-%06d", i);
    memset(&x, 0, sizeof(x));
    x.pKey = zKey;
    x.nKey = nk;
    x.pData = zVal;
    x.nData = nv;
    rc = sqlite3BtreeInsert(pCur, &x, BTREE_APPEND|BTREE_SAVEPOSITION, 0);
    CHECK( rc==SQLITE_OK );
    rc = sqlite3BtreeNext(pCur, 0);
    CHECK( rc==SQLITE_OK || rc==SQLITE_DONE );
  }

  /* Descending inserts take the slow search path (no append bias) and
  ** save positions just the same. */
  for(i=2*NROW; i>=NROW; i--){
    BtreePayload x;
    char zKey[32], zVal[32];
    int nk = sprintf(zKey, "key-%06d", i);
    int nv = sprintf(zVal, "val-%06d", i);
    memset(&x, 0, sizeof(x));
    x.pKey = zKey;
    x.nKey = nk;
    x.pData = zVal;
    x.nData = nv;
    rc = sqlite3BtreeInsert(pCur, &x, BTREE_SAVEPOSITION, 0);
    CHECK( rc==SQLITE_OK );
    rc = sqlite3BtreeNext(pCur, 0);
    CHECK( rc==SQLITE_OK || rc==SQLITE_DONE );
  }

  /* 2. The tree holds exactly keys key-000000..key-002400 in order. */
  {
    int res = 0, n = 0;
    char zExpect[32];
    rc = sqlite3BtreeFirst(pCur, &res);
    CHECK( rc==SQLITE_OK );
    CHECK( res==0 );
    while( rc==SQLITE_OK && res==0 ){
      u32 nKey = 0;
      const void *pKey = sqlite3BtreeKvKey(pCur, &nKey);
      int nk = sprintf(zExpect, "key-%06d", n);
      CHECK( (int)nKey==nk && memcmp(pKey, zExpect, nk)==0 );
      n++;
      rc = sqlite3BtreeNext(pCur, 0);
      if( rc==SQLITE_DONE ){ rc = SQLITE_OK; break; }
    }
    CHECK( rc==SQLITE_OK );
    CHECK( n==2*NROW+1 );
  }

  sqlite3BtreeCloseCursor(pCur);
  sqlite3_free(pCur);
  rc = sqlite3BtreeCommit(pBt);
  CHECK( rc==SQLITE_OK );
  sqlite3BtreeClose(pBt);
  sqlite3_mutex_leave(env.mutex);
  sqlite3_mutex_free(env.mutex);
  sqlite3_free(env.aDb);
  unlink(ZDB); unlink(ZWAL);

  printf("%d checks, %d failures\n", nTest, nFail);
  return nFail ? 1 : 0;
}
