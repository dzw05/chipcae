# Trace mapping correctness acceptance (TEST-FIRST)

这些合成夹具记录 **各向同性 trace mapping 近似** 的现状；本卡不实现方向性材料、
实体内铜裁剪或合并后的方向性传递。strict xfail 是解析目标尚未满足的失败验收，
并非跳过测试。修复使目标命中时，XPASS 会令 CTest 报红，届时应转成普通断言。

## 源码基线

2026-11-20 初次核实的 main 为 `e8e86ceb9a4aacb45c806c0602a2a90474f54297`；
交付复核时 `git ls-remote` 和 `git fetch --no-tags origin main` 核实 main 已前进到
`4a871e395b929755d2a532fde2bba794c27e2b59`。#204 的远端 head 和当前依赖基线仍为
`995424f5f56c6e90f538fe64ac2ca35c068682bf`。该依赖不是新 main 的祖先，且新 main
仍缺少 B 组需要的热 FVM grid/thermal 源码；B 组继续在依赖分支源码上运行。

`rasterize_copper` 实际返回 Morton dense 序，测试用 `level0_dense` 读取；
其头文件中的 row-major 注释与实现不符。当前基线 FEM pipeline 使用层常量 fraction，
与 brief 引用的逐列 FEM 路径不同；本卡不测 FEM。FVM 切割仍使用
`min(1, phi/sub_volfrac)`，合并后传递体积加权标量 fraction。

## 独立参考与夹具

F1 是 `[0,1 mm]²` 上沿 x 的十条同向矩形：第 k 条占
`y=[20+100k,70+100k] µm`，宽 50 µm、节距 100 µm。F1ᵀ 交换 x/y。
铜面积为 `5e-7 m²`。观察值的 phi 全部来自公开 rasterizer，张量全部调用
`copper_laminate_conductivity(0.3,390,f,k)`；不替代产品实现。

独立参考以 long double 区间交长计算 phi_ref。现行预测独立计算
`k_j=phi_ref,j*390+(1-phi_ref,j)*0.3`，不读取观测 phi、不调用混合 helper。
对顺变化方向累加 `H/sum(h_j/k_j)`，垂直变化方向累加 `sum(h_j*k_j)/H`。
解析目标独立计算 `par=(390+0.3)/2` 和 `ser=1/(0.5/390+0.5/0.3)`。

B 组平板用 boxes 直接给 x/y 各四等分、z 为 `[0,35 µm]`；先从 hex 顶点
识别列并核对 `elem_gid/nz == level0_dense(4,4,i,j)`。沿 y 两端分别设 1/0 K，
求解 rtol 为 `1e-14`。`k_eff=abs(heat_in)*L_y/A_y` 与 k_used.yy 行内算术、
行间调和平均比较。另一独立解析 phi 预测用于 strict xfail。

切割用 const `build_cut_layer` 保留单位立方体内 `x+y<=1`，先核对唯一子单元
体积分数为 0.5。实体内横带面积闭式为 `[y-y²/2]_a^b`；完全在实体内的矩形
直接乘长宽。铜横带越出实体时，目标只计实体内交面积；饱和案例还核对铜体积。

合并夹具是相邻两行等高盒：A 完整且全铜，B 仅保留贴着 A 的底部 0.1h 且无铜。
先不标 small_subs 得到两个 CV，再标记重建得到一个 CV；检查合并前后 sum(fV)、
体积为 1.1、f=1/1.1（均为绝对容差 1e-12），以及 kxx 的体积加权并联目标。kyy 的目标是
`1.1/(1/390+0.1/0.3)`，现行预测独立来自解析体积加权 fraction。

## Case 与容差

共 24 个 CTest case：8 个普通断言、16 个 strict xfail。表内的 N 每个均独立登记。

