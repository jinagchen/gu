# 第四阶段 B：静态基线与评测闭环

日期：2026 年 11 月 21 日。协议 EVAL-1.1 / INDEX-B-1.2 / FORMAT-1，公开 Engine API 不变。

## 结论

B 侧第四阶段的六项产出全部落地并在本机跑通：真实 IndexAdapter、首次构建程序、Engine 静态查询、真实基准目标、评测闭环、交付文档。`ctest` 11 项全过（其中 `index_engine` 147 条断言、`build_contract` 28 个用例）。在 Job 提交限额下产出的静态基线 `runs/stage4-static-001` 由 `evaluation.check` 判定 `verdict=pass`，10 项协议检查全部 pass、Recall@10 = 1.000；同一命令重复运行 25 项状态严格相同、0 项分歧。

有两处必须一并说明，否则数字会被误读：召回 1.000 是在 512 个 16 维向量的小夹具上取得的，每个查询实际读取约 438 行（接近全图），因此它证明的是链路与协议正确，不是检索效率；内存只验证了 Job 提交限额内可运行，`memory_acceptance` 仍为 `not_evaluated`，不构成 REQ-1.0 的内存比例结论。

## 交付物

### 索引层 `src/index/`

- [json.hpp](../src/index/json.hpp) / [json.cpp](../src/index/json.cpp)：有界 JSON 读写。严格语法、拒绝重复键、深度/元素/字符串上限、UTF-8 校验、紧凑输出且不带 BOM 与尾换行，使工件字节可复算。
- [digest.hpp](../src/index/digest.hpp) / [digest.cpp](../src/index/digest.cpp)：BCrypt SHA256 文件摘要、UUID 生成与解析、ASCII 路径检查。
- [format.hpp](../src/index/format.hpp) / [format.cpp](../src/index/format.cpp)：FORMAT-1 二进制编解码，含 `CURRENT` 记录、段元数据、32 字节版本记录（`id/version/node/保留零`）与节点行布局校验。
- [manifest.cpp](../src/index/manifest.cpp)：清单编解码与全字段校验。段路径必须是 `segments/<uuid>/segment.json`、`wal_replay_after` 必须等于 `checkpoint_sequence`、`live_count ≤ Σ node_count`、无段时两者必须同时为 0。
- [scratch.cpp](../src/index/scratch.cpp)：`ComputeScratchPlan` 按契约公式推导影子表、候选数组、refine、PQ scratch、导航与 IO 缓冲上界，装不下即 `resource_exhausted`。
- [reader.hpp](../src/index/reader.hpp) / [adapter.cpp](../src/index/adapter.cpp)：打开索引目录（校验 `CURRENT` → 清单 → `segment.json` → 每个文件哈希）、分页版本目录（稀疏首页索引加块读二分）、`GraphAdapter` 的 `Geometry/Locate/Expand/Refine`。
- [builder.hpp](../src/index/builder.hpp) / [builder.cpp](../src/index/builder.cpp)：`RunFirstBuild`。输入校验 → 精确 kNN 构图加反向边与确定性长程边 → staging 写 `graph.bin`/`versions.bin`/`segment.json` 并逐个 `FlushFileBuffers` → `MoveFileExW` 移入段目录 → 写清单刷盘改名 → `CURRENT` 替换。

### 引擎与命令行

- [engine.cpp](../src/engine/engine.cpp)：`Open/Search/Get/Insert/Upsert/Checkpoint/Stats/Close`。`LOCK` 独占打开；`Search` 为视图捕获 → `ComputeScratchPlan` → `TryReserve` → `Expand` → `Refine`，排序用压缩距离而返回距离按原向量 float64 重算。写路径显式返回 `unsupported`，不伪装成功。
- [vector_build](../tools/build/main.cpp)：首建 CLI，解析 build.json、核对当前 Job 限额、发布索引并输出报告。退出码 2 为请求自相矛盾（解析期拒绝，未触碰文件），4 为请求自洽但数据或环境不符。
- [engine-target.hpp](../tools/benchmark/engine-target.hpp) / [engine-target.cpp](../tools/benchmark/engine-target.cpp)：真实基准目标，校验四个输入的 SHA256、读查询与 ID、按配置预约 workspace 后调用 `Engine::Open` 与真实 `Search`。

