# D03 结果读取 API

基线：D01 `27beca9`（已 rebase 到含 schema B #41、preview #37、VTKHDF writer
#35、Desktop CI #45 的 origin/main），再合入 D02 `1bd44f1`。本卡不执行 merge/push。

链接 `chipcae_desktop_results`，包含 `ResultSession.hpp`，使用
`chipcae::desktop::ResultSession`：

```cpp
ResultSession session;
if (session.open(runDirectory)) {
    auto data = session.loadStep(0, "T");
    // data 是拥有引用的 VTK 原始数据；错误查看 error()，等待/兼容查看 warnings()。
}
```

`open` 接受 run 目录、manifest JSON，或 VTKHDF 文件及可选 manifest/event log 路径。
无 sidecar 的 standalone 文件提供 n=0/time=0；其他 temporal 时刻通过 sidecar steps 选择。
`steps/fields/blocks/summary/meshDigest` 返回 Qt JSON 值，保留单位、dtype、partitions
等原 manifest 信息（完整内容由 `manifest()` 提供）。块名和 summary 兼容名在访问器中规范化。
`nameTables()` 的键是 HDF dataset 路径，值为名字列表。`layerNames()` / `materialNames()` 给出规范表，`layerName(id)` / `materialName(id)` 按 1-based id 映射（索引 i 对应 id i+1）。名字表用 HDF5 直接读取 `FieldData/layer_names` 与 `material_names`，同时支持定长 NULL 填充 UTF-8（预览/D02 样例）和 H5T_VARIABLE（O1b / s1b）。vtkHDFReader 不会把这些数组放到分区 FieldData 上；`loadStep` 会把规范表写回返回对象及其分区，便于视图使用。
原始 cell_kind=2 数据保留，视图应按 `hiddenCellKind` 默认隐藏。

这是同步、无线程归属 UI 的库；大文件应在调用者 worker 线程读取，每个线程独占 session。
每次 load 新建 VTK reader，避免文件切换缓存旧几何/字段。返回对象持有自己的引用。
没有实现本卡之外的视图、图表或运行监控 UI。

错误分层：

- `open/loadStep` 返回 false/null 且 `error()` 非空：manifest 错误、越界路径、checksum
  不匹配、版本拒绝、负 PolyhedronToFaces、HDF/VTK 读取失败。
- null 且 `error()` 为空：step 未 complete 或文件尚未发布，warnings 说明等待。
- warnings：可读的 2.9/未来 minor、2.0 单 UG、旧块名、旧分类字段、旧 summary/manifest
  字段；U_mag_max 是正式键，优先于同一步的 U_abs_max，不产生 legacy 告警。
- `readEventLog` 返回已解码前缀、validatedBytes 和独立 error。CRC/截断不会被当作完整成功；
  session 自动读取 events.caelog 时将该 error 同时放入 warnings，结果网格仍可用。
  format≠1 明确拒绝；复用仓库只读 CAELOG1 payload 解码，不修改 core io。

CTest fixture 在 build 目录生成，不提交二进制。测试使用 `/workspace/samples/s1` 的真实输出，
部署到其他机器时需提供该验收数据。测试 Python 应具备 h5py/numpy/protobuf。
