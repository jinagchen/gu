# 第二阶段环境与验收记录

日期：2026 年 11 月 10 日。结论：按本机 Windows 的工程可行性范围，阶段二通过。按用户确认，不要求另一位成员执行复现。官方规则确认、完整 Engine、规模化性能、动态合并及 WAL 恢复属于后续验收。

## 1 退出条件与证据

| 条件 | 实测与产物 | 结果 |
| --- | --- | --- |
| C++ 构建与调试配置 | MSVC 19.43、SDK 10.0.22621；项目 Release 和 Debug 环境/接口目标，DiskANN Release | 通过 |
| Python 隔离依赖 | `.venv`，Python 3.12.10；全部包固定版本，`pip check`；NumPy 精确答案与 Matplotlib PNG 输出 | 通过 |
| 成熟磁盘图依赖 | DiskANN 0.6.1 准确提交、gperftools 子模块、NuGet 版本及许可证记录；保留两处最小补丁 | 通过 |
| 小数据构建、加载和查询 | 4096 × 32 float32，128 个独立随机查询，K=10、L=128、beam=4；两个进程重新加载 | 两次 Recall@10=1.0，ID 结果逐字一致 |
| 实际读路径可替换 | `PQFlashIndex → AlignedFileReader → ProbeReader → ReadBackend::Submit → ReadFile` | 每轮查询 16599 次图读取、67989504 字节 |
| 目标数据卷与 I/O | `D:\agent-io-stage2\20261110`；NTFS；逻辑扇区 512、地址对齐 4096；同步/IOCP/刷盘 | 通过 |
| 错误与生命周期探针 | 同步 EOF 短读、IOCP EOF 错误、不对齐读取拒绝、取消后完成排空；真实查询读取提交故障注入 | 通过，错误退出，无挂起 |
| 内存受限域及计量 | 启动前挂 Job Object，子进程自动归属、禁用逃逸，提交限额、超时清理；提交/私有提交/工作集分别报告 | 通过工程验证 |
| 容量与共存资源 | 512 MiB 文件写入、刷盘及首/中/尾抽样校验；旧索引保持打开时重建 8192 条新索引；容量预算算术与空间门槛检查 | 通过小规模验证与规划检查 |

原始结果在 `D:\agent-io-stage2\20261110`，汇总、输入/输出/源码/二进制 SHA256 在 `summary.json`。源码仓库保存 [证据快照](evidence/stage2/summary.json) 和关键日志/内存 JSON；大型数据和构建产物不入库。

## 2 构建与运行

在仓库根目录的 PowerShell 执行：

```powershell
.\tools\environment\bootstrap-stage2.ps1
```

脚本检查 NuGet SHA256、固定 DiskANN 提交和子模块并应用已记录补丁。构建依赖并安装锁定的 Python 3.12 隔离依赖后，配置和构建项目，运行 Release、Debug CTest 和小数据探针。首次下载依赖可能耗时。可用 `-DataDirectory` 指定有至少 40 GiB 空闲的 ASCII 路径。DiskANN 0.6.1 的窄字符文件接口要求数据路径为 ASCII。项目源码目录含中文已构建通过。

2026 年 11 月 28 日在 ASCII/LF 检出中复跑完整引导。从空项目依赖目录和新 Python 环境开始，使用本机代理访问依赖源。Release 和 Debug 各通过 18 项 CTest，两次 DiskANN 探针 Recall@10 均为 1.000。日志和测量摘要分别位于 `D:/agent-io-stages-1-7-clean-163548066/bootstrap.log` 与 `D:/agent-io-stages-1-7-clean-163548066/data/summary.json`。

已有构建时只重跑实验：

```powershell
.\.venv\Scripts\python.exe tools/environment/stage2.py
```

