// chipcae/material/activation.hpp —— 积分点激活 / 移除（N6，MVP-b FOWLP）
#pragma once

#include "chipcae/material/material_batch.hpp"
#include "chipcae/material/material_point.hpp"
#include "chipcae/material/state_store.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace chipcae::material {

// 每个积分点的激活记录：QuadratureStateStore 中所有材料共用的前缀字段（9 个 Real，可转移）
struct ActivationRecord {
    Real active = 0;       // 1 = 参与组装；0 = 未激活或已移除
    Real T_act = 0;        // 激活参考温度 [K]，即热应变零点（固化或沉积温度）
    Vec6 strain_ref{};     // 激活时刻的 ε（有限应变时为 E），Mandel
    Real step_id_act = -1; // 激活所在工艺步，用于重启校验与诊断
};

inline constexpr std::uint16_t kActivationRecordReals = 9;

enum class InactiveMode : std::uint8_t {
    Excluded,     // MVP 默认：从组装排除，本构不被调用
    SoftStiffness // 选项：类似 Ansys EKILL 的软化刚度，便于对比
};

struct ActivationEvent {
    std::uint32_t step_id; // 该层沉积或塑封工艺步
    Real T_act;            // 来自工艺配方 [K]
};

// Solver 在该工艺步的第一个增量调用；写入 trial，随步 commit / discard（被拒的步连同激活一起回滚）
template <class MS>
void activate(QuadratureStateStore<MS>& store,
              std::span<const std::size_t> points,
              const ActivationEvent& ev,
              Soa<const Real, MS> current_strain) {
    auto trial = store.trial();
    Kokkos::parallel_for(
        "chipcae::material::activate",
        Kokkos::RangePolicy<ExecSpaceFor<MS>, std::size_t>(0, points.size()),
        KOKKOS_LAMBDA(const std::size_t i) {
            const std::size_t j = points[i];
            trial(0, j) = 1.0;
            trial(1, j) = ev.T_act;
            for (int c = 0; c < 6; ++c)
                trial(2 + c, j) = current_strain(c, j);
            trial(8, j) = static_cast<Real>(ev.step_id);
        });
}

template <class MS>
void deactivate(QuadratureStateStore<MS>& store, std::span<const std::size_t> points) {
    auto trial = store.trial();
    Kokkos::parallel_for(
        "chipcae::material::deactivate",
        Kokkos::RangePolicy<ExecSpaceFor<MS>, std::size_t>(0, points.size()),
        KOKKOS_LAMBDA(const std::size_t i) { trial(0, points[i]) = 0.0; });
}

// 组合器：材料内核看到 strain − strain_ref，热应变零点取 T_act；自身满足 MaterialModel
template <MaterialModel M>
class WithActivation {
public:
    static constexpr bool tangent_symmetric = M::tangent_symmetric;

    static constexpr bool supports(Kinematics k) { return M::supports(k); }

    WithActivation() = default;
    explicit WithActivation(M model) : model_(std::move(model)) {}

    StateLayout layout() const {
        StateLayout base = model_.layout();
        StateLayout out = base;
        out.n_reals = base.n_reals + kActivationRecordReals;
        out.fields = {};
        // TODO(K3): 前缀 ActivationRecord 字段表 + 重算 layout_hash（CHUNK 3）。
        return out;
    }

    void init_state(StateRef s) const noexcept {
        for (std::uint16_t k = 0; k < kActivationRecordReals; ++k)
            s[k] = 0.0;
        s[0] = 1.0; // D5：active 初值 1
        model_.init_state(StateRef{s.p + kActivationRecordReals * s.stride, s.stride});
    }

    StatusCode update(const StepContext&,
                      const PointInput&,
                      ConstStateRef,
                      StateRef,
                      PointOutput& out) const noexcept {
        out = PointOutput{};
        out.status = Status{StatusCode::NotImplemented, 1.0};
        return StatusCode::NotImplemented; // TODO(K3)
    }

private:
    M model_{};
};

} // namespace chipcae::material
