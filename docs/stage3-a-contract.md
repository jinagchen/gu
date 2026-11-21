# 第三阶段 A 侧存储与基准契约

日期：2026 年 11 月 17 日冻结修订，版本 STORAGE-A-1.1，对应 API-1.2、EVAL-1.1 和 DESIGN-1.1；FORMAT-1 的 WAL 字节布局保持不变。本文件定义 A 的交付，供 B 对接。同步存储与静态基准已有第四阶段实现，缓存、IOCP 后端与 WAL 编解码仍待实现；交接关闭情况见[冻结记录](stage3-freeze.md)。

## 1 范围与交接

| 边界 | A 提供 | B 提供或继续确认 |
| --- | --- | --- |
| 图读取 | FileRegistry、BlockReader、ReadBackend、只读块租约 | 原生节点定位、逻辑 ID/版本映射、IndexAdapter 与候选扩展 |
| 增量与工作区 | 预算分区、资源预约及模式切换约束 | DeltaStore 的稳定槽与前缀、RequiredScratch 的实际大小 |
| WAL | FORMAT-1 编解码、完整追加、同步、轮换和失败语义 | 序号分配、冲突判断、业务发布、日志恢复编排 |
| 后台任务 | 资源和 I/O 配额、文件延迟回收支持 | 合并切点、清单持久发布依据、新视图及尾部拼接 |
| 基准工件 | C++ 到达/计时/有界输出契约、成功与失败样例 | 输入动态轨迹、首次构建配置、独立答案与最终评测 |

首次构建仍由 B 定义 vector_build 输入、参数及工件。A 的交接要求是：构建单独放入受限 Job，所有依赖子进程不得逃逸；记录输入/输出哈希、峰值和独立构建限额；在 staging 使用唯一目录，文件关闭/同步后按存储协议发布；失败不覆盖可用 CURRENT；明确从原生节点到逻辑 ID 的映射及版本 0；窄字符依赖数据路径继续限制为 ASCII。大规模后台构建没有资源上界证据时返回 unsupported，不放宽运行限额。

## 2 文件与只读块

声明位于 [storage.hpp](../include/agent_io/storage.hpp)。FileLease::SizeBytes 表示允许读取的实际物理文件长度，自有尾部填充的逻辑长度由 B 的段元数据约束。

- FileRegistry::Open 只注册已存在的不可变文件，探测对齐并持有句柄。同一实际文件的别名共享 FileId；实现用文件身份去重，不能仅比较路径字符串。注册信息与句柄也占元数据预算，溢出返回 resource_exhausted。
- BlockLocation 同时携带 BlockKey 与 FileLease。必须核对文件身份、正长度、偏移加长度无溢出、读取不越界及对齐。FILE_FLAG_NO_BUFFERING 不满足时明确返回错误，不偷偷改成缓冲读取。
- BlockLease::Bytes 长度恰为 key.byte_count，只有成功填充后才公开。租约同时固定文件、稳定只读缓冲及预算预约；调用方可以拷贝 shared_ptr，最后一个引用释放后才允许回池。不得保存租约之外的裸 span。
- 缓存驻留块及被调用方固定的已驱逐块均占 cache 分区；已驱逐但仍被固定的块不能算成已回收。启用缓存时，未命中缓冲在分配前先尝试 cache 预约，成功后连同在途填充都占 cache；不能预约时可用 workspace 作不入缓存的需求读取。禁用缓存时完成块与在途缓冲都占 workspace。分区归属在分配至释放期间固定，不进行没有接口保证的跨分区转移。预取仅使用 cache，不能借 workspace 挤占需求读。

### 2.1 批读取与在途去重

ReadBatch 是同步等待边界，可以由 IOCP 后端在内部并行提交：