### 评测与驱动

- [static_fixture.py](../evaluation/static_fixture.py)：确定性夹具生成器。向量、ID、查询、精确答案、首建配置、运行配置与轨迹一次生成，写盘强制 `\n`。
- [run-static-baseline.py](../tools/contracts/run-static-baseline.py)：端到端驱动。夹具 → Job 内首建 → Job 内测量 → `evaluation.check` → 汇总 `evaluation.json`。只启动进程，不重写任何已发出工件。
- [compare-runs.py](../tools/contracts/compare-runs.py)：重复运行对比。把状态分成「必须逐字节相同」「内容摘要必须相同但文件名可不同」「允许变动」三类，并把每个允许变动的项连同容差一起报告。
- [check-build.py](../tools/contracts/check-build.py)：首建独立复核。不读 `build-report.json` 的结论，而是自己重算哈希、从 `CURRENT` 走到清单再走到 `segment.json`，并走完拒绝矩阵。

## 真实静态基线

命令：

```text
python tools/contracts/run-static-baseline.py --run-id stage4-static-001
```

在 Job 提交限额 384 MiB 下执行，产物在 `runs/stage4-static-001/`。

| 项目 | 值 |
|---|---|
| 向量 / 维度 / k | 512 / 16 / 10 |
| 段文件 | `graph.bin` 2 097 152 字节（512 行 × 4096），`versions.bin` 16 384 字节 |
| 构图 | 精确 kNN，度数 32，含反向边与 XOR 长程边 |
| 到达速率 | 10 ops/s，单 worker，开放式 |
| 查询 | 60 全部成功，失败 0，拒绝 0 |
| Recall@10 | 1.000（60/60 查询各命中 10） |
| 窗口吞吐 | 10.0 ops/s |
| 引擎计数 | 提交读 26 310 次 / 完成 107 765 760 字节 / 读错误 0 |
| Job 峰值提交 | 4 820 992 字节（限额 402 653 184），单次观测 |
| 未计费元数据 | 20 045 字节，配置与 `environment.json` 一致 |
| `evaluation.check` | `verdict=pass`，10 项检查全 pass，距离最大绝对误差 0.0 |

`unbudgeted_metadata_bytes` 为 20 045，是 `DescribeIndex` 对索引格式元数据的估算，不是进程镜像、CRT、依赖库及分配器开销的实测总和。进程级峰值看独立 Job 工件。

每个查询 26 310/60 ≈ 438 次 4 KiB 无缓冲读（`direct_reads=true`），约 1.71 MiB；`graph.bin` 的物理大小恰好 2 MiB，故单查询读放大达 50.9 倍。这解释了为什么召回是 1.000：候选预算远大于夹具规模，搜索几乎遍历了全部节点。**这个基线不能用来评价检索效率，也不能外推到真实规模。**

## 重复运行可解释

```text
python tools/contracts/compare-runs.py --left runs/stage4-static-001 --right runs/stage4-static-002
```

结果：25 项严格相同、1 项派生一致、6 项允许变动、0 项分歧，`verdict=pass`。

严格相同的是输入计划和语义输出：`trace.jsonl` 的到达序、`operation_id`、`query_id`、`query_row` 序列；每个查询的命中数与返回数；快照序列；`by_type` 的 scheduled/succeeded/failed/rejected；`engine_counters` 全部字段；`evaluation.checks` 与 `verdict`。

