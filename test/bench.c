/*
** 2026 September 30
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** The KV-vs-SQL throughput benchmark the extraction plan calls for.
**
** Both engines run the identical point-lookup / append / overwrite /
** update / delete workload over the same number of rows, with rowids as
** keys for SQL and byte-string keys for btreelite.  The btreelite side
** uses the public API; the SQL side uses libsqlite3 through prepared
** statements, with WAL mode and synchronous=FULL to match btreelite's
** defaults.
**
**   Usage: ./bench <batch> <nBatch>
**     batch   rows per batch (one transaction per batch)
**     nBatch  number of batches
**
** Output is one line per measured phase with the elapsed wall time and
** rows/s, for both engines.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#ifdef BENCH_SQL
# include "sqlite3.h"
#else
# include "btreelite.h"
#endif

static double dsec(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec/1e9;
}

static int nBatch = 10;
static int batch = 1000;


# ifdef BENCH_SQL
# else
static int rowKey(int n, char *z){
  return sprintf(z, "row-%08d", n);
}
# endif

#ifdef BENCH_SQL

# define ENGINE "sqlite3(sql)"
static sqlite3 *db = 0;

static void stmt(const char *zSql, sqlite3_stmt **pp){
  int rc = sqlite3_prepare_v2(db, zSql, -1, pp, 0);
  if( rc!=SQLITE_OK ){
    printf("prepare failed: %s\n", sqlite3_errmsg(db));
    exit(2);
  }
}

static void setupDb(const char *zPath){
  char zWal[256];
  int rc;
  unlink(zPath);
  sprintf(zWal, "%s-wal", zPath); unlink(zWal);
  sprintf(zWal, "%s-shm", zPath); unlink(zWal);
  rc = sqlite3_open(zPath, &db);
  if( rc!=SQLITE_OK ){ printf("open failed\n"); exit(2); }
  rc = sqlite3_exec(db, "PRAGMA journal_mode=WAL", 0, 0, 0);
  if( rc ){ printf("wal failed\n"); exit(2); }
  sqlite3_exec(db, "PRAGMA synchronous=FULL", 0, 0, 0);
  sqlite3_exec(db,
    "CREATE TABLE t(k INTEGER PRIMARY KEY, v BLOB NOT NULL)", 0, 0, 0);
}

static void cleanupDb(void){ sqlite3_close(db); }

/* Append nBatch batches through one prepared INSERT. */
static double benchInsert(void){
  sqlite3_stmt *p = 0;
  char zVal[128];
  int i, b;
  double t0 = dsec();
  stmt("INSERT INTO t(k,v) VALUES(?1,?2)", &p);
  for(b=0; b<nBatch; b++){
    if( sqlite3_exec(db, "BEGIN", 0, 0, 0)!=SQLITE_OK ) return -1;
    for(i=0; i<batch; i++){
      int n = b*batch + i;
      int nk = sprintf(zVal, "value-%08d-payload", n);
      sqlite3_bind_int(p, 1, n);
      sqlite3_bind_text(p, 2, zVal, nk, SQLITE_STATIC);
      if( sqlite3_step(p)!=SQLITE_DONE ){
        printf("insert failed: %s\n", sqlite3_errmsg(db));
        exit(2);
      }
      sqlite3_reset(p);
    }
    sqlite3_exec(db, "COMMIT", 0, 0, 0);
  }
  sqlite3_finalize(p);
  return dsec()-t0;
}

/* Point lookup by primary key. */
static double benchLookup(int nLook){
  sqlite3_stmt *p = 0;
  int i;
  double t0 = dsec();
  stmt("SELECT v FROM t WHERE k=?1", &p);
  for(i=0; i<nLook; i++){
    sqlite3_bind_int(p, 1, i % (nBatch*batch));
    if( sqlite3_step(p)!=SQLITE_ROW ){
      printf("lookup failed\n"); exit(2);
    }
    sqlite3_reset(p);
  }
  sqlite3_finalize(p);
  return dsec()-t0;
}

/* Overwrite in place (same key, same size). */
static double benchOverwrite(int nRows){
  sqlite3_stmt *p = 0;
  char zVal[128];
  int i;
  double t0 = dsec();
  sqlite3_exec(db, "BEGIN", 0, 0, 0);
  stmt("UPDATE t SET v=?2 WHERE k=?1", &p);
  for(i=0; i<nRows; i++){
    int nk = sprintf(zVal, "value-%08d-payload", i);
    sqlite3_bind_int(p, 1, i);
    sqlite3_bind_text(p, 2, zVal, nk, SQLITE_STATIC);
    sqlite3_step(p);
    sqlite3_reset(p);
  }
  sqlite3_finalize(p);
  sqlite3_exec(db, "COMMIT", 0, 0, 0);
  return dsec()-t0;
}

