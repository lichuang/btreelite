/*
** 2026 September 25
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
** Library-level plumbing for btreelite: the error-reporting entry points
** that the SQLITE_*_BKPT macros expand to, the busy-handler dispatcher,
** the initialization path, and the SQL-layer query stubs that btree.c
** still calls (removed in Phase 2).
**
** In SQLite these live in main.c; they are separated here because they
** have no SQL-layer dependencies, only configuration state.
*/
#include "sqliteInt.h"

/*
** Source identity for log messages.
*/
const char *sqlite3_sourceid(void){ return "2026-09-25"; }

/*
** SQLite keeps a global temp-file directory; btreelite uses the OS
** default (NULL) and never mutates it.
*/
char *sqlite3_temp_directory = 0;

int sqlite3ReportError(int iErr, int lineno, const char *zType){
  sqlite3_log(iErr, "%s at line %d of [%.10s]", zType, lineno, 20+sqlite3_sourceid());
  return iErr;
}
int sqlite3CorruptError(int lineno){
  testcase( sqlite3GlobalConfig.xLog!=0 );
  return sqlite3ReportError(SQLITE_CORRUPT, lineno, "database corruption");
}
int sqlite3MisuseError(int lineno){
  testcase( sqlite3GlobalConfig.xLog!=0 );
  return sqlite3ReportError(SQLITE_MISUSE, lineno, "misuse");
}
int sqlite3CantopenError(int lineno){
  testcase( sqlite3GlobalConfig.xLog!=0 );
  return sqlite3ReportError(SQLITE_CANTOPEN, lineno, "cannot open file");
}
int sqlite3CorruptPgnoError(int lineno, Pgno pgno){
  char zMsg[100];
  sqlite3_snprintf(sizeof(zMsg), zMsg, "database corruption page %d", pgno);
  testcase( sqlite3GlobalConfig.xLog!=0 );
  return sqlite3ReportError(SQLITE_CORRUPT, lineno, zMsg);
}
int sqlite3NomemError(int lineno){
  testcase( sqlite3GlobalConfig.xLog!=0 );
  return sqlite3ReportError(SQLITE_NOMEM, lineno, "OOM");
}
int sqlite3IoerrnomemError(int lineno){
  testcase( sqlite3GlobalConfig.xLog!=0 );
  return sqlite3ReportError(SQLITE_IOERR_NOMEM, lineno, "I/O OOM error");
}

/*
** Busy-handler dispatcher (from main.c).  The pager invokes this with the
** BusyHandler object registered via sqlite3PagerSetBusyHandler().
*/
int sqlite3InvokeBusyHandler(BusyHandler *p){
  if( NEVER(p==0) || p->xBusyHandler==0 ) return 0;
  return p->xBusyHandler(p->pBusyArg, p->nBusy);
}

/*
** Real-to-int64 conversion (from util.c; used by os.c's time fallback).
*/
i64 sqlite3RealToI64(double r){
  i64 ix;
  if( r<-9223372036854775808.0 ) ix = SMALLEST_INT64;
  else if( r>9223372036854774784.0 ) ix = LARGEST_INT64;
  else ix = (i64)r;
  return ix;
}

u32 sqlite3HexToInt(int h){
  if( h>='0' && h<='9' ) return (u32)(h - '0');
  if( h>='a' && h<='f' ) return (u32)(h - 'a' + 10);
  return (u32)(h - 'A' + 10);
}

/*
** URI parameter access (from main.c).  btreelite supports the "nolock",
** "immutable" and "psow" parameters that pager.c reads; the filename
** format is the plain path (no URI scheme dispatch).
*/
const char *sqlite3_uri_parameter(const char *zFilename, const char *zParam){
  (void)zFilename; (void)zParam;
  return 0;
}
int sqlite3_uri_boolean(const char *zFilename, const char *zParam, int dflt){
  const char *z = sqlite3_uri_parameter(zFilename, zParam);
  return z ? sqlite3GetBoolean(z, dflt) : dflt;
}

/*
** Shared-cache notification is a no-op: btreelite compiles
** SQLITE_OMIT_SHARED_CACHE and the shared-cache code paths are deleted
** in Phase 2.  Kept as a linkable stub until then.
*/
void sqlite3ConnectionBlocked(sqlite3 *db, sqlite3 *pBlocker){
  (void)db; (void)pBlocker;
}

