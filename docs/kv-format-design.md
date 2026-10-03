# KV storage format: LEAFDATA page family with byte-string keys

> This is the English edition.  The 中文 original is
> [KV 存储格式设计](kv-format-design-cn.md).

## 1. Motivation

The original implementation packed a KV record
(`[varint nKeyBytes][key][value]`) into the cell payload of an **index
btree** (ZERODATA, page types 0x0a/0x02).  An index btree is designed on
the assumption that "records are usually small":

- The comparator interface `RecordCompare(int nKey, const void *pKey, ...)`
  requires the **whole record to be in contiguous memory**.  A record that
  spans overflow pages can only be compared after it is materialized with
  `malloc` + `accessPayload` (btree.c:6215-6228).  Every overflowing cell
  encountered during the binary search is materialized once.
- A divider cell carries the **entire payload, value included**; on a split
  a large value is promoted into the parent page, pushing the per-page
  scratch buffer in `balance_nonroot` to its limit.

A **table btree** (LEAFDATA, 0x0d/0x05), by contrast, has exactly the three
properties a KV store wants:

1. the key always lives locally (a varint or fixed field in the cell
   header) and never takes part in overflow splitting;
2. comparison reads only the key and **never touches the data**
   (`sqlite3BtreeTableMoveto` just calls `getVarint`);
3. a divider carries **only the key** (`btreeParseCellPtrNoPayload`,
   `nPayload=0`), so interior pages are never polluted by large data and
   splits follow the `leafData` key-only divider path.

**Conclusion**: keep the table-btree page format and divider semantics,
and replace only the "64-bit integer key" with a "byte-string key".  Large
values then take the native overflow path, and lookups materialize nothing.

## 2. Page types and flag reuse

The existing page-type bytes are reused, with new meanings under the
`BTS_KV` flag:

| Page-type byte (original meaning) | Meaning under KV mode |
|---|---|
| `0x0d` LEAFDATA\|INTKEY\|LEAF | **KV leaf page** |
| `0x05` LEAFDATA\|INTKEY | **KV interior page** |
| `0x0a` / `0x02` (the index family) | `CORRUPT` under KV mode (unused) |

Under `BTS_KV`, `decodeFlags` sets a KV page up as:

```c
intKey     = 1;      /* Reuse the "integer-key table" cursor branch
                        (pKeyInfo==0); the only difference is how the key
                        is encoded in the cell header. */
intKeyLeaf = 1;      /* balance() sees leafData=1 and builds key-only dividers. */
xCellSize  = kvCellSizeLeaf / kvCellSizeInterior;
xParseCell = kvParseCellLeaf / kvParseCellInterior;
maxLocal   = pBt->maxLeaf - KV_CELL_HDR_OVERAGE;   /* leaf pages, see §3 */
minLocal   = pBt->minLeaf;
```

- `intKey=1` lets the `moveToRoot` consistency check
  `(pKeyInfo==0)!=pRoot->intKey` pass (a KV cursor has `pKeyInfo==0`, the
  same shape as a table cursor) and routes `sqlite3BtreeInsert` down the
  "no KeyInfo" branch.  The byte-string key differs only in the cell
  header, where `nKeyLen` + key bytes replace the varint-encoded i64.
- `intKeyLeaf=1` makes `balance_nonroot` take `leafData=1` and build the
  divider through the "single key" branch.  It also makes KV pages skip the
  `balance_quick` fast path, whose divider extraction is intkey-specific
  (see §4.7).

## 3. Cell layout

```
KV leaf cell:
    [varint nValue] [varint nKeyLen] [key bytes] [value bytes] [4B ovfl?]
    (only the value overflows; the key is always local)

KV interior (divider) cell:
    [4B child] [varint nKeyLen] [key bytes]
    (isomorphic to the native intkey divider, only the key is a byte string)
```

- `nValue` is the value length (may be 0); `nKeyLen` <= `KV_MAX_KEY`
  (255 by default).
- Overflow splitting applies **only to the value**: the `nPayload`
  (local + overflow) notion inside KV means `nValue`, and `minLocal` is
  `minLeaf`.