| Case | 夹具/分辨率 | 判定 |
| --- | --- | --- |
| tracemap_along_x_parallel | F1，ny=1,4,10,20,40,80,160,320,640 | kxx=par，铜面积守恒 |
| tracemap_along_y_parallel | F1ᵀ，同一 nx 序列 | kyy=par，铜面积守恒 |
| tracemap_perp_x_xfail_nyN | F1，N=1,4,20,40 | kyy=ser，strict xfail |
| tracemap_perp_y_xfail_nxN | F1ᵀ，N=1,4,20,40 | kxx=ser，strict xfail |
| tracemap_aligned_exact | F1，ny=100 | phi 为 0/1，kxx=par、kyy=ser |
| tracemap_refine_xfail_nyN | F1，N=80,160,320,640 | kyy=ser，strict xfail，目标相对 1e-3 |
| tracemap_refine_order | 上述加密序列 | 误差严格下降，三个误差比均在 [1.8,2.6] |
| tracemap_empty_and_duplicate | F1 网格，ny=40 | 空输入 km；重复图形 NonZero 并集不变 |
| tracemap_fvm_slab_harmonic | 4×4×1 平板 | 求解与 k_used 调和平均一致，相对 1e-8 |
| tracemap_fvm_slab_series_xfail | 同一平板 | k_eff=ser，strict xfail |
| tracemap_cut_inside_normalized | 实体内矩形 [0,0.5]×[0,0.2] | phi=0.1，f=0.2 |
| tracemap_cut_outside_xfail | 横带 [0.4,0.6] | f=交面积/实体面积，strict xfail |
| tracemap_cut_conservation_xfail | 横带 [0,0.6] | fV=交面积×厚度，strict xfail |
| tracemap_merge_conservation | A+B 合并 | 铜量守恒，相对 1e-14；f=1/1.1 |
| tracemap_merge_directional_xfail | 同一合并 | kxx 并联普通断言，kyy 串联 strict xfail |

未另注明的普通导热断言相对容差为 `1e-12`；phi/f 的绝对容差为 `1e-12`；
空输入导热为相对 `1e-15`。xfail 解析目标默认相对 `1e-6`，现行预测相对 `1e-9`。
brief 中提及 C 组容差但没有定义 C 组夹具或验收 case，因此不增加 C 组。

每个 xfail 只调用一次共享 `xfail_check`：目标命中返回 1/XPASS；目标不满足且
吻合现行预测返回 0/XFAIL；其他情况（包括非有限值）返回 2/XFAIL-WRONG-REASON。
CTest 要求唯一 `XFAIL <完整 case 名> ` 行，并拒绝 XPASS、WRONG-REASON 或 ERROR。
不用 WILL_FAIL。夹具自检或运行异常返回 3/ERROR；未知 case 返回 4 并列出可用名称。

## 运行

保留构建环境的共享槽 shim 在 PATH 首位。可用依赖通过标准 CMake package/source
参数指定，不把机器路径写入测试。

```sh
cmake -S . -B build-tracemap -DCMAKE_BUILD_TYPE=Release -DCHIPCAE_ENABLE_ECAD=ON
# B 组另需本机启用 PETSc 的 MFEM package：
cmake -S . -B build-tracemap -DCHIPCAE_ENABLE_MFEM=ON -DCHIPCAE_ENABLE_MPI=ON \
  -DMFEM_DIR="$MFEM_DIR"
cmake --build build-tracemap --parallel 2
ctest --test-dir build-tracemap -R '^tracemap_' -V
ctest --test-dir build-tracemap --output-on-failure
```

仅 ECAD 登记 A 组 17 项；同时开启 MFEM 且 MFEM_USE_PETSC 时再登记 B 组 7 项。
不启用 ECAD 时不登记这些测试。A 标签 tracemap/tierA、超时 60 s；
B 标签 tracemap/fvm/numerical、超时 600 s；所有预期失败另加 xfail 标签。

## 验证结果

2026-11-20 Linux x86_64 / GCC 14.2 / Release / warnings-as-errors **实测**：
`ctest -R '^tracemap_' -V` 为 **24/24 通过**，其中 8 个普通 PASS、16 个 XFAIL；
没有 XPASS、WRONG-REASON、ERROR 或跳过。FVM 使用本机 MFEM 提供的 PETSc。

配置登记矩阵（实测，只检查登记）为 ECAD OFF: 0、仅 ECAD: 17、
ECAD + MFEM/PETSc: 24；标签、60/600 s 超时和 strict 输出正则均已检查。
两可执行文件的缺参、未知名称、错误选项和多余参数均返回 4 并列出可用 case。
隔离 checker harness 验证 12 个分支/边界/非有限输入；其 CTest 驱动接受 XFAIL，
并拒绝 XPASS、WRONG-REASON、非有限值。额外的外部目标检查驱动仅接受解析目标，
复用未改动的实际可执行文件，16 个缺失功能 case 均报告失败；仓库登记仍按 strict
xfail 协议记录现状。

