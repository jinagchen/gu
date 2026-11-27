# 阶段七 B 侧复现环境

| 类别 | 值 |
| --- | --- |
| 操作系统 | Microsoft Windows 11 家庭版 中文版 10.0.26300 build 26300 |
| CPU | Intel(R) Core(TM) i9-14900HX，24 核 / 32 逻辑核 |
| 内存 | 15.7 GB |
| 磁盘 | KPEGYMTBW2U001TCJD，SSD，NTFS |
| 编译器 | MSVC 14.44.35207（D:/Visual studio/Visual Studio 2022） |
| Windows SDK | 10.0.26100.0（D:/Windows Kits/10） |
| CMake | 4.4.4 |
| 生成器 | Ninja（VS 自带 2022/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja） |
| 构建类型 | Release |
| 编译选项 | `/W4 /WX /permissive-`，C++20 |

## 构建与运行

```bash
# 配置（本机已存在 build/release，等价命令）
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="D:/Visual studio/Visual Studio 2022/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe" \
  -DCMAKE_RC_COMPILER="D:/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe" \
  -DCMAKE_MT="D:/Windows Kits/10/bin/10.0.26100.0/x64/mt.exe" \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY

# 构建（需要 MSVC 环境变量 INCLUDE/LIB/PATH）
ninja -C build/release

# 阶段七 B 侧用例
build/release/maintenance_test.exe

# 全量回归
ctest --test-dir build/release --output-on-failure
```

## 本次构型的注册测试

`AGENT_IO_TEST_PYTHON` 未在本构型设置，因此 4 个 Python 契约测试不在 `ctest` 里注册（`CMakeLists.txt:144`）。
它们已单独执行，结果见 `python-contracts.log`。

```bash
python tools/contracts/check-stage3-b.py
python tools/contracts/test-stage3-b.py
python tools/contracts/test-stage3-freeze.py
python tools/storage/test-verify-read-path.py
```
