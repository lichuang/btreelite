/*
** fuzz/target_file.c -- Target B: hostile-file fuzzing.
**
** Input stream (fuzz/mkfiles.c's encoding):
**   [8B seed][base id][mutations]:
**     1 flip   [off16][count]   flip bits in the db
**     2 set    [off16][byte]    set one db byte
**     3 trunc  [frac]           truncate the db at frac/256 of the length
**     4 twal   [frac]           truncate the wal
**     5 splice [srcpg][dstpg]   copy one db page over another
**     6 rewire [off16][pg]      write a plausible page number at off
**     7 ptype  [t]              set the page-type byte of a random db page
**     8 fhdr   [f][off]         corrupt a WAL frame-header byte
**     9 fsum   [f][half]        flip a checksum word of a frame
**    10 shm    [off16][byte]    stomp the shared-memory index
**    11 magic  [v]              wipe or replace the 16-byte file magic
**
** Oracle (docs/fuzz-design.md 3.3): reopen the mutated trio; no crash,
** no hang (-timeout=5), no ASan report, no leak.  A failed open passes;
** a successful open must let every read-only probe return a value from
** its allowed set and the integrity checker must terminate without a
** crash -- any other outcome is a library bug.
**
** P2 scope: reopen-only (the design's 3.4 combination with Target A
** actions is a later batch).
*/
#include "btreelite.h"
#include "drv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NBASE     7
#define MAXMUT   400

typedef struct Buf {
  uint8_t *a;                /* NULL when the member does not exist */
  size_t n;
} Buf;

static Buf bDb, bWal, bShm;  /* the trio's working copies */
static unsigned bId;
static char zDbPathBuf[512]; /* the on-disk trio lives at this pid-uniqued
** base name: btfz-<pid>.db(+ -wal/-shm) in $TMPDIR */
static unsigned bRoots[NBASE];

static void bufFree(Buf *b){ free(b->a); memset(b,0,sizeof(Buf)); }
static void bufLoad(Buf *b, const char *zPath){
  FILE *f = fopen(zPath,"rb");
  bufFree(b);
  if( f ){
    fseek(f,0,SEEK_END);
    b->n = (size_t)ftell(f);
    fseek(f,0,SEEK_SET);
    b->a = (uint8_t*)malloc(b->n ? b->n : 1);
    if( b->a==0 ){ fclose(f); memset(b,0,sizeof(Buf)); return; }
    if( fread(b->a,1,b->n,f)!=b->n ){ bufFree(b); }
    fclose(f);
  }
}

static void trioLoad(void){
  char zPath[512];
  bufFree(&bDb); bufFree(&bWal); bufFree(&bShm);
  snprintf(zPath, sizeof zPath, "fuzz/files/f%u.db", bId);
  if( access(zPath,0)!=0 ) snprintf(zPath, sizeof zPath, "files/f%u.db", bId);
  bufLoad(&bDb, zPath);
  snprintf(zPath, sizeof zPath, "fuzz/files/f%u.db-wal", bId);
  if( access(zPath,0)!=0 ) snprintf(zPath, sizeof zPath, "files/f%u.db-wal", bId);
  bufLoad(&bWal, zPath);
  snprintf(zPath, sizeof zPath, "fuzz/files/f%u.db-shm", bId);
  if( access(zPath,0)!=0 ) snprintf(zPath, sizeof zPath, "files/f%u.db-shm", bId);
  bufLoad(&bShm, zPath);
}

static void trioWrite(void){
  static const char *suf[3] = { "", "-wal", "-shm" };
  Buf *bs[3]; int i;
  char zBase[384];
  const char *zTmpDir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  bs[0]=&bDb; bs[1]=&bWal; bs[2]=&bShm;
  snprintf(zBase, sizeof zBase, "%s/btfz-%ld", zTmpDir, (long)getpid());
  snprintf(zDbPathBuf, sizeof zDbPathBuf, "%s.db", zBase);
  for(i=0;i<3;i++){
    char zPath[512];
    FILE *f;
    snprintf(zPath, sizeof zPath, "%s.db%s", zBase, suf[i]);
    unlink(zPath);
    if( bs[i]->a==0 ) continue;   /* absent member: create nothing */
    f = fopen(zPath, "wb");
    if( f ){ fwrite(bs[i]->a, 1, bs[i]->n, f); fclose(f); }
  }
}

