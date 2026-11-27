# proj47-RecallFlow

| 参赛信息 | 内容 |
| --- | --- |
| 高校名称 | 中北大学 |
| 队伍 ID | T2026101109913134 |
| 队伍名 | 页对齐 |
| 赛题编号 | proj47 |
| 赛题名称 | 面向 Agent 记忆的向量检索系统 I/O 优化 |

技术路线为 C++20 核心加 Python 评测，当前平台为原生 Windows x64。**截至 2026 年 11 月 23 日，静态检索和读优化可运行，WAL 底层已交付，但 Engine 仍不能插入、更新或重放日志。** [项目状态与下一步](docs/project-status.md)集中记录能力、验证结果和验收缺口。

华北区域赛**报名截止：2026 年 12 月 6 日**；**初赛作品提交截止：2026 年 12 月 11 日 23:59**（希冀赛题窗口）。准备完整工程、设计实现文档、过程资料、进展汇报幻灯片和仓库内解说视频；用队伍账号核对报名状态、提交入口和回执。[赛程与提交清单](docs/open-questions.md#已核实的赛程与提交信息)

## 许可证

Copyright 2026 proj47-RecallFlow contributors.

除另有标注或属于下述第三方材料外，本项目按内容类型分别授权：

| 内容 | 许可证 | 适用范围 |
| --- | --- | --- |
| 团队自有源代码、脚本及构建和运行配置 | [Apache License 2.0](LICENSE)（Apache-2.0） | `include/`、`src/`、`tests/`、`tools/`、`evaluation/`、`configs/` 中的自有软件内容及 `CMakeLists.txt` |
| 团队原创项目文档 | [知识共享署名-相同方式共享 4.0 国际许可协议](LICENSE-docs)（CC-BY-SA-4.0） | 本 README、`docs/` 中的原创文档、[参赛技术方案](参赛技术方案.md)、[项目全生命周期](项目全生命周期.md)及其导出版本 |

文档署名为 proj47-RecallFlow（页对齐团队）及项目贡献者；单独分享或导出文档时，应保留署名、许可声明和许可文本或链接，修改时注明变更。

[赛题任务书](赛题任务书-面向Agent记忆的向量检索系统IO优化.md)为外部原文整理件，其原文不纳入团队原创文档的授权范围。第三方代码、补丁中的上游内容、引用材料和外部数据集保留各自原有权利及许可条件，详见[第三方依赖与许可证记录](docs/third-party.md)。

## 阶段一交付

- [需求确认基线](docs/requirements.md)：22 项可追踪需求，区分任务书规定、选定优化方向和工程约定。
- [验收矩阵](docs/acceptance-matrix.md)：逐项测试方法、通过条件和已有组件级证据，完整系统验收待执行。
- [待确认事项](docs/open-questions.md)记录部分确认的 Q-01、Q-09 和 8 项待官方确认的问题。任务分配 Q-11 和本机工程可行性 Q-12 已关闭。

两人任务分配已完成。按用户要求，姓名、熟练度和工时无需登记。八周计划供研发排期参考，初赛材料按公布的赛程准备。

## 总体设计与接口

- [总体设计](docs/architecture.md)：模块、快照、数据流、并发与内存边界，以及阶段三待验证项。
- [C++ 接口约定](docs/interface-contract.md)：公开 API、错误、准入、所有权和内部读取契约。
- [存储格式与恢复](docs/storage-protocol.md)：WAL 字节布局、清单、发布与恢复时序。
- [评测协议](docs/evaluation-protocol.md)：C++/Python 边界、快照答案、时间与结果工件。
- [第三阶段交付与设计冻结](docs/stage3-freeze.md)：A/B 交付、协议回归与五处交接，冻结 DESIGN-1.1、STORAGE-A-1.1 与 INDEX-B-1.2。
- [第四阶段 A 交付](docs/stage4-a-storage.md)：同步存储与并发基准生产端。
- [第四阶段 B 交付](docs/stage4-b-completion.md)：真实索引适配、首次构建、静态 Engine、真实基准目标与评测闭环；含静态基线结果、重复运行对比与已知限制。
- [第五阶段 A 交付](docs/stage5-a-completion.md)：有界 CLOCK 缓存、同块在途合并、Windows IOCP、下一跳预取和静态对照结果。
- [第五阶段 B 交付](docs/stage5-b-completion.md)：十组对照的独立复核、4096 节点上的缓存/工作集连续谱、有效缓存容量为标称值 45.07% 的记账偏差，以及 `read_path` 的消费端校验。
- [第六阶段 A：WAL 底层](docs/stage6-a-wal.md)：FORMAT-1 编解码、顺序追加、刷盘、轮换和故障验证。
- [第六阶段 B：动态闭环](docs/stage6-b-completion.md)：序号分配、增量层发布、查询融合与 WAL 重放；确认与可见延迟、恢复耗时和写放大的实测。
- [第七阶段 A：固定后台读配额](docs/stage7-a-read-scheduling.md)：后台读按预留槽调度，可独立于合并任务启用。
- [第七阶段 B：检查点、合并与回收](docs/stage7-b-completion.md)：增量落段、清单代次切换、后台合并、旧段目录回收，维护策略的四臂消融与定速负载（声明 200 写/秒，上限 200–400 写/秒）。

公开声明位于 [engine.hpp](include/agent_io/engine.hpp)、[storage.hpp](include/agent_io/storage.hpp) 和 [index.hpp](include/agent_io/index.hpp)。Engine 已按声明实现静态查询、动态写入、恢复、检查点与后台整理；同步存储已有独立实现。协议示例放在 configs/examples 和 evaluation/examples；阶段二小数据实测另见[验收记录](docs/stage2-completion.md)，不与协议示例混用。

`vector_bench --target engine` 对真实静态索引执行查询；`--target contract-test` 只生成合成的协议检查工件，不可用于性能测量。动态轨迹与独立答案校验已具备工具和样例，但不等于 C++ 动态运行已接通。

## 两人分工

- A 主抓 C++ 存储、缓存、异步 I/O、WAL 底层与基准计时。
- B 主抓 C++ 动态索引与合并，以及 Python 正确性和性能评测。
- 两人共同完成协议设计、故障验证、文档、集成和答辩。高级联合控制等增强项在基础范围稳定后推进。

任务归属、交付物、交接和复核已定稿，见[两人协作计划](docs/team-plan.md)。成员姓名及每周工时不作为阶段一完成条件。

## 模块边界

| 部分 | 语言 | 职责 |
| --- | --- | --- |
| 核心引擎 | C++20 | 磁盘图检索、块缓存、异步 I/O、WAL、增量索引、版本管理和合并 |
| 基准程序 | C++20 | 并发请求发出、延迟计时、吞吐统计和引擎指标采集 |
| 评测工具 | Python | 准备数据、编排实验、检查召回与更新正确性、汇总结果和绘图 |

先通过命令行参数、配置文件和 JSON 结果文件连接两部分。Python 启动 C++ 基准程序，C++ 在进程内完成测量后输出结果。后续有实际集成需求时再考虑语言绑定。

## 仓库目录

下列目录均已存在。`vector_bench` 需要锁定的 Boost 1.78 头文件；核心库、`vector_build` 与 WAL 测试不依赖 DiskANN 二进制。

```text
CMakeLists.txt
include/agent_io/     核心公开接口
src/storage/          同步与 Windows IOCP 读取、块缓存、预取和 WAL
src/index/            清单与版本目录、首建与图适配
src/engine/           静态查询引擎（写路径返回 unsupported）
src/benchmark/        静态开放式并发基准运行器
tools/                环境探针、基准与首建 CLI、契约自检与基线驱动
tests/                静态检索、并发读和 WAL 底层故障测试
evaluation/           Python 数据处理、实验编排和结果分析
configs/              可复现实验配置
docs/                 设计、实验方法和相关工作
```

## 首个里程碑

先交付一个可编译、可复现的同步磁盘检索基线：

1. 已完成阶段二：固定 DiskANN 0.6.1 提交及补丁，跑通小规模构建、加载和静态查询。
2. 已交付阶段四 A 侧同步存储和静态基准生产端，分别通过小数据探针和合成目标检查。
3. 已将 `IndexAdapter`、`vector_build` 和静态 `Engine` 接入 `vector_bench`，Python 独立计算 Recall@10，并复核内存与重复运行证据。
4. 已完成第五阶段 A 的静态读路径对照，记录缓存、IOCP 与预取对读取量、延迟及资源的影响；B 已独立复核并补测缓存/工作集连续谱，见[第五阶段 B 交付](docs/stage5-b-completion.md)。
5. 第六阶段已实现 WAL 底层与引擎动态闭环：确认后可见、重启可恢复、旧版本不冒充当前，见[第六阶段 B 交付](docs/stage6-b-completion.md)。
6. 第七阶段已实现整理闭环：增量落段、清单代次切换、后台合并与旧段目录回收，`Checkpoint` 不再是占位；策略对照与定速负载见[第七阶段 B 交付](docs/stage7-b-completion.md)。定速实测给出 200 写/秒的下限，并暴露读并发会吃掉写入（90–119 写/秒）；P99、多写线程与 EVAL-1.1 动态轨迹仍待后续。

动态评测使用对应数据版本的真实答案；正式实验矩阵属于第八阶段。

## 开发环境

用户已确定使用当前本机 Windows 11，服务器不承担当前开发及实验任务；已有租用实例保留为历史资源记录。无需 WSL 或 Linux。官方评测平台仍待主办方材料确认，本机选择不代表官方环境已经确认。

核心采用 MSVC、CMake、Windows SDK、同步 ReadFile 和 Overlapped I/O + IOCP。无缓冲读取使用 FILE_FLAG_NO_BUFFERING，WAL 刷盘使用 FlushFileBuffers。详细设计见 [Windows 平台约定](docs/windows-platform.md)，本机配置与实测范围见[阶段二环境与验收](docs/stage2-completion.md)。

### 完整测试的前置条件

- **使用 ASCII 路径。** 建议把源码检出到 `D:\agent-io`，并使用只含 ASCII 字符的构建、运行和临时目录。含中文等非 ASCII 字符的路径传给首建时，可能报 `Input paths must be ASCII`。`build_contract` 例外：目标路径非 ASCII 时它会把用例工作区移到 ASCII 临时目录，因此不因检出目录名失败；显式传 `--output` 的其他脚本仍需 ASCII 输出目录。在运行下方命令前，先创建 `D:\agent-io-tmp`，并在当前 PowerShell 中将 `TEMP` 和 `TMP` 都设为该目录。
- **使用 LF 检出。** `.gitattributes` 默认以 LF 检出文本，标记为 `-text` 的历史工件和补丁保留原始字节。已有工作副本可用 `git ls-files --eol -- evaluation/examples/stage3-b/trace.jsonl` 检查是否为 `w/lf`；如果看到 `w/crlf` 或测试报 `Checksum mismatch: trace.jsonl`，请将包含此规则的版本重新检出到新的 ASCII 目录，保留原工作副本中的未提交改动。

在已加载 Visual Studio 工具环境的 PowerShell 中运行：

```powershell
cmake -S . -B build/windows -G "Visual Studio 17 2022" -A x64
cmake --build build/windows --config Release
ctest --test-dir build/windows -C Release -V
```

如 `vector_bench` 未生成，配置时设置 `AGENT_IO_BOOST_INCLUDE` 指向固定 Boost 的 `include` 目录。完整 CTest 还需要 `AGENT_IO_TEST_PYTHON` 指向已安装锁定依赖的解释器；默认是仓库 `.venv/Scripts/python.exe`。普通终端找不到 `cmake` 时，使用[随附工具路径](docs/stage2-completion.md#61-独立环境探针)。

阶段七的整理路径由 `maintenance_test` 覆盖（检查点、合并、代次切换、回收与策略消融，约 9 秒）：

```powershell
cmake --build build/windows --config Release --target maintenance_test
ctest --test-dir build/windows -C Release -R '^maintenance$' --output-on-failure
```

Python 侧校验命令（使用 `.venv` 中的解释器）：

```powershell
.\.venv\Scripts\python.exe tools/contracts/check-stage3-b.py
.\.venv\Scripts\python.exe tools/contracts/test-stage3-b.py
.\.venv\Scripts\python.exe tools/contracts/run-static-baseline.py --root D:/agent-io-runs --run-id static-001
.\.venv\Scripts\python.exe tools/storage/run-stage5.py --output D:/agent-io-runs/read-compare-001
.\.venv\Scripts\python.exe tools/storage/run-stage5-b.py --output D:/agent-io-runs/read-sweep-001 --vectors 4096
.\.venv\Scripts\python.exe tools/storage/verify-read-path.py --run D:/agent-io-runs/read-sweep-001/sweep-16384kib-synchronous
.\.venv\Scripts\python.exe tools/dynamic/run-stage6-b.py --output D:/agent-io-runs/write-sweep-001 --write-counts 50 200 800
```

前两条检查配置、协议反例和动态评测回归。中间三条默认使用 `build/windows/Release` 的基准程序，输出须使用新目录（静态基线允许空目录）。`run-static-baseline.py` 在 Job 限额内完成首建、测量与离线评测；`run-stage5.py` 做五种读模式、两种缓存预算的对照；`run-stage5-b.py` 在一个装不下缓存的较大图上扫缓存与预取窗口；`verify-read-path.py` 校验 `summary.json` 的读路径统计，可对任意运行目录重复执行；`run-stage6-b.py` 逐档写入量测量确认与可见延迟、恢复耗时，并校验 WAL 字节数等于写次数乘单帧大小。单独运行 `evaluation.check` 会重写目标目录的 `evaluation.json`；复查受版本控制的示例时请先复制到临时目录。退出 0 只表示工件完整且已计分，是否达标另查 `verdict`；退出 1 为无效，2 为不计召回的 `incomplete` 或 `requires_review`。

完整阶段二验证使用 `.\tools\environment\bootstrap-stage2.ps1`，包括固定依赖、Release/Debug 检查、Python 隔离依赖、小数据查询、故障注入及新旧索引共存资源演练。默认数据目录为 `D:\agent-io-stage2\20261110`。依赖与许可证见[第三方记录](docs/third-party.md)，结果、容量估算和限制见[阶段二验收](docs/stage2-completion.md)。

完整架构、内存预算、实验矩阵和排期见同目录参赛技术方案。

## 项目文档

- [赛题任务书](赛题任务书-面向Agent记忆的向量检索系统IO优化.md)：赛题要求、参考资料和评审要点的外部原文整理件。
- [参赛技术方案](参赛技术方案.md)：系统架构、优化策略、评测方法和技术风险。
- [项目全生命周期](项目全生命周期.md)：从启动到提交、答辩和维护的阶段任务、产物、验收条件和延期处理。
- [两人协作计划](docs/team-plan.md)：两条工作线、范围优先级、八周草案和协作约定。
- [Windows 平台约定](docs/windows-platform.md)：本机决策、IOCP、无缓冲对齐、持久化和内存计量边界。
- [阶段二环境与验收](docs/stage2-completion.md)：本机配置、探针复现、小数据实测、资源口径和容量估算。
