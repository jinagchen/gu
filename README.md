# proj47-RecallFlow

| 参赛信息 | 内容 |
| --- | --- |
| 高校名称 | 中北大学 |
| 队伍 ID | T2026101109913134 |
| 队伍名 | 页对齐 |
| 赛题编号 | proj47 |
| 赛题名称 | 面向 Agent 记忆的向量检索系统 I/O 优化 |

技术路线为 C++20 核心加 Python 评测。本机使用原生 Windows，异步后端设计采用 IOCP。截至 2026 年 11 月 20 日，阶段二本机工程可行性和阶段三设计验收已通过，DESIGN-1.1 已冻结。阶段四已交付同步存储和静态 C++ 并发基准，当前待接入真实静态 Engine 并完成独立召回与资源评测。

华北区域赛初赛作品提交日为 **2026 年 12 月 11 日**，希冀赛题窗口截至当日 **23:59**。初赛材料包含现阶段源代码和设计实现文档。队伍最终提交流程、正式数据与内存计量口径继续跟进。[赛程来源和待确认事项](docs/open-questions.md)

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
- [待确认事项](docs/open-questions.md)：Q-01 已登记初赛赛程和材料要求，任务分配 Q-11 和本机工程可行性 Q-12 已关闭。

两人任务分配已完成。按用户要求，姓名、熟练度和工时无需登记。八周计划供研发排期参考，初赛材料按公布的赛程准备。

## 总体设计与接口

- [总体设计](docs/architecture.md)：模块、快照、数据流、并发与内存边界，以及阶段三待验证项。
- [C++ 接口约定](docs/interface-contract.md)：公开 API、错误、准入、所有权和内部读取契约。
- [存储格式与恢复](docs/storage-protocol.md)：WAL 字节布局、清单、发布与恢复时序。
- [评测协议](docs/evaluation-protocol.md)：C++/Python 边界、快照答案、时间与结果工件。
- [第三阶段 A 交付](docs/stage3-a-completion.md)：API-1.2 读取/资源/WAL 声明、预算切换及 EVAL-1.1 基准输出与自检。
- [第三阶段 B 交付](docs/stage3-b-completion.md)：索引/增量内部契约、首次构建与动态运行配置、轨迹与独立答案的生成和校验。
- [第三阶段联合复核与冻结](docs/stage3-freeze.md)：2026 年 11 月 17 日关闭五处交接，冻结 DESIGN-1.1、STORAGE-A-1.1 与 INDEX-B-1.2，第三阶段设计验收通过。

公开声明位于 [engine.hpp](include/agent_io/engine.hpp)、[storage.hpp](include/agent_io/storage.hpp) 和 [index.hpp](include/agent_io/index.hpp)。Engine 与索引层仍只有声明，同步存储已有独立实现，尚不能链接成完整引擎。协议示例放在 configs/examples 和 evaluation/examples；阶段二小数据实测另见[验收记录](docs/stage2-completion.md)，不与协议示例混用。

2026 年 11 月 12 日开始第四阶段 A：已新增可链接的 `agent_io_storage` 静态库，实现同步读取、文件/缓冲租约、预算预约和关闭缓存/预取的批读取，并接入现有 DiskANN 小数据探针。两次独立精确答案检查 Recall@10 均为 1.0，范围和命令见[同步存储交接记录](docs/stage4-a-storage.md)。

2026 年 11 月 15 日补齐 A 侧静态 C++ 并发基准运行器：`agent_io_benchmark`、`vector_bench` 的有界队列/结果槽、独立日志、阶段计时、吞吐与延迟直方图、资源采样及 EVAL-1.1 工件输出。显式 `contract-test` 目标验证生产端，拒绝正式 measurement；真实 Engine 目标仍待 B 接入，第四阶段整体验收尚未完成。命令与接入边界见[基准交付记录](docs/stage4-a-benchmark.md)。

2026 年 11 月 16 日交付第三阶段 B；11 月 17 日修订为 INDEX-B-1.1，修复动态快照、结果关联、upsert、计数和输入校验，补齐 DeltaStore 全局序号/捕获前缀及 ScratchRequest 参数。输入轨迹不再预设写序号或查询快照，旧轨迹须重新生成。索引层仍是声明，合成召回 1.0 来自精确扫描，不作为性能或近似算法结论。见[B 侧交付记录](docs/stage3-b-completion.md)。

2026 年 11 月 17 日按用户指示完成 A/B 跨模块联合复核：统一旧资源回收、预算与构建 Job 边界，INDEX-B-1.2 补齐 Manifest 的恢复引用和逻辑读取/预取诊断；DESIGN-1.1 已冻结。记录注明由 Codex 执行复核，不虚构两名成员签字；实现与性能继续按后续阶段验收。

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

## 计划目录

以下为目标结构。CMake 已可构建存储与基准库、相应测试、接口检查、Windows 环境探针、Job Object 启动器及可选 DiskANN 读取适配探针。已有固定 Boost 头文件时还构建 vector_bench；真实引擎与首次构建尚未完成，Python 评测入口已可用。

