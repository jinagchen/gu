# 第三阶段 B 侧索引、增量与动态评测契约

日期：2026 年 11 月 16 日。版本 INDEX-B-1，对应 API-1.2、EVAL-1.1、STORAGE-A-1 与 DESIGN-1.1；FORMAT-1 字节布局保持不变。本文件定义 B 的交付，供 A 对接与联合复核。声明位于 [index.hpp](../include/agent_io/index.hpp)，全部为声明，尚无 IndexAdapter、DeltaStore、首次构建或合并实现。

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

- `IndexAdapter::Locate` 只接受在当前 `ReadView` 内的段；未知段返回 `not_found`，节点编号越界、`block_index` 与 `offset_in_block` 相加超出段长返回 `invalid_argument`，已注册但布局不符返回 `corruption`。引用不存在节点的边不是错误源，适配器按截断处理并计入候选统计之外。
- `NodeLocation` 的 `block_index` 与 `offset_in_block` 由段布局和 `block_bytes` 推导，不接受调用方算术；读取字节数固定为 `layout.row_bytes`。B 不把行号直接当块号使用。
- `SegmentDescriptor::layout` 每段固定，Open 时校验 `row_bytes` 覆盖三个偏移与最大字段、且按 `block_bytes` 对齐向上取整不溢出；不一致直接报 `corruption`，不做兼容性重解释。
- 段内一个逻辑 ID 只保存该段覆盖范围内（`covered_sequence` 及以下）的最高版本；跨段允许重复 ID。版本 0 只出现在首次构建的 base 段。
- 快照内某 ID 的可见版本，是该 ID 在增量前缀影子表与各段版本目录中 `version ≤ snapshot_sequence` 的最大值；不存在则不可见。不查询任何"全局最新版本"映射。
- 版本过滤只影响结果资格，不影响图导航：旧版本节点仍可作为入口与邻居参与扩展。删除入口或导航边会让召回不可比，禁止实现为"过滤即剪枝"。
- 合并产生的新段覆盖 `covered_sequence = C`，切点以上更新不进该段；旧段在视图仍引用时保持可定位。

## 3 候选与总预算

- `Candidate.compressed_distance` 只用于排序探索，**永远不得作为返回距离**。返回距离必须来自原向量、相同度量的 float64 累加，写入 `exact_distance`；未精排的候选该字段为空。
- `CandidateBudget::total_limit` 默认 128，是跨段合并后的候选上限；`per_segment_limit` 默认 64，在各段内部先截断再合并，防止单一大段饿死其他段；`delta_row_limit` 默认等于 `max_retained_delta_vectors`（12288）；`workspace_bytes` 由 `ComputeScratchPlan` 给出，适配器不得自行放宽。
- 合并顺序：按 `(exact_distance 或 compressed_distance, VectorId)` 升序；同 ID 只保留快照内最高可见版本；合并后超过 `total_limit` 时截断并置 `candidate_limit_reached = true`。
- `ExpansionState` 语义固定：`exhausted` 表示图遍历自然结束；`more_available` 表示上游仍有未展开节点，因期限或预算停止；`budget_exhausted` 表示 `total_limit` 或 `per_segment_limit` 用尽。三者都可能是成功返回。
- 扩大候选预算即重新调用 `Expand`：初版不提供可恢复游标，必须从入口点重新开始；`ExpansionResult` 只报本次调用计数，不累加上一次。可恢复游标属后续替换项，不能在文档中提前宣称。
- 计数口径：`blocks_read` 与 `bytes_read` 是经 BlockReader 的去重后真实块数与字节；`prefetch_admitted` 来自 `TryPrefetch` 的 admitted，`prefetch_consumed` 每个驻留周期最多记一次，多个等待者不重复计成功。
- `deadline` 是当前进程 `steady_clock` 的绝对期限。到期限时若已获得不少于 K 的有效候选，可成功返回并置 `more_available`；否则返回 `deadline_exceeded`，不得返回半批候选让上层误当成完整结果。
- `Refine` 只对传入候选精排并再次过滤 `version > snapshot_sequence` 的项，不得引入新候选；候选数超过 `total_limit` 返回 `invalid_argument`。

## 4 DeltaStore 稳定槽与前缀

```text
ReserveSlot -> WriteSlot -> PublishPrefix（publication_mutex 下）
                                 |
                              Freeze -> 协调器切换新活跃段
```

