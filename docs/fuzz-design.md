# btreelite Fuzz 测试设计

> 设计文档（未实现）。目标读者是准备动手实现的作者本人，以及未来读 fuzz 的贡献者。
> 本文与 docs/kv-format-design.md（格式）、docs/design.md（journal/基准）同级。
> 设计基线 = 当前 main（t_api 144 版本之后）。

---

## 0. 总览

### 0.1 我们要证明什么

| 目标 | 措辞 | 归属 target |
|---|---|---|
| API 状态机任意使用下不崩、不挂、不 UB、不泄漏 | 任意调用序列 × 任意合法参数 | **A** |
| 存进去的东西永远是存进去的东西 | 内存模型对照 + 全量扫描 + integrity_check | A |
| 敌意文件不是 UB | 损坏的 db/-wal/-shm 重开 → CORRUPT 或行为正确，绝不越界/死环 | B |
| OOM 不是 UB 且已提交数据存活 | 确定性注入分配失败 | C |
| 并发不破坏格式 | 双 handle 争用、rc 在允许集 | E |
| （白盒）balance/SAVEPOSITION 深路径 | 直接驱动 BtCursor | D |

### 0.2 非目标

- 不 fuzz 解析 SQL/URI——btreelite 只有一层 C API，没有文法表面；
- 不追求"语义级漏洞"（逻辑多删一个 key 之类）以外的对抗性证明——模型对照就是语义级；
- 不做 kernel/多机分布式场景。

### 0.3 总体架构：双引擎

```text
                        ┌────────────── libFuzzer（覆盖率导向，发现新路径）┐
   corpus/（动作流）     │                                                │
   corpus/files/（敌意库）│  fuzz/target_*.c   harness（每输入一次调用序列） │
                        │  build: clang -fsanitize=fuzzer …              │
                        └────────────────────────────────────────────────┘
                                        │ 发现 crasher/repro
                                        ▼
                        ┌────── fuzz_replay（确定性回放 runner）────────────┐
                        │  同一 harness，main() 逐文件回放 corpus/           │
                        │  build: 无任何 fuzz 依赖，gcc 也能编              │
                        │  make fuzz → CI 每次跑（秒级）                    │
                        └─────────────────────────────────────────────────┘
```

铁律（两条，都决定 harness 怎么写）：

1. **一切随机来自输入流，不许 `rand()/time()/pid()`**——否则 libFuzzer 的覆盖导向失效；
   输入流本身就携带派生随机源（每条流的前 8 字节定义一个 xorshift 种子）。
2. **每个输入必须完整自净**——`LLVMFuzzerTestOneInput` 返回时句柄全关、临时文件全删
   （否则持久模式下把上一次迭代的垃圾喂给下一次）。

引擎选择：本机 clang（llvm@22）自带 libFuzzer（已验证 `-fsanitize=fuzzer` 可链接）；
Linux/gcc 场景由 fuzz_replay 兜底，保证回归语料永远可跑。

---

## 1. Attack-surface 清单（fuzz 要打在哪儿）

| # | 表面 | 代码位置 | 历史 bug 记录（都是真实发生过） |
|---|---|---|---|
| S1 | cell 解析/测长（敌意页字节） | `kvParseCellLeaf/Interior`、`kvCellSizeLeaf/Interior` | 解析器取址曾把 child 指针计入 key 区 |
| S2 | key 比较/二分 | `kvKeyCompareCell`、`sqlite3BtreeKvMoveto` | findCell 取址 bug → get 大面积失败 |
| S3 | 写入/覆盖/分隔符重建 | `fillInCell`(KV 分支)、`overwriteCell 类快路径`、`balance_nonroot` | balance_quick 垃圾分隔符；`putVarint64` 缺 `&=0x7f` |
| S4 | 游标恢复/SAVEPOSITION | `saveCursorKey`、KvMoveto 恢复路径 | NULL key memcmp → SIGSEGV（t_savepos 已钉） |
| S5 | 溢出链读写 | `accessPayload`、`getOverflowPage`、`freePageChain` | 尚无已知 bug——**没有针对它的回归**，是最大空白 |
| S6 | 事务/保存点栈 | `env.nSavepoint` ↔ pager savepoint 映射 | 镜像编号 bug；integrity_check 代提交 bug |
| S7 | journal 模式切换 | `btreelite_journal_mode`、`SetVersion`、`OpenWal/CloseWal` | 零页 checkpoint 边界（上游 forum fd0f19d2） |
| S8 | WAL 恢复/checkpoint | `walIndexRecover`（帧校验和）、`walCheckpoint`（可中断轮询） | autocheckpoint `aDb[0].pBt` NULL（静默空转） |
| S9 | shm 头解析 | `walIndexTryHdr`/`WalIndexHdr` 效验、aReadMark | 无回归 |
| S10 | hot-journal 回放 | pager `pager_playback`（校验和） | super-journal 删除面——删除后需重新证明等价 |
| S11 | 文件名/路径（上游假设） | pager 文件名块、URI 参数遍历 | 复合文件名假设 → 越界读（b7b98e0） |
| S12 | OOM 传播 | `mallocWithAlarm`→`OomFault`、`mallocFailed` 粘滞 | **btreelite 无 `sqlite3OomClear` 调用点**（见 §5） |