```text
CMakeLists.txt
include/agent_io/     核心公开接口
src/storage/          同步读写、块格式和后续 Windows IOCP 后端
src/benchmark/        静态开放式并发基准运行器
src/cache/            固定容量缓存、准入和在途读取去重
src/index/            检索与增量索引适配
src/update/           WAL、版本信息和后台合并
tools/                环境探针、接口检查、基准 CLI 与生产端集成检查
tests/                核心正确性、恢复和并发测试
evaluation/           Python 数据处理、实验编排和结果分析
configs/              可复现实验配置
docs/                 设计、实验方法和相关工作
```

## 首个里程碑

先交付一个可编译、可复现的同步磁盘检索基线：

1. 已完成阶段二：固定 DiskANN 0.6.1 提交及补丁，跑通小规模构建、加载和静态查询。
2. 已交付阶段四 A 侧同步存储和静态基准生产端，分别通过小数据探针和合成目标检查。
3. 待实现 `IndexAdapter`、`vector_build` 和静态 `Engine`，将真实查询接入 `vector_bench`，保存结果 ID、读取量、延迟和资源工件。
4. 由 Python 对照独立精确答案计算 Recall@10，联合复核召回、内存和重复运行证据，通过静态基线验收后再开展缓存与预取消融。

静态基线通过后再加入插入、更新、WAL 和后台合并，动态评测使用对应数据版本的真实答案。

## 开发环境

用户已确定使用当前本机 Windows 11，服务器不承担当前开发及实验任务；已有租用实例保留为历史资源记录。无需 WSL 或 Linux。官方评测平台仍待主办方材料确认，本机选择不代表官方环境已经确认。

核心采用 MSVC、CMake、Windows SDK、同步 ReadFile 和 Overlapped I/O + IOCP。无缓冲读取使用 FILE_FLAG_NO_BUFFERING，WAL 刷盘使用 FlushFileBuffers。详细设计见 [Windows 平台约定](docs/windows-platform.md)，实测范围与限制见[环境验证记录](docs/environment-validation.md)。

在已加载 Visual Studio 工具环境的 PowerShell 中运行：

```powershell
cmake -S . -B build/windows -G "Visual Studio 17 2022" -A x64
cmake --build build/windows --config Release
ctest --test-dir build/windows -C Release -V
```

当前命令构建同步存储及并发基准，验证存储、基准、环境与内存探针，不会产生完整 Engine。普通终端若无法找到 cmake，使用[环境记录中的随附工具路径](docs/environment-validation.md)。CLI 的协议验证命令见[基准交付记录](docs/stage4-a-benchmark.md)。

Python 侧校验命令（使用 `.venv` 中的解释器）：

```powershell
python tools/contracts/check-stage3-b.py
python tools/contracts/test-stage3-b.py
python -m evaluation.check --run evaluation/examples/stage3-b
```

前两条检查配置、30 个原有反例和动态回归场景；最后一条校验运行目录并计算 Recall@10，结果写入该目录的 evaluation.json。退出 1 为工件无效，退出 2 为 incomplete/requires_review、不计召回；退出 0 仅表示完整工件已计分。DESIGN-1.1 已按[冻结记录](docs/stage3-freeze.md)完成复核；新增清单 schema 检查运行 `python tools/contracts/test-stage3-freeze.py`，不替代真实 Engine 验收。

完整阶段二验证使用 `.\tools\environment\bootstrap-stage2.ps1`，包括固定依赖、Release/Debug 检查、Python 隔离依赖、小数据查询、故障注入及新旧索引共存资源演练。默认数据目录为 `D:\agent-io-stage2\20261110`。依赖与许可证见[第三方记录](docs/third-party.md)，结果、容量估算和限制见[阶段二验收](docs/stage2-completion.md)。

完整架构、内存预算、实验矩阵和排期见同目录参赛技术方案。

## 项目文档

- [赛题任务书](赛题任务书-面向Agent记忆的向量检索系统IO优化.md)：合并 DOCX 转换版和用户提供文本，统一排版，保留完整实质内容、参考资料和整理说明。
- [参赛技术方案](参赛技术方案.md)：系统架构、优化策略、评测方法和技术风险。
- [项目全生命周期](项目全生命周期.md)：从启动到提交、答辩和维护的阶段任务、产物、验收条件和延期处理。
- [环境验证记录](docs/environment-validation.md)：本机与服务器的职责、已验证事实及远程验证清单。
- [两人协作计划](docs/team-plan.md)：两条工作线、范围优先级、八周草案和协作约定。
- [Windows 平台约定](docs/windows-platform.md)：本机决策、IOCP、无缓冲对齐、持久化和内存计量边界。
- [前三阶段检查](docs/stages-1-3-review.md)：缺口及后续关闭进展。
- [阶段二验收](docs/stage2-completion.md)：小数据实测、读取适配、资源口径、容量估算与后续边界。
