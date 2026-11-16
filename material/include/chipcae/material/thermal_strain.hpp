// chipcae/material/thermal_strain.hpp —— 热应变 ε_th(T)（切线 / 割线 CTE，Q3）
#pragma once

#include "chipcae/material/material_point.hpp"
#include "chipcae/material/table_view.hpp"

#include <cstdint>
#include <string_view>
#include <utility>

namespace chipcae::material {

enum class CteType : std::uint8_t { Tangent, Secant };

enum class ThermalSymmetry : std::uint8_t { Isotropic, TransverselyIsotropic, Orthotropic };

// 只读参数（主机侧）；内核使用不可变 TableView 快照。
struct ThermalStrainParams {
    CteType cte_type = CteType::Tangent;
    ThermalSymmetry symmetry = ThermalSymmetry::Isotropic;
    Real T_ref = 0.0; // 割线 CTE 定义温度 [K]；cte_type = secant 时必填
    Real T_sf = 0.0;  // 应力自由温度（热应变零点）[K]
    // 局部坐标对角 CTE 分量数：isotropic=1, transversely_isotropic=2, orthotropic=3
    std::uint8_t n_components = 1;
};

// ε_th(T) 的权威实现（局部坐标对角张量）；主机/设备共用的小函数对象。
class ThermalStrain {
public:
    ThermalStrain() = default;
    ThermalStrain(ThermalStrainParams params, TemperatureTable table)
        : params_(params), table_(table) {}

    // 返回 ε_th(T) 的 Mandel 6 向量（仅 0..2 对角非零）。
    // tangent：ε_th(T) = ∫_{T_sf}^{T} α_tan(s) ds（分段线性 α 的精确分段二次积分）。
    // secant： ε_th(T) = α_sec(T)(T−T_ref) − α_sec(T_sf)(T_sf−T_ref)。
    KOKKOS_INLINE_FUNCTION StatusCode evaluate(Real T, Vec6& eps_th) const noexcept {
        return evaluate(T, params_.T_sf, eps_th);
    }
    KOKKOS_INLINE_FUNCTION StatusCode evaluate(Real T, Real T_sf, Vec6& eps_th) const noexcept {
        eps_th = Vec6{};
        if (table_.n == 0)
            return StatusCode::Ok;
        for (int d = 0; d < 3; ++d) {
            int c =
                params_.symmetry == ThermalSymmetry::Isotropic
                    ? 0
                    : (params_.symmetry == ThermalSymmetry::TransverselyIsotropic ? (d == 2 ? 1 : 0)
                                                                                  : d);
            Real v = 0;
            StatusCode code;
            if (params_.cte_type == CteType::Tangent)
                code = table_.integral(c, T_sf, T, v);
            else {
                Real a, b;
                code = table_.evaluate(c, T, a);
                if (code == StatusCode::Ok)
                    code = table_.evaluate(c, T_sf, b);
                if (code == StatusCode::Ok)
                    v = a * (T - params_.T_ref) - b * (T_sf - params_.T_ref);
            }
            if (code != StatusCode::Ok)
                return code;
            eps_th[d] = v;
        }
        return StatusCode::Ok;
    }

    // 实际使用的 CTE 类型、T_ref、T_sf（主机侧元数据，规范 1.2 日志要求）。
    CteType cte_type() const noexcept { return params_.cte_type; }
    Real reference_temperature() const noexcept { return params_.T_ref; }
    Real stress_free_temperature() const noexcept { return params_.T_sf; }

private:
    ThermalStrainParams params_{};
    TableView table_{};
};

} // namespace chipcae::material