环境与 Job Object CTest 在 Release、Debug 各为 3/3 通过。完整构建日志在 `build/stage2-bootstrap.log`；后一次 CTest 会覆盖同目录 `LastTest.log`，证据目录分别保存两种配置的输出。接口目标只验证声明，不生成 Engine 实现。

## 3 内存实测口径

`job_runner` 在恢复主线程前分配 Job Object；默认子进程继承同一 Job，无 breakaway 标志。每 5 ms 枚举 Job 成员并记录 private bytes 与 working set 总和，同时读取内核 Job 提交峰值。被测 Python 重建协调器及其子进程均计入，离线 NumPy/绘图不在查询受限域内。

| 工作负载 | 提交限额 | 解释 |
| --- | --- | --- |
| 4096 条依赖构建 | 512 MiB | 构建限额单独披露，不冒充数据载荷的 20% |
| 单线程加载和 128 次查询 | 128 MiB | 实测提交约 11 MB；精确值见证据 JSON |
| 旧索引驻留 + 8192 条全量重建演练 | 512 MiB | 同一 Job 测量 Python 协调器、旧读取进程与新构建进程；峰值约 123 MB |
| 超额分配负例 | 64 MiB | 96 MiB 提交返回错误 1455，进程继续受控 |

加载前后私有提交在 `query-0.log` 中单独报告，包括依赖元数据与 scratch，不把差值全部解释为 PQ 元数据。初始约 1.2 MB，加载后约 9.8 MB；图查询不会整文件映射。PQ、入口/格式元数据仍走上游缓冲文件流，已披露绕行，后续正式适配须纳入统一核算。

负例中内核 `PeakJobMemoryUsed` 约 103 MB，而 96 MiB 提交已失败、采样私有提交约 2 MB；本机观测说明该峰值不能直接解释为实际成功提交或物理驻留。保留原值，不进行截断，也不据此保证“物理内存始终低于 64 MiB”。正常构建/查询/重建的内核峰值均未超过各自提交限额。

采样可能漏掉短峰值，工作集求和可能重复计算共享页；Job 提交限额不覆盖全部内核、系统缓存或物理内存。官方 Q-02/Q-03 尚未确认，这些明确口径的工程测量不等于最终官方内存合规。

## 4 容量估算与准入边界

[容量配置](../configs/stage2-capacity.json)是一组后续候选参数，不是已运行的百万条实验：100 万条、768 维、float32 原始载荷 3072000000 字节，20% 内存预算 614400000 字节。D 盘本次约有 102.56 GB 空闲；已实际写入/刷盘/抽样校验 536870912 字节，临时容量文件验证后删除。

最大图度数 32，每节点约 `768×4 + 32×4 + 4 = 3204` 字节，每 4096 字节扇区放一个节点，单代磁盘主图约 4.096 GB。原始数据双份约 6.144 GB、新旧主图约 8.192 GB、双代 PQ 约 128 MB，另留构建临时文件、版本目录、WAL 和实验输出；采用 40 GiB 空闲门槛，覆盖上述估算并留余量。官方数据维度或保留代数变化时必须重算。

| 资源 | 查询模式预算（字节） | 重建模式候选预算（字节） |
| --- | --- | --- |
| 缓存 | 245760000 | 100663296 |
| 增量 | 122880000 | 67108864 |
| 元数据 | 122880000 | 167772160 |
| 工作区（含后台任务） | 92160000 | 245301248 |
| 余量 | 30720000 | 33554432 |
| 合计 | 614400000 | 614400000 |

同时保留的活跃/冻结/旧查询增量总上限为 12288 条，按每条 3072+32 字节估算约 38.14 MB。8 个查询各自的影子表按每项预算 32 字节共约 3.15 MB；依赖每查询的 PQ/扇区 scratch 约 3 MiB，另留候选与精排空间。64 个在途 4 KiB 缓冲约 256 KiB，64 条写入载荷约 200 KiB。PQ 每向量拟用 64 字节，单代约 64 MB；双代保留在重建模式的元数据预算内，版本目录按需读，禁止额外常驻全量版本哈希表。

