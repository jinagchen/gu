# 第三阶段 B 交付记录

日期：2026 年 11 月 17 日。版本 INDEX-B-1.1，对应 API-1.2、EVAL-1.1、STORAGE-A-1。11 月 16 日首次交付 B 侧内部契约与动态评测；本次修复复核发现的语义、校验和接口缺口。**索引层本身仍未实现**：`index.hpp` 全是声明，没有 IndexAdapter、DeltaStore、构建或合并代码。修订后的接口待 A/B 联合复核，DESIGN-1.1 尚未冻结。

## 1 交付物

| 文件 | 内容 |
| --- | --- |
| [include/agent_io/index.hpp](../include/agent_io/index.hpp) | 段描述、节点定位、候选与预算、ReadView、DeltaStore、IndexAdapter、VersionDirectory、ScratchPlan、BuildRequest/Artifacts、Manifest 声明 |
| [docs/stage3-b-contract.md](stage3-b-contract.md) | 契约正文：版本可见性、稳定槽前缀、RequiredScratch 计量、构建受限域、清单与目录、PQ 绕行披露、A/B 交错推演 |
| [configs/examples/build-run.json](../configs/examples/build-run.json) | vector_build 输入：数据与哈希、原生构图参数、独立 Job 限额、staging、输出与首建条目 |
| [configs/examples/dynamic-run.json](../configs/examples/dynamic-run.json) | 动态运行输入：固定轨迹、快照策略、混合负载比例、引擎与测量参数 |
| [evaluation/trace.py](../evaluation/trace.py) | 生成确定性轨迹、写入日志、查询日志与精确答案；固定 splitmix64，不依赖 numpy |
| [evaluation/check.py](../evaluation/check.py) | 消费运行目录：类型、守恒、时钟、版本、距离重算、序号连续性、**轨迹与工件关联**、`query_count` 与轨迹哈希、召回率；写出 evaluation.json |
| [evaluation/examples/stage3-b/](../evaluation/examples/stage3-b) | 24 条 4 维、10 次写、6 次查询、3 个快照的自洽样例及数据文件 |
| [tools/contracts/header-smoke-b.cpp](../tools/contracts/header-smoke-b.cpp) | 编译期类型与成员断言，注册为 `agent_io_index_contracts` 目标 |
| [tools/contracts/check-stage3-b.py](../tools/contracts/check-stage3-b.py) | 配置规则、轨迹顺序、精确答案一致性检查，30 个原有反例 |
| [tools/contracts/test-stage3-b.py](../tools/contracts/test-stage3-b.py) | 87 项动态回归（复审先新增 24 项，继续复核再新增 9 项），注册到 CTest |

## 2 验证结果

2026 年 11 月 17 日本机复审：Windows 原生，MSVC 19.43.34810.0，`/W4 /WX /permissive- /EHsc`。先前 54 项 B 回归已重跑通过，复审增至 78 项，继续复核增至 87 项。此前 MSVC 19.44 / 5 项 CTest 为 11 月 16 日的历史记录；本次使用修复工作树现有 Visual Studio 2022 x64 构建配置，结果如下（当前 shell 未将 CMake 加入 PATH，使用 VS 随附可执行文件的绝对路径）：

```text
cmake --build build\windows --config Release                     -> 全目标成功
cmake --build build\windows --config Debug                       -> 全目标成功
ctest --test-dir build\windows -C Release                        -> 8/8 passed
ctest --test-dir build\windows -C Debug                          -> 8/8 passed
python tools/contracts/test-stage3-b.py                         -> 87/87 passed
python tools/contracts/test-stage3-a.py                         -> 23/23 passed
```

B 侧契约自查：

```text
python tools/contracts/check-stage3-b.py
PASS: build config, dynamic config, trace contract (10 writes + 6 queries of 16 rows), exact answers (Recall@10=1.00), 30 rejected negative cases
```

A 侧无回归：

```text
python tools/contracts/check-stage3-a.py
PASS: static config, resource modes, static answer join, normal/uncertain outputs
```

