/*
** fuzz/target_api.c -- Target A: stateful API fuzz over ":memory:" handles.
**
** Replays a deterministic action stream (fuzz/drv.h) against up to four
** handles, mirroring every visible effect into per-tree models
** (fuzz/model.h).  Three oracle layers:
**   1. per-action return-code contract (probe facts P1..P15 encoded);
**   2. full forward scan equal to the model after every commit;
**   3. terminal: forward+backward scan plus integrity_check over every
**      live root (and the iRoot==0 form).
**
** A contract violation prints a context line and abort(); the input that
** produced it is the repro, and a violation is always a candidate
** library bug.
**
** Cursor states, all empirical (probes P11..P15, btreeNext source, t_kv):
**
**   FRESH   -- never landed.  next/prev return DONE and move nothing.
**              key/value accessors return BTREELITE_ERROR (EOF gate).
**   VALID   -- parked exactly on a known cursor key; reads are compared
**              with the model (epoch-gated).
**   TRIPPED -- position invalidated (own transaction fully rolled back,
**              or the pointed-at entry deleted by another cursor).  The
**              engine restores by saved key on the next use: accessors
**              return BTREELITE_ERROR until then; movement lands in the
**              model (existence-checked landing).
**   EOF     -- past the end.  Accessors error; next/prev DONE;
**              first/last/get/seek land normally.
**
** Content comparisons are epoch-gated: an entry read through a parked
** cursor is only compared when the entry has not been rewritten by a
** different cursor since -- the engine may legitimately serve the stale
** page it holds (inherited upstream behaviour).
**
** P0 scope: ":memory:" handles only.  File-backed handles, hostile
** files and OOM injection are the Target B/C batches (docs/fuzz-design.md).
*/
#include "btreelite.h"
#include "drv.h"
#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NH       4    /* handles per input */
#define NROOTMAX 32   /* root slots per handle */
#define NCUR     16   /* cursor slots */
#define NSNAP    3    /* nested savepoints kept (plus savepoint 0) */
#define MODCAP   (12u*1024*1024)  /* per-root buffered value bytes */

#define CS_FRESH   0
#define CS_VALID   1
#define CS_TRIPPED 2
#define CS_EOF     3

typedef struct Root {
  int used;
  unsigned pgno;
  int gen;                     /* savepoint depth at creation */
  Model mod;
} Root;

typedef struct Img {           /* whole-roots image: base or a snapshot */
  int taken;
  Root r[NROOTMAX];
} Img;

typedef struct Handle {
  int live;
  btreelite_db *db;
  int txn, nSpt, sync, interrupt;
  Root r[NROOTMAX];
  Img base;
  Img snap[NSNAP];
} Handle;

typedef struct Cur {
  int live, wr, h, slot, st, nk;
  uint8_t k[FZ_KEYMAX];
  uint32_t ep;                 /* epoch at landing time */
  btreelite_cur *b;
} Cur;

static Handle hs[NH];
static Cur cs[NCUR];
static uint32_t gAct, gChk, gNop;
static const char *gName, *gOp;

static void fzFailG(long got, const char *ex){
  fprintf(stderr, "FUZZ: %s: act#%u (%s): %s (got %ld)\n",
          gName ? gName : "?", gAct, gOp ? gOp : "init", ex, got);
  fflush(stderr);
  abort();
}
static void fzFail(const char *ex){ fzFailG(0, ex); }
#define CK(cond) do{ gChk++; if( !(cond) ) fzFailG((long)rc, #cond); }while(0)

/* ---- roots images ---------------------------------------------------- */

static void imgFree(Img *im){
  int i;
  if( !im->taken ) return;
  for(i=0; i<NROOTMAX; i++) model_free(&im->r[i].mod);
  memset(im, 0, sizeof(Img));
}

static void imgSnap(Handle *h, Img *im){
  int i;
  imgFree(im);
  for(i=0; i<NROOTMAX; i++){
    im->r[i].used = h->r[i].used;
    im->r[i].pgno = h->r[i].pgno;
    im->r[i].gen  = h->r[i].gen;
    if( h->r[i].used ){
      Model *c = model_copy(&h->r[i].mod);
      if( c==0 ) abort();
      im->r[i].mod = *c;
      free(c);
    }
  }
  im->taken = 1;
}

static void imgRestore(Handle *h, const Img *im){
  int i;
  for(i=0; i<NROOTMAX; i++){
    if( im->r[i].used ){
      Model *c = model_copy(&im->r[i].mod);
      if( c==0 ) abort();
      model_free(&h->r[i].mod);
      h->r[i].mod = *c;
      free(c);
    }else{
      model_free(&h->r[i].mod);
    }
    h->r[i].used = im->r[i].used;
    h->r[i].pgno = im->r[i].pgno;
    h->r[i].gen  = im->r[i].gen;
  }
}

static void freeRoots(Handle *h){
  int i;
  imgFree(&h->base);
  for(i=0; i<NSNAP; i++) imgFree(&h->snap[i]);
  for(i=0; i<NROOTMAX; i++) model_free(&h->r[i].mod);
}

