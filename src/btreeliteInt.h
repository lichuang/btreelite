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
** This is the internal header for the btreelite storage engine.  It plays
** the role that "sqliteInt.h" plays inside SQLite: every .c file includes
** it.  It defines only what the storage subsystem (btree.c, pager.c,
** wal.c, pcache*.c, os*.c, mutex*.c, malloc.c, util.c, ...) actually
** needs — the SQL-layer parts of sqliteInt.h are not here.
**
** The global configuration object is btreeliteConfig.  Its name differs
** from SQLite's sqlite3Config only; its layout mirrors the subset of
** struct Sqlite3Config used by the storage subsystem.
*/
#ifndef BTREELITEINT_H
#define BTREELITEINT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>

/*
** sqlite3.h.in base type spellings.  These must come first: the storage
** sources use sqlite3_int64 / sqlite_int64 / i64 / u64 interchangeably.
*/
typedef int64_t sqlite3_int64;
typedef uint64_t sqlite3_uint64;
typedef sqlite3_int64 sqlite_int64;
typedef sqlite3_uint64 sqlite_uint64;

/*
** Compile-time option set for a SQL-free storage build.
*/
#define SQLITE_OMIT_AUTOVACUUM       1
#define SQLITE_OMIT_SHARED_CACHE     1
#define SQLITE_OMIT_LOAD_EXTENSION   1
#define SQLITE_OMIT_DEPRECATED       1
#define SQLITE_OMIT_DESERIALIZE      1
#define SQLITE_OMIT_TRIGGER          1
#define SQLITE_OMIT_VIEW             1
#define SQLITE_OMIT_VIRTUALTABLE     1
#define SQLITE_OMIT_AUTHORIZATION    1
#define SQLITE_OMIT_PROGRESS_CALLBACK 1
#define SQLITE_OMIT_DEPRECATED       1
/* WAL is a feature, keep it.  wal.h and pager.h key off "#ifdef
** SQLITE_OMIT_WAL", so the macro must not be defined at all. */
#if defined(SQLITE_OMIT_WAL)
# undef SQLITE_OMIT_WAL
#endif
#define SQLITE_OMIT_DATETIME_FUNCS   1
#define SQLITE_OMIT_TRACE            1
#define SQLITE_OMIT_GET_TABLE        1
#define SQLITE_OMIT_DEPRECATED       1
/* Fault-simulation machinery is kept (fault.c); the macro must not be
** defined, because fault.c / util.c key off "#ifdef SQLITE_UNTESTABLE". */
#if defined(SQLITE_UNTESTABLE)
# undef SQLITE_UNTESTABLE
#endif
#define SQLITE_OMIT_UTF16            1
#define SQLITE_OMIT_COMPLETE         1
#define SQLITE_OMIT_LEGACY           1

/*
** Derived from sqlite3.h.in — the subset of public constants the storage
** subsystem references.  Kept as the SQLITE_* spellings so the extracted
** sources compile unmodified.
*/
#define SQLITE_OK           0
#define SQLITE_ERROR        1
#define SQLITE_INTERNAL     2
#define SQLITE_PERM         3
#define SQLITE_ABORT        4
#define SQLITE_BUSY         5
#define SQLITE_LOCKED       6
#define SQLITE_NOMEM        7
#define SQLITE_READONLY     8
#define SQLITE_INTERRUPT    9
#define SQLITE_IOERR       10
#define SQLITE_CORRUPT     11
#define SQLITE_NOTFOUND    12
#define SQLITE_FULL        13
#define SQLITE_CANTOPEN    14
#define SQLITE_PROTOCOL    15
#define SQLITE_EMPTY       16
#define SQLITE_SCHEMA      17
#define SQLITE_TOOBIG      18
#define SQLITE_CONSTRAINT  19
#define SQLITE_MISMATCH    20
#define SQLITE_MISUSE      21
#define SQLITE_NOLFS       22
#define SQLITE_AUTH        23
#define SQLITE_FORMAT      24
#define SQLITE_RANGE       25
#define SQLITE_NOTADB      26
#define SQLITE_NOTICE      27
#define SQLITE_WARNING     28
#define SQLITE_ROW         100
#define SQLITE_DONE        101

#define SQLITE_LOCKED_SHAREDCACHE    (SQLITE_LOCKED |  (1<<8))
#define SQLITE_BUSY_SNAPSHOT         (SQLITE_BUSY |     (2<<8))
#define SQLITE_BUSY_RECOVERY         (SQLITE_BUSY |     (1<<8))
#define SQLITE_BUSY_TIMEOUT          (SQLITE_BUSY |     (3<<8))
#define SQLITE_IOERR_READ            (SQLITE_IOERR | (1<<8))
#define SQLITE_IOERR_SHORT_READ      (SQLITE_IOERR | (2<<8))
#define SQLITE_IOERR_WRITE           (SQLITE_IOERR | (3<<8))
#define SQLITE_IOERR_FSYNC           (SQLITE_IOERR | (4<<8))
#define SQLITE_IOERR_DIR_FSYNC       (SQLITE_IOERR | (5<<8))
#define SQLITE_IOERR_TRUNCATE        (SQLITE_IOERR | (6<<8))
#define SQLITE_IOERR_FSTAT           (SQLITE_IOERR | (7<<8))
#define SQLITE_IOERR_UNLOCK          (SQLITE_IOERR | (8<<8))
#define SQLITE_IOERR_RDLOCK          (SQLITE_IOERR | (9<<8))
#define SQLITE_IOERR_DELETE          (SQLITE_IOERR | (10<<8))
#define SQLITE_IOERR_BLOCKED         (SQLITE_IOERR | (11<<8))
#define SQLITE_IOERR_NOMEM           (SQLITE_IOERR | (12<<8))
#define SQLITE_IOERR_ACCESS          (SQLITE_IOERR | (13<<8))
#define SQLITE_IOERR_CHECKRESERVEDLOCK (SQLITE_IOERR | (14<<8))
#define SQLITE_IOERR_LOCK            (SQLITE_IOERR | (15<<8))
#define SQLITE_IOERR_CLOSE           (SQLITE_IOERR | (16<<8))
#define SQLITE_IOERR_DIR_CLOSE       (SQLITE_IOERR | (17<<8))
#define SQLITE_IOERR_SHMOPEN         (SQLITE_IOERR | (18<<8))
#define SQLITE_IOERR_SHMSIZE         (SQLITE_IOERR | (19<<8))
#define SQLITE_IOERR_SHMLOCK         (SQLITE_IOERR | (20<<8))
#define SQLITE_IOERR_SHMMAP          (SQLITE_IOERR | (21<<8))
#define SQLITE_IOERR_SEEK            (SQLITE_IOERR | (22<<8))
#define SQLITE_IOERR_DELETE_NOENT    (SQLITE_IOERR | (23<<8))
#define SQLITE_IOERR_MMAP            (SQLITE_IOERR | (24<<8))
#define SQLITE_IOERR_GETTEMPPATH     (SQLITE_IOERR | (25<<8))
#define SQLITE_IOERR_CONVPATH        (SQLITE_IOERR | (26<<8))
#define SQLITE_IOERR_VNODE           (SQLITE_IOERR | (27<<8))
#define SQLITE_IOERR_AUTH            (SQLITE_IOERR | (28<<8))
#define SQLITE_IOERR_BEGIN_ATOMIC    (SQLITE_IOERR | (29<<8))
#define SQLITE_IOERR_COMMIT_ATOMIC   (SQLITE_IOERR | (30<<8))
#define SQLITE_IOERR_ROLLBACK_ATOMIC (SQLITE_IOERR | (31<<8))
#define SQLITE_IOERR_DATA            (SQLITE_IOERR | (32<<8))
#define SQLITE_IOERR_CORRUPTFS       (SQLITE_IOERR | (33<<8))
#define SQLITE_IOERR_IN_PAGE         (SQLITE_IOERR | (34<<8))
#define SQLITE_READONLY_ROLLBACK     (SQLITE_READONLY | (6<<8))
#define SQLITE_READONLY_CANTINIT     (SQLITE_READONLY | (7<<8))
#define SQLITE_READONLY_RECOVERY     (SQLITE_READONLY | (8<<8))
#define SQLITE_READONLY_DBMOVED      (SQLITE_READONLY | (10<<8))
#define SQLITE_READONLY_CANTWRITE    (SQLITE_READONLY | (11<<8))
#define SQLITE_READONLY_DIRECTORY    (SQLITE_READONLY | (13<<8))
#define SQLITE_ABORT_ROLLBACK        (SQLITE_ABORT | (2<<8))
#define SQLITE_CONSTRAINT_PINNED     (SQLITE_CONSTRAINT | (3<<8))
#define SQLITE_OK_SYMLINK            (SQLITE_OK | (12<<8))
#define SQLITE_CANTOPEN_SYMLINK      (SQLITE_CANTOPEN | (3<<8))
#define SQLITE_CANTOPEN_DIRTYWAL     (SQLITE_CANTOPEN | (5<<8))
#define SQLITE_NOTICE_RECOVER_WAL      (SQLITE_NOTICE | (3<<8))
#define SQLITE_NOTICE_RECOVER_ROLLBACK (SQLITE_NOTICE | (4<<8))