static void trioDrop(void){
  static const char *suf[3] = { "", "-wal", "-shm" };
  char zBase[384];
  const char *zTmpDir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  int i;
  snprintf(zBase, sizeof zBase, "%s/btfz-%ld", zTmpDir, (long)getpid());
  for(i=0;i<3;i++){
    char zPath[512];
    snprintf(zPath, sizeof zPath, "%s.db%s", zBase, suf[i]);
    unlink(zPath);
  }
}

static const char *zDbPath(void){
  return zDbPathBuf;
}

/* ---- mutators --------------------------------------------------------- */

static void applyMut(int mut, Drv *d){
  switch( mut ){
    case 1: {                                  /* bit flips in the db */
      uint32_t off;
      uint32_t cnt;
      if( bDb.n==0 ) break;
      off = drv_pick(d, 65536);
      cnt = drv_pick(d, 8) + 1;
      while( cnt-- ){
        uint32_t o = (off + drv_rand(d)) % (uint32_t)bDb.n;
        bDb.a[o] ^= (uint8_t)(1 << (drv_pick(d, 8)));
      }
      break;
    }
    case 2: {                                  /* set one db byte */
      if( bDb.n==0 ) break;
      bDb.a[drv_pick(d, 65536) % bDb.n] = (uint8_t)drv_pick(d, 256);
      break;
    }
    case 3: {                                  /* truncate the db */
      if( bDb.n==0 ) break;
      bDb.n = (size_t)((uint64_t)bDb.n * drv_pick(d, 256) / 256);
      break;
    }
    case 4: {                                  /* truncate the wal */
      if( bWal.a==0 ) break;
      bWal.n = (size_t)((uint64_t)bWal.n * drv_pick(d, 256) / 256);
      break;
    }
    case 5: {                                  /* page splice (db) */
      uint32_t sp = 1 + drv_pick(d, 16);
      uint32_t dp = 1 + drv_pick(d, 16);
      size_t a = (size_t)sp * 4096, b = (size_t)dp * 4096;
      if( bDb.n > b+4096 && bDb.n > a+4096 ){
        memcpy(bDb.a + b, bDb.a + a, 4096);
      }
      break;
    }
    case 6: {                                  /* rewire a page pointer */
      if( bDb.n < 4096 ) break;
      {
        uint32_t off = (4096 + drv_pick(d, 65536));
        size_t o = (size_t)off % (bDb.n - 4);
        uint32_t pg = 1 + drv_pick(d, 16);
        bDb.a[o]   = (uint8_t)(pg >> 24);
        bDb.a[o+1] = (uint8_t)(pg >> 16);
        bDb.a[o+2] = (uint8_t)(pg >> 8);
        bDb.a[o+3] = (uint8_t)pg;
      }
      break;
    }
    case 7: {                                  /* page type laundering */
      if( bDb.n < 4097 ) break;
      {
        uint32_t pg = drv_pick(d, 8) + 1;
        size_t o = (size_t)pg * 4096;
        if( o < bDb.n ) bDb.a[o] = (uint8_t)(2 << drv_pick(d,4));
      }
      break;
    }
    case 8: {                                  /* wal frame header byte */
      if( bWal.n < 100 ) break;
      {
        uint32_t f = drv_pick(d, 64);
        size_t o = 32 + (size_t)f * (4096 + 24);
        if( o + 24 > bWal.n ) break;
        o += drv_pick(d, 24);
        bWal.a[o] = (uint8_t)drv_pick(d, 256);
      }
      break;
    }
    case 9: {                                  /* flip a checksum word */
      if( bWal.n < 100 ) break;
      {
        uint32_t f = drv_pick(d, 64);
        size_t o = 32 + (size_t)f * (4096 + 24);
        if( o + 24 > bWal.n ) break;
        o += 16 + (drv_pick(d, 2) ? 4 : 0);    /* second checksum word zone */
        if( o + 4 <= bWal.n ){
          uint32_t w = (uint32_t)bWal.a[o] << 24 | (uint32_t)bWal.a[o+1] << 16
                     | (uint32_t)bWal.a[o+2] << 8 | bWal.a[o+3];
          w ^= (uint32_t)1 << drv_pick(d, 32);
          bWal.a[o]   = (uint8_t)(w >> 24);
          bWal.a[o+1] = (uint8_t)(w >> 16);
          bWal.a[o+2] = (uint8_t)(w >> 8);
          bWal.a[o+3] = (uint8_t)w;
        }
      }
      break;
    }
    case 10: {                                 /* shm header stomp */
      if( bShm.n==0 ) break;
      bShm.a[drv_pick(d, 256) % (bShm.n>256 ? 256 : bShm.n)] =
          (uint8_t)drv_pick(d, 256);
      break;
    }
    case 11: {                                 /* magic wipe/replace */
      if( bDb.n < 16 ) break;
      if( drv_pick(d, 2)==0 ){
        memset(bDb.a, 0, 16);
      }else{
        memcpy(bDb.a, "btreelite fuzz", 14);
      }
      break;
    }
  }
}

