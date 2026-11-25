# STEP 零件前置样例与待验收测试（SP7）

本 PR 仅交付依赖就绪前的样例、测试脚手架和复现步骤，不宣称完成 7a/7b 数值验收。
下列断言和阈值保留；合入前置依赖后仍须另行运行并提交实测证据。

2026-11-18 复核，基线仍缺 SP4 的 `chipcae-import --assembly`：实际返回退出码 2，
`stderr` 为 `not implemented yet: --assembly`。因此当前不能完成装配导入、预览、求解，
没有可报告的 warpage、网格切割体积或圆柱误差。下列正常 CTest 的 XFAIL/skip
表示已知依赖未实现，不是数值验收通过。`SP7_STRICT_ACCEPTANCE=1` 会将其变成失败；
参数被实现后自动执行原有完整断言。7b 还依赖 SP6-B；S2 已存在，不作为跳过条件。

按 PR #108 裁定 `pr108-ci-ecad-mapeq-solvemsg-11201640`，导入命令退出码 7 且
stderr 去掉首尾空白后恰为 `IMPORT_REJECTED: ECAD support disabled` 时，也按依赖未就绪
返回 skip 77；严格模式仍失败，其他导入拒绝不跳过。合成板的 Homogenized 层将 `Si`
映射到 `silicon_eq`，材料卡复制生成目录中现有的 `silicon` 卡，仅改 id，参数不变。

## 构建与验收

在仓库根目录执行。box 上先将共享槽位包装器目录放在 PATH 最前，排队时等待即可。
按本地安装设置 `jsoncons_DIR`、`HDF5_DIR`、`Kokkos_DIR`；`SP7_PYTHON` 指向所需的 Python 环境，
缺省为 `python3`。真实案例需预先设置 `CHIPCAE_DEMO_STEP_DIR`，指向私有输入基准目录。
迁移本地配置时，另外设置四个文件变量（值只保留在本地，不入库）：

- `CHIPCAE_DEMO_BULK_STEP`：bulk STEP，例如 `/path/to/example_parts.step`。
- `CHIPCAE_DEMO_IHP_STEP`：第二个切片 STEP，例如 `/path/to/second_parts.step`。
- `CHIPCAE_DEMO_BOARD_IPC`：基板 IPC-2581，例如 `/path/to/board.ipc2581.xml`。
- `CHIPCAE_DEMO_BULK_BODIES`：body 映射 JSON，例如 `/path/to/bodies.json`。

C++ 与 Python 均直接使用绝对文件路径，相对文件路径则相对 `CHIPCAE_DEMO_STEP_DIR` 解析。
目录变量即使在文件使用绝对路径时也必须非空。映射 JSON 的键集合须与 bulk 模板完全相等，
值为非空且互不重复的 STEP body 键，例如：

```json
{
  "example_parts/die": "<local die body key>",
  "example_parts/adhesive": "<local adhesive body key>",
  "example_parts/tim": "<local tim body key>",
  "example_parts/lid": "<local lid body key>"
}
```

该 JSON 放在仓库外；仅运行目录里的 assembly 副本替换 body 键，仓库模板保持中性键。
真实 STEP 链接名固定为 `example_parts.step`，基板链接名为 `board.xml`。
运行真实 bulk 前还须将 `CHIPCAE_REAL_RUNS_DIR` 设为本地的 `/path/to/private-runs/`；
测试拒绝将真实运行目录放入 checkout。
Python 环境需有 cadquery-ocp、h5py、numpy、jsonschema、PyYAML、VTK。
本次 Release 构建启用了 JSON/HDF5，ECAD、C++ OCCT、MPI、MFEM 关闭；
即使 SP4 合入，完整数值验收仍需启用对应导入和求解后端。

