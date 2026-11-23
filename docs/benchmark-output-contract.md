# C++ 基准输出字段契约

日期：2026 年 11 月 11 日，协议 EVAL-1.1，A 生产端交付。schema_version 保持 1，新增 protocol_version="EVAL-1.1" 区分字段修订，旧 EVAL-1 示例一并升级。本文件与 [评测协议](evaluation-protocol.md)、[A 侧契约](stage3-a-contract.md)共同使用。

A 侧示例均为 purpose="contract_example" 的手工协议样例，不对应真实向量文件或运行结果。B 已于 2026 年 11 月 16 日交付动态输入轨迹、精确答案生成端和 evaluation.json 消费端（见 [B 侧动态夹具与复核](stage3-freeze.md#62-b-侧动态夹具与复核)），其样例有小型真实输入文件，但结果由精确参考生成，同样标为 contract_example，不构成引擎运行结论。

## 1 类型规则

对象禁止未知字段、重复键、NaN 和 Infinity。必需字段不能省略；允许缺失的数据显式写 null，并满足下表的状态规则。

run_id、operation_id 各最多 128 个 UTF-8 字节，error.message 最多 4096 字节，载荷相对路径最多 1024 字节，必须在生产端截断诊断文字前保留完整错误码。距离序列化需能保留 double 往返精度。运行前按 `16384 + 6×4096 + 128×K` 字节保守检查单条结果上界（包括 JSON 转义和字段开销）；实际记录仍须≤max_record_bytes，不能靠截断邻居列表满足容量。

| 类型 | 字段 |
| --- | --- |
| JSON 整数，0 至 2³²−1，布尔不合法 | schema_version；dimension、block_bytes、k、candidate_limit、prefetch_blocks；max_queries、max_pending_writes、max_inflight_reads、max_prefetch_reads、max_delta_segments；ResourceLimits 的三个 max_* 条数；ReadScheduling 四字段；workers、两种 queue_capacity、max_record_bytes |
| JSON 有限数值 | 距离≥0、target_ops_per_second>0、各 fraction∈[0,1] |
| 无符号 64 位十进制字符串 | ID、序号、代次、输入行号、数据条数、累计计数、所有字节数、纳秒/毫秒、seed |
| JSON 字符串 | run_id、operation_id、枚举、路径、协议版本、错误消息、非空不可用原因 |
| JSON 布尔 | direct_reads、run_incomplete、requires_review |
| JSON 有符号 32 位整数 | error.system_error，与 C++ Error::system_error 一致 |

十进制字符串仅接受 "0" 或不以 0 开头的正数字串，最大 18446744073709551615，禁止负号、空白、指数及小数。正值参数还须>0。计数规则指数据条数/累计结果，不包含上表明确列出的有界配置整数。

MemoryBudget 的六个字段全为字节字符串。rebuild_memory 为同构对象或 null。WalOptions 的 rotation_bytes、max_frame_bytes 在 JSON 中都是字节字符串，后者还必须≤2³²−1。文件 SHA256 为 64 个小写十六进制字符；只有 contract_example 可为 null，measurement 必须真实校验，不能仅检查字符串格式。

## 2 查询与写入的共同字段

queries.jsonl / writes.jsonl 每行都包含：

| 字段 | 约束 |
| --- | --- |
| schema_version / protocol_version / purpose | 1 / EVAL-1.1 / contract_example 或 measurement |
| run_id / operation_id | 非空字符串；operation_id 跨查询和写入唯一 |
| status | ok、error、rejected |
| scheduled_ns | 相对测量起点的预定到达 |
| api_start_ns | 进入 Engine API 的时间，未调用时 null |
| completed_ns | API 返回后结果形成时间；拒绝为拒绝判定时间 |
| error | ok 时 null，其他状态为下述完整对象 |

error 必有 code、message、system_error、write_effect、attempted_sequence。code 与 API-1.2 ErrorCode 同名；write_effect 为 none 或 may_have_applied，attempted_sequence 为十进制字符串或 null。

ok/error 必须 api_start_ns 非 null 且 scheduled≤api_start≤completed。rejected 必须 api_start_ns=null、scheduled≤completed、code=resource_exhausted、write_effect=none、attempted_sequence=null。引擎内部准入失败已经进入 API，应记录 error，不能写成 rejected。

对于 rejected，端到端延迟仍有定义，queue_ns/service_ns 均为 null，不能将它伪造为一次耗时为 0 的 API 调用。到达后排队但尚未进 API 就达到客户端排空截止时保留为 pending，并在不完整轮次中披露缺失记录。

## 3 queries.jsonl

除共同字段外，必需 query_id、snapshot_sequence、view_generation、k、neighbors。

- query_id 为输入查询 ID 的十进制字符串；同一 query_id 可用于多个 operation_id。
- ok 时 snapshot_sequence 非 null，view_generation>0，neighbors 含≤k 个完整结果；实际数量是否等于 min(k,live_count) 由 B 根据相应快照独立核对。
- neighbor 恰有 id、version、squared_distance，ID 去重、version≤snapshot_sequence，按距离及数值 ID 升序。
- error/rejected 时 neighbors=[]、snapshot_sequence/view_generation=null。现有 Search 的 Error 不携带快照，A 不编造失败时的 S；失败仍计召回零分。未来增加失败诊断快照必须修订 API 和协议。
- 查询错误的 write_effect 只能为 none，attempted_sequence 必须为 null。

## 4 writes.jsonl

除共同字段外，必需 operation、id、payload、sequence。

operation 为 insert 或 upsert。payload 恰有 file、row、dimension、sha256；file 为运行目录内相对路径，正式运行通过 effective-config 登记源文件及哈希，row 为输入载荷行号。示例不要求实体文件存在；正式运行必须核对文件大小、行号和内容哈希。拒绝与失败也保留原始载荷定位。

| 状态 | sequence | error.write_effect / attempted_sequence |
| --- | --- | --- |
| ok | >0，成功 WriteReceipt 的序号 | error=null |
| error，明确未生效 | null | none；若只分配未追加，可保留候选序号>0，但不是成功收据 |
| error，可能已生效 | null | may_have_applied，已知时保留>0 的尝试序号；整轮 requires_review=true |
| rejected | null | none / null |

writes.jsonl 按完成顺序流式输出，不保证序号排序。排空后必须包含所有已确认响应；未响应/不可确定写入不得凭猜测写成 ok。B 遇到 requires_review 必须先恢复核查再评价动态结果，不直接从成功记录构建“完整答案”。

## 5 counters.json

必需字段：

| 字段 | 类型与含义 |
| --- | --- |
| schema_version / protocol_version / purpose / run_id | 同上 |
| duration_ns / drain_ns | 配置窗口及窗口结束后的排空耗时 |
| run_incomplete / requires_review | 未完全排空或工件不完整 / 存在不可确定写入 |
| measurement_start_sequence / measurement_end_sequence | 预热排空后的基线 / 测量排空后的发布序号；不能将预热成功写入算进本轮 |
| by_type | 恰有 query、insert、upsert，各为 RequestCounts |
| engine_counters | 下述八个累计计数的起止差值 |

RequestCounts 恰含 scheduled、succeeded、failed、rejected、pending、window_completed、window_succeeded，全部十进制字符串：

```text
scheduled = succeeded + failed + rejected + pending
window_succeeded <= window_completed <= succeeded + failed + rejected
window_succeeded <= succeeded
```

window_completed 统计窗口内终结的所有状态，window_succeeded 只统计窗口内成功响应。边界使用 scheduled<duration，completed<duration；completed==duration 归入排空阶段。

完整轮次 pending=0，文件中各类型的行数及状态计数与 by_type 一致；不完整轮次仍报告 pending 和已写行数，绝不声称工件完整。协议校验器可检查这种不完整声明，但不能证明没有未落盘日志；工件写出失败仍退出 5。

engine_counters 恰有 io_reads_submitted、io_read_bytes_completed、io_read_errors、prefetch_submitted、prefetch_consumed、wal_bytes_written、checkpoint_bytes_written、merge_bytes_written，均为十进制字符串。采用测量开始至排空完成的累计差值，测量外首次构建单独披露。Stats 的瞬时量如驻留字节和 backlog 不能作差当吞吐，记录在资源采样/环境工件中。

计量规则：

- cache_hits/cache_misses：每个需求块查找记一次；miss 后加入共享在途仍为 miss；预取不增加需求命中数。
- logical_block_demands：每个需求块一次，同块多等待者各自计数；实际 reads_submitted 按后端接管次数计，失败的未接管 Submit 不计接管，另记错误。
- prefetch_submitted：实际由推测新建的后端接管次数；prefetch_consumed 每次预取驻留周期最多计一次，需求合入在途时即算消费。
- io_read_errors：后端提交失败或完成失败各请求至多一次，短读计错误；错误读取的实际完成字节仍可加入 io_read_bytes_completed，但数据不发布。
- WAL/checkpoint/merge：各自实际完成的逻辑文件写字节，包含头和填充，重试造成的实际重复写也计；不包含设备 NAND 的内部写。

2026 年 11 月 17 日冻结对齐：INDEX-B-1.2 的 logical_blocks_read/logical_bytes_read、prefetch_admitted/prefetched_keys_used 是单次 Expand 逻辑诊断，包含命中与共享读取的行为，不能当作上述 engine_counters 或用查询之和替代全局消费。预取首次被需求合入后即使随后读取失败也不撤销 consumed；同键被淘汰后再新建预取才开始下一驻留周期。生产端字段保持 EVAL-1.1 不变。

查询 window_completed_qps 使用 by_type.query.window_succeeded / 窗口秒数；drained_qps 使用 succeeded / ((duration_ns+drain_ns)/10⁹)。写入分别对 insert/upsert 报同样口径。失败、拒绝单列，不混入成功吞吐。

完整且无不可确定写入时，成功写入序号集合必须恰覆盖 (measurement_start_sequence, measurement_end_sequence]，检查按序号排序后的集合，不能要求日志响应顺序递增。查询 S 必须在这两个序号之间。出现不可确定写入、并行外部写入或写入记录缺失时禁止给出动态正确性通过；本协议只允许基准进程控制全部写调用。

用计时交叉检查可见性时，仅对 write.completed_ns<query.api_start_ns 强制该写入序号≤S；write.api_start_ns>query.completed_ns 则其序号必须>S。时钟刻度相等不能证明跨线程先后，不额外推断；版本最高性仍由 B 根据初始数据和写入记录独立验证。

## 6 samples.jsonl 与受限域

资源采样行必需共同版本字段、run_id、sampled_ns、private_bytes、working_set_bytes、kernel_peak_job_commit_bytes、unavailable_reason。前三个资源字节数为十进制字符串或 null；有任一 null 必须提供非空 unavailable_reason，全部可用时该字段必须 null。采样时间单调，采样值是观测，不作为分配器准入凭据。

environment.json 须记录 job_limit_commit_bytes、sample_interval_ms、测量域、参与进程、线程/依赖配置及日志设备；无法观测的系统缓存不得填 0。外部 Job 启动器的最终 JSON 原样保留并在环境中引用，不能把进程采样峰值改名为整个 Job 的内核峰值。多进程采样由启动器求和，不能只记录主进程以漏掉重建子进程。

## 7 可执行自检与 B 的后续工作

2026 年 11 月 15 日已新增静态 C++ 生产端和实际输出检查，见[基准交付记录](stage4-a-benchmark.md)。运行 `tools/benchmark/check-benchmark.py --output <new-directory>` 会调用 C++ CLI 并验证生成工件；合成目标不代表真实 Engine 或召回验收。下面两条命令继续检查原始协议样例。

从仓库根目录执行：

```powershell
.\.venv\Scripts\python.exe tools/contracts/check-stage3-a.py
.\.venv\Scripts\python.exe tools/contracts/test-stage3-a.py
```

校验器检查固定配置及示例的类型、字段、顺序、守恒、序号关联和模式预算；负例验证协议矛盾会被拒绝。它不是测量入口。真实静态测量使用 `vector_bench --target engine`，独立验证使用 `evaluation.check`。读优化的附加诊断进入 `summary.json`，不改变 `counters.json` 的字段；动态写入字段仍只有协议样例，没有真实 C++ 动态运行数据。[评测协议](evaluation-protocol.md)
