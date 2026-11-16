# 第三阶段 A 侧交付记录

日期：2026 年 11 月 11 日。范围：按既定 A/B 分工完成第三阶段 A 的设计、声明、协议样例及自检交付，版本 STORAGE-A-1 / API-1.2 / EVAL-1.1，FORMAT-1 布局不变。整个第三阶段仍等待 B 交付及双方复核；没有将源码契约检查写成引擎实现验收。（2026 年 11 月 16 日补记：B 已交付 INDEX-B-1，见 [B 交付记录](stage3-b-completion.md)；本文件正文保持当日状态，剩余条件为联合复核与 DESIGN-1.1 冻结。）

## 1 交付清单

| A 负责项 | 本次产物 | 交付状态 |
| --- | --- | --- |
| 块读取与缓存边界 | BlockReader/BlockLease、批次顺序、只读引用期限、在途去重、预取、短读/失败/超时及 Drain | 已交付声明与协议 |
| 文件生命周期 | FileRegistry、非复用 FileId、退休标记、旧引用固定及延迟回收 | 已交付声明与协议 |
| 资源与背压 | ResourceGovernor/BudgetReservation、查询/重建字节预算、切换失败及回退、段/代次/增量上限 | 已交付声明、配置与算术自检 |
| 后台 I/O | 需求与后台保留槽、预取上限、后台公平推进、单维护任务和受限构建子进程 | 已交付配置与协议 |
| WAL 底层交接 | FORMAT-1 codec 声明、need_more/corruption、完整追加、同步、组边界轮换、失败停写和 Close | 已交付声明与协议 |
| C++ 基准 | 有界客户端/结果队列、计时/排空、失败/拒绝/不可确定写入、累计计数及采样字段 | 已交付字段与样例 |
| A/B 验证 | 共享读取失败、旧图回收、半帧退出、更新交错、预算切换失败及 Sync 失败推演 | 已交付六组具体场景 |

核心文件：

- [A 侧契约](stage3-a-contract.md)、[基准输出契约](benchmark-output-contract.md)。
- [EngineOptions](../include/agent_io/engine.hpp)、[存储声明](../include/agent_io/storage.hpp)、[基准值类型](../include/agent_io/benchmark.hpp)。
- [静态配置](../configs/examples/static-run.json)、[资源模式](../configs/examples/resource-modes.json)。
- [正常轮次汇总](../evaluation/examples/stage3-a/normal/counters.json)与[不可确定写入汇总](../evaluation/examples/stage3-a/uncertain/counters.json)，同目录有查询/写入样例；正常轮次另有采样样例。
- [契约自检](../tools/contracts/check-stage3-a.py)、[正反例检查](../tools/contracts/test-stage3-a.py)和[编译断言](../tools/contracts/header-smoke.cpp)。

## 2 验证与边界

在本机 MSVC C++20 环境使用现有 build/windows：

```powershell
$cmakeExe = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctestExe = Join-Path (Split-Path $cmakeExe) 'ctest.exe'
& $cmakeExe --build build/windows --config Release
& $cmakeExe --build build/windows --config Debug --target agent_io_contracts windows_environment_probe job_runner
& $ctestExe --test-dir build/windows -C Release --output-on-failure
& $ctestExe --test-dir build/windows -C Debug --output-on-failure
.\.venv\Scripts\python.exe tools/contracts/check-stage3-a.py
.\.venv\Scripts\python.exe tools/contracts/test-stage3-a.py
```

| 检查 | 结果 | 可证明的范围 |
| --- | --- | --- |
| Release 构建 | 通过 | 新声明的类型检查、现有 DiskANN 探针兼容；没有链接完整 Engine |
| Debug 契约/环境/启动器构建 | 通过 | 新增 C++ 声明及原有目标可编译 |
| Release/Debug CTest | 各 3/3 通过 | 原有环境、Job 进程树及提交拒绝探针无回归 |
| 协议样例与资源预算检查 | 通过 | 类型/字段、静态答案连接、守恒、序号关联和阶段二配置一致 |
| 正反例检查 | 23/23 通过 | 错误类型、越界、配额侵占、失败伪快照、不可确定写入遗漏、时钟边界等会被正确处理 |
| 文档链接与差异检查 | 通过 | 本地 Markdown 目标存在，git diff 无空白错误 |

Python 检查不是 vector_bench、evaluation.check 或真实数据评测；样例中的 null 哈希/资源值明确标为 contract_example，正式运行不能照搬为空。C++ BlockReader、ResourceGovernor、WalWriter codec 等仍只有声明，不能据本次编译宣称缓存正确、异步重叠或故障恢复已通过。

## 3 B 接续与联合复核

1. B 补齐 IndexAdapter、候选记录/总预算、原生节点映射、DeltaStore 稳定前缀和快照内部类型，按 BlockLease 引用期限接入；PQ/元数据绕行及实际 scratch 大小明确计量。
2. B 定义首次构建配置、原生图与版本目录工件，满足 A 的受限构建、staging 发布和错误清理要求；记录第三方格式校验及填充限制。
3. B 补动态输入轨迹、完整写入关联、独立快照答案及 evaluation 消费端，沿用 EVAL-1.1 类型与状态规则。
4. A/B 对块所有权、发布后的旧文件、组提交失败和预算模式交接进行复核后，再冻结整个第三阶段。当前没有声称另一名成员已经审阅或签字。

后续实现阶段完成正式同步 Engine 与基准，再实现缓存、IOCP、WAL 和动态恢复；官方 Q-02/Q-03 等仍按对应关闭时点处理。
