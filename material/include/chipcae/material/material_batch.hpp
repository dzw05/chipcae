// chipcae/material/material_batch.hpp —— 批量 SoA 接口 + 类型擦除外壳
#pragma once

#include "chipcae/material/material_point.hpp"

#include <Kokkos_Core.hpp>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace chipcae::material {

// SoA：Kokkos View，第 0 维是分量、第 1 维是积分点（LayoutRight：相邻线程访问相邻积分点，GPU
// 上合并访存） 内存空间由模板参数 MS 显式指定（Kokkos::HostSpace 或设备内存空间）
template <class T, class MS>
using Soa = Kokkos::View<T**, Kokkos::LayoutRight, MS>;
template <class T, class MS>
using Vec1 = Kokkos::View<T*, MS>;
// 设备上只读的参数（温度表、Prony 系数）：const + RandomAccess
template <class T, class MS>
using ParamView = Kokkos::View<const T*, MS, Kokkos::MemoryTraits<Kokkos::RandomAccess>>;

// 由内存空间推导用于启动 kernel 的执行空间（HostSpace → 默认主机执行空间）。
template <class MS>
struct ExecutionSpaceFor {
    using type = Kokkos::DefaultExecutionSpace;
};
template <>
struct ExecutionSpaceFor<Kokkos::HostSpace> {
    using type = Kokkos::DefaultHostExecutionSpace;
};
template <class MS>
using ExecSpaceFor = typename ExecutionSpaceFor<MS>::type;

template <class MS>
struct BatchInput {
    Soa<const Real, MS> strain_n, strain_np1; // [6][N]
    Vec1<const Real, MS> T_n, T_np1;          // [N]
    Soa<const Real, MS> R;                    // [9][N]，可为空 extents（表示与全局重合）
    Vec1<const Real, MS> point_param;         // [N], empty extents use PointInput default (φ = 0).
};
template <class MS>
struct BatchOutput {
    Soa<Real, MS> stress;          // [6][N]
    Soa<Real, MS> tangent;         // [36][N]，ctx.need_tangent=false 时可为空
    Soa<Real, MS> thermal_strain;  // [6][N]
    Soa<Real, MS> chemical_strain; // [6][N]，可为空
    Soa<Real, MS> dstress_dT;      // [6][N]，可为空（MVP 不实现）
    Vec1<Real, MS> dissipation;    // [N]，可为空
    Vec1<Status, MS> status;       // [N]
};

// 主机侧外壳（v0.2.2）：只用于按批分发、插件注册和 nanobind（Q10）入口；设备内核内不得出现虚调用。
// update_batch 内部按 ModelKind switch 到 MaterialAdapter<M, MS>，由它启动 Kokkos::parallel_for。
enum class ModelKind : std::uint8_t { ThermoElastic, PronyViscoelastic, CopperMixture };

template <class MS> // MS = Kokkos::HostSpace 或设备内存空间，各实例化一份
class IMaterial {
public:
    virtual ~IMaterial() = default;
    virtual std::string_view model_id() const = 0; // 如 "prony_viscoelastic@2"
    virtual std::string_view material_id() const { return model_id(); }
    virtual StateLayout layout() const = 0;
    virtual std::uint64_t parameter_hash() const { return 0; }
    virtual bool clamped_warning() const { return false; }
    virtual bool supports(Kinematics) const = 0;
    virtual bool tangent_symmetric() const = 0; // 决定求解器能否用对称求解器/AMG 选项
    // 本模型使用的近似（如 Q4 的常泊松比换算）；非空时求解日志与报告必须逐条标注“近似”
    virtual std::span<const std::string_view> approximation_notes() const = 0;
    virtual void init_state(Soa<Real, MS> state) const = 0; // [n_reals][N]
    virtual void update_batch(const StepContext& ctx,
                              const BatchInput<MS>& in,
                              Soa<const Real, MS> state_old,
                              Soa<Real, MS> state_new,
                              BatchOutput<MS>& out) const = 0; // 失败只体现在 out.status，不抛异常
};

// 主机侧：Kokkos::parallel_for 中逐点调用 M::update（设备内核，无虚调用）
template <MaterialModel M, class MS>
class MaterialAdapter final : public IMaterial<MS> {
public:
    explicit MaterialAdapter(M model,
                             std::uint64_t parameter_hash = 0,
                             TemperatureValidity validity = {},
                             std::string_view material_id = {})
        : model_(std::move(model)), parameter_hash_(parameter_hash), validity_(validity),
          material_id_(material_id.empty() ? M::model_id : material_id) {
        if constexpr (requires { model_.approximation_notes(); })
            notes_[0] = model_.approximation_notes();
    }

    std::string_view model_id() const override { return M::model_id; }
    std::string_view material_id() const override { return material_id_; }

    StateLayout layout() const override { return model_.layout(); }
    std::uint64_t parameter_hash() const override { return parameter_hash_; }
    bool clamped_warning() const override { return clamped_warning_; }

    bool supports(Kinematics k) const override { return M::supports(k); }

    bool tangent_symmetric() const override { return M::tangent_symmetric; }

    std::span<const std::string_view> approximation_notes() const override {
        return notes_[0].empty() ? std::span<const std::string_view>{}
                                 : std::span<const std::string_view>{notes_, 1};
    }

