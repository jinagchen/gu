# C++ 基准与 Python 评测协议

协议版本 EVAL-1.1，日期 2026 年 11 月 11 日。对应 DESIGN-1.1、API-1.2 与 REQ-1.0，当前执行平台为本机 Windows。2026 年 11 月 15 日已实现 A 的静态基准 CLI 和运行器，显式合成目标用于生产端验证，见[交付记录](stage4-a-benchmark.md)；2026 年 11 月 21 日 B 接通真实 Engine 目标并跑通静态基线闭环，见[第四阶段 B 交付](stage4-b-completion.md)。A 生产端的字段、守恒、错误和有界输出约束见 [基准输出字段契约](benchmark-output-contract.md)；2026 年 11 月 16 日 B 补齐构建配置、动态输入轨迹、独立答案与 `evaluation.check`，见 [B 侧契约](stage3-b-contract.md)。

原协议示例是用于检查字段与逻辑的手工合成数据，不是测得的吞吐、召回或延迟。新增 vector_bench 的 contract-test 会生成实际计时的合成调用记录，仍不作为磁盘检索或正式性能实验。

## 1 进程和命令边界

Python 负责数据、配置、启动和离线分析；C++ 进程负责请求发出、引擎调用、计时与有界日志输出。

```text
vector_build --config <build.json>
vector_bench --config <run.json> --output <new-run-directory>
python -m evaluation.check --run <run-directory>
```

vector_bench 的 `--target engine` 已链接真实引擎并产出 measured 工件；`--target contract-test` 仍保留为生产端自检，且拒绝 measurement。`evaluation.check` 可对运行目录做协议校验并计算召回；`vector_build` 已实现首建与发布，配置不完整退出 2，数据或环境不符退出 4。A 规定的构建受限域、发布与资源交接要求见 [A 侧契约](stage3-a-contract.md)。输出目录必须是新目录，已有结果不默认覆盖；被拒绝的运行不留下空目录。

基准程序退出码约定：0 表示请求执行完成并写出完整协议工件，2 为配置错误，3 为环境或依赖不支持，4 为运行错误，5 为工件不完整。退出 0 不代表 Recall 达标，召回与资源验收由 Python 检查给结论；有失败请求时结果必须显式标记。

## 2 配置

[静态配置示例](../configs/examples/static-run.json) 中的 memory 是引擎内部预算。正式内存约束仍须由外部受限域覆盖 C++、后台工作与日志缓冲，并保存计量范围和峰值；该 JSON 不能替代真实限制。

示例使用 10 条二维向量和 256 MiB 内部预算，仅演示字段，不是超内存工作集实验，也不满足任何已测的内存比例结论。可直接运行的静态配置由 `evaluation/static_fixture.py` 按参数生成，不手工维护。

数据采用首版自有 raw-f32le-v1：向量文件为无头行优先 float32 小端，条数和维度显式给出；ID 文件为对应行的 uint64 小端，查询 ID 单独给出。大小必须精确匹配，ID 唯一、数值有限；SIFT 等原格式由 Python 转换并保存原始与转换后校验值。

全局 JSON 规则：

- schema_version 为整数 1，protocol_version 为 EVAL-1.1；ID、序号、代次、字节数、累计计数、数据条数、纳秒和持续毫秒为十进制字符串。
- dimension、block_bytes、k、candidate_limit、prefetch_blocks、并发/队列条数、资源条数和调度次数为有界 JSON 整数，不能使用布尔代替。WalOptions 的 max_frame_bytes 虽在 C++ 为 uint32，JSON 仍按字节字符串编码；完整字段表见 [输出契约](benchmark-output-contract.md)。
- 布尔类型不能以字符串替代；距离为有限 JSON 数值，不允许 NaN 或 Infinity。
- 路径相对配置文件解析并记录最终绝对路径，结果工件之间采用运行目录内相对路径。
- 所有输入文件须有内容校验。示例的 sha256=null 必须配合 purpose=contract_example；正式运行不允许 null，运行前由数据准备填入真实哈希。
- 未知字段报配置错误，不能默默忽略拼写错误。

运行参数分成 engine、search、workload 和 measurement；不得把客户端到达速率、搜索候选预算与内核队列深度混成一个并发参数。

io_backend 当前取 synchronous 或 windows_iocp。Windows 内存约束和观测须记录 Job Object 提交内存限额、峰值提交量、工作集及计量范围；不能将提交限额称为包含系统文件缓存的物理内存限额。详见 [Windows 平台约定](windows-platform.md)。