下列为全部 case 的实测输出（保留每条 XFAIL 的 observed、target、prediction 和 ratio）：

```text
PASS tracemap_along_x_parallel
PASS tracemap_aligned_exact
PASS tracemap_along_y_parallel
PASS tracemap_empty_and_duplicate
ny=80 relative_error=0.33191271819255252
ny=160 relative_error=0.14207530694066128
ny=320 relative_error=0.066439174336712314
ny=640 relative_error=0.032098521287018844
PASS tracemap_refine_order
XFAIL tracemap_perp_x_xfail_ny1 observed=195.15000000000001 target=0.59953881629515759 current_prediction=195.15000000000001 ratio=325.50019230769232
XFAIL tracemap_perp_y_xfail_nx1 observed=195.15000000000001 target=0.59953881629515759 current_prediction=195.15000000000001 ratio=325.50019230769232
XFAIL tracemap_perp_x_xfail_ny4 observed=194.83871926210608 target=0.59953881629515759 current_prediction=194.83871926210605 ratio=324.98099200000001
XFAIL tracemap_perp_y_xfail_nx4 observed=194.83871926210608 target=0.59953881629515759 current_prediction=194.83871926210605 ratio=324.98099200000001
XFAIL tracemap_perp_x_xfail_ny20 observed=187.3679815526518 target=0.59953881629515759 current_prediction=187.3679815526518 ratio=312.52018461538461
XFAIL tracemap_perp_y_xfail_nx20 observed=187.3679815526518 target=0.59953881629515759 current_prediction=187.3679815526518 ratio=312.52018461538461
XFAIL tracemap_perp_x_xfail_ny40 observed=1.193359028389809 target=0.59953881629515759 current_prediction=1.193359028389809 ratio=1.9904616614553095
XFAIL tracemap_perp_y_xfail_nx40 observed=1.193359028389809 target=0.59953881629515759 current_prediction=1.193359028389809 ratio=1.9904616614553095
XFAIL tracemap_refine_xfail_ny80 observed=0.79853337447362871 target=0.59953881629515759 current_prediction=0.79853337447362871 ratio=1.3319127181925525
XFAIL tracemap_refine_xfail_ny160 observed=0.68471847764313287 target=0.59953881629515759 current_prediction=0.68471847764313287 ratio=1.1420753069406613
XFAIL tracemap_refine_xfail_ny640 observed=0.6187831257524018 target=0.59953881629515759 current_prediction=0.61878312575240191 ratio=1.0320985212870188
XFAIL tracemap_refine_xfail_ny320 observed=0.6393716802326177 target=0.59953881629515759 current_prediction=0.63937168023261781 ratio=1.0664391743367123
slab effective=194.83871926210605 harmonic=194.83871926210608
PASS tracemap_fvm_slab_harmonic
slab effective=194.83871926210605 harmonic=194.83871926210608
XFAIL tracemap_fvm_slab_series_xfail observed=194.83871926210605 target=0.59953881629515759 current_prediction=194.83871926210605 ratio=324.98099199999996
PASS tracemap_cut_inside_normalized
XFAIL tracemap_cut_outside_xfail observed=0.39999999999999991 target=0.20000000000000001 current_prediction=0.40000000000000002 ratio=1.9999999999999996
XFAIL tracemap_cut_conservation_xfail observed=0.5 target=0.41999999999999998 current_prediction=0.5 ratio=1.1904761904761905
PASS tracemap_merge_conservation
XFAIL tracemap_merge_directional_xfail observed=354.57272727272726 target=3.2748091603053435 current_prediction=354.57272727272726 ratio=108.27279084551812
```

完整既有回归（同一 Release，另启用 JSON/HDF5/MPI）首轮实测：
355 项中 332 PASS、13 个既有 SKIP、10 FAIL，CTest 返回 8，耗时 2360.76 s；
**尚未达到全量 CTest 全绿**。`copper_map_analytic` 和三个既有 FVM serial/cut/np2
回归均通过。失败均位于未改动的既有测试/产品路径：

