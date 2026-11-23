# 第三阶段 B 侧索引、增量与动态评测契约

日期：2026 年 11 月 17 日。冻结版本 INDEX-B-1.2，对应 API-1.2、EVAL-1.1、STORAGE-A-1.1 与 DESIGN-1.1；FORMAT-1 的 WAL 字节布局不变。1.1 补齐全局序号、捕获前缀和工作区参数；1.2 补齐清单类型及逻辑读取/预取诊断。旧轨迹不再接受 `sequence` 或 `committed_prefix_at_arrival`，须重新生成。布局与接口见 [index.hpp](../include/agent_io/index.hpp) 及[冻结记录](stage3-freeze.md)。

实现补记：静态 IndexAdapter、首次构建、版本目录与清单已在[第四阶段](stage4-b-completion.md)实现；静态基线采用自有 FORMAT-1 图，DiskANN 是当时的选型，不是现有首建后端。动态 `DeltaStore`、跨段版本融合与合并仍是本契约规定的目标。

## 1 范围与交接

| 边界 | B 提供 | 依赖 A 的保证 |
| --- | --- | --- |
| 图读取 | 原生节点定位、逻辑 ID/版本映射、IndexAdapter 与候选扩展 | BlockReader 只读租约、在途去重、预取准入 |
| 增量与工作区 | 稳定槽、已提交前缀、冻结、RequiredScratch 上界 | 预算分区、ResourceGovernor 预约、模式切换 |
| 首次构建 | vector_build 输入、参数、受限域、staging 与发布编排 | Job 内子进程不逃逸、文件发布与回收支持 |
| 清单与目录 | 清单字段、版本目录布局、工件与哈希 | 清单刷盘/替换原语、恢复时不自动选旧清单 |
| 动态评测 | 输入轨迹、独立精确答案、召回与守恒检查 | 查询/写入工件字段与有界输出 |

B 不重新发明磁盘图算法。底层构图与原生格式沿用已固定的 DiskANN 0.6.1（提交 `4c31367` 加两处补丁），B 只负责适配、版本语义、增量层与评测。任何绕过 BlockReader 的读取必须按第 9 节披露，不得声称读路径已替换。

## 2 原生节点定位与逻辑 ID/版本映射

四类标识互不替代：

| 标识 | 类型 | 含义 |
| --- | --- | --- |
| VectorId | uint64 | 对外逻辑 ID，同 ID 不重复出现在结果中 |
| NativeNodeId | uint32 | 段内图节点编号，仅由适配器解释 |
| SegmentId | uint64 | 不可变段身份，与清单条目一一对应 |
| Sequence | uint64 | 提交序号，0 为首次构建版本，其后严格递增 |

- `IndexAdapter::Locate(segment, node, lease)` 只接受传入 `ReadViewLease.view` 内的段；租约为空、与实例或段集合不匹配时拒绝调用，未知段返回 `not_found`，节点编号越界或推导的字节范围超出段长返回 `invalid_argument`，已注册但布局不符返回 `corruption`。偏移计算为 block_index × block_bytes + offset_in_block，并检查乘加溢出。引用不存在节点的边不是错误源，适配器按截断处理并计入候选统计之外。
- `NodeLocation` 的 `block_index` 与 `offset_in_block` 由段布局和 `block_bytes` 推导，不接受调用方算术；读取字节数固定为 `layout.row_bytes`。B 不把行号直接当块号使用。
- `SegmentDescriptor::layout` 每段固定，Open 时校验 `row_bytes` 覆盖三个偏移与最大字段、且按 `block_bytes` 对齐向上取整不溢出；不一致直接报 `corruption`，不做兼容性重解释。
- 段内一个逻辑 ID 只保存该段覆盖范围内（`covered_sequence` 及以下）的最高版本；跨段允许重复 ID。版本 0 表示初始数据，未更新的数据在合并后仍保留版本 0，不能把合并切点当作新业务版本。
- 快照内某 ID 的可见版本，是该 ID 在增量前缀影子表与各段版本目录中 `version ≤ snapshot_sequence` 的最大值；不存在则不可见。不查询任何"全局最新版本"映射。
- 版本过滤只影响结果资格，不影响图导航：旧版本节点仍可作为入口与邻居参与扩展。删除入口或导航边会让召回不可比，禁止实现为"过滤即剪枝"。
- 合并产生的新段覆盖 `covered_sequence = C`，切点以上更新不进该段；旧段在视图仍引用时保持可定位。

