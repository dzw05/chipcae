#pragma once
#include "chipcae/mesh/background/hex_mesh_arrays.hpp"
#include "chipcae/schema/stack.hpp"

#include <vector>

namespace chipcae::mesh::cut {
struct BodyLevelSet {
    BodyId body{}; // ID from HexMeshArrays::attr_table.bodies.
    // One value per background mesh vertex, in HexMeshArrays::vertices order
    // (phi[i] corresponds to vertices[3*i .. 3*i+2]); negative inside the union.
    std::vector<Real> phi;
};
} // namespace chipcae::mesh::cut

namespace chipcae::mesh {
using cut::BodyLevelSet;

struct RegionLSOptions {
    Real inside_tol_rel = 1e-12;
};
// region_level_sets — 契约 v1.0.2 续六（架构师裁定 2026-11-18 02:55，最终文本，审查以此为准）
//
// 前置条件：mesh 由 build_tensor_grid(stack, …) 用同一个 stack 生成，
// 因此每个 region 的有效 body 都已登记在 mesh.attr_table.bodies 里。
//
// 定义：effective_body(r) = r.body 非空时取 r.body，否则取 r.name（逐字比较，区分大小写）。
// 来源：一个 region 当且仅当 footprint.polygon 非空时才是来源；只有 box 的 region
//   不是来源，不产出 phi（box 是精确的，不切）。
//
// 输出：每个至少有一个来源 region 的有效 body b，恰好输出一个条目（唯一键 = body）。
//   - entry.body = mesh.attr_table.bodies.at(b)。
//   - entry.phi.size() == 网格节点数，节点顺序与 HexMeshArrays 的节点编号一致。
//   - 对节点 v：phi[v] = min over { (li, ri) : ri 是第 li 层中 effective_body == b 的来源，
//       且 v 的 z 落在第 li 层的闭区间 [z0, z1] 内 } 的 sd2(v.xy, ri.polygon)；
//     sd2 是到多边形边界的二维带符号距离，内部为负、外部为正。
//     没有任何满足条件的 (li, ri) 时，phi[v] = +std::numeric_limits<Real>::infinity()。
//     所以同一 body 跨多层、或同层多个 region 时，取它们的并集（逐点取 min）；
//     层界面上的节点同时属于上下两层，同样取 min。
//   - 条目按 entry.body（BodyId）升序排列，不含重复 body。
//   - 没有任何来源时返回空 vector。
//
// 错误：如果 b 不在 attr_table.bodies 里，说明违反了前置条件（调用方编程错误，
//   不是用户输入问题），抛 std::invalid_argument，
//   消息为 "region_level_sets: body '<b>' not in attr_table.bodies"。不跳过，也不返回部分结果。
//   签名已冻结为返回 vector，不引入 Expected。
//
// 不追溯来源：输出里不保留层号和 region 名，下游只按 body 使用。需要层或 region 来源的调用方
//   自己用上面的定义从 stack 重新推导。BodyLevelSet 不加字段（它属于 mesh/cut，
//   cut_layer 的 to_device 按 body 为主序打包）。
//
// 留给 SP5（不在 SP0 范围）：sd2 的数值实现和精度；inside_tol_rel 的具体用法
//   （在边界附近把 |phi| 很小的值归零或钳位）；带孔、自交或退化多边形（本函数假定
//   load_stack 已校验）；outline 与 region 相交时的取舍；void 单元。SP0 只交付声明、
//   这段注释，以及抛 logic_error 的桩。
std::vector<BodyLevelSet>
region_level_sets(const StackModel&, const HexMeshArrays&, const RegionLSOptions&);
} // namespace chipcae::mesh