/* SQLITE_OPEN_* flags (from sqlite.h.in) */
#define SQLITE_OPEN_READONLY         0x00000001
#define SQLITE_OPEN_READWRITE        0x00000002
#define SQLITE_OPEN_CREATE           0x00000004
#define SQLITE_OPEN_DELETEONCLOSE    0x00000008
#define SQLITE_OPEN_EXCLUSIVE        0x00000010
#define SQLITE_OPEN_AUTOPROXY        0x00000020
#define SQLITE_OPEN_URI              0x00000040
#define SQLITE_OPEN_MEMORY           0x00000080
#define SQLITE_OPEN_MAIN_DB          0x00000100
#define SQLITE_OPEN_TEMP_DB          0x00000200
#define SQLITE_OPEN_TRANSIENT_DB     0x00000400
#define SQLITE_OPEN_MAIN_JOURNAL     0x00000800
#define SQLITE_OPEN_TEMP_JOURNAL     0x00001000
#define SQLITE_OPEN_SUBJOURNAL       0x00002000
#define SQLITE_OPEN_SUPER_JOURNAL    0x00004000
#define SQLITE_OPEN_NOMUTEX          0x00008000
#define SQLITE_OPEN_FULLMUTEX        0x00010000
#define SQLITE_OPEN_SHAREDCACHE      0x00020000
#define SQLITE_OPEN_PRIVATECACHE     0x00040000
#define SQLITE_OPEN_WAL              0x00080000
#define SQLITE_OPEN_NOFOLLOW         0x01000000

/* SQLITE_IOCAP_* device characteristics */
#define SQLITE_IOCAP_ATOMIC                 0x00000001
#define SQLITE_IOCAP_ATOMIC512              0x00000002
#define SQLITE_IOCAP_ATOMIC1K               0x00000004
#define SQLITE_IOCAP_ATOMIC2K               0x00000008
#define SQLITE_IOCAP_ATOMIC4K               0x00000010
#define SQLITE_IOCAP_ATOMIC8K               0x00000020
#define SQLITE_IOCAP_ATOMIC16K              0x00000040
#define SQLITE_IOCAP_ATOMIC32K              0x00000080
#define SQLITE_IOCAP_ATOMIC64K              0x00000100
#define SQLITE_IOCAP_SAFE_APPEND            0x00000200
#define SQLITE_IOCAP_SEQUENTIAL             0x00000400
#define SQLITE_IOCAP_UNDELETABLE_WHEN_OPEN  0x00000800
#define SQLITE_IOCAP_POWERSAFE_OVERWRITE    0x00001000
#define SQLITE_IOCAP_IMMUTABLE              0x00002000
#define SQLITE_IOCAP_BATCH_ATOMIC           0x00004000
#define SQLITE_IOCAP_SUBPAGE_READ           0x00008000

/* File locking levels */
#define SQLITE_LOCK_NONE      0
#define SQLITE_LOCK_SHARED    1
#define SQLITE_LOCK_RESERVED  2
#define SQLITE_LOCK_PENDING   3
#define SQLITE_LOCK_EXCLUSIVE 4

/* Sync flags */
#define SQLITE_SYNC_NORMAL   0x00002
#define SQLITE_SYNC_FULL     0x00003
#define SQLITE_SYNC_DATAONLY 0x00010

/* SHM locks */
#define SQLITE_SHM_LOCK       2
#define SQLITE_SHM_SHARED     4
#define SQLITE_SHM_EXCLUSIVE  8
#define SQLITE_SHM_NLOCK      8

/* Transaction states (btreeInt.h asserts equality with TRANS_*) */
#define SQLITE_TXN_NONE  0
#define SQLITE_TXN_READ  1
#define SQLITE_TXN_WRITE 2

/* Checkpoint modes */
#define SQLITE_CHECKPOINT_NOOP    -1
#define SQLITE_CHECKPOINT_PASSIVE  0
#define SQLITE_CHECKPOINT_FULL     1
#define SQLITE_CHECKPOINT_RESTART  2
#define SQLITE_CHECKPOINT_TRUNCATE 3