- **The leaf `maxLocal` is not the native `maxLeaf`**: the native budget
  `maxLeaf = usableSize-35` only accounts for a 14-byte rowid cell header
  (payload varint + rowid varint), while a KV cell header can be up to
  262 bytes (2 varints + a 255-byte key).  Used as-is, a maximal cell
  (header + local value + 4-byte overflow pointer) could exceed what an
  empty page holds, leaving `balance_nonroot` no page to place it on and
  producing a spurious `SQLITE_CORRUPT` for long keys with boundary-size
  values (regression coverage in t_big).  The leaf `maxLocal` is therefore
  reduced by `KV_CELL_HDR_OVERAGE = KV_CELL_MAX_HDR(262) - 14 = 248` bytes,
  preserving the "a maximal cell always fits on an empty page" invariant
  that balance relies on.
- The overflow loop in `fillInCell` is therefore reused verbatim; only the
  bytes being spilled are the value.

### CellInfo extension

```c
struct CellInfo {
  ... existing fields ...
  u8  *pKey;      /* KV: start of the key bytes */
  u32  nKeyLen;   /* KV: key length */
};
```

(The old `nKeyBytes` field is replaced by `pKey` + `nKeyLen`.)

## 4. Functions that changed (item by item)

### 4.1 New KV parse / size variants (btree.c)

```
kvParseCellLeaf(pPage, pCell, pInfo)
    read the nValue varint -> read the nKeyLen varint -> pKey = pIter
    -> decide nLocal the way btreeParseCellPtr does (value only)
    -> pInfo->nPayload = nValue   (the value is the payload)
    -> pInfo->nKeyLen = nKeyLen; pInfo->pKey = pIter

kvParseCellInterior(pPage, pCell, pInfo)
    pIter = pCell+4; read the nKeyLen varint; pKey = pIter
    nSize = 4 + varintLen(nKeyLen) + nKeyLen
    nPayload = 0; nLocal = 0

kvCellSizeLeaf / kvCellSizeInterior
    consistent with the matching parse, local cell size only (no overflow).
```

### 4.2 decodeFlags

Under `BTS_KV`, `0x0d` becomes a KV leaf and `0x05` a KV interior; the index
family (0x0a/0x02) returns `CORRUPT`.  Non-KV mode is unchanged: the intkey
machinery survives in the source but is not exposed by btreelite.

### 4.3 fillInCell

New KV branch (tested with `pPage->pBt->btsFlags & BTS_KV`):

```
nHeader = childPtrSize (0 for a leaf)
nHeader += putVarint32(&pCell[nHeader], pX->nData + pX->nZero)  /* nValue */
nHeader += putVarint32(&pCell[nHeader], pX->nKey)               /* nKeyLen */
memcpy the key
then reuse the native payload-writing loop (writes the value, spilling
through allocateBtreePage when needed)
```

In other words, the native intkey branch's "write the integer key varint"
becomes "write nKeyLen + key bytes"; everything else is untouched.

### 4.4 New sqlite3BtreeKvMoveto (btree.c)

Mirrors the structure of `sqlite3BtreeTableMoveto`, but compares byte
strings:

```
bisect: kvKeyCompareCell(pPage, idx, pKey, nKey)   /* key only, zero materialization */
```

`kvKeyCompareCell` parses the key from the **full cell start**
`findCell(pPage,idx)` (a leaf skips its two varints; a divider's 4-byte
child pointer is skipped inside `kvParseCellInterior`) and compares with
`memcmp`, breaking ties by length.  **The key is always local, so an
overflow page or malloc is never touched.**  (An earlier version used
`findCellPastPtr`, which double-skipped the interior child pointer and
misaligned interior-page comparisons; it now uses `findCell`.)

`btreeMoveto()` dispatches to this function for KV cursors.

### 4.5 sqlite3BtreeInsert

- In the KV branch: the `loc==0` relocation uses `sqlite3BtreeKvMoveto`,
  and the "same key overwrite" optimization of `btreeOverwriteCell` is
  applied only when both the key length and the payload size match;
  otherwise the cell is dropped and re-inserted.

### 4.6 balance_nonroot

The `leafData=1` divider-construction branch is the native one:

```c
pNew->xParseCell(pNew, b.apCell[j], &info);
```

which for KV is replaced by:

```c
sz = 4 + putVarint32(&pCell[4], info.nKeyLen);
memcpy(&pCell[4+varintLen], info.pKey, info.nKeyLen);
```

Everything else (`insertCell`, the `leafCorrection` handling) is unchanged.

### 4.7 balance_quick

The native path derives the divider from the largest cell of an intkey leaf
by copying its integer key varint; that logic does not apply to KV.  **KV
pages therefore disable `balance_quick`** and route to `balance_nonroot`.
The condition is `!(pPage->pBt->btsFlags & BTS_KV) && pPage->intKeyLeaf`.

