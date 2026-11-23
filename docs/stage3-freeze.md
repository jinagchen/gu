# 第三阶段联合复核与设计冻结

日期：2026 年 11 月 17 日。状态：第三阶段设计退出条件通过，DESIGN-1.1 冻结。

本次按用户“完成双方联合复核、设计冻结”的指示，由 Codex 对 A/B 两侧交付进行跨模块核对、修订及本机验证。A/B 是模块责任标识；此记录不表示两名团队成员已分别签字。用户本次指示授权完成该项工程复核，不再把另行收集成员签字作为阶段退出条件。

## 1 冻结范围

| 基线 | 冻结内容 |
| --- | --- |
| DESIGN-1.1 | 本机 Windows、模块边界、快照、资源生命周期、更新/恢复及维护时序 |
| API-1.2 | Engine、存储与 WAL 抽象接口；公开 API 不变 |
| STORAGE-A-1.1 | Retire 前置条件、目录/缓冲计费、离线构建与在线维护的 Job 边界 |
| INDEX-B-1.2 | 增量前缀、ReadViewLease、候选预算、逻辑读取指标及完整 Manifest 内存类型 |
| FORMAT-1 | WAL 与版本目录原字节布局、清单 UTF-8 JSON schema、发布和恢复规则 |
| EVAL-1.1 | C++ 生产端与 Python 消费端的身份、时钟、版本、守恒及输出字段 |

依据：[总体设计](architecture.md)、[公开接口](interface-contract.md)、[A 契约](stage3-a-contract.md)、[B 契约](stage3-b-contract.md)、[存储协议](storage-protocol.md)、[评测协议](evaluation-protocol.md)。冻结的是实现遵循的协议；完整 Engine、真实索引与故障恢复不作为本阶段交付。

## 2 五处交接关闭

| 项 | 联合结论与修订 | 实现阶段验证 |
| --- | --- | --- |
| 旧文件退休与删除 | B 在新清单发布、旧发布者/ReadViewLease 释放及恢复安全成立后，锁外请求 Retire；A 继续保留独立块租约与在途读，最后引用释放后才能删除。自动删除仍受 Windows 发布/恢复故障验收门槛约束 | 长查询跨代次、超时尾部与失败删除重试 |
| 目录与缓冲计费 | 目录对象、稀疏定位、视图控制块占 metadata；缓存块及已驱逐固定块占 cache，禁用缓存或需求回退占 workspace；适配器复制与 scratch 另占 workspace。同一分配不重复计费，所有存活分配持续预约 | 驱逐压力、第三方 scratch 与新旧代资源峰值 |
| 构建 Job 归属 | 离线首次构建独立 Job；在线维护子进程加入引擎同一 Job，并由后台 workspace 预约覆盖。挂起创建、先加入再恢复；加入失败不得启动构建。现有 job_runner 提供进程树机制，不冒充在线维护实现 | 真实维护子进程、超限/加入失败与清理 |
| 读取和预取计数 | 删除查询侧“真实 I/O”歧义：ExpansionResult 使用 logical_blocks_read/logical_bytes_read；prefetch_admitted 与 prefetched_keys_used 是本次 Expand 诊断。全局接管、完成字节及每驻留周期首次消费只取 A 的 EngineStats；禁止并发 Stats 差值分摊或把查询指标相加冒充全局消费 | 同键共享、跨查询消费、驱逐后重取与失败 |
| Manifest 编码与恢复引用 | 原声明缺 adapter、格式/回放字段与 segment.json 路径/哈希，现已补齐定长类型、段 UUID 和文件引用；FORMAT-1 第 2.1 节固定完整 schema、64 KiB 上限、容量不足不写、错误分类及 Open 的内容哈希校验职责 | C++ 编解码、损坏工件、真实发布与恢复回滚 |

本次还修正：B 首建流程须先把段移到正式位置再发布清单；离线构建与在线合并使用不同的计量边界；CURRENT 替换失败不能假定自动回滚；节点行可跨多个对齐块，编译断言说明不再假定一行只能占一块。