```sh
export SP7_PYTHON="${SP7_PYTHON:-python3}"
export CHIPCAE_TEST_BUILD_DIR="${CHIPCAE_TEST_BUILD_DIR:-$PWD/build-sp7-release}"
cmake -S . -B "$CHIPCAE_TEST_BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCHIPCAE_ENABLE_JSONCONS=ON -Djsoncons_DIR="${jsoncons_DIR:?设置 jsoncons CMake 包目录}" \
  -DCHIPCAE_ENABLE_HDF5=ON -DHDF5_DIR="${HDF5_DIR:?设置 HDF5 CMake 包目录}" \
  -DKokkos_DIR="${Kokkos_DIR:?设置 Kokkos CMake 包目录}" \
  -DPython3_EXECUTABLE="$(command -v "$SP7_PYTHON")"
cmake --build "$CHIPCAE_TEST_BUILD_DIR" -j2
ctest --test-dir "$CHIPCAE_TEST_BUILD_DIR" --output-on-failure
SP7_STRICT_ACCEPTANCE=1 ctest --test-dir "$CHIPCAE_TEST_BUILD_DIR" -R '^e2e_' --output-on-failure
```

真实 bulk 按目录、BULK、BOARD、BODIES 的顺序检查上述变量，任一未设或为空时
打印 `SKIP: <变量名> is unset` 并退出 77；旧配置只设置目录时也会跳过，须迁移文件变量。
派生盖板只检查目录与 BULK。两个 C++ 真实切片测试分别使用 BULK 和 IHP，
目录或文件变量未设、为空，或路径不是普通文件时退出 77。
Python bulk 在变量齐全而文件不是普通文件时断言失败，诊断只写变量名；缺少 Python OCCT
也是错误。其他 CAD 用例无 Python OCCT 时跳过。供应商输入不入库，不打包到 CI 产物。
真实 bulk 在 `CHIPCAE_REAL_RUNS_DIR` 下创建独立的 `sp7-real-bulk-*` 目录并保留它。
求解完成后，目录根部的 `result.json` 记录 `data_class: "real"`、标为「实测」的
`warpage_pv`（m）、材料映射、「近似材料／近似几何」标记和铜层「整层平均」处理方式。
CLI stdout/stderr、Python 输出和失败详情写入同目录的 `run.log`；
真实用例禁用共享 solve helper 的 summary 打印，CTest 只报告状态和本地日志位置。
依赖未就绪时保留本地日志，不生成冒充实测的 `result.json`。
这些目录不得收集为 CI 产物，也不入仓库或演示结果表。

## 临时样例与单步命令

生成器与验收测试共用几何代码，输出目录必须位于仓库之外且为空。
下面可直接复现当前的 SP4 失败；SP4 及后端就绪后，同一命令继续执行预览、求解。
每次可将 `--case box` 改为 `ring-chamfer`、`cylinder`、`chamfer` 或 `negative`。
`rounded` 仅生成 C 级负例，分别导入 hat 与 ring，不能按正例继续预览或求解。
`bulk` 使用真实 substrate IPC 和 BULK STEP 的临时符号链接；其余正例使用现场生成的合成 IPC 板。
生成器的 `--demo-step-dir` 为相对路径基准，`--bulk-step`、`--board-ipc`、`--bulk-bodies`
分别覆盖对应环境变量；`chamfer` 只需前两项，`bulk` 四项均需设置。
缺少所需选项时生成器退出 2，并提示对应变量及选项。
未提供可用的 ODB++ 演示输入，此处只覆盖 IPC-2581。
真实 bulk 使用下面的专用 CTest 命令，以保留本地记录并避免将测量值输出到终端或 CTest：

```sh
: "${CHIPCAE_DEMO_STEP_DIR:?设置私有 STEP 所在目录}"
: "${CHIPCAE_DEMO_BULK_STEP:?设置 bulk STEP 路径}"
: "${CHIPCAE_DEMO_IHP_STEP:?设置第二个切片 STEP 路径}"
: "${CHIPCAE_DEMO_BOARD_IPC:?设置基板 IPC 路径}"
: "${CHIPCAE_DEMO_BULK_BODIES:?设置本地 body 映射 JSON 路径}"
: "${CHIPCAE_REAL_RUNS_DIR:?设置本地运行根目录}"
export CHIPCAE_DEMO_STEP_DIR CHIPCAE_REAL_RUNS_DIR
export CHIPCAE_DEMO_BULK_STEP CHIPCAE_DEMO_IHP_STEP CHIPCAE_DEMO_BOARD_IPC CHIPCAE_DEMO_BULK_BODIES
ctest --test-dir "$CHIPCAE_TEST_BUILD_DIR" -R '^e2e_real_bulk_parts$' --output-on-failure
```

