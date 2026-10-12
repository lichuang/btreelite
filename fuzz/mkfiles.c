/*
** fuzz/mkfiles.c -- deterministically build the hostile-file seed corpus.
**
** Two outputs:
**   fuzz/files/f<N>.db(+ -wal/-shm)  base database trios generated here
**                    through the public API (regenerated fresh by
**                    `make fuzz-files`, not checked in);
**   fuzz/fileseeds/*.bin  mutation streams for fuzz/target_file.c
**                    (checked in, tiny).
**
** The trios are copied while the generating handle is still open, so
** WAL frames and the shared-memory index exist as they do mid-life.
**
** Build/run tool, not part of the library.
*/
#include "btreelite.h"
#include "drv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint8_t sbuf[1<<16];
static size_t sn;

static void sb(uint64_t seed){
  int i;
  for(i=0; i<8; i++) sbuf[sn++] = (uint8_t)(seed >> (8*i));
}
static void x(int o){ sbuf[sn++] = (uint8_t)o; }

static void flush(const char *zn){
  char path[256];
  FILE *f;
  snprintf(path, sizeof path, "fuzz/fileseeds/%s.bin", zn);
  f = fopen(path, "wb");
  if( f==0 ){ fprintf(stderr, "mkfiles: cannot write %s\n", path); exit(1); }
  fwrite(sbuf, 1, sn, f);
  fclose(f);
  sn = 0;
}

/* ---- base database builder ------------------------------------------- */

static void chk(const char *z, int rc){
  if( rc!=BTREELITE_OK ){
    fprintf(stderr, "mkfiles: %s failed rc=%d\n", z, rc);
    exit(1);
  }
}

static void saveTrio(const char *zSrc, unsigned iFile){
  static const char *suf[3] = { "", "-wal", "-shm" };
  int i;
  for(i=0; i<3; i++){
    char a[256], b[256];
    FILE *f, *g;
    size_t n;
    static uint8_t big[1<<21];                 /* 2MB reads, fine */
    snprintf(a, sizeof a, "%s%s", zSrc, suf[i]);
    f = fopen(a, "rb");
    if( f==0 ) continue;                        /* absent part: fine */
    n = fread(big, 1, sizeof big, f);
    fclose(f);
    snprintf(b, sizeof b, "fuzz/files/f%u.db%s", iFile, suf[i]);
    g = fopen(b, "wb");
    if( g==0 ){ fprintf(stderr,"mkfiles: cannot write %s\n", b); exit(1); }
    fwrite(big, 1, n, g);
    fclose(g);
  }
}

static unsigned newTree(btreelite_db *db, btreelite_cur **ppCur){
  unsigned iRoot = 0;
  chk("begin", btreelite_begin(db,1));
  chk("create", btreelite_create_tree(db,&iRoot));
  chk("cur", btreelite_cursor_open(db,iRoot,1,ppCur));
  return iRoot;
}