1. options.deadline 为当前进程 steady_clock 的绝对期限，包含准入、排队和读取；demand 或 background 合法，prefetch 必须使用 TryPrefetch。
2. 空批返回空数组；非空批最多 max_inflight_reads 项（重复项也占输出槽），超出返回 invalid_argument，B 必须分批。整个输出数组、等待者及其所固定的数据必须先能预约，不能靠分批循环累积无限块。
3. 成功返回与输入等长、同序的非空租约，重复键可共享同一租约。失败返回一个 Error，不向调用方返回半批或半块。
4. 相同完整 BlockKey 的并发未命中共享一个后端提交，缓冲区只允许一个填充者。失败扇出给全部仍等待者，不入缓存。重试是新请求，只有原请求完成后才能重用缓冲。
5. 超时只解除当前等待者，已接管 I/O 仍持有 FileLease、BufferLease、预算和在途槽直到最终回调。其他等待者继续；最后等待者离开也不能提前释放内核在用的缓冲。超时查询的排空尾部继续占原查询准入槽及所需工作区；最后相关请求完成后才归还，防止反复超时绕开 max_queries。
6. 需求读命中在途预取时提升等待优先级、增加逻辑需求计数，不重复提交。调用方输出槽和工作区引用必须存活到等待者解除，回调不能访问已返回调用的栈变量。
7. BlockReader::Drain 的调用者先阻止所有新 ReadBatch/TryPrefetch，等待已接管 I/O、回调及内部等待者处理完毕；不等待外部只读租约释放。租约的池/计费状态必须能独立存活到最后释放，随后才允许销毁资源拥有者。

第二阶段 ProbeReader 借用 DiskANN 缓冲并在 Submit 内完成，只证明同步适配可行，不能直接用于 IOCP。正式 B 适配器先持有 ReadBatch 返回的租约再复制到上游缓冲；若要与计算重叠，由 B 将上游批读取拆分为可调度批次并保留租约，不能把同步返回接口描述为已实现计算/读取重叠。

TryPrefetch 不等待容量。合法请求返回 admitted（新在途）、already_present（缓存或在途已有）、dropped（关闭推测、期限到、配额/预算不足）。非法块返回 invalid_argument，关闭后返回 closed；admitted 只表示已接管，不保证最终读成功。后续异步错误计入 I/O 错误，需求等待者会得到同一错误。

### 2.2 延迟回收

B 仅在新清单持久发布、旧发布者与所有旧 ReadViewLease 已释放且恢复协议允许回收后，锁外调用 Retire(file_id)。视图计数归零由 B 确认；此时独立 BlockLease/在途读仍可能持有文件。A 标记退休、从缓存和注册表移除强驻留引用、禁止其重新入缓存；已有 FileLease/BlockLease/在途读继续可用。重复 Retire 同一已知 ID 幂等，未知 ID 返回 not_found。

退休文件在本进程结束前不得再次 Open；新代次使用新路径、新身份。CollectRetired 是非阻塞回收尝试：仍有外部引用时保留待回收条目并成功返回；全部引用释放后才关闭句柄并删除文件。删除失败返回带系统错误的 io_error，保留待回收记录与磁盘计量以供重试。FileRegistry 必须活到这些租约结束，不因 Engine 对象先销毁而提前删文件。

块引用、旧查询固定的完整段集合和回收待办都计入保留资源。段目录及 CURRENT/WAL 的删除决定由 B 作出；A 不依据缓存为空自行推断数据已可删除。Windows 发布/恢复故障验收通过前，自动删除仍关闭。

## 3 资源预约、模式和背压

ResourceGovernor::TryReserve 是非阻塞操作，bytes 必须大于 0；不能满足时返回 resource_exhausted。每一实际分配由相同生命周期的 BudgetReservation 固定，shared_ptr 的拷贝不重复计费；pool 保留的空闲分配仍计费。内存释放之后才能释放预约，所有权转移时不能出现未计费间隙。

MemoryRegion 分为 cache、delta、metadata、workspace；reserve 是不可供普通分配消费的峰值余量，不提供对应枚举。TrySwitchMode 是原子改变分区上限，不是增加总额度。它允许先驱逐无引用缓存，但不得等待资源或持 publication_mutex；若任一实际占用仍超目标分区，返回 resource_exhausted，生效模式不变。调用方串行协调模式切换与维护任务，不能并发启动多个重建。

以下参数来自 [阶段二容量输入](../configs/stage2-capacity.json)，作为后续百万条、768 维的配置候选：

