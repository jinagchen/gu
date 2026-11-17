# 第三方依赖与许可证记录

日期：2026 年 11 月 10 日。准确版本见 [依赖锁定](../configs/dependencies.lock.json)，Python 全部直接和传递依赖见 [requirements.lock.txt](../evaluation/requirements.lock.txt)。本表记录本地包中的许可证声明，不替代官方依赖准入要求 Q-09。

## 项目许可与第三方材料

2026 年 11 月 17 日确定：团队自有软件内容采用 [Apache-2.0](../LICENSE)，原创项目文档采用 [CC-BY-SA-4.0](../LICENSE-docs)，具体范围见 [README](../README.md#许可证)。

上述许可不替代第三方依赖自身的许可。使用或分发 DiskANN、其依赖及本地补丁涉及的上游代码时，保留相应原始版权和许可声明；补丁的修改范围见下文。赛题任务书原文、文档中的外部引用和外部数据集也不因收录进本项目而被重新授权。

## 依赖记录

| 依赖 | 固定版本或提交 | 本地许可证来源 |
| --- | --- | --- |
| DiskANN | 0.6.1 / `4c31367b6c3df5395bf8b786299988a5d001ec5a` | MIT；`build/deps/DiskANN/LICENSE` |
| gperftools | `fe85bbdf4cb891a67a8e2109c1c22a33aa958c7e` | BSD 三条款声明；`build/deps/DiskANN/gperftools/COPYING` |
| Tessil robin-map / robin-set | 随上述 DiskANN 提交固定 | MIT；`build/deps/DiskANN/include/tsl/robin_map.h` 等头文件开头 |
| Boost、program_options、unit_test_framework | 1.78.0，vc143 | Boost Software License 1.0；随包头文件声明，NuGet 原包保留在 `build/diskann/packages` |
| Intel MKL static win-x64 | 2022.0.3.171 | Intel Simplified Software License，August 2021；对应包根目录 `license.txt` |
| Intel OpenMP devel / redist win | 2022.0.3.3747 | Intel Simplified Software License，August 2021；两个包根目录各自的 `license.txt` |
| NumPy | 2.2.6 | 安装包 `numpy-2.2.6.dist-info/LICENSE.txt`，含其捆绑组件声明 |
| Matplotlib | 3.10.3 | 安装包 `matplotlib-3.10.3.dist-info/LICENSE` 及随附声明 |

Python 其余包已全部固定版本；原始 wheel 和安装包中的许可证及捆绑组件声明应随正式发布包一并审查、保留，阶段二不宣称已完成最终分发审查。MSVC、Windows SDK 使用本机既有安装，不纳入项目源码或二进制分发。

上游图构建和检索算法归 DiskANN。团队本次新增的是环境探针、Job Object 启动与计量、同步 `ReadBackend` 验证适配、小数据评测与构建脚本。两处上游修正保存在 [本地补丁](../tools/environment/diskann-0.6.1.patch)：

1. 删除 `use_medoids_data_as_centroids` 中的重复 scratch 借用，避免单线程加载死锁。
2. 将 `SSDQueryScratch` 对单个 `new PQScratch` 的释放改为 `delete`。

没有修改图构建、邻居选择和搜索算法。上游 Windows 读取器不直接作为项目生产读取器使用；提交失败、句柄管理及异步生命周期由后续正式适配负责。

首次运行脚本需要从 GitHub、NuGet 和 Python 包源下载。离线使用时，预先保留已锁定的源码及子模块、`build/diskann/packages`、NuGet 工具和 Python wheel 缓存，按锁定版本安装；仅保留可执行文件不足以重建。阶段二验证了联网构建流程，尚未演练完全断网重建。
