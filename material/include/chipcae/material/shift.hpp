// chipcae/material/shift.hpp —— 移位函数（WLF / Arrhenius / table / 拼接）
#pragma once

#include "chipcae/material/material_point.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace chipcae::material {

// CODATA 2018 气体常数 [J/(mol·K)]
inline constexpr Real kGasConstant = 8.314462618;

enum class ShiftType : std::uint8_t { WLF, Arrhenius, Table, PiecewiseWlfArrhenius };

// 主机侧不可变参数；设备端使用时须转成 const View（CHUNK 4）。
struct ShiftParams {
    ShiftType type = ShiftType::WLF;
    Real T_ref = 0.0;              // 参考温度 [K]
    Real T_g = 0.0;                // 拼接温度只取 glass_transition.T_g [K]
    Real C1 = 0.0;                 // WLF
    Real C2 = 0.0;                 // WLF [K]
    Real E_a = 0.0;                // Arrhenius 活化能 [J/mol]
    std::vector<Real> table_T;     // log10 aT 对 T 的节点 [K]
    std::vector<Real> table_log10; // 节点上的 log10 aT
};

// 设备可调用的定长视图版本（v0 先用小的内联数组，CHUNK 4 接入 ParamView）。
class ShiftFunction {
public:
    ShiftFunction() = default;
    explicit ShiftFunction(ShiftParams params) : params_(std::move(params)) {}

    // L_W(T) = −C1 (T − T_ref) / (C2 + T − T_ref)
    // L_A(T) = E_a/(R ln10) (1/T − 1/T_ref)
    // 拼接：T ≥ T_g 用 WLF；T < T_g 用 L_W(T_g) + E_a/(R ln10)(1/T − 1/T_g)
    // log10 aT = L(T) − L(T_ref)
    StatusCode evaluate(Real T, Real& log10_aT) const noexcept {
        (void)T;
        (void)log10_aT;
        return StatusCode::NotImplemented; // TODO(K3)
    }

    // 步内温度线性时的约化时间增量 Δξ = ∫ ds / a_T(T(s))（CHUNK 4）。
    StatusCode reduced_time_increment(Real T_n, Real T_np1, Real dt, Real& dxi) const noexcept {
        (void)T_n;
        (void)T_np1;
        (void)dt;
        (void)dxi;
        return StatusCode::NotImplemented; // TODO(K3)
    }

    const ShiftParams& params() const noexcept { return params_; }

private:
    ShiftParams params_{};
};

} // namespace chipcae::material