## 3 请求和动态轨迹

负载逻辑区分 query、insert、upsert。查询记录 query_id 和查询文件行号；写入记录 operation_id、逻辑 ID、值文件及行号。固定输入轨迹只决定请求与期望到达顺序，真实发布顺序由 C++ 写协调器记录。

动态基准必须保存：

1. 所有成功写入的操作标识、ID、载荷位置与 WriteReceipt.sequence。
2. may_have_applied 错误的操作与序号，整轮标记需核查，不直接算成完全正确的运行。
3. 每个查询实际捕获的 snapshot_sequence，以及返回 ID、版本和距离。

公开 API 不提供指定历史序号搜索。离线 Python 从初始数据加完整写入日志重建每个被测 S 的状态；相同 S 的精确答案可以复用。离线计算不在引擎测量同时占用同一设备或资源。

有限时长运行还须输出停止后的最终写入清单，确保包含“先发布、查询已经看见、响应稍后到达”的组成员；记录不完整时不能继续计算一个看似精确的动态召回。

2026 年 11 月 17 日修订为 INDEX-B-1.1：输入轨迹只定义请求与到达顺序，不携带预测的 sequence 或 committed_prefix_at_arrival。写入的全局序号来自实际 WriteReceipt，查询快照来自进入 Search 后捕获的 ReadView；它们不能从输入到达数推出。失败/拒绝查询的 null 快照与零分合法，失败/拒绝写入不占用成功收据序号。

`evaluation.check` 核对各工件的运行身份、跨查询和写入的 operation_id 唯一性、轨迹与终结记录一一关联、写入操作/ID/载荷行，以及 query.ids.u64 的 ID/行号映射。trace.operations/queries 是请求数，终结记录加 pending 必须与之守恒；dataset.query_count 是源查询向量和 ID 文件行数，允许不同请求复用同一查询。所有输入文件和轨迹的哈希均校验，路径解析后也须留在运行目录内。旧 INDEX-B-1 输入轨迹须重新生成。

测量成功收据必须覆盖 (measurement_start_sequence, measurement_end_sequence]，按序号重建而不要求响应顺序递增；查询 S 在该区间边界内，并按严格时钟先后验证已确认写可见性。非零起点要求显式 baseline 及逐行版本文件，详见 [B 侧契约](stage3-b-contract.md)第 10 节。upsert 对新 ID 插入，成功查询必须返回 min(K,live_count) 个邻居。

复审补充：时钟检查覆盖全部成功读写的发布前缀，禁止非重叠写收据倒序、后续查询快照回退或已观察前缀之后发布较低序号；重叠调用与相等刻度允许不同顺序。不完整轮次仍校验邻居类型、范围与排序。无删除模型的基线非零版本唯一、最大版本等于起点；答案不接受轨迹外操作或多余快照。正式测量还逐项检查外部 Job 指标、资源采样和预算外元数据披露的一致性；这些文件校验不能证明自述证据的来源真实性，不代表资源或性能验收。

正式测量的实际 direct_reads 必须为布尔值并与配置一致，资源采样中的累计 Job 峰值不得回退。分配被拒后原始内核高水位可能超过 Job 限额，不能据此抹掉或改写证据；evaluation.json 的 resource_evidence 保留原值，标记 memory_acceptance=not_evaluated，超限时转为 requires_review、不计分并退出 2，待资源核查。该状态也不直接证明成功提交量或物理内存超限，详见[Windows 平台约定](windows-platform.md)第 3 节。

答案文件和 environment.json 是必需工件。evaluation.check 的退出码 0 表示完整工件已计分，1 表示工件无效，2 表示 incomplete/requires_review 且不计分；输出保留标志，invalid 会替换旧评估结论。退出 0 和合成 Recall=1.0 均不能单独当作正式召回、内存或性能验收通过。

## 4 到达、计时与结束

使用 C++ steady_clock 单调时钟，三个时间均相对测量起点：

```text
scheduled_ns <= api_start_ns <= completed_ns
end_to_end_ns = completed_ns - scheduled_ns
queue_ns      = api_start_ns - scheduled_ns
service_ns    = completed_ns - api_start_ns
```

开放式固定速率下，调度器产生预定到达时间，有界客户端队列承接；队列满时记录拒绝，不推迟下一个预定时间以隐藏过载。引擎内部准入和背压在 API 调用内，因此 service_ns 包含内部等待。闭环模式可另测，但结果中明确模式，不能与开放式尾延迟直接混合。