static unsigned mkBase(unsigned iFile){
  char zPath[256];
  btreelite_db *db = 0;
  btreelite_cur *cur = 0;
  unsigned iRoot = 0;
  char key[64], val[70000];
  int i;

  snprintf(zPath, sizeof zPath, "fuzz/files/_b%u.db", iFile);
  unlink(zPath);
  snprintf(zPath, sizeof zPath, "fuzz/files/_b%u.db-wal", iFile);
  unlink(zPath);
  snprintf(zPath, sizeof zPath, "fuzz/files/_b%u.db", iFile);
  chk("open", btreelite_open(zPath, &db));
  memset(val, 'v', sizeof val);

  switch( iFile ){
    case 0: {                                    /* empty committed tree */
      iRoot = newTree(db, &cur);
      chk("commit", btreelite_commit(db));
      break;
    }
    case 1: {                                    /* 64 short entries */
      iRoot = newTree(db, &cur);
      for(i=0;i<64;i++){
        int nk = snprintf(key,sizeof key,"key-%04d",i);
        int nv = snprintf(val,sizeof val,"v%04d",i);
        chk("put", btreelite_put(cur,key,nk,val,nv));
      }
      chk("commit", btreelite_commit(db));
      break;
    }
    case 2: {                                    /* 3000 entries: deep tree */
      iRoot = newTree(db, &cur);
      for(i=0;i<3000;i++){
        int nk = snprintf(key,sizeof key,"row-%07d",i);
        int nv = snprintf(val,sizeof val,"deep-%07d",i);
        chk("put", btreelite_put(cur,key,nk,val,nv));
      }
      chk("commit", btreelite_commit(db));
      break;
    }
    case 3: {                                    /* 12 x 9000B overflowed */
      iRoot = newTree(db, &cur);
      for(i=0;i<12;i++){
        int nk = snprintf(key,sizeof key,"big-%04d",i);
        int nv = snprintf(val,200,"ovfl-%04d-",i);
        memset(val+nv, 'x', 8980);
        nv += 8980;
        chk("put", btreelite_put(cur,key,nk,val,nv));
      }
      chk("commit", btreelite_commit(db));
      break;
    }
    case 4: {                                    /* freelist-heavy */
      iRoot = newTree(db, &cur);
      for(i=0;i<3000;i++){
        int nk = snprintf(key,sizeof key,"row-%07d",i);
        int nv = snprintf(val,sizeof val,"free-%07d",i);
        chk("put", btreelite_put(cur,key,nk,val,nv));
      }
      for(i=0;i<1500;i++){
        int nk = snprintf(key,sizeof key,"row-%07d",(i*131)%45000);
        int rc2 = btreelite_del(cur,key,nk);
        if( rc2 && rc2!=BTREELITE_NOTFOUND ){
          fprintf(stderr, "mkfiles: del failed rc=%d\n", rc2);
          exit(1);
        }
      }
      chk("commit", btreelite_commit(db));
      break;
    }
    case 5: {                                    /* WAL-heavy trio */
      iRoot = newTree(db, &cur);
      btreelite_wal_autocheckpoint(db, 0);       /* keep every frame */
      for(int t=0;t<40;t++){
        for(int i2=0;i2<8;i2++){
          int nk = snprintf(key,sizeof key,"w%04d-%03d",t,i2);
          int nv = snprintf(val,sizeof val,"wal-frame-%04d-%03d",t,i2);
          chk("put", btreelite_put(cur,key,nk,val,nv));
        }
        chk("commit", btreelite_commit(db));
        chk("begin", btreelite_begin(db,1));
      }
      saveTrio(zPath, 5);                        /* 320 frames on disk */
      btreelite_cursor_close(cur);
      cur = 0;
      break;
    }
    case 6: {                                    /* half-commit trio */
      iRoot = newTree(db, &cur);
      for(i=0;i<40;i++){
        int nk = snprintf(key,sizeof key,"half-%04d",i);
        int nv = snprintf(val,sizeof val,"uncommitted-%04d",i);
        chk("put", btreelite_put(cur,key,nk,val,nv));
      }
      saveTrio(zPath, 6);                        /* before close: uncommitted */
      btreelite_cursor_close(cur);
      cur = 0;
      break;
    }
  }
  if( cur ) btreelite_cursor_close(cur);
  btreelite_close(db);
  return iRoot;
}

