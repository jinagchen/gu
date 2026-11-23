# 第四阶段 A：同步存储首批交付

日期：2026 年 11 月 12 日。本记录保留同步存储首批交付范围；当时静态 Engine 未接通。沿用 API-1.2，在 [synchronous_storage.hpp](../include/agent_io/synchronous_storage.hpp) 添加工厂，不修改 B 的索引与动态接口。

## 已实现

- `CreateResourceGovernor`：验证预算和溢出；非阻塞预约；引用共享计费；查询/重建模式原子切换及失败保持原模式；预约令牌可晚于 governor 释放。
- `CreateSynchronousFileRegistry`：只打开既有不可变文件；Windows 文件身份及硬链接去重；进程单调 FileId；有限注册数；缺失文件、退休文件和系统错误处理。
- `AllocateReadBuffer`：稳定对齐内存；分配前计费；先释放内存再释放预约；支持 workspace/cache 分区。
- `CreateSynchronousReadBackend`：同步 `ReadFile`、地址/范围/长度验证、文件级指针串行化、在途 ID/重叠缓冲拒绝、容量上限、锁外恰好一次完成回调、排空及关闭准入。
- `CreateSynchronousBlockReader`：按输入顺序返回只读租约；批内重复复用；并发同块共享在途读取；失败整批返回；整批载荷先预约；等待者超时；租约固定文件、缓冲和计费。返回块固定输出数组的保守计费直到最后引用释放。
- 实例累计后端读取提交数、完成字节和错误数。完成字节包括失败请求实际传输的字节，不冒充设备物理读取量。
- DiskANN 探针使用本次存储库，持有块租约再复制到依赖缓冲，不再借用依赖缓冲给后端填充。

缓存、预取关闭，合法预取返回 dropped；后台读取返回 unsupported，避免把尚未实现的维护公平调度描述为通过。同步内核调用不能抢占，执行读取的线程在系统调用返回后检查期限；共享等待者可按期限提前返回。Drain 停止准入并等待回调结束，不等待外部块租约释放；一个 BlockReader 应独占其后端的排空责任。

退休文件在仍有引用时保留；无外部引用后 `CollectRetired` 返回 unsupported 并保留文件。根据 STORAGE-A-1，Windows 发布/恢复故障验收前不启用自动删除。注册表受容量上限约束，退休条目继续占用额度；应由 Engine 长期持有一个注册表，不能通过重建注册表绕开退休约束。

内部计费包括保守的对象、控制块、容器节点和对齐开销，不是分配器实测上界或官方物理内存合规证明。依赖 PQ、scratch 和进程其余分配仍须由 B 计量并结合外部 Job 约束。本次未接入完整 Engine 的查询/重建准入。

## 验证

MSVC C++20，`/W4 /WX /permissive- /EHsc`。Release 全部构建及 Debug 存储/声明/环境目标构建通过；两种配置 CTest 各 4/4。Python 第三阶段契约检查通过，23 个正反例通过。

[C++ 存储测试](../tests/storage/synchronous-storage-test.cpp)覆盖预算耗尽与释放、模式切换失败及回退、文件身份别名、正常 buffered/direct 读取、范围/长度/对齐错误、并发偏移安全、回调计数、重叠缓冲、在途容量、排空、退休后的旧引用、真实 ReadFile 错误、批次顺序/重复、共享等待者超时和短完成不发布。短完成由测试后端注入；真实系统调用失败使用只读页作为目标，验证错误及原生错误码传播。

[集成检查脚本](../tools/storage/check-synchronous-storage.py)复用阶段二的 4096 条、32 维、128 查询数据，但直接从原始向量以 float64 距离独立计算答案。两次新进程加载 Recall@10=1.0，结果一致，与原探针结果一致；适配器故障注入正确退出失败。每轮使用 128 MiB Job 提交限额，实际计量及哈希见[证据](evidence/stage4-a-storage/summary.json)。这不是正式规模化召回或内存比例验收。

最终原始结果、日志和计量保存在 `runs/stage4-a-storage-final-20261112`，阶段二原始工件保持原样。证据记录本次最终实现、测试及检查脚本的源码哈希。

```powershell
$cmakeExe = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctestExe = Join-Path (Split-Path $cmakeExe) 'ctest.exe'
& $cmakeExe --build build/windows --config Release
& $cmakeExe --build build/windows --config Debug --target synchronous_storage_test agent_io_contracts windows_environment_probe job_runner
& $ctestExe --test-dir build/windows -C Release --output-on-failure
& $ctestExe --test-dir build/windows -C Debug --output-on-failure
.\.venv\Scripts\python.exe tools/storage/check-synchronous-storage.py `
  --fixture D:\agent-io-stage2\20261110 --output runs/stage4-a-storage-repeat
```

集成脚本要求新输出目录，防止覆盖已有证据；需已有固定 DiskANN 依赖及小数据索引。没有这些工件时仍可独立构建并运行 C++ 存储测试。

## B 交接和剩余工作

B 可使用 `ReadBatch` 的只读块租约对接 IndexAdapter，先明确原生节点映射、读取批次与 scratch 上界；不能保存脱离租约的裸 span。新接口由 `agent_io_storage` 导出，使用 CMake `target_link_libraries(... PRIVATE agent_io_storage)`。

后续已接通[静态基准生产端](stage4-a-benchmark.md)、[首建与真实静态 Engine](stage4-b-completion.md)，并加入[缓存、IOCP 与预取](stage5-a-completion.md)。当时的 DiskANN 探针仍是独立验证，不是当前 Engine 的构建后端。WAL 已交付到底层，动态恢复仍待接入。
