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
** Implementation of the btreelite public API (btreelite.h).
**
** A btreelite_db owns one Btree handle backed by one file and one
** connection object.  A btreelite_cur wraps a btree cursor opened on one
** tree.
**
** KV cells use the leaf-data page type and hold a byte-string key beside
** an independent value:
**
**   leaf:      [varint nValue][varint nKeyLen][key][value][ovfl?]
**   interior:  [4-byte child][varint nKeyLen][key]
**
** Keys are always local, so lookups never read overflow pages.  Only the
** value may spill onto overflow pages.  Key order is binary comparison
** with a length tiebreak.
*/
#include "btreeliteInt.h"
#include "btree.h"
#include "../include/btreelite.h"

struct btreelite_db {
  sqlite3 env;
  Btree *pBt;
  int iSync;              /* Current BTREELITE_SYNC_* level (FULL by default) */
};

struct btreelite_cur {
  BtCursor *pCur;
  Pgno iRoot;
};

/* Run the WAL autocheckpoint hook if a commit since the last pass added
** frames to the WAL.  Defined below; invoked by commit/rollback below. */
static void btreeliteWalCallback(btreelite_db *p);

void btreelite_free(void *p){ sqlite3_free(p); }

/*
** Reject an out-of-range key length.  Keys live inline on the btree page,
** so an over-long key would otherwise corrupt the cell.
*/
static int kvCheckKey(const void *k, int nK){
  if( nK<0 || nK>BTREELITE_MAX_KEY ) return BTREELITE_TOOBIG;
  if( nK>0 && k==0 ) return BTREELITE_ERROR;
  return BTREELITE_OK;
}

/*
** Run one KV moveto.  The key is the raw byte string (k,nK); keys are held
** locally so this never touches an overflow page.  *pRes follows the
** sqlite3BtreeTableMoveto convention.
*/
static int kvMoveto(btreelite_cur *c, const void *k, int nK, int *pRes){
  int rc = kvCheckKey(k, nK);
  if( rc!=BTREELITE_OK ) return rc;
  return sqlite3BtreeKvMoveto(c->pCur, k, nK, 0, pRes);
}

/* ---------------------------------------------------------------- */
/* Database lifecycle                                               */
/* ---------------------------------------------------------------- */

int btreelite_open(const char *zPath, btreelite_db **ppDb){
  int rc;
  int vfsFlags = SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_MAIN_DB;
  *ppDb = 0;
  rc = sqlite3_initialize();
  if( rc!=SQLITE_OK ) return rc;
  *ppDb = (btreelite_db*)sqlite3MallocZero(sizeof(btreelite_db));
  if( *ppDb==0 ) return BTREELITE_NOMEM;
  (*ppDb)->env.mutex = sqlite3MutexAlloc(SQLITE_MUTEX_RECURSIVE);
  (*ppDb)->env.aDb = (Db*)sqlite3MallocZero(sizeof(Db));
  (*ppDb)->env.nDb = 1;
  (*ppDb)->env.errMask = 0xff;
  (*ppDb)->iSync = BTREELITE_SYNC_FULL;   /* SQLite's default */
  sqlite3_mutex_enter((*ppDb)->env.mutex);
  rc = sqlite3BtreeOpen(sqlite3_vfs_find(0), zPath, &(*ppDb)->env,
                        &(*ppDb)->pBt, 0, vfsFlags);
  if( rc==SQLITE_OK ){
    /* WAL is the default.  journal_mode() falls back to MEMORY by itself
    ** for an in-memory database or an environment that cannot host a WAL. */
    btreelite_journal_mode(*ppDb, BTREELITE_JOURNAL_WAL);
  }else{
    sqlite3_mutex_leave((*ppDb)->env.mutex);
    sqlite3_mutex_free((*ppDb)->env.mutex);
    sqlite3_free((*ppDb)->env.aDb);
    sqlite3_free(*ppDb);
    *ppDb = 0;
  }
  return rc;
}

void btreelite_close(btreelite_db *p){
  if( p==0 ) return;
  sqlite3BtreeClose(p->pBt);
  sqlite3_mutex_leave(p->env.mutex);
  sqlite3_mutex_free(p->env.mutex);
  sqlite3_free(p->env.aDb);
  sqlite3_free(p);
}

/* ---------------------------------------------------------------- */
/* Trees                                                            */
/* ---------------------------------------------------------------- */