- `ReserveSlot` 只分配不发布，容量用尽返回 `resource_exhausted`；已分配未发布的槽对任何查询不可见。槽在 `retired` 且所有视图释放后才归还，`DeltaSegmentInfo::bytes_reserved` 覆盖活跃、冻结与旧引用槽。
- `WriteSlot` 要求 `slot < slot_capacity`、槽未发布（`slot >= committed_prefix`）；重复写已发布槽、维度与索引维度不符、含非有限值均返回 `invalid_argument`。写入成功不改变可见性。
- `PublishPrefix` 只前进不后退，`new_prefix ≤ slot_capacity`；调用方必须持有 `publication_mutex`。成功返回后前缀以内槽位在本次实例生命周期内不再改写。
- `Freeze` 在 `publication_mutex` 下把当时 `committed_prefix` 记为 `frozen_prefix`，返回 `state = frozen`；此后该段不再接受 `WriteSlot`。协调器负责在冻结后切换到已预留的新活跃段，冻结前缀不变。
- 捕获视图保存 `DeltaPin{segment, committed_prefix}`。查询只读 `< committed_prefix` 的槽；前缀外即使随后发布也不进入该视图。适配器不得缓存跨视图的槽指针或容器长度。
- `SlotRecord` / `SlotValues` 对不超过调用方捕获前缀的槽合法；对视图不可见的槽返回 `not_found`，不用 `invalid_argument` 混淆"越界"与"不可见"。
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
- `ReadView` 只持有段标识，不持有 FileLease。B 必须维护视图引用计数，计数归零前不得对相关文件调用 `Retire`；`Retire` 之后已有块租约仍可由 A 保持可用。
- 查询在返回结果或错误前释放视图与工作区；不得把视图存入跨请求缓存。
- 对外不提供历史快照句柄：`snapshot_sequence` 只用于评测关联，不能据此重新 Search。

## 6 RequiredScratch 计量

`ComputeScratchPlan` 的全部字段必须由参数推导，不接受小数据实测估算：

| 字段 | 推导 |
| --- | --- |
| shadow_table_bytes | delta_slots × 32（槽记录表）；若实现额外存哈希桶，按实际结构另加并说明 |
| candidate_array_bytes | candidate_limit × sizeof(Candidate) + 段数 × per_segment_limit × sizeof(Candidate) |
| refine_bytes | candidate_limit × dimension × 4（原向量精排缓冲，一次性读取） |
| pq_scratch_bytes | 由上游压缩参数决定的上界，必须显式给出；不得填 0 表示"未测" |
| io_buffer_bytes | prefetch_blocks × block_bytes（适配器复制缓冲）；A 的块租约与在途缓冲不计入此处 |
| total_bytes | 上述之和按 64 字节对齐累加 |

- `total_bytes` 是每查询上界，必须能装入 `workspace_bytes`；装不下时在准入阶段返回 `resource_exhausted`，不临时改用无限堆内存。
- 实际峰值不得超过 `total_bytes`。实现若在某字段上界不足，返回 `unsupported` 并拒绝该查询，允许暂时降低 `prefetch_blocks` 或 `candidate_limit` 后重试。
- 维护模式（rebuild）下前台可用的剩余工作区按 [STORAGE-A-1](stage3-a-contract.md) 第 3 节为 35586048 字节，`ComputeScratchPlan` 的结果必须与之比较后再准入。
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
2. 创建独立 Job 并设提交内存限额，启动构建（同进程受限线程或子进程均可，子进程不得逃逸 Job）。
3. 在 staging 生成段文件与版本目录，逐个 `FlushFileBuffers` 后关闭句柄。
4. 计算内容 SHA256、记录 `node_count`、`file_bytes`、`peak_commit_bytes`。
5. 写清单临时文件并刷盘，再发布为 `manifest-<generation>.json`。
6. 写 `CURRENT` 临时文件并刷盘，按 FORMAT-1 第 5 节替换；失败时保留旧 `CURRENT` 并进入需诊断状态。
7. 新清单持久发布后才切换视图；首次构建 `checkpoint_sequence = 0`。

失败语义：`published = false`，不覆盖任何可用 `CURRENT`；staging 残留计入磁盘占用，不报告成功；构建内存超限按 `resource_exhausted`，输入损坏按 `corruption`，依赖不具备有界分配能力按 `unsupported`。构建期间不得与查询共用同一内存计量域，否则峰值与召回实验都不可比。

`native_version` 记录依赖的实际版本标识。构建产物是本项目自有格式，不等于 DiskANN 原生索引可直接加载；两者关系须在交付文档中写明。

## 8 清单与版本目录工件

`Manifest` 定长可拷贝，`kMaxManifestSegments = 16`：

- `generation` 单调递增；`checkpoint_sequence` 为清单覆盖的最大提交序号；`live_count` 为清单内有效去重 ID 数。
- `segments[]` 每项包含段身份、代次、种类、覆盖序号、条数、字节数与内容哈希；`segment_count ≤ 16`，越界即 `corruption`。
- 发布顺序固定为：段与目录已刷盘 → 清单临时文件刷盘并改名 → `CURRENT` 刷盘替换。任何一步失败禁止删除旧文件与日志。
- `ManifestBytes` / `EncodeManifest` / `DecodeManifest` 为声明：`EncodeManifest` 容量不足时不写任何字节并返回 `invalid_argument`；`DecodeManifest` 校验长度、版本、条数与哈希，不一致返回 `corruption`，不猜测修复。

版本目录：

