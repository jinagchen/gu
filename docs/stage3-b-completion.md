# 第三阶段 B 交付记录

日期：2026 年 11 月 16 日。版本 INDEX-B-1，对应 API-1.2、EVAL-1.1、STORAGE-A-1。本次交付 B 侧内部契约、首次构建与动态运行配置、动态轨迹与独立答案的生成/校验代码，以及一组自洽样例。**索引层本身仍未实现**：`index.hpp` 全是声明，没有 IndexAdapter、DeltaStore、构建或合并代码。

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
| [tools/contracts/check-stage3-b.py](../tools/contracts/check-stage3-b.py) | 配置规则、轨迹顺序、精确答案一致性检查，外加 15 个反例 |

## 2 验证结果

构建与测试（Windows 原生，MSVC 19.44，`/W4 /WX`）：

```text
cmake -S . -B build\windows -G "Visual Studio 17 2022" -A x64   -> done
cmake --build build\windows --config Release                     -> 全目标成功
ctest --test-dir build\windows -C Release                        -> 5/5 passed
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

修正的同时把三条规则写进[契约第 10 节](stage3-b-contract.md)并补了对应反例，否则下次还会犯同样的错。第 10 节还明确了 `trace.operations` 的语义（写操作条数，不含查询）、到达序号稠密性、写序号连续性，以及"轨迹驱动不重新抽样"的约束。

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
2. 版本目录页读取计入 metadata 预算，需与 A 的缓存准入共同验证驱逐压力下的表现。
3. 构建子进程必须显式加入同一个 Job，A 的限额只对已加入进程生效。
4. 预取计数口径：B 记 `admitted/consumed`，A 记真实提交与消费，两边差值需在联合复核时对齐。
5. `Manifest` 的字节编码尚未实现，`EncodeManifest`/`DecodeManifest` 的容量与损坏语义需与 FORMAT-1 第 5 节逐条对齐。
