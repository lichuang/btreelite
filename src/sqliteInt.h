/*
** 2026 September 24
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
** Phase-0 compatibility shim.  The extracted storage sources include
** "sqliteInt.h" verbatim; this file keeps them compiling without edits
** by forwarding to btreeliteInt.h and adding the declarations the
** storage subsystem spells in the SQLite way.  Deleted in Phase 4.
*/
#ifndef SQLITEINT_H
#define SQLITEINT_H

#include "btreeliteInt.h"
#include <ctype.h>

/*
** Opaque forward types referenced through pointers by the storage sources.
*/
typedef struct Vdbe Vdbe;
typedef struct Hash Hash;
typedef struct HashElem HashElem;
typedef struct Module Module;
typedef struct sqlite3_backup sqlite3_backup;
typedef struct PrintfArguments PrintfArguments;
struct PrintfArguments {
  int nArg;                /* Total number of arguments */
  int nUsed;               /* Number of arguments used so far */
  sqlite3_value **apArg;   /* The argument values */
};

/* SQL value accessors used by the SQLFUNC printf path; the API layer
** (Phase 4) supplies implementations over its own value type. */
sqlite3_int64 sqlite3_value_int64(sqlite3_value*);
double sqlite3_value_double(sqlite3_value*);
const unsigned char *sqlite3_value_text(sqlite3_value*);
typedef struct StrAccum sqlite3_str;
#define SQLITE_PRINTF_INTERNAL 0x01
#define SQLITE_PRINTF_SQLFUNC  0x02
#define SQLITE_PRINTF_MALLOCED 0x04
#define isMalloced(X)  (((X)->printfFlags & SQLITE_PRINTF_MALLOCED)!=0)

/*
** Lookaside is a connection-level allocation cache.  The storage sources
** only ever probe db->lookaside.sz for size filtering; the full struct
** mirrors the subset of sqliteInt.h fields those probes touch.
*/
/*
** sqlite3.h.in API surface the storage sources call directly.
*/
void *sqlite3_malloc(int);
void *sqlite3_malloc64(sqlite3_uint64);
void *sqlite3_realloc(void*, int);
void *sqlite3_realloc64(void*, sqlite3_uint64);
void sqlite3_free(void*);
sqlite3_int64 sqlite3_memory_used(void);
sqlite3_int64 sqlite3_memory_highwater(int);
sqlite3_int64 sqlite3_soft_heap_limit64(sqlite3_int64);
int sqlite3_status64(int, sqlite3_int64*, sqlite3_int64*, int);
sqlite3_mutex *sqlite3_mutex_alloc(int);
void sqlite3_mutex_free(sqlite3_mutex*);
void sqlite3_mutex_enter(sqlite3_mutex*);
int sqlite3_mutex_try(sqlite3_mutex*);
void sqlite3_mutex_leave(sqlite3_mutex*);
int sqlite3_mutex_held(sqlite3_mutex*);
int sqlite3_mutex_notheld(sqlite3_mutex*);
int sqlite3_release_memory(int);
int sqlite3_db_readonly(sqlite3*, const char*);

/*
** util.c string/accumulator APIs.
*/
void sqlite3StrAccumInit(StrAccum*, sqlite3*, char*, int, int);
char *sqlite3StrAccumFinish(StrAccum*);
void sqlite3_str_append(sqlite3_str*, const char*, int);
void sqlite3_str_appendf(sqlite3_str*, const char*, ...);
void sqlite3_str_vappendf(sqlite3_str*, const char*, va_list);
void sqlite3_str_reset(sqlite3_str*);
void sqlite3_log(int, const char*, ...);
int sqlite3FaultSim(int);

/*
** util.c numeric/byte helpers.
*/
u32 sqlite3Get4byte(const u8*);
void sqlite3Put4byte(u8*, u32);
u8 sqlite3GetVarint(const unsigned char*, u64*);
int sqlite3PutVarint(unsigned char*, u64);
u8 sqlite3GetVarint32(const unsigned char*, u32*);
int sqlite3PutVarint32(unsigned char*, u32);
int sqlite3GetInt32(const char*, int*);
int sqlite3VarintLen(u64);
int sqlite3Utf8ByteLen(const char*, int);
int sqlite3AbsInt32(int);
void sqlite3Put2byte(u8*, u16);