## 3 候选与总预算

- `Candidate.compressed_distance` 只用于排序探索，**永远不得作为返回距离**。返回距离必须来自原向量、相同度量的 float64 累加，写入 `exact_distance`；未精排的候选该字段为空。
- `CandidateBudget::total_limit` 默认 128，是跨段合并后的候选上限；`per_segment_limit` 默认 64，在各段内部先截断再合并，防止单一大段饿死其他段；`delta_row_limit` 默认等于 `max_retained_delta_vectors`（12288）；`workspace_bytes` 是调用方提供的每查询可预约额度，`ComputeScratchPlan` 独立推导需求并与之比较，适配器不得自行放宽。
- 合并顺序：按 `(exact_distance 或 compressed_distance, VectorId)` 升序；同 ID 只保留快照内最高可见版本；合并后超过 `total_limit` 时截断并置 `candidate_limit_reached = true`。
- `ExpansionState` 语义固定：`exhausted` 表示图遍历自然结束；`more_available` 表示上游仍有未展开节点，因期限或预算停止；`budget_exhausted` 表示 `total_limit` 或 `per_segment_limit` 用尽。三者都可能是成功返回。
- 扩大候选预算即重新调用 `Expand`：初版不提供可恢复游标，必须从入口点重新开始；`ExpansionResult` 只报本次调用计数，不累加上一次。可恢复游标属后续替换项，不能在文档中提前宣称。
- 计数口径：`logical_blocks_read`/`logical_bytes_read` 只累计本次 Expand 成功需求读取返回的块条数及 key.byte_count 之和；重复输入、缓存命中和共享在途仍各计一次，失败整批不计成功获取，Refine 的读取不混入 Expand。两者不是后端提交/完成量。`prefetch_admitted` 只数本次 TryPrefetch 返回 admitted 的次数，不含 already_present/dropped；`prefetched_keys_used` 是本次 Expand 曾 admitted 且后来被本次成功需求获取的不同完整 BlockKey 数，单次 Expand 每键最多一次。它只是本查询的使用诊断，其他查询消费、同键多次驻留和失败均会使它不同于引擎计数；不得求和后冒充 EngineStats.prefetch_consumed。
- 后端真实接管、完成字节和预取首次消费，以 A 的 EngineStats 和 EVAL-1.1 engine_counters 为唯一来源，不以并发前后 Stats 差值分摊到查询。全局 `prefetch_consumed` 在需求首次合入预取在途或命中其驻留块时计一次，即使后续读取失败也不撤销；淘汰后新接管预取才开始下一周期。需求接管的同键 I/O 不因后续 TryPrefetch.already_present 改成预取。所有内部计数检查 uint64 溢出。
- 计算 prefetched_keys_used 所需键集合由候选/预取上限推导容量并计入 navigation_bytes；不能为诊断指标另开不受预算约束的容器。
- `ExpansionRequest.k` 为本次查询 K，必须满足 0<k≤budget.total_limit；`deadline` 是当前进程 `steady_clock` 的绝对期限。到期限时若已获得不少于 min(K, live_count) 的有效候选，可成功返回并置 `more_available`；否则返回 `deadline_exceeded`，不得返回半批候选让上层误当成完整结果。最终查询仍须完成原向量精排。
- `Refine(const RefinementRequest&)` 显式接收候选、查询向量、`ReadViewLease`、CandidateBudget 和同一查询期限；只对传入候选精排并再次过滤非当前可见版本，不得引入新候选。候选数超过 `budget.total_limit` 返回 `invalid_argument`；精排在期限内未完成则整个查询失败，不能重新起算超时。Expand/Refine 产生的 vector 容量均由调用方持有的查询预约覆盖，直到这些 vector 销毁，不在方法返回时提前释放预算。

## 4 DeltaStore 稳定槽与前缀

```text
ReserveSlot -> WriteSlot -> PublishPrefix（publication_mutex 下）
     |              |
     +-> CancelLastSlot（仅 WAL 追加前，按尾部逆序撤销）
                                 |
                              Freeze -> 协调器切换新活跃段
```