| 分区 | query 字节 | rebuild 字节 | 包含内容 |
| --- | --- | --- | --- |
| cache | 245760000 | 100663296 | 块、缓存索引/替换信息、已驱逐但仍固定的缓存分配 |
| delta | 122880000 | 67108864 | 活跃、冻结及旧查询固定的稳定槽 |
| metadata | 122880000 | 167772160 | PQ、新旧代次目录、文件注册表、控制结构与预算预约自身开销 |
| workspace | 92160000 | 245301248 | 查询 scratch/影子表/候选、I/O、写队列/WAL 编码、基准队列、后台构建 |
| reserve | 30720000 | 33554432 | 未归属开销及峰值余量 |
| 总额 | 614400000 | 614400000 | 外部 Job 与内部计费分别记录 |

rebuild_memory 缺省为 null：允许静态读，需构建新段的 Checkpoint/自动维护返回 unsupported。提供时两个模式 total_bytes 必须相等，各分区和必须等于总额；metadata/workspace/reserve 必须为正，cache 可为 0，delta 必须能容纳配置的稳定槽。关闭缓存不自动增加其他分区。

维护状态机：

```text
query -> preparing -> rebuild -> releasing -> query
            |                        |
       准入失败退回            旧引用未释放时保持 rebuild
```

1. B 提供候选输出大小、PQ/目录、临时工作区及切点 C；A 检查段数/代次/空间，压缩未固定缓存，并 TrySwitchMode(rebuild)。准备阶段停止新增高开销查询/写入预约以防无限饥饿，已有调用继续。
2. 模式切换后先预约后台工作区，候选上限为 209715200 字节，剩余 workspace 为 35586048 字节；前台已预约/新准入及日志、I/O 必须共同装入剩余额度。不满足时不启动任务，尝试退回 query。
3. 新旧 PQ 同时驻留也必须预约 metadata。最多一个维护任务；构建子进程纳入同一个 Job，不能只记 IPC 缓冲而遗漏其实际内存。无法有界预约第三方分配时拒绝任务并报告 unsupported。
4. B 完成清单/视图切换后释放构建工作区；旧代次仍固定的内存、文件和增量继续收费。TrySwitchMode(query) 只有各占用已装入 query 上限时才成功，失败时保持 rebuild 并对新任务背压。
5. 失败回退同样等已接管 I/O 和构建进程退出后释放资源，staging 残留计磁盘占用，不报告发布成功。

首次离线构建独立运行，使用 build-run.json 的独立 Job 限额；在线维护子进程必须加入引擎所在的同一受限 Job，不能新建平行计量域而遗漏其占用。若叠加更小的嵌套限额，它也必须仍受父 Job 总额限制。在线子进程实际内存同时由父 Job 约束与后台 workspace 预约覆盖，不与进程内构建工作区重复预约同一分配；IPC 等额外分配另计。受限关系必须在挂起创建后、恢复线程前建立，加入失败即终止任务且不执行构建；只设置 workspace 数字不等于完成 Job 归属。阶段二 job_process_tree 仅证明启动器的进程树机制，真实在线维护集成留实现验证。

ResourceLimits 及 ReadScheduling 对应 [engine.hpp](../include/agent_io/engine.hpp)：

| 参数 | 本版初值 | 强制条件 |
| --- | --- | --- |
| max_disk_segments | 16 | >0；当前、正在构建、旧查询固定及退休待删段的去重并集 |
| max_retained_generations | 2 | ≥2；当前/候选新代次/仍固定旧代次的去重并集，发布不额外重复计数 |
| max_retained_delta_vectors | 12288 | >0；活跃/冻结/旧引用槽总和，另检查真实分配字节 |
| max_delta_segments | 4 | >0；所有仍占资源的增量段，包括旧引用，不是磁盘段数 |
| min_free_disk_bytes | 42949672960 | 准备维护时 free ≥ max(该门槛, 新输出+临时文件+WAL 增长预约)；实际分配仍需错误处理 |
| demand_reserve | 8 | >0；低优先级不能占用这部分在途能力 |
| background_reserve | 1 | >0；维护活跃时需求读不能挤占这部分在途能力 |
| max_background_reads | 8 | ≥background_reserve，≤总在途-demand_reserve |
| demand_burst_before_background | 8 | >0；后台有待办且槽/预算可用时，最多连续提交这么多需求读后须服务一个后台请求 |

