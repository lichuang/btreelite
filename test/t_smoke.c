/*
** 2026 September 25
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
** Storage smoke test.  Drives the public btreelite key-value API to prove
** the storage mechanics work end to end:
**
**   1. a file-backed database is created and entries are written through a
**      cursor in a write transaction, then committed;
**   2. the committed entries are read back from disk in a fresh transaction;
**   3. a rolled-back write transaction leaves the database unchanged;
**   4. the database recovers from a crash: a child process that dies with
**      an uncommitted transaction open leaves the committed data intact on
**      the next open.
*/
#include "../include/btreelite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int nFail = 0;
static int nTest = 0;

#define CHECK(cond) do{                                            \
  nTest++;                                                         \
  if( !(cond) ){                                                   \
    nFail++;                                                       \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
  }                                                                \
}while(0)

#define KEY_FMT  "row-%04d"
#define VAL_FMT  "payload-for-row-%04d"

static void makeEntry(int i, char *zKey, int *pnKey, char *zVal, int *pnVal){
  *pnKey = sprintf(zKey, KEY_FMT, i);
  *pnVal = sprintf(zVal, VAL_FMT, i);
}

static int writeRows(btreelite_cur *cur, int iFirst, int iLast){
  int rc = BTREELITE_OK, i;
  for(i=iFirst; i<=iLast && rc==BTREELITE_OK; i++){
    char zKey[64], zVal[64];
    int nKey, nVal;
    makeEntry(i, zKey, &nKey, zVal, &nVal);
    rc = btreelite_put(cur, zKey, nKey, zVal, nVal);
  }
  return rc;
}

/*
** Iterate the whole tree and verify every entry's value matches its key.
** *pnRow receives the number of entries seen.
*/
static int countRows(btreelite_cur *cur, int *pnRow){
  int rc, res = 0, nRow = 0;
  rc = btreelite_first(cur, &res);
  while( rc==BTREELITE_OK && res==0 ){
    char zKey[64], zVal[64], zGot[64];
    int nKey = 0, nVal, iKey;
    uint32_t nValStored = 0;
    if( btreelite_key(cur, zKey, sizeof(zKey), &nKey)!=BTREELITE_OK
     || nKey<0 || nKey>=(int)sizeof(zKey) ){
      rc = BTREELITE_CORRUPT;
      break;
    }
    zKey[nKey] = 0;
    if( sscanf(zKey, KEY_FMT, &iKey)!=1 ){ rc = BTREELITE_CORRUPT; break; }
    nVal = sprintf(zVal, VAL_FMT, iKey);
    if( btreelite_value_size(cur, &nValStored)!=BTREELITE_OK
     || nValStored!=(uint32_t)nVal ){
      rc = BTREELITE_CORRUPT;
      break;
    }
    if( btreelite_value_read(cur, 0, nValStored, zGot)!=BTREELITE_OK
     || memcmp(zGot, zVal, (size_t)nVal)!=0 ){
      rc = BTREELITE_CORRUPT;
      break;
    }
    nRow++;
    rc = btreelite_next(cur);
    if( rc==BTREELITE_DONE ){ rc = BTREELITE_OK; break; }
    res = btreelite_eof(cur) ? 1 : 0;
  }
  *pnRow = nRow;
  return rc;
}