## 3 阶段退出推演

| 必验场景 | 核对结论 |
| --- | --- |
| 更新与查询重叠 | Q1 固定 S=7 及 DeltaPin；后续序号 8 不进入它的前缀。Q1 可用旧导航节点但只能返回最高可见版本≤7；同步发布后的新查询看到 8。取消未发布尾槽只允许 WAL 追加前，追加后失败进入 recovery_required |
| 合并时查询旧图 | 切点 C=10 的新段发布时拼回>10 的尾部，S 不回退。Q1 的旧 ReadViewLease 继续固定旧资源；B 等旧视图释放后请求 Retire，A 再等独立块/在途引用；两代都计费，不能靠删旧文件避开预算 |
| WAL 写到一半退出 | 最新日志真实 EOF 的 need_more 可丢弃未完成尾，不确认该条；历史封闭日志缺尾或完整帧 CRC 错误报 corruption。完整同步后未响应的写可在恢复出现，不能补成“未生效” |
| 设备读取失败 | Q1/Q2 共享读取时，Q1 超时只解除自身等待；缓冲/文件/预算等到最终完成。完成失败传给仍等待的 Q2，半块不发布；结果以错误计数，不能补旧版本维持成功 |

追加核对：预算模式切换失败保持原模式；Sync 失败停止 writer 与后续成功发布；Checkpoint 发布失败保留旧文件与 WAL，并核查 CURRENT 实际状态。上述为设计推演，运行时故障注入在对应实现阶段验收。

## 4 验证证据

本机 Windows/MSVC C++20，沿用现有 Visual Studio 2022 构建目录。

| 检查 | 结果与边界 |
| --- | --- |
| Release/Debug 全目标构建 | 均通过，包括两侧接口编译断言；不包含完整 Engine 链接 |
| A 契约自检与回归 | 自检通过，23/23 回归 |
| B 契约自检与回归 | 自检通过，30 个损坏反例按预期拒绝，87/87 回归 |
| Manifest 契约检查 | 22/22；覆盖字段缺失/未知、身份、越界、布局、路径、长度、哈希字段与合法多块行；只验证 schema 示例，不是 C++ codec 实现 |
| Release/Debug CTest | 各 9/9 通过；包含存储、基准、进程树与提交拒绝探针及新增清单检查 |

重跑命令：

```powershell
$stage3Cmake = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$stage3Ctest = Join-Path (Split-Path $stage3Cmake) 'ctest.exe'
& $stage3Cmake --build build/windows --config Release
& $stage3Cmake --build build/windows --config Debug
& $stage3Ctest --test-dir build/windows -C Release --output-on-failure
& $stage3Ctest --test-dir build/windows -C Debug --output-on-failure
.\.venv\Scripts\python.exe tools/contracts/check-stage3-a.py
.\.venv\Scripts\python.exe tools/contracts/test-stage3-a.py
.\.venv\Scripts\python.exe tools/contracts/check-stage3-b.py
.\.venv\Scripts\python.exe tools/contracts/test-stage3-b.py
.\.venv\Scripts\python.exe tools/contracts/test-stage3-freeze.py
```

证据：[验证汇总及源码哈希](evidence/stage3-freeze/summary.json)、[Release CTest](evidence/stage3-freeze/ctest-Release.log)、[Debug CTest](evidence/stage3-freeze/ctest-Debug.log)。现有构建目录 Release 出现 MSB8028 中间目录共享警告，构建成功；该目录历史状态不作为协议正确性证据。

## 5 冻结后的变更与交接

修改公开 API、内部字段语义或磁盘 schema 时，先升级对应版本，再同时更新头文件、协议、配置/夹具、生产端与消费端及检查。INDEX-B-1.1 内部字段不继续当作新实现入口；INDEX-B-1.2 的逻辑统计与完整清单类型为实现依据，公开 API-1.2 和 EVAL-1.1 的运行输出字段未变。FORMAT-1 清单此前没有实现，本次固定其完整 schema；WAL/版本目录的字节布局不变。

