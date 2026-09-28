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
};

struct btreelite_cur {
  BtCursor *pCur;
  Pgno iRoot;
};

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
  sqlite3_mutex_enter((*ppDb)->env.mutex);
  rc = sqlite3BtreeOpen(sqlite3_vfs_find(0), zPath, &(*ppDb)->env,
                        &(*ppDb)->pBt, 0, vfsFlags);
  if( rc!=SQLITE_OK ){
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
  if( p==0 ) return BTREELITE_ERROR;
  if( wrflag<0 || wrflag>2 ) return BTREELITE_ERROR;
  return sqlite3BtreeBeginTrans(p->pBt, wrflag, 0);
}
int btreelite_commit(btreelite_db *p){
  if( p==0 ) return BTREELITE_ERROR;
  return sqlite3BtreeCommit(p->pBt);
}
int btreelite_rollback(btreelite_db *p){
  if( p==0 ) return BTREELITE_ERROR;
  return sqlite3BtreeRollback(p->pBt, SQLITE_OK, 0);
}
int btreelite_txn_state(btreelite_db *p){
  if( p==0 ) return 0;
  return sqlite3BtreeTxnState(p->pBt);
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
  /* Locate the insertion point; the returned value is the seekResult
  ** sqlite3BtreeInsert expects. */
  rc = kvMoveto(c, k, nK, &loc);
  if( rc!=SQLITE_OK ) return rc;
  memset(&x, 0, sizeof(x));
  x.pKey = k;
  x.nKey = nK;
  x.pData = v;
  x.nData = nV;
  rc = sqlite3BtreeInsert(c->pCur, &x, 0, loc);
  if( rc!=SQLITE_OK ) return rc;
  /* Reposition on the stored entry. */
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

int btreelite_journal_mode(btreelite_db *p, int eMode){
  if( p==0 ) return BTREELITE_ERROR;
  return sqlite3PagerSetJournalMode(sqlite3BtreePager(p->pBt), eMode);
}

void btreelite_set_interrupt(btreelite_db *p){
  if( p ) AtomicStore(&p->env.u1.isInterrupted, 1);
}

void btreelite_busy_timeout(btreelite_db *p, int ms){
  (void)p; (void)ms;
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