生成端往返复核：在独立新目录运行 evaluation.trace，再运行 evaluation.check；除重新计算的 evaluation.json 外，全部生成文件与仓库夹具 SHA256 一致，快照仍为 S=5、8、10。临时正式测量格式的正例也仅是测试生成的证据，不代表实际执行了正式测量。

样例自身的评测结果（`evaluation/examples/stage3-b/evaluation.json`）：6 次查询全部 ok，3 个快照（S=5、8、10），已确认写入 10 条，距离重算最大绝对误差 0，Recall@10 = 1.0。

**这个 1.0 不构成召回证据。** 样例的邻居由精确暴力扫描生成，用于验证消费链与答案独立性，任何近似算法的召回必须来自真实索引运行。

## 3 被反例覆盖的拒绝路径

每个反例只改动一处，用来证明校验器不是空转：旧版本冒充当前版本、版本大于快照、重复 ID、非规范 uint64、快照不可见 ID、距离漂移、邻居乱序、快照前移、快照回退、失败查询伪造快照、写序号断裂、insert 冲突、请求守恒破坏、隐藏 pending、篡改答案。全部按预期原因被拒。

配置侧另有 10 个反例：staging 与 output 同路径或互相包含、构建配置未知字段（`memory_limit_mb` 拼写错误）、动态配置未知顶层字段、engine 内未知字段（`block_size`）、`trace.operations` 与实际行数不符、写入比例与轨迹构成不符、`run_id` 与轨迹行不一致、轨迹文件缺失、`engine.directory` 与 `index.directory` 不一致。全部逐条核对过拒绝原因，不是碰巧失败。

轨迹与工件关联另有 5 个反例：轨迹指向不存在的写操作、`arrival_index` 断号、`trace.operations` 与写入行数不符、轨迹被改动后哈希不匹配、`dataset.query_count` 与 `queries.jsonl` 行数不符。这组检查已下沉到 `evaluation.check` 本身（原先只在本自查脚本里），因此任何运行目录都会被执行，不只是夹具。

## 4 复核中修正的三处不一致

初版 `dynamic-run.json` 与它所引用的轨迹互相矛盾，复查时才发现：

| 项 | 初版 | 修正 | 影响 |
| --- | --- | --- | --- |
| `trace.operations` | `"14"` | `"10"` | 14 既不是总行数（16）也不是写操作数（10），任何按此校验的消费者都会要求一个不存在的轨迹 |
| 写入比例 | 0.6 / 0.25 / 0.15 | 0.375 / 0.25 / 0.375 | 轨迹实为 6 查 4 插 6 更新，与声明不符；轨迹驱动模式下按比例重抽样会得到另一套答案 |
| `run_id` | `dynamic-contract-001` | `stage3-b-exact` | 配置与轨迹行的 run_id 不一致，工件无法关联 |

以上为 11 月 16 日的修正记录。当前[契约第 10 节](stage3-b-contract.md)规定 trace.operations 只计写请求，到达序号稠密，固定轨迹不重新抽样；实际成功序号来自收据，不再由输入轨迹预测。

### 4.1 11 月 17 日复核修复

| 发现 | 修复及证据 |
| --- | --- |
| 输入到达数被误当成提交前缀 | 删除轨迹的 sequence/committed_prefix_at_arrival；按真实收据和查询 S 重建答案，合法并发、失败、拒绝均有回归 |
| 跨运行结果、重复操作及载荷错配可通过 | 检查运行身份、跨类型 operation_id 唯一、一一关联和完整输入定位 |
| DeltaStore 缺全局版本与捕获边界 | WriteSlot 显式传 sequence；新增 CapturePin，SlotRecord/SlotValues 携带 DeltaPin |
| 工作区公式缺参数 | ScratchRequest 显式提供段数、候选预算、复制块数、PQ 上界；本次复审进一步让 Locate 携带 ReadViewLease，加入导航上界及精排预算 |
| 窗口计数与测量序号漏检 | 逐行重算窗口计数并检查完整成功序号区间；pending 与轨迹缺失记录按类型对应 |
| upsert 新 ID 被拒、短成功结果被接受 | upsert 允许插入，成功邻居数必须等于 min(K,live_count) |
| 非零起点没有版本来源 | baseline 显式登记测量起点、逐行版本文件和哈希；预热不混入测量日志 |
| 输入与环境证据不完整 | 核对全部数据哈希、query.ids.u64 映射、载荷登记、答案和 environment；正式测量检查附加证据字段 |
| incomplete/requires_review 仍显示 pass | 保留标志、不计分、退出 2；无效工件覆盖旧 evaluation.json 并退出 1 |

