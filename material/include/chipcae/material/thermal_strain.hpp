// chipcae/material/thermal_strain.hpp —— 热应变 ε_th(T)（切线 / 割线 CTE，Q3）
#pragma once

#include "chipcae/material/material_point.hpp"
#include "chipcae/material/temperature_table.hpp"

#include <cstdint>
#include <string_view>
#include <utility>

namespace chipcae::material {

enum class CteType : std::uint8_t { Tangent, Secant };

enum class ThermalSymmetry : std::uint8_t { Isotropic, TransverselyIsotropic, Orthotropic };

// 只读参数（主机侧）；内核使用时另建只读 View（CHUNK 3）。
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
        : params_(params), table_(std::move(table)) {}

    // 返回 ε_th(T) 的 Mandel 6 向量（仅 0..2 对角非零）。
    // tangent：ε_th(T) = ∫_{T_sf}^{T} α_tan(s) ds（分段线性 α 的精确分段二次积分）。
    // secant： ε_th(T) = α_sec(T)(T−T_ref) − α_sec(T_sf)(T_sf−T_ref)。
    StatusCode evaluate(Real T, Vec6& eps_th) const noexcept {
        (void)T;
        (void)eps_th;
        return StatusCode::NotImplemented; // TODO(K3)
    }

    // 实际使用的 CTE 类型、T_ref、T_sf（主机侧元数据，规范 1.2 日志要求）。
    CteType cte_type() const noexcept { return params_.cte_type; }
    Real reference_temperature() const noexcept { return params_.T_ref; }
    Real stress_free_temperature() const noexcept { return params_.T_sf; }

private:
    ThermalStrainParams params_{};
    TemperatureTable table_{};
};

} // namespace chipcae::material
