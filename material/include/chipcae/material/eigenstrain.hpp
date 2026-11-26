// chipcae/material/eigenstrain.hpp —— 叠加本征应变（P0：等效化学收缩）
#pragma once

#include "chipcae/material/material_point.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
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
// Host binding of string process ids to the uint32 ProcessClock ABI.
inline std::uint32_t process_step_id(std::string_view name) {
    std::uint64_t h = 14695981039346656037ull;
    for (unsigned char c : name) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return static_cast<std::uint32_t>(h);
}
// Schedule times are relative to the selected process step. Step ids must be ordered.
class ShrinkageSchedule {
public:
    ShrinkageSchedule() = default;
    ShrinkageSchedule(Real effective,
                      std::uint32_t step,
                      Real start,
                      Real end,
                      std::span<const Real> times = {},
                      std::span<const Real> fractions = {},
                      bool ordered_steps = true)
        : effective_(effective), start_(start), end_(end), step_(step), n_(times.size()),
          ordered_(ordered_steps) {
        if (!(effective >= 0 && effective < 1) || !std::isfinite(start) || !std::isfinite(end) ||
            !(end > start))
            throw std::invalid_argument("chemical_shrinkage: invalid s_eff or schedule interval");
        if (n_ > 256 || static_cast<std::size_t>(n_) != fractions.size() ||
            (n_ && (n_ < 2 || times.front() != 0 || times.back() != 1 || fractions.front() != 0 ||
                    fractions.back() != 1)))
            throw std::invalid_argument(
                "chemical_shrinkage.schedule.table: endpoints must be 0 and 1");
        for (int i = 0; i < n_; ++i) {
            if (!std::isfinite(times[i]) || !std::isfinite(fractions[i]) ||
                (i && (times[i] <= times[i - 1] || fractions[i] < fractions[i - 1])))
                throw std::invalid_argument(
                    "chemical_shrinkage.schedule.table: monotone values required");
            times_[i] = times[i];
            fractions_[i] = fractions[i];
        }
    }
    // The process layer resolves card strings and supplies the actual step interval.
    ShrinkageSchedule bind(std::uint32_t step, Real start, Real end) const {
        if (!std::isfinite(start) || !std::isfinite(end) || !(end > start))
            throw std::invalid_argument("chemical_shrinkage.schedule: invalid process interval");
        auto bound = *this;
        bound.step_ = step;
        bound.start_ = start;
        bound.end_ = end;
        bound.ordered_ = true;
        return bound;
    }
    KOKKOS_INLINE_FUNCTION Real accumulated(const ProcessClock& clock,
                                            Real previous) const noexcept {
        return ordered_ || applies(clock) ? shrinkage(clock, true) : previous;
    }
    KOKKOS_INLINE_FUNCTION bool applies(const ProcessClock& clock) const noexcept {
        return clock.step_id == step_;
    }
    KOKKOS_INLINE_FUNCTION Real shrinkage(const ProcessClock& clock, bool next) const noexcept {
        if (clock.step_id < step_)
            return 0;
        if (clock.step_id > step_)
            return effective_;
        Real t = next ? clock.t_step_np1 : clock.t_step_n;
        Real u = (t - start_) / (end_ - start_);
        if (u <= 0)
            return 0;
        if (u >= 1)
            return effective_;
        if (!n_)
            return effective_ * u;
        int i = 0;
        while (i + 1 < n_ && times_[i + 1] < u)
            ++i;
        return effective_ * (fractions_[i] + (u - times_[i]) / (times_[i + 1] - times_[i]) *
                                                 (fractions_[i + 1] - fractions_[i]));
    }

private:
    Real effective_ = 0, start_ = 0, end_ = 1;
    std::uint32_t step_ = 0;
    int n_ = 0;
    bool ordered_ = true;
    Kokkos::Array<Real, 256> times_{}, fractions_{};
};
struct EquivalentCureShrinkage {
    ShrinkageSchedule schedule;
    StateLayout layout() const {
        return {1, {fields_, 1}, state_layout_hash("equivalent_cure_shrinkage@1", 1, {fields_, 1})};
    }
    KOKKOS_INLINE_FUNCTION void update(const StepContext& ctx,
                                       const PointInput&,
                                       ConstStateRef old,
                                       StateRef next,
                                       Vec6& en,
                                       Vec6& ep,
                                       Vec6& dT) const noexcept {
        en = {};
        ep = {};
        dT = {};
        next[0] = schedule.accumulated(ctx.clock, old[0]);
        for (int i = 0; i < 3; ++i) {
            en[i] = -old[0] / 3;
            ep[i] = -next[0] / 3;
        }
    }

private:
    inline static const StateField fields_[1] = {{"s_applied", 0, 1, true}};
};
static_assert(EigenstrainSource<EquivalentCureShrinkage>);

