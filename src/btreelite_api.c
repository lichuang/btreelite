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
** tree.  A KV record is stored as an index-btree cell whose payload is
** the record [varint nKeyBytes][key][value]; records are encoded on
** insert and split on read.  Key order is plain memcmp with a length
** tiebreak (kvCompare in btreelite_compat.c).
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
  KeyInfo *pKeyInfo;
  Pgno iRoot;
  int nKeyOff;
  int nKeyLen;
  int nValLen;
  int bValid;
};

int sqlite3KvEncode(const void *pKey, int nKey, const void *pVal, int nVal,
                    void **ppRec){
  u8 aTmp[9];
  int nHdr, nRec;
  u8 *pRec;
  if( nKey<0 || nVal<0 ) return BTREELITE_ERROR;
  nHdr = sqlite3PutVarint32(aTmp, (u32)nKey);
  nRec = nHdr + nKey + nVal;
  pRec = (u8*)sqlite3Malloc((u64)nRec);
  if( pRec==0 ) return BTREELITE_NOMEM;
  (void)sqlite3PutVarint32(pRec, (u32)nKey);
  if( nKey>0 ) memcpy(&pRec[nHdr], pKey, (size_t)nKey);
  if( nVal>0 ) memcpy(&pRec[nHdr+nKey], pVal, (size_t)nVal);
  *ppRec = pRec;
  return BTREELITE_OK;
}

void btreelite_free(void *p){ sqlite3_free(p); }

/*
** Decode the KV record header of the cursor's current cell and cache the
** key/value split.  The payload layout is [varint nKeyBytes][key][value].
*/
/*
** Build an UnpackedRecord whose search key is the raw byte string (k,nK)
** and run one IndexMoveto.  On return *pRes<0 means the parked cell
** sorts before the key, *pRes>0 after, 0 exact.
*/
static int kvMoveto(btreelite_cur *c, const void *k, int nK, int *pRes){
  UnpackedRecord r;
  memset(&r, 0, sizeof(r));
  r.pKeyInfo = c->pKeyInfo;
  r.u.z = (char*)k;
  r.n = nK;
  r.nField = 1;
  r.default_rc = 0;
  return sqlite3BtreeIndexMoveto(c->pCur, &r, pRes);
}

/*
** Decode the KV record of the cell the cursor rests on and cache the
** key/value split.  The payload layout is [varint nKeyBytes][key][value].
*/
static int kvCacheRefresh(btreelite_cur *c){
  u32 nPayload = sqlite3BtreePayloadSize(c->pCur);
  u8 aHead[16];
  int nAvail;
  u64 nKey;
  u8 *p, *pEnd;
  if( nPayload==0 ){ c->bValid = 0; return BTREELITE_OK; }
  nAvail = nPayload<(u32)sizeof(aHead) ? (int)nPayload : (int)sizeof(aHead);
  {
    int rc = sqlite3BtreePayload(c->pCur, 0, (u32)nAvail, aHead);
    if( rc!=SQLITE_OK ) return rc;
  }
  p = aHead;
  pEnd = &aHead[nAvail];
  nKey = *p;
  while( (*p)>=0x80 && p<pEnd ){
    nKey = (nKey<<7) | (u64)(*++p & 0x7f);
  }
  p++;
  c->nKeyOff = (int)(p - aHead);
  c->nKeyLen = (int)nKey;
  c->nValLen = (int)nPayload - c->nKeyOff - c->nKeyLen;
  if( c->nKeyLen<0 || c->nKeyOff+c->nKeyLen>(int)nPayload ){
    return BTREELITE_CORRUPT;
  }
  c->bValid = 1;
  return BTREELITE_OK;
}

static void kvCacheInvalidate(btreelite_cur *c){ c->bValid = 0; }

/*
** Move the cursor to the entry with key (k,nK) and refresh the split
** cache when the key is present.  *pRes follows the IndexMoveto
** contract; the caller decides what "not found" means for its op.
*/
static int kvLocate(btreelite_cur *c, const void *k, int nK, int *pRes){
  int rc;
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  rc = kvMoveto(c, k, nK, pRes);
  if( rc!=SQLITE_OK ) return rc;
  if( *pRes==0 ){
    rc = kvCacheRefresh(c);
    if( rc!=SQLITE_OK ) return rc;
  }else{
    kvCacheInvalidate(c);
  }
  return BTREELITE_OK;
}

