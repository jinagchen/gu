# 第四阶段 A：静态 C++ 并发基准运行器

日期：2026 年 11 月 15 日。协议 EVAL-1.1，公开 Engine API-1.2 不变。

## 交付范围

新增 `agent_io_benchmark` 静态库、`vector_bench` 命令和显式 `contract-test` 测试目标。A 的静态查询生产端已实现；正式 Engine 尚未链接，第四阶段整体仍未完成。合成目标的 ID、版本和距离用于验证输出，不是检索结果，不计算召回或宣称磁盘检索性能。

- [benchmark_runner.hpp](../include/agent_io/benchmark_runner.hpp)：运行参数、查询目标和日志接收接口、内存规划与运行报告。
- [runner.cpp](../src/benchmark/runner.cpp)：开放式到达、并发执行、结果槽、日志线程、资源采样和排空。
- [命令行实现](../tools/benchmark/main.cpp)、[配置解析](../tools/benchmark/config.cpp)：配置校验、工件输出及退出码。
- [C++ 检查](../tests/benchmark/benchmark-runner-test.cpp)、[命令行集成检查](../tools/benchmark/check-benchmark.py)：验证真实生成的工件，复用已有 EVAL-1.1 校验器。

目前仅支持静态 query，明确拒绝 insert/upsert 比例非零的配置。默认 `--target engine` 返回 3，表示真实目标未链接。只有显式指定 `--target contract-test` 且 `purpose=contract_example` 才运行合成目标；该目标拒绝 measurement，即使填写了形式正确的哈希也不会冒充正式运行。

## 调度、内存和计时

预定到达为 `floor(ordinal * 1e9 / target_ops_per_second)` 纳秒，从 0 开始，严格取 scheduled < duration 的请求。到达计划不随查询完成或日志速度推迟。当前查询行选择为 `(seed % query_count + ordinal % query_count) % query_count`；这是确定性的轮转，不宣称随机负载。速率支持 (0, 1e9]，每阶段连同排空上限不超过 24 小时。

客户端环形队列同时受配置条数和字节上限约束。请求仅保存借用输入的行号，目标的输入载荷必须另行计费。结果池预分配 JSON 缓冲和 K 个 Neighbor，每个槽包含保守的对象/分配开销；实际槽数取条数上限与字节上限可容纳数的较小值，报告在 summary.resources，不能把配置中的 128 条当成已经获得 128 个最大记录槽。

工作线程在进入 Search 前取得结果槽。槽从调用、结果形成到日志写入结束始终保留。额外保留一个调度者专用拒绝槽，该槽也占结果条数和字节额度。队列满产生 rejected，api_start_ns 为 null；拒绝槽仍忙时整轮标为不完整并退出 5，未记录的计划请求保留在 pending，不用同步文件写阻塞到达，也没有无限回退队列。

运行器在分配队列/结果池前通过 ResourceGovernor 预约 workspace，最后一个阶段和全部线程结束后释放。CLI 对保留的配置、流缓冲和汇总额外预约 4 MiB。控制结构和分配器开销采用保守估算；线程栈、CRT、目标输入、依赖 scratch 等仍须分别声明并接受外部 Job 观测，不能把内部计费当成完整物理内存证明。

索引/目标构造、预热、正式窗口分开记录。预热按同一负载运行并排空，其请求数保存在 summary.warmup_query，不进入正式 JSONL、查询计数或引擎计数差值。真实目标的加载由调用者在运行器外完成。

每条记录保留 scheduled_ns、api_start_ns、completed_ns。completed 在 API 返回并验证结果后捕获，不包含后续 JSON 序列化和日志落盘。端到端、排队、执行时延可从原始行精确计算；C++ 同时输出按状态分开的固定大小二次幂直方图、最小值和最大值，没有累积全部延迟样本。API 超时还单独输出计数和三种延迟分布，错误按 ErrorCode 分类，客户端拒绝保持独立。summary 中分别给出窗口成功吞吐和排空成功吞吐。

客户端、日志（包括 Flush）和底层 I/O 排空使用同一总截止时间。超时停止发布新结果，保留 pending，退出 4；随后请求协作取消并 join 全部线程，不留下引用已释放输入的后台线程。freeze 后的清理耗时另记 cleanup_ns。C++ 无法安全强杀不响应取消的线程，生产目标必须尊重查询超时和 Drain 截止时间，外层 Job 还需设进程超时。

Query target 错误记为 error，不伪装客户端拒绝；Search 异常转换为请求错误。重复 ID、非法距离/顺序/版本、静态快照错误会形成 corruption 记录。记录超限、非法 UTF-8、日志/采样写入失败或拒绝日志拥塞返回 5；最终文件关闭失败也使 run_incomplete=true。

## 输出与依赖

输出目录必须不存在，父目录须已存在：