/* SQLITE_FCNTL_* file-control opcodes used by the storage subsystem */
#define SQLITE_FCNTL_LOCKSTATE               1
#define SQLITE_FCNTL_GET_LOCKPROXYFILE       2
#define SQLITE_FCNTL_SET_LOCKPROXYFILE       3
#define SQLITE_FCNTL_LAST_ERRNO              4
#define SQLITE_FCNTL_SIZE_HINT               5
#define SQLITE_FCNTL_CHUNK_SIZE              6
#define SQLITE_FCNTL_FILE_POINTER            7
#define SQLITE_FCNTL_SYNC_OMITTED            8
#define SQLITE_FCNTL_WIN32_AV_RETRY          9
#define SQLITE_FCNTL_PERSIST_WAL            10
#define SQLITE_FCNTL_OVERWRITE              11
#define SQLITE_FCNTL_VFSNAME                12
#define SQLITE_FCNTL_POWERSAFE_OVERWRITE    13
#define SQLITE_FCNTL_PRAGMA                 14
#define SQLITE_FCNTL_BUSYHANDLER            15
#define SQLITE_FCNTL_TEMPFILENAME           16
#define SQLITE_FCNTL_MMAP_SIZE              18
#define SQLITE_FCNTL_TRACE                  19
#define SQLITE_FCNTL_HAS_MOVED              20
#define SQLITE_FCNTL_SYNC                   21
#define SQLITE_FCNTL_COMMIT_PHASETWO        22
#define SQLITE_FCNTL_WIN32_SET_HANDLE       23
#define SQLITE_FCNTL_WAL_BLOCK              24
#define SQLITE_FCNTL_ZIPVFS                 25
#define SQLITE_FCNTL_RBU                    26
#define SQLITE_FCNTL_VFS_POINTER            27
#define SQLITE_FCNTL_JOURNAL_POINTER        28
#define SQLITE_FCNTL_WIN32_GET_HANDLE       29
#define SQLITE_FCNTL_PDB                    30
#define SQLITE_FCNTL_BEGIN_ATOMIC_WRITE     31
#define SQLITE_FCNTL_COMMIT_ATOMIC_WRITE    32
#define SQLITE_FCNTL_ROLLBACK_ATOMIC_WRITE  33
#define SQLITE_FCNTL_LOCK_TIMEOUT           34
#define SQLITE_FCNTL_DATA_VERSION           35
#define SQLITE_FCNTL_SIZE_HINT              5
#define SQLITE_FCNTL_CKPT_DONE              36
#define SQLITE_FCNTL_RESERVE_BYTES          38
#define SQLITE_FCNTL_CKPT_START             39

/* Memory status counters (used by malloc.c, mem1.c, pcache1.c) */
#define SQLITE_STATUS_MEMORY_USED        0
#define SQLITE_STATUS_PAGECACHE_USED     1
#define SQLITE_STATUS_PAGECACHE_OVERFLOW 2
#define SQLITE_STATUS_MALLOC_SIZE        5
#define SQLITE_STATUS_PAGECACHE_SIZE     7
#define SQLITE_STATUS_MALLOC_COUNT       9

/* The sqlite3_file and sqlite3_vfs interfaces (from sqlite.h.in) */
typedef struct sqlite3_file sqlite3_file;
typedef struct sqlite3_vfs sqlite3_vfs;
typedef void (*sqlite3_syscall_ptr)(void);
typedef struct sqlite3_vfs sqlite3_vfs;
typedef struct sqlite3_mutex sqlite3_mutex;
typedef struct sqlite3_pcache sqlite3_pcache;
typedef struct sqlite3_snapshot {
  unsigned char hidden[48];
} sqlite3_snapshot;

typedef struct sqlite3_io_methods sqlite3_io_methods;
struct sqlite3_io_methods {
  int iVersion;
  int (*xClose)(sqlite3_file*);
  int (*xRead)(sqlite3_file*, void*, int iAmt, sqlite3_int64 iOfst);
  int (*xWrite)(sqlite3_file*, const void*, int iAmt, sqlite3_int64 iOfst);
  int (*xTruncate)(sqlite3_file*, sqlite3_int64 size);
  int (*xSync)(sqlite3_file*, int flags);
  int (*xFileSize)(sqlite3_file*, sqlite3_int64 *pSize);
  int (*xLock)(sqlite3_file*, int);
  int (*xUnlock)(sqlite3_file*, int);
  int (*xCheckReservedLock)(sqlite3_file*, int *pResOut);
  int (*xFileControl)(sqlite3_file*, int op, void *pArg);
  int (*xSectorSize)(sqlite3_file*);
  int (*xDeviceCharacteristics)(sqlite3_file*);
  int (*xShmMap)(sqlite3_file*, int iPg, int pgsz, int, void volatile**);
  int (*xShmLock)(sqlite3_file*, int offset, int n, int flags);
  void (*xShmBarrier)(sqlite3_file*);
  int (*xShmUnmap)(sqlite3_file*, int deleteFlag);
  int (*xFetch)(sqlite3_file*, sqlite3_int64 iOfst, int iAmt, void **pp);
  int (*xUnfetch)(sqlite3_file*, sqlite3_int64 iOfst, void *p);
};

struct sqlite3_vfs {
  int iVersion;
  int szOsFile;
  int mxPathname;
  sqlite3_vfs *pNext;
  const char *zName;
  void *pAppData;
  int (*xOpen)(sqlite3_vfs*, const char *zName, sqlite3_file*,
               int flags, int *pOutFlags);
  int (*xDelete)(sqlite3_vfs*, const char *zName, int syncDir);
  int (*xAccess)(sqlite3_vfs*, const char *zName, int flags, int *pResOut);
  int (*xFullPathname)(sqlite3_vfs*, const char *zName, int nOut, char *zOut);
  void *(*xDlOpen)(sqlite3_vfs*, const char *zFilename);
  void (*xDlError)(sqlite3_vfs*, int nByte, char *zErrMsg);
  void (*(*xDlSym)(sqlite3_vfs*,void*, const char *zSymbol))(void);
  void (*xDlClose)(sqlite3_vfs*, void*);
  int (*xRandomness)(sqlite3_vfs*, int nByte, char *zOut);
  int (*xSleep)(sqlite3_vfs*, int microseconds);
  int (*xCurrentTime)(sqlite3_vfs*, double*);
  int (*xGetLastError)(sqlite3_vfs*, int, char *);
  int (*xCurrentTimeInt64)(sqlite3_vfs*, sqlite3_int64*);
  int (*xSetSystemCall)(sqlite3_vfs*, const char *zName, sqlite3_syscall_ptr);
  sqlite3_syscall_ptr (*xGetSystemCall)(sqlite3_vfs*, const char *zName);
  const char *(*xNextSystemCall)(sqlite3_vfs*, const char *zName);
};

struct sqlite3_file {
  const struct sqlite3_io_methods *pMethods;
};

/* Page-cache pluggable interface (from sqlite.h.in) */
typedef struct sqlite3_pcache_page sqlite3_pcache_page;
struct sqlite3_pcache_page {
  void *pBuf;
  void *pExtra;
};