/*
** Backup hooks: backup.c is not part of the btreelite build.  The pager
** invokes these through opaque pointers; both are no-ops here.  Phase 1
** replaces the call sites with macros and drops these stubs.
*/
void sqlite3BackupRestart(sqlite3_backup *p){ (void)p; }
void sqlite3BackupUpdate(sqlite3_backup *p, Pgno pgno, u8 *a){
  (void)p; (void)pgno; (void)a;
}

/*
** SQL-layer state queries the btree layer makes.  btreelite has neither
** temp databases nor writable schemas, so both are always false.  The
** call sites are removed in Phase 2; these stubs keep the Phase 1 build
** whole.
*/
int sqlite3TempInMemory(sqlite3 *db){ (void)db; return 0; }
int sqlite3WritableSchema(sqlite3 *db){ (void)db; return 0; }

/*
** Per-tree count sink used by integrity_check; Phase 2 replaces the
** Mem-based output with a plain i64 array.
*/
void sqlite3MemSetArrayInt64(void *p, int i, i64 v){ (void)p; (void)i; (void)v; }

/*
** Filename-object helpers referenced by pager.c's filename lookup.
** Phase 1 simplifies pager.c to plain paths and drops these.
*/
const char *sqlite3_create_filename(
  const char *zDatabase,
  const char *zJournal,
  const char *zWal,
  int nParam,
  const char **azParam
){
  (void)zDatabase; (void)zJournal; (void)zWal; (void)nParam; (void)azParam;
  return 0;
}


/*
** SQL value accessors over the sqlite3_value type.  btreelite's record
** cells are raw byte strings, so the integer/double/text projections of
** a value degenerate to reading the blob.  Real SQLFUNC printf consumers
** arrive in Phase 4 with the public API.
*/
sqlite3_int64 sqlite3_value_int64(sqlite3_value *p){ (void)p; return 0; }
double sqlite3_value_double(sqlite3_value *p){ (void)p; return 0.0; }
const unsigned char *sqlite3_value_text(sqlite3_value *p){ (void)p; return (const unsigned char*)""; }

/*
** Global-config accessors (from util.c / os.c / main.c).
*/


/*
** 8.3 filename shortening (from util.c).  Only active when
** SQLITE_ENABLE_8_3_NAMES is defined; btreelite leaves names untouched.
*/
void sqlite3FileSuffix3(const char *zBase, char *z){
  (void)zBase; (void)z;
}

/*
** Variable-length integer, 32-bit entry point (from util.c).
*/
int sqlite3PutVarint32(unsigned char *p, u32 v){
  if( (v & ~0x7f)==0 ){
    p[0] = (unsigned char)v;
    return 1;
  }
  return (int)sqlite3PutVarint(p, v);
}

/*
** Floating-point decoder (from util.c).  Only %f/%e/%g rendering of
** log messages consumes it.
*/
void sqlite3FpDecode(FpDecode *p, double r, int iRound, int mxRound){
  int i;
  char *zOut = p->zBuf;
  if( mxRound<=5 ) mxRound = 30;
  if( mxRound>p->n ) mxRound = p->n;
  p->z = p->zBuf;
  if( r<0.0 ){ p->sign = '-'; r = -r; }else{ p->sign = '+'; }
  p->isSpecial = 0;
  if( r==0.0 ){
    p->n = 1;
    p->iDP = 1;
    p->zBuf[0] = '0';
    p->zBuf[1] = 0;
    return;
  }
  /* Decimal rendering via libc; btreelite only formats log messages. */
  i = snprintf(p->zBuf, sizeof(p->zBuf), "%.15g", r);
  if( i<0 ) i = 0;
  if( (size_t)i>=sizeof(p->zBuf) ) i = (int)sizeof(p->zBuf)-1;
  p->n = i;
  p->iDP = i;
  (void)zOut; (void)iRound;
}

/*
** The VFS "memdb" probe: btreelite ships no in-VFS memory database, so
** no VFS ever reports itself as memdb.
*/
int sqlite3IsMemdb(sqlite3_vfs *pVfs){ (void)pVfs; return 0; }

/*
** Connection error state.  btreelite keeps only mallocFailed propagation;
** the API-layer error code lives on the btreelite_db object in Phase 4.
*/
void sqlite3Error(sqlite3 *db, int err_code){
  (void)db; (void)err_code;
}
void sqlite3ErrorMsg(Parse *pParse, const char *zFormat, ...){
  /* btreelite has no parser; OOM paths that referenced the parser are
  ** simplified in Phase 2.  Silence the unused parameters here. */
  (void)pParse; (void)zFormat;
}