/* ---- cursor helpers --------------------------------------------------- */

static Root *rootOf(Cur *c){ return &hs[c->h].r[c->slot]; }

/* Cursors parked on a tree whose page image changed under them: the
** engine restores their saved key on the next use (P6/P14 semantics). */
static void tripCursors(int hn, int slot){
  int i;
  for(i=0; i<NCUR; i++){
    if( cs[i].live && cs[i].h==hn && cs[i].slot==slot
     && cs[i].st==CS_VALID ) cs[i].st = CS_TRIPPED;
  }
}

static void tripAll(int hn){
  int i;
  for(i=0; i<NCUR; i++){
    if( cs[i].live && cs[i].h==hn && cs[i].st==CS_VALID )
      cs[i].st = CS_TRIPPED;
  }
}

/*
** After a roots-image restore (rollback / ROLLBACK TO i), root pages
** created by the undone transaction no longer exist in the file: any
** cursor open on a slot whose tree disappeared is closed here, and
** cursors on surviving trees are tripped.  (The engine keeps a stale
** page reference alive for one cached read -- reuse is unreliable and
** reports BTREELITE_CORRUPT; the harness models "don't do that".)
*/
static void fixCursorsAfterRestore(int hn){
  int i;
  Handle *h = &hs[hn];
  tripAll(hn);
  for(i=0; i<NCUR; i++){
    Cur *c = &cs[i];
    if( c->live && c->h==hn && !h->r[c->slot].used ){
      btreelite_cursor_close(c->b);
      c->b = 0; c->live = 0;
    }
  }
}

/*
** Re-derive the cursor position from the engine: EOF (and any invalid
** position) stays EOF; else the engine's current key must exist in the
** model and the cursor becomes VALID on it.
*/
static void landKey(Cur *c){
  uint8_t kb[FZ_KEYMAX+1];
  int nl = 0;
  MEnt *e = 0;
  if( btreelite_eof(c->b) ){ c->st = CS_EOF; c->nk = 0; return; }
  {
    int rc = btreelite_key(c->b, kb, sizeof kb, &nl);
    if( rc!=BTREELITE_OK ) fzFail("key() after landing");
  }
  if( model_get(&rootOf(c)->mod, kb, nl, &e) )
    fzFail("landed key not in model");
  memcpy(c->k, kb, (size_t)nl);
  c->nk = nl;
  c->ep = e->epoch;
  c->st = CS_VALID;
}

static void setEOF(Cur *c){ c->st = CS_EOF; c->nk = 0; }

/* Content comparison through a VALID cursor (epoch-gated). */
static void ckContent(Cur *c){
  MEnt *e = 0;
  uint32_t nvsz = 0;
  uint8_t vb[8192];
  int rc, off;
  if( c->st!=CS_VALID ) return;
  if( model_get(&rootOf(c)->mod, c->k, c->nk, &e) )
    fzFail("entry vanished under VALID cursor");
  if( e->epoch != c->ep ) return;        /* rewritten since: skip read */
  rc = btreelite_value_size(c->b, &nvsz);
  if( rc!=BTREELITE_OK || (int)nvsz!=e->nv ) fzFail("value size mismatch");
  for(off=0; off<e->nv; off+=(int)sizeof vb){
    uint32_t chunk = (uint32_t)(e->nv - off);
    if( chunk > sizeof vb ) chunk = sizeof vb;
    rc = btreelite_value_read(c->b, (uint32_t)off, chunk, vb);
    if( rc!=BTREELITE_OK ) fzFail("value read chunk");
    if( memcmp(vb, e->pv+off, chunk) ) fzFail("value bytes differ");
  }
}

/* Entry at model index i (front = 0). */
static MEnt *modelAt(Model *m, int i){
  MEnt *e = m->head;
  while( i-- > 0 ){
    e = e->next;
    if( e==0 ) fzFail("modelAt overflow");
  }
  return e;
}