typedef struct sqlite3_pcache_methods2 sqlite3_pcache_methods2;
struct sqlite3_pcache_methods2 {
  int iVersion;
  void *pArg;
  int (*xInit)(void*);
  void (*xShutdown)(void*);
  sqlite3_pcache *(*xCreate)(int szPage, int szExtra, int bPurgeable);
  void (*xCachesize)(sqlite3_pcache*, int nCachesize);
  int (*xPagecount)(sqlite3_pcache*);
  sqlite3_pcache_page *(*xFetch)(sqlite3_pcache*, unsigned key, int createFlag);
  void (*xUnpin)(sqlite3_pcache*, sqlite3_pcache_page*, int reuseUnlikely);
  void (*xRekey)(sqlite3_pcache*, sqlite3_pcache_page*,
                 unsigned oldKey, unsigned newKey);
  void (*xTruncate)(sqlite3_pcache*, unsigned iLimit);
  void (*xDestroy)(sqlite3_pcache*);
  void (*xShrink)(sqlite3_pcache*);
};

typedef struct sqlite3_mem_methods sqlite3_mem_methods;
struct sqlite3_mem_methods {
  void *(*xMalloc)(int);
  void (*xFree)(void*);
  void *(*xRealloc)(void*, int);
  int (*xSize)(void*);
  int (*xRoundup)(int);
  int (*xInit)(void*);
  void (*xShutdown)(void*);
  void *pAppData;
};

typedef struct sqlite3_mutex_methods sqlite3_mutex_methods;
struct sqlite3_mutex_methods {
  int (*xMutexInit)(void);
  int (*xMutexEnd)(void);
  sqlite3_mutex *(*xMutexAlloc)(int);
  void (*xMutexFree)(sqlite3_mutex*);
  void (*xMutexEnter)(sqlite3_mutex*);
  int (*xMutexTry)(sqlite3_mutex*);
  void (*xMutexLeave)(sqlite3_mutex*);
  int (*xMutexHeld)(sqlite3_mutex*);
  int (*xMutexNotheld)(sqlite3_mutex*);
};

/* Mutex id values (subset used by storage subsystem) */
#define SQLITE_MUTEX_FAST             0
#define SQLITE_MUTEX_RECURSIVE        1
#define SQLITE_MUTEX_STATIC_MASTER    2
#define SQLITE_MUTEX_STATIC_MEM       3
#define SQLITE_MUTEX_STATIC_MEM2      4
#define SQLITE_MUTEX_STATIC_OPEN      4
#define SQLITE_MUTEX_STATIC_PRNG      5
#define SQLITE_MUTEX_STATIC_LRU       6
#define SQLITE_MUTEX_STATIC_LRU2      7
#define SQLITE_MUTEX_STATIC_PMEM      7
#define SQLITE_MUTEX_STATIC_APP1      8
#define SQLITE_MUTEX_STATIC_APP2      9
#define SQLITE_MUTEX_STATIC_APP3     10
#define SQLITE_MUTEX_STATIC_VFS1     11
#define SQLITE_MUTEX_STATIC_VFS2     11
#define SQLITE_MUTEX_STATIC_VFS3     12
#define SQLITE_MUTEX_STATIC_MAIN     2

/* Integer types (mirrors sqliteInt.h) */
#ifndef UINT32_TYPE
# define UINT32_TYPE uint32_t
#endif
#ifndef UINT16_TYPE
# define UINT16_TYPE uint16_t
#endif
#ifndef INT16_TYPE
# define INT16_TYPE int16_t
#endif
#ifndef UINT8_TYPE
# define UINT8_TYPE unsigned char
#endif
#ifndef INT8_TYPE
# define INT8_TYPE signed char
#endif
typedef sqlite_int64 i64;
typedef sqlite_uint64 u64;
typedef UINT32_TYPE u32;
typedef UINT16_TYPE u16;
typedef INT16_TYPE i16;
typedef UINT8_TYPE u8;
typedef INT8_TYPE i8;
typedef unsigned bft;

#ifndef SQLITE_PTRSIZE
# if defined(__SIZEOF_POINTER__)
#   define SQLITE_PTRSIZE __SIZEOF_POINTER__
# elif defined(i386)     || defined(__i386__)   || defined(_M_IX86) ||    \
       defined(_M_ARM)   || defined(__arm__)
#   define SQLITE_PTRSIZE 4
# else
#   define SQLITE_PTRSIZE 8
# endif
#endif
#if defined(HAVE_STDINT_H)
  typedef uintptr_t uptr;
#elif SQLITE_PTRSIZE==4
  typedef u32 uptr;
#else
  typedef u64 uptr;
#endif

#define SQLITE_MAX_U32  ((((u64)1)<<32)-1)
#define LARGEST_INT64  (0xffffffff|(((i64)0x7fffffff)<<32))
#define LARGEST_UINT64 (0xffffffff|(((u64)0xffffffff)<<32))
#define SMALLEST_INT64 (((i64)-1) - LARGEST_INT64)

#ifndef offsetof
# define offsetof(ST,M) ((size_t)((char*)&((ST*)0)->M - (char*)0))
#endif
#define sizeof64(X) ((sqlite3_int64)sizeof(X))

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L)
# define FLEXARRAY
#else
# define FLEXARRAY 1
#endif

#ifndef MIN
# define MIN(A,B) ((A)<(B)?(A):(B))
#endif
#ifndef MAX
# define MAX(A,B) ((A)>(B)?(A):(B))
#endif
#define SWAP(TYPE,A,B) {TYPE t=A; A=B; B=t;}

#define ROUND8(x)     (((x)+7)&~7)
#define ROUNDDOWN8(x) ((x)&~7)
#if SQLITE_PTRSIZE==8
# define ROUND8P(x)   (x)
#else
# define ROUND8P(x)   (((x)+7)&~7)
#endif

#ifdef SQLITE_4_BYTE_ALIGNED_MALLOC
# define EIGHT_BYTE_ALIGNMENT(X)   ((((uptr)(X) - (uptr)0)&3)==0)
#else
# define EIGHT_BYTE_ALIGNMENT(X)   ((((uptr)(X) - (uptr)0)&7)==0)
#endif
#define TWO_BYTE_ALIGNMENT(X)      ((((uptr)(X) - (uptr)0)&1)==0)

#define SQLITE_WITHIN(P,S,E)   (((uptr)(P)>=(uptr)(S))&&((uptr)(P)<(uptr)(E)))
#define SQLITE_OVERFLOW(P,S,E) (((uptr)(S)<(uptr)(P))&&((uptr)(E)>(uptr)(P)))

/*
** Atomic load/store of aligned integer values (mirrors sqliteInt.h).
*/
#ifndef __has_extension
# define __has_extension(x) 0
#endif
#if GCC_VERSION>=4007000 || __has_extension(c_atomic)
# define SQLITE_ATOMIC_INTRINSICS 1
# define AtomicLoad(PTR)       __atomic_load_n((PTR),__ATOMIC_RELAXED)
# define AtomicStore(PTR,VAL)  __atomic_store_n((PTR),(VAL),__ATOMIC_RELAXED)
#else
# define SQLITE_ATOMIC_INTRINSICS 0
# define AtomicLoad(PTR)       (*(PTR))
# define AtomicStore(PTR,VAL)  (*(PTR) = (VAL))
#endif

