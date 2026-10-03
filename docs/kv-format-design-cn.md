# KV 存储格式设计：LEAFDATA 家族 + 字节串 key

> 本文档为中文版；英文版见 [KV storage format design](kv-format-design.md)。

## 1. 动机

当前实现把 KV 记录（`[varint nKeyBytes][key][value]`）塞进 **index btree** 
（ZERODATA，0x0a/0x02）的 cell payload 里。index btree 是为"记录通常很小"
设计的：

- 比较器接口 `RecordCompare(int nKey, const void *pKey, ...)` 要求**整条记录
  的连续内存**；跨 overflow 页的记录只能先 `malloc + accessPayload` 读满再比
  （btree.c:6215-6228）。二分查找路径上每个溢出 cell 物化一次。
- divider cell 携带**整份 payload**（含 value），分裂时大 value 被提升到父页，
  把 `balance_nonroot` 的页内临时缓冲顶到极限。

而 **table btree**（LEAFDATA，0x0d/0x05）恰好有 KV 想要的三个性质：

1. key 恒在本地（cell 头部的 varint 或定长字段），不参与 overflow 切分；
2. 比较只读 key，**完全不碰 data**（`sqlite3BtreeTableMoveto` 只 `getVarint`）；
3. divider **只带 key**（`btreeParseCellPtrNoPayload`，`nPayload=0`），内部页
   不被大 data 污染，分裂走 `leafData` 的 key-only divider 路径。

**结论**：保留 table btree 的页格式与分隔符语义，只把"64 位整数 key"替换为
"字节串 key"。这样大 value 走原生 overflow 路径，查找零物化。

## 2. 页类型与标志复用

沿用现有页类型字节，但在 `BTS_KV` 模式下赋予新含义：

| 页类型字节（原有含义） | KV 模式下的含义 |
|---|---|
| `0x0d` LEAFDATA\|INTKEY\|LEAF | **KV 叶子页** |
| `0x05` LEAFDATA\|INTKEY | **KV 内部页** |
| `0x0a` / `0x02`（index 家族） | KV 模式下判为 CORRUPT（不再使用） |

`decodeFlags` 在 `BTS_KV` 下为 KV 页设置：

```c
intKey     = 1;      /* 关键：复用 "整数键表" 游标分支 (pKeyInfo==0)，
                        key 类型差异只在 cell 头部编码 */
intKeyLeaf = 1;      /* 关键：balance 取 leafData=1，用 key-only divider */
xCellSize  = kvCellSizeLeaf / kvCellSizeInterior;
xParseCell = kvParseCellLeaf / kvParseCellInterior;
maxLocal   = pBt->maxLeaf - KV_CELL_HDR_OVERAGE;   /* 仅叶页，见 §3 */
minLocal   = pBt->minLeaf;
```

- `intKey=1` 让 `moveToRoot` 的一致性检查 `(pKeyInfo==0)!=pRoot->intKey` 通过
  （KV 游标 `pKeyInfo==0`，与 intkey 表游标同型），并让 `sqlite3BtreeInsert`
  走"无 KeyInfo"分支；字节串 key 的差异只在 cell 头部（`nKeyLen`+key 代替
  varint i64）。
- `intKeyLeaf=1` 让 `balance_nonroot` 取 `leafData=1`，divider 构造走
  "只有一个 key"的分支；同时 KV 页跳过 `balance_quick` 快路（其 divider
  提取逻辑是 intkey 专用的，见 §4.7）。

## 3. Cell 布局

```
KV 叶子 cell:
    [varint nValue] [varint nKeyLen] [key bytes] [value bytes] [4B ovfl?]
    （仅 value 会溢出；key 恒本地）

KV 内部（divider）cell:
    [4B child] [varint nKeyLen] [key bytes]
    （与原生 intkey divider 同构，只是 key 是字节串）
```

- `nValue` = value 长度（可为 0）；`nKeyLen` ≤ `KV_MAX_KEY`（默认 255）。
- overflow 切分**只针对 value**：`nPayload`（本地+溢出）语义在 KV 中即
  `nValue`，`minLocal` 用 `minLeaf`。
- **叶页的 `maxLocal` 不是原生 `maxLeaf`**：原生 `maxLeaf = usableSize-35`
  的预算只够 14 字节的 rowid cell 头（varint payload + varint rowid），而
  KV cell 头最坏为 262 字节（2 个 varint + 255 字节 key）。若直接使用
  `maxLeaf`，最坏 cell（header + 本地 value + 4B 溢出指针）会超过空页容量，
  `balance_nonroot` 无页可放而报 `SQLITE_CORRUPT`（长 key + 临界 value 曾
  触发此缺陷，见 t_big 回归）。因此叶页 `maxLocal` 缩减
  `KV_CELL_HDR_OVERAGE = KV_CELL_MAX_HDR(262) - 14 = 248` 字节，保持
  "最大 cell 必能放进空页"这一 balance 不变量。
