# CutFEM 热弹性可行性原型

这是自写 Q1 的可行性原型，正式实现走 MFEM 4.10，接口形状已按 K1↔K2 组织。
本目录是独立 CMake 项目，不加入仓库主构建；Eigen 不在依赖许可清单中，故这里自行实现稀疏装配、带状直接解与 CG。
无必选外部依赖。可选 hypre 3.2.1（MPI、32-bit int、静态库）。只用 IJMatrix/ParCSR 与 BoomerAMG-PCG，不调用其他有限元库。

仅 2D、Cartesian Q1、plane strain、FP64、单 MPI rank；3D 未实现。
可选 Kokkos 仅用于热传导和弹性的体积分装配内核，默认仍使用原主机路径。

## 运行

```bash
cd solvers/fem/prototypes/cutfem_thermoelastic   # 从仓库根目录
./run_all.sh
./run_all.sh --with-hypre
./run_all.sh --with-kokkos
./run_all.sh --with-hypre --with-kokkos
```

需要 CMake≥3.20、C++20 编译器、Ninja、python3、numpy、matplotlib。脚本使用 Release `-O2`、`-j4`，实验顺序执行，BLAS 设单线程；Kokkos 计时分别使用 1、2、4 个 OpenMP 线程。
默认无 hypre；`--with-hypre` 使用 `/workspace/tools/env/bin/mpicxx` 和只读的 `/workspace/prototypes/p1-mfem-thinlayer/deps/hypre`。脚本设运行时库路径。路径可用环境变量 `KOKKOS_DIR`、`MPI_CXX`、`HYPRE_ROOT` 覆盖。
CMake 可指定 `-DHYPRE_ROOT=...` 与 `-DMPI_CXX_COMPILER=...`。hypre 的默认配置是 HMIS coarsening (10)、extended+i interpolation (6)、symmetric hybrid GS relaxation (6)、每次一轮 V-cycle、阈值 thermal 0.25 / elastic 0.5；elastic 设置 NumFunctions=2、Nodal=1，节点 DOF 交错。PCG 真相对残差目标 1e-8。针对安装库与系统链接器的 GOT relocation relaxation 兼容问题，hypre 路径添加 `--no-relax`，防止函数地址入栈被错误改写为函数调用。

单项调试：

```bash
./build/cutfem_proto unit_tests
./build/cutfem_proto mms_thermal --N 32 --gamma-N 10 --gamma-g 0.1 --output results/debug
./build/cutfem_proto mms_elastic --solver direct
./build/cutfem_proto mms_thermal_bimat
./build/cutfem_proto timoshenko
./build/cutfem_proto gp_sweep
```

`--solver direct|pcg|hypre` 控制 MMS/条带主求解器；gp_sweep 固定比较所有可用迭代解法。
MMS 的 thermal 两变体与 elastic GP on/off 均执行；Timoshenko 扫描 gamma_N=10、gamma_g=0.01/0.1/0.3/1，并比较 gamma_N=20/40、gamma_g=0.1。gp_sweep 扫描 gamma_N=10/20/40、gamma_g=0/0.03/0.1/0.3/1/3 和五种 vf；包含热传导、左端夹持弹性及右侧切割端夹持弹性。
`--gamma-g` 控制 MMS 稳定化；实验指定扫描值优先于该标量参数。

## 可选 Kokkos 体积分装配

`CUTFEM_WITH_KOKKOS=ON` 使用 Kokkos 4.7（本机 `/workspace/deps/kokkos`），`--with-kokkos` 自动配置、测试并运行计时。`Problem::kokkos_volume` 选择体积分内核，默认关闭；`kokkos_serial` 选择 Serial，否则使用默认执行空间；`kokkos_atomic_scatter` 选择原子 CSR 散射，默认采用确定性的主机散射。源项和温度回调在主机预先按积分点求值。每个活跃单元的矩阵与右端项在 Kokkos 中计算，材料点函数在每个积分点调用。Nitsche 边界、ghost penalty、CSR 结构构造和求解器仍在主机上。