int btreelite_create_tree(btreelite_db *p, unsigned *piRoot){
  Pgno iRoot = 0;
  int rc;
  if( p==0 ) return BTREELITE_ERROR;
  rc = sqlite3BtreeCreateTable(p->pBt, &iRoot, BTREE_BLOBKEY);
  if( rc==SQLITE_OK && piRoot ) *piRoot = (unsigned)iRoot;
  return rc;
}

int btreelite_clear_tree(btreelite_db *p, unsigned iRoot){
  if( p==0 ) return BTREELITE_ERROR;
  return sqlite3BtreeClearTable(p->pBt, (int)iRoot, 0);
}

unsigned btreelite_page_count(btreelite_db *p){
  if( p==0 ) return 0;
  return (unsigned)sqlite3BtreeLastPage(p->pBt);
}

/* ---------------------------------------------------------------- */
/* Transactions                                                     */
/* ---------------------------------------------------------------- */

int btreelite_begin(btreelite_db *p, int wrflag){
  int rc;
  if( p==0 ) return BTREELITE_ERROR;
  if( wrflag<0 || wrflag>2 ) return BTREELITE_ERROR;
  rc = sqlite3BtreeBeginTrans(p->pBt, wrflag, 0);
  if( rc==SQLITE_OK && wrflag && sqlite3BtreeTxnState(p->pBt)==SQLITE_TXN_WRITE ){
    /* A nested write-begin opens one more savepoint. */
    p->env.nSavepoint++;
  }
  return rc;
}
int btreelite_commit(btreelite_db *p){
  int rc;
  if( p==0 ) return BTREELITE_ERROR;
  rc = sqlite3BtreeCommit(p->pBt);
  if( rc==SQLITE_OK ){ p->env.nSavepoint = 0; btreeliteWalCallback(p); }
  return rc;
}
int btreelite_rollback(btreelite_db *p){
  int rc;
  if( p==0 ) return BTREELITE_ERROR;
  rc = sqlite3BtreeRollback(p->pBt, SQLITE_OK, 0);
  if( rc==SQLITE_OK ){ p->env.nSavepoint = 0; btreeliteWalCallback(p); }
  return rc;
}
int btreelite_txn_state(btreelite_db *p){
  if( p==0 ) return 0;
  return sqlite3BtreeTxnState(p->pBt);
}

int btreelite_savepoint(btreelite_db *p, int op, int iSavepoint){
  int iBt, rc;
  if( p==0 ) return BTREELITE_ERROR;
  if( op!=BTREELITE_SAVEPOINT_RELEASE && op!=BTREELITE_SAVEPOINT_ROLLBACK ){
    return BTREELITE_ERROR;
  }
  if( sqlite3BtreeTxnState(p->pBt)!=SQLITE_TXN_WRITE ){
    return BTREELITE_ERROR;
  }
  /* iSavepoint counts from 0 for the outermost savepoint, matching the
  ** SQL SAVEPOINT numbering. */
  if( iSavepoint<0 || iSavepoint>=p->env.nSavepoint ){
    return BTREELITE_ERROR;
  }
  /* The outermost savepoint *is* the write transaction (the first
  ** btreelite_begin() began it), so releasing it commits, and rolling
  ** back to it undoes the entire transaction while leaving it open. */
  if( iSavepoint==0 ){
    if( op==BTREELITE_SAVEPOINT_RELEASE ){
      return btreelite_commit(p);
    }
    /* sqlite3PagerSavepoint() treats index -1 as a full-transaction
    ** playback that keeps the write transaction open. */
    rc = sqlite3BtreeSavepoint(p->pBt, SAVEPOINT_ROLLBACK, -1);
    if( rc==SQLITE_OK ) p->env.nSavepoint = 1;
    return rc;
  }
  /* Pager savepoints are absolute indices counted from the outermost one:
  ** the first nested btreelite_begin() opened pager savepoint 0, the
  ** second opened 1, and so on.  API savepoint i therefore maps to pager
  ** savepoint i-1.  (A previous version computed the "distance from the
  ** innermost savepoint" instead, which mirrored the numbering: rolling
  ** back to savepoint i silently restored the state of savepoint N-i.)
  */
  iBt = iSavepoint - 1;
  rc = sqlite3BtreeSavepoint(p->pBt,
                op==BTREELITE_SAVEPOINT_ROLLBACK ? SAVEPOINT_ROLLBACK
                                                 : SAVEPOINT_RELEASE, iBt);
  if( rc==SQLITE_OK ){
    /* A ROLLBACK re-opens the target savepoint, so i savepoints plus the
    ** outermost transaction remain; a RELEASE destroys the target and
    ** everything nested inside it, leaving iSavepoint savepoints. */
    p->env.nSavepoint = op==BTREELITE_SAVEPOINT_RELEASE ? iSavepoint
                                                       : iSavepoint+1;
  }
  return rc;
}