/* One full scan equals the model exactly (keys AND values).  A read
** transaction of its own is opened when the handle is idle (cursor_open
** requires one). */
static void scanTree(Handle *h, Root *R, int bBack){
  btreelite_cur *sc = 0;
  Model *m = &R->mod;
  int rc, res;
  int i;
  int ownTxn = h->txn==0;
  if( ownTxn && btreelite_begin(h->db,0)!=BTREELITE_OK )
    fzFail("scan begin");
  rc = btreelite_cursor_open(h->db, R->pgno, 0, &sc);
  if( rc!=BTREELITE_OK ) fzFail("scan cursor open");
  rc = bBack ? btreelite_last(sc,&res) : btreelite_first(sc,&res);
  if( rc!=BTREELITE_OK ) fzFail("scan first/last");
  if( m->n==0 ){
    if( res!=1 ) fzFail("empty tree did not res=1");
    btreelite_cursor_close(sc);
    if( ownTxn ) btreelite_commit(h->db);
    return;
  }
  if( res!=0 ) fzFail("scan did not land");
  i = bBack ? m->n-1 : 0;
  for(;;){
    MEnt *e = modelAt(m, i);
    int nl = 0;
    uint32_t nvs = 0;
    uint8_t big[FZ_VALMAX];
    uint8_t kb[FZ_KEYMAX+1];
    if( btreelite_eof(sc) ) fzFail("scan EOF early");
    rc = btreelite_key(sc, kb, sizeof kb, &nl);
    if( rc!=BTREELITE_OK || nl!=e->nk || memcmp(kb,e->pk,(size_t)nl) )
      fzFail("scan key mismatch");
    rc = btreelite_value_size(sc, &nvs);
    if( rc!=BTREELITE_OK || (int)nvs!=e->nv ) fzFail("scan value size");
    if( nvs > 0 ){
      rc = btreelite_value_read(sc, 0, nvs, big);
      if( rc!=BTREELITE_OK || memcmp(big,e->pv,nvs) )
        fzFail("scan value bytes");
    }
    rc = bBack ? btreelite_prev(sc) : btreelite_next(sc);
    if( bBack ){ if( --i < 0 ) break; }
    else { if( ++i >= m->n ) break; }
    if( rc!=BTREELITE_OK ) fzFail("scan step failed");
  }
  if( rc!=BTREELITE_DONE ) fzFail("scan overshoot missing");
  btreelite_cursor_close(sc);
  if( ownTxn ){
    if( btreelite_commit(h->db)!=BTREELITE_OK ) fzFail("scan end");
    h->txn = 0;
  }
}

/* Terminal-oracle sweep: everything that can be checked once the whole
** action stream has run. */
static void terminalOracle(void){
  int i, j;
  for(i=0; i<NH; i++){
    Handle *h = &hs[i];
    int ownTxn = 0;
    if( !h->live ) continue;
    if( h->txn==0 ){
      if( btreelite_begin(h->db, 0)!=BTREELITE_OK ) fzFail("terminal begin");
      h->txn = 1; ownTxn = 1;
    }
    for(j=0; j<NROOTMAX; j++){
      Root *R = &h->r[j];
      if( R->used ){
        scanTree(h, R, 0);
        scanTree(h, R, 1);
        {
          char *z = 0; int nErr = -1;
          int rc = btreelite_integrity_check(h->db, R->pgno, 100, &nErr, &z);
          if( h->interrupt && rc!=BTREELITE_INTERRUPT )
            fzFail("terminal integrity (interrupt path)");
          if( !h->interrupt && (rc!=BTREELITE_OK || nErr!=0) )
            fzFail("terminal integrity");
          if( z ) btreelite_free(z);
        }
      }
    }
    if( !h->interrupt ){
      char *z = 0; int nErr = -1;
      int rc = btreelite_integrity_check(h->db, 0, 100, &nErr, &z);
      if( rc!=BTREELITE_OK || nErr!=0 ) fzFail("terminal integrity iRoot=0");
      if( z ) btreelite_free(z);
    }
    if( ownTxn ) btreelite_rollback(h->db);
    h->txn = 0;
  }
}

/* ---- generation ------------------------------------------------------- */

/* genKey/genVal do NOT consume stream bytes: every handler reads its
** full operand list first (even for a dead handle or a no-op), so the
** stream stays aligned no matter which guards fire. */
static void genKey(Drv *d, uint8_t *buf, int nk, int mode, int *pn){
  if( nk > FZ_KEYMAX ) nk = FZ_KEYMAX;
  switch( mode ){
    case 0: {                    /* "row-NNNNN" shaped */
      uint32_t v = drv_rand(d) % 100000u;
      *pn = nk ? (nk < 10 ? nk : 10) : 0;
      if( *pn ) snprintf((char*)buf, (size_t)*pn+1, "row-%05u", v);
      break;
    }
    case 1: {                    /* tiny "kNNN" */
      uint32_t v = drv_rand(d) % 1000u;
      *pn = nk ? (nk < 5 ? nk : 5) : 0;
      if( *pn ) snprintf((char*)buf, (size_t)*pn+1, "k%03u", v);
      break;
    }
    case 2: {                    /* fill 'm', truncate to nk */
      int j;
      *pn = nk;
      memset(buf, 'm', FZ_KEYMAX);
      snprintf((char*)buf, FZ_KEYMAX, "L255-%04u",
               (unsigned)(drv_rand(d)%10000u));
      for(j=0; j<*pn; j++) if( buf[j]==0 ) buf[j] = 'm';
      break;
    }
    default: {                   /* pseudo bytes, NUL-free */
      int j;
      *pn = nk;
      if( *pn ) drv_fill(d, buf, *pn);
      for(j=0; j<*pn; j++) if( buf[j]==0 ) buf[j] = 1;
      break;
    }
  }
}

static void genVal(Drv *d, Model *m, uint8_t *buf, int nv, int *pn){
  if( m->vbytes + (size_t)(nv>0?nv:0) > MODCAP ) nv = 0;
  if( nv > 0 ) drv_fill(d, buf, nv);
  *pn = nv;
}

/* ---- state reset ------------------------------------------------------ */