template <std::size_t I, class T>
struct SourceSlot {
    T value;
};
template <class Index, class... T>
struct SourcePack;
template <std::size_t... I, class... T>
struct SourcePack<std::index_sequence<I...>, T...> : SourceSlot<I, T>... {
    explicit SourcePack(T... values) : SourceSlot<I, T>{std::move(values)}... {}
};
template <std::size_t I, class T>
KOKKOS_INLINE_FUNCTION const T& source_get(const SourceSlot<I, T>& slot) noexcept {
    return slot.value;
}

template <MaterialModel M, EigenstrainSource... E>
class WithEigenstrains {
public:
    static constexpr auto model_id = M::model_id;
    static constexpr bool tangent_symmetric = M::tangent_symmetric;
    static constexpr bool supports(Kinematics k) { return M::supports(k); }
    WithEigenstrains(M m, E... e) : model_(std::move(m)), sources_(std::move(e)...) {
        auto b = model_.layout();
        count_ = b.n_reals;
        hash_ = b.layout_hash;
        if (b.fields.size() + sizeof...(E) > 64)
            throw std::invalid_argument("eigenstrain.layout: too many fields");
        for (auto f : b.fields)
            fields_[nf_++] = f;
        append(std::index_sequence_for<E...>{});
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
        model_.init_state(s);
    }
    KOKKOS_INLINE_FUNCTION StatusCode update(const StepContext& ctx,
                                             const PointInput& in,
                                             ConstStateRef old,
                                             StateRef next,
                                             PointOutput& out) const noexcept {
        PointInput adjusted = in;
        Vec6 total{};
        apply(ctx, in, old, next, adjusted, total, std::index_sequence_for<E...>{});
        auto code = model_.update(ctx, adjusted, old, next, out);
        out.chemical_strain = total;
        return code;
    }

private:
    template <std::size_t... I>
    void append(std::index_sequence<I...>) {
        (
            [&] {
                auto l = source_get<I>(sources_).layout();
                offsets_[I] = count_;
                if (nf_ + l.fields.size() > 64 || count_ + l.n_reals > 65535)
                    throw std::invalid_argument("eigenstrain.layout: metadata capacity exceeded");
                for (auto f : l.fields) {
                    f.offset += count_;
                    fields_[nf_++] = f;
                }
                count_ += l.n_reals;
                hash_ = (hash_ ^ l.layout_hash) * 1099511628211ull;
            }(),
            ...);
    }
    template <std::size_t... I>
    KOKKOS_INLINE_FUNCTION void apply(const StepContext& ctx,
                                      const PointInput& in,
                                      ConstStateRef old,
                                      StateRef next,
                                      PointInput& adjusted,
                                      Vec6& total,
                                      std::index_sequence<I...>) const noexcept {
        (
            [&] {
                Vec6 en{}, ep{}, dT{};
                auto off = offsets_[I];
                source_get<I>(sources_).update(ctx,
                                               in,
                                               {old.p + off * old.stride, old.stride},
                                               {next.p + off * next.stride, next.stride},
                                               en,
                                               ep,
                                               dT);
                for (int j = 0; j < 6; ++j) {
                    adjusted.strain_n[j] -= en[j];
                    adjusted.strain_np1[j] -= ep[j];
                    total[j] += ep[j];
                }
            }(),
            ...);
    }
    M model_;
    SourcePack<std::index_sequence_for<E...>, E...> sources_;
    Kokkos::Array<unsigned, sizeof...(E)> offsets_{};
    Kokkos::Array<StateField, 64> fields_{};
    unsigned count_ = 0, nf_ = 0;
    std::uint64_t hash_ = 0;
};

// ---- P1 占位：完整固化动力学（Kamal–Sourour 型），状态为转化度 α_c，依赖温度 ----
struct CureKineticsShrinkage; // 未实现；接口同 EigenstrainSource

} // namespace chipcae::material