/*
** Allocation type tags (from sqliteInt.h; malloc.c tracks allocation
** kinds in SQLITE_DEBUG builds).
*/
void sqlite3MemdebugSetType(void*,u8);
int sqlite3MemdebugHasType(const void*,u8);
int sqlite3MemdebugNoType(const void*,u8);
#ifdef SQLITE_DEBUG
#define MEMTYPE_HEAP       0x01
#define MEMTYPE_LOOKASIDE  0x02
#define MEMTYPE_PCACHE     0x04
#else
#define MEMTYPE_HEAP       0x01
#define MEMTYPE_LOOKASIDE  0x02
#define MEMTYPE_PCACHE     0x04
# undef sqlite3MemdebugSetType
# undef sqlite3MemdebugHasType
# undef sqlite3MemdebugNoType
# define sqlite3MemdebugSetType(X,Y)
# define sqlite3MemdebugHasType(X,Y)  1
# define sqlite3MemdebugNoType(X,Y)   1
#endif

#define LOOKASIDE_SMALL 128
#define DisableLookaside  db->lookaside.bDisable++;db->lookaside.sz=0
#define EnableLookaside   db->lookaside.bDisable++;\
   db->lookaside.sz=db->lookaside.bDisable?0:db->lookaside.szTrue

/*
** main.c-level helpers the btree/pager sources call.  Phase 0 stubs
** these in btreelite.c; Phase 2 removes the call sites along with the
** features they serve.
*/
int sqlite3TempInMemory(sqlite3*);
int sqlite3WritableSchema(sqlite3*);
int sqlite3InvokeBusyHandler(BusyHandler*);
void sqlite3ConnectionBlocked(sqlite3*, sqlite3*);
void sqlite3BackupRestart(sqlite3_backup*);
void sqlite3BackupUpdate(sqlite3_backup*, Pgno, u8*);
sqlite3_file *sqlite3_database_file_object(const char*);
const char *sqlite3_create_filename(const char*, const char*, const char*,
                                    int, const char**);
int sqlite3_initialize(void);
int sqlite3_shutdown(void);
int sqlite3_os_init(void);
int sqlite3_os_end(void);
void sqlite3MemSetArrayInt64(void*, int, i64);

/*
** Error-reporting entry points (util.c) that the SQLITE_*_BKPT macros
** expand to; they route through sqlite3_log().
*/
int sqlite3ReportError(int iErr, int lineno, const char *zType);
int sqlite3CorruptError(int);
int sqlite3MisuseError(int);
int sqlite3CantopenError(int);
int sqlite3NomemError(int);
int sqlite3IoerrnomemError(int);
int sqlite3CorruptPgnoError(int,Pgno);
#define SQLITE_CORRUPT_BKPT sqlite3CorruptError(__LINE__)
#define SQLITE_MISUSE_BKPT sqlite3MisuseError(__LINE__)
#define SQLITE_CANTOPEN_BKPT sqlite3CantopenError(__LINE__)
#define SQLITE_NOMEM_BKPT sqlite3NomemError(__LINE__)
#define SQLITE_IOERR_NOMEM_BKPT sqlite3IoerrnomemError(__LINE__)
#define SQLITE_CORRUPT_PGNO(P) sqlite3CorruptPgnoError(__LINE__,(P))
void sqlite3Error(sqlite3*,int);
void sqlite3ErrorMsg(Parse*, const char*, ...);
void *sqlite3OomFault(sqlite3*);
void sqlite3OomClear(sqlite3*);
int sqlite3ApiExit(sqlite3*, int);

/* ASCII ctype macros (from sqliteInt.h) */
#define IsNaN(X)             0
#define IsOvfl(X)            0
#define UpperToLower         sqlite3UpperToLower
u32 sqlite3HexToInt(int h);

