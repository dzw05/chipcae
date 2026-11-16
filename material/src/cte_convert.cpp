// chipcae/material/src/cte_convert.cpp —— 切线 / 割线 CTE 转换（占位）
#include "chipcae/material/cte_convert.hpp"

namespace chipcae::material {

StatusCode secant_to_tangent(const CteTable&, std::vector<Real>& alpha_tangent) {
    alpha_tangent.clear();
    return StatusCode::NotImplemented; // TODO(K3)
}

StatusCode tangent_to_secant(const CteTable&, std::vector<Real>& alpha_secant) {
    alpha_secant.clear();
    return StatusCode::NotImplemented; // TODO(K3)
}

} // namespace chipcae::material