/*
** Byte-order detection (mirrors sqliteInt.h)
*/
#ifndef SQLITE_BYTEORDER
# if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) \
     && defined(__ORDER_LITTLE_ENDIAN__)
#  if __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
#   define SQLITE_BYTEORDER 1234
#  elif __BYTE_ORDER__==__ORDER_BIG_ENDIAN__
#   define SQLITE_BYTEORDER 4321
#  else
#   define SQLITE_BYTEORDER 0
#  endif
# elif defined(__i386__)  || defined(__x86_64__)  || defined(__x86)   ||    \
       defined(_M_IX86)   || defined(_M_X64)     || defined(_M_AMD64)||    \
       defined(_M_ARM)    || defined(_M_ARM64)   || defined(__AARCH64EL__)|| \
      (defined(__arm__) && !defined(__ARMEB__)) || defined(__riscv)
#   define SQLITE_BYTEORDER 1234
# elif defined(sparc)   || defined(__ARMEB__)     || defined(__AARCH64EB__)
#   define SQLITE_BYTEORDER 4321
# else
#   define SQLITE_BYTEORDER 0
# endif
#endif
#if SQLITE_BYTEORDER==4321
# define SQLITE_BIGENDIAN    1
# define SQLITE_LITTLEENDIAN 0
#elif SQLITE_BYTEORDER==1234
# define SQLITE_BIGENDIAN    0
# define SQLITE_LITTLEENDIAN 1
#else
# ifdef SQLITE_AMALGAMATION
  const int sqlite3one = 1;
# else
  extern const int sqlite3one;
# endif
# define SQLITE_BIGENDIAN    (*(char *)(&sqlite3one)==0)
# define SQLITE_LITTLEENDIAN (*(char *)(&sqlite3one)==1)
#endif

/*
** GCC_VERSION / MSVC_VERSION (mirrors sqliteInt.h)
*/
#if defined(__GNUC__) && !defined(SQLITE_DISABLE_INTRINSIC)
# define GCC_VERSION (__GNUC__*1000000+__GNUC_MINOR__*1000+__GNUC_PATCHLEVEL__)
#else
# define GCC_VERSION 0
#endif
#if defined(_MSC_VER) && !defined(SQLITE_DISABLE_INTRINSIC)
# define MSVC_VERSION _MSC_VER
#else
# define MSVC_VERSION 0
#endif

/*
** SQLITE_USE_SEH by default on MSVC builds.
*/
#if defined(_MSC_VER) && !defined(SQLITE_OMIT_SEH)
# define SQLITE_USE_SEH 1
#else
# undef SQLITE_USE_SEH
#endif

#define SQLITE_DIRECT_OVERFLOW_READ 1

#define SQLITE_SYSTEM_MALLOC 1
#define SQLITE_MUTEX_PTHREADS 1

/* THREADSAFE default */
#if !defined(SQLITE_THREADSAFE)
# define SQLITE_THREADSAFE 1
#endif

#define SQLITE_POWERSAFE_OVERWRITE 1

#ifndef SQLITE_MALLOC_SOFT_LIMIT
# define SQLITE_MALLOC_SOFT_LIMIT 1024
#endif

#if !defined(NDEBUG) && !defined(SQLITE_DEBUG)
# define NDEBUG 1
#endif
#if defined(NDEBUG) && defined(SQLITE_DEBUG)
# undef NDEBUG
#endif
#include <assert.h>

/* Coverage / testing macros (mirrors sqliteInt.h) */
#if defined(SQLITE_COVERAGE_TEST) || defined(SQLITE_DEBUG)
# ifndef SQLITE_AMALGAMATION
    extern unsigned int sqlite3CoverageCounter;
# endif
# define testcase(X)  if( X ){ sqlite3CoverageCounter += (unsigned)__LINE__; }
#else
# define testcase(X)
#endif

#if !defined(NDEBUG) || defined(SQLITE_COVERAGE_TEST)
# define TESTONLY(X)  X
#else
# define TESTONLY(X)
#endif

#ifndef NDEBUG
# define VVA_ONLY(X)  X
#else
# define VVA_ONLY(X)
#endif

#if defined(SQLITE_OMIT_AUXILIARY_SAFETY_CHECKS)
# define ALWAYS(X)      (1)
# define NEVER(X)       (0)
#elif !defined(NDEBUG)
# define ALWAYS(X)      ((X)?1:(assert(0),0))
# define NEVER(X)       ((X)?(assert(0),1):0)
#else
# define ALWAYS(X)      (X)
# define NEVER(X)       (X)
#endif

#if defined(SQLITE_MUTATION_TEST)
# define OK_IF_ALWAYS_TRUE(X)  (1)
# define OK_IF_ALWAYS_FALSE(X) (0)
#else
# define OK_IF_ALWAYS_TRUE(X)  (X)
# define OK_IF_ALWAYS_FALSE(X) (X)
#endif

#if defined(SQLITE_TEST_REALLOC_STRESS)
# define ONLY_IF_REALLOC_STRESS(X)  (X)
#elif !defined(NDEBUG)
# define ONLY_IF_REALLOC_STRESS(X)  ((X)?(assert(0),1):0)
#else
# define ONLY_IF_REALLOC_STRESS(X)  (0)
#endif

#if defined(SQLITE_COVERAGE_TEST) || defined(SQLITE_MUTATION_TEST)
# define SQLITE_OMIT_AUXILIARY_SAFETY_CHECKS  1
#endif

#define likely(X)    (X)
#define unlikely(X)  (X)
#define IS_BIG_INT(X)  (((X)&~(i64)0xffffffff)!=0)

/*
** SQLITE_WSD / GLOBAL (mirrors sqliteInt.h)
*/
#ifdef SQLITE_OMIT_WSD
# define SQLITE_WSD const
# define GLOBAL(t,v) (*(t*)sqlite3_wsd_find((void*)&(v), sizeof(v)))
# define SQLITE_WSD_INIT(t,v) 0
#else
# define SQLITE_WSD
# define GLOBAL(t,v) v
# define SQLITE_WSD_INIT(t,v) v
#endif