规律：已知 bug 全部落在"上游隐含假设被拔掉 SQL 层之后"的缝里。fuzz 设计的核心
就是把每个缝变成一个可被自动打穿的 oracle。

---

## 2. Target A：状态化 API fuzz（中心靶）

### 2.1 形态

持久模式 libFuzzer target：每条输入 = 一个**动作流**（action stream），解码器把它
确定性地解释为一段 btreelite API 调用序列，全程对照**内存模型**。

```text
输入字节流 = [8B xorshift 种子][动作记录 …]（≤20000 条，防死循环）
     每条动作记录: [1B opcode][操作数 varint 们]
     非法索引/越界 handle id = 吞掉为 no-op（仍消耗覆盖——守卫路径也要测）
```

### 2.2 opcode 编码规范（实现照这个写）

| op | 名 | 操作数（varint） | 语义要点 |
|---|---|---|---|
| 1 | open | h, flavor(0=:memory: / 1=file) | file 走 `$TMPDIR/bt-fuzz-<pid>-<h>.db`，迭代末 delete |
| 2 | close | h | 悬空 handle/游标访问 → 计入"必须拒绝或已关" |
| 3 | begin | h, wrflag(0/1/2) | wrflag=2 拼进 EXCLUSIVE 路径 |
| 4 | commit | h | commit 后跑模型一致性检查点（§2.4） |
| 5 | rollback | h | 回滚 → 模型回退 + 游标失效语义检查 |
| 6 | savepoint | h, op(1/2), i | i=0 特别口径（RELEASE 0 = commit 等） |
| 7 | create_tree | h | root 号进"live roots"集合 |
| 8 | clear_tree | h, root(≤live) | 清空的树回退为空模型 |
| 9 | cur_open | h, root, wrflag | 游标对象进池（≤16 只） |
| 10 | cur_close | c | — |
| 11 | put | c, keylen, keygen, vallen, valgen | — |
| 12 | get | c, key | rc 契约 + **读回值与模型比对** |
| 13 | del | c, key | — |
| 14 | seek | c, key | 落点必须 = 模型中首个 ≥ key |
| 15/16 | first/last | c | — |
| 17/18 | next/prev | c | 走到头返回 DONE 契约 |
| 19 | val_read | c, off, amt | 越界拒绝 / 内容与模型比对 |
| 20 | val_write | c, off, amt, bytes | 不能增长 value 的契约必须守住 |
| 21 | checkpoint | mode(0..3) | pnLog/pnCkpt 合理范围 |
| 22 | journal_mode | h, mode(4/5) | 必须在事务外才可切换；切换失败回落规则 |
| 23 | synchronous | h, lvl(0..2) | 返回旧值 |
| 24 | autocheckpoint | h, nf | 0/负 = 关 |
| 25 | set_interrupt | h, on | on=1 后下一条 integrity → INTERRUPT |
| 26 | integrity | h, root(live 或 0) | 周期性全量 oracle（§2.4） |

### 2.3 生成器（长度分布是重头戏）

key 长度专门过采样阈值附近：`{0,1,2,127,128,129,254,255}` 占 40%，其余
[0,255] 均匀。value 长度从固定档位抽（档位即"结构"）：

```text
{0, 1, 10, 488, 489, 490, 3812, 3813, 3814, 4092, 8185, 65536} —— 前 9 档占 85%
（489=minLocal 溢出门槛、3813=maxLocal 本地门槛、4092=溢出页满容量、8185=链长 3）
内容 = 种子驱动字节生成（非零率可调，含全 0 / 全 0xff 之类可压缩形态）
```

