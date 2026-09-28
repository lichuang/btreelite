# Phase 1 报告：pager 层脱 SQL

日期：2026-09-25
状态：**完成** —— 24 个编译单元零错误，`libbtreelite.a`（286KB）零 SQL 层外部符号
（仅剩 libc/POSIX），冒烟测试 **20 checks / 0 failures**。

## 1. 交付物

| 交付物 | 位置 | 说明 |
|---|---|---|
| 库 | `libbtreelite.a` | 24 个 TU，仅依赖 libc/pthread |
| 兼容层 | `src/btreelite_compat.c` | 错误报告入口、busy handler、初始化、memcmp 比较器、SQL 查询桩 |
| 错误机制 | `src/fault.c` | BenignMalloc 钩子（上游移植） |
| 无操作互斥 | `src/mutex_noop.c` | 单线程互斥表（上游移植） |
| 格式化 | `src/printf.c` `src/utf.c` | StrAccum 机制 + 格式引擎 + UTF-8（裁剪 SQL 专用格式 %T/%S、RCStr） |
| 冒烟测试 | `test/t_smoke.c` | 直接驱动 btree/pager/journal |
| 构建 | `Makefile`（根目录） | `make` / `make test` / `make scan` / `make census` |

## 2. 编译与链接结果

```
24 个 TU，0 error / 0 error-warning
libbtreelite.a  286,880 bytes
未解析符号：仅 libc/POSIX（_open/_read/_pwrite/_mmap/_fsync/_fcntl/
_pthread_* 等）—— 无任何 sqlite3 SQL 层符号残留
```

## 3. 冒烟测试（`make test`）

```
testCommitPersist : 建库 → 写 50 行（BTREE_INTKEY，BTREE_APPEND）→ commit
                    → 关闭 → 重开 → 逐行校验 rowid 与 payload → 50 行全对
testRollback      : 已有 50 行 → 再写 20 行 → rollback → 重读仍 50 行
testHotJournal    : fork 子进程写脏页后 _exit（不 commit）→ 父进程重开
                    → hot-journal 自动回放 → 50 行完好
20 checks, 0 failures
```

这验证了存储机制的四个核心保证：**落盘、重读、回滚、崩溃恢复**，
且全程不经 SQL 层。

## 4. Phase 1 完成的缝线工作

| 工作项 | 做法 |
|---|---|
| printf 机制 | 移植 printf.c（1597 行，删 %T/%S/RCStr/SQLFUNC 的 value 访问依赖）+ utf.c |
| 错误报告入口 | `sqlite3ReportError/CorruptError/MisuseError/CantopenError/CorruptPgnoError/NomemError/IoerrnomemError` 从 main.c 移入 compat.c |
| BenignMalloc | 移植 fault.c；`SQLITE_UNTESTABLE` 改为未定义（否则 fault.c 编空） |
| 默认互斥 | 移植 mutex_noop.c；定义 `SQLITE_MUTEX_PTHREADS` 激活 mutex_unix.c |
| 内存分配 | 定义 `SQLITE_SYSTEM_MALLOC` 激活 mem1.c；`sqlite3MemSetDefault` 直接绑定方法表（不经 sqlite3_config）；**移除 mem0.c**（它是 SQLITE_ZERO_MALLOC 的占位实现，会顶掉 mem1 的符号） |
| OS 初始化 | btreeliteInt.h 补 `#include "os.h"`，触发 os_setup.h 的平台探测 `SQLITE_OS_UNIX`（否则 os_unix.c 编成空文件） |
| super-journal | 删 `readSuperJournal/writeSuperJournal/pager_delsuper/freeSuperJournal/pagerIsSuperJrnlName` 五函数及调用点；`zSuper` 参数保留恒 NULL |
| KV 比较器 | compat.c 提供 memcmp 版 `sqlite3VdbeRecordUnpack/AllocUnpackedRecord/RecordCompare/FindCompare`，替代 vdbe.c 的 serial-type 记录比较 |
| 初始化顺序 | `sqlite3_initialize` 用 `isInit`/`inProgress` 守卫防重入，顺序为 Mutex→Malloc→Pcache→Os（对齐上游 main.c） |

## 5. Phase 1 期间遇到并修复的运行时缺陷

| 缺陷 | 根因 | 修复 |
|---|---|---|
| 初始化段错误（PC=0） | `sqlite3_initialize` 先 MallocInit 后 MutexInit，而 MallocInit 内部调 MutexAlloc | 调换顺序（对齐上游） |
| VFS 为空（`sqlite3_vfs_find(0)==NULL`） | os_unix.c 因缺 `SQLITE_OS_UNIX` 编成空 TU | btreeliteInt.h 补 `#include "os.h"` |
| 无限递归 | `sqlite3_vfs_register` 回调 `sqlite3_initialize`，`inited` 未及时置位 | 改用 `inProgress` 守卫 |
| `m.xInit` 为空 | mem0.c 的 `sqlite3MemSetDefault` 占位版在归档中优先于 mem1.c | Makefile 移除 mem0.c |
| 重复符号 ×3 | compat.c 与 pager.c/util.c/mem1.c 重复定义 | 删 compat.c 的重复项 |

## 6. 与计划的偏差

- **Phase 1.4（签名改 btreelite_db*）未执行**：审查后确认 pager.c/wal.c 对
  `sqlite3*` 的使用仅 3 处（`u1.isInterrupted`、`mallocFailed`、`flags&NoCkptOnClose`），
  而 Phase 0 的 struct sqlite3 已裁剪到只剩这些字段。改类型名只有可读性收益，
  无功能收益，且会制造大 diff。**决定：保留 `sqlite3*` 拼写**（内部类型，
  公共 API 层用 btreelite_db*）。真正的收缩在 Phase 2 随调用点删除完成。
- **printf.c 整文件移植**（非计划的最小子集）：因 integrity_check 消息与
  sqlite3_log 共用格式引擎，拆分会重复实现；整文件移植仅 1597 行。
- 剩余 6 个编译警告（btreeHeap*/invalidateAllOverflowCache 未使用、
  3 个 unused var）：来自已删的 autovacuum/super-journal 分支，
  Phase 2 正式清理 autovacuum 时消除，当前不阻塞。

## 7. Phase 2 入口（btree 层 KV 改造）

1. **KV cell 格式**：`CellInfo` 增加 `nKeyBytes`/`nValueBytes`；新增
   `btreeParseCellPtrKV`/`cellSizePtrKV` 变体；`decodeFlags` 按 `BTS_KV` 走 KV 分支。
2. **比较器切换**：`btreeMoveto` 的 `sqlite3VdbeRecordCompare` 调用点改为
   compat.c 的 memcmp 版（已完成雏形）；`KeyInfo` 依赖解除。
3. **删除 autovacuum**（`SQLITE_OMIT_AUTOVACUUM` 已定义，清理残余警告）。
4. **删除 intkey 专用路径**：`btreeParseCellPtr`/`sqlite3BtreeTableMoveto`/
   `BTREE_PREFORMAT`/`sqlite3BtreeTransferRow` 等。
5. **`btreelite_put/get` 实现**：`[key][value]` 拼接 cell 的写入与切分读取。
6. 目标：`test/t_kv.c` 覆盖 CRUD/遍历/seek/大 value/多树，`make test` 全绿。