# 本机 Windows 平台约定

决策日期：2026 年 11 月 10 日。用户确定使用本机原生 Windows 开发、运行和评测，不使用 WSL 或租用服务器作为当前运行环境。当前冻结基线为 DESIGN-1.1 / [API-1.2](interface-contract.md)；任务书正文与需求编号不变。官方评测平台是否兼容仍属于 Q-07/Q-09。

平台决策时采用 API-1.1。2026 年 11 月 11 日新增 [第三阶段 A 契约](stage3-a-contract.md)时修订为 API-1.2，11 月 17 日随设计冻结；平台路线和本文件的持久化限制不变。

## 1 工程与 I/O

采用 x64、C++20、MSVC、CMake 与 Windows SDK。Python 继续负责数据准备、编排与离线评测，C++ 负责核心与计时。

| 能力 | 当前约定 | 验证边界 |
| --- | --- | --- |
| 同步读取 | CreateFileW + ReadFile | 本机临时文件内容比对通过 |
| 绕过系统文件数据缓存 | FILE_FLAG_NO_BUFFERING | 本机临时文件探针通过，不宣称绕过设备缓存 |
| 异步读取 | FILE_FLAG_OVERLAPPED + IOCP | 独立探针及真实静态 Engine 已验证；混合负载待验 |
| 缓冲对齐 | FileStorageInfo 探测，按逻辑/物理扇区和块大小分配 | 本次逻辑扇区 512 字节，保守地址对齐 4096 字节；正式数据卷需重新探测 |
| WAL 写入与刷盘 | 普通顺序 WriteFile + FlushFileBuffers | 已有底层轮换、失败与进程终止用例；未接入 Engine，不等于断电恢复 |
| 目录独占 | 独占打开 LOCK 的 HANDLE，生命周期覆盖 Engine | 静态 Engine 已实现；动态恢复编排尚未接入 |
| 内存限制 | 独立 benchmark Job Object 设置提交内存限额 | 已有启动器、子进程归属、提交拒绝及小数据构建/查询/重建计量；官方口径未关闭 |

每个已接管异步请求持有文件、缓冲和 OVERLAPPED，最终完成前不得回收。默认完成模式下 ReadFile 立即成功也会产生 IOCP 完成通知；只能处理一次业务完成。第一版不启用跳过成功通知的优化，不以 CancelIoEx 返回成功代替实际完成。

同一实际文件共享不复用的 FileId，缓存键不依赖可复用 HANDLE 数值。旧查询释放前不关闭或删除其使用的段；清单替换前关闭相关短期元数据句柄，避免共享模式冲突。

## 2 持久化与文件发布

不将 Linux 的 fdatasync、rename 和目录 fsync 调用逐字搬到 Windows。

目标写入顺序是 WAL 完整追加并 `FlushFileBuffers` 成功，再发布内存视图、确认写入。[WAL 底层](stage6-a-wal.md)已验证文件创建、轮换和进程终止后的截断观察；Engine 打开后的重放、尾部处理及恢复后续写仍待验证，不承诺断电安全。

新段及清单使用唯一文件名，关闭或刷盘后才允许被发布清单引用。CURRENT 替换拟用 ReplaceFileW，首次创建拟用同卷 MoveFileExW；完整步骤见[存储协议](storage-protocol.md)。ReplaceFileW 的 REPLACEFILE_WRITE_THROUGH 标志不受支持，不能借此声称已同步目录。

阶段目标先限定在经测试的本机 NTFS 进程故障恢复；系统掉电与设备故障保证尚未验证。Windows 发布/恢复协议通过故障注入前禁止启用 checkpoint 驱动的 WAL 删除，以免把未经证明的名字持久化假设变成不可逆的数据回收。

## 3 内存计量

Job Object 的 JobMemoryLimit 限制提交内存，不等同于物理内存或整个系统文件缓存上限。工作集限额也不能单独替代严格总内存约束。

独立 `job_runner` 将挂起的主进程先加入 Job Object 再恢复，子进程继承受限域，禁用逃逸。已测小数据构建、真实静态 Engine 查询和阶段二的旧索引驻留重建；Python 离线答案与绘图在查询计量域之外执行。记录 Job 内核提交峰值及每 5 ms 采样的进程私有字节和工作集；共享页和系统缓存单独披露。正式规模及动态运行待验。绕过存储层的读取或映射必须另行核算。

初始探针设置 67108864 字节 job 提交限额，8 MiB 分配成功，96 MiB 提交被拒绝，错误码 1455。QueryInformationJobObject 返回的 PeakJobMemoryUsed 为 101670912 字节，大于配置值；后续独立启动器复测同类负例，拒绝后采样私有提交仅约 2 MB，内核高水位仍约 103 MB。原始值如实保留，不能将该高水位当作成功提交量或宣称“峰值物理内存始终小于 64 MiB”。正常小数据运行的提交峰值、私有提交和工作集均有独立记录，见[阶段二验收](stage2-completion.md)。

Q-02/Q-03 关闭及受限大工作集实验完成前，只能报告明确口径下的工程自测，不能声称已满足全部官方内存规则。

## 4 依赖与容量

已固定 DiskANN 0.6.1 提交 `4c31367b6c3df5395bf8b786299988a5d001ec5a` 及两处本地补丁，Windows Release 构建、小数据加载/查询和同步读取适配通过。版本和许可证见[第三方记录](third-party.md)。数据目录采用 ASCII 路径；PQ 与格式元数据仍使用上游缓冲流，正式适配须继续核算。

若依赖仅在 Linux 通过验证，应先评估 Windows 适配或更换符合范围的成熟依赖，不自动改变用户选定的运行系统，不自行从零重写整套图检索算法。

本机有约 16 GB 物理内存；已选 D 盘并完成 512 MiB 容量写入/刷盘/抽样校验、百万条 768 维容量估算和小数据共存重建计量。正式实验仍须依据实测可用资源选数据与预算，数据工作集应超过声明内存限额；本阶段不宣称已测规模化吞吐。

## 5 官方资料

Microsoft Learn：Synchronous and Asynchronous I/O、I/O Completion Ports、File Buffering、FlushFileBuffers、ReplaceFileW、Job Objects、JOBOBJECT_EXTENDED_LIMIT_INFORMATION。它们是 Windows 语义依据，具体采用的行为仍须结合本机探针和后续故障测试验证。