- `ReserveSlot` 只分配不发布，容量用尽返回 `resource_exhausted`；已分配未发布的槽对任何查询不可见。槽在 `retired` 且所有视图释放后才归还，`DeltaSegmentInfo::bytes_reserved` 覆盖活跃、冻结与旧引用槽。
- 单写协调器串行调用 ReserveSlot/WriteSlot/CancelLastSlot。冲突、校验和其他可失败的准入尽量先于预留；WAL 追加前失败时，`CancelLastSlot(slot)` 撤销 active 段最后一个未发布槽（可已填好），清除记录并回退预留尾部，多槽按逆序撤销。非尾槽、已发布槽和 frozen 段返回 `invalid_argument`；仅这种从未发布的尾部可以重用。开始 WAL 追加后禁止撤销，失败按不可确定写入进入恢复。否则一次 insert 冲突就可能留下永远不能发布的空洞。
- `WriteSlot(slot, id, sequence, values)` 接收写协调器分配的全局 sequence>0，同时保存 `DeltaSlot.sequence = DeltaSlot.version = sequence`。WAL 恢复传入原始序号；轮换不从局部槽号重新编号。要求槽已经 ReserveSlot 且尚未填充或发布，维度与数值合法；不合法返回 `invalid_argument`。每次预留只填充一次，不能在 WAL 编码后改写槽内容；成功不改变可见性。
- `PublishPrefix` 只在 active 段前进不后退，`new_prefix ≤ slot_capacity`，新增覆盖槽必须全部写完，槽内全局序号严格递增；缺槽、未写槽或乱序拒绝发布。协调器在追加 WAL 前验证新增组恰接上全局连续前缀，跨段轮换也不能跳号；WAL 同步成功后持有 `publication_mutex` 一次性发布槽前缀与全局 S。成功发布路径不分配，不因正常预算不足失败。前缀以内槽位不再改写，冻结后不得推进 frozen_prefix。
- `Freeze` 仅在提交组之间、没有未发布预留槽时执行；否则返回 `invalid_argument`，不能丢弃已准备或已追加 WAL 的槽。它在 `publication_mutex` 下把当时 `committed_prefix` 记为 `frozen_prefix`，返回 `state = frozen`；此后该段不再接受 ReserveSlot/WriteSlot/CancelLastSlot/推进前缀。协调器负责在冻结后切换到已预留的新活跃段，冻结前缀不变。
- `CapturePin()` 在 `publication_mutex` 下返回 `DeltaPin{segment, committed_prefix}`，不分配、不抛异常；捕获视图保存该值。查询只读 `< committed_prefix` 的槽，前缀外即使随后发布也不进入该视图。适配器不得缓存跨视图的槽指针或容器长度。
- `SlotRecord(slot, pin)` / `SlotValues(slot, pin)` 验证 pin.segment 与本段一致，前缀不超过已发布或冻结前缀；越界前缀返回 `invalid_argument`，未知段或 `slot >= pin.committed_prefix` 返回 `not_found`。这些范围检查不能证明 pin 确实来自旧快照：调用方只能使用同一 ReadViewLease 中的原始 pin，不得手工扩大前缀。SegmentId 在实例内不复用，避免旧 pin 命中新段。调用方通过 lease.lifetime 同时持有 DeltaStore，返回的只读 span 不能超出租约；DeltaPin 本身不是所有权句柄。
- `SlotId` 是槽位序号，不是逻辑 ID。`DeltaSlot` 固定 32 字节（4 字节槽号 + 4 字节对齐 + 8 字节序号 + 8 字节 ID + 8 字节版本），与 A 侧影子表 `8 × 12288 × 32 = 3145728` 的估算口径一致。
- 冻结段转成磁盘段时，发布把"冻结内存段"替换为等价磁盘段，`snapshot_sequence` 不回退；旧查询继续持有自己的内存段引用。

## 5 快照内部类型

`ReadView` 对应 [architecture.md](architecture.md) 第 4 节的内部结构，可拷贝、定长、捕获时不分配：

| 字段 | 约束 |
| --- | --- |
| instance_id | 与当前实例一致；不一致按 `closed` 或 `not_found` 处理，不当作新实例 |
| view_generation | 单调递增，仅用于事后关联工件，不参与结果正确性 |
| snapshot_sequence S | 捕获时已发布序号；视图只反映 ≤ S 的增量记录 |
| manifest_generation | 视图对应的清单代次，用于核对段集合与工件 |
| segments[16] / segment_count | 条数 ≤ `kMaxSegmentRefs`，与 `max_disk_segments = 16` 一致 |
| deltas[4] / delta_count | 条数 ≤ `kMaxDeltaPins`，与 `max_delta_segments = 4` 一致 |
| live_count | 视图内有效去重 ID 数，用于判断 `min(K, live_count)` |

