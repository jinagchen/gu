# 向量检索 I/O 优化参赛项目

技术路线为 C++20 核心加 Python 评测。2026 年 11 月 10 日用户确定使用本机原生 Windows 开发、运行和评测；当前设计为 DESIGN-1.1，异步后端采用 IOCP。阶段二工程可行性验收已通过：DiskANN 固定提交及补丁、小数据构建/加载/查询、读接口替换、隔离依赖、数据卷及资源计量均有实测。完整 Engine 与正式性能实验仍未实现。

## 阶段一交付

- [需求确认基线](docs/requirements.md)：22 项可追踪需求，区分任务书规定、选定优化方向和工程约定。
- [验收矩阵](docs/acceptance-matrix.md)：逐项测试方法、通过条件和证据，全部实现验收尚未执行。
- [待确认事项](docs/open-questions.md)：10 项官方口径及团队资源记录；任务分配 Q-11 和本机工程可行性 Q-12 已关闭，官方口径继续跟进。

环境与技术可行性验证已完成本机阶段二范围。内存分母、正式数据与负载、提交日期等未定前，不宣称正式规则已经全部确认。两人任务分配已完成；按用户要求不收集姓名、熟练度或工时，八周仅为相对推进参考。

## 总体设计与接口

- [总体设计](docs/architecture.md)：模块、快照、数据流、并发与内存边界，以及阶段三待验证项。
- [C++ 接口约定](docs/interface-contract.md)：公开 API、错误、准入、所有权和内部读取契约。
- [存储格式与恢复](docs/storage-protocol.md)：WAL 字节布局、清单、发布与恢复时序。
- [评测协议](docs/evaluation-protocol.md)：C++/Python 边界、快照答案、时间与结果工件。

公开声明位于 [engine.hpp](include/agent_io/engine.hpp) 和 [storage.hpp](include/agent_io/storage.hpp)，目前仅有声明，不能链接成引擎。协议示例放在 configs/examples 和 evaluation/examples；阶段二小数据实测另见[验收记录](docs/stage2-completion.md)，不与协议示例混用。

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

以下为目标结构。CMake 已可构建接口检查、Windows 环境探针、Job Object 启动器及可选 DiskANN 读取适配探针；include/agent_io 已有接口声明，configs/examples 与 evaluation/examples 已有协议示例；引擎实现及正式命令尚未完成。

```text
CMakeLists.txt
include/agent_io/     核心公开接口
src/storage/          同步读写、块格式和后续 Windows IOCP 后端
src/cache/            固定容量缓存、准入和在途读取去重
src/index/            检索与增量索引适配
src/update/           WAL、版本信息和后台合并
tools/                已有环境探针和接口检查；后续增加命令行与基准
tests/                核心正确性、恢复和并发测试
evaluation/           Python 数据处理、实验编排和结果分析
configs/              可复现实验配置
docs/                 设计、实验方法和相关工作
```

## 首个里程碑

先交付一个可编译、可复现的同步磁盘检索基线：

1. 已完成阶段二：固定 DiskANN 0.6.1 提交及补丁，跑通小规模构建、加载和静态查询。
2. 定义存储读取接口，接入同步磁盘读取和实际读取量统计。
3. 由 C++ 基准程序输出配置、完成请求数、查询延迟、读取量与内存指标；保留查询结果 ID 供准确性检查。
4. 由 Python 对照精确结果计算 Recall@10，保存输入配置、原始结果和汇总表。
5. 确认内存统计口径和召回达标后，开始缓存与异步预取消融。

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

当前命令构建并验证环境与内存探针，不会产生完整 Engine。普通终端若无法找到 cmake，使用[环境记录中的随附工具路径](docs/environment-validation.md)。

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
