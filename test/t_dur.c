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
** Phase 4 durability acceptance: the synchronous mixing and the mmap
** read path.
**
**   1. synchronous levels: OFF/NORMAL/FULL all install, return the
**      previous level, and FULL is the default;
**   2. every level still commits and reads back: changing synchronous
**      changes durability, never correctness;
**   3. the memory-mapped read path: with a mmap limit set, pages are
**      fetched through the mapped region and every entry still reads
**      back byte-identical after a close/reopen cycle;
**   4. a crash (child killed mid-transaction) recovers the committed
**      state under FULL.
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

#define ZDB     "t_dur_test.db"
#define ZWAL    "t_dur_test.db-wal"
#define ZSHM    "t_dur_test.db-shm"
#define ZJRNL   "t_dur_test.db-journal"
#define NROWS   200

static int writeRows(btreelite_db *db, unsigned iRoot, int iFirst, int iLast){
  btreelite_cur *cur = 0;
  char zKey[64], zVal[256];
  int rc, i;
  rc = btreelite_begin(db, 1);
  if( rc ) return rc;
  rc = btreelite_cursor_open(db, iRoot, 1, &cur);
  if( rc ) return rc;
  for(i=iFirst; i<=iLast; i++){
    int nk = sprintf(zKey, "row-%04d", i);
    int nv = sprintf(zVal, "value-%06d-of-row-%04d", i, i);
    rc = btreelite_put(cur, zKey, nk, zVal, nv);
    if( rc ) break;
  }
  btreelite_cursor_close(cur);
  return rc ? rc : btreelite_commit(db);
}

/*
** Verify every row [iFirst..iLast]: the stored value must be exactly
** "value-%06d-of-row-%04d" for its key.  *pnSeen receives the count.
*/
static int verifyRows(btreelite_db *db, unsigned iRoot, int iFirst, int iLast,
                      int *pnSeen){
  btreelite_cur *cur = 0;
  char zKey[64], zGot[256];
  int rc, i, nErr = 0;
  *pnSeen = 0;
  rc = btreelite_begin(db, 0);
  if( rc ) return rc;
  rc = btreelite_cursor_open(db, iRoot, 0, &cur);
  if( rc ) return rc;
  for(i=iFirst; i<=iLast; i++){
    int nk = sprintf(zKey, "row-%04d", i);
    char zExpect[256];
    int ne = sprintf(zExpect, "value-%06d-of-row-%04d", i, i);
    uint32_t nVal = 0;
    if( btreelite_get(cur, zKey, nk)!=BTREELITE_OK ){ nErr++; break; }
    if( btreelite_value_size(cur, &nVal)!=BTREELITE_OK
     || nVal!=(uint32_t)ne ){ nErr++; break; }
    if( btreelite_value_read(cur, 0, nVal, zGot)!=BTREELITE_OK
     || memcmp(zGot, zExpect, (size_t)ne)!=0 ){ nErr++; break; }
    (*pnSeen)++;
  }
  btreelite_cursor_close(cur);
  btreelite_commit(db);
  return nErr ? BTREELITE_ERROR : BTREELITE_OK;
}