- 自有格式为固定 32 字节小端记录，按逻辑 ID 升序：`id(8) + version(8) + node(4) + 12 字节保留零`。禁止直接 dump C++ 结构体。
- 目录文件长度必须是 32 的整数倍；`EntryCount` 由清单与文件长度共同确定，两者不符即 `corruption`。
- `Find` 按页二分或稀疏定位，页读取必须经 BlockReader 并计入 metadata 预算；不得整体常驻，也不得在查询路径上一次性读入全部目录。
- `Find` 只返回该段内最高版本，不跨段合并；跨段可见性由查询的影子表逻辑决定。
- 允许 `has_version_directory = false` 的段，此时该段内 ID 只能被增量覆盖或被判不可见；清单必须显式标注，不能让缺目录表现为"查不到即无此 ID"。

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
| `operation` / `id` / `sequence` / `payload_row` | write | `sequence` 从 1 连续，`id` 为该操作的目标逻辑 ID，`payload_row` 指向 updates 文件中的行号 |
| `query_id` / `query_row` / `committed_prefix_at_arrival` | query | 查询捕获的快照序号；消费者必须验证 `queries.jsonl` 的 `snapshot_sequence` 与之一致 |

- 轨迹行须带 `schema_version`、`protocol_version`、`purpose`、`run_id`；与运行工件不一致按工件不完整处理，不能靠"大概是同一轮"口头认定。
- 轨迹文件必须位于运行目录内，用相对路径引用，不允许 `..` 或绝对路径；`trace.sha256` 非空时由消费者校验内容。
- `trace.operations` 是**写操作条数**，不含查询；`trace.queries` 是查询条数。两者之和必须等于轨迹行数，且必须与轨迹文件实际统计一致，配置与文件不符按配置错误处理。
- 轨迹驱动模式不重新抽样：`workload.query_fraction` / `insert_fraction` / `upsert_fraction` 必须与轨迹实际构成一致（容差 1e-9），冲突时以轨迹为准并拒绝运行，不能悄悄按比例重生成。
- `trace.sha256` 正式运行必填；示例可空但须标 `purpose = contract_example`。`seed` 只用于可复现地生成轨迹，不参与查询语义。
- `snapshot_policy.mode = committed_prefix`：每个查询在到达时捕获当期已发布前缀，`min_distinct_snapshots` 与 `max_queries_per_snapshot` 是生成端的配额，不是引擎侧限制。
- 答案由独立精确扫描生成（`answers.method = exact_scan_float64`），与引擎输出隔离；答案文件缺失或与查询集不匹配按工件不完整处理，不用近似结果顶替。

## 11 具体交错推演

| 输入与交错 | B 的处理与结果 | A 的处理与资源释放 |
| --- | --- | --- |
| Q1 固定 S=7，随后 id=42 upsert 到序号 8 | 影子表 42→8 被 `> S` 过滤；结果若含 42，版本只允许 ≤7 | 块租约按视图保留，不用可变缓存条目覆盖旧字节 |
| DeltaStore 已发布前缀 1000，Q1 早先捕获 900 | Q1 只读 <900 的槽；901 至 1000 对它不存在 | 前缀内槽不可改写，旧查询不因新发布失效 |
| 合并切点 C=10，Q1 仍固定增量前缀 6 | 发布时把 >10 的尾部拼入新视图，S 不回退；新段 `covered_sequence = 10` | 两代资源都计费，旧段待视图计数归零后才 Retire |
| 构建申请超过 `memory_limit_bytes` | 构建返回 `resource_exhausted`，`published = false`，`CURRENT` 不变 | Job 拒绝分配，staging 残留计磁盘，不删旧文件 |
| 版本目录页被缓存驱逐后 Q2 查同一 ID | `Find` 重新读取并计 metadata；读取失败使整个查询 `io_error`，不降级为"无此 ID" | 需求读优先于预取，失败扇出给全部等待者 |
| PQ 码表 12 MiB 未纳入预算 | 披露 `unbudgeted_metadata_bytes = 12582912`，内存结论单列，不并入引擎占比 | 该分配不经 BlockReader，按绕行登记，不重复计 cache |
| 预取 admitted 8 块，查询在第 3 块后早退 | `prefetch_admitted = 8`、`prefetch_consumed = 3`，节流比如实报告 | 已接管 I/O 继续持有租约直至回调整完成，随后入缓存 |

以上为设计推演。DeltaStore 并发发布、跨段版本合并、构建失败回滚与目录换页仍须在实现阶段做专项故障测试。

## 12 待验证事项

| 事项 | 状态 | 检查方式 |
| --- | --- | --- |
| 头文件语法与类型约束 | 已通过 | `header-smoke-b.cpp` 在 MSVC `/W4 /WX` 下编译 |
| ScratchPlan 上界是否真能装入 workspace | 未验证 | 大维度与 8 查询并发实测峰值 |
| 稳定槽前缀与视图一致性 | 未验证 | 并发发布 + 长查询压测 |
| 首次构建受限域 | 未验证 | Job 限额下构建并记录峰值提交 |
| 版本目录读取预算 | 未验证 | 驱逐压力下统计 metadata 驻留 |
| PQ/元数据绕行实际字节 | 未验证 | 依赖参数与进程实际分配对照 |

本契约可用于 A/B 并行实现；在此之前不得宣称索引层、增量层或构建链已实现。