/*
** Global configuration object.  Layout mirrors the storage-relevant
** subset of struct Sqlite3Config.
*/
struct Sqlite3Config {
  int bMemstat;                     /* True to enable memory status */
  u8 bCoreMutex;                    /* True to enable core mutexing */
  u8 bFullMutex;                    /* True to enable full mutexing */
  u8 bOpenUri;                      /* True to interpret filenames as URIs */
  int szLookaside;                  /* Default lookaside buffer size */
  int nLookaside;                   /* Default lookaside buffer count */
  int mxStrlen;                     /* Maximum string length */
  int neverCorrupt;                 /* Database is always well-formed */
  int nStmtSpill;                   /* Stmt-journal spill-to-disk threshold */
  sqlite3_mem_methods m;            /* Low-level memory allocation */
  sqlite3_mutex_methods mutex;      /* Low-level mutex interface */
  sqlite3_pcache_methods2 pcache2;  /* Low-level page-cache interface */
  void *pHeap;                      /* Heap storage space */
  int nHeap;                        /* Size of pHeap[] */
  int mnReq, mxReq;                 /* Min and max heap requests sizes */
  sqlite3_int64 szMmap;             /* mmap() space per open file */
  sqlite3_int64 mxMmap;             /* Maximum value for szMmap */
  void *pPage;                      /* Page cache memory */
  int szPage;                       /* Size of each page in pPage[] */
  int nPage;                        /* Number of pages in pPage[] */
  int isInit;                       /* True after initialization */
  int inProgress;                   /* True while initialization in progress */
  int isMutexInit;                  /* True after mutexes are initialized */
  int isMallocInit;                 /* True after malloc is initialized */
  int isPCacheInit;                 /* True after pcache is initialized */
  int nRefInitMutex;                /* Number of users of pInitMutex */
  sqlite3_mutex *pInitMutex;        /* Mutex used by btreelite_init() */
  int sharedCacheEnabled;           /* True if shared-cache mode enabled */
  void (*xLog)(void*,int,const char*);
  void *pLogArg;
  int (*xTestCallback)(int);        /* Invoked by sqlite3FaultSim() */
  unsigned int iPrngSeed;           /* Alternative fixed seed for the PRNG */
};
extern SQLITE_WSD struct Sqlite3Config sqlite3Config;

#define CORRUPT_DB  (sqlite3Config.neverCorrupt==0)

/*
** Global configuration object for the btreelite library.  The storage
** sources read it via the historical sqlite3GlobalConfig spelling.
*/
#define sqlite3GlobalConfig sqlite3Config

/*
** sqlite3_busy handler struct (from sqliteInt.h)
*/
typedef struct BusyHandler BusyHandler;
struct BusyHandler {
  int (*xBusyHandler)(void *,int);
  void *pBusyArg;
  int nBusy;
};

/*
** The sqlite3 type is, in btreelite, a thin "environment" object: it
** carries the connection mutex, the busy handler, and the interrupt flag
** that wal.c and pager.c inspect.  The full SQL-layer sqlite3 struct is
** NOT reproduced here — only the fields the storage subsystem touches.
**
** Fields are ordered so that the offsets used in wal.c / pager.c
** (db->u1.isInterrupted, db->busyHandler, db->mutex, db->nSavepoint)
** are all present.
*/
typedef struct LookasideSlot LookasideSlot;
struct LookasideSlot {
  struct LookasideSlot *pNext;
};
typedef struct Lookaside Lookaside;
struct Lookaside {
  u32 bDisable;
  u16 sz;
  u16 szTrue;
  u8 bMalloced;
  u32 nSlot;
  u32 anStat[3];
  LookasideSlot *pInit;
  LookasideSlot *pFree;
  LookasideSlot *pSmallInit;
  LookasideSlot *pSmallFree;
  void *pMiddle;
  void *pStart;
  void *pEnd;
  void *pTrueEnd;
};
#define LOOKASIDE_SMALL 128
#define DisableLookaside  db->lookaside.bDisable++;db->lookaside.sz=0
#define EnableLookaside   db->lookaside.bDisable++;\
   db->lookaside.sz=db->lookaside.bDisable?0:db->lookaside.szTrue

/*
** Parse is opaque to the storage subsystem.  sqlite3OomFault() touches
** db->pParse->rc / nErr / pOuterParse; those fields exist in this stub.
** Phase 2 removes the branch and this stub.
*/
typedef struct Parse Parse;
struct Parse {
  int rc;
  int nErr;
  struct Parse *pOuterParse;
};
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_file sqlite3_file;
typedef u32 Pgno;              /* Page number type (from pager.h) */
typedef struct Db Db;
struct sqlite3 {
  sqlite3_mutex *mutex;         /* Connection mutex */
  u64 flags;                    /* flags settable by pragmas (subset) */
  struct btreelite_env_init {   /* Information used during init */
    u8 busy;                    /* TRUE if currently initializing */
  } init;
  union {
    volatile int isInterrupted;
    double notUsed1;
  } u1;
  BusyHandler busyHandler;
  /* WAL autocheckpoint hook: invoked with the frame count after each
  ** commit (see btreelite_commit / btreelite_rollback). */
  int (*xWalCallback)(void*, struct sqlite3*, const char*, int);
  void *pWalArg;                /* First argument for xWalCallback */
  int busyTimeout;              /* Busy handler timeout, in msec */
  void *pAutovacPagesArg;           /* unused stub for btree.c signature */
  void (*xAutovacDestr)(void*);
  unsigned int (*xAutovacPages)(void*,const char*,u32,u32,u32);
  Db *aDb;                      /* All backends */
  int nDb;                      /* Number of backends currently in use */
  i64 szMmap;                   /* Default mmap_size setting */
  int nSavepoint;               /* Number of non-transaction savepoints */
  int nStatement;               /* Number of nested statement-transactions */
  Lookaside lookaside;
  u8 mallocFailed;
  u8 bBenignMalloc;
  int *pnBytesFreed;
  int nVdbeActive;              /* Number of VDBEs currently running */
  int nVdbeRead;                /* Number of active VDBEs that read or write */
  int nVdbeWrite;               /* Number of active VDBEs that read and write */
  int nVdbeExec;                /* Number of nested calls to VdbeExec() */
  int errMask;                  /* & result codes with this before returning */
  struct Parse *pParse;         /* Current parse (opaque; SQL-layer stub) */
#ifdef SQLITE_ENABLE_SETLK_TIMEOUT
  int setlkTimeout;
#endif
};

typedef struct sqlite3_backup sqlite3_backup;
typedef struct StrAccum StrAccum;
struct StrAccum {
  sqlite3 *db;
  char *zText;
  u32  nAlloc;
  u32  mxAlloc;
  u32  nChar;
  u8   accError;
  u8   printfFlags;
};
typedef struct sqlite3_value sqlite3_value;
typedef struct sqlite3_value Mem; /* alias */
typedef struct sqlite3_context sqlite3_context;
typedef struct Savepoint Savepoint;
typedef struct KeyInfo KeyInfo;
typedef struct sqlite3_value Mem; /* record-compare cells are values */
typedef struct UnpackedRecord UnpackedRecord;
/*
** sqlite3BtreeInsert() and friends use the following types.  The
** KeyInfo and UnpackedRecord structures below mirror the subset of
** sqliteInt.h used by btree.c.  In btreelite the record-compare
** functions are replaced by raw memcmp on the key blob, so KeyInfo
** shrinks to a connection back-pointer.
*/
struct KeyInfo {
  u32 nRef;
  u16 nKeyField;      /* Number of key columns in the index */
  u16 nAllField;      /* Total columns, including key plus others */
  struct sqlite3 *db; /* The database connection */
  u8 *aSortFlags;
};

