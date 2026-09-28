# Phase 2 报告：btree 层 KV 改造

日期：2026-09-25
状态：**核心完成** —— 公共 API 全链路可用，`make test` 双套件
**20 + 1529 checks / 0 failures**（CRUD、读回校验、正/反向遍历、seek、删除、
回滚、多树）。大 value（>页）链路为已知缺陷（见 §6），已隔离为 Phase 2.7b。

## 1. 交付物

| 交付物 | 位置 | 说明 |
|---|---|---|
| 公共 API 实现 | `src/btreelite_api.c` | btreelite_open/close/put/get/del/first/last/next/prev/seek/事务/checkpoint |
| KV 记录编码 | `src/btreelite_api.c` `sqlite3KvEncode` | 拼接 `[varint nKeyBytes][key][value]` |
| KV 解析 | `src/btree.c` btreeParseCellPtrIndex KV 块（+25 行） | 记录 key/value 边界到 `CellInfo.nKeyBytes` |
| KV 标记 | `src/btreeInt.h` `BTS_KV` | BtShared 级 KV 模式位 |
| 比较器 | `src/btreelite_compat.c` kvCompare/kvRecordDecode | cell 记录 vs 原始 key 的 memcmp 比较 |
| KV 测试 | `test/t_kv.c` | 1529 checks |
| 构建 | `Makefile` | `make test` = t_smoke + t_kv |

## 2. 设计定案（与 phase.md §2 的差异）

实施中把 KV 记录格式收敛为**与 SQLite index btree 完全同构**的方案：

```
KV leaf cell  = index leaf cell，payload = KV 记录
KV 记录       = [varint nKeyBytes][key][value]
KV divider    = index interior cell（payload = 原 divider 内容，同构）
```

由此 **btree.c 的算法主体零修改**（balance/overflow/游标原样工作），
改动只有三处：

1. `CellInfo` 增加 `nKeyBytes`（读取时切分 key/value 的边界信息）；
2. `btreeParseCellPtrIndex` 尾部在 `BTS_KV` 树上多读一个 varint
   （记录 key 字节数，payload 指针跳过 KV 头）；
3. 公共 API 层在 insert 前 encode、读取时按 cache 切分。

`btreelite_put` 把 (k,v) 编码为 KV 记录后直接走
`sqlite3BtreeInsert(pCur, &x, 0, loc)`，seekResult 复用 kvLocate 的
IndexMoveto 结果，避免二次搜索；overwrite 快路径由 Insert 原生
`info.nKey==pX->nKey` 判断自然成立（KV 记录长度两侧一致）。

## 3. API 语义定案（含运行时验证修正）

| API | 语义 | 关键实现点 |
|---|---|---|
| put | kvLocate → encode → Insert(seekResult=loc) → kvLocate 复位 | 插入点复用避免二次搜索 |
| get | kvLocate，res==0 命中 | 未命中停在邻居，返回 NOTFOUND |
| del | get 命中后 `BtreeDelete` | 删除后 invalidate |
| seek | **Moveto + 前进推进**：res<0 时 Next 一次；推进后用完整比较刷新 res | IndexMoveto 是"插入点"语义，不保证停在 >=k |
| first/last/next/prev | 每次移动后 `kvCacheRefresh` | EOF 返回 BTREELITE_DONE |
| 空树 | kvMoveto 返回 EMPTY → res=1 → get/seek 返回 NOTFOUND/EOF | 处理 `SQLITE_EMPTY` |

**关键修复**：IndexMoveto 可能停在"最后一个小于 key 的 cell"（页尾插入点），
`btreelite_seek` 旧代码在 res!=0 时 invalidate 缓存，与新推进逻辑冲突；
已改为由 kvSeekRaw 统一管理缓存有效性。

## 4. 测试矩阵（`make test`）