```sh
(
  set -eu
  repo=$PWD
  build=${CHIPCAE_TEST_BUILD_DIR:?先执行上面的构建命令}
  fixture=$(mktemp -d "${TMPDIR:-/tmp}/sp7-demo-XXXXXX")
  trap 'rm -rf "$fixture"' EXIT
  "$SP7_PYTHON" "$repo/tools/samples/step_parts/make_parts.py" \
    --case box --out "$fixture"
  "$build/kernel/chipcae-import" --assembly "$fixture/assembly.json" \
    -o "$fixture/stack.json" --report "$fixture/report.json" --accept-b-tier
  "$SP7_PYTHON" "$repo/tools/stack/copper_fraction.py" \
    "$fixture/stack.json" -o "$fixture/solve.stack.json" --report "$fixture/cf.json"
  "$SP7_PYTHON" - "$fixture/case.json" <<'PY'
import json
import sys
from pathlib import Path
path = Path(sys.argv[1])
case = json.loads(path.read_text())
case['stack']['ref'] = 'solve.stack.json'
path.write_text(json.dumps(case))
PY
  "$build/preview/chipcae-preview" "$fixture/solve.stack.json" -o "$fixture/preview.vtkhdf" \
    --nx 28 --ny 22 --max-ratio 100
  "$build/apps/chipcae-solve/chipcae-solve" --case "$fixture/case.json" \
    --profile "$fixture/profile.json" --out "$fixture/result"
)
```

`ring-chamfer` 使用 nx=ny=2 与 4 比较；派生盖板用 112/88。
`cylinder` 使用 40×40 mm 板，nx=ny=8、16、32，levels=0。
上面的单步 preview 参数（含 `--levels`）也要与相应 case 一致。
五条正例 `e2e_` 测试定义数值比较及 C1 manifest/VTKHDF 校验；两条负例分别检查 C 级拒绝和
CUT_UNSUPPORTED。所有正例均经 CKPT-CF（#105），铜层按「整层平均」处理；该工具未合入时
测试标记依赖未就绪，不能删除 copper.polygons 绕过。当前尚未通过端到端数值验收。
`negative` 先导入不含零件的 assembly，然后用生成目录中的
`negative_material_layer.stack.json` 替换 `stack.json`，再运行 preview/solve；
两者的退出码均从 `protocol/errors.yaml` 的 `CUT_UNSUPPORTED` 读取；preview 的 stderr
仍须包含该错误名和 `polygon in material layer`。上述裁定只将 solve 的 stderr 断言改为
`internal material cuts are not supported`，与现有 case loader 的文案一致。
solve 补齐错误名和原因属 M3 后续产品工作，本 PR 不改产品代码。
该输入故意在 silicon 层中放置旋转 copper region；copper 卡尚未入库，不能捏造参数。

`box_parts_regions.stack.json` 是独立手写基准；STEP 和 regions 使用相同 case。
圆角帽盖和旋转 15° 的有孔环是 v1.0 的 C 级负例：分别要求 PART_NOT_PRISMATIC，
details 前缀分别为 `cut_unsupported: multiple distinct cut sections` 和
`cut_unsupported: holed non-rectilinear section`。正例由轴对齐方环和凸八边形盖板承担。

圆柱半径 10 mm、高 1 mm。按 2026-11-19 23:55 裁定，不再拟合收敛阶：平面切割对离散多边形
应当精确，圆柱误差来自弦离散。测试用导入 stack 中的顶点按鞋带公式计算 A_poly，从本次
`import_report.json` 读取 tau_A；契约 §3 / 续七-10 规定实际 SliceOptions.chord_tol = tau_A。
每档要求 |V_h − A_poly·h|/(A_poly·h) ≤1e-10，任意两档间 |V_a − V_b|/V_a ≤1e-12，
圆柱解析体积相对误差 ≤1.1×4×chord_tol/(3R)。levels=0、min_elems_per_layer=1，
cell_budget 保持默认 50000。测试逐档输出 V_h、A_poly·h、两项误差、chord_tol、上界及偏大/偏小，
并输出全部两两网格差异；依赖就绪后将这些「实测」数据写入后续 PR。