struct UnpackedRecord {
  KeyInfo *pKeyInfo;
  Mem *aMem;
  union {
    char *z;
    i64 i;
  } u;
  int n;
  u16 nField;
  i8 default_rc;
  u8 errCode;
  i8 r1;
  i8 r2;
  u8 eqSeen;
};

typedef int (*RecordCompare)(int, const void*, UnpackedRecord*);

/* os.h triggers os_setup.h platform detection (SQLITE_OS_UNIX / OS_WIN);
** without it os_unix.c compiles to an empty translation unit. */
#include "os.h"

/* Storage-subsystem headers: full definitions for the Pager / Btree /
** PCache / WAL types forward-declared above. */
#include "pager.h"
#include "btree.h"
#include "pcache.h"
#include "wal.h"

/*
** The Db type is retained because setDefaultSyncFlag() in btree.c walks
** db->aDb[] looking for the Btree whose pBt it was given.  In btreelite
** there is exactly one database per handle, so the array has length 1.
*/
typedef struct Btree Btree;
typedef struct BtShared BtShared;
typedef struct BtCursor BtCursor;
typedef struct BtreePayload BtreePayload;
typedef struct Pager Pager;
typedef struct PgHdr PgHdr;
typedef struct Wal Wal;
typedef struct PCache PCache;
typedef struct Bitvec Bitvec;
struct Db {
  char *zDbSName;
  Btree *pBt;
  u8 safety_level;
  u8 bSyncSet;
};

/* Schema is opaque to the storage subsystem; sqlite3BtreeSchema() reserves
** a blob whose size is supplied by the caller.  Keep a forward reference
** so btree.c compiles; the SQL layer's real Schema is gone. */
typedef struct Schema Schema;

/*
** sqlite3.h.in also defines these limits; btreelite keeps them as
** compile-time constants.
*/
#ifndef SQLITE_DEFAULT_JOURNAL_SIZE_LIMIT
# define SQLITE_DEFAULT_JOURNAL_SIZE_LIMIT -1
#endif
#ifndef SQLITE_DEFAULT_PCACHE_INITSZ
# define SQLITE_DEFAULT_PCACHE_INITSZ 20
#endif
#ifndef SQLITE_DEFAULT_MEMSTATUS
# define SQLITE_DEFAULT_MEMSTATUS 1
#endif
#ifndef SQLITE_DEFAULT_LOOKASIDE
# define SQLITE_DEFAULT_LOOKASIDE 1200,100
#endif
#ifndef SQLITE_STMTJRNL_SPILL
# define SQLITE_STMTJRNL_SPILL 65536
#endif
#ifndef SQLITE_USE_URI
# define SQLITE_USE_URI 0
#endif
#ifndef SQLITE_MAX_MMAP_SIZE
# if defined(__OpenBSD__) || defined(__QNXNTO__)
#   define SQLITE_MAX_MMAP_SIZE 0
# elif defined(__linux__) || defined(_WIN32) \
    || (defined(__APPLE__) && defined(__MACH__)) \
    || defined(__sun) || defined(__FreeBSD__) || defined(__DragonFly__)
#   define SQLITE_MAX_MMAP_SIZE 0x7fff0000
# else
#   define SQLITE_MAX_MMAP_SIZE 0
# endif
#endif
#ifndef SQLITE_DEFAULT_MMAP_SIZE
# define SQLITE_DEFAULT_MMAP_SIZE 0
#endif
#if SQLITE_DEFAULT_MMAP_SIZE>SQLITE_MAX_MMAP_SIZE
# undef SQLITE_DEFAULT_MMAP_SIZE
# define SQLITE_DEFAULT_MMAP_SIZE SQLITE_MAX_MMAP_SIZE
#endif
#ifndef SQLITE_MAX_PAGE_SIZE
# define SQLITE_MAX_PAGE_SIZE 65536
#endif
#ifndef SQLITE_DEFAULT_PAGE_SIZE
# define SQLITE_DEFAULT_PAGE_SIZE 4096
#endif
#ifndef SQLITE_MAX_DEFAULT_PAGE_SIZE
# define SQLITE_MAX_DEFAULT_PAGE_SIZE 8192
#endif
#if SQLITE_MAX_DEFAULT_PAGE_SIZE<SQLITE_DEFAULT_PAGE_SIZE
# undef SQLITE_MAX_DEFAULT_PAGE_SIZE
# define SQLITE_MAX_DEFAULT_PAGE_SIZE SQLITE_DEFAULT_PAGE_SIZE
#endif
#ifndef SQLITE_MAX_PAGE_COUNT
# define SQLITE_MAX_PAGE_COUNT 0xfffffffe
#endif
#ifndef SQLITE_DEFAULT_WAL_AUTOCHECKPOINT
# define SQLITE_DEFAULT_WAL_AUTOCHECKPOINT  1000
#endif
#ifndef SQLITE_DEFAULT_CACHE_SIZE
# define SQLITE_DEFAULT_CACHE_SIZE  -2000
#endif
#ifndef SQLITE_MAX_LENGTH
# define SQLITE_MAX_LENGTH 1000000000
#endif
#ifndef SQLITE_MAX_ALLOCATION_SIZE
# define SQLITE_MAX_ALLOCATION_SIZE  2147483391
#endif

#ifndef SQLITE_DEFAULT_SYNCHRONOUS
# define SQLITE_DEFAULT_SYNCHRONOUS 2
#endif
#ifndef SQLITE_DEFAULT_WAL_SYNCHRONOUS
# define SQLITE_DEFAULT_WAL_SYNCHRONOUS SQLITE_DEFAULT_SYNCHRONOUS
#endif

#ifndef SQLITE_DEFAULT_FILE_FORMAT
# define SQLITE_DEFAULT_FILE_FORMAT 4
#endif
#define SQLITE_MAX_FILE_FORMAT 4

#ifndef SQLITE_DEFAULT_AUTOVACUUM
# define SQLITE_DEFAULT_AUTOVACUUM 0
#endif
#ifndef SQLITE_DEFAULT_TEMP_CACHE_SIZE
# define SQLITE_DEFAULT_TEMP_CACHE_SIZE 0
#endif

#define SQLITE_MAX_DB 1

#ifndef SQLITE_MAX_PATHLEN
# define SQLITE_MAX_PATHLEN FILENAME_MAX
#endif