    void init_state(Soa<Real, MS> state) const override {
        const std::size_t n_points = state.extent(1);
        const std::size_t stride = state.stride(0);
        const M model = model_;
        Kokkos::parallel_for(
            "chipcae::material::init_state",
            Kokkos::RangePolicy<ExecSpaceFor<MS>, std::size_t>(0, n_points),
            KOKKOS_LAMBDA(const std::size_t j) {
                model.init_state(StateRef{state.extent(0) ? &state(0, j) : nullptr, stride});
            });
    }

    void update_batch(const StepContext& ctx,
                      const BatchInput<MS>& in,
                      Soa<const Real, MS> state_old,
                      Soa<Real, MS> state_new,
                      BatchOutput<MS>& out) const override {
        if (in.point_param.extent(0) != 0 && in.point_param.extent(0) != in.strain_np1.extent(1)) {
            Status failed{};
            failed.code = StatusCode::Failed;
            Kokkos::deep_copy(out.status, failed);
            return;
        }
        const auto validity = validity_;
        if (validity.enabled && validity.clamp) {
            auto a = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, in.T_n);
            auto b = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, in.T_np1);
            for (std::size_t j = 0; j < a.extent(0); ++j)
                if (a(j) < validity.lower || a(j) > validity.upper || b(j) < validity.lower ||
                    b(j) > validity.upper)
                    clamped_warning_ = true;
        }
        const std::size_t n_points = in.strain_np1.extent(1);
        const std::size_t state_stride_old = state_old.extent(1);
        const std::size_t state_stride_new = state_new.extent(1);
        const M model = model_;
        Kokkos::parallel_for(
            "chipcae::material::update_batch",
            Kokkos::RangePolicy<ExecSpaceFor<MS>, std::size_t>(0, n_points),
            KOKKOS_LAMBDA(const std::size_t j) {
                PointInput pin{};
                for (int i = 0; i < 6; ++i) {
                    pin.strain_n[i] = in.strain_n(i, j);
                    pin.strain_np1[i] = in.strain_np1(i, j);
                }
                pin.T_n = in.T_n(j);
                pin.T_np1 = in.T_np1(j);
                if (in.point_param.extent(0) != 0)
                    pin.point_param = in.point_param(j);
                Mat3 R{};
                if (in.R.extent(1) != 0) {
                    for (int i = 0; i < 9; ++i)
                        R[i] = in.R(i, j);
                    pin.R = &R;
                }
                PointOutput pout{};
                ConstStateRef s_old{state_old.extent(0) ? &state_old(0, j) : nullptr,
                                    state_stride_old};
                StateRef s_new{state_new.extent(0) ? &state_new(0, j) : nullptr, state_stride_new};
                bool outside =
                    validity.enabled && (pin.T_n < validity.lower || pin.T_n > validity.upper ||
                                         pin.T_np1 < validity.lower || pin.T_np1 > validity.upper);
                StatusCode code;
                if (outside && !validity.clamp) {
                    code = StatusCode::OutOfRange;
                    for (std::size_t c = 0; c < state_old.extent(0); ++c)
                        s_new[c] = s_old[c];
                } else {
                    if (outside) {
                        pin.T_n =
                            Kokkos::fmin(validity.upper, Kokkos::fmax(validity.lower, pin.T_n));
                        pin.T_np1 =
                            Kokkos::fmin(validity.upper, Kokkos::fmax(validity.lower, pin.T_np1));
                    }
                    code = model.update(ctx, pin, s_old, s_new, pout);
                }
                pout.status.code = code;
                for (int i = 0; i < 6; ++i) {
                    out.stress(i, j) = pout.stress[i];
                    out.thermal_strain(i, j) = pout.thermal_strain[i];
                    if (out.chemical_strain.extent(0) != 0)
                        out.chemical_strain(i, j) = pout.chemical_strain[i];
                    if (out.dstress_dT.extent(0) != 0)
                        out.dstress_dT(i, j) = pout.dstress_dT[i];
                }
                if (out.tangent.extent(0) != 0) {
                    for (int i = 0; i < 36; ++i)
                        out.tangent(i, j) = pout.tangent[i];
                }
                if (out.dissipation.extent(0) != 0)
                    out.dissipation(j) = pout.dissipation;
                out.status(j) = pout.status;
            });
    }

private:
    std::string_view notes_[1]{};
    M model_;
    std::uint64_t parameter_hash_ = 0;
    TemperatureValidity validity_{};
    std::string material_id_;
    mutable bool clamped_warning_ = false;
};

// 参数先经 Schema 校验（存储格式只含 SI），得到不可变的 MaterialParams，再构造模型
struct MaterialParams; // 见 chipcae/material/material_params.hpp
template <class MS> // model_id → ModelKind → switch 到 MaterialAdapter<M, MS>；参数 deep_copy 到 MS
                    // 中的只读 View
std::unique_ptr<IMaterial<MS>> make_material(const MaterialParams& validated);

// Runtime stack composition; options carry outer activation/eigenstrain and identity only.
// Component cards must be plain isotropic thermo_elastic@1 materials.
template <class MS>
std::unique_ptr<IMaterial<MS>> make_copper_mixture(const MaterialParams& matrix,
                                                   const MaterialParams& copper,
                                                   const MaterialParams& options);

} // namespace chipcae::material
