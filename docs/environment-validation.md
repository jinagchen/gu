# 阶段二环境验证记录

建立日期：2026 年 11 月 9 日。最新验证：2026 年 11 月 10 日。

状态：阶段二本机工程可行性验收通过。CMake Debug/Release、DiskANN 固定提交的小数据构建/加载/查询、同步读取适配、数据卷、错误/排空探针、Python 隔离依赖及 Job Object 资源计量均有实测。完整范围、证据、容量估算和限制见[阶段二验收记录](stage2-completion.md)。

验收范围调整：按用户 2026 年 11 月 10 日确认，阶段二不要求另一名成员按文档复现；构建步骤、依赖清单和实测结果仍须记录。正式官方内存口径与完整引擎验收不随本阶段关闭。

历史安排：此前曾规划本机编辑、租用服务器运行，随后讨论本机 Linux/WSL2，现由用户的本机 Windows 决策替代。服务器截图显示当时为 Windows Server 2022、2 vCPU、4 GiB、40 GiB ESSD Entry 且已停止；未连接或修改，也未取消租用。

## 1 环境分工

| 环境 | 用途 | 当前状态 |
| --- | --- | --- |
| Windows 本机 | 编辑、构建、运行测试、C++ 基准与 Python 评测 | 阶段二小数据可行性通过；完整 Engine 及正式实验待完成 |
| 租用服务器 | 历史资源，不承担当前开发及实验 | 有截图配置，未连接实测 |

本机使用已有 MSVC 与 Windows SDK，无需安装 WSL 或 liburing。异步方案采用 IOCP，详见 [Windows 平台约定](windows-platform.md)。官方环境尚未确认。

## 2 本机已核实的信息

以下为本机记录；不套用于远程实例。硬件和磁盘空间动态变化，正式实验需重新采集。

| 项目 | 本机实测 |
| --- | --- |
| 系统 | Windows 11 家庭版中文版，64 位，版本 10.0.26200 |
| CPU | Intel Core i7-14650HX，16 核、24 逻辑处理器 |
| 内存 | 物理内存约 16 GB |
| 磁盘 | 系统报告一块约 1 TB NVMe SSD；C/D 卷均为 NTFS |
| 空间快照 | 11 月 10 日 C 盘 28601802752 字节空闲，D 盘 102570381312 字节空闲；会随使用变化 |
| Python | 3.12.10；NumPy 2.5.3、Matplotlib 3.11.2 可导入，小数组距离计算通过 |
| Git | 2.45.2.windows.1 |
| SSH 客户端 | OpenSSH_for_Windows_9.5p2 |
| Visual Studio | Community 2022 17.13.6，发现 MSVC 14.43.34808、随附 CMake 和 Ninja 的文件 |
| C++ 工具链 | MSVC 编译器识别 19.43.34810.0，工具目录 14.43.34808，Windows SDK 10.0.22621.0；CMake 3.30.5-msvc23，Release 编译/链接及探针执行通过 |
| C++ 验证范围 | 项目 Debug/Release 探针与接口编译、DiskANN Release 构建及真实小数据查询通过；Engine 仍仅有声明 |
| WSL | 历史检查为未安装；当前原生 Windows 路线不需要 |

此前“未找到 CMake/C++ 编译器”仅反映普通终端的命令解析结果；当前已使用现有组件完成探针构建运行，不再是仅发现文件。本次没有安装或修改系统组件。

## 3 历史基础探针记录

以下保留最初 1/1 探针结果。后续已经扩展为 Release/Debug 各 3/3，以及实际磁盘图验证，最新记录见[阶段二验收](stage2-completion.md)。

入口为 [CMakeLists.txt](../CMakeLists.txt) 和 [windows-probe.cpp](../tools/environment/windows-probe.cpp)。普通终端未加载 CMake 时，使用已有安装的完整路径：

```powershell
$cmakeExe = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctestExe = 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
& $cmakeExe -S . -B build/windows -G 'Visual Studio 17 2022' -A x64
& $cmakeExe --build build/windows --config Release
& $ctestExe --test-dir build/windows -C Release -V
```

本次 Release 构建和 CTest 均成功，1/1 探针通过。实际输出：

```text
file_flush=pass; logical_sector=512; alignment=4096
synchronous_unbuffered_read=pass; bytes=4096
iocp_unbuffered_reads=pass; requests=2; bytes=8192
job_commit_limit=pass; limit_bytes=67108864; rejected_bytes=100663296; error=1455; peak_job_commit_bytes=101670912
scope=functionality_only; engine_not_implemented
```

探针只读写系统临时目录中自己创建的小文件，正常退出删除该文件。Job Object 只约束独立探针进程，不改变桌面、PowerShell 或其他应用。8 MiB 提交成功，96 MiB 提交失败。

当时只确认分配拒绝机制，不宣称已建立严格物理内存上限。后续已在 D 盘完成对齐、读写与错误验证，并用独立启动器分别记录成功运行和拒绝分配时的 Job 峰值、私有提交及工作集；负例的内核峰值异常与计量限制见最新验收记录。

构建产物和完整 CTest 输出位于 build/windows，已忽略进 Git；完整输出为 build/windows/Testing/Temporary/LastTest.log。探针测试时限为 20 秒。

## 4 本阶段关闭与后续边界

1. 已固定磁盘图依赖准确提交、许可证及本地补丁，小数据构建/加载/查询和读取适配通过。
2. 已建立 `.venv` 与完整版本锁定，NumPy 精确答案和 Matplotlib 输出通过。
3. 已选 D 盘数据目录，完成卷上探针、512 MiB 容量写入校验、依赖加载私有提交及旧读取进程/新构建进程共存测量，并建立百万条容量估算。
4. 已建立可复用的 Job Object 启动器，验证子进程归属、提交拒绝，记录私有提交、工作集与内核提交峰值；官方 Q-02/Q-03 保留。
5. 已通过短读、提交失败、异步错误和取消排空探针；完整引擎 Close、WAL/清单发布和进程恢复随对应实现验收。

本阶段通过不代表完整引擎、官方内存比例或性能通过。小数据重建演练与容量估算不替代百万条或持续混合负载实测。无需连接服务器，完整恢复测试随实现进行。