本机测试 Serial 与 OpenMP（1、2、4 线程），没有 CUDA 环境，未验证 GPU。单元测试比较热/弹性切割问题的矩阵和右端项，以及 Nt=16 Timoshenko 位移；主机和 Kokkos 相对误差阈值为 1e-12。计时为五次中位数，包括仅体积分内核和完整装配；主机体积分计时包含回调求值，Kokkos 内核计时不包含预先在主机求值的回调和数据传输，完整装配则包含这些开销。共享机器的 1 分钟负载也记录在 CSV 中。实测数字与完整表见 [Kokkos 计时汇总](results/summary.md) 和 [CSV](results/kokkos_bench.csv)。

本次 63,228 自由度算例中，主机体积分/总装配中位数为 0.0461/0.1119 秒，OpenMP 4 线程为 0.0232/0.1279 秒；212,368 自由度算例分别为 0.1523/0.3380 秒和 0.0836/0.4605 秒。共享机器负载及全部后端数字见汇总表。

## 目录与接口

- `include/cutfem/mesh.hpp`：HexMeshArrays，平坦 vertices、elem_vert、GID、attribute、partitioning，保留 xcoords/ycoords；VTK_QUAD 逆时针顶点，节点 `i*(ny+1)+j`，y 最快以压低条带矩阵带宽。
- `include/cutfem/cut_layer.hpp`、`src/cut_layer.cpp`：Body 半平面交集、CutLayer 的 cell_state/material/volfrac、CV compact 编号、积分 CSR 与 gp_faces。
- `include/cutfem/quadrature.hpp`：Sutherland–Hodgman clipping、Dunavant 三角积分、一般 level set 递归子网格积分。
- `include/cutfem/material.hpp`：MaterialParamsSoA 和无堆分配/无异常的线弹性材料点函数、导热系数函数。
- `include/cutfem/assembly.hpp`、`src/assembly.cpp`：局部积分、CSR 装配、紧凑 active-node DOF、PrototypeSolver。
- `include/cutfem/solvers.hpp`、`src/solvers.cpp`：CSR、约束消元、带状 LDLᵀ、CG/PCG、Lanczos 条件数、可选 hypre。
- `stubs/k5_stubs.hpp`：serial Communicator、CvField（Node/Cell/QuadPoint）、StepResult、Solver。温度是节点 Q1 CvField，stress 是带 vq_offsets 的三分量 QuadPoint CvField，StepResult 包含位移、温度、应力、warpage、curvature、求解统计。
- `src/experiments.cpp`、`apps/cutfem_proto.cpp`：实验和 CLI；`tests/unit_tests.cpp`：patch、几何积分、递归积分、FD 力、消元和解法/谱估计测试。
- `scripts/plot.py`：仅从实测 CSV 生成 PNG、Markdown 表和阶数，检查稳定路径的最细层收敛阶。
- `results/`：CSV、运行日志、四张图与[实测汇总](results/summary.md)。

CutLayer 显式三遍构建：count → exclusive prefix sum → fill。体积分权重在 reference [0,1]² 内，和等于 volfrac，尚未乘 hx*hy；边界权重字段明确叫 **bq_w_phys**，已含物理弧长。法线是物理单位外法线。tag 来源于对应 body 的半平面。
材料交界必须在背景网格线上，交界半平面 tag=-1，不作为外边界施加 BC；材料不同的面不加 GP。attribute 对每个 (body/layer, material) 紧凑从 1 开始，inactive 为 0。

## 弱式与符号

设 h=min(hx,hy)，默认 gamma_N=20、gamma_g=0.1。梯度均为物理坐标梯度，体积分只覆盖实际材料。
热传导方程 −div(k∇T)=f：

$$
a_T(T,v)=\sum_K\int_{K\cap\Omega} k\nabla T\cdot\nabla v
-\int_{\Gamma_D} k\partial_nT\,v
-\int_{\Gamma_D} k\partial_nv\,T
+\int_{\Gamma_D}\gamma_N k/h\,Tv
+\int_{\Gamma_R}h_cTv+j_T(T,v),
$$

$$
\ell_T(v)=\int_\Omega fv-\int_{\Gamma_D}k\partial_nv\,g
+\int_{\Gamma_D}\gamma_Nk/h\,gv
+\int_{\Gamma_R}(h_cT_\infty+q)v.
$$