static void testCommitPersist(const char *zFile){
  btreelite_db *db = 0;
  btreelite_cur *cur = 0;
  unsigned iRoot = 0;
  int rc, nRow = 0;

  rc = btreelite_open(zFile, &db);
  CHECK( rc==BTREELITE_OK );
  if( rc ) return;

  rc = btreelite_begin(db, 1);
  CHECK( rc==BTREELITE_OK );
  rc = btreelite_create_tree(db, &iRoot);
  CHECK( rc==BTREELITE_OK );
  rc = btreelite_cursor_open(db, iRoot, 1, &cur);
  CHECK( rc==BTREELITE_OK );
  rc = writeRows(cur, 1, 50);
  CHECK( rc==BTREELITE_OK );
  btreelite_cursor_close(cur);
  rc = btreelite_commit(db);
  CHECK( rc==BTREELITE_OK );
  btreelite_close(db);

  db = 0;
  rc = btreelite_open(zFile, &db);
  CHECK( rc==BTREELITE_OK );
  if( rc ) return;
  rc = btreelite_begin(db, 0);
  CHECK( rc==BTREELITE_OK );
  rc = btreelite_cursor_open(db, iRoot, 0, &cur);
  CHECK( rc==BTREELITE_OK );
  rc = countRows(cur, &nRow);
  CHECK( rc==BTREELITE_OK );
  CHECK( nRow==50 );
  btreelite_cursor_close(cur);
  btreelite_commit(db);
  btreelite_close(db);
}

static void testRollback(const char *zFile){
  btreelite_db *db = 0;
  btreelite_cur *cur = 0;
  int rc, nRow = 0;

  rc = btreelite_open(zFile, &db);
  CHECK( rc==BTREELITE_OK );
  if( rc ) return;

  rc = btreelite_begin(db, 1);
  CHECK( rc==BTREELITE_OK );
  rc = btreelite_cursor_open(db, 2, 1, &cur);
  CHECK( rc==BTREELITE_OK );
  rc = writeRows(cur, 51, 70);
  CHECK( rc==BTREELITE_OK );
  btreelite_cursor_close(cur);
  rc = btreelite_rollback(db);
  CHECK( rc==BTREELITE_OK );

  rc = btreelite_begin(db, 0);
  CHECK( rc==BTREELITE_OK );
  rc = btreelite_cursor_open(db, 2, 0, &cur);
  CHECK( rc==BTREELITE_OK );
  rc = countRows(cur, &nRow);
  CHECK( rc==BTREELITE_OK );
  CHECK( nRow==50 );
  btreelite_cursor_close(cur);
  btreelite_commit(db);
  btreelite_close(db);
}

static void testCrashRecovery(const char *zFile){
  btreelite_db *db = 0;
  btreelite_cur *cur = 0;
  int rc, nRow = 0, status;
  pid_t pid;

  pid = fork();
  if( pid==0 ){
    btreelite_db *c = 0;
    btreelite_cur *cc = 0;
    if( btreelite_open(zFile, &c)==BTREELITE_OK
     && btreelite_begin(c, 1)==BTREELITE_OK
     && btreelite_cursor_open(c, 2, 1, &cc)==BTREELITE_OK ){
      char zKey[64], zVal[64];
      int nKey, nVal;
      makeEntry(1, zKey, &nKey, zVal, &nVal);
      btreelite_put(cc, zKey, nKey, "dirty-uncommitted-payload", 25);
      btreelite_cursor_close(cc);
    }
    /* Exit without commit: the next open discards the open transaction. */
    _exit(0);
  }
  waitpid(pid, &status, 0);

  rc = btreelite_open(zFile, &db);
  CHECK( rc==BTREELITE_OK );
  if( rc ) return;
  rc = btreelite_begin(db, 0);
  CHECK( rc==BTREELITE_OK );
  rc = btreelite_cursor_open(db, 2, 0, &cur);
  CHECK( rc==BTREELITE_OK );
  rc = countRows(cur, &nRow);
  CHECK( rc==BTREELITE_OK );
  CHECK( nRow==50 );
  btreelite_cursor_close(cur);
  btreelite_commit(db);
  btreelite_close(db);
}

int main(void){
  const char *zFile = "t_smoke.db";
  unlink(zFile);
  unlink("t_smoke.db-journal");
  unlink("t_smoke.db-wal");
  unlink("t_smoke.db-shm");

  testCommitPersist(zFile);
  testRollback(zFile);
  testCrashRecovery(zFile);

  unlink(zFile);
  unlink("t_smoke.db-journal");
  unlink("t_smoke.db-wal");
  unlink("t_smoke.db-shm");

  printf("%d checks, %d failures\n", nTest, nFail);
  return nFail ? 1 : 0;
}
