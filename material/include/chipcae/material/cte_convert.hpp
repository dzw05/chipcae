// chipcae/material/cte_convert.hpp —— 切线 / 割线 CTE 转换（规范式 4、5；C8）
#pragma once

#include "chipcae/material/material_point.hpp"

#include <vector>

namespace chipcae::material {

// 输入同一张 α(T) 表（T 严格递增），在节点上互相转换：
//   α_tan(T) = α_sec(T) + (T − T_ref) dα_sec/dT
// Node samples alone do not preserve the converted function between knots.
// Use the scalar overloads below for exact conversion at arbitrary T.
struct CteTable {
    std::vector<Real> temperatures; // K，严格递增
    std::vector<Real> alpha;        // 1/K
    Real reference_temperature = 0.0;
};

// 割线 → 切线（节点值）。返回 Ok 或 Failed（无效表）。
StatusCode secant_to_tangent(const CteTable& secant, std::vector<Real>& alpha_tangent);

// 切线 → 割线：α_sec(T) = (1/(T−T_ref)) ∫_{T_ref}^{T} α_tan(s) ds。
StatusCode tangent_to_secant(const CteTable& tangent, std::vector<Real>& alpha_secant);

// Exact evaluation of the converted function; constant endpoint extension.
// Secant derivatives use the right-hand segment at interior knots.
StatusCode secant_to_tangent(const CteTable& secant, Real T, Real& alpha_tangent);
StatusCode tangent_to_secant(const CteTable& tangent, Real T, Real& alpha_secant);

} // namespace chipcae::material