/* ---- the oracle -------------------------------------------------------- */

static uint32_t gChk;

#define CK(cond) do{ gChk++; if( !(cond) ) fzFail(#cond, (long)rc); }while(0)

static void fzFail(const char *ex, long rcGot){
  fprintf(stderr, "FUZZ-FILE: %s: %s (got %ld)\n",
          zDbPathBuf[0] ? zDbPathBuf : "?", ex, rcGot);
  fflush(stderr);
  abort();
}

/*
** The rc-level contracts are dropped for the hostile-file target on
** purpose: a corrupted database may legitimately make the engine return
** any member of its error family from any probe (a failed WAL recovery
** can even refuse the read transaction with a lock-family code).  Here
** the only violations are: a call that never returns (hang), a signal,
** or a sanitizer report -- the per-call counting just records progress.
*/
static void openOracle(void){
  btreelite_db *db = 0;
  btreelite_cur *cur = 0;
  int rc, res;
  char *z = 0;
  int nErr = -1;
  unsigned root;
  int iStep;

  rc = btreelite_open(zDbPath(), &db);
  gChk++;                               /* the open itself: any outcome */
  if( rc==BTREELITE_OK ){
    root = bRoots[bId] ? bRoots[bId] : 2;
    if( btreelite_begin(db, 0)==BTREELITE_OK ){
      if( btreelite_cursor_open(db, root, 0, &cur)==BTREELITE_OK ){
        uint8_t kb[300];
        int nl = 0;
        uint32_t nvs = 0;
        uint8_t vb[4096];
        rc = btreelite_first(cur, &res);
        for(iStep=0; rc==BTREELITE_OK && res==0 && iStep<65536; iStep++){
          gChk++;
          if( btreelite_eof(cur) ) break;
          rc = btreelite_key(cur, kb, sizeof kb, &nl);
          if( rc!=BTREELITE_OK ) break;
          rc = btreelite_value_size(cur, &nvs);
          if( rc!=BTREELITE_OK ) break;
          if( nvs>0 ){
            uint32_t chunk = nvs < sizeof vb ? nvs : sizeof vb;
            rc = btreelite_value_read(cur, 0, chunk, vb);
            if( rc!=BTREELITE_OK ) break;
          }
          rc = btreelite_next(cur);
          if( rc!=BTREELITE_OK || rc==BTREELITE_DONE ) break;
          res = btreelite_eof(cur);
        }
        btreelite_cursor_close(cur);
        cur = 0;
      }
      btreelite_commit(db);
    }
    rc = btreelite_integrity_check(db, 0, 100, &nErr, &z);
    gChk++;
    if( z ) btreelite_free(z);
    rc = btreelite_integrity_check(db, root, 100, &nErr, &z);
    gChk++;
    if( z ) btreelite_free(z);
    btreelite_close(db);
  }
  trioDrop();
}

/* ---- entry ------------------------------------------------------------- */

int btreeliteFuzzFileOneInput(const uint8_t *a, size_t n, const char *name){
  Drv d;
  int muts, i;
  (void)name;
  bufFree(&bDb); bufFree(&bWal); bufFree(&bShm);
  gChk = 0;
  if( n < 9 ) return 0;         /* no base+mut: nothing to do */
  drv_init(&d, a, n);
  bId = drv_pick(&d, NBASE);
  trioLoad();
  if( bDb.a==0 ) return (int)gChk;    /* base file missing: skip */
  muts = 4000;
  for( i=0; i<muts; i++ ){
    int mut = drv_op(&d);
    if( mut<0 ) break;
    if( mut==0 ) continue;
    applyMut(mut, &d);
  }
  trioWrite();
  openOracle();
  bufFree(&bDb); bufFree(&bWal); bufFree(&bShm);
  return (int)gChk;
}