仓库内轨迹、写入记录及有效配置已由生成器同步更新。INDEX-B-1 的输入轨迹不向后兼容，重新生成后使用；公开 API-1.2 与查询/写入输出字段不变。新增回归既检查应拒绝的损坏输入，也检查合法并发、失败、重复查询 ID 和非零起点应通过，避免靠过度拒绝掩盖问题。

### 4.2 本次复审发现与修复

按严重程度排列；下述修改仍未提交：

| 级别 | 发现 | 修复与验证边界 |
| --- | --- | --- |
| P1 | 写→写收据倒序、查→查快照回退及不可确定错误后继续成功写仍可能被接受 | 对全部成功操作检查严格时钟先后与发布前缀，已知不可确定序号不可跨越；新增非法次序反例与合法重叠/相等刻度对照。仅为离线检查，不是并发实现验收 |
| P1 | 普通 ReadView/DeltaPin 拷贝不固定资源，且预留后失败可能留下阻断发布的槽洞 | 新增 ReadViewLease/ReadViewLifetime 所有权声明；CancelLastSlot 仅撤销 WAL 前的未发布尾槽；冻结只能发生在组边界。原生头文件断言通过，具体实现与压力测试仍待后续 |
| P1 | 精排没有预算/期限入口，候选重叠存活与导航开销未能明确计入工作区 | RefinementRequest 携带租约、预算及期限，Expand 显式接收 K；候选两份重叠数组、navigation_scratch_bytes 与共享 workspace 一并计量，消除输入预算和计划结果的循环定义 |
| P1 | 正式测量可用空 Job JSON 冒充证据，资源采样未被消费 | 核对 Job schema/限额/峰值/退出状态与采样身份、时钟、资源类型及预算外披露；新增正反例。只验证文件内部一致性，不能认证证据来源或证明内存比例达标 |
| P2 | 不完整轮次跳过邻居类型校验；跨请求类型的记录可伪装 pending；多余答案与可选配置被忽略 | 结构检查移到未评分返回之前，强制请求类型关联和答案集合约束，校验 workload/measurement/snapshot_policy；新增定向反例 |
| P2 | 基线允许重复非零全局版本，或遗漏起点最后一条写 | 无删除模型中强制非零版本唯一、max(version)=start；修正原先不可能成立的正例，新增损坏基线回归 |

Git 核验：修复工作树 HEAD 与本地 origin/B 均为 `ce4a853`，主工作树 main 为 `8874599`（比 origin/main 多 1 个参赛信息文档提交）；未发现适用 AGENTS.md，原有修改保留且实际暂存区为空。使用独立临时 Git index 将完整 B 树及未提交修复三路应用到本地 main，预演无冲突，未修改主工作树或创建提交。预演只针对本地引用，不代表远端在此后不会变化。

代码和本地合并兼容性检查未发现剩余必须修复的阻塞，可作为修复提交进入 B 并合入 main；这不等于第三阶段正式冻结。修订后的接口仍需双方联合确认，真实 Engine、资源合规与性能验收均未执行。

### 4.3 继续复核修正

