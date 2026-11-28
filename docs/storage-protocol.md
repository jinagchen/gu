# 磁盘格式和恢复协议

设计版本 FORMAT-1，日期 2026 年 11 月 10 日，对应 DESIGN-1.1。`vector_build` 发布静态索引的 `CURRENT`、清单、段及版本目录。Engine 实现 WAL 确认与重放、检查点、代次切换及正常回收。2026 年 11 月 28 日复测范围见[前七阶段问题清单](stages-1-7-issue-register.md)。

2026 年 11 月 11 日增加 A 侧编码/解码、追加、同步、轮换和回收交接。2026 年 11 月 23 日实现 [WAL 底层](stage6-a-wal.md)。2026 年 11 月 26 日补齐 [Engine 恢复和 WAL 文件头发布](stage6-b-completion.md#2026-年-11-月-26-日恢复修复)。FORMAT-1 的字段、字节序和 CRC 范围保持原定义。

2026 年 11 月 17 日冻结补充：清单 JSON 与 INDEX-B-1.2 的定长内存类型逐字段对应，规则见第 2.1 节。WAL 和版本目录字节布局不变；此前未实现清单编码，本次首次明确其完整 schema。[阶段三冻结记录](stage3-freeze.md)列出交接结论与后续实现验收。

## 1 所有权与目录

```text
index/
  LOCK
  CURRENT
  manifests/
    manifest-<generation>.json
  segments/
    <segment-uuid>/
      segment.json
      adapter-native-files...
      versions.bin
  wal/
    wal-<start-sequence>-<uuid>.log
    wal-<start-sequence>-<uuid>.pending
  staging/
    <build-uuid>/
```

LOCK 在 Windows 由独占打开并保持的文件 HANDLE 实现，不以“文件存在”判断占用。目录级只允许一个读写实例；初版不支持另一个进程直接同时打开它读，外部访问通过同一个引擎。

CURRENT 是小型 UTF-8 JSON，包含 format_version、instance_id、manifest 文件名及其 SHA256。manifest 和 segment.json 都先写临时文件再发布；路径必须为索引目录内的相对路径，校验不得逃出根目录，不跟随任意外部路径。

每次生成不可变段使用新 UUID 和文件名，不覆盖已有原生索引文件。BlockKey 中 FileId 是运行时注册身份，不是磁盘 UUID，不需要跨重启保持数值一致。

## 2 清单与不可变段

| 清单字段 | 含义 |
| --- | --- |
| format_version | 自有清单版本，首版 1 |
| instance_id | 构建时生成并长期固定的索引实例 UUID |
| generation | 清单代次，十进制字符串，单调递增 |
| checkpoint_sequence | C；清单中的完整段集合覆盖所有已提交操作 ≤C |
| dimension / metric | 单索引固定维度及 squared_l2 |
| live_count | C 时刻有效逻辑 ID 数量，十进制字符串 |
| adapter | 实现名、精确提交、原生格式版本及所需构建参数 |
| segments | 段 UUID、范围、文件路径、真实长度、填充长度和校验信息 |
| wal_replay_after | 等于 C；恢复按日志重放 C 之后完整有效记录 |

初版没有删除，所以各新 ID 只增加 live_count，upsert 已存在 ID 不增加。C 覆盖的是完整提交前缀，不是“文件中碰巧最大的序号”；刷新较晚段却漏掉早段时不能推进它。

segment.json 列出所有必需文件的长度、SHA256、原生格式和版本目录信息。全文件校验用于构建完成与离线核查；每次读取需有块级或原生格式校验策略。依赖没有块校验时必须记录缺口，不能宣称每次读取都已检测静默损坏。

主图和向量原生编码由适配器决定，自有版本目录采用固定 32 字节小端记录，按逻辑 ID 升序排列：

| 偏移 | 类型 | 含义 |
| --- | --- | --- |
| 0 | uint64 | logical_id |
| 8 | uint64 | sequence |
| 16 | uint64 | adapter_node_id |
| 24 | uint64 | 保留，必须为 0 |

每个不可变段内，一个逻辑 ID 只保存该段覆盖范围内最高版本；跨段仍可能有重复 ID。manifest/segment 元数据保存条数、记录大小和校验，文件本体不再有独立原生结构体头。字节序手工编码，禁止直接 dump C++ struct。

版本目录按页二分或稀疏定位，稀疏信息占元数据预算，页经过相同读取与缓存。开启 direct_reads 时可将自有文件尾补零到探测对齐单位，真实逻辑长度另存；第三方文件的填充必须先证明不会破坏加载，不能盲目追加。

### 2.1 清单 JSON 与内存类型

唯一 schema 示例为 [manifest.json](../configs/examples/manifest.json)，声明见 [index.hpp](../include/agent_io/index.hpp)。示例哈希全部为零，是未对应实体文件的协议夹具，不能作为可打开索引；清单本身没有 EVAL 的 purpose/schema_version 字段。

| JSON 字段 | C++ 来源及约束 |
| --- | --- |
| format_version / instance_id | Manifest 同名字段；版本为整数 1；非零 UUID，规范小写连字符表示，内存按 RFC 4122 字节序保存 |
| generation / checkpoint_sequence / live_count | 同名 uint64，编码为规范十进制字符串；generation>0 |
| dimension / metric / block_bytes | 正整数 dimension≤65536；metric 固定 squared_l2；block_bytes 为正的 uint32 二次幂，实际设备对齐另由 Open 验证 |
| adapter | ManifestAdapter；name、完整 40 位小写 commit、native_version 各≤128 UTF-8 字节，parameters≤512 字节，均非空、无控制字符；原生版本未知时禁止发布可用清单 |
| layout | NodeRecordLayout 的全部六个字段，均 uint32 JSON 整数；dimension 与清单一致，0<max_degree≤512，row_bytes 为 block_bytes 的正整数倍 |
| segments | segment_count 个 ManifestEntry，最多 16 个；id/uuid 各自唯一；固定数组其余条目不参与编码 |
| wal_replay_after | 同名字段，必须等于 checkpoint_sequence |

layout 的三个不重叠区间分别为 vector_offset 起 dimension×4 字节、neighbor_offset 起 4+max_degree×4 字节（uint32 邻居数与节点号）、metadata_offset 起 16 字节（uint64 ID/版本），都不得超出 row_bytes。节点可以跨多个对齐块；实际原生布局不能满足时由适配器转换或返回 unsupported，不据此假定 DiskANN 原生行就是此布局。

每个 segments 条目恰含以下字段：

| JSON 字段 | 约束 |
| --- | --- |
| id / uuid | 非零 uint64 字符串及持久段 UUID；id 在同一索引实例内分配且不复用，重启从持久清单及已保留资源恢复分配水位 |
| generation / kind / covered_sequence | 0<generation≤清单代次；kind 为 base/merged/flushed_delta；covered_sequence≤C，base 固定 0 |
| node_count / file_bytes / content_sha256 | node_count 为正、≤2³²；主图物理文件字节为正且按 block_bytes 对齐，至少 node_count×row_bytes；主图完整物理内容 SHA256 |
| metadata | ManifestFile，恰含 path、logical_bytes、physical_bytes、sha256；path 固定 segments/&lt;uuid&gt;/segment.json，≤1024 UTF-8 字节；0<logical_bytes≤physical_bytes，哈希覆盖完整物理文件 |

SHA256 一律 64 位小写十六进制字符串，编码时由 Digest 转换。segment.json 再登记所有必需原生文件与 versions.bin 的相对路径、逻辑/物理长度、内容哈希及布局；这些文件引用均限制在该段目录内，不允许重解析点或路径逃逸。Open 先验证 CURRENT 中的清单哈希，再验证清单中 metadata 的长度/哈希，最后按 segment.json 验证必需文件和版本目录；不能只校验主图。段内布局须与当前清单 layout 一致，异构布局本版不支持。live_count≤段条数总和；段条数总和检查 uint64 溢出。没有段只允许 C=0、live_count=0。

`ManifestText<N>` 显式 size≤N，不含终止符，未使用的字节为零。JSON 内存类型不直接落盘。`ManifestBytes` 校验全部输入后返回编码的准确字节数；`EncodeManifest` 的对象键顺序固定为 manifest.json 示例顺序（嵌套对象亦同），段数组按传入顺序，不序列化 segment_count。输出紧凑 UTF-8 JSON、无 BOM/尾换行/NUL。非 ASCII 字符直接编码 UTF-8，不做 Unicode 归一化；双引号与反斜杠分别写为 `\"` 与 `\\`，斜杠不转义；文本不允许控制字符。两者对非法输入、长度溢出或编码超过 65536 字节返回 invalid_argument；Encode 还先检查输出容量，任何失败均不得改写输出，成功只写前返回长度个字节。编码工作区也须预约。

`DecodeManifest` 最多接收 65536 字节；解析完整 JSON，可接受首尾 JSON 空白，拒绝重复/未知/缺失字段、尾随非空白、错误类型、非规范数值、非法 UTF-8、非法路径、重复身份、越界与不一致布局，返回 corruption。合法但非 1 的格式版本返回 unsupported。它只能验证哈希字段的编码，文件内容哈希由 Open 验证；其接口没有文件系统输入，不能声称已核查磁盘内容。解码临时空间须有界并计入 workspace，返回对象占 metadata。本次只有声明与 schema 夹具校验，C++ 编解码及损坏文件测试留实现阶段。

## 3 WAL 记录 FORMAT-1

WAL 用普通顺序 WriteFile 与 FlushFileBuffers，初版不启用 FILE_FLAG_NO_BUFFERING。所有整数字段为小端；浮点载荷按 IEEE 754 binary32 小端编码。每条记录独立校验，不承诺多记录事务。

每个日志文件先有 64 字节文件头，记录从偏移 64 开始。文件头为 magic `AGIOLOG1`（8 字节）、format_version=1（uint32）、header_bytes=64（uint32）、instance_id 的 RFC 4122 字节序 UUID（16 字节）、start_sequence（uint64）、零保留位（20 字节）及 CRC32C（4 字节，对前 60 字节计算）。写入器将起始序号写入文件名和文件头。恢复以文件头为准，验证实例身份以及起始序号与首条记录的一致性。文件名使用每个文件独立的 UUID，与头内的实例 UUID 无关。空的新日志允许暂时没有首条记录，此时可见序号不推进。

每条记录的固定头也为 64 字节：

| 偏移 | 字节 | 字段 | 约定 |
| --- | --- | --- | --- |
| 0 | 8 | magic | ASCII AGIOWAL1 |
| 8 | 2 | format_version | 1 |
| 10 | 2 | operation | 1=insert，2=upsert |
| 12 | 4 | header_bytes | 64 |
| 16 | 8 | frame_bytes | 头、载荷、CRC 和补齐的总长度 |
| 24 | 8 | sequence | >0，按写协调器提交顺序递增 |
| 32 | 8 | logical_id | 无符号 64 位 |
| 40 | 4 | dimension | 必须等于清单维度 |
| 44 | 4 | flags | 初版为 0 |
| 48 | 8 | payload_bytes | dimension × 4 |
| 56 | 8 | reserved | 0 |

随后为向量载荷、4 字节 CRC32C 和零填充：

```text
frame_bytes = align_up(64 + dimension * 4 + 4, 8)
```

CRC32C 使用 Castagnoli 多项式，标准初始值及最终异或均为 0xffffffff，对“头 + 载荷”的确切字节计算，不包括 CRC 字段或填充。实现使用标准 `"123456789" → 0xe3069283` 向量和固定字节样例验证。

读取头后先检查最大维度、合法运算和 frame_bytes 上限，再分配或读取载荷；不能信任日志中的长度。保留字段和填充非零、维度错误、非法操作或完整帧 CRC 不匹配均报 corruption。

新 WAL 在同目录用 `CREATE_NEW` 创建唯一的 `.pending` 文件，写完文件头并刷盘。关闭临时句柄后，通过 `MoveFileExW` 同卷改名为 `.log`，禁止覆盖。写入器独占重开并复核新文件后，才允许追加业务帧。发布或接管遇到 I/O 错误时，当前引擎停止写入，等待下次 `Open` 重新判定。轮换只发生在已同步的记录边界。文件名的断电持久化尚待实测。

## 4 成功写入协议

资源预留和 ID 冲突检查在追加之前完成，已发布稳定槽不可更改：

1. 协调器按顺序为合法操作分配序号，编码并处理完整追加。
2. 对当前组的 WAL 执行 FlushFileBuffers，检查返回状态并保存对应系统错误。
3. 在 publication_mutex 下发布完成的前缀及最高可见序号。
4. 向对应调用方返回成功；已经完整同步但来不及响应的写入允许在恢复时出现。

日志保存 operation 类型用于审计；恢复处理已通过准入的完整记录，不重新运行会变的业务冲突判定。若发现 insert 与恢复状态矛盾则报 corruption，不悄悄把它改成 upsert。

同步或追加失败后，对已经可能写入的请求返回 may_have_applied，并阻止后续写入。完全没开始追加的排队请求返回 none；已被同步但尚未逐个响应的组成员也不能凭空当成未生效。

## 5 清单发布协议

以下流程已在本机 NTFS 上进行进程级验证。文件内容刷盘、名字切换和重启恢复有各自的回归用例。断电后的文件名持久性仍待设备试验。

1. 在 staging 构建并核查段，逐个 FlushFileBuffers 后关闭构建句柄。
2. 使用同卷 MoveFileExW 将段放到 segments 的唯一正式位置，不覆盖现有段；检查返回结果。MOVEFILE_WRITE_THROUGH 不能当作完整目录持久化证明。
3. 写并刷盘新的 manifest 临时文件，关闭句柄后发布为唯一代次文件名。
4. 写并刷盘 `CURRENT.tmp`，记录清单路径和哈希。维护使用 `ReplaceFileW` 切换 `CURRENT`，备份使用唯一文件名。首次发布使用同卷 `MoveFileExW`。替换报错后重新读取 `CURRENT` 并比对切换前的内容哈希，只有能确认仍为旧指针才允许原进程重试。
5. 重开并校验新代次，在发布锁下采纳新视图。保留切点 C 之后的增量尾部和对应 WAL。清单满时，后台先合并已落段前缀来释放槽位，再检查点增量尾部。
6. 检查点采纳后回收 C 已完整覆盖的封闭 WAL。重开时也尝试删除通过恢复扫描、且最高序号不超过 C 的非空 WAL，保留含待重放记录的日志。合并采纳后退役已被取代的文件，文件句柄释放后可删除旧段。旧段删除失败后通过进程内队列重试。

后台任务输出只覆盖 C 以内数据，C 以上已确认写入仍在 WAL 中，恢复新清单时继续重放。新清单发布前崩溃则恢复旧清单和足够 WAL；未引用新文件只是孤儿，不是已提交数据。

发布失败保留旧代次与覆盖恢复边界所需的 WAL。已留下的孤立清单代次在重试时跳过。`CURRENT` 指向不明或切换后无法采纳时返回 `recovery_required` 并停止新写入。重开只采用能校验的 `CURRENT`，备份与更高代次清单留待人工诊断。

段数准入计入当前段、已知待删段和临时输出。检查点为下一次合并保留一个输出槽。已有 16 个活跃段的遗留清单，在无待删段且配置上限为 16 时，可以临时创建一个段完成恢复合并。该操作也需要通过内存和可用磁盘预算检查。

## 6 Open 与恢复

1. 获取独占锁，校验 CURRENT 指向的清单与实例身份、格式和必需文件；缺失或不支持明确报错。
2. 装载清单中的完整持久化前缀 C，按实际内存预算建立目录与元数据。
3. 只读扫描 `.log`，按头内起始序号排序并逐帧检查。最多收集 65,536 个日志文件描述，待重放记录上限由可用 delta 容量决定。文件列表、扫描帧和重放记录的估算开销不得超出查询模式 workspace 分区。跳过 `.pending`。外来实例、重复序号及跨入 C 之后的断档报 `corruption`。C 内的空洞可来自已回收的封闭日志。
4. 仅接受最新日志真实 EOF 处与下一序号匹配的合法帧前缀。缺失或损坏的 `.log` 文件头、历史日志残尾、完整帧 CRC 错误均报 `corruption`。扫描完成后返回最新文件的身份、文件头、扫描长度和最后完整帧的结束偏移。
5. 检查增量层容量，按序重放 `> C` 的记录，恢复到最后完整有效序号 S。完整但未回复调用方的帧可以重放。容量不足时返回 `resource_exhausted`。
6. 在实例锁内以 `OPEN_EXISTING` 独占打开最新 WAL，并复核文件身份、头和长度。同一句柄负责定位、截断和刷盘。已验证的最新日志即使长度正常也执行一次 `FlushFileBuffers`，以便重试上一次可能尚未同步的截断。任何修复 I/O 失败返回带原生错误码的 `io_error`，`Open` 不发布引擎。
7. 最新 WAL 修复后若只剩合法文件头且起始序号为 S+1，移交该句柄给写入器。对于已有完整帧的最新 WAL，下一次写入按需创建新的日志。
8. 在实例锁内检查 `segments/` 中当前清单未引用的 UUID 目录，以及根目录下的 `checkpoint-<uuid>` 和 `merge-<uuid>` 暂存目录。每个父目录最多扫描 4,096 项。只删除内容限于普通 `graph.bin`、`versions.bin`、`segment.json` 文件的这些目录，跳过重解析点和含有未知文件的目录。回收遇到共享冲突时进入本进程重试队列。`Open` 发布 `ReadView(S)` 并返回。

日志中清单覆盖范围 ≤C 的记录可跳过重放，但文件完整性和实例归属仍需核查。首次构建也按同样清单发布协议创建 C=0 的索引。

## 7 故障与交错推演

| 场景 | 预期处理 |
| --- | --- |
| 查询先捕获 S，随后同 ID 更新到 S+1 | 查询只扫已捕获前缀，返回 ≤S 的最高版本；后续查询看到 S+1 |
| WAL 写到半帧进程退出 | 校验、重放后截去最新日志的合法残尾并刷盘 |
| 截断完成但进程在刷盘前退出 | 下次 `Open` 重新验证最新 WAL，即使长度正常也再次刷盘 |
| 新 WAL 在改名前退出 | 恢复跳过 `.pending` |
| 已发布新 WAL，但第一帧尚未写完 | 合法空日志在恢复后接管，用同一文件写第一帧 |
| WAL 完整同步后、发布或响应前退出 | 重放完整记录，允许调用方未收到成功的操作生效 |
| 已确认写入后、增量未刷盘时退出 | 通过 WAL 恢复，不依赖进程内增量幸存 |
| CURRENT 替换前退出 | 旧清单加 WAL 恢复，未引用段不自动采用 |
| CURRENT 发布后、内存切换前退出 | 新清单加 C 之后 WAL 恢复 |
| 旧查询仍引用被替换图 | 保留文件与块引用，引用释放后回收；缓存不能把新文件误认成旧 fd |
| 在途读失败或短读 | 全部等待者收到失败，缓冲待完成后回池，不发布半块 |
| 同步报错或完整帧校验错误 | 停止写入或 Open 失败，不无条件截断并继续 |

这些是协议推演，不是故障测试结果。进程退出、系统掉电和物理设备故障是不同覆盖级别；本轮默认验收先覆盖本机 NTFS 进程恢复。切换到 Windows 后必须重新验证创建、替换失败及进程中断场景，持久化假设依赖实际系统、文件系统和设备兑现同步语义。

## 8 平台依据与实现验证项

- Microsoft Learn File Buffering：FILE_FLAG_NO_BUFFERING 对齐与缓存约束。
- Microsoft Learn FlushFileBuffers：文件缓冲刷新与设备相关限制。
- Microsoft Learn ReplaceFileW / MoveFileExW：文件替换、错误与标志限制。
- Microsoft Learn I/O Completion Ports：异步提交、完成队列与资源生命周期。

原 Linux 发布方案不作为 Windows 已具备的保证，详见 [Windows 平台约定](windows-platform.md)。

WAL 文件轮换默认 64 MiB，单帧默认上限 1 MiB，组边界轮换；底层实现和故障测试见 [第六阶段 A](stage6-a-wal.md)。图原生格式、每块校验方案、数据文件填充可行性、运行时目标卷对齐和构建内存开销仍随 B 的适配与实现验证。改变 FORMAT-1 字段、字节序或 CRC 范围须升级格式，不靠编译器结构体布局隐式变更。
