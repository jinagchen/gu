# 第六阶段 B 侧交付：动态写入的确认、可见与恢复

2026 年 11 月 25 日完成初版，11 月 26 日补齐 WAL 残尾修复与新日志文件头发布。A 侧的 [WAL 底层](stage6-a-wal.md)提供编解码、追加、刷盘与轮换。本文件记录引擎闭环、故障测试与初版实测成本。

## 2026 年 11 月 26 日恢复修复

- **根因**。第一次 `Open` 只在内存中跳过最新日志的半帧。续写创建新 `.log` 后，原日志变成历史文件，第二次 `Open` 将遗留的半帧判为 `corruption`。
- **处理顺序**。持有实例独占锁，完成只读扫描、增量预算检查和重放后，独占复核最新 WAL 的文件身份、文件头和扫描长度。在同一句柄上截去合法残尾并刷盘。每次 `Open` 都刷盘已验证的最新 WAL，包括长度已经正常的重试。失败返回原生错误码，成功后才发布视图。
- **日志接管与发布**。起始序号等于下一写入序号的空 `.log` 由恢复直接接管。新 WAL 先写入并刷盘同目录唯一 `.pending`，再同卷改名发布且禁止覆盖。业务帧在重开并复核正式文件后才能追加。创建或轮换遇到 I/O 错误后，当前实例停止写入，须重新 `Open`。
- **复现对照**。独立夹具确认序号 1 后，只追加序号 2 的合法帧前缀。旧实现下子进程首次恢复、确认序号 2 并强制退出，父进程第二次 `Open` 因 `Non-final WAL file ends inside a frame` 失败，`dynamic_engine` 为 180 项断言、1 失败，`wal` 通过。修复后第二次打开读回序号 1 和 2 的 ID、版本与向量，下一次写入确认序号 3。
- **故障覆盖**。包含头部空日志、首帧撕裂与轮换后首帧撕裂、截断后的刷盘失败和进程退出、已同步未回复的完整帧、`.pending` 写入/同步/改名失败、目标名冲突、发布后接管失败、身份/长度/头复核失败，以及损坏日志逐字节不变。
- **边界**。旧版本造成的历史 WAL 残尾仍返回 `corruption`。当前扫描仍在内存收集日志和重放记录，缺少大历史的独立恢复内存上限。每次打开最新 WAL 会增加一次刷盘。未运行物理断电试验或重测下方历史性能数据，文件名的断电持久化仍待验证。`Checkpoint` 和 WAL 自动回收仍未启用。

实际验证使用 Windows x64、MSVC 19.43.34810、NTFS 和 LF 源码，`TEMP` 与 `TMP` 均指向 `D:/agent-io-walfix-20261126/tmp`。构建目录由 `cmake -G "Visual Studio 17 2022" -A x64` 配置，启用了仓库的 Python 检查与固定 Boost 依赖。

| 验证 | Debug | Release |
| --- | --- | --- |
| `wal` / `dynamic_engine` 专项 | 2/2 通过，动态测试 424 项断言全部通过 | 2/2 通过，动态测试 424 项断言全部通过 |
| 全量 CTest | 14/14 通过，25.66 秒 | 14/14 通过，24.42 秒 |

```powershell
$build = 'D:/agent-io-walfix-20261126/build'
foreach ($config in 'Debug', 'Release') {
    cmake --build $build --config $config --target wal_test dynamic_engine_test --parallel
    ctest --test-dir $build -C $config -R '^(wal|dynamic_engine)$' --output-on-failure -V
}
foreach ($config in 'Debug', 'Release') {
    cmake --build $build --config $config --parallel
    ctest --test-dir $build -C $config --output-on-failure
}
```

本机测试日志位于 `D:/agent-io-walfix-20261126/ctest-*.log`。

## 结论

