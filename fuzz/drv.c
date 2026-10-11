#include "drv.h"
#include <string.h>

void drv_init(Drv *d, const uint8_t *a, size_t n){
  int j;
  memset(d, 0, sizeof(*d));
  d->p = a; d->n = n;
  for(j=0; j<8 && (size_t)j<n; j++){
    d->rng |= ((uint64_t)a[j]) << (8*j);
  }
  d->i = ((size_t)n > 8) ? 8 : n;
  if( d->rng==0 ) d->rng = 0x9e3779b97f4a7c15ULL;
}

static uint64_t drv_step(Drv *d){
  d->rng ^= d->rng >> 12;
  d->rng ^= d->rng << 25;
  d->rng ^= d->rng >> 27;
  return d->rng * 2685821657736338717ULL;
}

uint32_t drv_rand(Drv *d){
  d->ctr++;
  return (uint32_t)(drv_step(d) >> 32);
}

int drv_op(Drv *d){
  uint32_t b;
  if( d->i >= d->n ) return -1;
  b = d->p[d->i++];
  if( b==0 ) return 0;
  return (int)((b - 1) % 26) + 1;
}

uint32_t drv_u8(Drv *d){
  if( d->i >= d->n ) return 0;
  return d->p[d->i++];
}

uint32_t drv_pick(Drv *d, uint32_t n){
  if( n==0 ) return 0;
  return drv_u8(d) % n;
}

int drv_keylen(Drv *d){
  uint32_t b = drv_u8(d);
  static const uint8_t lut[8] = {0,1,2,127,128,129,254,255};
  if( (b & 1)==0 ) return lut[(b >> 5) & 7];
  return (int)(b % 256);
}

int drv_vallen(Drv *d){
  /* 32-bit value lengths; the harness shrinks these under the model
  ** budget.  Tiers sit on minLocal(489), maxLocal(3813) and cell/page
  ** boundaries so the spill code is on every path. */
  static const uint32_t vtab[12] = {
    0, 1, 10, 488, 489, 490, 3812, 3813, 3814, 4092, 8185, 65536
  };
  uint32_t b = drv_u8(d);
  if( (b & 3)==0 ) return (int)vtab[(b >> 2) % 12];
  return (int)(1 + ((uint64_t)b * 2654435761u + drv_rand(d)) % 4096);
}

void drv_fill(Drv *d, uint8_t *buf, int n){
  int j;
  uint32_t t = (d->ctr % 32);
  if( t==27 ){
    memset(buf, 0x00, (size_t)n);
    return;
  }
  if( t==28 ){
    memset(buf, 0xff, (size_t)n);
    return;
  }
  for(j=0; j<n; j++){
    buf[j] = (uint8_t)(drv_step(d) >> 24);
  }
}