后续静态 IndexAdapter、`vector_build`、Engine 与评测闭环已接通，静态缓存/IOCP 和 WAL 底层已验证。动态恢复与合并尚待验收，见[项目状态](project-status.md)。官方内存分母、正式数据/负载等继续在[待确认事项](open-questions.md)跟进。

## 6 A/B 交付与复核

### 6.1 A 侧存储与基准

2026 年 11 月 11 日，A 交付 STORAGE-A-1、API-1.2 与 EVAL-1.1，固定块租约、文件退休、预算与读取配额、WAL 编解码声明及基准工件。初次 CTest 为两种配置各 3/3，另有 23 个 A 侧回归；冻结后的结果与重跑命令见第 4 节。样例包括[静态配置](../configs/examples/static-run.json)、[资源模式](../configs/examples/resource-modes.json)、[正常轮次](../evaluation/examples/stage3-a/normal/counters.json)和[不可确定写入轮次](../evaluation/examples/stage3-a/uncertain/counters.json)。这些样例和声明不证明缓存、IOCP 或恢复已实现。

### 6.2 B 侧动态夹具与复核

2026 年 11 月 16 日，B 交付索引与增量声明、[首建配置](../configs/examples/build-run.json)、[动态运行配置](../configs/examples/dynamic-run.json)、[轨迹生成器](../evaluation/trace.py)及[独立校验器](../evaluation/check.py)。[动态夹具](../evaluation/examples/stage3-b)含 24 条 4 维初始向量、10 次写、6 次查询和三个查询快照（S=5、8、10）。独立目录的重新生成与校验中，除重新写出的 `evaluation.json` 外，14 个工件的 SHA256 与仓库样例一致；样例邻居由精确扫描生成，Recall@10=1.0 不是 Engine 或近似算法结果。

| 复核中发现的问题 | 修正与回归范围 |
| --- | --- |
| 初版写操作数、负载比例及 `run_id` 与轨迹不符 | 写操作数改为 10（共 16 行），比例按 6 查、4 插、6 更新，统一运行身份；固定轨迹不按比例重新抽样 |
| 输入到达数被误当成提交前缀，跨运行结果和错序可通过 | 轨迹不预设提交序号或查询快照，按实际成功收据和查询 S 重建；验证 operation_id 唯一、轨迹/工件一一关联、写序连续、快照前移和回退，以及不确定写入后的序号边界 |
| upsert、短成功结果、非零起点和 pending 漏判 | 新 ID 可 upsert；成功结果数检查 `min(K, live_count)`；非零起点校验基线版本来源、唯一性和最大序号；按请求种类核对 pending、窗口和守恒计数 |
| 结果与输入证据不完整 | 验证输入与轨迹哈希、查询 ID 和载荷映射、查询数、邻居距离及旧版本；不完整轮次也做结构检查，不计分并退出 2；无效工件覆盖旧评测文件并退出 1 |
| Job 字段缺失、峰值回退或配置与实际不符 | 校验 Job 限额、峰值、退出状态、采样和 `direct_reads`；累计峰值跨不可用采样仍不得回退。内核高水位超限保留原值并标记 `requires_review`，不据此给出内存合规结论 |
| 只复制 ReadView/DeltaPin 不固定资源；精排和导航 scratch 上界不清；缺版本目录可能静默丢数据 | 引入租约、全局写序号、稳定前缀及 WAL 前可撤销尾槽；将段数、候选、复制块、精排与导航纳入预算；区分未适配、目录损坏和读取失败 |
| 夹具生成可能覆盖既有目录，小种子配置可能无法覆盖设定快照 | 参数预校验、排他创建目录，按工作负载规模选择可实现的快照覆盖；默认夹具保持可复现 |

B 侧原有 54 项动态回归先增至 78 项，再增至 87 项。另有 30 个契约反例；新增检查覆盖合法并发、等时刻、失败、拒绝及非零起点等对照，避免把所有输入一概拒绝。以上是协议、夹具和消费端的复核记录，真实并发发布、WAL 重放与多代次切换仍须运行验收。
