# C++ 接口约定

契约版本 API-1.2，日期 2026 年 11 月 11 日，对应 DESIGN-1.1 [总体设计](architecture.md)。1.1 确定 Windows IOCP；1.2 补充第三阶段 A 的 BlockReader、文件回收、预算模式、读取调度、WAL 编码与基准交接。接口是项目内部源代码契约，不承诺跨编译器 ABI 稳定。

截至 2026 年 11 月 23 日，`Engine` 可链接并支持静态 `Open`、`Search`、`Get`、`Stats`、`QuiesceReads` 和 `Close`。`Open` 独占 `LOCK` 并校验索引，不做 WAL 重放；`Insert`、`Upsert`、`Checkpoint` 固定返回 `unsupported`。下文的动态语义是冻结契约，不是已交付行为。[项目状态](project-status.md)

## 1 类型和返回值

命名空间为 `agent_io`，公开声明见 [engine.hpp](../include/agent_io/engine.hpp)。

`VectorId`、`Sequence`、`Generation` 都是 uint64。JSON 和 JSONL 中这些值统一使用十进制字符串，避免其他语言解析大整数时丢精度；字节数、累计计数、数据条数和时间也采用十进制字符串。有界配置整数（维度、K、并发及队列条数等）使用 JSON 整数，逐字段规则见 [基准输出契约](benchmark-output-contract.md)。

`Result<T>` 是 `std::variant<T, Error>`。成功持有 T，失败持有 Error；不返回空指针伪装成功。Error 保留项目错误码、文字、可选系统错误和写入是否可能生效。项目正常错误不以异常作为公开协议，边界捕获分配及第三方异常并转成错误；无法继续提供一致状态时停止写入或返回失败。

| 错误码 | 含义与调用方动作 |
| --- | --- |
| invalid_argument | 维度、非有限数、K、预算或参数非法；修正输入 |
| not_found | Get 的逻辑 ID 不存在；Open 的目录或必需文件不存在 |
| already_exists | insert 与现有 ID 冲突；不是 upsert 失败 |
| resource_exhausted | 准入、内存、段数或空间不足；说明是拒绝还是等待后失败 |
| deadline_exceeded | 查询或准入期限到；不代表内核 I/O 已立即释放 |
| search_incomplete | 候选预算内不足应返回的有效数量；整次查询失败 |
| io_error | 读取、写入或同步错误，附系统错误 |
| corruption | 已存在文件不满足校验或格式一致性；不静默忽略 |
| unsupported | 格式、后端或目标环境不支持；不静默切换读取方式 |
| closed | 排空或关闭后新请求；不接受新任务 |
| recovery_required | 写入路径已进入不可继续状态，须关闭并恢复 |

write_effect 默认 none。只有写入可能进入 WAL 后的不可确定错误设 may_have_applied；此时禁止盲目将重试统计成一个独立成功操作。attempted_sequence 若已分配则返回，但不是成功收据。

## 2 Engine 操作

| 操作 | 输入与输出 | 主要约束 |
| --- | --- | --- |
| Open(directory, options) | Result<unique_ptr<Engine>> | 打开现有索引，读取维度和度量、独占写锁、恢复后才成功；不隐式创建空索引 |
| Search(query, options) | Result<SearchResult> | 同步调用，内部可以异步 I/O；输出 S、视图代次、版本及距离 |
| Get(id) | Result<StoredVector> | 当前快照的最高有效版本，用于精确探针和诊断，不做全量历史版本服务 |
| Insert(id, values, options) | Result<WriteReceipt> | 已存在返回冲突；成功已持久化并发布 |
| Upsert(id, values, options) | Result<WriteReceipt> | 不存在则插入，存在则产生新版本 |
| Checkpoint(timeout) | Result<CheckpointReceipt> | 捕获目标 T，成功表示持久化段覆盖至 T，可报告更高已完成切点 |
| Stats() | Result<EngineStats> | 有界计量快照；多计数非原子一致，不能代替查询视图 |
| Close() | Result<Success> | 停新请求、排空、报告错误；重复成功关闭幂等 |

