/*
** fuzz/drv.h -- deterministic action-stream decoder shared by the fuzz
** targets and the replay/seed tools.
**
** An action stream is: [8B little-endian xorshift seed][action records].
** Every record is [1B opcode][operands].  Control decisions (opcodes,
** indices, lengths) come from the stream itself; payload *content* comes
** from the seed-derived xorshift (a 1MB value cannot be spelled out in
** the stream, and libFuzzer coverage-guidance works on the stream).
** All randomness therefore derives from the input, never from time.
*/
#ifndef BTREELITE_FUZZ_DRV_H
#define BTREELITE_FUZZ_DRV_H
#include <stdint.h>
#include <stddef.h>

/* Action opcodes: the first operand byte selects 1..26 via (b-1)%26+1,
** so seed files may use the raw numbers 1..26 for readability. */
#define FZ_OP_OPEN        1   /* [h][flavor]   open db h (":memory:")    */
#define FZ_OP_CLOSE       2   /* [h]           close db h                */
#define FZ_OP_BEGIN       3   /* [h][wrflag]   begin (wrflag%3)          */
#define FZ_OP_COMMIT      4   /* [h]                                     */
#define FZ_OP_ROLLBACK    5   /* [h]                                     */
#define FZ_OP_SAVEPOINT   6   /* [h][op][i]    op%2+1: 1=RELEASE 2=RB    */
#define FZ_OP_CREATE_TREE 7   /* [h]                                     */
#define FZ_OP_CLEAR_TREE  8   /* [h][root]                             */
#define FZ_OP_CUR_OPEN    9   /* [h][root][wr]  wr%2                    */
#define FZ_OP_CUR_CLOSE  10   /* [c]                                      */
#define FZ_OP_PUT        11   /* [c][kl][km][vl][vm]                      */
#define FZ_OP_GET        12   /* [c][kl][km]                              */
#define FZ_OP_DEL        13   /* [c][kl][km]                              */
#define FZ_OP_SEEK       14   /* [c][kl][km]                              */
#define FZ_OP_FIRST      15   /* [c]                                      */
#define FZ_OP_LAST       16   /* [c]                                      */
#define FZ_OP_NEXT       17   /* [c]                                      */
#define FZ_OP_PREV       18   /* [c]                                      */
#define FZ_OP_VALREAD    19   /* [c][off][amt]                            */
#define FZ_OP_VALWRITE   20   /* [c][off][amt][big]                       */
#define FZ_OP_CHECKPOINT 21   /* [h][mode] mode%4                         */
#define FZ_OP_JOURNAL    22   /* [h][mode] mode%2+4                       */
#define FZ_OP_SYNC       23   /* [h][lvl] lvl%3                           */
#define FZ_OP_AUTOCKPT   24   /* [h][nf]                                  */
#define FZ_OP_SETINT     25   /* [h] the interrupt flag can only turn on */
#define FZ_OP_INTEGRITY  26   /* [h][root]                                */

#define FZ_KEYMAX   255
#define FZ_VALMAX   65536
#define FZ_MAXACT   20000

typedef struct Drv {
  const uint8_t *p;          /* control stream */
  size_t n, i;
  uint64_t rng;              /* content PRNG state (from the 8B seed) */
  uint32_t ctr;              /* content counter (pattern variety) */
  uint32_t vbytes;           /* value-byte budget consumed */
} Drv;

void drv_init(Drv *d, const uint8_t *a, size_t n);
int  drv_op(Drv *d);                   /* opcode 1..26, or -1 when out */
uint32_t drv_u8(Drv *d);               /* 0 when the stream is exhausted */
uint32_t drv_pick(Drv *d, uint32_t n); /* 0..n-1 (0 when n==0) */
uint32_t drv_rand(Drv *d);             /* raw PRNG word (content-only) */
int      drv_keylen(Drv *d);           /* threshold-biased, 0..FZ_KEYMAX */
int      drv_vallen(Drv *d);           /* tier-biased, 0..FZ_VALMAX      */

/* Fill buf[0..n-1] with deterministic pseudo-content.  Every ~32nd call
** produces constant fill (all-0x00 or all-0xff) to expose compression and
** memcmp edge handling. */
void drv_fill(Drv *d, uint8_t *buf, int n);

#endif