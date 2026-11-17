# schema

从参数声明导出的 Schema 快照，供桌面端和脚本使用。求解器参数稳定后，由构建过程更新本目录中的文件。

- `case-1.0.schema.json` 定义 `chipcae.case/1.0`；JSON/YAML 示例与负例位于 `examples/`。
- `run-profile-1.0.schema.json` 定义独立保存的运行 profile。

## 网格参数

`mesh.nx`、`mesh.ny` 是面内初始列数，缺省时由网格根据叠层自动确定；`max_ratio` 缺省为 2，`cell_budget` 缺省为 50000。GUI 在提交求解前，应将预览解析出的 `levels` 作为显式值（至少为 1）写入并保存 case。`levels: 0` 表示自动选层，只供命令行使用；自动层数 L 依赖运行 profile 中的 MPI ranks，因此不能作为 GUI case 的可移植设置。

求解内核应在网格生成后、求解前将摘要写入 manifest 的 `mesh` 字段。除 `mesh_sha256` 外，摘要字段（包括 `nx0`、`ny0`、`L`、`padded_cells`、`nz` 及节点数、单元数）均为整数。`mesh_sha256` 是下述字节序列的 SHA-256，编码为 64 位小写十六进制：先写入 int64 little-endian 头部 `[nx0, ny0, L, nz, 节点数, 单元数]`；随后按全局节点号升序写入全部节点坐标，每个点按 xyz 顺序编码为 float64 little-endian；最后按 `elem_gid` 升序写入全部单元连接，每个连接号编码为 int64 little-endian 全局节点号。哈希与 MPI rank 数无关，内核可由 rank 0 汇总，或分块后按全局序拼接。存在 cut 时仅覆盖背景 hex 网格，cut 一致性以后通过新增字段记录。

`chipcae-preview` 应输出同一摘要（JSON 或 VTKHDF 属性）。内核在网格生成后、求解前把摘要写入 manifest；GUI 在摘要出现时将其与预览比对，若不一致则提示用户并允许取消。

## assembly/0.1

`assembly-0.1.schema.json` 定义 `chipcae.assembly/0.1`（Draft 2020-12）。
长度单位为 m，角度单位为度；所有路径相对 assembly 文件，禁止绝对路径和 `..`。
必填字段为 `schema`、`board`、`parts`，允许空 `parts`。
零件放置可选 attach（`layer:<名称>@top|bottom` 或 `part:<id>@top|bottom`）
或绝对 STEP frame（`frame: "step"` 与三维 `origin`），两种写法互斥。
`parts_from_step` 用 body 名映射到 id、material，支持可选布尔 `material_approx`。

示例为 `examples/assembly/fcbga_lid_stiffener.assembly.json` 和
`examples/assembly/package_bulk_parts.assembly.json`，仅引用文件名，不包含 STEP 数据。
`tests/contract/validate_assembly.py` 校验正例及七个负例，并在结构校验后检查
重复 part id（指向后出现的 id）及 bond/gap：无 bond 时 gap 可省略或非负；
有 bond 时 gap 可省略，否则必须与 thickness 按解析后的数值完全相等，不使用容差。
缺少 jsonschema 或 referencing 时测试退出 77（SKIP）。