每个引擎请求总上限仍为 max_inflight_reads，预取还受 max_prefetch_reads 与查询 prefetch_blocks 限制。预取上限不得大于总量减两个 reserve；所有加减先检查上下界。后台不活跃时可将其保留槽借给需求，活跃时停止新增借用并等已借用请求排空，之后保证后台保留槽。需求读不抢占已提交 I/O，WAL Sync 是独立写路径，不能放进可丢弃预取队列。

准入按实际容量计算：向量载荷 dimension×4；64 条写请求还包含 FORMAT-1 帧和队列开销；64 个 4 KiB I/O 至少 262144 字节；8 查询影子表候选为 8×12288×32=3145728 字节；每查询 PQ scratch 约 3 MiB 只是阶段二估算，B 必须按参数给出实际上界；候选、精排、结果租约、池空闲和分配器开销另计。不能用上述载荷乘法冒充完整分配上界。

## 4 WAL 编码与提交交接

字节定义仍以 [FORMAT-1](storage-protocol.md) 为准。本阶段只提供下列声明与实现约束：

| 入口 | A 的保证 |
| --- | --- |
| WalFrameBytes | 正维度、溢出检查后返回 align_up(64+dimension×4+4,8) |
| EncodeWalFileHeader / DecodeWalFileHeader | 输入/输出头恰为 64 字节，校验 magic、格式、UUID、start_sequence>0、保留位和 CRC32C |
| EncodeWalFrame | span 载荷仅同步借用；输入有限 float32、合法操作、sequence>0；输出容量不足不写；成功返回帧长度，尾部补零 |
| DecodeWalFrame | expected_dimension>0；max_frame_bytes 足够；输入可包含后续帧，只解析首帧，成功 consumed_bytes=frame_bytes |
| CreateWalWriter | 独占创建唯一日志文件，写完整文件头并同步成功后返回；不打开未知旧尾继续追加 |
| Append | 只接受一个合法完整帧，序号符合当前文件连续性；处理短写，不等于持久化 |
| Sync | 同步此前完整追加，成功后由 B 发布并确认 |
| Rotate | 仅在已完成同步和发布的组之间调用；旧文件同步成功后关闭，next 的实例相同、序号为下一条；创建并同步新头后才成功 |
| Close | 幂等关闭并报告此前错误，不代替提交 Sync，不隐式确认业务；异常析构只尽力关闭 |

DecodeWalFrame 对不足 64 字节的合法头前缀或已校验头之后的不足帧返回 need_more，consumed_bytes=0，payload 为空，其余业务字段置零/不得使用；已经足够判断非法 magic/版本/保留位/长度的输入直接 corruption。完整帧 CRC 错误永远是 corruption。只有 B 的恢复器可以依据“最新日志且真实 EOF”决定 need_more 是否允许丢弃；非 EOF 继续取数据，历史封闭日志缺尾报 corruption。

解码 payload 是借用原输入的字节 span，不是本机 float 指针；B 必须按小端读取，不能 reinterpret_cast 未对齐数据。编码函数在写输出前完成参数与容量校验；CRC 范围只含头和载荷，头 CRC/帧 CRC 不混用。

WalOptions 初值：rotation_bytes=67108864，max_frame_bytes=1048576。两者须满足 rotation_bytes≥64+max_frame_bytes，当前维度的帧≤max_frame_bytes。达到文件阈值前 B 在组边界轮换，单帧/提交组不跨文件；组装组时不得超出当前文件剩余量，必要时先轮换。无记录的新文件不推进序号。

Append 一旦已尝试系统写入后失败，即使无法判断落盘长度，也按 may_have_applied 返回并永久停止本 writer 的后续追加；完全未开始写入的校验/准入错误为 none。Sync 失败使此前未确认组都不可确定。B 将状态切到 recovery_required，未开始追加的队列项返回 none；不能轮换新文件后继续报告成功。原生系统错误应立即保存。

B 对已同步组的发布必须不再分配；operation_id 只用于基准关联，不改变 FORMAT-1。恢复、CURRENT 替换和 WAL 删除由 B 编排，A 不自行推断 checkpoint 已覆盖哪些记录。

