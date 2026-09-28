#include "../include/btreelite.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int nFail = 0, nTest = 0;
#define CHECK(c) do{ nTest++; if(!(c)){ nFail++; printf("FAIL %d: %s\n", __LINE__, #c);} }while(0)

int main(void){
  btreelite_db *db = 0;
  btreelite_cur *cur = 0;
  unsigned iRoot = 0;
  char key[64], val[128];
  int rc, i, res = 0;

  unlink("t_kv.db");
  rc = btreelite_open("t_kv.db", &db);
  printf("open rc=%d\n", rc);
  if( rc ) return 1;

  rc = btreelite_begin(db, 1);
  CHECK(rc==0);
  rc = btreelite_create_tree(db, &iRoot);
  CHECK(rc==0);
  rc = btreelite_cursor_open(db, iRoot, 1, &cur);
  CHECK(rc==0);

  for(i=0;i<200;i++){
    int nk = sprintf(key, "key-%06d", (i*37)%1000);
    int nv = sprintf(val, "value-%06d-payload", i);
    rc = btreelite_put(cur, key, nk, val, nv);
    if( rc ){ printf("put rc=%d at i=%d\n", rc, i); }
    CHECK(rc==0);
  }
  rc = btreelite_commit(db);
  CHECK(rc==0);
  btreelite_cursor_close(cur);

  /* ---- Read back + verify all 200 unique keys ---- */
  rc = btreelite_begin(db, 0);
  CHECK(rc==0);
  rc = btreelite_cursor_open(db, iRoot, 0, &cur);
  CHECK(rc==0);
  cur = cur;
  for(i=0;i<200;i++){
    char got[128]; int nk, nv, nLen=0;
    uint32_t nVal=0;
    nk = sprintf(key, "key-%06d", (i*37)%1000);
    nv = sprintf(val, "value-%06d-payload", i);
    rc = btreelite_get(cur, key, nk);
    if( rc ){ printf("get rc=%d key=%s\n", rc, key); return 2; }
    CHECK(rc==0);
    rc = btreelite_key(cur, got, sizeof(got), &nLen);
    CHECK(rc==0);
    CHECK(nLen==nk && memcmp(got, key, nk)==0);
    rc = btreelite_value_size(cur, &nVal);
    CHECK(rc==0 && nVal==(uint32_t)nv);
    rc = btreelite_value_read(cur, 0, nVal, got);
    CHECK(rc==0 && memcmp(got, val, nv)==0);
  }
  rc = btreelite_commit(db);
  CHECK(rc==0);
  btreelite_cursor_close(cur);
  rc = btreelite_cursor_open(db, iRoot, 0, &cur);
  CHECK(rc==0);

  /* ---- Forward scan: count and order ---- */
  rc = btreelite_first(cur, &res);
  CHECK(rc==0 && res==0);
  {
    char prev[64]; int nPrev=0, nCount=0;
    memset(prev, 0, sizeof(prev));
    while(1){
      int nLen=0;
      if( btreelite_key(cur, key, sizeof(key), &nLen)==0 ){
        if( nCount>0 ) CHECK(strcmp(prev, key)<0);
        memcpy(prev, key, nLen);
      }
      nCount++;
      rc = btreelite_next(cur);
      if( rc!=0 ) break;
    }
    CHECK(nCount==200);
  }

  /* ---- Reverse scan consistency ---- */
  {
    int nCount=0;
    rc = btreelite_last(cur, &res);
    CHECK(rc==0 && res==0);
    while( rc==0 && res==0 ){
      nCount++;
      rc = btreelite_prev(cur);
      if( rc!=0 ) break;
      res = btreelite_eof(cur)?1:0;
      if( btreelite_eof(cur) ) break;
    }
    CHECK(nCount==200);
  }

  /* ---- Seek semantics ---- */
  {
    char got[128]; int nLen=0;
    rc = btreelite_seek(cur, "key-000500", 10);
    CHECK(rc==0);
    CHECK(btreelite_eof(cur)==0);
    rc = btreelite_key(cur, got, sizeof(got), &nLen);
    CHECK(rc==0);
    CHECK(strcmp(got, "key-000500")>=0);
    /* Seek past the end. */
    rc = btreelite_seek(cur, "zzzz", 4);
    CHECK(rc==0);
    CHECK(btreelite_eof(cur)!=0);
  }

  /* ---- Delete half, verify ---- */
  rc = btreelite_begin(db, 1);
  CHECK(rc==0);
  for(i=0;i<200;i+=2){
    int nk = sprintf(key, "key-%06d", (i*37)%1000);
    rc = btreelite_del(cur, key, nk);
    CHECK(rc==0);
  }
  rc = btreelite_commit(db);
  CHECK(rc==0);
  btreelite_cursor_close(cur);
  rc = btreelite_cursor_open(db, iRoot, 0, &cur);
  CHECK(rc==0);
  {
    int nCount=0;
    rc = btreelite_first(cur, &res);
    while( rc==0 && res==0 ){
      nCount++;
      rc = btreelite_next(cur);
    }
    CHECK(nCount==100);
  }

  /* ---- Rollback does not disturb the committed tree ---- */
  {
    rc = btreelite_begin(db, 1);
    CHECK(rc==0);
    rc = btreelite_cursor_open(db, iRoot, 1, &cur);
    CHECK(rc==0);
    rc = btreelite_put(cur, "temp-key", 8, "temp-value", 10);
    CHECK(rc==0);
    rc = btreelite_rollback(db);
    CHECK(rc==0);
    btreelite_cursor_close(cur);
    rc = btreelite_begin(db, 0);
    CHECK(rc==0);
    rc = btreelite_cursor_open(db, iRoot, 0, &cur);
    CHECK(rc==0);
    rc = btreelite_get(cur, "temp-key", 8);
    CHECK(rc==12 /* NOTFOUND */);
    {
      int nCount=0;
      rc = btreelite_first(cur, &res);
      while( rc==0 && res==0 ){ nCount++; rc = btreelite_next(cur); }
      CHECK(nCount==100);
    }
  }

  btreelite_cursor_close(cur);
  btreelite_close(db);
  unlink("t_kv.db");

  printf("%d checks, %d failures\n", nTest, nFail);
  return nFail?1:0;
}