Robin 定义为 k∂nT+hc(T−Tinf)=q。MMS Robin 一条边使用 hc=2，其 q 从解析解精确计算。

plane strain 材料：mu=E/[2(1+nu)]，lambda=E nu/[(1+nu)(1−2nu)]，beta=(3lambda+2mu)alpha，dT=T−T_stress_free。
Voigt 应变为 (eps_xx, eps_yy, 2eps_xy)，应力为 (sigma_xx,sigma_yy,sigma_xy)，D33=mu。

$$
\sigma=C:\epsilon(u)-\beta\Delta T I,
$$

$$
a_u(u,v)=\int_\Omega\epsilon(v):C:\epsilon(u)
-\int_{\Gamma_D}(C:\epsilon(u)n)\cdot v
-\int_{\Gamma_D}(C:\epsilon(v)n)\cdot u
+\int_{\Gamma_D}\gamma_N(\lambda+2\mu)/h\,u\cdot v+j_u(u,v),
$$

$$
\ell_u(v)=\int_\Omega f\cdot v+\int_\Omega\beta\Delta T\,\mathrm{div}\,v
+\int_{\Gamma_t}t\cdot v
-\int_{\Gamma_D}(C:\epsilon(v)n)\cdot g
+\int_{\Gamma_D}\gamma_N(\lambda+2\mu)/h\,g\cdot v
-\int_{\Gamma_D}\beta\Delta T\,n\cdot v.
$$

最后一个负号来自完整应力 traction 的一致性项，测试用非零边界 dT 验证。traction-free 边界不加项。
自由体用内部背景节点在界面上点约束：A 接近质心，固定 ux/uy，B 距 A 约 L/4 固定 uy；按行列消元并修正 RHS，不用惩罚大数。

$$
j(w,v)=\sum_{F\in\mathcal F_g}\gamma_g h\,c_F\int_F[\partial_n w][\partial_n v].
$$

F 是两个 active、同材料且至少一侧 cut 的完整背景面；两点 Gauss。thermal cF=k，elastic cF=E 并逐分量相加；系数实现取两侧最大值，但材料界面面被排除。仅一阶导数跳跃（Q1）。关闭 GP 时固定 gamma_N/h 对很小体积分数不保证正定，扫描记录负曲率，绝不通过调大参数隐藏失败。

## 几何积分与解法

半平面在单元 reference 坐标精确截方形，凸多边形扇形三角化，每三角用七点 degree-5 Dunavant；full 单元 3×3 Gauss。
实际 body 边界每段三点 Gauss；碰巧位于背景面上的真实外边界也只计一次，这使 fitted 参考和 Robin 全边界正确。普通内部单元边不生成边界积分。
递归选项 CLI `--quadrature recursive --max-depth 2`，API `build_level_set_layer(mesh,phi,max_depth,material_id)` / `recursive_quadrature(phi,max_depth)`：在 2^depth 子网格叶方形中用两个三角形的角点线性插值 clipping，再用同样三角规则。Body 的递归选项默认使用 max(phi_k)，一般 scalar lambda 用 build_level_set_layer 接入完整 CutLayer CSR（含边界法线/物理权重）。平面 level set 精确，测试比较 0–4 阶所有矩以及 CutLayer 面积/边长；非线性 level set 是角点分片线性近似，不能可靠发现角点采样不可见的孤立小体。默认 Body 半平面路径。

稀疏结构先建立 element/GP pattern，再累计局部矩阵。带状 LDLᵀ 从 CSR 的下三角提取，存储 O(n*b)、运算 O(n*b²)，利用节点排序的小半带宽。
默认要求 SPD；仅 GP-off elastic MMS 允许负 pivot 的 LDLᵀ 求解以报告原方程的误差，同时标记 indefinite，不添加正则化。
Jacobi-PCG 与无预条件 CG 均 FP64、上限 20000，1e-8 相对残差；用 alpha/beta 建 Lanczos 三对角矩阵，Sturm 二分取极端 Ritz 值，估计相应预条件算子条件数。无收敛记录 no_conv，非正 pᵀAp / Jacobi 非正对角记录 indefinite。扫描另外用带状 LDLᵀ 负 pivot 独立检查不定性，并以 NumPy 对导出矩阵求完整特征谱；Ritz 条件数估计依赖 RHS 和停止阈值，极小单元的未激发模态会使估计偏低或不单调。