### 2.4 Oracle（三档，都要有）

1. **每 op 即时**：返回码 ∈ 该 op 契约集（表格进 harness 常量）；`get/val_read`
   内容必须逐字节等于模型；
2. **每 commit 即时**：前向扫描 == 模型中位序（≤512 键时全扫，否则抽样 64 键点查）；
3. **终态全量**：每 input 结束前对每个 live root 跑 `integrity_check`（0 错误）
   + `iRoot=0` 形式（检查 freelist/页 1）+ 后向扫描 == 模型逆序。

### 2.5 模型数据结构（C，单文件）

每 (handle, root) 一个**有序数组** `{key[255], val[1MB cap], len}`，bsearch +
memmove。fuzz 规模（键 ≤2 万、值 ≤1MB cap）下 O(n) 完全可承受，不值得造 AVL。
`savepoint` 时深拷贝当前模型（savepoint 深度 ≤3），ROLLBACK TO/RELEASE 即可
对模型做同样的"回到/合并"——**这就是 S6 的 oracle**。

### 2.6 rc 契约表（:memory: 句柄，强口径）

```text
:memory: 单连接无并发 → 没有 BUSY/IOERR 出口 ⇒ 契约收严：
  put   必须 OK（除非守卫拒绝/TOOBIG）
  get   OK | NOTFOUND（模型说了算）
  scan  OK | DONE
任何越出契约的 rc ⇒ 立即 abort（libFuzzer 存 repro）
文件句柄（flavor=1）：允许集放宽 {OK, BUSY, LOCKED, BUSY_SNAPSHOT, IOERR}，
模型口径不变——IOERR 后模型视为"未知"，下次 commit 检查点重建。
```

### 2.7 骨架（可编译级别的 sketch）

```c
#include "btreelite.h"

static sqlite3_uint64 drv_seed;   /* = xorshift from input */

static u32 drv_rand(void){…}              /* 种子驱动的均匀数 */
static int  drv_key(u8 *buf){…}           /* 长度分布 + 生成 */
static void modelPut(Model*, const u8*,int, const u8*,int);
static int  modelGet(Model*, const u8*,int, u8*[/*val*/]);  /* 返回 found */

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size){
  /* 解码循环 */
  for(;;){
    op = drv_next_byte(Data,&Size);
    switch(op){
      case 11: { /* put */
        uint8_t k[260], v[260]; int nK=drv_key(k), nV=drv_val(v);
        int rc = cur_ok? btreelite_put(cur,k,nK,v,nV) : BTREELITE_ERROR;
        CONTRACT(rc);            /* rc 契约集判定，不符 → abort() */
        if( rc==BTREELITE_OK ) modelPut(&model,k,nK,v,nV);
        break; }
      case 4: { /* commit */
        int rc = btreelite_commit(db);
        CONTRACT(rc);
        checkScan(&model, cur);  /* 前向 == 模型 */
        break; }
      …
    }
    if( Size==0 && op==0 ) break;
  }
  finalizeIntegrity();           /* integrity_check(=0) + 终态扫描 */
  cleanupAll();                  /* 句柄/游标/临时文件 */
  return 0;
}
```

（实现时这是 4~6 个文件：`fuzz/drv.c` 编解码与分布、`fuzz/model.c`、
`fuzz/target_api.c`、`fuzz/target_file.c`、`fuzz/fuzz_replay.c`。）

---

## 3. Target B：敌意文件（S5/S8/S9/S10/S11 的唯一入口）

### 3.1 语料生成（`make fuzz-seeds`，不依赖 libFuzzer）

用公共 API 造**特征库文件**快照（每个都是 db/-wal/-shm 三件套整体拷贝）：

```text
seed 类别（每类 2~4 个文件）:
  empty         零字节 / 只有页 1
  onetree       1 棵树 64 键，全短值
  deep          3000 键单树（≥6 层）
  bigval        12 键 × 900B value（2~3 页溢出链）
  freelist      5000 写 3000 删后 commit（充满可复用页）
  halfcommit    写事务进行中的三件套（模拟 kill -9）——需要 harness 提供快照钩子
  walheavy      200 帧未 checkpoint 的 WAL + 正确 shm
  walsalt       同上但 shm 的 aSalt 故意与 WAL 头不匹配（必走 recovery 慢路径）
```

生成器是**确定性**的（固定种子），语料可复现；小文件直接检入 git
（每个 <64KB），deep/bigval 用脚本现场再生，不检入。