重建模式先回收缓存再预留资源，后台构建工作区候选上限 200 MiB，仅允许一个任务。磁盘段最多 16、保留代次最多 2；达到增量/代次/空间上限时拒绝新增工作或等待引用释放。这些是阶段三和实现的输入，尚未实现动态切换。

本次实际演练是小规模全量重建与旧读取进程共存，不是动态 Engine 合并。百万条全量重建能否在该预算下运行尚未实证；必须验证依赖分区/有界构建，不能直接放行全量内存重建。首次大规模构建若需提高限额应单独标注，不能借本次结果声称全流程同限额已通过。

## 5 后续验收

阶段二之后，小规模静态 Engine、同步对照、缓存与 IOCP 已验证，WAL 底层已交付。接下来验收 Engine 动态发布、进程恢复与多轮有界合并，再执行大工作集测试。官方平台、数据、依赖规则及内存口径继续按开放问题跟进；见[项目状态](project-status.md)。

本阶段通过表示原生 Windows 的依赖、读路径和资源测量具备小规模工程可行性，不表示全部 R-001 至 R-022 已通过，也不产生性能提升结论。

## 6 本机环境与基础探针

当前使用本机原生 Windows 编辑、构建和评测，不需要 WSL 或 liburing。此前曾计划在租用服务器运行；当时截图显示 Windows Server 2022、2 vCPU、4 GiB、40 GiB ESSD Entry，状态为已停止。未连接、修改或取消租用，不将截图视作实测。官方平台见[待确认事项](open-questions.md)。

| 项目 | 已记录环境 |
| --- | --- |
| 系统与硬件 | Windows 11 家庭版中文版 x64（10.0.26200）；Intel Core i7-14650HX（16 核、24 逻辑处理器）；内存约 16 GB |
| 本地存储 | 约 1 TB NVMe SSD，C/D 卷为 NTFS；2026 年 11 月 10 日空闲分别为 28,601,802,752 与 102,570,381,312 字节。正式实验前重新采集 |
| C++ 工具链 | Visual Studio Community 2022 17.13.6；MSVC 19.43.34810.0（工具目录 14.43.34808）；Windows SDK 10.0.22621.0；随附 CMake 3.30.5-msvc23 |
| 隔离 Python | 2026 年 11 月 23 日核对 `.venv`：Python 3.12.10、NumPy 2.2.6、Matplotlib 3.10.3；普通终端的 `python` 不一定指向该环境 |

### 6.1 独立环境探针

普通 PowerShell 找不到 `cmake` 时，可用 Visual Studio 随附的工具，只构建并测试 [Windows 环境探针](../tools/environment/windows-probe.cpp)：

```powershell
$cmakeExe = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctestExe = Join-Path (Split-Path $cmakeExe) 'ctest.exe'
& $cmakeExe -S . -B build/windows -G 'Visual Studio 17 2022' -A x64
& $cmakeExe --build build/windows --config Release --target windows_environment_probe
& $ctestExe --test-dir build/windows -C Release -R '^windows_environment_probe$' -V
```

最初的 1/1 验证记录如下。后续的卷上实验、计量限制及阶段二证据见本文件前文；最新静态 Engine 和 WAL 复测见[项目状态](project-status.md#本次复核)。

```text
file_flush=pass; logical_sector=512; alignment=4096
synchronous_unbuffered_read=pass; bytes=4096
iocp_unbuffered_reads=pass; requests=2; bytes=8192
job_commit_limit=pass; limit_bytes=67108864; rejected_bytes=100663296; error=1455; peak_job_commit_bytes=101670912
scope=functionality_only; engine_not_implemented
```

探针只操作系统临时目录中自己创建的文件，Job 限额作用于探针进程；当时验证的是 8 MiB 提交成功、96 MiB 提交被拒绝，不是整机物理内存上限。