- 因此 `fillInCell` 的溢出循环完全复用原生逻辑，只是溢出的是 value 段。

### CellInfo 扩展

```c
struct CellInfo {
  ... 现有字段 ...
  u8  *pKey;      /* KV: key 起点 */
  u32  nKeyLen;   /* KV: key 长度 */
};
```

（`nKeyBytes` 字段由 `pKey`+`nKeyLen` 取代；旧的 `nKeyBytes` 移除。）

## 4. 需要改动的函数（逐项）

### 4.1 新增 KV parse / size 变体（btree.c）

```
kvParseCellLeaf(pPage, pCell, pInfo)
    读 nValue varint → 读 nKeyLen varint → pKey=pIter
    → 按原生 btreeParseCellPtr 的方式决定 nLocal（只看 value）
    → pInfo->nPayload = nValue（value 才是 payload）
    → pInfo->nKeyLen = nKeyLen; pInfo->pKey = pIter

kvParseCellInterior(pPage, pCell, pInfo)
    pIter = pCell+4; 读 nKeyLen varint; pKey=pIter
    nSize = 4 + varintLen(nKeyLen) + nKeyLen
    nPayload = 0; nLocal = 0

kvCellSizeLeaf / kvCellSizeInterior
    与对应 parse 一致，只计算本地 cell 尺寸（不含 overflow）。
```

### 4.2 decodeFlags（btree.c:2028）

在 `BTS_KV` 时，`0x0d`→KV leaf、`0x05`→KV interior；index 家族（0x0a/0x02）
返回 CORRUPT。非 KV 模式保持原样（intkey 能力保留在源码中，但 btreelite 不暴露）。

### 4.3 fillInCell（btree.c:7074）

新增 KV 分支（`pPage->pBt->btsFlags & BTS_KV`）：
```
nHeader = childPtrSize(=0 for leaf)
nHeader += putVarint32(&pCell[nHeader], pX->nData + pX->nZero)   /* nValue */
nHeader += putVarint32(&pCell[nHeader], pX->nKey)                /* nKeyLen */
memcpy key
然后复用原生 payload 写入循环（写 value，溢出走 allocateBtreePage）
```
即：把原生 intkey 分支里"写整数 key varint"换成"写 nKeyLen + key 字节"，
其余不动。

### 4.4 新增 sqlite3BtreeKvMoveto（btree.c）

照抄 `sqlite3BtreeTableMoveto` 的结构，但比较用字节串：
```
二分: kvKeyCompareCell(pPage, idx, pKey, nKey)   /* 只读 key，零物化 */
```
`kvKeyCompareCell` 从**完整 cell 起点** `findCell(pPage,idx)` 解析 key
（leaf 跳两个 varint；divider 由 `kvParseCellInterior` 内部跳过 4B child），
memcmp + 长度决胜。**key 恒本地，永不触发 overflow / malloc。**
（注意：早先版本用 `findCellPastPtr` 会与 `kvParseCellInterior` 的 4B child
跳过重复，导致内部页比较错位——已修正为 `findCell`。）

`btreeMoveto()` 在 KV 游标时分派到此函数。

### 4.5 sqlite3BtreeInsert（btree.c）

- 游标为 KV 时（`pKeyInfo==0` 分支内再判 `BTS_KV`）：
  - `loc==0` 的重定位用 `sqlite3BtreeKvMoveto`（替代 btreeMoveto/IndexMoveto）；
  - **保留** `btreeOverwriteCell` 原地覆写优化，但判据同时要求 **key 字节相同
    且 value 尺寸相同**（`info.nKeyLen==pX->nKey && memcmp(...)==0` 且
    `info.nPayload==pX->nData+pX->nZero`）；不满足则 drop+insert。
- 断言为 `assert( pPage->intKey || pX->nKey>=0 )`（KV 下 `intKey==1`，恒真）。

### 4.6 balance_nonroot（btree.c:8847-8900）

`leafData=1` 时 divider 构造分支（8882-8890）当前是：
```c
pNew->xParseCell(pNew, b.apCell[j], &info);
sz = 4 + putVarint(&pCell[4], info.nKey);      /* 写整数 key */
```
KV 改为：
```c
sz = 4 + putVarint32(&pCell[4], info.nKeyLen);
memcpy(&pCell[4+varintLen], info.pKey, info.nKeyLen);
```
其余（`insertCell` 调用、leafCorrection 处理）不变。

### 4.7 balance_quick（btree.c:8007）