/* ---------------------------------------------------------------- */
/* Cursors                                                          */
/* ---------------------------------------------------------------- */

int btreelite_cursor_open(btreelite_db *p, unsigned iRoot, int wrFlag,
                          btreelite_cur **ppCur){
  int rc;
  btreelite_cur *c;
  if( p==0 || ppCur==0 ) return BTREELITE_ERROR;
  *ppCur = 0;
  c = (btreelite_cur*)sqlite3MallocZero(sizeof(btreelite_cur));
  if( c==0 ) return BTREELITE_NOMEM;
  c->iRoot = iRoot;
  c->pCur = (BtCursor*)sqlite3Malloc(sqlite3BtreeCursorSize());
  if( c->pCur==0 ){ sqlite3_free(c); return BTREELITE_NOMEM; }
  sqlite3BtreeCursorZero(c->pCur);
  /* KV trees use table-btree semantics with a byte-string key, so the
  ** cursor carries no KeyInfo (pKeyInfo==0). */
  rc = sqlite3BtreeCursor(p->pBt, (Pgno)iRoot,
                          wrFlag ? BTREE_WRCSR : 0, 0, c->pCur);
  if( rc!=SQLITE_OK ){
    sqlite3_free(c->pCur);
    sqlite3_free(c);
    return rc;
  }
  *ppCur = c;
  return BTREELITE_OK;
}

void btreelite_cursor_close(btreelite_cur *c){
  if( c==0 ) return;
  if( c->pCur ){ sqlite3BtreeCloseCursor(c->pCur); sqlite3_free(c->pCur); }
  sqlite3_free(c);
}

/* ---------------------------------------------------------------- */
/* CRUD                                                             */
/* ---------------------------------------------------------------- */

int btreelite_put(btreelite_cur *c, const void *k, int nK,
                  const void *v, int nV){
  BtreePayload x;
  int rc, loc = 0;
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  rc = kvCheckKey(k, nK);
  if( rc!=BTREELITE_OK ) return rc;
  if( nV<0 || (nV>0 && v==0) ) return BTREELITE_ERROR;
  memset(&x, 0, sizeof(x));
  x.pKey = k;
  x.nKey = nK;
  x.pData = v;
  x.nData = nV;
  /* Let sqlite3BtreeInsert locate the insertion point itself: the
  ** BTREE_APPEND hint biases its search toward the highest cell, which is
  ** a pure hint (safe for keys in any order) and puts monotone key traffic
  ** -- the common KV case -- on the append fast path. */
  rc = sqlite3BtreeInsert(c->pCur, &x, BTREE_APPEND, 0);
  if( rc!=SQLITE_OK ) return rc;
  /* Reposition on the stored entry: sqlite3BtreeInsert leaves the cursor
  ** in an arbitrary state after a balance, and "cursor at the stored
  ** entry" is the contract the value accessors rely on. */
  rc = kvMoveto(c, k, nK, &loc);
  return rc;
}

int btreelite_get(btreelite_cur *c, const void *k, int nK){
  int rc, res = 0;
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  rc = kvMoveto(c, k, nK, &res);
  if( rc!=SQLITE_OK ) return rc;
  return res==0 ? BTREELITE_OK : BTREELITE_NOTFOUND;
}

int btreelite_checkpoint(btreelite_db *p, int eMode, int *pnLog, int *pnCkpt){
  if( p==0 ) return BTREELITE_ERROR;
  return sqlite3BtreeCheckpoint(p->pBt, eMode, pnLog, pnCkpt);
}

/*
** One btreeliteWalCallback() pass: collect the number of frames the last
** commit added to the WAL and pass it to the hook, which checkpoints when
** the count reaches the configured threshold (upstream doWalCallbacks).
*/
static void btreeliteWalCallback(btreelite_db *p){
  int nEntry;
  Btree *pBt = p->pBt;
  sqlite3BtreeEnter(pBt);
  nEntry = sqlite3PagerWalCallback(sqlite3BtreePager(pBt));
  sqlite3BtreeLeave(pBt);
  if( nEntry>0 && p->env.xWalCallback!=0 ){
    p->env.xWalCallback(p->env.pWalArg, &p->env, 0, nEntry);
  }
}

