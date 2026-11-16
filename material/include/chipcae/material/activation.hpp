// chipcae/material/activation.hpp —— 积分点激活 / 移除（N6，MVP-b FOWLP）
#pragma once

#include "chipcae/material/material_batch.hpp"
#include "chipcae/material/material_point.hpp"
#include "chipcae/material/state_store.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace chipcae::material {

// 每个积分点的激活记录：QuadratureStateStore 中所有材料共用的前缀字段（9 个 Real，可转移）
struct ActivationRecord {
    Real active = 1;       // 1 = 参与组装；0 = 未激活或已移除
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
    store.prepare_events();
    for (auto point : points)
        if (point >= store.n_points())
            throw std::out_of_range("activation.points: out of range");
    Vec1<std::size_t, MS> indices("activation_indices", points.size());
    auto host = Kokkos::create_mirror_view(indices);
    for (std::size_t i = 0; i < points.size(); ++i)
        host(i) = points[i];
    Kokkos::deep_copy(indices, host);
    auto trial = store.trial();
    Kokkos::parallel_for(
        "chipcae::material::activate",
        Kokkos::RangePolicy<ExecSpaceFor<MS>, std::size_t>(0, points.size()),
        KOKKOS_LAMBDA(const std::size_t i) {
            const std::size_t j = indices(i);
            trial(0, j) = 1.0;
            trial(1, j) = ev.T_act;
            for (unsigned c = 9; c < trial.extent(0); ++c)
                trial(c, j) = 0;
            for (int c = 0; c < 6; ++c)
                trial(2 + c, j) = current_strain(c, j);
            trial(8, j) = static_cast<Real>(ev.step_id);
        });
    Kokkos::fence();
}

template <class MS>
void deactivate(QuadratureStateStore<MS>& store, std::span<const std::size_t> points) {
    store.prepare_events();
    for (auto point : points)
        if (point >= store.n_points())
            throw std::out_of_range("activation.points: out of range");
    Vec1<std::size_t, MS> indices("activation_indices", points.size());
    auto host = Kokkos::create_mirror_view(indices);
    for (std::size_t i = 0; i < points.size(); ++i)
        host(i) = points[i];
    Kokkos::deep_copy(indices, host);
    auto trial = store.trial();
    Kokkos::parallel_for(
        "chipcae::material::deactivate",
        Kokkos::RangePolicy<ExecSpaceFor<MS>, std::size_t>(0, points.size()),
        KOKKOS_LAMBDA(const std::size_t i) { trial(0, indices(i)) = 0.0; });
    Kokkos::fence();
}

// 组合器：材料内核看到 strain − strain_ref，热应变零点取 T_act；自身满足 MaterialModel
template <MaterialModel M, bool Enabled = true>
class WithActivation {
public:
    static constexpr bool tangent_symmetric = M::tangent_symmetric;

    static constexpr bool supports(Kinematics k) { return M::supports(k); }

    static constexpr bool activation_prefix = true;
    static constexpr auto model_id = M::model_id;
    WithActivation() : WithActivation(M{}) {}
    explicit WithActivation(M model, InactiveMode mode = InactiveMode::Excluded, Real scale = 1e-6)
        : model_(std::move(model)), mode_(mode), scale_(scale) {
        if (!(scale > 0) || !std::isfinite(scale))
            throw std::invalid_argument(
                "activation.soft_stiffness_scale: must be positive and finite");
        auto b = model_.layout();
        count_ = b.n_reals + 9;
        hash_ = (b.layout_hash ^ 0x6163746976617465ull) * 1099511628211ull;
        fields_[0] = {"active", 0, 1, true};
        fields_[1] = {"T_act", 1, 1, true};
        fields_[2] = {"strain_ref", 2, 6, true};
        fields_[3] = {"step_id_act", 8, 1, true};
        if (b.fields.size() > 60 || count_ > 65535)
            throw std::invalid_argument("activation.layout: metadata capacity exceeded");
        for (auto f : b.fields) {
            f.offset += 9;
            fields_[nf_++] = f;
        }
        hash_ = state_layout_hash(model_id, count_, {fields_.data(), nf_});
    }
    StateLayout layout() const { return {count_, {fields_.data(), nf_}, hash_}; }
    std::string_view approximation_notes() const {
        if constexpr (requires { model_.approximation_notes(); })
            return model_.approximation_notes();
        return {};
    }
    KOKKOS_INLINE_FUNCTION void init_state(StateRef s) const noexcept {
        for (unsigned i = 0; i < count_; ++i)
            s[i] = 0;
        s[0] = 1;
        s[8] = -1;
        model_.init_state({s.p + 9 * s.stride, s.stride});
    }
    KOKKOS_INLINE_FUNCTION StatusCode update(const StepContext& ctx,
                                             const PointInput& in,
                                             ConstStateRef old,
                                             StateRef next,
                                             PointOutput& out) const noexcept {
        for (unsigned i = 0; i < count_; ++i)
            next[i] = old[i];
        out = {};
        if constexpr (!Enabled)
            return model_.update(ctx,
                                 in,
                                 {old.p + 9 * old.stride, old.stride},
                                 {next.p + 9 * next.stride, next.stride},
                                 out);
        if (old[0] == 0) {
            if (mode_ == InactiveMode::SoftStiffness) {
                PointInput zero = in;
                zero.strain_n = {};
                zero.strain_np1 = {};
                // Evaluate instantaneous tangent in trial, then restore the frozen state.

                StepContext instant = ctx;
                instant.dt = 0;
                instant.need_tangent = true;
                auto code = model_.update(instant,
                                          zero,
                                          {old.p + 9 * old.stride, old.stride},
                                          {next.p + 9 * next.stride, next.stride},
                                          out);
                for (auto& v : out.tangent)
                    v *= scale_;
                out.stress = {};
                for (unsigned i = 0; i < count_; ++i)
                    next[i] = old[i];
                return code;
            }
            return StatusCode::Ok;
        }
        PointInput adjusted = in;
        for (int i = 0; i < 6; ++i) {
            adjusted.strain_n[i] -= old[2 + i];
            adjusted.strain_np1[i] -= old[2 + i];
        }
        if (old[8] >= 0) {
            adjusted.override_T_sf = true;
            adjusted.T_sf = old[1];
        }
        return model_.update(ctx,
                             adjusted,
                             {old.p + 9 * old.stride, old.stride},
                             {next.p + 9 * next.stride, next.stride},
                             out);
    }

private:
    M model_;
    InactiveMode mode_;
    Real scale_;
    unsigned count_ = 9, nf_ = 4;
    std::uint64_t hash_ = 0;
    Kokkos::Array<StateField, 64> fields_{};
};

} // namespace chipcae::material