extern const unsigned char sqlite3UpperToLower[];
#define sqlite3Isspace(x)   (isspace((unsigned char)(x)))
#define sqlite3Isdigit(x)   (isdigit((unsigned char)(x)))
#define sqlite3Isxdigit(x)  (isxdigit((unsigned char)(x)))
#define sqlite3Isalpha(x)   (isalpha((unsigned char)(x)))
#define sqlite3Isalnum(x)   (isalnum((unsigned char)(x)))


/* Status counters (from sqlite.h.in; used by status.c) */
#define SQLITE_STATUS_PARSER_STACK       6
#define SQLITE_DBSTATUS_LOOKASIDE_USED      0
#define SQLITE_DBSTATUS_CACHE_USED          1
#define SQLITE_DBSTATUS_SCHEMA_USED         2
#define SQLITE_DBSTATUS_STMT_USED           3
#define SQLITE_DBSTATUS_LOOKASIDE_HIT       4
#define SQLITE_DBSTATUS_LOOKASIDE_MISS_SIZE 5
#define SQLITE_DBSTATUS_LOOKASIDE_MISS_FULL 6
#define SQLITE_DBSTATUS_CACHE_HIT           7
#define SQLITE_DBSTATUS_CACHE_MISS          8
#define SQLITE_DBSTATUS_CACHE_WRITE         9
#define SQLITE_DBSTATUS_DEFERRED_FKS        10
#define SQLITE_DBSTATUS_CACHE_USED_SHARED   11
#define SQLITE_DBSTATUS_CACHE_SPILL         12
#define SQLITE_DBSTATUS_TEMPBUF_SPILL       13
#define SQLITE_DBSTATUS_MAX                 13


/* btree.h / pager.h interfaces used by the storage sources; the real
** prototypes come from btree.h / pager.h, included by btreeliteInt.h. */


/* deliberate_fall_through (from sqliteInt.h) */
#if defined(__has_attribute)
#  if __has_attribute(fallthrough)
#    define deliberate_fall_through __attribute__((fallthrough));
#  endif
#endif
#if !defined(deliberate_fall_through)
#  define deliberate_fall_through
#endif

/* pager API surface used by status.c (declarations also in pager.h) */
int sqlite3PagerMemUsed(Pager*);
void sqlite3PagerCacheStat(Pager*, int, int, u64*);


/* os.h interface (extracted verbatim from os.h function declarations) */
void sqlite3OsClose(sqlite3_file*);
int sqlite3OsRead(sqlite3_file*, void*, int amt, i64 offset);
int sqlite3OsWrite(sqlite3_file*, const void*, int amt, i64 offset);
int sqlite3OsTruncate(sqlite3_file*, i64 size);
int sqlite3OsSync(sqlite3_file*, int);
int sqlite3OsFileSize(sqlite3_file*, i64 *pSize);
int sqlite3OsLock(sqlite3_file*, int);
int sqlite3OsUnlock(sqlite3_file*, int);
int sqlite3OsCheckReservedLock(sqlite3_file *id, int *pResOut);
int sqlite3OsFileControl(sqlite3_file*,int,void*);
void sqlite3OsFileControlHint(sqlite3_file*,int,void*);
#define SQLITE_FCNTL_DB_UNCHANGED 0xca093fa0
int sqlite3OsSectorSize(sqlite3_file *id);
int sqlite3OsDeviceCharacteristics(sqlite3_file *id);
int sqlite3OsShmMap(sqlite3_file *,int,int,int,void volatile **);
int sqlite3OsShmLock(sqlite3_file *id, int, int, int);
void sqlite3OsShmBarrier(sqlite3_file *id);
int sqlite3OsShmUnmap(sqlite3_file *id, int);
int sqlite3OsFetch(sqlite3_file *id, i64, int, void **);
int sqlite3OsUnfetch(sqlite3_file *, i64, void *);
int sqlite3OsOpen(sqlite3_vfs *, const char *, sqlite3_file*, int, int *);
int sqlite3OsDelete(sqlite3_vfs *, const char *, int);
int sqlite3OsAccess(sqlite3_vfs *, const char *, int, int *pResOut);
int sqlite3OsFullPathname(sqlite3_vfs *, const char *, int, char *);
int sqlite3OsRandomness(sqlite3_vfs *, int, char *);
int sqlite3OsSleep(sqlite3_vfs *, int);
int sqlite3OsGetLastError(sqlite3_vfs*);
int sqlite3OsCurrentTimeInt64(sqlite3_vfs *, sqlite3_int64*);
int sqlite3OsOpenMalloc(sqlite3_vfs *, const char *, sqlite3_file **, int,int*);
void sqlite3OsCloseFree(sqlite3_file *);
int sqlite3OsInit(void);


