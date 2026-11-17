# schema

从参数声明导出的 Schema 快照，供桌面端和脚本使用。求解器参数稳定后，由构建过程更新本目录中的文件。

- `case-1.0.schema.json` 定义 `chipcae.case/1.0`；JSON/YAML 示例与负例位于 `examples/`。
- `run-profile-1.0.schema.json` 定义独立保存的运行 profile。

## 网格参数

`mesh.nx`、`mesh.ny` 是面内初始列数，缺省时由网格根据叠层自动确定；`max_ratio` 缺省为 2，`cell_budget` 缺省为 50000。GUI 在提交求解前，应将预览解析出的 `levels` 作为显式值（至少为 1）写入并保存 case。`levels: 0` 表示自动选层，只供命令行使用；自动层数 L 依赖运行 profile 中的 MPI ranks，因此不能作为 GUI case 的可移植设置。

求解内核应在 manifest 的 `mesh` 摘要中记录 `nx0`、`ny0`、`L`、`padded_cells`、`nz` 和 `mesh_sha256`。摘要哈希覆盖节点坐标和单元连接。`chipcae-preview` 应输出同一摘要（JSON 或 VTKHDF 属性）；GUI 在提交前比对预览与运行摘要，不一致时提示用户。
