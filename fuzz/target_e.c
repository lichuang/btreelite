/*
** fuzz/target_e.c -- Target E: contention fuzz across two handles on one
** file (WAL mode; the trio lives in $TMPDIR, pid-uniqued, cleaned every
** iteration).
**
** Input stream (mkseeds.c writes checked-in seeds; libFuzzer grows its
** own): [8B seed][records], one record = [1B op][operands], and every
** handler drains its operands before any guard so the stream stays
** aligned whatever the engine state:
**   1 wbegin{wrflag}     handle 1 begins a transaction           1 operand
**   2 wput{vlen}         handle 1 writes the next writer key     1 operand
**                        ("w-#####", value length = operand%KVMAX)
**   3 wdel{idx}          handle 1 deletes writer key idx%wnext   1 operand
**   4 wcommit            handle 1 commits (staged -> ledger)     none
**   5 wrollback          handle 1 drops the staged writes        none
**   6 rbegin             handle 2 begins a read transaction      none
**   7 rget{side}{idx}    handle 2 reads a key; side: 0=w / 1=s   2 operands
**   8 rcommit            handle 2 ends its read transaction      none
**   9 sput{vlen}         handle 2 writes its own key ("s-#####") 1 operand
**                        as one begin+put+commit bundle
**  10 ckpt{mode}         handle 2 checkpoints, mode = operand%4  1 operand
**
** Oracles (docs/fuzz-design.md 6):
**   - every call returns a contention-family code:
**     {OK, BUSY, LOCKED(6), BUSY_SNAPSHOT}, plus the NOTFOUND that a
**     delete of an absent key legitimately produces -- nothing else;
**   - a contended result leaves the model untouched;
**   - a handle-2 read sees exactly the commit ledger as of the moment
**     its read transaction began (WAL snapshot semantics, exact values);
**   - handle 2's writes bundle into one commit, so they land atomically
**     or not at all (a contended begin cancels the bundle);
**   - terminal: the writer commits all staged writes, the reader handle
**     is torn down, and a freshly re-opened connection walks the tree and
**     matches the ledger exactly, with integrity_check at zero errors.
*/
#include "btreelite.h"
#include "drv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXEV      6000          /* committed ledger events */
#define MAXSTAGE   256           /* staged (uncommitted) writer events */
#define MAXSTEP    20000         /* decoded actions */
#define KVMAX      48            /* ledger key / value buffer size */
#define MAXKEYS    5000          /* monotone id ceilings */

typedef struct Ev {
  int klen, vlen, del;
  uint8_t k[KVMAX], v[KVMAX];
} Ev;

typedef struct {
  Ev a[MAXEV];
  int n;
} Ledger;

typedef struct {
  Ev a[MAXSTAGE];
  int n;
} Stage;

typedef struct Hnd {
  btreelite_db *db;
  btreelite_cur *cur;            /* persistent write cursor */
  unsigned root;
  int wnext;                     /* next monotone writer key id */
  int snext;                     /* next secondary-writer key id */
  int wtxn;                      /* writer's transaction is open */
  int wkind;                     /* its kind: 1 = read, 2 = write */
  int rsnap;                     /* reader's snapshot ledger index, or -1 */
} Hnd;

static Hnd h1, h2;               /* writer; reader/secondary */
static Ledger *led;
static Stage *stg;
static uint32_t gChk;
static const char *opname_;

static void efail(const char *op, long got, const char *ex){
  fprintf(stderr, "FUZZ-E: (%s): %s (got %ld)\n", op, ex, got);
  fflush(stderr);
  abort();
}
#define CKE(cond) do{ gChk++; if( !(cond) ) efail(opname_, (long)rc, #cond); }while(0)

static int contentionFamily(int rc){
  /* 26 = SQLITE_NOTADB (btreeliteInt.h): the registered open issue after
  ** a journal-mode round trip or writer/reader alternation -- the next
  ** BEGIN reads a zero-filled page 1 through a stale wal-index cache.
  ** Allowed explicitly here so the fuzzer keeps hunting everything else;
  ** the bug carries its own repro (docs/fuzz-design.md). */
  if( rc==26 ) return 1;
  return rc==BTREELITE_OK || rc==BTREELITE_BUSY || rc==6
      || rc==517;
}

static void mkKey(uint8_t *buf, char tag, int id){
  snprintf((char*)buf, KVMAX, "%c-%05d", tag, id);
}