| 级别 | 发现 | 修复与验证边界 |
| --- | --- | --- |
| P1 | 缺少版本目录时原契约允许把段内 ID 判为不可见，会静默丢失有效数据 | 可查询段强制具备目录；未适配返回 unsupported，已声明的目录缺失/损坏返回 corruption，读取失败返回 io_error。仅修正声明和契约，真实适配器尚未实现 |
| P1 | 上轮新增的“Job 内核峰值必须不超过限额”拒绝规则与 Windows 已有分配拒绝实验冲突，会把有效原始证据判为损坏 | 保留原始高水位及限额到 resource_evidence，派生 requires_review、不计分并退出 2；memory_acceptance 始终为 not_evaluated，不据此声称资源验收通过 |
| P2 | 实际 direct_reads 可与配置不一致；累计 Job 峰值可以回退 | 检查实际布尔配置一致性，跨不可用采样保持已知累计峰值；新增合法增长和非法回退对照 |
| P2 | 直接调用夹具生成函数会覆盖既有目录；一个写与一个查询、部分小种子会生成失败 | 参数先校验、排他创建新目录；按规模设置可实现的快照覆盖目标，必要时调整查询位置；默认夹具保持可复现 |

五项定向反例在修复前均暴露问题（3 项断言失败、2 项异常），修复后 B 回归 87/87、A 回归 23/23。新增九项覆盖原始峰值超限、实际 I/O 配置、跨不可用采样的峰值单调性、目录覆盖保护及小规模轨迹生成；目录语义为设计修订，未伪造运行时验证。

继续复核结束后重跑：Release/Debug 全目标构建成功，CTest 各 8/8（含 87 项 B 回归），B 契约自检 30 个负例、A 自检及 23 项 A 回归均通过。新目录往返生成的 14 个非 evaluation.json 工件哈希与仓库样例一致；完整修复树再次以临时 index 应用到本地 main 无冲突，实际暂存区仍为空、主工作树仍干净。未提交、合并或推送，也未开始第四阶段实现。

## 5 关键约定

- 版本可见性：某 ID 在快照 S 的有效版本是增量前缀与各段目录中 `version ≤ S` 的最大值；不依赖全局 latest 映射。
- 版本过滤不剪枝：旧节点仍参与导航，只影响结果资格。
- 稳定槽：写槽不等于可见，`PublishPrefix` 只前进，前缀以内不可改写；冻结后切换新活跃段。
- 候选预算：跨段合并前先按段截断；压缩距离只排序探索，返回距离由原向量 float64 重算。
- 构建：独立 Job 提交内存限额，staging 唯一且同卷，发布顺序为段刷盘 → 清单刷盘 → CURRENT 替换，失败不覆盖可用 CURRENT。
- 绕行披露：`unbudgeted_metadata_bytes` 非零时必须写进工件并在内存结论中单列。

## 6 仍未验证

| 事项 | 状态 |
| --- | --- |
| IndexAdapter / DeltaStore 实现 | 未开始，只有声明 |
| ScratchPlan 上界能否装入 workspace | 公式已给，缺实测峰值 |
| 版本目录分页与元数据驻留 | 未实现 |
| 首次构建与发布回滚 | 未实现，仅有契约 |
| PQ 与原生加载期内存的实际字节 | 未知，依赖构建后测量 |
| 并发发布下的前缀一致性 | 缺压力测试 |

## 7 与 A 的待确认点

1. `Retire` 的调用时机需要 B 的视图引用计数归零作为前置条件，A 不自行推断。
2. 目录对象与稀疏定位占 metadata，块缓冲沿用 A 的 cache/workspace 计费；需联合验证驱逐压力与实际峰值。
3. 构建子进程必须显式加入同一个 Job，A 的限额只对已加入进程生效。
4. 预取计数口径：B 记 `admitted/consumed`，A 记真实提交与消费，两边差值需在联合复核时对齐。
5. `Manifest` 的字节编码尚未实现，`EncodeManifest`/`DecodeManifest` 的容量与损坏语义需与 FORMAT-1 第 5 节逐条对齐。

本次仅完成所列设计、消费端及回归修复；没有执行真实 Engine 动态负载、正式资源合规或性能验收，也不替代另一成员的接口审阅。
