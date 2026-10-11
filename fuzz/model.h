/*
** fuzz/model.h -- the in-memory oracle: an ordered (key,value) multiset
** of one tree.  Entries keep an epoch counter; the harness parks cursors
** together with the epoch they landed on so content reads are only
** compared when the entry has not been rewritten by another cursor
** since (the reader-owning-page subtlety btreelite inherits from SQLite
** is accepted, not asserted).
*/
#ifndef BTREELITE_FUZZ_MODEL_H
#define BTREELITE_FUZZ_MODEL_H
#include <stdint.h>
#include <stddef.h>

typedef struct MEnt {
  struct MEnt *next;
  uint8_t *pk;               /* malloc'd key copy */
  uint8_t *pv;               /* malloc'd value copy */
  int nk, nv;
  uint32_t epoch;
} MEnt;

typedef struct Model {
  MEnt *head;                /* sorted by key: memcmp + length tiebreak */
  int n;
  size_t vbytes;             /* total value bytes buffered */
  uint32_t epochSrc;
} Model;

int  model_put(Model *m, const uint8_t *k, int nk,
               const uint8_t *v, int nv);
int  model_get(Model *m, const uint8_t *k, int nk, MEnt **out);
int  model_del(Model *m, const uint8_t *k, int nk);
/* First entry >= (k,nk); NULL when there is none. */
MEnt *model_lower(Model *m, const uint8_t *k, int nk);
void model_free(Model *m);
Model *model_copy(const Model *m);   /* deep copy, NULL on OOM */
void model_takeover(Model *m, const Model *src);  /* deep replace */

#endif