- 字段在 Open/发布时已校验，捕获路径只做断言式检查；捕获不应返回 `resource_exhausted`——若捕获会分配，说明结构设计已偏离本契约。
- `ReadView` 是无所有权的定长元数据；复制它或 DeltaPin 不延长任何资源生命周期。适配器 Locate/Expand/Refine 必须接收 `ReadViewLease{view,lifetime}`，不能只凭裸 ReadView 发起读取。B 的 ReadViewLifetime 具体对象固定该 view 全部磁盘注册、DeltaStore 和预算预约；不得把来自不同发布状态的 view 与 lifetime 拼接。
- 生命周期对象及 shared_ptr 控制块在发布前、锁外准备并计入 metadata；同一资源集合的多次前缀发布可复用该对象。捕获在 publication_mutex 下复制元数据和已有 shared_ptr，不分配；旧发布者和所有查询租约均释放后，才允许锁外安排 Retire，不能在最后引用的析构中持发布锁做 I/O。两代资源及其空闲保留池持续计费。
- 查询返回前释放自己的 lease 和可释放工作区，不得跨请求缓存。已接管 I/O 的文件、缓冲、预约及查询准入槽按 A 的规则保留到完成；不能因超时返回而提前回收仍在使用的工作区。
- 对外不提供历史快照句柄：`snapshot_sequence` 只用于评测关联，不能据此重新 Search。

## 6 RequiredScratch 计量

`ComputeScratchPlan(const ScratchRequest&)` 的全部字段必须由参数推导，不接受小数据实测估算。输入显式携带 dimension、CandidateBudget（含 total_limit/per_segment_limit/workspace_bytes）、delta_slots、segment_count、max_degree、copy_buffer_blocks、block_bytes、上游 pq_scratch_bytes 和 navigation_scratch_bytes：

| 字段 | 推导 |
| --- | --- |
| shadow_table_bytes | delta_slots × 32（槽记录表）；若实现额外存哈希桶，按实际结构另加并说明 |
| candidate_array_bytes | (2 × budget.total_limit + segment_count × budget.per_segment_limit) × sizeof(Candidate)，覆盖 Expand 输出与 Refine 输出重叠存活；增量扫描复用有界总候选堆，不另攒 delta_slots 个候选 |
| refine_bytes | budget.total_limit × dimension × 4（原向量精排缓冲，一次性读取） |
| pq_scratch_bytes | 上游按实际压缩参数推导的字节上界，显式传入 optional 值；缺失返回 unsupported，0 仅表示已经证明无需该分配 |
| navigation_bytes | navigation_scratch_bytes，调用方按已打开索引的节点数/布局推导 visited、遍历队列、影子表额外哈希桶及其余控制/分配器开销的上界；缺失返回 unsupported，不能仅凭 max_degree 假定 visited 有界 |
| io_buffer_bytes | copy_buffer_blocks × block_bytes，覆盖需求读与预取的适配器复制缓冲；禁用预取不自动令需求复制缓冲归零，A 的块租约与在途缓冲另计 |
| total_bytes | 上述之和按 64 字节对齐累加 |

- `total_bytes` 是每查询上界，必须能装入 `workspace_bytes`；装不下时在准入阶段返回 `resource_exhausted`，不临时改用无限堆内存。
- 输入约束：segment_count≤16、delta_slots≤budget.delta_row_limit；预算和维度合法；所有乘加及对齐检查 uint64 溢出。额外导航、visited 表或第三方缓冲必须纳入已声明分配上界，否则返回 unsupported，不能只报告表内的部分分配。
- 实际峰值不得超过 `total_bytes`。实现若在某字段上界不足，返回 `unsupported` 并拒绝该查询，允许暂时降低 `prefetch_blocks` 或 `candidate_limit` 后重试。
- 维护模式（rebuild）下前台、I/O、写入/WAL 和基准队列共享 [STORAGE-A-1](stage3-a-contract.md) 第 3 节的剩余 35586048 字节；不是每查询各有这笔额度。调用方持有覆盖实际分配的 ResourceGovernor 预约，所有并发查询上界之和与其他占用共同装入剩余额度，模式切换不能释放旧查询仍持有的预约。
- 影子表只对已捕获增量前缀建立，大小随 `delta_slots` 而非全局 ID 数增长；不得退化为全量版本表。

