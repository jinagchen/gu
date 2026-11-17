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

后续按第四阶段完成 IndexAdapter、vector_build、Engine 与真实静态评测闭环；缓存/IOCP、WAL/恢复、动态合并分别按生命周期后续阶段验证。官方内存分母、正式数据/负载等外部问题继续在[待确认事项](open-questions.md)跟进，不据设计冻结宣称正式资源合规或性能收益。