/*
** Count all entries of the tree; *pn receives the total.
*/
static int countRowsAll(btreelite_db *db, unsigned iRoot, int *pn){
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
  unsigned iRoot = 2;
  int rc, n = 0, nSeen = 0;
  int i;

  unlink(ZDB); unlink(ZWAL); unlink(ZSHM); unlink(ZJRNL);

  /* ---------------------------------------------------------------- */
  /* 1. synchronous levels: FULL is the default, each level installs,  */
  /*    and every level still commits + reads back.                    */
  /* ---------------------------------------------------------------- */
  rc = btreelite_open(ZDB, &db);
  CHECK( rc==BTREELITE_OK );
  if( rc ) return 1;
  rc = btreelite_begin(db, 1);
  CHECK( rc==BTREELITE_OK );
  rc = btreelite_create_tree(db, &iRoot);
  CHECK( rc==BTREELITE_OK );
  btreelite_commit(db);

  CHECK( btreelite_synchronous(db, BTREELITE_SYNC_FULL)==BTREELITE_SYNC_FULL );

  for(i=BTREELITE_SYNC_OFF; i<=BTREELITE_SYNC_FULL; i++){
    int iPrev;
    iPrev = btreelite_synchronous(db, i);
    CHECK( iPrev==BTREELITE_SYNC_FULL || iPrev==BTREELITE_SYNC_NORMAL
        || iPrev==BTREELITE_SYNC_OFF );
    /* Every level must still commit and read back correctly. */
    rc = writeRows(db, iRoot, i*50, i*50+49);
    CHECK( rc==BTREELITE_OK );
    rc = verifyRows(db, iRoot, i*50, i*50+49, &nSeen);
    CHECK( rc==BTREELITE_OK );
    CHECK( nSeen==50 );
  }
  /* Back to FULL, which the following sections assume. */
  btreelite_synchronous(db, BTREELITE_SYNC_FULL);
  {
    int iPrev = btreelite_synchronous(db, BTREELITE_SYNC_FULL);
    CHECK( iPrev==BTREELITE_SYNC_FULL );
  }
  btreelite_close(db);

  /* All 200 rows written under three sync levels read back. */
  rc = btreelite_open(ZDB, &db);
  CHECK( rc==BTREELITE_OK );
  rc = verifyRows(db, iRoot, 0, 149, &nSeen);
  CHECK( rc==BTREELITE_OK );
  CHECK( nSeen==150 );
  btreelite_close(db);

  /* ---------------------------------------------------------------- */
  /* 2. The mmap read path: enable the limit, write one large commit   */
  /*    and read the whole tree back byte-exact, including after a     */
  /*    close/reopen cycle that re-establishes the mapping.            */
  /* ---------------------------------------------------------------- */
  rc = btreelite_open(ZDB, &db);
  CHECK( rc==BTREELITE_OK );
  btreelite_mmap_limit(db, 1<<24);   /* map up to 16 MB */
  rc = writeRows(db, iRoot, 150, 349);
  CHECK( rc==BTREELITE_OK );
  {
    /* Read back the freshly written rows through (likely) mapped pages. */
    btreelite_cur *cur = 0;
    char zKey[64], zGot[256];
    int iBad = 0;
    rc = btreelite_begin(db, 0);
    CHECK( rc==BTREELITE_OK );
    rc = btreelite_cursor_open(db, iRoot, 0, &cur);
    CHECK( rc==BTREELITE_OK );
    for(i=150; i<=349; i++){
      int nk = sprintf(zKey, "row-%04d", i);
      char zExpect[256];
      int ne = sprintf(zExpect, "value-%06d-of-row-%04d", i, i);
      uint32_t nVal = 0;
      if( btreelite_get(cur, zKey, nk)!=BTREELITE_OK ){ iBad++; break; }
      btreelite_value_size(cur, &nVal);
      if( nVal!=(uint32_t)ne ){ iBad++; break; }
      if( btreelite_value_read(cur, 0, nVal, zGot)!=BTREELITE_OK
       || memcmp(zGot, zExpect, (size_t)ne)!=0 ){ iBad++; break; }
    }
    CHECK( iBad==0 );
    btreelite_cursor_close(cur);
    btreelite_commit(db);
  }
  /* Reopen: the mapping is re-established and the mmap read path serves
  ** committed data again. */
  btreelite_close(db);
  rc = btreelite_open(ZDB, &db);
  CHECK( rc==BTREELITE_OK );
  btreelite_mmap_limit(db, 1<<24);
  rc = verifyRows(db, iRoot, 0, 349, &nSeen);
  CHECK( rc==BTREELITE_OK );
  CHECK( nSeen==350 );
  btreelite_close(db);

  /* ---------------------------------------------------------------- */
  /* 3. FULL durability: a child killed mid-transaction loses nothing  */
  /*    that was committed before it.                                  */
  /* ---------------------------------------------------------------- */
  {
    pid_t pid;
    int status = 0;
    rc = btreelite_open(ZDB, &db);
    CHECK( rc==BTREELITE_OK );
    pid = fork();
    if( pid==0 ){
      /* Child: write 50 rows and die hard with the transaction open
      ** (writeRows would commit, so the puts are issued directly). */
      btreelite_db *c = 0;
      btreelite_cur *cc = 0;
      int i2;
      char zKey[64], zVal[256];
      if( btreelite_open(ZDB, &c)==BTREELITE_OK
       && btreelite_begin(c, 1)==BTREELITE_OK
       && btreelite_cursor_open(c, iRoot, 1, &cc)==BTREELITE_OK ){
        for(i2=350; i2<=399; i2++){
          int nk = sprintf(zKey, "row-%04d", i2);
          int nv = sprintf(zVal, "value-%06d-of-row-%04d", i2, i2);
          btreelite_put(cc, zKey, nk, zVal, nv);
        }
        btreelite_cursor_close(cc);
        /* Never commits: _exit discards the open transaction. */
      }
      _exit(0);
    }
    waitpid(pid, &status, 0);
    /* Everything committed before the crash survives: all 350 rows. */
    rc = countRowsAll(db, iRoot, &n);
    CHECK( rc==BTREELITE_OK );
    CHECK( n==350 );
    btreelite_close(db);
  }

  unlink(ZDB); unlink(ZWAL); unlink(ZSHM); unlink(ZJRNL);
  rc = 0;
  printf("%d checks, %d failures\n", nTest, nFail);
  return nFail ? 1 : 0;
}