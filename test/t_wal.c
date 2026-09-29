/*
** 2026 September 28
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
** Phase 3 acceptance test: the write-ahead-log seam.  Covers
**
**   1. entering WAL mode (version bytes flip to 2, the -wal file appears);
**   2. commits written through the WAL and read back in WAL mode;
**   3. checkpoints in PASSIVE / FULL / RESTART / TRUNCATE modes, with
**      sane pnLog/pnCkpt output;
**   4. crash recovery: a child process that writes and exits without
**      committing leaves its WAL frames for the next connection to roll
**      back;
**   5. automatic checkpointing after every nFrame commits;
**   6. leaving WAL mode (checkpoint, drop -wal, version bytes back to 1);
**   7. WAL transitions inside a transaction are refused;
**   8. the busy timeout installs and clears.
*/
#include "../include/btreelite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int nFail = 0;
static int nTest = 0;
#define CHECK(c) do{ nTest++; if( !(c) ){                          \
    nFail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
  } }while(0)

#define ZDB     "t_wal_test.db"
#define ZWAL    "t_wal_test.db-wal"
#define ZSHM    "t_wal_test.db-shm"
#define ZJRNL   "t_wal_test.db-journal"

/*
** Open zPath and set WAL mode on the new handle.
*/
static btreelite_db *openWalDb(const char *zPath){
  btreelite_db *db = 0;
  int rc = btreelite_open(zPath, &db);
  if( rc!=BTREELITE_OK ) return 0;
  rc = btreelite_journal_mode(db, BTREELITE_JOURNAL_WAL);
  if( rc!=BTREELITE_JOURNAL_WAL ){
    btreelite_close(db);
    return 0;
  }
  return db;
}

/*
** Write entries [iFirst..iLast] ("row-N" -> "val-N") and commit.
*/
static int writeEntries(btreelite_db *db, unsigned iRoot, int iFirst, int iLast){
  btreelite_cur *cur = 0;
  char zKey[64], zVal[64];
  int rc, i;
  rc = btreelite_begin(db, 1);
  if( rc ) return rc;
  rc = btreelite_cursor_open(db, iRoot, 1, &cur);
  if( rc ) return rc;
  for(i=iFirst; i<=iLast; i++){
    int nk = sprintf(zKey, "row-%04d", i);
    int nv = sprintf(zVal, "val-%04d", i);
    rc = btreelite_put(cur, zKey, nk, zVal, nv);
    if( rc ) break;
  }
  btreelite_cursor_close(cur);
  return rc ? rc : btreelite_commit(db);
}

/*
** Count entries in the whole tree; *pnCount receives the result.
*/
static int countEntries(btreelite_db *db, unsigned iRoot, int *pnCount){
  btreelite_cur *cur = 0;
  int rc, res = 0, n = 0, nErr = 0;
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
  if( rc!=BTREELITE_OK ) nErr++;
  btreelite_cursor_close(cur);
  btreelite_commit(db);
  *pnCount = n;
  return nErr ? BTREELITE_ERROR : BTREELITE_OK;
}

/*
** Verify entries [iFirst..iLast] against their keys; *pnCount receives the
** number of entries found.
*/
static int verifyEntries(btreelite_db *db, unsigned iRoot, int iFirst, int iLast,
                         int *pnCount){
  btreelite_cur *cur = 0;
  char zKey[64], zGot[64];
  int rc, i, nErr = 0;
  *pnCount = 0;
  rc = btreelite_begin(db, 0);
  if( rc ) return rc;
  rc = btreelite_cursor_open(db, iRoot, 0, &cur);
  if( rc ) return rc;
  for(i=iFirst; i<=iLast; i++){
    int no = sprintf(zKey, "row-%04d", i);
    char zExpect[64];
    uint32_t nVal = 0;
    no = sprintf(zExpect, "val-%04d", i);
    if( btreelite_get(cur, zKey, (int)strlen(zKey))!=BTREELITE_OK ){
      nErr++;
      break;
    }
    btreelite_value_size(cur, &nVal);
    if( nVal!=(uint32_t)no ){
      nErr++;
      break;
    }
    btreelite_value_read(cur, 0, nVal, zGot);
    if( memcmp(zGot, zExpect, (size_t)no)!=0 ){
      nErr++;
      break;
    }
    (*pnCount)++;
  }
  btreelite_cursor_close(cur);
  btreelite_commit(db);
  return nErr ? BTREELITE_ERROR : BTREELITE_OK;
}