/* Delete every row. */
static double benchDelete(int nRows){
  sqlite3_stmt *p = 0;
  int i;
  double t0 = dsec();
  sqlite3_exec(db, "BEGIN", 0, 0, 0);
  stmt("DELETE FROM t WHERE k=?1", &p);
  for(i=0; i<nRows; i++){
    sqlite3_bind_int(p, 1, i);
    sqlite3_step(p);
    sqlite3_reset(p);
  }
  sqlite3_finalize(p);
  sqlite3_exec(db, "COMMIT", 0, 0, 0);
  return dsec()-t0;
}

#else /* btreelite side */

# define ENGINE "btreelite(kv)"
static btreelite_db *db = 0;
static unsigned iRoot;

static void setupDb(const char *zPath){
  unlink(zPath); unlink("bench.db-wal");
  if( btreelite_open(zPath, &db)!=BTREELITE_OK ){
    printf("open failed\n"); exit(2);
  }
  if( btreelite_begin(db, 1)!=BTREELITE_OK ) exit(2);
  btreelite_create_tree(db, &iRoot);
  btreelite_commit(db);
}
static void cleanupDb(void){ btreelite_close(db); }

static double benchInsert(void){
  btreelite_cur *cur = 0;
  char zKey[64], zVal[128];
  int i, b;
  double t0 = dsec();
  btreelite_begin(db, 1);
  btreelite_cursor_open(db, iRoot, 1, &cur);
  for(b=0; b<nBatch; b++){
    for(i=0; i<batch; i++){
      int n = b*batch + i;
      int nk = rowKey(n, zKey);
      int nv = sprintf(zVal, "value-%08d-payload", n);
      if( btreelite_put(cur, zKey, nk, zVal, nv)!=BTREELITE_OK ){
        printf("insert failed\n"); exit(2);
      }
    }
  }
  btreelite_cursor_close(cur);
  btreelite_commit(db);
  return dsec()-t0;
}

static double benchLookup(int nLook){
  btreelite_cur *cur = 0;
  char zKey[64];
  int i;
  double t0 = dsec();
  btreelite_begin(db, 0);
  btreelite_cursor_open(db, iRoot, 0, &cur);
  for(i=0; i<nLook; i++){
    int nk = rowKey(i % (nBatch*batch), zKey);
    if( btreelite_get(cur, zKey, nk)!=BTREELITE_OK ){
      printf("lookup failed\n"); exit(2);
    }
  }
  btreelite_cursor_close(cur);
  btreelite_commit(db);
  return dsec()-t0;
}

static double benchOverwrite(int nRows){
  btreelite_cur *cur = 0;
  char zKey[64], zVal[128];
  int i;
  double t0 = dsec();
  btreelite_begin(db, 1);
  btreelite_cursor_open(db, iRoot, 1, &cur);
  for(i=0; i<nRows; i++){
    int nk = rowKey(i, zKey);
    int nv = sprintf(zVal, "value-%08d-payload", i);
    if( btreelite_put(cur, zKey, nk, zVal, nv)!=BTREELITE_OK ){
      printf("overwrite failed\n"); exit(2);
    }
  }
  btreelite_cursor_close(cur);
  btreelite_commit(db);
  return dsec()-t0;
}

static double benchDelete(int nRows){
  btreelite_cur *cur = 0;
  char zKey[64];
  int i;
  double t0 = dsec();
  btreelite_begin(db, 1);
  btreelite_cursor_open(db, iRoot, 1, &cur);
  for(i=0; i<nRows; i++){
    int nk = rowKey(i, zKey);
    if( btreelite_del(cur, zKey, nk)!=BTREELITE_OK ){
      printf("delete failed\n"); exit(2);
    }
  }
  btreelite_cursor_close(cur);
  btreelite_commit(db);
  return dsec()-t0;
}

#endif

static void report(const char *zWhat, int nOps, double dt){
  printf("%-10s %-16s %8d ops  %7.2f s  %9.0f ops/s\n",
         ENGINE, zWhat, nOps, dt, nOps/dt);
}

int main(int argc, char **argv){
  int nRows, nLook;
  double dt;

  if( argc>=3 ){
    batch = atoi(argv[1]);
    nBatch = atoi(argv[2]);
  }
  nRows = batch*nBatch;
  nLook = nRows;
  unlink("bench.db");
  setupDb("bench.db");

  dt = benchInsert();
  report("insert", nRows, dt);
  dt = benchLookup(nLook);
  report("lookup", nLook, dt);
  dt = benchOverwrite(nRows);
  report("overwrite", nRows, dt);
  dt = benchDelete(nRows);
  report("delete", nRows, dt);

  cleanupDb();
  unlink("bench.db"); unlink("bench.db-wal"); unlink("bench.db-shm");
  return 0;
}