预热和测量数据分开。测量窗口内预定到达的请求在窗口末尾停止新增并排空，保存 drain_ns。报告两种速率：

- window_completed_qps：测量窗口内完成的查询数量 / 窗口秒数。
- drained_qps：窗口内投递且最终成功的查询数量 / 从窗口起点到最后排空完成的秒数。

写入速率同理另列；并给出窗口投递数、窗口完成数、排空后成功/失败/拒绝数。尾延迟统计纳入这些测量请求的最终结果，不只看窗口内已经完成者。超时和拒绝单独分布与计数。

若排空截止时仍有未完成请求，标记 run_incomplete 并返回运行失败，不把它们从统计中删除。关闭阶段做后台 I/O 排空，不能遗留进程后立即开始下一组。

## 5 输出目录和证据

```text
run-<id>/
  effective-config.json
  environment.json
  counters.json
  queries.jsonl
  writes.jsonl
  samples.jsonl
  stdout.log
  stderr.log
  evaluation.json
```

environment 保存代码提交、本地修改状态、编译模式与参数、依赖提交、操作系统、CPU/配额、内存、存储/文件系统/虚拟化、I/O 后端和已验证对齐。尚未测得字段用 null 和原因，不能写成已验证。

effective-config 保存解析后的路径、哈希、实际参数与协议版本。示例配置允许空哈希仅用于契约校验，正式运行必须拒绝。

queries/writes 在同一受限域内有界流式输出。基准主计时在查询结果形成时结束，但持久日志线程造成的资源消耗仍存在；各组保持同策略，报告日志字节量和落盘设备，不宣称没有观测开销。

EngineStats 计数是实例累计值，基准从测量起止差值得到窗口统计。在途去重的一个真实读取只记一次后端提交；共享需求次数记在 logical_block_demands。预取被消费按每次预取驻留周期最多一次计，不把多个等待者当成多次预取成功。

引擎写放大分子是 WAL、checkpoint、merge 逻辑写入字节之和，分母是接受的更新载荷字节。恢复或构建若在测量外，单独披露，不冒充设备 NAND 写放大。

## 6 查询与答案工件

[查询示例](../evaluation/examples/queries.jsonl) 与 [精确答案示例](../evaluation/examples/ground-truth.jsonl) 按 `(operation_id, query_id, snapshot_sequence)` 连接。每条查询保存状态、快照和计时；未进入引擎的拒绝请求使用 snapshot_sequence/view_generation/api_start_ns=null，计失败，不计算伪造答案。已调用 API 但失败时现有 Error 无快照，snapshot_sequence/view_generation 同样为 null，api_start_ns 保留真实值。

本次合成示例使用 S=0、10 条有效数据、K=10，并列规则为距离后 ID。返回 ID 0 至 9，版本全为 0；答案给出同一组 ID，独立计算应得到 Recall@10=1。距离仅用于示例协议检查，不是从真实向量测得。

正式精确答案由原始向量与动态轨迹计算，不能从查询返回结果复制。单次召回为有效去重 ID 与精确 Top-10 交集 /10，对约定查询集平均。失败和 search_incomplete 按零分进入汇总，OOM、损坏、不可确定写入或工件缺失的整轮不能宣称验收通过。

并列先按距离、再按 ID 统一规则；如果选定近似算法无法稳定处理边界等距集合，必须在正式实验前登记兼容的并列计分办法与原始规则结果，不能事后只选有利答案。

## 7 校验清单

Python 在执行离线召回前检查协议版本、ID/序号类型、配置与哈希、结果唯一性、版本≤快照、时间单调、请求数守恒和写入序号连续性。检查动态最高版本需要真实初始数据和写入日志，本次示例不替代该测试。

比较性能时要求数据哈希、有效内存上限、持久化语义、目标机、运行模式与实测召回可比。结果不同源或字段缺失时拒绝自动生成“提升百分比”。

A 的静态计时器、CLI 和实际生产端输出已实现；B 的轨迹生成、运行校验与独立召回已实现，并用 24 条 4 维、10 次写、6 次查询的样例自检（见 [B 侧交付记录](stage3-b-completion.md)）。正式数据生成、真实 Engine 集成及性能结果仍待完成；样例的 Recall@10=1.0 来自精确扫描参考，不作为近似算法结论。