### 3.2 变异器（按输入流选择 + 权重）

| 变异器 | 权重 | 打击点 |
|---|---|---|
| 随机字节翻转（加权：页类型字节@100、nCell@102、内容区偏移@104、cell 指针阵、cell 体、WAL 帧头/校验和、shm 头 48B） | 30% | S1/S9 |
| 截断（文件任意字节处砍断，三件套分别随机选择砍谁） | 15% | S10/S8 半截回放 |
| 溢出链改指（把某 cell 的 ovfl 页号改成任意已有页 → 自环/成环） | 10% | **S5 死环检查**（libFuzzer `-timeout=5` 兜底） |
| child 指针改写（页号合法但指错/循环指向祖先） | 10% | S3/S5 |
| WAL 帧手术（复制/删除/换序帧、改 frame checksum、改 mxFrame/aReadMark） | 15% | S8/S9 |
| 页类型字节洗染（0d↔05↔0a↔02↔垃圾） | 10% | S1 |
| 结构性：复制整页、页对调、magic 擦除、`version=2↔1` | 10% | S7 复杂化 |

配 `fuzz/file.dict`：magic 十六进制、页类型全值、varint 边界编码（0x7f/0x80…）、
WAL 帧头 magic（37 7f 06 82 / 37 7f 06 83）、shm 头魔数等。

### 3.3 Oracle（敌意文件的通过/失败矩阵）

```text
API 重新打开损坏文件组:（不得 crash / hang / ASan 报 / 泄漏；hang 由 timeout=5 判死）

  open 失败（CANTOPEN/CORRUPT/BUSY…）         ✓ 通过
  open 成功:
     integrity_check(root) 必须给出报告或 0 错误，且**不返回非 CORUPT 的越界后果**
     首条 first/get：允许 {OK, CORRUPT, NOTFOUND, DONE} —— 任何其它 rc = 失败
     内容等值比对：仅在"变异命中无害字节"时成立——不强制；
     但「integrity_check 报 0 错误 且 后续 scan 全绿」的情况，必须与
     损伤前模型一致 —— 这把"假接受"也钉住了。
```

（关键设计点：**不**承诺"损坏必报 CORRUPT"——上游也不承诺；承诺的是**UB 不存在**。)

### 3.4 与 Target A 的组合（进阶，P4）

变异 → open 成功 → 继续状态化 API 操作（模型=未知，只查 rc 合法集 + integrity 不崩）。
libFuzzer 天然会把这两段串成一次输入，Target B 的 harness 内嵌 Target A 的解码器即可。

---

## 4. Target C：OOM 注入 fuzz（S12）

### 4.1 实现机制（已核实）

btree 的分配走 `sqlite3GlobalConfig.m`（mem1 默认实现；`sqlite3_config` 未编译进来，
mem0.c 不在构建里）。测试驱动 TU 可以**在 `sqlite3_initialize()` 之前**直接写
`sqlite3GlobalConfig.m`（`sqliteInt.h` 可见该结构），装一个"第 i 号分配失败"的
确定性分配器：

```
fuzz/target_oom.c:
  globalConfig.m.xMalloc = faultMalloc;   /* 读全局计数器，命中 i ⇒ 返回 NULL 一次 */
  btreelite_initialize() 之前装；动作流 = [触发时刻列表][Target A 的动作流]
```

`fault.c` 现有的 benign-malloc 钩子保留可用（上游内部在 hash 扩容等处包 benign
窗口），fuzz 里不额外利用它——**上游怎么包的，btree 内部原样**，这正是要测的。

### 4.2 ⚠️ 实现前必须先定夺的语义缺口

**`sqlite3OomClear` 在 btreelite 没有任何调用点**（上游是 `sqlite3ApiExit` 在每个
公共 API 出口清除）。当前行为：某连接一旦 NOMEM，`mallocFailed` 粘滞，该连接基本
报废（可 close + 重开，但连接内不恢复）。两个选法（影响 fuzz oracle 怎么写）：

- **选 1（推荐）**：移植 OOM 清理——commit/rollback/各查询出口在 rc 传播时
  `OomClear`（或至少 rollback/close 前清理 lookaside 禁态）。oracle 变成
  "NOMEM 之后连接可继续用或至少可干净关闭"；
- **选 2**：维持粘滞，把"NOMEM 后该连接只允许 close"写进头文件契约，fuzz oracle
  按此建模（连接在 OOM 后进入"僵尸态"，后续动作一律吞掉）。