static void resetAll(void){
  int i;
  /* cursors first: btreelite_close would auto-close them and the pool
  ** would then hold dangling pointers */
  for(i=0; i<NCUR; i++){
    if( cs[i].b ) btreelite_cursor_close(cs[i].b);
    memset(&cs[i], 0, sizeof(Cur));
  }
  for(i=0; i<NH; i++){
    if( hs[i].live ) btreelite_close(hs[i].db);
    freeRoots(&hs[i]);
    memset(&hs[i], 0, sizeof(Handle));
  }
  gAct = gChk = gNop = 0;
}

/* ---- the action loop -------------------------------------------------- */

int btreeliteFuzzOneInput(const uint8_t *a, size_t n, const char *name){
  Drv d;
  static uint8_t kb[FZ_KEYMAX], vb[FZ_VALMAX];
  int rc, res, nk, nv;
  uint32_t hb, ci;
  Handle *h;
  Cur *c;
  gName = name;
  gOp = "";
  resetAll();
  drv_init(&d, a, n);

  for(gAct=0; gAct<FZ_MAXACT; gAct++){
    int op = drv_op(&d);
    if( op<0 ) break;
    if( op==0 ){ gNop++; continue; }
    switch( op ){

    case FZ_OP_OPEN: {
      gOp = "open";
      hb = drv_pick(&d, NH); (void)drv_u8(&d);
      h = &hs[hb];
      if( h->live ) break;
      rc = btreelite_open(":memory:", &h->db);
      CK( rc==BTREELITE_OK );
      h->live = 1; h->txn = 0; h->nSpt = 0;
      h->sync = BTREELITE_SYNC_FULL; h->interrupt = 0;
      break;
    }

    case FZ_OP_CLOSE: {
      gOp = "close";
      hb = drv_pick(&d, NH);
      h = &hs[hb];
      if( !h->live ) break;
      { int i;
        for(i=0; i<NCUR; i++)
          if( cs[i].live && cs[i].h==(int)hb ){
            btreelite_cursor_close(cs[i].b);
            cs[i].live = 0;
          }
      }
      btreelite_close(h->db);
      freeRoots(h);
      memset(h, 0, sizeof(Handle));
      break;
    }

    case FZ_OP_BEGIN: {
      uint32_t wrb;
      int wasWrite;
      gOp = "begin";
      hb = drv_pick(&d, NH);
      wrb = drv_pick(&d, 3);
      h = &hs[hb];
      if( !h->live ) break;
      wasWrite = (h->txn==2);
      if( !wasWrite && wrb>=1 ){
        imgSnap(h, &h->base);             /* base = pre-txn image */
      }else if( wasWrite && wrb>=1 ){     /* nested: a new savepoint */
        if( h->nSpt > NSNAP ) break;      /* depth cap: skip */
        imgSnap(h, &h->snap[h->nSpt-1]);
      }
      rc = btreelite_begin(h->db, (int)wrb);
      CK( rc==BTREELITE_OK );
      if( getenv("BTREELITE_FUZZ_DEBUG") )
        fprintf(stderr, "BGDBG act=%u wrb=%u pgno0=%u pc=%u\n",
                gAct, wrb, (unsigned)h->r[0].pgno,
                (unsigned)btreelite_page_count(h->db));
      rc = btreelite_txn_state(h->db);
      CK( rc==1 || rc==2 );
      h->txn = rc;
      /* one implicit savepoint for the write transaction, one more per
      ** nested write-begin; a read begin opens none */
      if( h->txn==2 ) h->nSpt = wasWrite ? (wrb>=1 ? h->nSpt+1 : h->nSpt) : 1;
      break;
    }

    case FZ_OP_COMMIT: {
      gOp = "commit";
      hb = drv_pick(&d, NH);
      h = &hs[hb];
      if( !h->live ) break;
      rc = btreelite_commit(h->db);
      CK( rc==BTREELITE_OK );
      CK( btreelite_txn_state(h->db)==0 );
      imgSnap(h, &h->base);               /* committed image */
      h->txn = 0; h->nSpt = 0;
      { int i; for(i=0;i<NSNAP;i++) imgFree(&h->snap[i]); }
      /* cursors keep working after commit and read the committed state
      ** (P5), so they are left alone here */
      { int i;
        for(i=0; i<NROOTMAX; i++)
          if( h->r[i].used ) scanTree(h, &h->r[i], 0);
      }
      break;
    }

    case FZ_OP_ROLLBACK: {
      gOp = "rollback";
      hb = drv_pick(&d, NH);
      h = &hs[hb];
      if( !h->live ) break;
      rc = btreelite_rollback(h->db);
      CK( rc==BTREELITE_OK );
      imgRestore(h, &h->base);
      h->txn = 0; h->nSpt = 0;
      { int i; for(i=0;i<NSNAP;i++) imgFree(&h->snap[i]); }
      fixCursorsAfterRestore((int)hb);
      break;
    }

    case FZ_OP_SAVEPOINT: {
      uint32_t spOp, spI;
      gOp = "savepoint";
      hb = drv_pick(&d, NH);
      spOp = drv_pick(&d, 2) + 1;         /* 1=RELEASE 2=ROLLBACK */
      spI  = drv_pick(&d, NSNAP+2);
      h = &hs[hb];
      if( !h->live ) break;
      if( h->txn!=2 || (int)spI >= h->nSpt ){
        fprintf(stderr, "SPJDBG: mirror-txn=%d engine-txn=%d h.nSpt=%d spOp=%u spI=%u\n",
                h->txn, btreelite_txn_state(h->db), h->nSpt, spOp, spI);
        rc = btreelite_savepoint(h->db,(int)spOp,(int)spI);
        CK( rc==BTREELITE_ERROR );
        break;
      }
      rc = btreelite_savepoint(h->db, (int)spOp, (int)spI);
      CK( rc==BTREELITE_OK );
      if( getenv("BTREELITE_FUZZ_DEBUG") )
        fprintf(stderr, "SPDBG act=%u op=%u i=%u pgno0=%u pc=%u nSpt=%d txn=%d\n",
                gAct, spOp, spI, (unsigned)h->r[0].pgno,
                (unsigned)btreelite_page_count(h->db), h->nSpt, h->txn);
      if( spI==0 && spOp==1 ){            /* RELEASE 0 == commit */
        imgSnap(h, &h->base);
        h->txn = 0; h->nSpt = 0;
        { int i; for(i=0;i<NSNAP;i++) imgFree(&h->snap[i]); }
        { int i;
          for(i=0; i<NROOTMAX; i++)
            if( h->r[i].used ) scanTree(h, &h->r[i], 0);
        }
      }else if( spI==0 ){                 /* ROLLBACK TO 0 */
        imgRestore(h, &h->base);
        h->nSpt = 1;
        { int i; for(i=0;i<NSNAP;i++) imgFree(&h->snap[i]); }
        fixCursorsAfterRestore((int)hb);
      }else if( spOp==1 ){                /* RELEASE i: keep state */
        int j;
        h->nSpt = (int)spI;
        for(j=(int)spI-1; j<NSNAP; j++) imgFree(&h->snap[j]);
      }else{                              /* ROLLBACK TO i: keep i open */
        int j;
        imgRestore(h, &h->snap[spI-1]);
        for(j=(int)spI; j<NSNAP; j++) imgFree(&h->snap[j]);
        h->nSpt = (int)spI + 1;
        fixCursorsAfterRestore((int)hb);
      }
      break;
    }

    case FZ_OP_CREATE_TREE: {
      int slot;
      gOp = "create_tree";
      hb = drv_pick(&d, NH);
      h = &hs[hb];
      if( !h->live ) break;
      if( h->txn!=2 ){
        rc = btreelite_create_tree(h->db, 0);
        CK( rc==BTREELITE_ERROR );
        break;
      }
      for(slot=0; slot<NROOTMAX && h->r[slot].used; slot++){}
      if( slot==NROOTMAX ) break;
      rc = btreelite_create_tree(h->db, &h->r[slot].pgno);
      CK( rc==BTREELITE_OK );
      h->r[slot].used = 1;
      h->r[slot].gen = h->nSpt;
      memset(&h->r[slot].mod, 0, sizeof(Model));
      break;
    }

    case FZ_OP_CLEAR_TREE: {
      gOp = "clear_tree";
      hb = drv_pick(&d, NH);
      h = &hs[hb];
      ci = drv_pick(&d, NROOTMAX);
      if( !h->live ) break;
      if( !h->r[ci].used ) break;
      if( h->txn!=2 ){
        rc = btreelite_clear_tree(h->db, h->r[ci].pgno);
        CK( rc==BTREELITE_ERROR );
        break;
      }
      rc = btreelite_clear_tree(h->db, h->r[ci].pgno);
      CK( rc==BTREELITE_OK );
      model_free(&h->r[ci].mod);
      tripCursors((int)hb, (int)ci);
      break;
    }

    case FZ_OP_CUR_OPEN: {
      uint32_t slot, wrf;
      gOp = "cursor_open";
      hb = drv_pick(&d, NH);
      h = &hs[hb];
      slot = drv_pick(&d, NROOTMAX);
      wrf = drv_pick(&d, 2);
      if( !h->live ) break;
      if( !h->r[slot].used ) break;
      for(ci=0; ci<NCUR && cs[ci].live; ci++){}
      if( ci==NCUR ) break;
      if( h->txn==0 ){
        rc = btreelite_cursor_open(h->db,h->r[slot].pgno,(int)wrf,&cs[ci].b);
        CK( rc==BTREELITE_ERROR );
        break;
      }
      rc = btreelite_cursor_open(h->db, h->r[slot].pgno, (int)wrf, &cs[ci].b);
      CK( rc==BTREELITE_OK );
      cs[ci].live = 1; cs[ci].h = (int)hb; cs[ci].slot = (int)slot;
      cs[ci].wr = (int)wrf; cs[ci].st = CS_FRESH; cs[ci].nk = 0;
      cs[ci].ep = 0;
      break;
    }

    case FZ_OP_CUR_CLOSE: {
      gOp = "cursor_close";
      ci = drv_pick(&d, NCUR);
      if( !cs[ci].live ) break;
      btreelite_cursor_close(cs[ci].b);
      memset(&cs[ci], 0, sizeof(Cur));
      break;
    }

    case FZ_OP_PUT: {
      gOp = "put";
      int kmode;
      ci = drv_pick(&d, NCUR);
      nk = drv_keylen(&d);
      kmode = (int)(drv_u8(&d) % 5);
      nv = drv_vallen(&d);
      c = &cs[ci];
      if( !c->live ) break;
      h = &hs[c->h];
      genKey(&d, kb, nk, kmode, &res);
      nk = res;
      genVal(&d, &h->r[c->slot].mod, vb, nv, &nv);
      if( !c->wr || h->txn!=2 ){
        rc = btreelite_put(c->b,(nk>0?(uint8_t*)kb:0),nk,(nv>0?(uint8_t*)vb:0),nv);
        CK( rc==BTREELITE_ERROR );
        break;
      }
      rc = btreelite_put(c->b, (nk>0?(uint8_t*)kb:0), nk,
                         (nv>0?(uint8_t*)vb:0), nv);
      if( rc!=BTREELITE_OK && getenv("BTREELITE_FUZZ_DEBUG") )
        fprintf(stderr, "PUTDBG act=%u nk=%d k=%.8s nv=%d h=%d slot=%d pgno=%u pagecount=%u txn=%d nSpt=%d wr=%d\n",
                gAct, nk, (char*)kb, nv, c->h, c->slot,
                (unsigned)h->r[c->slot].pgno,
                (unsigned)btreelite_page_count(h->db),
                btreelite_txn_state(h->db), h->nSpt, c->wr);
      CK( rc==BTREELITE_OK );
      if( model_put(&h->r[c->slot].mod, kb, nk, vb, nv) )
        fzFail("model OOM");
      {
        MEnt *e = 0;
        if( model_get(&h->r[c->slot].mod, kb, nk, &e) )
          fzFail("put: model mismatch");
        memcpy(c->k, kb, (size_t)nk);
        c->nk = nk;
        c->ep = e->epoch;
      }
      c->st = CS_VALID;
      ckContent(c);
      break;
    }

    case FZ_OP_GET: {
      uint8_t pb[FZ_KEYMAX];
      int present;
      gOp = "get";
      int kmode;
      ci = drv_pick(&d, NCUR);
      nk = drv_keylen(&d);
      kmode = (int)(drv_u8(&d) % 5);
      c = &cs[ci];
      if( !c->live ) break;
      h = &hs[c->h];
      genKey(&d, pb, nk, kmode, &res);
      nk = res;
      present = model_get(&h->r[c->slot].mod, pb, nk, 0)==0;
      rc = btreelite_get(c->b, (nk>0?(uint8_t*)pb:0), nk);
      CK( rc == (present ? BTREELITE_OK : BTREELITE_NOTFOUND) );
      landKey(c);
      break;
    }

    case FZ_OP_DEL: {
      uint8_t pb[FZ_KEYMAX];
      int present;
      gOp = "del";
      int kmode;
      ci = drv_pick(&d, NCUR);
      nk = drv_keylen(&d);
      kmode = (int)(drv_u8(&d) % 5);
      c = &cs[ci];
      h = &hs[c->h];
      if( !c->live ) break;
      genKey(&d, pb, nk, kmode, &res);
      nk = res;
      present = model_get(&h->r[c->slot].mod, pb, nk, 0)==0;
      if( !c->wr || h->txn!=2 ){
        rc = btreelite_del(c->b,(nk>0?(uint8_t*)pb:0),nk);
        CK( rc==BTREELITE_ERROR );
        break;
      }
      if( present ){
        int i;
        if( model_del(&h->r[c->slot].mod, pb, nk) ) fzFail("model del");
        for(i=0; i<NCUR; i++){
          if( cs[i].live && i!=(int)ci && cs[i].h==c->h
           && cs[i].slot==c->slot && cs[i].st==CS_VALID
           && cs[i].nk==nk && memcmp(cs[i].k,pb,(size_t)nk)==0 ){
            cs[i].st = CS_TRIPPED;
          }
        }
      }
      rc = btreelite_del(c->b, (nk>0?(uint8_t*)pb:0), nk);
      CK( rc == (present ? BTREELITE_OK : BTREELITE_NOTFOUND) );
      landKey(c);
      break;
    }

    case FZ_OP_SEEK: {
      uint8_t pb[FZ_KEYMAX];
      MEnt *lb;
      gOp = "seek";
      int kmode;
      ci = drv_pick(&d, NCUR);
      nk = drv_keylen(&d);
      kmode = (int)(drv_u8(&d) % 5);
      c = &cs[ci];
      if( !c->live ) break;
      h = &hs[c->h];
      genKey(&d, pb, nk, kmode, &res);
      nk = res;
      rc = btreelite_seek(c->b, (nk>0?(uint8_t*)pb:0), nk);
      CK( rc==BTREELITE_OK );
      lb = model_lower(&h->r[c->slot].mod, pb, nk);
      if( lb==0 ){
        CK( btreelite_eof(c->b) );
        setEOF(c);
      }else{
        CK( !btreelite_eof(c->b) );
        landKey(c);
        CK( c->nk==lb->nk && memcmp(c->k,lb->pk,(size_t)lb->nk)==0 );
        c->ep = lb->epoch;
      }
      break;
    }

    case FZ_OP_FIRST: case FZ_OP_LAST: {
      MEnt *t;
      gOp = op==FZ_OP_FIRST ? "first" : "last";
      ci = drv_pick(&d, NCUR);
      c = &cs[ci];
      if( !c->live ) break;
      res = -9;
      rc = op==FZ_OP_FIRST ? btreelite_first(c->b,&res)
                           : btreelite_last(c->b,&res);
      CK( rc==BTREELITE_OK );
      if( rootOf(c)->mod.n==0 ){
        CK( res==1 );
        setEOF(c);
      }else{
        CK( res==0 );
        landKey(c);
        t = modelAt(&rootOf(c)->mod, op==FZ_OP_FIRST ? 0 : rootOf(c)->mod.n-1);
        CK( c->nk==t->nk && memcmp(c->k,t->pk,(size_t)t->nk)==0 );
        c->ep = t->epoch;
      }
      break;
    }

    case FZ_OP_NEXT: case FZ_OP_PREV: {
      MEnt *e = 0, *t = 0;
      int i2 = -1, j2;
      gOp = op==FZ_OP_NEXT ? "next" : "prev";
      ci = drv_pick(&d, NCUR);
      c = &cs[ci];
      if( !c->live ) break;
      if( c->st==CS_VALID ){
        if( model_get(&rootOf(c)->mod, c->k, c->nk, &e) )
          fzFail("VALID cursor lost its entry");
        for(j2=0, t=rootOf(c)->mod.head; t; t=t->next, j2++){
          if( t==e ){ i2 = j2; break; }
        }
        if( i2<0 ) fzFail("model list broken");
      }
      rc = op==FZ_OP_NEXT ? btreelite_next(c->b) : btreelite_prev(c->b);
      if( c->st==CS_FRESH || c->st==CS_EOF ){
        CK( rc==BTREELITE_DONE );
        break;
      }
      if( c->st==CS_TRIPPED ){
        CK( rc==BTREELITE_OK || rc==BTREELITE_DONE );
        if( rc==BTREELITE_OK ) landKey(c); else setEOF(c);
        break;
      }
      if( op==FZ_OP_NEXT ){
        if( i2+1 < rootOf(c)->mod.n ){
          t = modelAt(&rootOf(c)->mod, i2+1);
          CK( rc==BTREELITE_OK );
          CK( !btreelite_eof(c->b) );
          landKey(c);
          CK( c->nk==t->nk && memcmp(c->k,t->pk,(size_t)t->nk)==0 );
          c->ep = t->epoch;
        }else{
          CK( rc==BTREELITE_DONE );
          setEOF(c);
        }
      }else{
        if( i2 > 0 ){
          t = modelAt(&rootOf(c)->mod, i2-1);
          CK( rc==BTREELITE_OK );
          CK( !btreelite_eof(c->b) );
          landKey(c);
          CK( c->nk==t->nk && memcmp(c->k,t->pk,(size_t)t->nk)==0 );
          c->ep = t->epoch;
        }else{
          CK( rc==BTREELITE_DONE );
          setEOF(c);
        }
      }
      break;
    }

    case FZ_OP_VALREAD: {
      uint32_t off, amt, chunk;
      MEnt *e = 0;
      uint8_t vb2[8192];
      gOp = "val_read";
      ci = drv_pick(&d, NCUR);
      c = &cs[ci];
      if( !c->live ) break;
      off = drv_pick(&d, 4096);
      amt = drv_pick(&d, 512);
      amt = amt ? amt : 1;
      if( c->st!=CS_VALID ){              /* FRESH/TRIPPED/EOF: error */
        rc = btreelite_value_read(c->b, off, amt, vb2);
        CK( rc==BTREELITE_ERROR );
        break;
      }
      if( model_get(&rootOf(c)->mod, c->k, c->nk, &e) )
        fzFail("VALID cursor lost its entry");
      if( (uint64_t)off+(uint64_t)amt > (uint64_t)e->nv ){
        rc = btreelite_value_read(c->b, off, amt, vb2);
        CK( rc==BTREELITE_ERROR );        /* range refused */
        break;
      }
      chunk = amt;
      rc = btreelite_value_read(c->b, off, chunk, vb2);
      CK( rc==BTREELITE_OK );
      if( e->epoch == c->ep && amt>0 ){
        if( memcmp(vb2, e->pv+off, chunk) ) fzFail("valread bytes differ");
      }
      break;
    }

    case FZ_OP_VALWRITE: {
      uint32_t off, amt, vmode, nlen;
      MEnt *e = 0;
      int idx;
      gOp = "val_write";
      ci = drv_pick(&d, NCUR);
      c = &cs[ci];
      if( !c->live ) break;
      h = &hs[c->h];
      off = drv_pick(&d, 8192);
      amt = drv_pick(&d, 1024);
      vmode = drv_pick(&d, 2);
      if( !c->wr || h->txn!=2 ){
        rc = btreelite_value_write(c->b,"x",1,(int)off,(int)amt,"y");
        CK( rc==BTREELITE_ERROR );
        break;
      }
      if( rootOf(c)->mod.n==0 ) break;
      /* target a real entry: half the time probe an out-of-range window,
      ** which must be refused; else rewrite a legal in-range window */
      idx = (int)(off % (uint32_t)rootOf(c)->mod.n);
      e = modelAt(&rootOf(c)->mod, idx);
      nlen = (uint32_t)(e->nv > 0 ? e->nv : 0);
      if( vmode==0 ){
        if( nlen==0 ) break;              /* nothing to rewrite */
        off = drv_rand(&d) % nlen;
        amt = 1 + drv_rand(&d) % (nlen - off);
        drv_fill(&d, vb, (int)amt);
        rc = btreelite_value_write(c->b, e->pk, e->nk, off, amt, vb);
        CK( rc==BTREELITE_OK );
        memcpy(e->pv+off, vb, amt);
        e->epoch = ++rootOf(c)->mod.epochSrc;
        landKey(c);
      }else{
        off = nlen + 1 + drv_rand(&d)%16; /* beyond the end */
        amt = 2 + drv_rand(&d)%8;
        rc = btreelite_value_write(c->b, e->pk, e->nk, off, amt, vb);
        CK( rc==BTREELITE_ERROR );
      }
      break;
    }

    case FZ_OP_CHECKPOINT: {
      uint32_t mode;
      gOp = "checkpoint";
      hb = drv_pick(&d, NH);
      h = &hs[hb];
      if( !h->live ) break;
      mode = drv_pick(&d, 4);
      rc = btreelite_checkpoint(h->db, (int)mode, 0, 0);
      CK( (h->txn!=0) ? rc==6 : rc==BTREELITE_OK );
      break;
    }

    case FZ_OP_JOURNAL: {
      uint32_t mode;
      gOp = "journal_mode";
      hb = drv_pick(&d, NH);
      h = &hs[hb];
      if( !h->live ) break;
      mode = drv_pick(&d, 2) + 4;
      if( h->txn!=0 && getenv("BTREELITE_FUZZ_DEBUG")
       && h->txn!=btreelite_txn_state(h->db) )
        fprintf(stderr, "JDBG mirror-txn=%d engine-txn=%d\n",
                h->txn, btreelite_txn_state(h->db));
      rc = btreelite_journal_mode(h->db, (int)mode);
      /* :memory: folds every request to MEMORY *before* the transaction
      ** guard, and an already-current request early-returns: on a memdb
      ** this always yields 4 -- the no-transition-inside-a-txn refusal
      ** is only reachable with file-backed handles (Target B). */
      CK( rc==BTREELITE_JOURNAL_MEMORY );
      break;
    }

    case FZ_OP_SYNC: {
      uint32_t lvl;
      gOp = "synchronous";
      hb = drv_pick(&d, NH);
      h = &hs[hb];
      if( !h->live ) break;
      lvl = drv_pick(&d, 3);
      rc = btreelite_synchronous(h->db, (int)lvl);
      CK( rc==h->sync );
      h->sync = (int)lvl;
      break;
    }

    case FZ_OP_AUTOCKPT: {
      gOp = "autocheckpoint";
      hb = drv_pick(&d, NH);
      h = &hs[hb];
      if( !h->live ) break;
      btreelite_wal_autocheckpoint(h->db, (int)drv_pick(&d, 20));
      break;
    }

    case FZ_OP_SETINT: {
      gOp = "set_interrupt";
      hb = drv_pick(&d, NH);
      h = &hs[hb];
      if( !h->live ) break;
      /* the flag can only be set, never cleared, within a handle */
      h->interrupt = 1;
      btreelite_set_interrupt(h->db);
      break;
    }

    case FZ_OP_INTEGRITY: {
      gOp = "integrity_check";
      hb = drv_pick(&d, NH);
      h = &hs[hb];
      ci = drv_pick(&d, NROOTMAX);
      if( !h->live ) break;
      if( !h->r[ci].used ) break;
      {
        char *z = 0; int nErr = -1;
        rc = btreelite_integrity_check(h->db, h->r[ci].pgno, 100, &nErr, &z);
        if( h->interrupt ) CK( rc==BTREELITE_INTERRUPT );
        else CK( rc==BTREELITE_OK );
        if( z ) btreelite_free(z);
      }
      break;
    }

    default:
      gNop++;
      break;
    }
  }
  terminalOracle();
  {
    int total = (int)gChk;
    resetAll();
    return total;
  }
}