# 第六阶段 A：WAL 底层

2026 年 11 月 23 日实现冻结的 [FORMAT-1](storage-protocol.md#3-wal-记录-format-1)，保留 [STORAGE-A-1.1](stage3-a-contract.md#4-wal-编码与提交交接) 的接口和提交边界。`agent_io_storage` 已提供可链接的 [编解码](../src/storage/wal_codec.cpp)和 [Windows writer](../src/storage/wal_writer.cpp)。

## B 侧接入

- 先持有实例锁、完成恢复检查并确保 `wal/` 已存在，再传入该目录、实例 UUID、下一序号和维度。文件以 `CREATE_NEW` 独占创建，头写完整且刷盘后才返回 writer。文件名末尾是每文件独立 UUID，实例身份要读文件头；不得用文件 UUID 判定新旧或实例身份。
- 调用方串行使用 writer，并自行预约编解码缓冲及提交组的空间。`Append` 只接受单个完整帧，验证维度、CRC、填充及连续序号。不在 `Append` 内刷盘或自动轮换；超过当前文件上限的帧在写入前拒绝。B 须在组开始前核算容量，不能依赖中途拒绝来拆组。
- 一组 `Append` 后显式 `Sync`，成功且发布可见序号后才确认写入。成功同步并发布的组之间才能 `Rotate`，新头使用相同实例 UUID 和下一条序号。`Close` 幂等，返回已记录的首个错误，不代替 `Sync`。
- 尝试系统写入后失败或待提交组刷盘失败时返回 `may_have_applied`，本 writer 不再写入或轮换；尚未开始写入的新请求返回 `none`。B 保留受影响的组并进入 `recovery_required`。失败文件不由 A 擅自删除。
- 解码遇到合法但不完整的帧返回 `need_more`，完整损坏帧返回 `corruption`。文件头解码只接受恰好 64 字节；B 扫描时自行区分最新文件的实际 EOF 和历史文件缺尾，负责重放、尾部处理、实例身份及跨文件序号检查。

## 验证

```powershell
cmake --build build/windows --config Release --target wal_test
ctest --test-dir build/windows -C Release -R '^wal$' --output-on-failure
```

固定字节样例独立核对文件头、浮点小端编码和 CRC32C。用例检查帧的每种截断长度、损坏或超限输入、追加顺序、组边界轮换、短写续写、写入与刷盘故障，以及停止写入后保留原错误。进程故障用例在本机用真实文件写入一条已同步记录和半条后续记录，强制终止子进程后检查完整记录和截断尾部。该用例不等同于 Engine 恢复，也不证明断电持久性。

本机 MSVC x64 的 CTest 全量回归在 Release 和 Debug 各通过 12/12 项。复现时使用 LF 检出的 ASCII 源码路径，并将测试进程的 `TEMP` 和 `TMP` 设为已存在的 ASCII 目录，以满足现有固定校验和及索引首建夹具的路径约束。

仍待联调：Engine `Insert`/`Upsert` 的确认后可见、重启重放、重复恢复、恢复后续写，以及混合负载下的写入计数、资源上限与查询尾延迟。旧日志删除和 checkpoint 不由底层 writer 执行。
