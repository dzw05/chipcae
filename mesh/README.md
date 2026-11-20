# mesh

cut-cell 网格和分层六面体网格生成。

Cut-cell 从第一天起按“一个单元可含多个控制体”定义，每个控制体带有 region、material 和 body 编号，以支持多材料共轭传热和薄壁结构。底座在 Kokkos 与 MPI 上自研：MVP 只做均匀网格或静态加密，动态 AMR 放到后续版本。

分层六面体网格面向封装翘曲的有限元分析。本模块不引入外部网格库作为依赖。

## nx/ny 与 cell_budget

`GridOptions::nx` / `ny` 是每个特征线区间的最少单元数，不是全板的总列数。
[`tensor_grid.cpp:363–384`](background/src/tensor_grid.cpp#L363-L384) 的 `plan_axes`
用最短特征区间除以相应轴的最少单元数来确定全轴目标步长；`1 < max_ratio < 2`
时还会进一步缩小目标步长。每个区间的单元数向上取整，因此实际单元宽度可以略有不同。
窄条带会加密整个轴，直接把全板分辨率数值填入 nx/ny 会大幅放大网格。

预算检查使用稠密的 X 区间数 × Y 区间数 × nz，即使空层区域过滤后实际输出的单元更少，
也仍按这个稠密规模检查 `cell_budget`。以下是合成基板、带孔加强圈和倒角八边形盖板经
`merge_parts → build_tensor_grid` 的**实测值**，由 `asm_mesh_chain` 回归测试锁定。
基板为 56 × 44 × 1 mm，加强圈和盖板各厚 0.5 mm；所有层的 `n_elem_min` 相同。
选项为 `levels=1`、`ranks=1`、`max_ratio=2`、`snap_tol=1e-14`、`cell_budget=50000`。
单元数和 pending 数均在切割前记录。

所有层 `n_elem_min=1`，nz=3：

| nx × ny | 实测 X × Y 区间数 | 稠密 X × Y × nz | 实测输出单元数 | 实测 pending_cut 数 | 结果 |
|---|---|---:|---:|---:|---|
| 1 × 1 | 14 × 25 | 1050 | 808 | 350 | 成功 |
| 2 × 2 | 26 × 48 | 3744 | 2867 | 1248 | 成功 |
| 4 × 4 | 48 × 92 | 13248 | 10123 | 4410 | 成功 |
| 8 × 8 | 92 × 180 | 49680 | 37915 | 16526 | 成功 |
| 56 × 44 | 630 × 972 | 1837080 | — | — | HIERARCHY_INVALID |

所有层 `n_elem_min=2`，nz=6（相当于求解默认的 `min_elems_per_layer=2`）：

| nx × ny | 实测 X × Y 区间数 | 稠密 X × Y × nz | 实测输出单元数 | 结果 |
|---|---|---:|---:|---|
| 1 × 1 | 14 × 25 | 2100 | 1616 | 成功 |
| 2 × 2 | 26 × 48 | 7488 | 5734 | 成功 |
| 4 × 4 | 48 × 92 | 26496 | 20246 | 成功 |
| 8 × 8 | 92 × 180 | 99360 | — | HIERARCHY_INVALID |
| 56 × 44 | 630 × 972 | 3674160 | — | HIERARCHY_INVALID |

在已测试档位中，nz=3 时 n=1、2、4、8 均可行，nz=6 时 n=1、2、4 可行；
56 × 44 在两种 nz 下都不可行。这些范围只针对上述合成栈与预算，不是其他几何的通用上限。
失败档的 `hierarchy_candidates` 首行为 L=0、上述 X/Y 区间数、`feasible=false`。
当前错误路径为 `HIERARCHY_INVALID`，消息为
`hierarchy padding violates budget or max_ratio`，不带实际列数、nz 或预算数字。
