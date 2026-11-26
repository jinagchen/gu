# 第七阶段 A：固定后台读配额

2026 年 11 月 26 日交付第一项独立能力：优化块读器可在同一个后端上调度需求读、预取和后台读，不依赖动态 Engine 或合并任务。范围是[冻结读调度契约](stage3-a-contract.md#3-资源预约模式和背压)的并发读取槽位；不是设备带宽、后台写入或 WAL 刷盘限速。

## 接入方式

- 在 `CreateOptimizedBlockReader` 的 `OptimizedReaderOptions::scheduling` 中显式传入冻结接口已有的 `ReadScheduling`，用 `ReadBatch` 和 `ReadPriority::background` 提交后台读。缺省不启用，现有静态 Engine 和同步基线行为不变。后端容量须至少与读器总在途上限一致。
- 后台和预取合计不得占用 `demand_reserve`，后台单独受 `max_background_reads` 限制。后台有已预约的待办或仍在途时，停止借用 `background_reserve`；已接管的需求读不被抢占。后台待办具备槽位时，需求读最多连续获得 `demand_burst_before_background` 次调度，之后让后台推进。
- 后台未命中使用 `workspace`，不驱逐或填充查询缓存。已有缓存块、相同键在途读仍可共享；不重复提交，也不把后台访问记作需求命中。后台批次数以 `max_background_reads` 为独立准入上限，各批的块、等待者和输出仍受既有内存预约约束；资源不足返回 `resource_exhausted`。
- 超时不向调用方交付半批。已提交的读仍占槽并持有文件与缓冲，直到完成回调；过期的排队读不再提交。同一维护任务不应在返回结果后无限保留块租约。
- `BlockReaderStats` 增加 `background_submitted`（后端接受的后台提交）、`queued_background_reads`（采样时仍有有效后台等待者的未提交唯一块）和 `peak_background_reads`（后台在途槽位峰值）。未接入 Engine 统计或 EVAL-1.1 工件。

## 验证

```powershell
cmake --build build/windows --config Release --target synchronous_storage_test
ctest --test-dir build/windows -C Release -R '^synchronous_storage$' --output-on-failure
```

可暂停的测试后端覆盖前台保留槽、后台上限、预取竞争、需求提交突发限制、空闲保留槽借用与归还、后台排队准入、超时保留在途槽、同键去重和短读后重试。实际 IOCP 文件测试额外核对后台读返回的字节。

Windows x64 / MSVC：在独立 LF 检出与 ASCII 临时路径下，Release 全量 CTest 通过 13/13，存储专项连续通过 10 次；本机 Debug 存储专项也通过。

## 待联调

第六阶段须先通过真实写入确认、重启重放及恢复后续写；B 再接入真实任务并共同验证跨代次查询、段和磁盘上限、写入背压及前台尾延迟。自动回收仍关闭，须等 Windows 清单发布和恢复故障验证通过，并满足旧引用释放与 WAL 覆盖条件。固定配额的组件测试不构成第七阶段整体验收。