/* printf.c internals */
int sqlite3StrAccumEnlarge(StrAccum*, i64);
int sqlite3StrAccumEnlargeIfNeeded(StrAccum*, i64);
void sqlite3_str_appendchar(sqlite3_str*, int, char);
int sqlite3AppendOneUtf8Character(char*, u32);
u32 sqlite3Utf8Read(const unsigned char**);
#define SQLITE_SKIP_UTF8(zIn) {                        \
  if( (*(zIn++))>=0xc0 ){                              \
    while( (*zIn & 0xc0)==0x80 ){ zIn++; }             \
  }                                                    \
}

/* Floating-point decode (util.c subset) */
struct FpDecode {
  int n;
  int iDP;
  char *z;
  char zBuf[20+1];
  char sign;
  char isSpecial;
};
typedef struct FpDecode FpDecode;
void sqlite3FpDecode(FpDecode*,double,int,int);

/* Stack allocation macros (from sqliteInt.h) */
#ifdef SQLITE_USE_ALLOCA
# define sqlite3StackAllocRaw(D,N)   alloca(N)
# define sqlite3StackAllocRawNN(D,N) alloca(N)
# define sqlite3StackFree(D,P)
# define sqlite3StackFreeNN(D,P)
#else
# define sqlite3StackAllocRaw(D,N)   sqlite3DbMallocRaw(D,N)
# define sqlite3StackAllocRawNN(D,N) sqlite3DbMallocRawNN(D,N)
# define sqlite3StackFree(D,P)       sqlite3DbFree(D,P)
# define sqlite3StackFreeNN(D,P)     sqlite3DbFreeNN(D,P)
#endif


/* mutex.h MUTEX_LOGIC + os.c helpers */
#ifndef MUTEX_LOGIC
# if SQLITE_THREADSAFE
#   define MUTEX_LOGIC(X)            X
# else
#   define MUTEX_LOGIC(X)
# endif
#endif
i64 sqlite3RealToI64(double);

#define SQLITE_DEFAULT_SECTOR_SIZE 4096


/* SHM lock flags (from sqlite.h.in) */
#define SQLITE_SHM_UNLOCK      1
#define SQLITE_SHM_LOCK        2
#define SQLITE_SHM_SHARED      4
#define SQLITE_SHM_EXCLUSIVE   8
#define SQLITE_SHM_NLOCK       8

/* File-lock levels and IOTRACE (from os.h / os_common.h) */
#define NO_LOCK         0
#define SHARED_LOCK     1
#define RESERVED_LOCK   2
#define PENDING_LOCK    3
#define EXCLUSIVE_LOCK  4
#define UNKNOWN_LOCK    (EXCLUSIVE_LOCK+1)

/* IOTRACE / PAGERTRACE / WALTRACE macros (no-op unless tracing enabled) */
#define IOTRACE(X)
#define OSTRACE(X)
#define OS_VXWORKS 0
#define SQLITE_FCNTL_NULL_IO 45
#define SQLITE_FCNTL_EXTERNAL_READER 46
#define SQLITE_FCNTL_GET_LOCKPROXYFILE_UNUSED 0
char *sqlite3_mprintf(const char*, ...);
void sqlite3FileSuffix3(const char*, char*);
extern char *sqlite3_temp_directory;
#define SQLITE_MUTEX_STATIC_TEMPDIR SQLITE_MUTEX_STATIC_VFS1
#define SQLITE_OMIT_TRACE 1