int btreelite_journal_mode(btreelite_db *p, int eMode){
  int eOld, rc;
  Pager *pPager;
  if( p==0 ) return BTREELITE_ERROR;
  if( eMode!=BTREELITE_JOURNAL_WAL
   && eMode!=BTREELITE_JOURNAL_MEMORY ){
    return BTREELITE_ERROR;
  }
  pPager = sqlite3BtreePager(p->pBt);

  /* WAL needs a shared-memory index, so an in-memory database (and any
  ** environment whose VFS cannot host one) falls back to MEMORY. */
  if( eMode==BTREELITE_JOURNAL_WAL
   && (sqlite3PagerIsMemdb(pPager) || !sqlite3PagerWalSupported(pPager)) ){
    eMode = BTREELITE_JOURNAL_MEMORY;
  }

  eOld = sqlite3PagerGetJournalMode(pPager);
  if( eMode==eOld ) return eMode;

  /* WAL transitions must happen outside a transaction.  Upstream
  ** OP_JournalMode refuses them with a plain error. */
  if( sqlite3BtreeTxnState(p->pBt)!=SQLITE_TXN_NONE ){
    return BTREELITE_ERROR;
  }

  /* Upstream OP_JournalMode closes the log (checkpointing it) before
  ** switching out of WAL.  MEMORY->WAL additionally passes through OFF:
  ** the pager cannot frame an in-memory journal. */
  if( eOld==PAGER_JOURNALMODE_WAL ){
    rc = sqlite3PagerCloseWal(pPager, &p->env);
  }else if( eOld==PAGER_JOURNALMODE_MEMORY && eMode==BTREELITE_JOURNAL_WAL ){
    sqlite3PagerSetJournalMode(pPager, PAGER_JOURNALMODE_OFF);
    rc = SQLITE_OK;
  }else{
    rc = SQLITE_OK;
  }
  if( rc!=SQLITE_OK ) return sqlite3PagerGetJournalMode(pPager);

  rc = sqlite3BtreeSetVersion(p->pBt,
                  eMode==BTREELITE_JOURNAL_WAL ? 2 : 1);
  {
    int rc2 = SQLITE_OK;
    if( rc==SQLITE_OK
     && sqlite3BtreeTxnState(p->pBt)!=SQLITE_TXN_NONE ){
      /* sqlite3BtreeSetVersion() leaves its implicit write transaction
      ** open; commit it so the version bytes reach the file.  Upstream
      ** gets this from statement finalization, which btreelite does
      ** not have. */
      rc2 = sqlite3BtreeCommit(p->pBt);
    }
    if( rc2!=SQLITE_OK ){
      return sqlite3PagerGetJournalMode(pPager);
    }
  }
  if( eMode==BTREELITE_JOURNAL_WAL ){
    /* Open the WAL connection eagerly.  lockBtree opens it on a page-1
    ** read, but that read is skipped whenever pPage1 is already cached,
    ** so the WAL would otherwise stay closed on this handle. */
    int isOpen = 0;
    rc = sqlite3PagerOpenWal(pPager, &isOpen);
    if( rc!=SQLITE_OK ) return sqlite3PagerGetJournalMode(pPager);
  }
  return sqlite3PagerSetJournalMode(pPager, eMode);
}

void btreelite_wal_autocheckpoint(btreelite_db *p, int nFrames){
  if( p==0 ) return;
  if( nFrames>0 ){
    p->env.xWalCallback = sqlite3WalDefaultHook;
    p->env.pWalArg = SQLITE_INT_TO_PTR(nFrames);
  }else{
    p->env.xWalCallback = 0;
    p->env.pWalArg = 0;
  }
}

void btreelite_set_interrupt(btreelite_db *p){
  if( p ) AtomicStore(&p->env.u1.isInterrupted, 1);
}

void btreelite_busy_timeout(btreelite_db *p, int ms){
  if( p==0 ) return;
  sqlite3_busy_timeout(&p->env, ms);
}