## SP7 测试读取说明

以下说明仅记录本卡测试对现有导入和求解输出的使用方式，不定义新的公开或版本化契约。
bulk 测试解码报告 JSON 后检查五个零件的 assembly id 和「近似材料」标记；
派生盖板测试通过完整生成器生成输入，并在 `fixture-notes.json` 的 `labels` 中检查完整来源标签。
导入报告的 JSON 键名、嵌套和诊断顺序不作假定，
零件覆盖还通过 stack 中的 `region.body` 与材料映射独立验证。

体积测试在 C1 校验后读取 `io/src/vtkhdf_results.cpp` 生成的正常求解结果，
不使用 `cut_as_hex` 输出。`FieldData/material_names` 按 UTF-8 解码，
`CellData/material_id` 是其一基索引。完整轴对齐 hex 的体积由八个顶点的三维坐标跨度相乘，
按分区分别推进点与单元偏移，Connectivity 使用各分区局部点索引。
cut 使用 `CellData/volume` 中以 m³ 表示的保留体积，按材料筛选后与 hex 体积相加；
空 cut 块可以没有 volume 数据集，非空块缺少该数据集仍报错。
这些读取假设只用于本卡的求解器输出，不适用于任意 VTK 文件。

## 材料与近似

`bulk_parts.assembly.json` 的 die 用 silicon，adhesive 与 tim 用 underfill，
lid 当前用 silicon，四者均标 `material_approx=true`，均为「近似材料」。
lid：刚度近似，CTE 偏低，待 copper 入库后重跑。生成器在已发布材料库出现 copper 后
自动选择 copper；静态 bulk 模板映射届时也需更新并重跑。
不自行编材料参数。为与测试一致，临时材料库将已发布温度表固定在首个温度点。

真实 STEP 用 frame=step，origin=[0, -0.044, 0.000020002] m。
`interposer_approx` 为「近似材料、近似几何」，underfill，STEP 坐标 x[2,54]、y[2,34] mm，
z 从基板顶到 die 底。assembly/0.1 没有内联 region 字段，因此用临时 box STEP 表达同一区域，
随同 origin 平移；标签保存在 `fixture-notes.json` 和本文，不扩展 schema。
导入报告中的近似材料标记由 bulk 验收断言检查，当前被 SP4 阻塞。

派生盖板标记为「派生件（由真实盖板四角倒角，非原始设计）」。
生成器从真实 BULK STEP 中识别唯一尺寸匹配的盖板，四角作 C=2 mm 的 45° 倒角，
并生成 `lid_chamfer_derived.step`。原始设计不修改。标签保存在 `fixture-notes.json`，
测试检查该文件；当前 assembly/0.1 没有此标签字段，导入器也不读取 notes，不能要求导入报告携带该标签。

## 尚未完成的验收证据

本 PR 不提供验收结果表；CAD 体积不能代替网格切割或求解证据。
以下项目均未完成，不能因 CTest 的依赖跳过而记为通过：

- 真实 bulk（近似材料）：五个 body 的导入覆盖、材料覆盖与实测 warpage。
- box：STEP 与手写 regions 的 warpage 相对误差 ≤1e-10。
- 方环加强环＋八边形倒角盖板：h 与 h/2 的实测 warpage 变化 ≤2%，cut 单元数 >0。
- 圆角帽盖和旋转有孔环：分别返回 C 级 PART_NOT_PRISMATIC 及规定的原因。
- 派生件（由真实盖板四角倒角，非原始设计）：切割体积相对解析值误差 ≤1e-9。
- 圆柱：三档实测多边形体积精度、跨网格一致性和圆柱弦误差上界。
- 负例：preview 和 solve 均返回 CUT_UNSUPPORTED 退出码；preview 检查错误名及
  `polygon in material layer`，solve 按上述裁定检查 `internal material cuts are not supported`。

依赖就绪后运行严格模式，只有完整正例及负例均执行通过后，才能在后续 PR/docs
填写标为「实测」的 warpage、比较误差、双网格变化、切割体积和弦误差上界检查。