/* ---------------------------------------------------------------- */
/* Database lifecycle                                               */
/* ---------------------------------------------------------------- */

int btreelite_open(const char *zPath, btreelite_db **ppDb){
  int rc;
  btreelite_db *p;
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
  /* btreeMoveto rejects keys whose field count exceeds nAllField; a KV
  ** search key is one field. */
  c->pKeyInfo = (KeyInfo*)sqlite3MallocZero(sizeof(KeyInfo));
  if( c->pKeyInfo ){
    c->pKeyInfo->nKeyField = 1;
    c->pKeyInfo->nAllField = 1;
  }
  if( c->pKeyInfo==0 ){ sqlite3_free(c); return BTREELITE_NOMEM; }
  c->iRoot = iRoot;
  c->pCur = (BtCursor*)sqlite3Malloc(sqlite3BtreeCursorSize());
  if( c->pCur==0 ){
    sqlite3_free(c->pKeyInfo);
    sqlite3_free(c);
    return BTREELITE_NOMEM;
  }
  sqlite3BtreeCursorZero(c->pCur);
  rc = sqlite3BtreeCursor(p->pBt, (Pgno)iRoot,
                          wrFlag ? BTREE_WRCSR : 0, c->pKeyInfo, c->pCur);
  if( rc!=SQLITE_OK ){
    sqlite3_free(c->pCur);
    sqlite3_free(c->pKeyInfo);
    sqlite3_free(c);
    return rc;
  }
  *ppCur = c;
  return BTREELITE_OK;
}

void btreelite_cursor_close(btreelite_cur *c){
  if( c==0 ) return;
  if( c->pCur ){ sqlite3BtreeCloseCursor(c->pCur); sqlite3_free(c->pCur); }
  sqlite3_free(c->pKeyInfo);
  sqlite3_free(c);
}

/* ---------------------------------------------------------------- */
/* CRUD                                                             */
/* ---------------------------------------------------------------- */

int btreelite_put(btreelite_cur *c, const void *k, int nK,
                  const void *v, int nV){
  void *pRec = 0;
  BtreePayload x;
  int rc, res = 0, loc;
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  /* IndexMoveto parks the cursor at the insert position; its res is
  ** exactly the seekResult sqlite3BtreeInsert expects. */
  rc = kvMoveto(c, k, nK, &loc);
  if( rc!=SQLITE_OK ) return rc;
  rc = sqlite3KvEncode(k, nK, v, nV, &pRec);
  if( rc!=BTREELITE_OK ) return rc;
  memset(&x, 0, sizeof(x));
  x.pKey = pRec;
  {
    u8 aTmp[9];
    x.nKey = (i64)(sqlite3PutVarint32(aTmp, (u32)nK) + nK + nV);
  }
  rc = sqlite3BtreeInsert(c->pCur, &x, 0, loc);
  sqlite3_free(pRec);
  if( rc!=SQLITE_OK ) return rc;
  /* Reposition on the stored record and refresh the split cache. */
  rc = kvMoveto(c, k, nK, &loc);
  if( rc!=SQLITE_OK ) return rc;
  if( loc==0 ){
    rc = kvCacheRefresh(c);
    if( rc!=SQLITE_OK ) return rc;
  }else{
    kvCacheInvalidate(c);
  }
  return rc;
}

int btreelite_get(btreelite_cur *c, const void *k, int nK){
  int rc, res = 0;
  rc = kvLocate(c, k, nK, &res);
  if( rc!=BTREELITE_OK ) return rc;
  return res==0 ? BTREELITE_OK : BTREELITE_NOTFOUND;
}

int btreelite_checkpoint(btreelite_db *p, int eMode, int *pnLog, int *pnCkpt){
  if( p==0 ) return BTREELITE_ERROR;
  return sqlite3BtreeCheckpoint(p->pBt, eMode, pnLog, pnCkpt);
}

int btreelite_journal_mode(btreelite_db *p, int eMode){
  Pager *pPager;
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
  rc = kvLocate(c, k, nK, &res);
  if( rc!=BTREELITE_OK ) return rc;
  if( res!=0 ) return BTREELITE_NOTFOUND;
  rc = sqlite3BtreeDelete(c->pCur, 0);
  if( rc!=SQLITE_OK ) return rc;
  kvCacheInvalidate(c);
  return BTREELITE_OK;
}