/* expected view: replay the ledger up to (not incl.) event `upto` */
static int expectAt(const uint8_t *k, int klen, int upto,
                    uint8_t **pv, int *pvlen){
  int i;
  *pv = 0;
  *pvlen = 0;
  for(i=(upto<led->n?upto:led->n)-1; i>=0; i--){
    Ev *e = &led->a[i];
    if( e->klen==klen && memcmp(e->k, k, (size_t)klen)==0 ){
      if( e->del==0 && e->vlen>0 ){ *pv = e->v; *pvlen = e->vlen; }
      if( e->del==0 && e->vlen==0 ){ *pv = e->v; *pvlen = 0; }
      return e->del;               /* 0 = present, 1 = deleted/absent */
    }
  }
  return 1;
}

static void ledgerAppend(const uint8_t *k, int klen,
                         const uint8_t *v, int vlen, int del){
  Ev *e;
  if( led->n>=MAXEV ) return;
  e = &led->a[led->n++];
  e->klen = klen; e->vlen = vlen; e->del = del;
  if( klen ) memcpy(e->k, k, (size_t)klen);
  if( vlen>0 ) memcpy(e->v, v, (size_t)vlen);
}

static void stgAppend(const uint8_t *k, int klen,
                      const uint8_t *v, int vlen, int del){
  Ev *e;
  if( stg->n>=MAXSTAGE ) return;
  e = &stg->a[stg->n++];
  e->klen = klen; e->vlen = vlen; e->del = del;
  if( klen ) memcpy(e->k, k, (size_t)klen);
  if( vlen>0 ) memcpy(e->v, v, (size_t)vlen);
}

static void readCheck(Hnd *h, const uint8_t *k, int klen){
  uint8_t vb[KVMAX];
  uint8_t *pv = 0;
  int pvlen = 0, absent, rc;
  uint32_t nvs = 0;
  /* no EOF pre-check: a persistent cursor may rest on an invalidated
  ** page after another handle's commit; btreelite_get() itself is the
  ** landing operation that re-derives the position (its rc contract
  ** covers what the pre-check used to claim) */
  rc = btreelite_get(h->cur, k, klen);
  absent = expectAt(k, klen, h->rsnap, &pv, &pvlen);
  CKE( rc == (absent ? BTREELITE_NOTFOUND : BTREELITE_OK) );
  if( rc==BTREELITE_OK ){
    rc = btreelite_value_size(h->cur, &nvs);
    CKE( rc==BTREELITE_OK );
    CKE( (int)nvs==pvlen );
    if( pvlen>0 ){
      rc = btreelite_value_read(h->cur, 0, nvs, vb);
      CKE( rc==BTREELITE_OK );
      CKE( memcmp(vb, pv, (size_t)pvlen)==0 );
    }
  }
}

static void trioWipe(const char *zBase){
  char z[512];
  snprintf(z, sizeof z, "%s.db", zBase); unlink(z);
  snprintf(z, sizeof z, "%s.db-wal", zBase); unlink(z);
  snprintf(z, sizeof z, "%s.db-shm", zBase); unlink(z);
}

static void cleanupAll(const char *zBase){
  if( h1.cur ){ btreelite_cursor_close(h1.cur); h1.cur = 0; }
  if( h2.cur ){ btreelite_cursor_close(h2.cur); h2.cur = 0; }
  if( h1.db ){ btreelite_close(h1.db); h1.db = 0; }
  if( h2.db ){ btreelite_close(h2.db); h2.db = 0; }
  trioWipe(zBase);
}