| 既有失败 case | 首轮诊断 |
| --- | --- |
| vtkhdf_results_mpi_2 | 超时，报告 1162.29 s（登记超时 180 s） |
| cut_results_serial | 超时，报告 1146.00 s（登记超时 60 s） |
| k1_preview_mpi_surface | JSON syntax_error，子进程异常退出 |
| solve_column_gmg_fcbga_scaling | scaling-fine-gmg 子进程退出 -9，原因未确认 |
| preview_outline_external_polyset_rejected | 返回 13，但缺少预期 error/failure 事件 |
| demogate_solve | zero 子进程退出 -9，原因未确认 |
| f1130_uncut_cli_e2e | In1.Cu 实测 0.09，既有预期为 0 |
| stack_writer_outline_polyset | solve 返回 11，duplicate material id |
| import_assembly_cf_chain | solve 返回 11，duplicate material id |
| ecaddiff_import_pair | summary 计数与既有预期不符 |

十项失败使用原环境、原断言和原超时串行复跑（实测，312.52 s）：4 PASS、6 FAIL。
`vtkhdf_results_mpi_2`、`cut_results_serial`、
`preview_outline_external_polyset_rejected`、`demogate_solve` 复跑通过；
仍失败的是 `k1_preview_mpi_surface`、`solve_column_gmg_fcbga_scaling`、
`f1130_uncut_cli_e2e`、`stack_writer_outline_polyset`、`import_assembly_cf_chain`、
`ecaddiff_import_pair`，诊断与首轮相同。没有修改产品代码、既有断言或超时。
这些未改动路径的失败阻止了本次全量全绿；本卡只交付失败验收测试。
macOS/Apple clang、其他可选后端配置：未执行。
远端 CI/ci-ok：未执行，本次仅本地提交、不推送。

## 同日实现复核

复核起点为已提交且工作区干净的 `ef34937`，已有本卡的夹具、独立参考与 strict
xfail 实现，没有未提交草稿。合并体积普通断言按计划收紧为绝对 `1e-12`，
此前使用相对 `1e-12`；其余验收断言保留。重新执行 Release 构建成功，验收实测仍为
24/24（8 PASS + 16 XFAIL，6.03 s），所有数值与上列输出逐行一致。
外部解析目标驱动仍为 16/16 失败，且均打印匹配现行预测的 XFAIL；
xfail 的解析目标、容差和判定均保留。

重新核对登记矩阵 0/17/24、标签、超时、输出正则及无 WILL_FAIL；
checker 的 12 项返回值/输出检查通过，正则驱动只接受 XFAIL，拒绝另外三种状态。
两 dispatcher 共 10 项非法参数探针均返回 4 并列出完整名称。
clang-format 23.1.5 的 `-i` 和 `--dry-run --Werror` 均成功。

本轮完整 Release 回归使用原断言、原超时串行运行，实测 355 项：333 PASS、
13 个既有 SKIP、9 FAIL，CTest 返回 8，耗时 3969.89 s；24 个 tracemap case 均通过。
仍失败的六项为 `k1_preview_mpi_surface`、`solve_column_gmg_fcbga_scaling`、
`f1130_uncut_cli_e2e`、`stack_writer_outline_polyset`、`import_assembly_cf_chain`、
`ecaddiff_import_pair`。GMG scaling 本轮在 coarse 子进程退出 -9，其余诊断同上表。
此前超时的 MPI/HDF5、cut-results，以及 demo gate 本轮均通过。

另外三项在本轮出现此前未持续重现的失败，原因尚未确认：

| Case | 本轮首跑实测 |
| --- | --- |
| solve_profile_cuda_unavailable | 子进程触发原有 120 s 超时 |
| solve_cut_manufactured | 原有 1800 s 超时，实测 1800.03 s |
| cutbim_cut_bimetal | A/nx16/ranks1 子进程退出 -9，缺少比较量 |

这三项以相同环境、原断言和原超时串行单独复跑，实测 3/3 PASS，CTest 返回 0，
总耗时 480.69 s；依次为 1.54 s、235.61 s、243.53 s。首跑原因仍未确认，
复跑结果不改变首轮全量回归失败的事实。上述六项既有失败仍未解决，
所要求的全量 CTest 全绿尚未达到；未修改 brief 禁改的产品路径或其他测试。
macOS/Apple clang、其他后端及远端 CI/ci-ok 均为未执行。