派生一致的一项最值得记：两次首建的 `graph.bin` 与 `versions.bin` **逐字节相同**（`eca9ee28…`、`c4d67667…`），但 `segment.json` 与目录名不同，因为每次首建铸造新的段 UUID。所以判定依据是清单钉住的内容摘要，不是外层文件哈希。第一次跑对比时我按外层哈希判等，误报成"索引不确定"；这正是把判据写进脚本而不是靠眼睛看的原因。

允许变动的六项都附了容差：窗口吞吐 10.0 → 10.0（相对偏差 0，容差 10%）、三个延迟直方图的众数 bin 未移动（容差 1 个 bin）、排空时长 1.43 ms → 0.84 ms（仅记录）。

排空时长这一项暴露过一个真实设计错误，已修：夹具原先按 50 ops/s 到达，而单次查询服务时间约 40 ms，单 worker 根本跟不上，运行处于饱和态。此时 `drained_qps` 由排空耗时决定，两次跑出 19.5 与 16.4（漂移 15.8%），看起来像"系统不稳定"，实际是配置超出了服务能力。修法是把到达速率降到 10 ops/s 并改用 `window_completed_qps` 作为可比量，同时给 `compare-runs.py` 加了饱和护栏：任一运行的窗口吞吐低于配置速率的 90% 直接判为分歧，因为饱和运行时测不出索引的性质。

## 首建拒绝矩阵

```text
python tools/contracts/check-build.py --output build/build-contract
```

28 个用例。核心不是"进程非零退出"，而是**被拒绝的首建不得改变已发布的索引**：每个用例都重新校验 `CURRENT` 的哈希与内容未变、没有发布新 `CURRENT`、没有留下 staging 目录。

- 配置期拒绝（退出 2，未触碰文件）：未知字段、缺字段、`schema_version` 与 `protocol_version` 不符、非法 purpose、非规范十进制（`"064"`）、布尔冒充整数、非法 metric/format、`build_beam > max_degree`、`generation ≠ 1`、`checkpoint_sequence ≠ 0`、`publish_current=false`、`job_object=false`、`reuse_allowed=true`、`native.version` 与 `native.native_version` 不一致。
- 数据期拒绝（退出 4）：向量或 ID 哈希不符、向量文件缺失。
- 请求自相矛盾（退出 2）：`vector_count` 与文件长度不符、`dimension` 不符。
- 重复首建：向已有索引再建一次被拒，`CURRENT` 与清单 `instance_id` 均未变。
- 畸形 JSON：重复键、尾随内容、`NaN`、截断，全部退出 2。

独立复核部分自己重算而不采信报告：`CURRENT.sha256` 覆盖清单字节、清单与 `CURRENT` 的 `instance_id` 一致、`live_count` 等于唯一段的 `node_count`、`segment.json` 的摘要与清单一致且路径为相对路径、`graph.bin`/`versions.bin` 的摘要与大小和 `segment.json` 相符、`physical_bytes` 块对齐且不小于 `logical_bytes`、版本目录按外部 ID 严格升序且 node 列是 `0..n-1` 的排列、保留字节为零、padding 为零。

一个用例的正确行为与其他不同，单独列出：自我声明的峰值上限 `max_peak_commit_bytes` 只能在进程跑完后才知道，所以无法阻止发布。`check-build.py` 把它单独立为 `peak-overrun-after-publish`，要求退出 4、`CURRENT` 存在、报告 `published=true` 且如实给出实测峰值。把它混进"拒绝"矩阵会变成一句谎话。

## 本阶段修正的缺陷

这些不是收尾润色，是会让结论失效的问题，按根因记录：