## 实验和 Timoshenko 对照

MMS 使用非贴体凸四边形，N=8/16/32/64/128；热解、混合 Robin、非零热应变弹性解、双材料热解均在真实 cut 积分点计算解析 RHS。弹性 MMS 的 dT 在积分点精确求值，不插值。L2 与 H1 seminorm 直接在体积分 CSR 上求值。
双材料界面 y=0.5，温度 `sin(2x+0.5)*(1+sin(y-0.5)/k)` 连续且通量连续。

双层条带为 **example parameters**（示例参数，非实测封装数据）：底 EMC E=20 GPa、nu=0.3、alpha=1e-5/K、k=0.9；顶 Si E=130 GPa、nu=0.28、alpha=2.6e-6/K、k=150。t1=0.47 mm，t2=0.31 mm，L=25h=19.5 mm。
125°C 为无应力温度，Robin hc=10、Tinf=25°C，全边冷却 dT=−100 K。先实际求稳态温度，再以节点 Q1 温度加载弹性。
cut 网格 hc=h/Nt，界面 y=0，x0=1.37hc，底顶两端均不贴网格并检查距离；仅当自定义 Nt 恰好造成厚度整除时微调 hc，默认四级不需微调；fitted 网格每层厚度按 round(t_layer/hc) 划分，保留界面与真实外表面。

Timoshenko plane-strain 修正：E_i′=E_i/(1−nu_i²)，alpha_i′=alpha_i(1+nu_i)，m=t1/t2，n=E1′/E2′，h=t1+t2。

$$
\kappa_{\rm formula}=\frac{6(\alpha_2'-\alpha_1')\Delta T(1+m)^2}
{h[3(1+m)^2+(1+mn)(m^2+1/(mn))]}.
$$

本实现以向上为 y 正、kappa_num=uy″；因 eps_xx=u0′−y uy″，报告 kappa_ref=−kappa_formula。底部冷缩更多，对应负曲率，两端相对中部向下。
从界面背景节点、中央半宽 L/4 拟合 uy=c0+c1 x+c2 x²，kappa=2c2；用无量纲居中 x/L 提高拟合数值稳定性。另以 L/8 半宽检查窗口敏感性。top warpage 由真实顶面 Q1 插值在所有背景 x 网格交点及条带两端的 uy 最大减最小求得（分片线性的精确极值）；两点约束包含刚体转动选择，因此这是约束坐标系的 warpage。
曲率误差中包含 Q1 弯曲、切割稳定化、二维/有限长度参考差异。实测报告三层 Richardson 阶、fitted 最细网格差值与窗口敏感性，不声称未经外推的最细层差值就是纯模型误差。

## 结果

执行脚本生成 [results/summary.md](results/summary.md)，包括完整表、收敛阶和诚实 notes；[热 MMS](results/mms_thermal.png)、[弹性 MMS](results/mms_elastic.png)、[条带曲率](results/timoshenko.png)、[GP 扫描](results/gp_sweep.png)。
最后从仓库根目录执行现有 `cmake/check_license_policy.py`，日志保存在 results/license_policy.log。代码不加入主构建，不改其他目录，不提交。

GP 扫描可使用 `./build/cutfem_proto gp_sweep --dump-matrices` 将装配矩阵写入 `results/debug/`（不纳入版本控制）；`python3 scripts/spectrum.py --results results` 生成 `gp_spectrum.csv`，记录全谱极值、负特征值数及 Jacobi 缩放条件数。`run_all.sh` 自动运行该流程；矩阵内容 SHA-256 不变时复用 `results/debug/` 中的实测谱缓存。验收表对每个物理、Nitsche 系数和 ghost penalty 十倍区间统计全部 vf 的 SPD 状态与迭代数最大/最小比；求解失败或不定时比值标为不可用。

按三种物理、全部 vf 的谱扫描，默认 Nitsche 系数取 20、GP 系数取 0.1；相较 10/0.1，小切割下保持正定，条带曲率精度见结果表。该选择仅针对本扫描的参数和模型，十倍 GP 区间的迭代波动见验收表。