无论选哪个，oracle 的**不变式都在**：OOM 期间已提交的数据（模型快照）
在 close+reopen 后逐字节存在。

### 4.3 Oracle

```text
动作 = Target A 全集 + 每条动作携带"注入概率表"（来自输入流）

  rc ∈ {契约集 ∪ NOMEM}；NOMEM 之后：选1 → 继续按 A 跑；选2 → 强制 close
  终态: close+reopen 全句柄 → committed 模型逐字节在（包括 OOM 发生前最后一次 commit）
  integrity_check 必须 0 错误（OOM 不许留半截树）
```

---

## 5. Target D：白盒深层（低优先，可选）

t_savepos 已建的模式推广：绕过公共 API，用内部头直接驱动 `BtCursor` 的
`BTREE_SAVEPOSITION`、biasRight 组合、`KvMoveto(0,·)` 边界、
`KvValueRead(整链)`。**只有当 fuzz A/B 命中了公开面够不到的区域时才立项**——
现在公开 API 已能触达绝大多数路径，此 target 留作覆盖率缺口填补工具。

---

## 6. Target E：并发争用（P2，小而便宜）

```text
  双 handle 同一文件（WAL 必开）：
    handle 1 一直写单调 key；handle 2 读 + 偶尔写 + checkpoint(PASSIVE/FULL)
    oracle：rc ∈ {OK, BUSY, LOCKED, BUSY_SNAPSHOT, IOERR}
            终局：写 handle 最后一次 commit 后，读 handle 重新 open 必见全量
            integrity_check = 0
  （多线程同 handle = 契约禁区，fuzz 不覆盖；那是未定义）
  （跨进程争用已有 t_proc 钉着；E 是它的持续 fuzz 化，非必须先行）
```

### 6.1 进程级 kill fuzz（并入 B 的 harness 变体）

写循环脚本化 kill -9（t_dur 的持续化）：每轮用**不同 kill 时机**（commit 前、
checkpoint 中、SetVersion 中、CloseWal 中——用 set_interrupt/长 value 控制
kill 窗口宽度）。oracle 同 t_dur（前缀完整）+ 累积轮数计数。

---

## 7. 交付物布局与构建

```text
fuzz/
  drv.c / drv.h          动作流解码 + 分布 + xorshift（两个 target 共用）
  model.c / model.h      内存模型 + 深拷贝快照（savepoint/committed）
  target_api.c           Target A 主 harness（libFuzzer 入口 = 同一个 LLVMFuzzerTestOneInput）
  target_file.c          Target B harness（语料 = db/wal/shm 三件套 tar 或目录约定）
  target_oom.c           Target C harness
  fuzz_replay.c          确定性回放 main()（读 corpus 目录，逐文件调 OneInput）
  seeds/                 Target A 动作流种子（检入 git，都是 <2KB 的字节流）
  file.dict              Target B 字典
  corpus/                libFuzzer 运行时动态生长（不检入；CI 内 cache 传递）
Makefile（新增）:
  fuzz-seeds     确定性再生成 B 的语料文件
  fuzz           无依赖回放（= fuzz_replay 跑全部 seeds；并入 CI/测试链的入口）
  fuzz-libfuzzer 编 fuzzer 版（-fsanitize=fuzzer,address,undefined）
  fuzz-fuzz      实际 fuzz 运行封装（-jobs=N -max_total_time…）
```

编译 flag 固定：`-O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer`
（replay runner 只用 `-fsanitize=address,undefined`）。语料回放纳入 `make test`
链（秒级），libFuzzer 长跑单独跑。

---

## 8. 崩溃分类与 triage 流程

```text
  crasher 落地
     │ libFuzzer 自动最小化（-minimize_crash=1）→ corpus/repro/<slug>
     ▼
  分类（按先捕获的报告类型）:
     heap-buffer-overflow / use-after-free  → 定位解析/游标层（S1/S2/S5）
     UBSan 报告                             → 同上 + 变量类型问题
     timeout                                → 溢出链/child 环（S5/S3）
     Leak                                   → 错误路径资源清理
     integrity FAIL（非崩溃）               → 语义 bug，走 Target A 模型链路定位
     ▼
  处置：API 层可达 → 抽成 t_* 检查；不可达 → 白盒或保留 repro + 注释
     ▼
  修复后：原 repro 进 seeds（永久回归），fuzz 重启继续
```