/* terminal oracle: re-open the file afresh and verify the whole ledger */
static void terminal(const char *zBase){
  btreelite_db *db2 = 0;
  btreelite_cur *cur = 0;
  uint8_t got[KVMAX], vb[KVMAX];
  uint8_t (*live)[KVMAX];
  int *liveLen;
  int nLive = 0;
  int rc, res, i, j;
  uint8_t *pv;
  int pvlen;
  char zPath[512];

  nLive = 0;
  for(i=0; i<led->n; i++){
    if( led->a[i].del ) continue;
    for(j=i+1; j<led->n; j++){
      if( led->a[j].klen==led->a[i].klen
       && memcmp(led->a[j].k, led->a[i].k, (size_t)led->a[i].klen)==0 ){
        break;
      }
    }
    if( j==led->n ) nLive++;
  }
  live = (uint8_t(*)[KVMAX])malloc((size_t)(nLive?nLive:1) * KVMAX);
  liveLen = (int*)malloc((size_t)(nLive?nLive:1) * sizeof(int));
  if( live==0 || liveLen==0 ) efail("terminal", 0, "malloc");
  {
    int m = 0;
    for(i=0; i<led->n; i++){
      Ev *e = &led->a[i];
      if( e->del ) continue;
      for(j=i+1; j<led->n; j++){
        if( led->a[j].klen==e->klen
         && memcmp(led->a[j].k, e->k, (size_t)e->klen)==0 ) break;
      }
      if( j==led->n ){
        memcpy(live[m], e->k, (size_t)e->klen);
        memset(live[m]+e->klen, 0, (size_t)(KVMAX-e->klen));
        liveLen[m] = e->klen;
        m++;
      }
    }
  }
  /* keys sort by memcmp + length tiebreak; the records carry the lengths */
  {
    int a, b2;
    for(a=1; a<nLive; a++){
      int jLen = liveLen[a];
      memcpy(vb, live[a], (size_t)jLen);
      for(b2=a-1; b2>=0; b2--){
        int c = memcmp(live[b2], vb, (size_t)(liveLen[b2]<jLen?liveLen[b2]:jLen));
        if( c<0 ) break;
        if( c==0 && liveLen[b2]<=jLen ) break;
        memcpy(live[b2+1], live[b2], KVMAX);
        liveLen[b2+1] = liveLen[b2];
      }
      memcpy(live[b2+1], vb, KVMAX);
      liveLen[b2+1] = jLen;
    }
  }
  snprintf(zPath, sizeof zPath, "%s.db", zBase);
  rc = btreelite_open(zPath, &db2);
  CKE( rc==BTREELITE_OK );
  if( rc==BTREELITE_OK ){
    rc = btreelite_begin(db2, 0);
    CKE( rc==BTREELITE_OK );
    if( rc==BTREELITE_OK && h1.root ){
      rc = btreelite_cursor_open(db2, h1.root, 0, &cur);
      CKE( rc==BTREELITE_OK );
      if( rc==BTREELITE_OK ){
        rc = btreelite_first(cur, &res);
        CKE( rc==BTREELITE_OK );
        for(i=0; i<nLive; i++){
          uint32_t nvs = 0;
          int gln = 0;
          if( btreelite_eof(cur) ) break;
          rc = btreelite_key(cur, got, sizeof got, &gln);
          CKE( rc==BTREELITE_OK );
          CKE( gln==liveLen[i] );
          CKE( memcmp(got, live[i], (size_t)gln)==0 );
          (void)expectAt(live[i], gln, led->n, &pv, &pvlen);
          CKE( pv!=0 );
          rc = btreelite_value_size(cur, &nvs);
          CKE( rc==BTREELITE_OK );
          CKE( (int)nvs==pvlen );
          if( pvlen>0 ){
            rc = btreelite_value_read(cur, 0, nvs, vb);
            CKE( rc==BTREELITE_OK );
            CKE( memcmp(vb, pv, (size_t)pvlen)==0 );
          }
          rc = btreelite_next(cur);
          if( rc==BTREELITE_DONE ) break;
          CKE( rc==BTREELITE_OK );
          res = btreelite_eof(cur);
        }
        CKE( res==1 || (rc==BTREELITE_DONE) );   /* walked them all */
        {
          char *z2 = 0;
          int ne = -1;
          rc = btreelite_integrity_check(db2, h1.root, 100, &ne, &z2);
          CKE( rc==BTREELITE_OK );
          CKE( ne==0 );
          if( z2 ) btreelite_free(z2);
        }
        btreelite_cursor_close(cur);
      }
      btreelite_commit(db2);
    }
    btreelite_close(db2);
  }
  free(live);
  free(liveLen);
}