索引首次创建由 `vector_build` 负责，输入参数、独立 Job 限额与 staging 发布规则见 [B 侧契约](stage3-b-contract.md)及 [build-run.json](../configs/examples/build-run.json)；拒绝矩阵见[第四阶段 B 交付](stage4-b-completion.md)。按目标契约，写入同步由 `Insert`/`Upsert` 保证，`Checkpoint` 专指数据段与清单覆盖；当前不能调用它们执行这些操作。没有删除、事务组和历史 Search API。

Get 返回的 `version` 是该向量最后修改序号，`snapshot_sequence` 是操作读取的整体快照，两者不必相同。Search 的每个版本不得大于 S，且不得存在同一 ID 的更高可见版本。

## 3 参数和预算

MemoryBudget 各分区均以字节指定，总和必须等于 total_bytes；total_bytes=0 或缺必需分区不能通过 Open。分区涵盖缓存、增量、元数据、工作区和安全余量，具体用途见总体设计。

API-1.2 新增 rebuild_memory（缺省 null）、ResourceLimits、ReadScheduling、WalOptions。查询/重建模式保持相同总限额；未配置 rebuild_memory 时不运行构建新段的维护。各阈值、最小工作集、切换失败与退回路径按 [A 侧契约第 3 节](stage3-a-contract.md)执行；正式分配与大规模可行性仍待实现验证。

`block_bytes=4096` 是配置初值，不是平台对齐保证。direct_reads 在 Windows 映射为 FILE_FLAG_NO_BUFFERING；后端探测逻辑/物理扇区并检查地址、偏移和长度。无法满足时返回 unsupported 或 invalid_argument，调用方显式改配置后才允许 buffered 路线。io_backend 可取 synchronous 或 windows_iocp。

| 参数 | 校验和语义 |
| --- | --- |
| max_queries | >0；Search/Get 合计占用上限，查询执行和 I/O 排空的资源都计入 |
| max_pending_writes | >0；含排队与尚未完成确认的写请求，数据缓冲纳入预算 |
| max_inflight_reads | >0；所有优先级总上限 |
| max_prefetch_reads | ≤ 总在途量，必须为需求读和后台推进保留额度；为 0 时关闭推测 |
| max_delta_segments | >0；包括可见及仍被旧查询固定的增量段资源，具体内存与磁盘段分别统计 |
| group_commit_window | 非负；0 为不人为等待成组，仍须持久化后确认 |
| k | >0；结果工作区必须能容纳，超配置能力明确报错 |
| candidate_limit | ≥k；跨所有磁盘段的总候选上限，初版增量扫描另计行数和预算 |
| prefetch_blocks | 每查询允许推测的在途块数，不能超过全局或内存上限 |
| timeout | >0；从 Search 进入 API 起算，涵盖准入；不保证超时返回时内核已停止工作 |
| admission_timeout | >0；从写 API 进入起，限制可撤销准入阶段；进入 WAL 后等待实际结果 |

单个查询/写入的维度、上限和乘法先做溢出检查，再预留资源。客户端传入的 span 只需在同步调用返回前有效；写队列复制到预算内存，后台不借用调用者数组。

返回的 vector 由调用方持有。基准程序在同一受限域内，必须流式写结果并限制待写队列，不能在测量中累积全部结果逃出内存核算。查询日志的 I/O 开销需记录并在各组保持一致。

## 4 并发与关闭约定

Ready 状态支持多个线程并发 Search/Get/Insert/Upsert/Stats。Checkpoint 同一时间最多一个执行中的请求，可合并目标或返回 resource_exhausted；不能启动多个无界后台全量构建。

Close 可以与普通调用重叠，但同一时间只由一个线程执行 Close；先停止新准入，再等已准入调用和 I/O 完成。析构与任何成员调用不得并发，对象生命周期由调用方负责。

耗时操作不持有 publication_mutex。公开接口不会在用户回调中执行引擎逻辑，因为初版公开 API 不提供回调；内部读取完成回调另按下节限制。

## 5 底层读取与日志接口

内部契约声明见 [storage.hpp](../include/agent_io/storage.hpp)，这些接口供 A/B 对接，不作为上层稳定 API。

BlockKey 使用不复用的 FileId、对齐偏移和长度。FileId 由进程内文件注册表按每次打开递增分配，同一实际文件共享注册项；文件关闭后 ID 不再在本进程复用。跨进程缓存不存在，重启后重新分配即可。这样无需把“图代次”直接当成缓存唯一身份，也不会因 fd 被操作系统复用命中旧块。

