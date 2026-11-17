# WV-PKG-I25D / interposer25d-kim2016（2.5D 硅中介层翘曲基准 v0.1，V&V 修订 2026-11-16）

- 规范格式：`benchmark.yaml`（V&V 规范格式 v0.1）。数值以 YAML 为准。
- 共用约定、后处理与 SC-3–SC-10 见姊妹卡 `../fcbga-shih2023/`。
- 用途分层、公开对比、尚未运行的声明：同 FCBGA 卡。

## 几何与 Kim 2016 对称性核对

文献（Kim et al., Microelectron. Reliab. 65:234–242, 2016，cited）Table 1：基板 40×40×1.15 mm，中介层 26.4×19.5×0.1 mm，ASIC 18.5×18.6×0.4 mm，存储块 8×4×0.5 mm，并含 stiffener。正文（§3，Fig. 7）写明采用 **3D 半模型**（“symmetric structure … a 3D half model”）。ASIC 非正方，中介层为矩形，文中未给出可核验的双轴对称四存储块平面坐标。

**结论：文献封装不能当作四分之一（双轴）对称。** P 系列与所有 NLGEOM 打开的档位用全模型；与原文对照时可用半模型。chipcae 默认跑全模型。

本卡参数集 A 中「SoC 14.0×17.5 + 4 个镜像存储块」是 **chosen** 的 verification 布局，不是 Kim 2016 实物布置。该布局仅允许在 NLGEOM 关闭的小变形外部对照中使用四分之一模型，且必须另留全模型变体。

## 网格（MVP：cut-cell 主路径）

与 FCBGA 卡相同：张量积笛卡尔背景网格，z 向对齐层界面；C4/微凸点、圆角、斜边用 cut。新增 `F-UNALIGNED` 变体（中介层上底填或芯片下底填圆角不与网格对齐），QoI：W_bfp、S_cc、沿 P1/P2 路径的翘曲、κ、芯片中心应力（对齐 vs 非对齐容差待首次收敛后定）。路径定义（衬底底面 z = 0，原点在底面中心）：**P1** 为 y = 0、x ∈ [0, 20] mm；**P2** 为 x = 0、y ∈ [0, 20] mm；各 41 点，先扣最佳拟合平面，度量 max|Δw| / W_ref（见 `benchmark.yaml` 的 `qoi_definitions`）。

## 系列

- X / PW：verification，约定同 FCBGA 卡。PW 冻结值同一套测试 `tests/vv/test_pw_reference.py`。
- **P-KIM2016**（validation）：已发表 Ansys 14.5 半模型结果（图 8、图 10 等）。布局、加强圈尺寸与曲线数字化待核对（加强圈材料与 175 °C 贴装温度为文献值，尺寸 Kim 2016 未给，`geometry.stiffener` 标 tbd）。P 系列硅用 Kim 2016 Table 3（中介层 168 GPa / 3.3 ppm，ASIC 169 / 3.3，存储 169 / 2.6，ν = 0.28），即 `SI_KIM2016_*` 卡；`SI`（Chen 2022）只用于 X/PW。均质层按 Kim 2016 Table 3 组分重推（式 (1)–(4)，`tools/vv/derive_cards.py`）：微凸点/底填层用卡 `I25D_UBUMP_UF_KIM2016`（Cu 125 GPa / ν 0.3 / 16.5 ppm，φ = 0.03 为「less than 3%」的上限）；C4/底填层（SnAg ν 0.4 / 21.5 ppm，φ = 0.10）的 SnAg E(T) 转引 Lau 1996、单位未注明，核对前 **tbd**，不进冻结卡。Voigt 横观各向同性只作备选卡，Halpin–Tsai 不作默认。Prony 缩放 s 灵敏度同 FCBGA 卡（三端点 s = 1、s = 0、E ≡ E∞，`tools/vv/prony_scale.py`）。文献、借用、chosen、tbd 项全部列在 `series.P-KIM2016.parameters`。数字化前只比趋势；之后开源复现与已发表曲线相差 ≤15%（建议）。P 系列不得借用 EMC 底填卡。B 系列是简化模型，不期望等于 P-KIM2016。

## 许可

本算例数据按 CC BY 4.0 发布，署名与第三方文献说明见 [../NOTICE](../NOTICE)，全文见 [../LICENSE](../LICENSE)。使用文献参数时请同时引用原文献。