1. **"同步 → 发布 → 确认"闭环打通**。`Insert` / `Upsert` 返回收据时，记录已在 WAL 落盘、已在快照中可见。187 项断言全部通过，含进程被强制终止后的恢复。
2. **旧版本不会冒充当前结果**。同一 ID 多次 `Upsert` 后，查询与 `Get` 只返回最高版本，同一 ID 在结果集中至多出现一次。
3. **WAL 无写放大**。16 维下每写恰好追加 **136 字节**（一帧），50 / 200 / 800 三档实测总字节数与 `写次数 × 136` 逐位相等。这是可核算的强证据，不是估算。
4. **确认延迟的构成是 fsync，不是计算**。三档 p50 稳定在 **2.12–2.13 ms**，与写入量无关；紧随其后的读回只需 **9.5 µs**。两者相差约 220 倍，说明成本全在持久化边界上。
5. **`Checkpoint` 仍返回 `unsupported`**，原因与后续路径见[缺口](#缺口与后续)一节。本阶段不宣称已能回收 WAL 或压缩恢复成本。

## 实现范围

### 新增模块

| 文件 | 职责 |
| --- | --- |
| `src/index/delta.hpp` / `delta.cpp` | 稳定槽位增量层：预约、写入、单调发布前缀、冻结、预算记账 |
| `src/index/recovery.hpp` / `recovery.cpp` | WAL 目录扫描：实例身份、序号连续性、尾部截断与重放 |
| `src/engine/engine.cpp` | `Open` 接入恢复、`Insert`/`Upsert`/`Get`/`Search`/`Stats` 接入增量层 |
| `tests/index/dynamic-engine-test.cpp` | 动态正确性与故障测试 |
| `tools/dynamic/write-probe.cpp` | 确认与可见延迟、恢复耗时的测量探针 |
| `tools/dynamic/run-stage6-b.py` | 三档写入量的测量驱动与不变量校验 |
| `tools/dynamic/plot-stage6-b.py` | 证据图 |

### 写路径的固定顺序

一条写入只走一条路径，顺序不可交换：

```text
准入（pending 计数 + 超时）
  → 发布锁（唯一协调者）
  → 存在性检查（Insert 冲突 / Upsert 覆盖）
  → 序号 = 已发布 + 1
  → 增量层预约槽位 → 写入槽位（未发布）
  → 帧容量核算 → 追加 WAL → Sync
  → 推进发布前缀 → 推进已发布序号 → 重建快照
  → 返回收据
```

失败点的语义按 A 侧契约区分，不混为一谈：

- **追加之前失败**：槽位取消，`write_effect = none`。索引与调用前完全一致。
- **追加失败**：槽位取消，writer 标记为不可再用，后续写请求返回 `recovery_required`。已写出的半帧由下次启动按截断尾丢弃。
- **Sync 失败**：槽位**保留但不发布**，返回 `may_have_applied` 与 `attempted_sequence`。当前进程看不到这条记录，重启后可能看到——这正是 `may_have_applied` 应有的含义，不能简化为成功或失败。

### 查询融合

主图扩展与精排照旧，增量层单独全量精排，然后按版本合并：

```text
主图候选（Expand → Refine，已按快照过滤版本）
  ∪ 增量记录（槽位 < 捕获的已发布前缀，且版本 ≤ 快照）
  → 同一 ID 取最高版本
  → 按精确距离排序，取前 K
```

增量层用暴力精排而不是图搜索，是因为它的规模被 `max_retained_delta_vectors` 限死（默认 12288），而图里根本没有新写入的边。让增量参与图遍历才是错的。

`Get` 同样先查增量层，取版本更高者。两侧都用**捕获快照**过滤，因此一次查询开始后到达的写入不会出现在它的结果里。

### 恢复

`Open` 在返回引擎之前完成重放：

1. 扫描 `wal/` 下所有 `.log`，读**文件头**取实例身份与起始序号。文件名只用于区分文件，不用于判定新旧。
2. 拒绝外来实例、重复起始序号、序号断档。
3. 逐帧解码；只有**最新**文件允许以不完整帧结尾（那是从未被确认的写），历史文件出现短尾按 `corruption` 处理——轮换只发生在已同步的组之间，历史文件缺尾意味着已承诺的字节丢了。
4. 跳过 `≤ checkpoint_sequence` 的记录，其余按顺序重放进增量层，并恢复已发布序号。
5. 存活数按 **ID** 统计，不按记录数：同一 ID 的多条记录只算一个，已存在于段中的 ID 不重复计数。写入路径用的就是这条口径，恢复若按记录数累加，重启后 `live_vectors` 就会比重启前偏大。
6. 重放完成后修复并同步最新 WAL，需要时接管其空文件，然后再发布可读视图。

重放是幂等的：日志里没有"已应用"标记，状态完全由记录序列重建，因此重复打开不会重复应用。

### 统计

`Stats` 的 `published_sequence`、`wal_bytes_written`、`logical_update_bytes`、`pending_write_count`、`delta_resident_bytes` 不再恒为零值，均来自真实计数。

## 正确性测试

初版 `dynamic_engine_test` 有 **187 项断言、0 失败**。覆盖与验收条目的对应关系如下，11 月 26 日的新增用例和验证见上方修复记录。

> 2026 年 11 月 27 日补注：该用例此后继续增加（恢复故障、日志接管、容量与拒绝路径），当前实测为 **426 项断言、0 失败**。本文其余结论（每写 136 字节、确认延迟量级、恢复不重放已落段记录）在本次复跑中不变。`Checkpoint` 已在[第七阶段 B](stage7-b-completion.md)接通，本文"仍为 `unsupported`"的表述属当时状态。

| 验收要求 | 用例 | 断言要点 |
| --- | --- | --- |
| 确认后立即可见 | `TestConfirmThenVisible` | 收据序号 = 1；紧随的 `Get` 返回同版本、同向量；查询快照 = 已发布序号 |
| 新写入可被检索 | 同上 | 写入的向量以其自身距离 0 出现在结果中，`delta_records_scanned = 1` |
| 重复 ID 的 insert 冲突 | `TestVersionsAndConflict` | 已存在的 ID 返回 `already_exists`，不产生序号 |
| 旧版本不冒充当前 | `TestVersionsAndConflict` | 两次 `Upsert` 后 `Get` 返回版本 2；查询结果中该 ID 只出现一次且版本为 2 |
| 查询快照隔离 | `TestSnapshotIsolation` | 写入前后的查询快照递增；写入后的结果不含新 ID，之前的快照结果亦不含 |
| 并发查询与写入 | `TestConcurrentQueriesAndWrites` | 两个读线程持续查询期间有 40 次写入发布；没有结果超出自身快照序号，没有 ID 重复，没有查询失败 |
| 确认写入可重放 | `TestRecoveryReplaysConfirmedWrites` | 重启后已发布序号、存活数、每条记录版本逐项相等 |
| 重复恢复不重复应用 | 同上 | 第二次打开后序号不再增长 |
| 恢复后可续写 | 同上 | 下一条写入的序号 = 恢复序号 + 1，而非从 1 重来 |
| 存活数按 ID 计 | `TestRecoveryLiveCountCountsIds` | 同一 ID 写两个版本、再覆盖一条段中已有的向量，存活数只加 1；重启后与重启前相等 |
| 日志残尾修复 | `TestTornTailThenConfirmedWriteSurvivesAnotherOpen` | 追加未确认帧前缀，恢复并确认下一次写入，强制结束后再次打开验证两条已确认记录 |
| 损坏帧不静默放过 | `TestCorruptFrameIsRefused` | 翻转载荷一个字节使 CRC 失配，打开返回 `corruption` 而非丢弃该帧继续 |
| 历史文件缺尾是损坏 | `TestHistoricalShortTailIsRefused` | 用 400 字节轮换上限制造多文件，截短第一个文件后打开返回 `corruption` |
| 拒绝路径不留下痕迹 | `TestRefusals` | 零 ID、维度不符、非有限值、零超时全部被拒；已发布序号与存活数不变 |
| 容量上限是拒绝而非覆盖 | `TestDeltaCapacityIsEnforced` | 容量 2 时第三次写返回 `resource_exhausted` |
| 进程终止后恢复 | `TestProcessCrashRecovery` | 子进程确认 3 条后 `TerminateProcess` 自杀，父进程验证 3 条全在、序号=3、可继续写 |

进程故障用例启动真实子进程并在其确认写入后强杀，**退出码为 1 而非 0**，确保测的是崩溃而不是正常关闭。它只证明对应进程故障场景，不宣称已验证断电可靠性。

## 实测成本

固定 4096 向量、16 维、K=10，写入量 50 / 200 / 800 三档，每档独立生成夹具与索引。详见 `docs/evidence/stage6-b/sweep-summary.json` 与 `write-sweep.png`。

| 写入量 | 确认 p50 | 确认 p95 | 确认 p99 | 读回 p50 | WAL 字节 | 每写字节 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 50 | 2.129 ms | 4.046 ms | 10.097 ms | 9.3 µs | 6,800 | 136 |
| 200 | 2.119 ms | 2.538 ms | 4.066 ms | 9.9 µs | 27,200 | 136 |
| 800 | 2.131 ms | 2.452 ms | 2.673 ms | 9.5 µs | 108,800 | 136 |

三点观察：

- **确认 p50 与写入量无关**。上表三档落在 2.119–2.131 ms（同一次测量内波动 0.6%），独立复跑落在 2.14–2.68 ms。绝对值随机器状态变化，但三档之间不随写入量单调变化，也没有随历史增长的趋势——每条写入单独 `Sync`，成本就是一次 `FlushFileBuffers`。跨运行波动约 25%，说明这个绝对值不能当作稳定的性能指标，只有"与历史长度无关"这一条是可外推的结论。
- **p99 随样本量收敛**：50 条时 10.1 ms 是单个冷启动尖峰，800 条时降到 2.67 ms。小样本的分位值不可外推。
- **读回是内存操作**（≈9.5 µs）。确认与可见的成本相差两个数量级以上，这决定了后续优化的方向：要降延迟只能组提交，不能优化比较逻辑。

### 恢复成本

| 写入量 | 同进程重开 | 新进程打开 |
| ---: | ---: | ---: |
| 50 | 26.1 ms | 40.4 ms |
| 200 | 30.1 ms | 38.6 ms |
| 800 | 30.3 ms | 37.1 ms |

恢复耗时与历史长度**几乎无关**（800 条比 50 条只多 4 ms）。原因有两层：一是记录很小（136 字节/条，800 条仅 108 KB）；二是打开的主要成本是段元数据加载与文件哈希校验，重放只占零头。

这不是"恢复已经很快"的证据，而是**当前规模太小**的证据。没有 checkpoint 时恢复必须重放全部历史，一旦历史涨到百万级，这条曲线就会抬头。同进程重开的 26 ms 只是第一个数据点的缓存未命中，不能当作趋势读。

## 缺口与后续

### `Checkpoint` 仍未实现

`Checkpoint` 返回 `unsupported`，消息说明原因。这不是遗漏，而是一个需要连带机制的决定：

要把增量落成 `flushed_delta` 段并让它继续参与查询，必须先有**增量段枚举**能力——`VersionDirectory` 只有按 ID 的 `Find`，没有遍历。没有枚举，落盘后的段在重启后就会变成"磁盘上存在但查询看不见"的数据，比不做更糟。

补齐顺序应是：枚举接口 → 段落盘与清单发布 → 查询扫描增量段 → `checkpoint_sequence` 推进。这一整块属于**合并与代次切换**（阶段七）的前置，本阶段不做半成品。

### EVAL-1.1 动态轨迹闭环仍缺生产端

`evaluation.check` 要求 `trace.jsonl` / `writes.jsonl` / `receipts.jsonl` / `queries.jsonl` 完整配套。引擎已能产出收据，但缺少把轨迹驱动成收据的**基准生产端**（A 侧职责，团队计划中记为"混合基准生产端"）。本阶段因此没有动态评测工件，也不宣称已通过 EVAL-1.1 动态验收。

### 其余限制

- **仍是小夹具**。4096 向量、16 维，增量容量 12288 槽（1.31 MiB 预算）。绝对延迟不可外推。
- **单条同步**。每条写入一次 `Sync`，没有组提交。`group_commit_window` 字段存在但未使用，属于后续优化。
- **无混合负载**。未测读写并发下的尾延迟与写入背压，只验证了协议正确性。
- **`max_pending_writes` 用轮询等待**。超过上限的写请求以 1 ms 间隔轮询至超时，不是条件变量唤醒；正确但会浪费 CPU，规模化前应替换。

## 复现

```powershell
# 前置：在「Developer PowerShell for VS 2022」中执行，或先运行 vcvars64.bat。
# 只把 cl.exe 的绝对路径传给 CMake 而不加载 VC 环境会失败：此时 rc.exe / mt.exe
# 不在 PATH，CMake 生成的链接命令是 --rc=rc --mt=CMAKE_MT-NOTFOUND，编译器自检过不去。
# Ninja 不在 PATH 时另需 -DCMAKE_MAKE_PROGRAM=<ninja 全路径>。

# 构建（Release）
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
cmake --build build/release --parallel

# 动态正确性与故障测试
ctest --test-dir build/release -R dynamic_engine --output-on-failure

# 三档写入量测量（输出目录必须是 ASCII）
.\.venv\Scripts\python.exe tools/dynamic/run-stage6-b.py `
    --output D:/agent-io-stage6/sweep --vectors 4096 --dimension 16 `
    --write-counts 50 200 800

# 证据图
.\.venv\Scripts\python.exe tools/dynamic/plot-stage6-b.py `
    --summary D:/agent-io-stage6/sweep/summary.json --output write-sweep.png
```

索引与 WAL 仍受 FORMAT-1 的 ASCII 路径约束，因此运行目录放在 `D:/agent-io-stage6/` 而非仓库内。脚本拒绝复用非空输出目录：证据若能被静默重写就不算证据，重跑请换新目录。
