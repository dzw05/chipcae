// chipcae/material/eigenstrain.hpp —— 叠加本征应变（P0：等效化学收缩）
#pragma once

#include "chipcae/material/material_point.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

namespace chipcae::material {

// 本征应变源：给出 ε_e(t_{n+1}) 及其对温度的导数；自身可带状态
template <class E>
concept EigenstrainSource = requires(const E e,
                                     const StepContext& ctx,
                                     const PointInput& in,
                                     ConstStateRef s_old,
                                     StateRef s_new,
                                     Vec6& eps_n,
                                     Vec6& eps_np1,
                                     Vec6& deps_dT) {
    { e.layout() } -> std::same_as<StateLayout>;
    { e.update(ctx, in, s_old, s_new, eps_n, eps_np1, deps_dT) } noexcept -> std::same_as<void>;
};

// 组合器：自身也满足 MaterialModel。热应变仍由 M 内部按 α(T) 计算；
// 其余本征应变（化学收缩等）由组合器先从总应变中扣除，再交给 M：
//   strain_n'   = strain_n   − Σ ε_e(t_n)
//   strain_np1' = strain_np1 − Σ ε_e(t_{n+1})
// 状态布局 = [ M 的状态 | E₁ 的状态 | ... ]；out.chemical_strain = Σ ε_e(t_{n+1})
template <MaterialModel M, EigenstrainSource... E>
class WithEigenstrains {
public:
    static constexpr bool tangent_symmetric = M::tangent_symmetric;

    static constexpr bool supports(Kinematics k) { return M::supports(k); }

    WithEigenstrains(M model, E... sources)
        : model_(std::move(model)), sources_(std::move(sources)...) {}

    StateLayout layout() const {
        StateLayout base = model_.layout();
        StateLayout out = base;
        out.n_reals = base.n_reals + sum_source_reals(std::index_sequence_for<E...>{});
        out.fields = {};
        // TODO(K3): 拼接字段表并重算 layout_hash（CHUNK 2+）。
        return out;
    }

    void init_state(StateRef s) const noexcept {
        model_.init_state(s);
        // TODO(K3): 初始化各本征应变源状态（CHUNK 3）。
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
    template <std::size_t... I>
    std::uint32_t sum_source_reals(std::index_sequence<I...>) const {
        std::uint32_t total = 0;
        ((total += std::get<I>(sources_).layout().n_reals), ...);
        return total;
    }

    M model_;
    std::tuple<E...> sources_;
};

// ---- P0：简化固化收缩（等效各向同性化学收缩应变）----
class ShrinkageSchedule { // 由 Schema 构造，不可变；只在 cure_step_ids（P0 校验恰好一项）
                          // 中唯一 id 所指固化步内（由 ProcessClock.step_id
                          // 判断）按线性斜坡（默认）或表格施加 s_eff
public:
    // 返回该时刻累计体积收缩率 s_ch ≥ 0（正值表示收缩），单调不减
    Real shrinkage(const ProcessClock&, bool) const noexcept {
        return 0.0; // TODO(K3): 固化步内线性斜坡 / 表格（CHUNK 3）。
    }
};

struct EquivalentCureShrinkage { // 满足 EigenstrainSource
    ShrinkageSchedule schedule;

    StateLayout layout() const {
        StateLayout out{};
        out.n_reals = 1;
        out.fields = std::span<const StateField>(fields_, 1);
        return out;
    }
    // eps_np1 = (β_ch/3)·m，β_ch = −s_ch(t_{n+1})，m = (1,1,1,0,0,0)；eps_n 由 s_old 中的 s_applied
    // 给出 deps_dT = 0（P0 不依赖温度）；s_new[s_applied] = s_ch(t_{n+1})
    void update(const StepContext&,
                const PointInput&,
                ConstStateRef,
                StateRef s_new,
                Vec6& eps_n,
                Vec6& eps_np1,
                Vec6& deps_dT) const noexcept {
        eps_n = Vec6{};
        eps_np1 = Vec6{};
        deps_dT = Vec6{};
        s_new[0] = 0.0; // TODO(K3)
    }

private:
    inline static const StateField fields_[1] = {{"s_applied", 0, 1, true}};
};

// ---- P1 占位：完整固化动力学（Kamal–Sourour 型），状态为转化度 α_c，依赖温度 ----
struct CureKineticsShrinkage; // 未实现；接口同 EigenstrainSource

} // namespace chipcae::material