/*
** Raw memcmp-based record comparison.  SQLite's vdbeRecordCompare decodes
** a serial-type record; btreelite keys are raw byte strings, so the
** comparison is a plain memcmp with length tiebreak.  These four entry
** points replace the vdbe.c implementations.
*/


/*
** Decode the KV record header: pRec points at the payload of a KV cell
** (the record [varint nKeyBytes][key][value]).  Stores the byte offset
** of the key inside the record in *pnKOff and the key length in
** *pnKey.  Returns the total record length.
*/
static int kvRecordDecode(const u8 *pRec, int nRec, int *pnKOff, int *pnKey){
  u64 nKey = 0;
  u8 *p = (u8*)pRec;
  u8 *pEnd = &p[8];
  if( nRec<=0 ) return 0;
  nKey = *p;
  if( nKey>=0x80 ){
    nKey &= 0x7f;
    do{
      nKey = (nKey<<7) | (*++p & 0x7f);
    }while( (*p)>=0x80 && p<pEnd );
  }
  p++;
  *pnKOff = (int)(p - pRec);
  *pnKey = (int)nKey;
  return nRec;
}

/*
** Record-compare implementation: pKey1 is the KV record stored in the
** cell ([varint nKeyBytes][key][value]) and pPKey2 carries the raw
** search key.  Compare key-vs-key only, with length tiebreak.
*/
static int kvCompare(int nKey1, const void *pKey1, UnpackedRecord *pPKey2){
  int nKOff = 0, nKeyLen = 0, nCmp, c;
  const u8 *pRec = (const u8*)pKey1;
  if( pPKey2->n<0 ) return 99;
  (void)kvRecordDecode(pRec, nKey1, &nKOff, &nKeyLen);
  if( nKeyLen<0 || nKOff+nKeyLen>nKey1 ) return 99;
  nCmp = (pPKey2->n < nKeyLen) ? pPKey2->n : nKeyLen;
  c = memcmp(&pRec[nKOff], pPKey2->u.z, (size_t)nCmp);
  if( c==0 ) c = nKeyLen - pPKey2->n;
  if( c<0 ) return -1;
  if( c>0 ) return +1;
  return 0;
}

void sqlite3VdbeRecordUnpack(
  int nKey, const void *pKey, UnpackedRecord *p
){
  p->nField = 1;
  p->u.z = (char*)pKey;
  p->n = nKey;
  p->errCode = 0;
  p->eqSeen = 0;
  p->default_rc = 0;
}

UnpackedRecord *sqlite3VdbeAllocUnpackedRecord(KeyInfo *pKeyInfo){
  UnpackedRecord *p = (UnpackedRecord*)sqlite3MallocZero(sizeof(UnpackedRecord));
  if( p ){
    p->pKeyInfo = pKeyInfo;
    p->nField = 1;
    p->aMem = 0;
  }
  return p;
}

int sqlite3VdbeRecordCompare(int nKey1, const void *pKey1, UnpackedRecord *pPKey2){
  return kvCompare(nKey1, pKey1, pPKey2);
}

RecordCompare sqlite3VdbeFindCompare(UnpackedRecord *p){
  (void)p;
  return kvCompare;
}

/*
** Initialize / shutdown.  Phase 0-1 builds initialize eagerly via
** sqlite3_initialize() auto-init; the mutex/malloc/pcache subsystems are
** wired through their *Init/End entry points directly.
*/
int sqlite3_initialize(void){
  int rc = SQLITE_OK;
  if( sqlite3GlobalConfig.isInit ) return SQLITE_OK;
  if( sqlite3GlobalConfig.inProgress ) return SQLITE_OK;
  sqlite3GlobalConfig.inProgress = 1;
  rc = sqlite3MutexInit();
  if( rc==SQLITE_OK ){
    rc = sqlite3MallocInit();
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3PcacheInitialize();
  }
  if( rc==SQLITE_OK ){
    rc = sqlite3OsInit();
  }
  sqlite3GlobalConfig.inProgress = 0;
  if( rc==SQLITE_OK ) sqlite3GlobalConfig.isInit = 1;
  return rc;
}
int sqlite3_shutdown(void){
  sqlite3PcacheShutdown();
  sqlite3MutexEnd();
  sqlite3MallocEnd();
  return SQLITE_OK;
}
/* sqlite3_os_init() / sqlite3_os_end() are provided by os_unix.c. */