原生从 intkey 叶的最大 cell 取整数 key 造 divider。KV 下其 key 提取逻辑
不适用 → **KV 时禁用 balance_quick**，直接走 balance_nonroot。判定：
`!(pPage->pBt->btsFlags & BTS_KV) && pPage->intKeyLeaf`。

> **状态：已实现。** 该项最初漏做，导致一个真实缺陷：当某叶子上有
> 「value ≤ maxLeaf 的条目 + 一个溢出大 value」时走 balance_quick，divider
> 由 intkey 逻辑构造出**垃圾 key**（拷到了 `nKeyLen` 而非 key 字节），
> 查找会越过小 key 返回 NOTFOUND。修法即本节所述：KV 页跳过 fast path、
> 走 balance_nonroot（它由真实 KV key 构造 divider）。回归测试见
> `test/t_big.c`。

### 4.8 saveCursorKey（btree.c:714）

`pCur->curIntKey` 为 0（KV），原走"index 分支"拷贝**整条 payload**（=value，
可能巨大且错误）。新增 KV 分支：只拷贝 key。
判定：`pBt->btsFlags & BTS_KV` → 用 `getCellInfo` 拿 `info.pKey`/`nKeyLen`，
malloc+copy key，`nKey=len`。

### 4.9 sqlite3BtreeDelete（btree.c）内部节点删除

删除内部页上的条目时，原生代码把"前驱子树最大 cell"整体上移替换
divider；若该 cell 来自叶子、携带 value，才需要改写为 divider 形式
（剥掉 value，只留 nKeyLen+key）。

**该分支对 KV 树不可达，因此无需任何 KV 专用改写。** `KvMoveto` 恒下降到
叶子：在内部页命中 key 时执行 `lwr=idx; goto kv_moveto_next_layer` 强制进入
下一层，而非停在内部页（见 §4.4）。所以 KV 游标永远停在叶条目上，
`sqlite3BtreeDelete` 不会进入 `!pPage->leaf` 分支；该内部节点路径只由
legacy intkey 表树使用，保持原样。
（实测确认：对 2000 键树删 1/3、对 6000 键树删 1/2，删除顺序专门制造大量
merge，均未进入该分支，`integrity_check` 全程干净。）

### 4.10 sqlite3BtreeTableMoveto 相关

KV 不用 `TableMoveto`；`btreeMoveto` 的 `pKey==0` 分支仅 intkey 用。保持原样。

### 4.11 移除旧的 index 家族 KV 适配

- `btreelite_compat.c`：`kvCompare`/`kvRecordDecode`/`sqlite3VdbeRecordUnpack`/
  `AllocUnpackedRecord`/`RecordCompare`/`FindCompare` 桩 → 删除。
- `btree.c`：`btreeParseCellPtrIndex` 里的 `BTS_KV` 解析块删除（index 家族
  不再承载 KV）。
- `btreeInt.h`：`CellInfo.nKeyBytes` → `pKey`/`nKeyLen`。

## 5. btreelite API 层（btreelite_api.c）

- `btreelite_put`：直接 `BtreePayload{ pKey=k, nKey=nK, pData=v, nData=nV }`，
  **不做拼接编码**（cell 头由 fillInCell 写）。定位用 KvMoveto。
- `btreelite_get/seek/del`：用 KvMoveto（原始 res 语义）。
- 读取：`btreelite_key` 经 `sqlite3BtreeKvKey()` 拿到 `info.pKey`/`nKeyLen`
  （key 恒本地）；`btreelite_value_size/read/fetch` 分别经
  `sqlite3BtreeKvValueSize/Read/Fetch` 读取 value，跨 overflow 页透明。
  （不需要在 BtCursor 里存 key/value 偏移：KV 访问器内部用 `getCellInfo`
  得到的 `pKey`/`pPayload` 定位。）
- `KV_MAX_KEY` = 255；超限 put 返回错误。

## 6. 验证

- `t_big`（200KB value）→ 全绿（本设计的核心目标）。
- `t_kv` 与 `t_smoke` 不回归。二者均已改用公开 KV API 建树/读写，
  不再走 intkey 路径。

## 7. 兼容性

- 文件魔串已从 `"SQLite format 3"` 改为 `"btreelite fmt 1"`（16 字节含 NUL），
  因此 `sqlite3` CLI 与本库互相拒绝对方文件（`file is not a database`）。
- `KV_MAX_KEY` = 255 已在 API 层强制（`BTREELITE_MAX_KEY` / `BTREELITE_TOOBIG`）；
  超限的 put/get/del 返回错误码 18，而不是写入损坏的 cell。
- 旧 KV 文件（index 家族格式）不向后兼容——项目未发布，无需迁移。