## 7 首次构建受限域与 staging 发布

`vector_build --config <build.json>` 的字段以 [build-run.json](../configs/examples/build-run.json) 为准；下表用 JSON 路径书写，避免与嵌套结构不一致。

| 字段 | 约束 |
| --- | --- |
| `dataset.format` | 固定 `raw-f32le-v1`；`dataset.metric` 首版固定 `squared_l2` |
| `dataset.vectors` / `dataset.ids` | 无头行优先 float32 小端 + 对应 uint64 小端；路径必须为纯 ASCII |
| `dataset.vector_count` / `dataset.dimension` | 条数为十进制字符串，维度为有界整数；文件长度必须精确匹配，多一字节或少一字节都按 `invalid_argument` |
| `dataset.sha256.vectors` / `dataset.sha256.ids` | 正式运行必填真实哈希；示例可空但须标 `purpose = contract_example` |
| `native.max_degree` / `native.build_beam` / `native.pq_bytes` | 上游构图参数，原样传给依赖并原样记录；`build_beam ≤ max_degree` |
| `native.parameters` | 上游参数字符串，原样保存，事后不重新推导 |
| `native.version` / `native.native_version` | 依赖版本标识；`native_version` 是依赖自报的实际版本，未知写 null 并注明原因 |
| `limits.memory_limit_bytes` | 独立 Job Object 提交内存限额，覆盖构建进程及其子进程 |
| `limits.min_free_disk_bytes` | 默认 42949672960；准备阶段检查 `free ≥ max(该值, 输出+临时+WAL 预约)` |
| `limits.job_object` / `limits.measurement_scope` | 必须为 true / `build_process_tree`，否则峰值不可比 |
| `limits.max_peak_commit_bytes` | 声明的峰值上界，不得超过 `memory_limit_bytes`；超限按 `resource_exhausted` |
| `staging.directory` | 唯一目录、同卷、ASCII；`staging.reuse_allowed` 必须为 false，不复用上一次残留 |
| `output.directory` | 索引根目录，发布写入 `CURRENT`、`manifests/`、段文件；不得与 staging 相同或互相包含 |
| `output.checkpoint_sequence` / `output.generation` / `output.publish_current` | 首次构建固定 `"0"` / `"1"` / true |

构建流程：

1. 校验输入哈希、维度、文件长度精确匹配与 ID 唯一；失败返回 `invalid_argument`，不创建 staging。
2. 离线首次构建创建独立 Job 并设提交内存限额，在恢复构建进程线程前加入 Job；失败即终止、不开始构建。在线维护使用引擎同一 Job 及后台 workspace 预约，不能复用离线独立计量域绕开总额，详见 A 契约第 3 节。
3. 在 staging 生成段文件与版本目录，逐个 `FlushFileBuffers` 后关闭句柄。
4. 计算内容 SHA256、记录 `node_count`、`file_bytes`、`peak_commit_bytes`。
5. 按 FORMAT-1 第 5 节将段同卷移到 segments 的唯一正式目录后，写清单临时文件并刷盘，再发布为 `manifest-<generation>.json`；禁止清单引用仍在 staging 的文件。
6. 写 `CURRENT` 临时文件并刷盘，按 FORMAT-1 第 5 节替换；失败时保留旧 `CURRENT` 并进入需诊断状态。
7. 新清单持久发布后才切换视图；首次构建 `checkpoint_sequence = 0`。

失败语义：`published = false`，发布前的构建失败保留已有可用 `CURRENT`；CURRENT 替换报错时按 FORMAT-1 核查实际状态，不能假定系统自动回滚。staging 残留计入磁盘占用，不报告成功；构建内存超限按 `resource_exhausted`，输入损坏按 `corruption`，依赖不具备有界分配能力按 `unsupported`。离线首次构建与查询分开计量；在线合并必须和查询共用引擎受限域，其干扰属于动态评测成本。

`native_version` 记录依赖的实际版本标识。构建产物是本项目自有格式，不等于 DiskANN 原生索引可直接加载；两者关系须在交付文档中写明。

## 8 清单与版本目录工件

`Manifest` 定长可拷贝，`kMaxManifestSegments = 16`：

