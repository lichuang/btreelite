/*
** White-box test: put/get a value larger than one page.  Exercises the
** overflow-cell path of fillInCell and the overflow branch of the KV value
** accessors.
*/
#include "../include/btreelite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int nFail = 0, nTest = 0;
#define CHECK(c) do{ nTest++; if(!(c)){ nFail++; printf("FAIL %d: %s\n", __LINE__, #c);} }while(0)

int main(void){
  btreelite_db *db=0; btreelite_cur *cur=0; unsigned root=0;
  char key[64], val[256];
  int rc, i, res=0;
  unlink("t_big.db");
  rc = btreelite_open("t_big.db",&db);
  CHECK(rc==0);
  if( rc ) return 1;
  rc = btreelite_begin(db,1);
  CHECK(rc==0);
  rc = btreelite_create_tree(db,&root);
  CHECK(rc==0);
  rc = btreelite_cursor_open(db,root,1,&cur);
  CHECK(rc==0);
  for(i=0;i<100;i++){
    int nk=sprintf(key,"key-%06d",(i*37)%1000);
    int nv=sprintf(val,"value-%06d",i);
    rc=btreelite_put(cur,key,nk,val,nv);
    CHECK(rc==0);
  }
  {
    char *big=(char*)malloc(200000);
    char *out=(char*)malloc(200000);
    uint32_t nOut=0;
    memset(big,'x',200000);
    sprintf(big,"big-head-%06d",1);
    rc=btreelite_put(cur,"big-key",7,big,200000);
    printf("put(big) rc=%d\n", rc);
    CHECK(rc==0);
    rc=btreelite_commit(db);
    CHECK(rc==0);
    btreelite_cursor_close(cur);
    rc = btreelite_begin(db, 0);
    CHECK(rc==0);
    rc = btreelite_cursor_open(db, root, 0, &cur);
    CHECK(rc==0);
    rc=btreelite_get(cur,"big-key",7);
    CHECK(rc==0);
    rc=btreelite_value_size(cur,&nOut);
    CHECK(rc==0 && nOut==200000);
    rc=btreelite_value_read(cur,0,nOut,out);
    CHECK(rc==0);
    CHECK(memcmp(out,big,200000)==0);
    rc=btreelite_value_read(cur,100000,1000,out);
    CHECK(rc==0 && memcmp(out,big+100000,1000)==0);
    free(big); free(out);
  }
  btreelite_cursor_close(cur);

  /*
  ** Regression: a value at or below maxLeaf (4061 bytes on a 4 KB page)
  ** sharing a leaf with an overflowing value used to be unfindable, because
  ** the balance_quick() fast path built the interior divider from the intkey
  ** varint rather than the KV key.  Store small values straddling maxLeaf
  ** alongside a large one and require every key to be found again.
  */
  {
    static const int aSmall[] = { 0, 1, 4058, 4059, 4060, 4061, 4062, 4063 };
    const int nSmall = (int)(sizeof(aSmall)/sizeof(aSmall[0]));
    char *payload = (char*)malloc(200000);
    char skey[32], got[32];
    int k, nLen;
    uint32_t nOut;

    rc = btreelite_begin(db, 1);
    CHECK(rc==0);
    rc = btreelite_create_tree(db, &root);
    CHECK(rc==0);
    rc = btreelite_cursor_open(db, root, 1, &cur);
    CHECK(rc==0);
    memset(payload, 'p', 200000);
    for(k=0;k<nSmall;k++){
      int nk = sprintf(skey, "sz-%04d", aSmall[k]);
      rc = btreelite_put(cur, skey, nk, payload, aSmall[k]);
      CHECK(rc==0);
    }
    rc = btreelite_put(cur, "big-Z", 5, payload, 200000);
    CHECK(rc==0);
    rc = btreelite_commit(db);
    CHECK(rc==0);
    btreelite_cursor_close(cur);

    rc = btreelite_begin(db, 0);
    CHECK(rc==0);
    rc = btreelite_cursor_open(db, root, 0, &cur);
    CHECK(rc==0);
    for(k=0;k<nSmall;k++){
      int nk = sprintf(skey, "sz-%04d", aSmall[k]);
      rc = btreelite_get(cur, skey, nk);
      CHECK(rc==0);
      if( rc==0 ){
        nLen = 0;
        rc = btreelite_key(cur, got, sizeof(got), &nLen);
        CHECK(rc==0 && nLen==nk && memcmp(got, skey, nk)==0);
        nOut = 0;
        rc = btreelite_value_size(cur, &nOut);
        CHECK(rc==0 && (int)nOut==aSmall[k]);
      }
    }
    rc = btreelite_get(cur, "big-Z", 5);
    CHECK(rc==0);
    nOut = 0;
    rc = btreelite_value_size(cur, &nOut);
    CHECK(rc==0 && nOut==200000);
    btreelite_cursor_close(cur);
    btreelite_commit(db);
    free(payload);
  }

  btreelite_close(db);
  unlink("t_big.db");
  printf("%d checks, %d failures\n", nTest, nFail);
  return nFail?1:0;
}