/*
** Move to the first entry whose key is greater than or equal to (k,nK).
** IndexMoveto parks the cursor where the key would be inserted; when
** *pRes!=0 that position is the successor of the key, which is exactly
** the btreelite_seek contract.
*/
int btreelite_seek(btreelite_cur *c, const void *k, int nK){
  int rc, res = 0;
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  rc = kvMoveto(c, k, nK, &res);
  if( rc==SQLITE_EMPTY ){
    kvCacheInvalidate(c);
    return BTREELITE_OK;   /* empty tree: cursor is at EOF */
  }
  if( rc!=SQLITE_OK ) return rc;
  if( res<0 ){
    /* Parked on a cell that sorts before the key (the insert position).
    ** The first entry >= key is the next one, or nothing. */
    rc = sqlite3BtreeNext(c->pCur, 0);
    if( rc==SQLITE_DONE ){
      kvCacheInvalidate(c);
      return BTREELITE_OK;
    }
    if( rc!=SQLITE_OK ) return rc;
  }
  /* Parked on the first entry >= key. */
  rc = kvCacheRefresh(c);
  return rc;
}

int btreelite_eof(btreelite_cur *c){
  if( c==0 || c->pCur==0 ) return 1;
  return sqlite3BtreeEof(c->pCur);
}

int btreelite_first(btreelite_cur *c, int *pRes){
  int rc;
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  rc = sqlite3BtreeFirst(c->pCur, pRes);
  if( rc==SQLITE_OK && *pRes==0 ) rc = kvCacheRefresh(c);
  return rc;
}

int btreelite_last(btreelite_cur *c, int *pRes){
  int rc;
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  rc = sqlite3BtreeLast(c->pCur, pRes);
  if( rc==SQLITE_OK && *pRes==0 ) rc = kvCacheRefresh(c);
  return rc;
}

int btreelite_next(btreelite_cur *c){
  int rc;
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  rc = sqlite3BtreeNext(c->pCur, 0);
  if( rc==SQLITE_DONE ) return BTREELITE_DONE;
  if( rc!=SQLITE_OK ) return rc;
  rc = kvCacheRefresh(c);
  return rc;
}

int btreelite_prev(btreelite_cur *c){
  int rc;
  if( c==0 || c->pCur==0 ) return BTREELITE_ERROR;
  rc = sqlite3BtreePrevious(c->pCur, 0);
  if( rc==SQLITE_DONE ) return BTREELITE_DONE;
  if( rc!=SQLITE_OK ) return rc;
  rc = kvCacheRefresh(c);
  return rc;
}

/* ---------------------------------------------------------------- */
/* Reading the current entry                                        */
/* ---------------------------------------------------------------- */

int btreelite_key(btreelite_cur *c, void *buf, int nMax, int *pnLen){
  int rc;
  if( c==0 || c->pCur==0 || !c->bValid ) return BTREELITE_ERROR;
  if( pnLen ) *pnLen = c->nKeyLen;
  {
    u32 nCopy = (u32)(c->nKeyLen<0 ? 0 : c->nKeyLen);
    if( nCopy>(u32)nMax ) nCopy = (u32)nMax;
    rc = sqlite3BtreePayload(c->pCur, (u32)c->nKeyOff, nCopy, buf);
    if( rc!=SQLITE_OK ) return rc;
  }
  return BTREELITE_OK;
}

int btreelite_value_size(btreelite_cur *c, uint32_t *pnVal){
  if( c==0 || c->pCur==0 || !c->bValid ) return BTREELITE_ERROR;
  *pnVal = (uint32_t)(c->nValLen<0 ? 0 : c->nValLen);
  return BTREELITE_OK;
}

int btreelite_value_read(btreelite_cur *c, uint32_t offset, uint32_t amt,
                         void *pBuf){
  if( c==0 || c->pCur==0 || !c->bValid ) return BTREELITE_ERROR;
  if( (i64)offset+amt > c->nValLen ) return BTREELITE_ERROR;
  return sqlite3BtreePayload(c->pCur,
                             (u32)(c->nKeyOff + c->nKeyLen) + offset, amt, pBuf);
}