int btreelite_del(btreelite_cur *c, const void *k, int nK){
  int rc, res = 0;
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  rc = kvMoveto(c, k, nK, &res);
  if( rc!=SQLITE_OK ) return rc;
  if( res!=0 ) return BTREELITE_NOTFOUND;
  return sqlite3BtreeDelete(c->pCur, 0);
}

int btreelite_seek(btreelite_cur *c, const void *k, int nK){
  int rc, res = 0;
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  rc = kvMoveto(c, k, nK, &res);
  if( rc!=SQLITE_OK ) return rc;
  if( res<0 ){
    /* Parked before the key: step to the first entry >= key. */
    rc = sqlite3BtreeNext(c->pCur, 0);
    if( rc==SQLITE_DONE ) return BTREELITE_OK;
    if( rc!=SQLITE_OK ) return rc;
  }
  return BTREELITE_OK;
}

int btreelite_eof(btreelite_cur *c){
  if( c==0 || c->pCur==0 ) return 1;
  return sqlite3BtreeEof(c->pCur);
}

int btreelite_first(btreelite_cur *c, int *pRes){
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  return sqlite3BtreeFirst(c->pCur, pRes);
}

int btreelite_last(btreelite_cur *c, int *pRes){
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  return sqlite3BtreeLast(c->pCur, pRes);
}

int btreelite_next(btreelite_cur *c){
  int rc;
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  rc = sqlite3BtreeNext(c->pCur, 0);
  return rc==SQLITE_DONE ? BTREELITE_DONE : rc;
}

int btreelite_prev(btreelite_cur *c){
  int rc;
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  rc = sqlite3BtreePrevious(c->pCur, 0);
  return rc==SQLITE_DONE ? BTREELITE_DONE : rc;
}

/* ---------------------------------------------------------------- */
/* Reading the current entry                                        */
/* ---------------------------------------------------------------- */

int btreelite_key(btreelite_cur *c, void *buf, int nMax, int *pnLen){
  u32 nKey = 0;
  const void *pKey;
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  if( sqlite3BtreeEof(c->pCur) ) return BTREELITE_ERROR;
  /* The key is held locally, so a single fetch suffices. */
  pKey = sqlite3BtreeKvKey(c->pCur, &nKey);
  if( pnLen ) *pnLen = (int)nKey;
  {
    u32 nCopy = nKey;
    if( nCopy>(u32)nMax ) nCopy = (u32)nMax;
    if( nCopy ) memcpy(buf, pKey, (size_t)nCopy);
  }
  return BTREELITE_OK;
}

int btreelite_value_size(btreelite_cur *c, uint32_t *pnVal){
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  if( sqlite3BtreeEof(c->pCur) ) return BTREELITE_ERROR;
  *pnVal = sqlite3BtreeKvValueSize(c->pCur);
  return BTREELITE_OK;
}

int btreelite_value_read(btreelite_cur *c, uint32_t offset, uint32_t amt,
                         void *pBuf){
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  if( sqlite3BtreeEof(c->pCur) ) return BTREELITE_ERROR;
  return sqlite3BtreeKvValueRead(c->pCur, offset, amt, pBuf);
}