---

## 9. 覆盖率与推进计划

**测量**：`-fsanitize-coverage=edge,indirect-calls` + `%SANITIZER_COV` 报告，
聚焦函数白名单（§1 S1–S10 全部函数名）。基线：当前 t_* 体系对 KV 主路径覆盖已高
——fuzz 的边际价值集中在 S5（溢出链）、S8/S9（WAL/shm）、S6（保存点栈）、S12（OOM）。

**阶段**（每阶段以"连续 N 小时无新 crasher + 覆盖率不再涨"为停）：

| 阶段 | target | 依赖 | 预估 |
|---|---|---|---|
| P0 | A（:memory:） | 骨架 + 模型 | 1 天（含语料/契约表） |
| P1 | A（file flavor）+ E | — | 0.5 天 |
| P2 | B（敌意文件）+ seeds | seed 生成器 | 1 天 |
| P3 | C（OOM） | §4.2 语义先定 | 0.5 天 |
| P4 | A+B 组合、D（白盒缺口） | — | 视情况 |

**CI 约定**：`make test`（含 seeds 回放，秒级）每次提交跑；libFuzzer 长跑
（`-max_total_time=3600 -jobs=8`）夜间跑。

---

## 10. 设计中确认过的三个决策点（实现者进场前读）

> **附录（P0 已交付后的修正）**——Target A 实现期沉淀的契约事实，已落进
> include/btreelite.h 与对应套件，harness 按以下口径建模：
>
> 1. `btreelite_cursor_open` 现在要求句柄上已开事务（读或写），否则
>    `BTREELITE_ERROR`（此前是纯未定义区；新增 API 守卫，t_api 148 checks）。
> 2. memdb 的 `btreelite_journal_mode` **永远返回 4**：memdb 折叠在事务守卫之前、
>    "请求当前模式"早退——事务内拒切换只在文件库可达（Target B）。
> 3. checkpoint 在事务内返回 **6（SQLITE_LOCKED）**，无事务返回 OK；帧数指针不动。
> 4. `btreelite_set_interrupt` 只能打开、不能清除（每粘滞句柄）。
> 5. **未提交事务里 create_tree 的根页号随回滚消失**（文件截断、page_count 回落），
>    复用旧页号 = CORRUPT；头文件已写契约，t_trees 96 checks 有两分支回归
>    （死根不可用 / 提交过的树根在后续回滚中存活）。
> 6. `btreelite_close` 现在自动关闭该句柄上残余的游标（API 层维护游标链表），
>    逆序关闭不再悬挂；头文件写明"建议仍显式关闭"。
>
> P0 交付物：`fuzz/drv.{h,c}`（动作流解码）、`fuzz/model.{h,c}`（内存模型+快照）、
> `fuzz/target_api.c`（harness，四句柄 ×26 操作 ×三档 oracle）、`fuzz/mkseeds.c`
> （生成 24 条检入种子）、`fuzz/replay.c`（零依赖确认回放，每种子跑两遍 =
> 跨迭代卫生检查）、`fuzz/wrap.c`（libFuzzer 适配）、Makefile 的
> `fuzz` / `fuzz-seeds` / `fuzz-libfuzzer` / `clean-fuzz`。
> 烟测：24/24 种子全绿（22k contract checks）；libFuzzer 四作业 90 秒
> ~4.2 万次/秒×4 干净（0 crash 0 leak）。注意：macOS 上 libFuzzer 与 UBSan
> 不共链（运行时符号冲突），fuzzer 链接用 `fuzzer,address` 去掉 ubsan——
> 这是 OSS-Fuzz 的标准 macOS 口径。

1. **mallocFailed 粘滞**（§4.2）——选 1（移植清除）或选 2（文档化僵尸态）。
   影响的是 Target C 的 oracle 与一处头文件契约；建议选 1，
   顺带这本身就是一次"上游清理链路重建"。
2. ** :memory: = fuzz 主形态**——99% 的迭代用内存库（速度快，journal 固定 MEMORY）
   + Target A 文件 flavor 与 Target B 负责 WAL/journal 切换面。
   这样 B 的语料库不必每次迭代都落盘。
3. **语料检入策略**——动作流种子（KB 级）检入；文件语料（>64KB 的 deep/bigval）
   由 `make fuzz-seeds` 现场再生。repro 一律检入（最小化后通常 <1KB）。

——设计完毕。