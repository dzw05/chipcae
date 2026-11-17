# D02 合成结果契约

本目录为确定性 mock，非工程求解结果。运行：

```sh
python3 desktop/tools/make_samples.py --out build-desktop/samples
python3 -m unittest discover -s desktop/tests -p test_samples.py -v
python3 desktop/tools/mock_solver.py --run-dir build-desktop/samples/default --delay 0.2
```

需要 numpy/h5py/protobuf。系统受 PEP 668 管理时可在 /tmp 创建 venv，
将其 bin 加入 PATH；不用修改已有 proto-vtkhdf venv。测试自动使用已有
`/workspace/proto-vtkhdf/venv/bin/python` 做独立 VTK 读回。
生成二进制只放 build-desktop/samples（根已有 build-* ignore），不提交。
重复运行覆盖同名合成产物；不同运行不要共用输出目录。

## PR #35 / S1 键名对照

|键|本样例|
|---|---|
|schema_version|chipcae.results/1.0|
|units / stackup|SI / stackup.json（复制仓库 demo stack，非此合成几何的工程模型）|
|steps[]|n、time、file；附 complete 与 SHA-256 checksum|
|fields[]|name、location、components、dtype、unit；sigma_vm reserved=true|
|partitions|per_step 分区数，solver_fixed_per_run；适用于每个块|
|summary[]|name、value、unit、step、method|
|summary 名称|warpage_pv(m)、T_max(K)、U_mag_max(m)|

不采用 step/association/ncomp/U_abs_max 作为正式键。generation 随已登记
step 递增，complete 表示全部三步发布。数据关闭后同目录 `.tmp` rename，
最后原子替换 manifest；无 fsync 持久性承诺。读取器以 manifest 登记为准，
不能以 OutputAvailable 为结果到达依据。

## 样例集合

- default：三步 PDC，每块固定三分区，最后一分区为空，particles 整块为空。
  Version=[2,8]；Index=0..3：hex/cut/eb_surface/particles。
- hex_fallback：cut 为 VTK_HEXAHEDRON(12)，cell_kind=1；无多面体数组。
- old_names：独立 layer/material int32 旧名样例。
- s1_compat：Version=[2,9]，cut_poly/embedded_boundary 旧块名，无 partitions；
  面号均非负，仅模拟 S1 容器兼容情况，不声称复现 S1 数值。供 D03 告警兼容。
- variable_parts：各块分区数 3→2→2，独立兼容测试，非主求解器布局。
- reject_negative.vtkhdf：只用于隔离子进程前置拒绝，禁止交给 VTK。
- temporal_ug.vtkhdf：standalone UG，三时间步，各两分区、各两相邻立方体。
- events：确定性 CAELOG1 有效、半记录、CRC 错误 fixtures。
  default 另含完整 mock 运行日志；STOP 可向 control.jsonl 追加命令。

拓扑/计数 int64，Types uint8，分类 int32；elem_gid int64 含 2^53+17。
layer_id/material_id 从 1 起；FieldData 名称表按 demo stack 首次出现顺序提供，
不能把 id 当数组下标而漏减 1。第二非空分区 cell_kind=2，界面默认隐藏。
空 UG Offsets、FaceOffsets、PolyhedronOffsets 保留起始零；PolyData 四拓扑组齐全。

cut 是父立方体 x≤0.625 的解析裁剪几何，每单元六面独立朝外；真体积
0.625*(1+0.1*n)^3。测试逐面外向、反向有向边配对、逐单元散度体积，
不使用 vtkCellSizeFilter 认证。

T 为绝对温度 K；父坐标 x/y/z 中解析场：
T=193.15+7*n+3*x²+2*y*z+z²，
U=(1e-4*x*z,2e-4*y*z,(n+1)*1e-3*(x²+2*y²)) m。
实际从 27 个 Q2 DOF 张量插值取 hex/cut 顶点，独立测试用解析值核对。
几何随步缩放并 z 平移，场使用父坐标。summary 基于完整解析 Q2 场：
warpage_pv 在顶面去最小二乘平面后求 PV，不从导出顶点反推；
T_max/U_mag_max 为完整父单元最大值。对称顶面的 Q2 节点与稠密网格
去平面得到相同斜率和 PV；独立稠密网格测试验证。

## 时间与 reader 注意事项

主 PDC 是 manifest 多文件时间序列，可 0→1→2→0 回切。
**不宣称验证了 PDC Steps。** standalone UG 已用 vtkHDFReader 验证时间回切。
UG 偏移为：PartOffsets=[0,2,4]、PointOffsets=[0,24,48]、CellOffsets=[0,4,8]、
ConnectivityIdOffsets=[0,32,64]、FaceConnectivityOffsets=[0,96,192]、
FaceOffsetsOffsets/PolyhedronToFaceIdOffsets=[0,24,48]。
FaceOffsets 每分区起始零由 reader 的 PartOffset 加入，不能双计。

本机 VTK 9.7.2 的默认 cache 在同尺寸 PDC 文件间 SetFileName/Update
会保留旧几何及 T；关闭 cache 可更新，但该开关已弃用。本测试在切文件时
重建 reader，保留默认 cache，验证数值及几何回切。PDC→UG 切换复用 reader
还触发过崩溃；重建 reader 后通过。D03 应采用 reader 重建或独立 cache 隔离，
不要仅凭分区计数断言已切步。

## 本地验收记录（2026-11-17）

使用 `/tmp/d02-python/bin` 加入 PATH（Python 3.13，h5py/numpy/protobuf），实际运行：

```sh
python3 desktop/tools/make_samples.py --out build-desktop/samples
python3 -m unittest discover -s desktop/tests -p test_samples.py -v
python3 cmake/check_license_policy.py .
python3 cmake/check_protocol_compat.py .
```

全部通过，9 项 unittest；其中 VTK 子进程使用现有只读 venv 的 VTK 9.7.2。
无新增 C++，无需 clang-format；未做 Qt/C++ SDK 或 CTest 集成。
未交付：B3 writer golden、真实 C3 联验、PDC Steps。mock JSONL 额外事件
不表示当前 C++ event_log.hpp 已具备对应 payload。