const void *btreelite_value_fetch(btreelite_cur *c, int *pAmt){
  u32 nAmt = 0;
  const void *p;
  if( c==0 || c->pCur==0 || sqlite3BtreeEof(c->pCur) ) return 0;
  p = sqlite3BtreeKvValueFetch(c->pCur, &nAmt);
  if( pAmt ) *pAmt = (int)nAmt;
  return p;
}
/*
** Overwrite a range of the value the cursor points at, in place.
**
** The range must already lie inside the stored value: an incremental write
** cannot grow the value.  The (k,nK) argument repositions the cursor on the
** named entry, since the underlying incremental-blob cursor must sit on the
** row being written when sqlite3BtreePutData runs.
*/
int btreelite_value_write(btreelite_cur *c, const void *k, int nK,
                          uint32_t offset, uint32_t amt, const void *z){
  uint32_t nVal = 0;
  int rc, res = 0;
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  if( z==0 && amt>0 ) return BTREELITE_ERROR;
  rc = kvMoveto(c, k, nK, &res);
  if( rc!=SQLITE_OK ) return rc;
  if( res!=0 || sqlite3BtreeEof(c->pCur) ) return BTREELITE_NOTFOUND;
  rc = btreelite_value_size(c, &nVal);
  if( rc!=BTREELITE_OK ) return rc;
  if( (u64)offset+(u64)amt>(u64)nVal ){
    /* Incremental write cannot grow the value. */
    return BTREELITE_ERROR;
  }
  sqlite3BtreeIncrblobCursor(c->pCur);
  return sqlite3BtreePutData(c->pCur, offset, amt, (void*)z);
}
/*
** Run the btree integrity checker over the tree rooted at iRoot, or over
** page 1 plus the freelist when iRoot is 0 (btreelite has no catalog of
** tree roots, so "the whole file" degenerates to a freelist-and-page-1
** check).  A read transaction is opened for the duration.  *pzOut must be
** freed with btreelite_free().
*/
int btreelite_integrity_check(btreelite_db *p, unsigned iRoot, int mxErr,
                              int *pnErr, char **pzOut){
  Pgno aRoot[3];
  int nRoot;
  /* Counts are written via sqlite3MemSetArrayInt64, which is a no-op in
  ** btreelite, so the memory cells only need to be addressable storage. */
  char aCnt[3*128];
  int rc;

  if( p==0 || pnErr==0 || pzOut==0 ) return BTREELITE_ERROR;
  *pnErr = 0;
  *pzOut = 0;
  if( mxErr<1 ) mxErr = 1;
  nRoot = 0;
  if( iRoot==0 ){
    /* No catalog of tree roots exists, so "whole file" degenerates to a
    ** freelist-and-page-1 check: aRoot[0]==0 marks the partial form. */
    aRoot[nRoot++] = 0;
    aRoot[nRoot] = 1;
    nRoot++;
  }else if( iRoot!=1 ){
    /* Page 1 hosts the (always empty) schema tree in a KV file; including
    ** it keeps the checker's page-usage sweep from reporting it unused. */
    aRoot[nRoot++] = 0;
    aRoot[nRoot++] = 1;
    aRoot[nRoot++] = (Pgno)iRoot;
  }else{
    aRoot[nRoot++] = (Pgno)iRoot;
  }
  rc = btreelite_begin(p, 0);
  if( rc!=BTREELITE_OK ) return rc;
  memset(aCnt, 0, sizeof(aCnt));
  rc = (int)sqlite3BtreeIntegrityCheck(&p->env, p->pBt, aRoot, (Mem*)aCnt,
                      nRoot, mxErr, pnErr, pzOut);
  if( rc!=SQLITE_OK && *pzOut ){ btreelite_free(*pzOut); *pzOut = 0; }
  btreelite_commit(p);
  return rc;
}

/*
** Bytes of heap currently acquired by the allocator (malloc.c keeps the
** running total in SQLITE_STATUS_MEMORY_USED), which includes the pager
** and page-cache for this connection in the single-connection case.
*/
int btreelite_mem_used(btreelite_db *p){
  if( p==0 ) return BTREELITE_ERROR;
  return (int)sqlite3StatusValue(SQLITE_STATUS_MEMORY_USED);
}
/*
** Select the synchronization level used to commit transactions and
** checkpoints, and return the previously set level.  Internally this maps
** to the PAGER_SYNCHRONOUS_* levels SQLite's PRAGMA synchronous drives
** (the public values are the zero-based PRAGMA numbering, the pager wants
** a one-based flag).
*/
int btreelite_synchronous(btreelite_db *p, int level){
  int iOld;
  unsigned pgFlags;
  if( p==0 ) return BTREELITE_ERROR;
  if( level<BTREELITE_SYNC_OFF || level>BTREELITE_SYNC_FULL ){
    return BTREELITE_ERROR;
  }
  /* The pager keeps no getter; the previous level is tracked on the
  ** handle (initialized to FULL, SQLite's default). */
  iOld = p->iSync;
  pgFlags = 1 + (unsigned)level;   /* PAGER_SYNCHRONOUS_OFF=0x01 */
  sqlite3BtreeSetPagerFlags(p->pBt, pgFlags);
  p->iSync = level;
  return iOld;
}

#if SQLITE_MAX_MMAP_SIZE>0
void btreelite_mmap_limit(btreelite_db *p, long nLimit){
  if( p==0 || nLimit<0 ) return;
  sqlite3BtreeSetMmapLimit(p->pBt, (sqlite3_int64)nLimit);
}
#else
void btreelite_mmap_limit(btreelite_db *p, long nLimit){
  (void)p; (void)nLimit;
}
#endif