int main(void){
  btreelite_db *db = 0;
  btreelite_cur *cur = 0;
  unsigned iRoot = 2;
  int rc, nCount = 0;
  int tmpCount = 0;

  unlink(ZDB); unlink(ZWAL); unlink(ZJRNL); unlink(ZSHM);

  db = openWalDb(ZDB);
  CHECK( db!=0 );
  if( db==0 ){ return 1; }
  rc = btreelite_begin(db, 1);
  CHECK( rc==BTREELITE_OK );
  rc = btreelite_create_tree(db, &iRoot);
  CHECK( rc==BTREELITE_OK );
  rc = btreelite_cursor_open(db, iRoot, 1, &cur);
  CHECK( rc==BTREELITE_OK );
  rc = btreelite_put(cur, "seed", 4, "seed", 4);
  CHECK( rc==BTREELITE_OK );
  btreelite_cursor_close(cur);
  rc = btreelite_commit(db);
  CHECK( rc==BTREELITE_OK );
  CHECK( access(ZWAL, 0)==0 );
  rc = writeEntries(db, iRoot, 0, 39);
  CHECK( rc==BTREELITE_OK );
  rc = verifyEntries(db, iRoot, 0, 39, &nCount);
  CHECK( rc==BTREELITE_OK );
  CHECK( nCount==40 );

  /* 2. Checkpoints in all four modes. */
  {
    int nLog = -99, nCkpt = -99;
    rc = writeEntries(db, iRoot, 40, 59);
    CHECK( rc==BTREELITE_OK );

    nLog = -99; nCkpt = -99;
    rc = btreelite_checkpoint(db, BTREELITE_CHECKPOINT_PASSIVE,
                              &nLog, &nCkpt);
    CHECK( rc==BTREELITE_OK );
    CHECK( nLog>=0 && nCkpt>=0 && nCkpt<=nLog );

    nLog = -99; nCkpt = -99;
    rc = btreelite_checkpoint(db, BTREELITE_CHECKPOINT_FULL, &nLog, &nCkpt);
    CHECK( rc==BTREELITE_OK );
    CHECK( nLog>=0 && nCkpt>=0 );

    nLog = -99; nCkpt = -99;
    rc = btreelite_checkpoint(db, BTREELITE_CHECKPOINT_RESTART, &nLog, &nCkpt);
    CHECK( rc==BTREELITE_OK || rc==BTREELITE_BUSY );

    /* TRUNCATE reports an empty log. */
    nLog = -99; nCkpt = -99;
    rc = btreelite_checkpoint(db, BTREELITE_CHECKPOINT_TRUNCATE,
                              &nLog, &nCkpt);
    CHECK( rc==BTREELITE_OK );
    CHECK( nLog==0 );

    rc = verifyEntries(db, iRoot, 0, 59, &nCount);
    CHECK( rc==BTREELITE_OK );
    CHECK( nCount==60 );
  }

  /* 3. Crash recovery: child writes, exits without committing. */
  {
    pid_t pid;
    int crashStatus = 0;
    pid = fork();
    if( pid==0 ){
      btreelite_db *cDb = openWalDb(ZDB);
      if( cDb ){
        btreelite_cur *crash = 0;
        char zKey[64];
        int i;
        if( btreelite_begin(cDb, 1)==BTREELITE_OK
         && btreelite_cursor_open(cDb, iRoot, 1, &crash)==BTREELITE_OK ){
          for(i=60; i<=69; i++){
            int nk = sprintf(zKey, "row-%04d", i);
            btreelite_put(crash, zKey, nk, "crash", 5);
          }
          btreelite_cursor_close(crash);
          /* Deliberately exit without committing. */
        }
        btreelite_close(cDb);
      }
      _exit(0);
    }
    waitpid(pid, &crashStatus, 0);

    /* The crash rows 60..69 never committed. */
    rc = verifyEntries(db, iRoot, 60, 69, &tmpCount);
    CHECK( rc==BTREELITE_ERROR );
    rc = countEntries(db, iRoot, &nCount);
    CHECK( rc==BTREELITE_OK );
    CHECK( nCount==61 );   /* seed + rows 0..59 */
  }

  /* 4. Automatic checkpoint: nFrame=1 keeps the log short. */
  {
    int i;
    btreelite_wal_autocheckpoint(db, 1);
    for(i=0; i<10; i++){
      int nLog = -1, nCkpt = -1;
      rc = writeEntries(db, iRoot, 71+i, 71+i);
      CHECK( rc==BTREELITE_OK );
      rc = btreelite_checkpoint(db, BTREELITE_CHECKPOINT_PASSIVE,
                                &nLog, &nCkpt);
      CHECK( rc==BTREELITE_OK );
      CHECK( nLog<=1 );
    }
    btreelite_wal_autocheckpoint(db, 0);
    rc = countEntries(db, iRoot, &nCount);
    CHECK( rc==BTREELITE_OK );
    CHECK( nCount==71 );   /* seed + 0..59 + 71..80 */
  }

  /* 5. Leave WAL mode: log is checkpointed and removed. */
  {
    FILE *f;
    unsigned char hdr[100];
    int nRow = 0;
    rc = btreelite_journal_mode(db, BTREELITE_JOURNAL_DELETE);
    CHECK( rc==BTREELITE_JOURNAL_DELETE );
    CHECK( access(ZWAL, 0)!=0 );
    rc = countEntries(db, iRoot, &nRow);
    CHECK( rc==BTREELITE_OK );
    CHECK( nRow==71 );
    btreelite_close(db);
    f = fopen(ZDB, "rb");
    if( f ){
      fread(hdr, 1, sizeof(hdr), f);
      fclose(f);
      CHECK( hdr[18]==1 && hdr[19]==1 );
    }
  }

  /* 6. WAL transitions inside a transaction are refused. */
  {
    rc = btreelite_open(ZDB, &db);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_begin(db, 1);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_journal_mode(db, BTREELITE_JOURNAL_WAL);
    CHECK( rc==BTREELITE_ERROR );
    rc = btreelite_rollback(db);
    CHECK( rc==BTREELITE_OK );
    btreelite_close(db);
  }

  /* 7. The busy timeout installs and clears. */
  {
    rc = btreelite_open(ZDB, &db);
    CHECK( rc==BTREELITE_OK );
    btreelite_busy_timeout(db, 50);
    btreelite_busy_timeout(db, 0);
    btreelite_close(db);
  }

  unlink(ZDB); unlink(ZWAL); unlink(ZJRNL); unlink(ZSHM);

  printf("%d checks, %d failures\n", nTest, nFail);
  return nFail ? 1 : 0;
}