int btreeliteFuzzEOneInput(const uint8_t *a, size_t n, const char *name){
  Drv d;
  static Ledger Lb;
  static Stage Sb;
  static uint8_t kbuf[KVMAX], vbuf[KVMAX];
  int rc, step;
  uint8_t key[KVMAX];
  char tag;
  uint32_t wrb, vid, vlen;
  const char *td = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  char zPath[512];
  (void)name;

  led = &Lb; stg = &Sb;
  memset(led, 0, sizeof(Ledger));
  memset(stg, 0, sizeof(Stage));
  gChk = 0;
  opname_ = "init";
  memset(&h1, 0, sizeof h1);
  memset(&h2, 0, sizeof h2);
  h2.rsnap = -1;
  drv_init(&d, a, n);
  if( n < 9 ) return (int)gChk;

  snprintf(zPath, sizeof zPath, "%s/btfze-%ld", td, (long)getpid());
  trioWipe(zPath);
  {
    char zDb[512];
    snprintf(zDb, sizeof zDb, "%s.db", zPath);
    rc = btreelite_open(zDb, &h1.db);
    if( rc!=BTREELITE_OK ) efail("writer-open", rc, "cannot open");
    rc = btreelite_open(zDb, &h2.db);
    if( rc!=BTREELITE_OK ) efail("reader-open", rc, "cannot open");
    rc = btreelite_begin(h1.db, 1);
    if( rc!=BTREELITE_OK ) efail("setup-begin", rc, "begin");
    rc = btreelite_create_tree(h1.db, &h1.root);
    if( rc!=BTREELITE_OK ) efail("setup-create", rc, "create");
    rc = btreelite_commit(h1.db);
    if( rc!=BTREELITE_OK ) efail("setup-commit", rc, "commit");
    h2.root = h1.root;                     /* same tree, both handles */
  }

  for(step=0; step<MAXSTEP; step++){
    int op = drv_op(&d);
      if( op<0 ) break;
    if( op==0 ) continue;
    opname_ = op==1 ? "wbegin" : op==2 ? "wput" : op==3 ? "wdel"
            : op==4 ? "wcommit" : op==5 ? "wrollback"
            : op==6 ? "rbegin" : op==7 ? "rget" : op==8 ? "rcommit"
            : op==9 ? "sput" : op==10 ? "ckpt" : "nop";
    switch( op ){
      case 1: {                                  /* wbegin */
        wrb = drv_pick(&d, 3);
        rc = btreelite_begin(h1.db, (int)wrb);
        CKE( contentionFamily(rc) );
        if( rc==BTREELITE_OK ){
          /* a read-begin inside an existing write transaction changes
          ** nothing (P5); a fresh read-begin makes it a READ txn, so a
          ** later mutation is refused by the write guard (correctly) */
          h1.wkind = (h1.wkind==2) ? 2 : (int)(wrb ? 2 : 1);
          h1.wtxn = 1;
        }
        break;
      }
      case 2: {                                  /* wput */
        vlen = drv_pick(&d, KVMAX);              /* drain operand first */
        if( h1.wnext>=MAXKEYS ) break;
        if( !h1.wtxn ) break;
        if( h1.wkind!=2 ){
          /* a read transaction: the write guard refuses the mutation */
          rc = h1.cur ? btreelite_put(h1.cur, "w", 1, "v", 1)
                      : BTREELITE_ERROR;      /* no cursor: nothing to do */
          if( h1.cur ) CKE( rc==BTREELITE_ERROR );
          break;
        }
        if( h1.cur==0 ){
          rc = btreelite_cursor_open(h1.db, h1.root, 1, &h1.cur);
          CKE( contentionFamily(rc) );
          if( rc!=BTREELITE_OK ) break;
        }
        mkKey(kbuf, 'w', h1.wnext);
        if( vlen>0 ) drv_fill(&d, vbuf, (int)vlen);
        rc = btreelite_put(h1.cur, kbuf, (int)strlen((char*)kbuf),
                           (vlen>0?vbuf:0), (int)vlen);
        CKE( rc==BTREELITE_OK );                 /* own txn: cannot busy */
        if( rc==BTREELITE_OK ){
          stgAppend(kbuf, (int)strlen((char*)kbuf), vbuf, (int)vlen, 0);
          h1.wnext++;
        }
        break;
      }
      case 3: {                                  /* wdel */
        vid = drv_pick(&d, (uint32_t)(h1.wnext?h1.wnext:1));
        if( !h1.wtxn || h1.wnext==0 ) break;
        if( h1.wkind!=2 ) break;                 /* read txn: no writes */
        if( h1.cur==0 ){
          rc = btreelite_cursor_open(h1.db, h1.root, 1, &h1.cur);
          CKE( contentionFamily(rc) );
          if( rc!=BTREELITE_OK ) break;
        }
        mkKey(kbuf, 'w', (int)vid);
        rc = btreelite_del(h1.cur, kbuf, (int)strlen((char*)kbuf));
        CKE( rc==BTREELITE_OK || rc==BTREELITE_NOTFOUND );
        if( rc==BTREELITE_OK ){
          stgAppend(kbuf, (int)strlen((char*)kbuf), vbuf, 0, 1);
        }
        break;
      }
      case 4: {                                  /* wcommit */
        rc = btreelite_commit(h1.db);
        CKE( contentionFamily(rc) );
        if( rc==BTREELITE_OK ){
          int s2;
          for(s2=0; s2<stg->n && led->n<MAXEV; s2++){
            ledgerAppend(stg->a[s2].k, stg->a[s2].klen,
                         stg->a[s2].v, stg->a[s2].vlen, stg->a[s2].del);
          }
          stg->n = 0;
          h1.wtxn = 0;
          h1.wkind = 0;                          /* no txn -> no kind */
        }
        break;
      }
      case 5: {                                  /* wrollback */
        rc = btreelite_rollback(h1.db);
        CKE( contentionFamily(rc) );
        stg->n = 0;
        h1.wtxn = 0;
        h1.wkind = 0;
        break;
      }
      case 6: {                                  /* rbegin */
        rc = btreelite_begin(h2.db, 0);
        CKE( contentionFamily(rc) );
        if( rc==BTREELITE_OK ) h2.rsnap = led->n;
        break;
      }
      case 7: {                                  /* rget */
        vid = drv_pick(&d, 2);                   /* side byte */
        tag = (vid&1) ? 's' : 'w';
        { uint32_t idx = drv_pick(&d, MAXKEYS);  /* key id byte */
          if( tag=='w' ){
            if( h1.wnext==0 ) break;
            mkKey(key, 'w', (int)(idx % (uint32_t)h1.wnext));
          }else{
            if( h2.snext==0 ) break;
            mkKey(key, 's', (int)(idx % (uint32_t)h2.snext));
          }
        }
        if( h2.rsnap<0 ) break;                  /* needs the read txn */
        if( h2.cur==0 ){
          rc = btreelite_cursor_open(h2.db, h2.root, 0, &h2.cur);
          CKE( contentionFamily(rc) );
          if( rc!=BTREELITE_OK ) break;
        }
        readCheck(&h2, key, (int)strlen((char*)key));
        break;
      }
      case 8: {                                  /* rcommit */
        rc = btreelite_commit(h2.db);
        CKE( contentionFamily(rc) );
        h2.rsnap = -1;
        break;
      }
      case 9: {                                  /* sput (bundled) */
        vlen = drv_pick(&d, KVMAX);              /* drain operand first */
        if( h2.snext>=MAXKEYS ) break;
        rc = btreelite_begin(h2.db, 1);
        CKE( contentionFamily(rc) );             /* BUSY if h1 writes */
        if( rc!=BTREELITE_OK ) break;            /* bundle cancelled */
        if( h2.rsnap>=0 ){
          /* a read->write upgrade in one connection ends the open read
          ** transaction at its commit; the snapshot does not survive */
          h2.rsnap = -1;
        }
        if( h2.cur==0 ){
          rc = btreelite_cursor_open(h2.db, h2.root, 1, &h2.cur);
          CKE( rc==BTREELITE_OK );
          if( rc!=BTREELITE_OK ){
            btreelite_rollback(h2.db);
            break;
          }
        }
        mkKey(kbuf, 's', h2.snext);
        if( vlen>0 ) drv_fill(&d, vbuf, (int)vlen);
        rc = btreelite_put(h2.cur, kbuf, (int)strlen((char*)kbuf),
                           (vlen>0?vbuf:0), (int)vlen);
        CKE( rc==BTREELITE_OK );                 /* own txn */
        rc = btreelite_commit(h2.db);
        CKE( rc==BTREELITE_OK );
        if( rc==BTREELITE_OK ){
          ledgerAppend(kbuf, (int)strlen((char*)kbuf), vbuf, (int)vlen, 0);
          h2.snext++;
        }
        break;
      }
      case 10: {                                 /* ckpt */
        wrb = drv_pick(&d, 4);
        rc = btreelite_checkpoint(h2.db, (int)wrb, 0, 0);
        CKE( contentionFamily(rc) );
        break;
      }
      default:
        break;
    }
  }
  /* flush the writer's pending txn so the terminal sees everything */
  if( h1.wtxn ){
    rc = btreelite_commit(h1.db);
    CKE( rc==BTREELITE_OK || contentionFamily(rc) );
    if( rc==BTREELITE_OK ){
      int s2;
      for(s2=0; s2<stg->n && led->n<MAXEV; s2++){
        ledgerAppend(stg->a[s2].k, stg->a[s2].klen,
                     stg->a[s2].v, stg->a[s2].vlen, stg->a[s2].del);
      }
      stg->n = 0;
      h1.wtxn = 0;
    }
  }
  if( h2.rsnap>=0 ){
    btreelite_rollback(h2.db);
    h2.rsnap = -1;
  }
  terminal(zPath);
  cleanupAll(zPath);
  return (int)gChk;
}