ReadRequest 持有 FileLease 和 BufferLease，校验 `block.file_id == file.Id()`，偏移/长度无溢出，缓冲容量与对齐足够，读取范围不越文件。上层在提交后不得读写该缓冲，除非已收到成功完成。

| 操作 | 契约 |
| --- | --- |
| ReadBackend::Submit | 返回成功表示接管请求，完成回调恰好一次；返回错误则不回调、请求未接管 |
| ReadCallback | 可能在 Submit 返回前触发，允许乱序；不持有后端内部锁，必须 noexcept 地处理自己的业务，不递归等待 Drain |
| ReadCompletion | request_id 唯一标识在途请求；成功 bytes_completed 必须等于请求长度 |
| Drain | 调用者先停止新提交；排空已接管请求和全部回调，之后才可销毁后端 |
| WalWriter::Append | 仅写协调器调用；处理短写或报告错误，不等于持久化，不并发交错记录 |
| WalWriter::Sync | 同步所有此前追加的数据并返回状态；组提交由上层控制 |
| WalWriter::Rotate / Close | 组边界同步后轮换、关闭；错误后禁止通过轮换绕过恢复 |

`BlockReader::ReadBatch` 返回按输入排序的只读块租约，`TryPrefetch` 非阻塞准入；文件引用与存储资源按 [A 侧契约](stage3-a-contract.md)管理。静态 `IndexAdapter`、`ScratchRequest` 计量和 Manifest 编解码已经实现。`DeltaStore`、`DeltaPin`、写入发布与跨代次回收按冻结的 [INDEX-B-1.2](stage3-b-contract.md) 实现和验收，尚无动态端到端运行。

INDEX-B-1.1 的复审补充：ReadView/DeltaPin 为非所有权元数据，读取方法接收 ReadViewLease，由预先准备的生命周期对象固定段、增量和预算；未发布的最后预留槽可在 WAL 追加前 CancelLastSlot。Expand 显式接收 K，Refine 通过 RefinementRequest 接收租约、预算与同一查询期限；工作区包括两份重叠候选数组和导航上界。以上仅为声明，不是生命周期或并发实现验收。

同步后端允许在 Submit 内完成；IOCP 后端为每个请求保留独立 OVERLAPPED，完成线程通过 GetQueuedCompletionStatus 取得结果。ReadFile 返回 ERROR_IO_PENDING 表示已接管；立即成功也须遵守默认 IOCP 完成包规则，不重复通知。初版禁止设置跳过成功完成通知的模式。同步提交失败且未接管时不等待完成包；带非空 OVERLAPPED 的失败完成仍须归还在途资源。短读不能当作完整块，对齐模式下不盲目用未对齐尾缓冲重试。

取消查询仅解除等待者资格，ReadBackend 第一版不承诺内核取消。缓冲和文件在最终完成回调结束前仍被后端固定；共享缓存引用若仍有其他等待者，不能随单个查询超时撤销。

BufferLease 的可写入口只供后端或填充者使用，缓存发布后只读；同一缓冲不允许被多个在途写填充。缓冲池保证地址稳定与明确容量，不能将普通 vector 地址直接假定为无缓冲 I/O 合法地址。

## 6 契约演进

2026 年 11 月 17 日冻结组合：API-1.2 / STORAGE-A-1.1 / INDEX-B-1.2 / FORMAT-1 / EVAL-1.1，对应 DESIGN-1.1。11 月 21 日落地清单和段格式、首建发布、静态 Engine 及读优化；11 月 23 日另行交付 [WAL 编解码与 writer](stage6-a-wal.md)。本次状态同步不改变版本或字段语义。

公开类型或语义改变时修改 API 版本并更新头文件、文档、基准与调用示例。单纯新增计数不改变已存在字段含义。阶段二若证实依赖无法走该读接口，先记录决策并修改契约，不在实现中绕过后仍声称符合设计。

Windows 接口语义依据及持久化限制见 [Windows 平台约定](windows-platform.md)。平台类型保留在实现中，不将 windows.h 暴露到核心公开头文件；系统错误保存 GetLastError 的相应结果。
