# KV 存储格式重设计：LEAFDATA 家族 + 字节串 key

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
intKey     = 0;      /* 关键：仍按 "无整数键" 处理，使游标走 pKeyInfo!=0 分支 */
intKeyLeaf = 1;      /* 关键：balance 取 leafData=1，用 key-only divider */
xCellSize  = kvCellSizeLeaf / kvCellSizeInterior;
xParseCell = kvParseCellLeaf / kvParseCellInterior;
maxLocal   = pBt->maxLeaf;
minLocal   = pBt->minLeaf;
```

- `intKey=0` 让 `moveToRoot` 的一致性检查 `(pKeyInfo==0)!=pRoot->intKey` 通过
  （KV 游标 `pKeyInfo!=0`），并让 `sqlite3BtreeInsert` 走 index 分支。
- `intKeyLeaf=1` 让 `balance_nonroot` 取 `leafData=1`，divider 构造走
  "只有一个 key"的分支（btree.c:8459, 8882-8890）。

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
  `nValue`，`minLocal/maxLocal` 用 `minLeaf/maxLeaf`。
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
返回 CORRUPT。非 KV 模式保持原样（intkey 仍可用，用于 t_smoke）。

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
二分: pCell = findCellPastPtr(pPage, idx)
      kvKeyCompare(pPage, pCell, pKey, nKey)   /* 只读 key，零物化 */
```
`kvKeyCompare` 解析 cell 的 key（divider：跳过 4B child；leaf：跳过两个
varint），memcmp + 长度决胜。**key 恒本地，永不触发 overflow / malloc。**

`btreeMoveto()` 在 KV 游标时分派到此函数。

### 4.5 sqlite3BtreeInsert（btree.c:9419）

- index 分支里，若游标为 KV：
  - loc==0 的重定位用 `sqlite3BtreeKvMoveto`（替代 btreeMoveto/IndexMoveto）；
  - **跳过** `btreeOverwriteCell` 的"same key overwrite"优化（其语义是
    index 记录覆盖，对 KV 的 value 不适用）；改用 drop+insert。
- `assert( pPage->intKey || pX->nKey>=0 )` 成立（intKey=0）。

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
不适用 → **KV 时禁用 balance_quick**，直接走 balance_nonroot（性能优化项，
不影响正确性）。判定：`pPage->intKeyLeaf && !KV`。

### 4.8 saveCursorKey（btree.c:714）

`pCur->curIntKey` 为 0（KV），原走"index 分支"拷贝**整条 payload**（=value，
可能巨大且错误）。新增 KV 分支：只拷贝 key。
判定：`pBt->btsFlags & BTS_KV` → 用 `getCellInfo` 拿 `info.pKey`/`nKeyLen`，
malloc+copy key，`nKey=len`。

### 4.9 sqlite3BtreeDelete（btree.c:9841）内部节点删除

删除内部 cell 时，原生把"前驱子树最大 cell"整体上移替换 divider。KV 下
leaf cell 含 value，需重写为 divider 形式（剥掉 value，只留 nKeyLen+key）。
在 `!pPage->leaf` 分支按 KV 改写构造。

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
- 读取：`btreelite_key` 从 `info.pKey/nKeyLen` 直接 `sqlite3BtreePayload` 读
  key 段；`btreelite_value_read` 从 `nKey` 之后读 value（偏移 =
  headerSize + nKeyLen；用 `sqlite3BtreePayload` 的绝对 offset）。
  **需要 KvParse 暴露给游标的 key/value 偏移** → 存于 BtCursor：
  `u32 kvKeyOff; u16 kvKeyLen; u32 kvValOff;`。
- `KV_MAX_KEY` = 255；超限 put 返回错误。

## 6. 验证

- `t_big`（200KB value）→ 全绿（本设计的核心目标）。
- `t_kv`（1529 checks）与 `t_smoke`（20 checks）不回归。
- `t_smoke` 仍用 intkey 树（非 KV 模式），验证"非 KV 路径未被破坏"。

## 7. 兼容性

- 文件魔串已从 `"SQLite format 3"` 改为 `"btreelite fmt 1"`（16 字节含 NUL），
  因此 `sqlite3` CLI 与本库互相拒绝对方文件（`file is not a database`）。
- `KV_MAX_KEY` = 255 已在 API 层强制（`BTREELITE_MAX_KEY` / `BTREELITE_TOOBIG`）；
  超限的 put/get/del 返回错误码 18，而不是写入损坏的 cell。
- 旧 KV 文件（index 家族格式）不向后兼容——项目未发布，无需迁移。