```
t_smoke (Phase 1) : 20 checks / 0 failures（intkey 机制回归）
t_kv    (Phase 2) : 1529 checks / 0 failures
  - 200 个无碰撞 key 插入 + commit
  - 逐 key get + key/value 内容精确校验（200 轮）
  - 正向遍历（含顺序断言）+ 反向遍历，各 200 条
  - seek 到存在 key / 不存在 key / 越过末尾（EOF）
  - 删除半数（100 次 del）+ 复扫计数
  - rollback 撤销 temp-key 且已提交数据完好（含计数）
  - checkpoint / journal_mode / page_count
```

## 5. Phase 2 未执行项（顺延）

| 项 | 状态 | 说明 |
|---|---|---|
| 删 autovacuum 残余 | pending | `SQLITE_OMIT_AUTOVACUUM` 已定义，剩余为 unused 警告清理 |
| 删 intkey 专用路径 | 顺延 | intkey parse/moveto 无调用者冲突（KV 文件不用 intkey 树），删除属清理性质 |
| 魔串替换 | 顺延 | 文件魔串仍是 SQLite 的 `"SQLite format 3"`；KV 格式已自洽，换串影响 sqlite3 CLI 互斥，安排在 Phase 3 一起做（届时 cell 格式定稿） |

## 6. 已知缺陷：大 value（>页）链路

**现象**：put/get 200KB value 返回 CORRUPT(11)。

**根因定位**（白盒复现：seek2 断点 `sqlite3BtreeIndexMoveto+60`，
`pCur->pBtree` 为 NULL 的访问崩溃）：

1. KV 记录超过 `maxLocal` 时 cell 进入 overflow 路径，本地部分只有
   header+前缀；`sqlite3BtreeInsert` 对这类 cell 走 balance；
2. balance_nonroot 的 divider 临时缓冲（`aSpace1`，pageSize 字节）在
   **divider 重新分布**时按 `sz = 原 divider 本地尺寸` 复制 —— 原生
   index divider 的 payload 就是 key，而 KV 树的 leaf 记录包含 value，
   两条路径对"divider 本地尺寸"的假设在 KV 模式下不一致；
3. `IndexMoveto` 的 overflow-cell 分支（malloc 拷贝完整记录）在
   `pCur->eState` 为 trip 后状态时访问了未复位字段。

**进一步定位（本轮）**：修复了两处次级问题后，缺陷已收敛到单点——

- 已修复 ①：KV parse 块此前错误地把 `pInfo->pPayload` 移过 KV 头，导致
  accessPayload 的读取基准整体偏移 1 字节（refresh 把 'k' 当 varint 得
  107）。现 pPayload 保持指向记录开头，value 由 `nKeyBytes` 切分。
- 已修复 ②：KV 树的 schema root 页由 intkey 型改为 `PTF_ZERODATA|PTF_LEAF`
  （newDatabase），消除 leaf/interior 类型不匹配。
- 已修复 ③：游标 KeyInfo stub 的 `nAllField` 初始化为 1（btreeMoveto 的
  字段数检查拒绝 nField>nAllField 的搜索键）。
- **剩余现象**：200KB put 本身成功（insert 无错），但随后的**重定位
  IndexMoveto 在 page=2 返回 11**（dbg3 插桩定位）。断点确认 11 不经过
  sqlite3CorruptError 入口（是 NDEBUG 下 CORRUPT_PAGE 的直接 return），
  即 IndexMoveto 的 overflow-cell malloc 分支内
  `accessPayload`/越界检查链路（btree.c:6176-6200）对"balance 后新分配的
  overflow 链"返回失败。怀疑 overflow 链页号与 BtShared.nPage 在
  balance 重排后短暂不一致，需要审计 balance_nonroot 与 fillInCell 的
  nPage 维护时序。

**规避**：t_kv（release 目标）不含超过页面的 value；put 对超限 value
返回 CORRUPT 而非崩溃（内存安全）。修复估计需半天专项
（fillInCell/balance/IndexMoveto 三方 KV 感知审计）。

## 7. Phase 3 入口

1. Phase 2.7b 大 value 修复（上节三点）。
2. WAL 接缝验证：KV 写事务在 WAL 模式下跑 checkpoint/crash 恢复
   （`t_wal.c`）。
3. 多进程锁互斥冒烟（`t_concurrent.c`）。