- `generation` 单调递增；`checkpoint_sequence` 为清单覆盖的最大提交序号；`live_count` 为清单内有效去重 ID 数。
- `segments[]` 每项包含段 ID/UUID、代次、种类、覆盖序号、条数、主图字节/哈希，以及 segment.json 的路径、逻辑/物理字节和哈希；`segment_count ≤ 16`，越界即 `corruption`。adapter、格式版本与 wal_replay_after 均在 Manifest 内显式提供，定长 ManifestText 不在发布时分配。
- 发布顺序固定为：段与目录已刷盘 → 清单临时文件刷盘并改名 → `CURRENT` 刷盘替换。任何一步失败禁止删除旧文件与日志。
- `ManifestBytes` / `EncodeManifest` / `DecodeManifest` 为声明，完整字段、64 KiB 上限、容量、错误与 UTF-8 JSON 规则唯一以 [FORMAT-1 第 2.1 节](storage-protocol.md#21-清单-json-与内存类型)为准。Decode 只核查哈希字段格式，Open 另外验证 CURRENT、segment.json 及其全部文件内容哈希，不能让纯 span 解码接口声称检查了磁盘。

版本目录：

- 自有格式沿用 FORMAT-1 的固定 32 字节小端记录，按逻辑 ID 升序：`id(8) + version(8) + node(8) + 8 字节保留零`。NativeNodeId 当前为 uint32，因此解码 node 时还要检查高 32 位为零，不能截断。禁止直接 dump C++ 结构体。
- 目录文件长度必须是 32 的整数倍；`EntryCount` 由清单与文件长度共同确定，两者不符即 `corruption`。
- `Find` 按页二分或稀疏定位，页读取必须经 BlockReader；目录对象和稀疏定位索引占 metadata，块缓冲按 STORAGE-A-1 固定占 cache 或 workspace，不重复改记 metadata。不得整体常驻，也不得在查询路径上一次性读入全部目录。
- `Find` 只返回该段内最高版本，不跨段合并；跨段可见性由查询的影子表逻辑决定。
- 本版所有可查询段都必须具备版本目录。`has_version_directory = false` 只用于描述尚未完成适配的产物，Open/发布查询视图时返回 `unsupported`，不得把段内 ID 判为不可见后继续查询。已声明具备目录但文件缺失或内容损坏时返回 `corruption`；目录读取失败按 `io_error` 使查询失败，不能降级为“无此 ID”。

## 9 PQ 与元数据绕行披露

- `IndexGeometry::pq_bytes_per_vector` 是上游实际为每条向量分配的压缩码字节数，不是参数声明值。取不到时返回 `unsupported`，不填估算。
- `unbudgeted_metadata_bytes` 非零即表示存在绕行：PQ 码表、原生加载期结构、依赖内部一次性缓冲等未纳入 `metadata` 预算的常驻字节。该值必须写入 `environment.json` 与 `effective-config.json`，并在内存占比结论中单独列出。
- 绕行存在时禁止声称"读路径已完全替换"或"内存占比已按引擎预算验证"；只能表述为"引擎内预算 X 字节，另有 Y 字节依赖分配未计入"。
- 若依赖只支持一次性全批读取，先调整适配设计（拆批、复制到自有对齐缓冲），不能假定已有重叠能力。适配器内复制必须计入 `io_buffer_bytes`。
- 压缩距离只用于候选探索；结果距离一律原向量 float64 重算。依赖无法提供原向量时必须保留原始数据文件或等价存储，不得用压缩距离近似返回。

## 10 动态输入与轨迹

动态运行的输入字段以 [dynamic-run.json](../configs/examples/dynamic-run.json) 为准，轨迹格式由 [trace.jsonl](../evaluation/examples/stage3-b/trace.jsonl) 与 [trace.py](../evaluation/trace.py) 共同定义。

轨迹逐行一个事件，按 `arrival_index` 从 0 稠密递增，`kind` 取 `write` 或 `query`：

| 字段 | 适用 | 约束 |
| --- | --- | --- |
| `operation_id` | 两者 | 本 run 唯一，与 `writes.jsonl` / `queries.jsonl` 按此关联；重试必须换新标识 |
| `arrival_index` | 两者 | 稠密递增，顺序即到达顺序；不表示发布顺序 |
| `kind` | 两者 | `write` / `query`，其他值按 `invalid_argument` |
| `operation` / `id` / `payload_row` | write | 请求类型、目标逻辑 ID 与 updates 文件行号；不预测 WriteReceipt.sequence |
| `query_id` / `query_row` | query | 输入查询 ID 与查询向量行号；query.ids.u64 对应行必须等于 query_id，不预测 snapshot_sequence |

- 轨迹行须带 `schema_version`、`protocol_version`、`purpose`、`run_id`；与运行工件不一致按工件不完整处理，不能靠"大概是同一轮"口头认定。
- operation_id 跨查询、写入及轨迹分别唯一，终结记录必须与轨迹一一对应，不能通过字典覆盖重复键。比对 operation、id、payload_row、query_id 和查询行号映射；重复查询 ID 可用于不同 operation_id。不同到达顺序不限制 worker 的 API 进入与完成顺序。
- 轨迹文件必须位于运行目录内，用相对路径引用，不允许 `..` 或绝对路径；`trace.sha256` 非空时由消费者校验内容。
- `trace.operations` 是**写操作条数**，不含查询；`trace.queries` 是查询请求条数。两者之和等于轨迹行数，终结记录与 pending 按类型共同守恒；dataset.query_count 则是源查询文件的行数，不是请求数。
- 轨迹驱动模式不重新抽样：`workload.query_fraction` / `insert_fraction` / `upsert_fraction` 必须与轨迹实际构成一致（容差 1e-9），冲突时以轨迹为准并拒绝运行，不能悄悄按比例重生成。
- `trace.sha256` 正式运行必填；示例可空但须标 `purpose = contract_example`。`seed` 只用于可复现地生成轨迹，不参与查询语义。
- `evaluation.trace` 的 CLI 和直接函数调用都只创建新目录，不覆盖已有运行；计数须为正整数，K 不超过初始向量数。一个写请求与一个查询也可生成；当写和查询各至少两个时保证至少两个不同快照，默认种子的既有轨迹保持不变。
- `snapshot_policy.mode = committed_prefix`：每个查询在实际进入 API 并捕获 ReadView 时确定已发布前缀。输入到达时不能预知排队和并发发布结果；失败/拒绝查询的快照为 null 并按零分计分。min_distinct_snapshots/max_queries_per_snapshot 是夹具生成端的覆盖目标，不能强制真实引擎命中输入预设快照。
- 答案由独立精确扫描生成（`answers.method = exact_scan_float64`），与引擎输出隔离；答案文件缺失或与查询集不匹配按工件不完整处理，不用近似结果顶替。
- 消费端校验所有输入文件哈希、载荷登记与每条结果的类型；成功邻居数必须等于 min(K,live_count)。measurement_start/end 限定测量期间的成功收据前缀，响应顺序不必按序号排列；窗口完成数与逐行时间重算一致。
- 非零测量起点必须提供 baseline={sequence,versions,sha256}：dataset.vectors/ids 是该起点的完整有效快照，versions 为对应行的 raw uint64 小端版本文件，各版本≤sequence；sequence 等于 measurement_start_sequence。预热写入包含在基线，不能再次混进测量日志；缺少基线不猜测版本。
- 本版没有删除且每条成功写只修改一个 ID，因此 baseline 的非零版本不能重复，最大版本必须等于起点（起点 0 时全为 0）；版本来源的真实性仍依赖受控基线导出与内容哈希。
- 消费端按严格 `completed_ns < api_start_ns` 检查写→写、写→查、查→写、查→查的发布前缀先后关系；重叠调用及时间刻度相等不推断先后。不可确定写之后不能再开始成功写入，已知 attempted_sequence 时更高或相同成功序号也不合法。此检查不代替真实并发压力测试。
- incomplete/requires_review 仍逐项检查邻居的结构、类型、版本≤S、去重与排序；只跳过依赖完整提交记录的状态重建与评分。终结记录不能借用另一请求类型的 operation_id 冒充 pending。独立答案必须关联轨迹中的查询；完整运行恰好覆盖成功查询，失败查询不保留伪快照答案。不完整运行允许缺终结记录的 pending 查询保留答案，但不计分。
- effective-config 中出现 workload/measurement 或 trace.snapshot_policy 时按完整字段校验，不忽略未知字段；比例对照轨迹，配置窗口对照 counters。coverage 目标仍不强制引擎命中预设快照。
- environment.json 与 answers.file 必须存在；measurement 还要求真实后端、输入校验、Job 限额及指标文件、线程与日志设备记录。当前校验不构成正式内存比例或性能验收。
- measurement 的 external_job_metrics 使用仓库 job_runner 原始 JSON 整数格式，核对字段、类型、限额与 environment 一致、非零内核峰值，以及完整轮次的退出码/超时状态。实际 `io_backend` 和布尔 `direct_reads` 必须与 engine 配置一致。samples.jsonl 必须存在并检查运行身份、资源类型、时钟；累计 Job 峰值不得回退（中间不可用采样也不重置已知峰值），且不得超过最终原始峰值。environment/effective-config 都必须披露一致的 unbudgeted_metadata_bytes。
- 内核原始高水位可能在分配被拒后超过 Job 限额，见[本机平台实验](windows-platform.md)第 3 节；不得将这种证据判为格式损坏或截断原值。消费端在 evaluation.json 的 `resource_evidence` 保留限额、原始峰值、`kernel_peak_exceeds_limit` 与 `memory_acceptance = not_evaluated`；超过限额时派生 `requires_review = true`，不计分并退出 2，待核查拒绝分配等原因。契约样例的 resource_evidence 为 null。消费端校验的是证据内部一致性，不能凭文件自述证明真实 Engine、设备或代码实际参与了运行，也不据此给出资源合规结论。
- evaluation.check 返回 0 表示工件完整且已计分，1 表示工件无效，2 表示 incomplete/requires_review、尚未计分。后两类不输出成功召回结论；invalid 会替换旧 evaluation.json，避免残留 pass。Recall 是否达标仍单独判断。

## 11 具体交错推演

| 输入与交错 | B 的处理与结果 | A 的处理与资源释放 |
| --- | --- | --- |
| Q1 固定 S=7，随后 id=42 upsert 到序号 8 | 影子表 42→8 被 `> S` 过滤；结果若含 42，版本只允许 ≤7 | 块租约按视图保留，不用可变缓存条目覆盖旧字节 |
| DeltaStore 已发布前缀 1000，Q1 早先捕获 900 | Q1 只读 <900 的槽；901 至 1000 对它不存在 | 前缀内槽不可改写，旧查询不因新发布失效 |
| 合并切点 C=10，Q1 仍固定增量前缀 6 | 发布时把 >10 的尾部拼入新视图，S 不回退；新段 `covered_sequence = 10` | 两代资源都计费，旧段待视图计数归零后才 Retire |
| 构建申请超过 `memory_limit_bytes` | 构建返回 `resource_exhausted`，`published = false`，`CURRENT` 不变 | Job 拒绝分配，staging 残留计磁盘，不删旧文件 |
| 版本目录页被缓存驱逐后 Q2 查同一 ID | `Find` 重新读取；稀疏索引占 metadata，块按 A 分区计费；读取失败使整个查询 `io_error`，不降级为"无此 ID" | 需求读优先于预取，失败扇出给全部等待者 |
| PQ 码表 12 MiB 未纳入预算 | 披露 `unbudgeted_metadata_bytes = 12582912`，内存结论单列，不并入引擎占比 | 该分配不经 BlockReader，按绕行登记，不重复计 cache |
| 本次 Expand admitted 8 个不同键，成功需求获取其中 3 个后结束，另一个查询也消费其中 1 个 | `prefetch_admitted = 8`、`prefetched_keys_used = 3`；不据此推断全局消费为 3 | 无其他交错时全局 submitted=8、consumed=4；已接管 I/O 持有租约至完成，不重复计两查询共享的首次消费 |

以上为设计推演。DeltaStore 并发发布、跨段版本合并、构建失败回滚与目录换页仍须在实现阶段做专项故障测试。

## 12 待验证事项

| 事项 | 状态 | 检查方式 |
| --- | --- | --- |
| 头文件语法与类型约束 | 已通过 | `header-smoke-b.cpp` 在 MSVC `/W4 /WX` 下编译 |
| ScratchPlan 上界是否真能装入 workspace | 未验证 | 大维度与 8 查询并发实测峰值 |
| 稳定槽前缀与视图一致性 | 未验证 | 并发发布 + 长查询压测 |
| 首次构建受限域 | 小夹具已验证，规模化未验 | Job 限额下记录大规模构建峰值提交 |
| 版本目录读取预算 | 未验证 | 驱逐压力下统计 metadata 驻留 |
| PQ/元数据绕行实际字节 | 未验证 | 依赖参数与进程实际分配对照 |

小规模首建和静态图适配不等于动态索引验收；后续依本契约验证增量、快照和跨代次资源。