## 5 C++ 基准的生产端

值类型见 [benchmark.hpp](../include/agent_io/benchmark.hpp)，字段见 [基准输出协议](benchmark-output-contract.md)。本契约制定时尚未实现 vector_bench；2026 年 11 月 15 日已交付静态运行器与 CLI，真实 Engine 接入仍待完成，见[第四阶段基准交付](stage4-a-benchmark.md)。

- 所有预定到达先在 C++ 计数，operation_id 在本轮唯一，重试必须使用新标识并另存关联，不能悄悄替换失败记录。
- 有界客户端队列同时受条数和字节限制。请求借用的输入/复制的写载荷、worker、结果队列与日志线程同属 Job；队列分配和保留容量占 workspace，不在引擎预算外另开无限缓冲。
- 每个已进入 API 的请求在开始前预约一个有界结果槽，完成时必须能无分配记录结果。调度者至少保留一个独立的有界拒绝记录槽，通过日志线程流式输出，不在调度线程执行阻塞文件写；该槽同样计入队列条数与字节总额。日志拥塞不能暂停到达时钟，若连拒绝记录也无法完整投递，整轮以工件不完整失败退出。
- logger 没有无限回退列表。结果队列条数/字节、max_record_bytes 在运行前检查能容纳最大 K、错误消息与载荷定位；记录过大/磁盘写满使 run_incomplete=true，退出码 5。
- completed_ns 在 API 返回、结果形成时捕获，不包含之后日志落盘时间；队列/日志争用造成的实际调用延迟保留。序列化/日志 I/O 与物理设备、总日志字节另行报告。
- 有限窗口结束后排空 API、结果队列和引擎 I/O；排空期限内仍未完成者计 pending，不能删掉。没有运行前后静默点时不从非原子 Stats 拼出请求守恒。

BenchmarkLimits 初值 workers=1、client_queue_capacity=128、client_queue_bytes=1048576、result_queue_capacity=128、result_queue_bytes=1048576、max_record_bytes=65536。workers/capacity/record 均>0，两个 byte cap 各≥max_record_bytes，初始化分配失败直接报错；实际多槽预留按记录大小核算，不能把 capacity×max_record_bytes 当作已经分配的隐含额度。

## 6 具体交错推演

| 输入与交错 | A 的处理与资源释放 | B 的处理与结果 |
| --- | --- | --- |
| Q1 固定 S=7、文件 F1，随后 id=42 更新到 8 | Q1 的 F1 与块租约保留；不通过可变缓存条目覆盖旧字节 | Q1 只用≤7 的版本；写入 8 同步且发布后，新 Q2 可以看到 8 |
| 合并切点 C=10，新图 F2 发布时 Q1 仍固定 F1 | F1 与两代资源继续计费；收到 Retire 后只排退休，独立块/在途引用释放后 CollectRetired 才删除 | B 发布时拼回>10 的尾部，等旧发布者/Q1 视图释放且清单及恢复安全成立后，锁外请求 Retire |
| sequence=11 的 WAL 只写入半帧后进程退出 | Append 无法成功完成，不能被视为已同步；重启解码最新 EOF 返回 need_more | B 丢弃未完成尾，不确认 11；完整帧 CRC 错误则 Open 失败 |
| Q1/Q2 共享 F1 的同一块，Q1 超时，随后 IOCP 读失败 | Q1 脱离，缓冲与槽仍保留；失败传给 Q2；回调结束后归还；不缓存半块 | Q1 记录 deadline_exceeded，Q2 记录 io_error；均失败计数，结果不补旧版本 |
| 切 rebuild 时旧查询固定缓存 120 MB，目标 cache 100663296 | TrySwitchMode 返回 resource_exhausted，mode 仍 query，不能暂借 reserve | B 等引用释放或让维护准入失败；不启动第三方全量构建 |
| 序号 12 Append 完整、Sync 报错，后面仍有两条排队写 | 12 返回 may_have_applied；停止 writer；后两条未追加为 none | B recovery_required；工件标 requires_review，恢复前不宣称动态正确 |

以上是设计推演；缓存并发、模式切换、WAL 和进程恢复仍须在对应实现阶段做故障测试。
