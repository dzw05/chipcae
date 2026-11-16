# WV-PKG-FCBGA / fcbga-shih2023（公开封装翘曲基准 v0.1，V&V 修订 2026-11-16）

- 规范格式：`benchmark.yaml`（V&V 规范格式 v0.1，不是求解器算例 schema）
- 数值以 YAML 为准。
- 用途分层：
  - **X / PW 系列 = verification（代码对代码）**，允许 chosen 参数。
  - **P 系列 = validation（对照已发表数据）**，参数只能来自文献；chosen 或 tbd 参数必须在报告中显式列出。P 系列不得使用借自 EMC 的底填 Prony/WLF；拿不到文献值时改为灵敏度参数并给出取值范围。
- 公开对比：开源求解器（code_aster+MFront 为主，MOOSE 交叉 B2/B3，CalculiX 覆盖 B0/B1/B3a）与已发表数据。外部工具只作为外部程序运行，不链接进 chipcae。
- **本卡与各求解器输入均尚未运行**，不把未跑数据当作参考。

## 几何（默认集 A，文献）

取自 Shih et al. 2023 无盖 FCBGA（cited）：衬底 40×40×1.2 mm，芯片 15.5×15.5×0.78 mm，凸点/底填均质层厚 0.025 mm。芯片居中，层 footprint 为选定值（chosen）。

## 对称性

- 四分之一对称**只用在 NLGEOM 关闭的小变形外部对照**。
- **几何非线性打开的档位（B3、B4、X-ON、B3a）一律用全模型**（对称边界会压掉非对称分叉，Finot & Suresh 1996）。
- 每个对称模型至少保留一个全模型变体，做一次一致性核对（SC-5）。
- chipcae 默认跑全模型。

## 网格（MVP：cut-cell 主路径）

- 背景网格：张量积笛卡尔网格，**z 向网格面与层界面对齐**。
- 焊球、底填圆角、斜边用 cut-cell 切断，不要求与 x、y 网格线对齐。
- 体拟合开源结果仍作为同一几何的外部对照。
- **变体 `F-UNALIGNED`**：在默认底填侧面增加不与背景网格对齐的圆角（或 45° 斜边）。对齐版（无圆角，或圆角特征边落在网格面上）记为 `F-ALIGNED`。
  - 关心量：B0、25 °C 的 W_bfp、S_cc、κ、σ_die_center。
  - 与**同一几何**的体拟合开源参考比：沿用 B0 容差（建议）。
  - 与对齐版的差是几何敏感性，不是同一问题的离散误差。对齐 vs 非对齐的相对容差 **待首次网格收敛后定**（建议：先报告差值；全局翘曲相对差大于 2% 记为需讨论，不作 hard fail）。理由：圆角改变局部体积与应力集中，全局翘曲预期变化小、局部应力变化大，不宜在无跑数前设硬门槛。

## 阶梯 B0–B4

B0 线弹性、温度无关、小变形；B1 加 E(T) 与 Tg 处 CTE 阶跃；B2 加 Prony+单一 WLF；B3 再加几何非线性（全模型）；B4 再加简化固化收缩。隔离变体 B3a、B4a。

## 系列

- **X 系列**（verification）：单一 WLF、固定设置；X-OFF（NLGEOM 关，允许四分之一）与 X-ON（全模型）。
- **PW 系列**（verification）：跨 Tg 分段移位，只与 `tools/vv/pw_reference.py` 及开源实现比。单元级冻结值见 `tests/vv/test_pw_reference.py`（16.32000 / 16.24540 与 10.44021 / 9.35481 MPa）。
- **P-SHIH2023**（validation）：Shih 2023 已发表室温 FE 最大翘曲 128 µm（实测约 122 µm）。图 12 E(T) 尚未数字化（待核对）。B 系列是简化模型，不期望等于 128 µm。数字化后开源复现与 128 µm 相差 ≤10%（建议）。P 系列底填若无文献 Prony，不得借用 EMC 卡。P 系列参数清单（文献 / 借用 / chosen / tbd）见 `benchmark.yaml` 的 `series.P-SHIH2023.parameters`：`SUB_ISO` 借自 Kim 2016；Shih 2023 Table 3 自带芯片（131 GPa / 2.8 ppm / ν 0.30，卡 `SI_SHIH2023`，P-SHIH2023 专用；`SI` 仅 X/PW）、基板与底填的 CTE、Tg、ν，E(T) 在图 12，尚未数字化。
  - **注意**：P-SHIH2023 的基板（`SUB_ISO`，Kim 2016）与底填/凸点均质层（`FCBGA_BUMP_UF`）参数是借用的。Shih 图 12 数字化之前，结果**不算**对 128 µm 翘曲（Shih 2023 p.9 的 FE 预测值；实测约 122 µm）的复现。均质层不按 Kim 组分重推，改用 Shih 图 12 的「Equivalent material (Underfill+Solder bump)」曲线（数字化 tbd）。
  - Prony 形状缩放 s 只作灵敏度：由 `tools/vv/prony_scale.py` 在加载前生成 Schema v1 卡（s 不是运行时字段），只缩放 E_i、τ_i 不变，E0 不变，E∞(s) = E0 − s(E0 − E∞_ref)。报告 s = 1、s = 0（E ≡ E0）、E ≡ E∞ 三个端点；中间值不是默认本构。`constant_poisson_from_E` 为近似。

## 公开参考与容差

主参考 code_aster + MFront；B2/B3 另用 MOOSE；CalculiX：B0/B1/B3a。容差见 YAML `tolerances_vs_reference`（均为建议）。CPU/GPU 一致性（正确性门禁，任一超出即阻断）：翘曲与位移相对差 ≤1e-6，积分点应力差 ≤最大值的 1e-6。一致性算例要求线性相对残差 ≤1e-12（小算例可用直接法），Newton ≤1e-10。

## 健全性检查

SC-1 等见原卡 §8。Hsueh 多层闭式已按 2006 式 (4b) 更正（见 CHANGELOG）。