```text
effective-config.json   解析绝对路径后的 EVAL 配置
environment.json        实际目标、构建信息、Job 限额、线程及日志设备
queries.jsonl           正式窗口请求的结果与时间
writes.jsonl            静态运行为空文件
samples.jsonl           进程 private/working set 和当前 Job 内核峰值
counters.json           EVAL-1.1 守恒计数和八项引擎计数差值
summary.json            吞吐、延迟直方图、槽数、日志字节与阶段耗时
stdout.log / stderr.log 本程序的汇总与诊断
```

资源采样由日志线程执行，拥塞可延迟采样，记录实际采样时间，不补造固定间隔样本。采样缺失值是 null 并有原因。当前 Job 内核峰值与本进程采样值分列；外部 Job 启动器的最终工件由 `--job-metrics` 引用并保留原样。

合成目标不读取配置里的示例向量和索引路径，也不验证这些文件的内容哈希。environment 明确记录 input_content_used=false、input_hashes_verified=false、engine_options_applied=false、formal_engine_measurement=false 和实际无 I/O 后端；配置中的 engine 参数仅保留为协议请求，不能将其视作实际已启用的存储设置。

核心库及 C++ 测试只依赖已有存储库和 Windows SDK。CLI 使用已锁定的 Boost 1.78.0 JSON 源头文件，以单独翻译单元编译，不联网下载新依赖。CMake 自动查找阶段二安装路径，也可设置 `AGENT_IO_BOOST_INCLUDE`；缺少头文件时仅禁用 CLI。Boost.JSON 使用其自有默认资源，关闭未使用的 Container 自动链接；没有引入额外二进制包。

## 复现

在已有工具链环境中：

```powershell
cmake --build build/windows --config Release
ctest --test-dir build/windows -C Release --output-on-failure
cmake --build build/windows --config Debug --target vector_bench benchmark_runner_test synchronous_storage_test agent_io_contracts windows_environment_probe job_runner
ctest --test-dir build/windows -C Debug --output-on-failure

.\build\windows\Release\vector_bench.exe `
  --config configs/examples/benchmark-contract-test.json `
  --output runs/benchmark-producer-demo --target contract-test

.\.venv\Scripts\python.exe tools/benchmark/check-benchmark.py `
  --output runs/benchmark-producer-check
```

示例配置运行约 250 ms 加排空；目标为合成调用。CLI 集成脚本为各子运行施加 128 MiB Job 提交限额和 15 秒进程超时，检查正常/重复、API 错误、客户端拒绝、排空超时、目标限制、配置反例及不覆盖既有结果。它验证生产端，不接管 B 的 evaluation.check 或独立向量答案。

C++ 检查覆盖三线程真实重叠、预热与 Stats 隔离、错误转义、非法目标结果、拒绝、日志阻塞、采样失败、协作取消、计数回退及预约失败/释放。Release 和 Debug 的 CTest 同时包含现有存储与环境检查。

## 本次验证结果

MSVC C++20 自有代码以 `/W4 /WX /permissive- /EHsc` 编译通过。Release 全部目标及上列 Debug 目标构建通过；两种配置 CTest 各 6/6，CLI 集成各 20 个场景通过，包含单列 API 超时的场景。原有第三阶段契约检查及 23 个正反例继续通过。

源码哈希与最终检查工件逐项匹配。证据为 [Release 汇总](evidence/stage4-a-benchmark/Release-summary.json)、[Debug 汇总](evidence/stage4-a-benchmark/Debug-summary.json)、[Release CTest 日志](evidence/stage4-a-benchmark/ctest-Release.log) 和 [Debug CTest 日志](evidence/stage4-a-benchmark/ctest-Debug.log)。每种配置的汇总均记录原始运行目录、各场景结果、源码及二进制哈希。该证据只证明静态生产端行为，不证明真实磁盘查询、召回或官方内存比例合规。

## B 侧接入

B 可链接 `agent_io_benchmark`，实现 BenchmarkTarget 并提供 BenchmarkSink，调用 RunStaticBenchmark：

1. 加载真实索引和查询，验证维度、有限数、唯一 ID、大小和实际 SHA256，输入及依赖 scratch 纳入预算。
2. QueryId(row) 返回真实查询 ID；Search(row, options, output, stop) 使用稳定输入，向借用的预分配 span 写 Neighbor 并返回真实快照、代次和数量；不得保存该 span。
3. Stats 返回实例累计计数；Drain 只等待该目标的在途 I/O，不关闭后续准入，预热后还要继续查询。停止/超时必须真正释放对应工作区和 I/O 引用。
4. 新增真实目标的 CLI 分支，把 load_ns、哈希、版本、依赖和实际存储参数写入 environment；移除当前该分支的 unsupported 返回，不能让 contract-test 接受 measurement。
5. 由 Python 在测量外独立计算精确答案，验证相同输入下的召回、资源和重复运行，再验收完整静态基线。

本次没有实现 Engine、IndexAdapter、首次构建、动态输入、WAL 或第五阶段缓存/IOCP/预取。