int main(void){
  unsigned roots[7];
  int i;

  mkdir("fuzz", 0777);
  mkdir("fuzz/files", 0777);
  mkdir("fuzz/fileseeds", 0777);
  for(i=0; i<7; i++){
    roots[i] = mkBase((unsigned)i);
  }

  /* ---- fileseeds: [8B seed][base id][mutations] ----------------------- */
  /* mutation encodings used by fuzz/target_file.c:
  **   1 flip   [off16][count]
  **   2 set    [off16][byte]
  **   3 trunc  [frac]      truncate the db at frac/256 of its length
  **   4 twal   [frac]      truncate the wal at frac/256 of its length
  **   5 splice [srcpg][dstpg]  copy a 4096-byte page over another (db)
  **   6 rewire [off16][pg] write a (valid-looking) page number at off
  **   7 ptype  [t]         set the page-type byte of page 1..: t%8
  **   8 fhdr   [f][off16]  corrupt WAL frame header (f%frames, off%24)
  **   9 fsum   [f][half]   flip one checksum word of a frame
  **  10 shm    [off16][byte]  stomp the shm header region
  **  11 magic  [v]         wipe/replace the 16-byte header magic        */

  sb(0x00000000000000b1ULL); x(1);
  x(1); x(120); x(0);       /* flip 8 bytes in the db near page 1 */
  x(1); x(4200); x(3);      /* and inside page 2 */
  flush("t01-flips-small");

  sb(0x00000000000000b2ULL); x(1);
  x(7); x(1);               /* page type of an early page: junk */
  x(7); x(3);               /* another */
  x(2); x(3000); x(0xff);   /* set one heap byte */
  flush("t02-paytypelaunder");

  sb(0x00000000000000b3ULL); x(1);
  x(3); x(180);             /* truncate db at ~70% of its length */
  x(3); x(20);              /* truncate again near the start */
  flush("t03-truncdb-deep");

  sb(0x00000000000000b4ULL); x(3);
  x(6); x(12); x(13);       /* rewire an overflow pointer to page 13 */
  x(6); x(130); x(12);      /* second chain rewire (possible cycle) */
  x(6); x(100); x(12);      /* third */
  flush("t04-ovfl-rewire");

  sb(0x00000000000000b5ULL); x(5);
  x(9); x(30); x(0);        /* flip a checksum word of frame 30 */
  x(4); x(150);             /* truncate the WAL mid-frame */
  x(8); x(30); x(5);        /* corrupt a frame header byte */
  x(9); x(10); x(1);        /* flip another checksum */
  flush("t05-walcorrupt");

  sb(0x00000000000000b6ULL); x(5);
  x(10); x(32); x(0x00);    /* saltA[0] cleared? off 32 = aSalt zone */
  x(10); x(24); x(0xff);    /* index header mxFrame region */
  x(10); x(40); x(0xaa);    /* aReadMark[0] low half */
  x(10); x(0); x(0x12);     /* version bytes */
  flush("t06-shm-stomp");

  sb(0x00000000000000b7ULL); x(6);
  x(4); x(77);              /* truncate the uncommitted WAL */
  x(4); x(255);             /* truncate at ~100% (edge) */
  x(1); x(4096); x(5);      /* bit flips inside the wal-header page */
  flush("t07-halfcommit");

  sb(0x00000000000000b8ULL); x(1);
  x(6); x(8); x(2);         /* child pointer of an interior cell rewire */
  x(6); x(300); x(1);       /* points page 1: cycle-ish */
  x(5); x(4); x(5);         /* splice page 4 over page 5 */
  x(5); x(5); x(4);
  flush("t08-childrewire");

  sb(0x00000000000000b9ULL); x(1);
  x(11); x(0);              /* wipe the magic */
  x(11); x(1);              /* replace the magic with 'bt...' filler */
  x(1); x(0); x(7);
  flush("t09-magicwipes");

  sb(0x00000000000000baULL); x(1);
  for(i=0;i<12;i++){ x(5); x((i*7)%8+2); x(((i+1)*11)%8+2); }   /* page splices */
  x(3); x(240);
  flush("t10-splicesoup");

  sb(0x00000000000000bbULL); x(5);
  x(4); x(0);               /* wal truncated to 0 */
  x(11); x(0);              /* magic gone */
  x(10); x(16); x(0x00);
  flush("t11-wal-gone");

  sb(0x00000000000000bcULL); x(2);
  for(i=0;i<200;i++){
    int m = (i*5)%11+1;
    x(m);
    if( m==1||m==2||m==6||m==10 ){ x((i*97)%6000); x((i*13)%200); }
    else if( m==5 ){ x((i*3)%8+2); x((i*7)%8+2); }
    else if( m==3||m==4 ){ x((i*31)%256); }
    else if( m==8||m==9 ){ x((i*7)%40); x((i*17)%24); }
    else if( m==7 ){ x(i%8); }
    else { x(i); }
  }
  flush("t12-randomsoup");

  for(i=0; i<7; i++){                          /* drop the generating copies */
    char zPath[256];
    snprintf(zPath, sizeof zPath, "fuzz/files/_b%u.db", i); unlink(zPath);
    snprintf(zPath, sizeof zPath, "fuzz/files/_b%u.db-wal", i); unlink(zPath);
    snprintf(zPath, sizeof zPath, "fuzz/files/_b%u.db-shm", i); unlink(zPath);
  }
  printf("mkfiles: 7 bases in fuzz/files/, 12 seeds in fuzz/fileseeds/\n");
  return 0;
}