1. **图不连通导致召回 0.58**。`BuildGraph` 在反向边之后按距离裁到 `max_degree`，其后的长程边填充永远没有空位可用，是死代码。紧簇数据上精确 kNN 图不连通，单入口 best-first 上升不到其他簇。改为按度数预算分配，最近邻占 `max_degree - long_range`，余下用 XOR 幂次长程边。
2. **单入口仍会困在入口簇**。簇内距离远小于跨簇距离，结果表被入口簇填满后所有跨簇跳点都被剪枝。改为 16 个确定性入口种子，召回回到 1.000。
3. **`trace.jsonl` 哈希与配置记录不符**，导致 `stage3_b_contracts`/`stage3_b_regressions` 全挂。根因是 `Path.write_text` 在 Windows 默认转 CRLF，而夹具哈希按 LF 计算。改为显式 `newline="\n"`。
4. **配置解析把相对路径改写成绝对路径**。这让 `check.py` 的"工件必须留在运行目录内"永远无法满足，真实运行目录无法被消费。
5. **`job_runner` 与基准的输出目录约定互斥**。前者必须在子进程启动前创建指标文件，后者拒绝已存在的输出目录，EVAL-1.1 要求的"Job 证据在运行目录内"因此无法同时成立。改为基准只在自己将要写入时创建目录，且失败不留空目录。
6. **`environment.json` 记录绝对路径**。改为运行目录相对路径，使证据可随目录搬迁。
7. **清单 `adapter.native_version` 写的是适配器版本**。配置里 `native.native_version` 被要求存在却从未被读取，清单与报告都写成了 `1.0.0`，而实际底座是 DiskANN `0.6.1+4c31367`。为 `FirstBuildRequest` 增加 `native_version` 并贯通到清单与报告，同时校验两个字段不得矛盾。
8. **`code_commit_at_configure` 为空字符串**。CMake 在使用无 Git 元数据的源码快照时补 `unavailable-no-git-metadata`，正常 Git 检出记录配置时的提交。
9. **基准 CLI 仍期望 `--target engine` 返回 3**。真实目标接通后过期的断言，改为配置不完整时退出 2、索引打不开时退出 2 且不创建输出目录。

## 已知限制

- **夹具规模小**。512 × 16 只够验证协议，不足以支撑任何性能或内存比例结论。召回 1.000 来自近乎全图扫描。
- **写路径未实现**。`Insert`/`Upsert`/`Checkpoint` 返回 `unsupported`，动态运行、WAL 重放、后台合并不在本阶段范围，`writes.jsonl` 为空是设计结果。
- **内存只验证了限额**。`memory_acceptance=not_evaluated`，Job 提交限额是调度限额，不是物理内存测量。
- **该基线为单 worker**。后续[第五阶段](stage5-a-completion.md)做了两个 worker 的静态读侧对照，规模化并发与混合读写仍待验证。
- **`code_dirty_at_configure` 当前输出 `"true"` 或 `"false"` 字符串**。若 `code_commit_at_configure` 是 `unavailable-no-git-metadata`，不能从 `"false"` 推断源码没有修改。
- **`build-report.json` 的 `adapter` 字段是硬编码常量**，与清单内容重复，未与 `FirstBuildRequest.adapter_name` 联动。

## 复现

```powershell
cmake -S . -B build/windows -G "Visual Studio 17 2022" -A x64
cmake --build build/windows --config Release
ctest --test-dir build/windows -C Release --output-on-failure

.\.venv\Scripts\python.exe tools/contracts/run-static-baseline.py --root D:/agent-io-runs --run-id stage4-static-001
.\.venv\Scripts\python.exe tools/contracts/run-static-baseline.py --root D:/agent-io-runs --run-id stage4-static-002
.\.venv\Scripts\python.exe tools/contracts/compare-runs.py --left D:/agent-io-runs/stage4-static-001 --right D:/agent-io-runs/stage4-static-002
```

`vector_bench` 需要固定 Boost 1.78 头文件；首建和引擎库不依赖 Boost。完整回归的 LF、ASCII 路径和临时目录设置见 [README](../README.md#开发环境)。

## 未完成

小规模静态闭环已经验收。静态检索效率与严格内存比例仍需正式规模数据验证；动态运行、增量段、恢复与后台合并按第六、七阶段验收。2026 年 11 月 23 日的干净构建与小夹具复测见[项目状态](project-status.md#本次复核)。
