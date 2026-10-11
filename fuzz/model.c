#include "model.h"
#include <stdlib.h>
#include <string.h>

static int mcmp(const uint8_t *a, int na, const uint8_t *b, int nb){
  int n = na < nb ? na : nb;
  int c = n ? memcmp(a, b, (size_t)n) : 0;
  if( c ) return c;
  return na - nb;
}

static MEnt *ment_new(const uint8_t *k, int nk, const uint8_t *v, int nv){
  MEnt *e = (MEnt*)malloc(sizeof(MEnt));
  if( e==0 ) return 0;
  e->pk = (uint8_t*)malloc(nk ? (size_t)nk : 1);
  e->pv = (uint8_t*)malloc(nv ? (size_t)nv : 1);
  if( e->pk==0 || e->pv==0 ){
    free(e->pk); free(e->pv); free(e);
    return 0;
  }
  if( nk ) memcpy(e->pk, k, (size_t)nk);
  if( nv ) memcpy(e->pv, v, (size_t)nv);
  e->nk = nk; e->nv = nv;
  e->epoch = 0;
  e->next = 0;
  return e;
}

int model_put(Model *m, const uint8_t *k, int nk,
              const uint8_t *v, int nv){
  MEnt **pp = &m->head;
  while( *pp && mcmp((*pp)->pk,(*pp)->nk, k, nk) < 0 ) pp = &(*pp)->next;
  if( *pp && mcmp((*pp)->pk,(*pp)->nk, k, nk)==0 ){
    MEnt *e = *pp;
    uint8_t *nvb = (uint8_t*)malloc(nv ? (size_t)nv : 1);
    if( nvb==0 ) return -1;
    if( nv ) memcpy(nvb, v, (size_t)nv);
    free(e->pv);
    e->pv = nvb;
    e->nv = nv;
    e->epoch = ++m->epochSrc;
    return 0;
  }
  {
    MEnt *e = ment_new(k, nk, v, nv);
    if( e==0 ) return -1;
    e->next = *pp;
    *pp = e;
    m->n++;
    m->vbytes += (size_t)nv;
    return 0;
  }
}

int model_get(Model *m, const uint8_t *k, int nk, MEnt **out){
  MEnt *e;
  for(e=m->head; e; e=e->next){
    int c = mcmp(e->pk,e->nk, k, nk);
    if( c==0 ){ if( out ) *out = e; return 0; }
    if( c>0 ) break;
  }
  return 1;
}

int model_del(Model *m, const uint8_t *k, int nk){
  MEnt **pp = &m->head;
  while( *pp && mcmp((*pp)->pk,(*pp)->nk, k, nk) < 0 ) pp = &(*pp)->next;
  if( *pp && mcmp((*pp)->pk,(*pp)->nk, k, nk)==0 ){
    MEnt *e = *pp;
    *pp = e->next;
    m->n--;
    m->vbytes -= (size_t)e->nv;
    free(e->pk); free(e->pv); free(e);
    return 0;
  }
  return 1;
}

MEnt *model_lower(Model *m, const uint8_t *k, int nk){
  MEnt *e;
  for(e=m->head; e; e=e->next){
    if( mcmp(e->pk,e->nk, k, nk) >= 0 ) return e;
  }
  return 0;
}

void model_free(Model *m){
  MEnt *e = m->head;
  while( e ){
    MEnt *nx = e->next;
    free(e->pk); free(e->pv); free(e);
    e = nx;
  }
  memset(m, 0, sizeof(*m));
}

Model *model_copy(const Model *m){
  Model *c = (Model*)malloc(sizeof(Model));
  MEnt *e, **pp;
  if( c==0 ) return 0;
  memset(c, 0, sizeof(*c));
  pp = &c->head;
  for(e=m->head; e; e=e->next){
    MEnt *ne = ment_new(e->pk, e->nk, e->pv, e->nv);
    if( ne==0 ){ model_free(c); free(c); return 0; }
    ne->epoch = e->epoch;
    *pp = ne;
    pp = &ne->next;
  }
  c->n = m->n;
  c->vbytes = m->vbytes;
  c->epochSrc = m->epochSrc;
  return c;
}

void model_takeover(Model *m, const Model *src){
  model_free(m);
  *m = (Model){0,0,0,0};
  {
    MEnt *e, **pp = &m->head;
    for(e=src->head; e; e=e->next){
      MEnt *ne = ment_new(e->pk, e->nk, e->pv, e->nv);
      if( ne==0 ) return;   /* partial state; harness treats OOM out */
      ne->epoch = e->epoch;
      *pp = ne;
      pp = &ne->next;
    }
  }
  m->n = src->n;
  m->vbytes = src->vbytes;
  m->epochSrc = src->epochSrc;
}