/*
** Memory-function and mutex-function prototypes used by the storage
** subsystem (extracted from sqliteInt.h, keeping original spellings).
*/
int sqlite3MallocInit(void);
void sqlite3MallocEnd(void);
void *sqlite3Malloc(u64);
void *sqlite3MallocZero(u64);
void *sqlite3DbMallocZero(sqlite3*, u64);
void *sqlite3DbMallocRaw(sqlite3*, u64);
void *sqlite3DbMallocRawNN(sqlite3*, u64);
char *sqlite3DbStrDup(sqlite3*,const char*);
char *sqlite3DbStrNDup(sqlite3*,const char*, u64);
char *sqlite3DbSpanDup(sqlite3*,const char*,const char*);
void *sqlite3Realloc(void*, u64);
void *sqlite3DbReallocOrFree(sqlite3 *, void *, u64);
void *sqlite3DbRealloc(sqlite3 *, void *, u64);
void sqlite3DbFree(sqlite3*, void*);
void sqlite3DbFreeNN(sqlite3*, void*);
int sqlite3MallocSize(const void*);
int sqlite3DbMallocSize(sqlite3*, const void*);
void *sqlite3PageMalloc(int);
void sqlite3PageFree(void*);
void sqlite3MemSetDefault(void);
void sqlite3BenignMallocHooks(void (*)(void), void (*)(void));
int sqlite3HeapNearlyFull(void);
int sqlite3MallocUsed(void);
void sqlite3MemSetArrayInt64(void *p, int i, i64 v);

#ifndef SQLITE_MUTEX_OMIT
  sqlite3_mutex_methods const *sqlite3DefaultMutex(void);
  sqlite3_mutex_methods const *sqlite3NoopMutex(void);
  sqlite3_mutex *sqlite3MutexAlloc(int);
  int sqlite3MutexInit(void);
  int sqlite3MutexEnd(void);
#endif
#if !defined(SQLITE_MUTEX_OMIT) && !defined(SQLITE_MUTEX_NOOP)
  void sqlite3MemoryBarrier(void);
#else
# define sqlite3MemoryBarrier()
#endif

sqlite3_int64 sqlite3StatusValue(int);
void sqlite3StatusUp(int, int);
void sqlite3StatusDown(int, int);
void sqlite3StatusHighwater(int, int);
sqlite3_mutex *sqlite3Pcache1Mutex(void);
sqlite3_mutex *sqlite3MallocMutex(void);

void sqlite3BeginBenignMalloc(void);
void sqlite3EndBenignMalloc(void);
int sqlite3FaultSim(int);

int sqlite3IsNaN(double);

int sqlite3StrICmp(const char*,const char*);
int sqlite3Strlen30(const char*);
#define sqlite3Strlen30NN(C) (strlen(C)&0x3fffffff)
char *sqlite3MPrintf(sqlite3*,const char*, ...);
char *sqlite3VMPrintf(sqlite3*,const char*, va_list);
void sqlite3DebugPrintf(const char*, ...);

void sqlite3SetString(char **, sqlite3*, const char*);

/* Page-cache plumbing used by the storage subsystem */
int sqlite3PcacheInitialize(void);
void sqlite3PcacheShutdown(void);
void sqlite3PCacheSetDefault(void);
int sqlite3HeaderSizePcache(void);
int sqlite3HeaderSizePcache1(void);
void sqlite3PCacheBufferSetup(void *, int sz, int n);

/* VFS registration (os.c) */
int sqlite3_vfs_register(sqlite3_vfs*, int makeDflt);
int sqlite3_vfs_unregister(sqlite3_vfs*);
sqlite3_vfs *sqlite3_vfs_find(const char *zVfs);

/* PRNG (random.c) */
void sqlite3_randomness(int, void*);

/* Schema / Savepoint / value types (real definitions live elsewhere) */
typedef struct Schema Schema;
typedef struct Savepoint Savepoint;
typedef struct sqlite3_snapshot sqlite3_snapshot;
typedef struct sqlite3_value sqlite3_value;
typedef struct sqlite3_context sqlite3_context;

struct sqlite3;


/*
** sqlite3BtreeSchema()'s xFreeSchema hook needs a schema destructor
** signature; the actual schema object is opaque (see above).
*/

/*
** Record-compare function pointer used by btree.c.
*/
typedef int (*RecordCompare)(int, const void*, UnpackedRecord*);

/*
** The number of bytes in a btree cursor object, exposed so that the
** public API can preallocate cursors.
*/
int sqlite3BtreeCursorSize(void);

/* StrAccum (sqlite3_str) — used by integrity_check message building */
typedef struct StrAccum sqlite3_str;

/*
** The SQLITE_PTR_TO_INT / SQLITE_INT_TO_PTR macros (mirrors sqliteInt.h)
*/
#if defined(HAVE_STDINT_H)
# define SQLITE_INT_TO_PTR(X)  ((void*)(intptr_t)(X))
# define SQLITE_PTR_TO_INT(X)  ((int)(intptr_t)(X))
#elif defined(__PTRDIFF_TYPE__)
# define SQLITE_INT_TO_PTR(X)  ((void*)(__PTRDIFF_TYPE__)(X))
# define SQLITE_PTR_TO_INT(X)  ((int)(__PTRDIFF_TYPE__)(X))
#elif !defined(__GNUC__)
# define SQLITE_INT_TO_PTR(X)  ((void*)&((char*)0)[X])
# define SQLITE_PTR_TO_INT(X)  ((int)(((char*)X)-(char*)0))
#else
# define SQLITE_INT_TO_PTR(X)  ((void*)(X))
# define SQLITE_PTR_TO_INT(X)  ((int)(X))
#endif

/*
** SQLITE_NOINLINE / SQLITE_INLINE hints (mirrors sqliteInt.h)
*/
#if defined(__GNUC__)
#  define SQLITE_NOINLINE  __attribute__((noinline))
#  define SQLITE_INLINE    __attribute__((always_inline)) inline
#elif defined(_MSC_VER) && _MSC_VER>=1310
#  define SQLITE_NOINLINE  __declspec(noinline)
#  define SQLITE_INLINE    __forceinline
#else
#  define SQLITE_NOINLINE
#  define SQLITE_INLINE
#endif
#if defined(SQLITE_COVERAGE_TEST) || defined(__STRICT_ANSI__)
# undef SQLITE_INLINE
# define SQLITE_INLINE
#endif

#define UNUSED_PARAMETER(x) (void)(x)
#define UNUSED_PARAMETER2(x,y) UNUSED_PARAMETER(x),UNUSED_PARAMETER(y)
#define ArraySize(X)    ((int)(sizeof(X)/sizeof(X[0])))
#define IsPowerOfTwo(X) (((X)&((X)-1))==0)

/*
** The database header size, btree header size, pcache header sizes.
*/
int sqlite3HeaderSizeBtree(void);
int sqlite3HeaderSizePcache1(void);

/*
** The following constants limit the size of various structures.
*/
#define SQLITE_MAX_PAGE_COUNT 0xfffffffe  /* 4294967294 */

#endif /* BTREELITEINT_H */