/* bitvec.c API */
typedef struct Bitvec Bitvec;
int sqlite3BitvecTestNotNull(Bitvec*, u32);
int sqlite3BitvecSet(Bitvec*, u32);
void sqlite3BitvecClear(Bitvec*, u32, void*);
void sqlite3BitvecDestroy(Bitvec*);
u32 sqlite3BitvecSize(Bitvec*);
Bitvec *sqlite3BitvecCreate(u32);
int sqlite3BitvecTest(Bitvec*, u32);
void sqlite3BitvecClearAll(Bitvec*);


/* memjournal.c API + access flags */
int sqlite3JournalOpen(sqlite3_vfs*, const char*, sqlite3_file*, int, int);
int sqlite3JournalSize(sqlite3_vfs*);
int sqlite3JournalIsInMemory(sqlite3_file*);
int sqlite3JournalCreate(sqlite3_file*);
#define SQLITE_ACCESS_EXISTS     0
#define SQLITE_ACCESS_READWRITE  1
#define SQLITE_ACCESS_READ       2

#define SQLITE_VERSION_NUMBER 3054000
#define PENDING_BYTE      sqlite3PendingByte
#define RESERVED_BYTE     (PENDING_BYTE+1)
#define SHARED_FIRST      (PENDING_BYTE+2)
#define SHARED_SIZE       510
extern int sqlite3PendingByte;

void sqlite3MemJournalOpen(sqlite3_file*);
int sqlite3IsMemdb(sqlite3_vfs*);

/* URI parameter access (main.c; pager.c reads "nolock"/"immutable" flags) */
int sqlite3_uri_boolean(const char*, const char*, int);
const char *sqlite3_uri_parameter(const char*, const char*);

/* Savepoint ops (from sqliteInt.h) */
#define SAVEPOINT_BEGIN      0
#define SAVEPOINT_RELEASE    1
#define SAVEPOINT_ROLLBACK   2


/* Varint macros (from sqliteInt.h) */
#define getVarint32(A,B)  \
  (u8)((*(A)<(u8)0x80)?((B)=(u32)*(A)),1:sqlite3GetVarint32((A),(u32 *)&(B)))
#define getVarint32NR(A,B) \
  B=(u32)*(A);if(B>=0x80)sqlite3GetVarint32((A),(u32*)&(B))
#define putVarint32(A,B)  \
  (u8)(((u32)(B)<(u32)0x80)?(*(A)=(unsigned char)(B)),1:\
  sqlite3PutVarint((A),(B)))
#define getVarint    sqlite3GetVarint
#define putVarint    sqlite3PutVarint
#define get2byteNotZero(X)  (((((int)get2byte(X))-1)&0xffff)+1)

char *sqlite3_snprintf(int, char*, const char*, ...);
int sqlite3GetBoolean(const char *z, int dflt);

int sqlite3BtreeIndexMoveto(BtCursor*, UnpackedRecord*, int*);
int sqlite3KvEncode(const void*, int, const void*, int, void**);

/* VDBE record-compare entry points (vdbe.c in SQLite; memcmp-based
** implementations ship in the btreelite API layer). */
void sqlite3VdbeRecordUnpack(int,const void*,UnpackedRecord*);
int sqlite3VdbeRecordCompare(int,const void*,UnpackedRecord*);
UnpackedRecord *sqlite3VdbeAllocUnpackedRecord(KeyInfo*);
RecordCompare sqlite3VdbeFindCompare(UnpackedRecord*);

/* Connection-level knobs referenced by btree.c */
#define SQLITE_CellSizeCk      0x00200000
#define SQLITE_ReadUncommit    HI(0x00004)
#define SQLITE_ResetDatabase   0x02000000
#define SQLITE_NoCkptOnClose  0x00000800
#define HI(X) ((u64)(X)<<32)

#endif /* SQLITE_INT_H */