> **Status: implemented.**  This item was originally missed and caused a
> real defect: when a leaf held a value `<= maxLeaf` together with one
> overflowing large value, `balance_quick` built the divider from the
> intkey varint and copied `nKeyLen` instead of the key bytes, so the
> divider carried a garbage key and lookups descended past the small key
> (returning `NOTFOUND`).  The fix is the one described here: KV pages skip
> the fast path and go through `balance_nonroot`, which builds the divider
> from the real KV key.  Regression coverage is in `test/t_big.c`.

### 4.8 saveCursorKey

For a KV cursor, the original "index branch" copied the **entire payload**
(the value, potentially huge and wrong).  A KV branch copies only the key:
`getCellInfo` yields `info.pKey`/`nKeyLen`, the key is malloc'd and copied,
and `nKey` is its length.

### 4.9 sqlite3BtreeDelete: interior-node deletion

When an entry on an interior page is deleted, the native code moves the
largest cell of the predecessor subtree up to replace the divider, and a
leaf cell that carried a value would have to be rewritten into divider
form (value stripped, `nKeyLen` + key kept).

**This branch is unreachable for a KV btree**, so no KV-specific rewrite is
needed.  `KvMoveto` always descends to a leaf: when it matches a key on an
interior page it sets `lwr = idx` and jumps to the next layer rather than
stopping there (§4.4), so a KV cursor is always parked on a leaf entry and
`sqlite3BtreeDelete` never sees `!pPage->leaf`.  The interior-node code
path is exercised only by the legacy intkey table btree and is left
unchanged.  (Confirmed empirically: deleting a third of a 2000-key tree
and half of a 6000-key tree, in orders chosen to force many merges, never
entered the branch, with `integrity_check` clean throughout.)

### 4.10 sqlite3BtreeTableMoveto

KV does not use `TableMoveto`; the `pKey==0` branch of `btreeMoveto` is for
the legacy intkey path.  Unchanged.

### 4.11 Removing the old index-family KV adaptation

- `btreelite_compat.c`: the `kvCompare` / `kvRecordDecode` /
  `sqlite3VdbeRecordUnpack` / `AllocUnpackedRecord` / `RecordCompare` /
  `FindCompare` stubs were deleted (see the intkey-only code removal).
- `btree.c`: the `BTS_KV` parsing block inside `btreeParseCellPtrIndex` was
  removed (the index family no longer carries KV).
- `btreeInt.h`: `CellInfo.nKeyBytes` became `pKey` / `nKeyLen`.

## 5. The btreelite API layer (btreelite_api.c)

- `btreelite_put`: builds `BtreePayload{ pKey=k, nKey=nK, pData=v,
  nData=nV }` directly, with **no concatenation encoding** (the cell header
  is written by `fillInCell`).  Positioning uses `KvMoveto`.
- `btreelite_get` / `seek` / `del`: use `KvMoveto` and its native `res`
  semantics.
- Reading: `btreelite_key` reads the key via `sqlite3BtreeKvKey()`
  (`info.pKey`/`nKeyLen`, always local); `btreelite_value_size` / `_read` /
  `_fetch` go through `sqlite3BtreeKvValueSize` / `KvValueRead` /
  `KvValueFetch`, which read across overflow pages transparently.  (No
  per-cursor key/value offset fields are needed: the KV accessors locate
  them from the `pKey`/`pPayload` produced by `getCellInfo`.)
- `KV_MAX_KEY` = 255; an over-long key is rejected at the API layer
  (`BTREELITE_MAX_KEY` / `BTREELITE_TOOBIG`), and put/get/del return
  `BTREELITE_TOOBIG` (18) instead of writing a corrupt cell.

## 6. Verification

- `t_big` (a 200 KB value): green — the core goal of this design.
- `t_kv` and `t_smoke` do not regress; both drive the public KV API now and
  no longer exercise the legacy intkey path.
- Lookups never materialize a record and never read an overflow page
  (asserted by construction; keys are always local).

## 7. Compatibility

- The file magic string is now `"btreelite fmt 1"` (16 bytes including the
  NUL) instead of `"SQLite format 3"`, so the `sqlite3` CLI and this
  library reject each other's files (`file is not a database`).
- `KV_MAX_KEY` = 255 is enforced at the API layer
  (`BTREELITE_MAX_KEY` / `BTREELITE_TOOBIG`); an over-long key gets error
  code 18 rather than a corrupt cell.
- Old KV files (the index-family format) are not backward compatible — the
  project is unreleased, so no migration is provided.
