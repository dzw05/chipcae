// chipcae/material/material_point.hpp —— 只依赖 C++20 标准库 + Kokkos，不包含任何 MFEM / deal.II
// 头文件
#pragma once
#include <Kokkos_Core.hpp> // v0.2.2：Kokkos View 与 KOKKOS_INLINE_FUNCTION，取代 Q9 的 kokkos/mdspan
#include <array>
#include <concepts>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

namespace chipcae::material {

using Real = double; // 本构一律 FP64（ε − ε_th 的相减抵消要求双精度）；所有物理量一律 SI（Q2）
// 设备可调用规则：本文件中所有模型成员函数均为 KOKKOS_INLINE_FUNCTION，不用虚函数、堆分配、异常

// ---- Mandel 约定：分量顺序 11,22,33,23,13,12；剪切分量乘 √2（应力、应变一致）----
using Vec6 = Kokkos::Array<Real, 6>;  // 二阶对称张量（应力、应变、本征应变）
using Mat6 = Kokkos::Array<Real, 36>; // 四阶张量，行主序，与 Vec6 同一正交基
using Mat3 = Kokkos::Array<Real, 9>;  // 3x3，行主序

enum class Kinematics : std::uint8_t {
    SmallStrain,    // 输入 ε；输出 Cauchy σ；切线 ∂σ/∂ε
    TotalLagrangian // 输入 E = ½(FᵀF − I)；输出 PK2 S；切线 ∂S/∂E（适配层负责推前）—— P0（FOWLP）
};

enum class StatusCode : std::uint8_t { Ok, CutStep, OutOfRange, NotImplemented, Failed };
struct Status {
    StatusCode code = StatusCode::Ok;
    Real dt_factor = 1.0; // CutStep 时建议的步长缩放
};

struct ProcessClock { // 工艺时间表，由 Solver 的工艺步序列给出，所有积分点共享
    std::uint32_t step_id =
        0; // 当前工艺步（与 Schema 中 chemical_shrinkage.schedule 的 step_id 对应）
    Real t_step_n = 0, t_step_np1 = 0; // 本工艺步内的时间 [s]，从该步开始计
    Real t_global_n = 0, t_global_np1 = 0;
};

struct StepContext {      // 一个时间步内所有积分点共享
    Real t_n = 0, dt = 0; // dt 允许为 0（纯弹性重平衡步）
    ProcessClock clock{};
    Kinematics kin = Kinematics::SmallStrain;
    bool need_tangent = true;
    bool need_dstress_dT = false; // Q8：接口保留，MVP 不实现；置 true 时 update 返回
                                  // StatusCode::NotImplemented（返回码，不抛异常）
    bool need_dissipation = false;
};

struct TemperatureValidity {
    bool enabled = false, clamp = false;
    Real lower = 0, upper = 0;
};

struct PointInput {
    Vec6 strain_n{}, strain_np1{}; // ε 或 E（全局坐标，Mandel，SI）
    const Mat3* F_np1 = nullptr;   // 有限应变时由适配层提供（为乘法分解模型预留）
    Real T_n = 0, T_np1 = 0;       // 温度 [K]
    bool override_T_sf = false;
    Real T_sf = 0;
    const Mat3* R = nullptr; // 全局→材料主轴的旋转（参考构型中定义）；nullptr = 重合
};

struct PointOutput {
    Vec6 stress{};          // σ 或 S（全局坐标，Mandel）
    Mat6 tangent{};         // 一致切线，与 stress 同一度量
    Vec6 thermal_strain{};  // ε_th(T_{n+1})
    Vec6 chemical_strain{}; // ε_ch(t_{n+1})；ε_eig = ε_th + ε_ch
    Vec6 dstress_dT{};      // 可选（Q8：MVP 不实现）
    Real dissipation = 0;   // 本步耗散 [J/m³]，可选
    Status status{};
};

// ---- 积分点状态：每个模型定长 Real 数组 + 字段表 ----
struct StateField {
    std::string_view name; // 如 "h_dev[3]"
    std::uint16_t offset, size;
    bool transferable; // 重分网时是否需要转移（派生量为 false）
};
struct StateLayout { // 主机侧元数据，不进入设备内核
    std::uint32_t n_reals = 0;
    std::span<const StateField> fields;
    std::uint64_t layout_hash = 0; // 模型 id + 版本 + 字段表 的哈希，restore 时校验
};

// Canonical FNV-1a: numeric metadata is encoded little endian, independent of host ABI.
inline std::uint64_t
state_layout_hash(std::string_view model, std::uint32_t count, std::span<const StateField> fields) {
    std::uint64_t h = 14695981039346656037ull;
    auto byte = [&](unsigned char b) {
        h ^= b;
        h *= 1099511628211ull;
    };
    auto number = [&](std::uint32_t n) {
        for (int i = 0; i < 4; ++i)
            byte(static_cast<unsigned char>(n >> (8 * i)));
    };
    for (unsigned char c : model)
        byte(c);
    byte(0);
    number(1);
    number(count);
    for (auto f : fields) {
        for (unsigned char c : f.name)
            byte(c);
        byte(0);
        number(f.offset);
        number(f.size);
        byte(f.transferable ? 1 : 0);
    }
    return h;
}

// ---- 单个积分点在 SoA 状态 View 中的视图（步长 = 点数 N），设备可用 ----
struct StateRef {
    Real* p;
    std::size_t stride;
    KOKKOS_INLINE_FUNCTION Real& operator[](int k) const { return p[k * stride]; }
};
struct ConstStateRef {
    const Real* p;
    std::size_t stride;
    KOKKOS_INLINE_FUNCTION Real operator[](int k) const { return p[k * stride]; }
};

// ---- 核心 concept：设备可调用的逐点内核（CPU/GPU 双路径硬性要求）----
template <class M>
concept MaterialModel = requires(const M m,
                                 const StepContext& ctx,
                                 const PointInput& in,
                                 ConstStateRef s_old,
                                 StateRef s_new,
                                 PointOutput& out,
                                 Kinematics k) {
    { m.layout() } -> std::same_as<StateLayout>;
    { m.init_state(s_new) } noexcept -> std::same_as<void>;
    // 纯函数，KOKKOS_INLINE_FUNCTION 的 const 成员：只读 s_old、只写 s_new 和
    // out；不得有虚调用、静态可变量、堆分配、异常
    {
        m.update(ctx, in, s_old, s_new, out)
    } noexcept -> std::same_as<StatusCode>; // 返回码与 out.status 一致
    { M::supports(k) } -> std::same_as<bool>;
    { M::tangent_symmetric } -> std::convertible_to<bool>;
};

} // namespace chipcae::material
