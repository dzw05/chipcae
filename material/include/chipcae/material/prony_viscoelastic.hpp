#pragma once
#include "chipcae/material/shift.hpp"
#include "chipcae/material/thermo_elastic.hpp"

#include <optional>
namespace chipcae::material {
enum class VolumetricMode : std::uint8_t { ElasticBulk, ConstantPoissonFromE };
struct PronyTerm {
    Real G = 0, tau = 0;
};
struct PronyHostParams {
    std::optional<VolumetricMode> mode;
    Real infinity = 0, poisson = 0.3;
    std::vector<PronyTerm> terms;
    PronyHostParams() = default;
    explicit PronyHostParams(VolumetricMode volumetric_mode,
                             Real infinity = 0,
                             Real poisson = 0.3,
                             std::vector<PronyTerm> terms = {})
        : mode(volumetric_mode), infinity(infinity), poisson(poisson), terms(std::move(terms)) {}
};
// Immutable parameter snapshot in the explicitly selected memory space.
template <class MS>
struct PronyParams {
    VolumetricMode mode_{};
    int n_;
    Real ginf_, ratio_ = 0;
    Kokkos::View<const Real*, MS, Kokkos::MemoryTraits<Kokkos::RandomAccess>> g_, tau_;
    explicit PronyParams(const PronyHostParams& p) : n_(p.terms.size()), ginf_(p.infinity) {
        if (!p.mode)
            throw std::invalid_argument("viscoelastic.volumetric_mode: required");
        mode_ = *p.mode;
        if (n_ > 9000)
            throw std::invalid_argument("too many Prony branches for StateField offsets");
        if (!(p.infinity >= 0) || !std::isfinite(p.infinity))
            throw std::invalid_argument("Prony infinity must be nonnegative");
        if (mode_ == VolumetricMode::ConstantPoissonFromE) {
            if (!(p.poisson > -1 && p.poisson < 0.5))
                throw std::invalid_argument("poisson_ratio out of range");
            ratio_ = 2 * (1 + p.poisson) / (3 * (1 - 2 * p.poisson));
            ginf_ /= 2 * (1 + p.poisson);
        }
        Kokkos::View<Real*, MS> data("Prony_parameters", 2 * n_);
        auto host = Kokkos::create_mirror_view(data);
        for (int i = 0; i < n_; ++i) {
            if (!(p.terms[i].G >= 0) || !(p.terms[i].tau > 0) || !std::isfinite(p.terms[i].G) ||
                !std::isfinite(p.terms[i].tau))
                throw std::invalid_argument("invalid Prony branch");
            host(i) = p.terms[i].G /
                      (mode_ == VolumetricMode::ConstantPoissonFromE ? 2 * (1 + p.poisson) : 1);
            host(n_ + i) = p.terms[i].tau;
        }
        Kokkos::deep_copy(data, host);
        g_ = Kokkos::subview(data, std::make_pair(0, n_));
        tau_ = Kokkos::subview(data, std::make_pair(n_, 2 * n_));
    }
};
template <class MS>
class PronyViscoelastic {
public:
    static constexpr std::string_view model_id = "prony_viscoelastic@1";
    static constexpr bool tangent_symmetric = true;
    static constexpr bool supports(Kinematics) { return true; }
    PronyViscoelastic() = default;
    PronyViscoelastic(const PronyParams<MS>& p,
                      const TemperatureTable& bulk,
                      ShiftFunction<MS> shift = {},
                      ThermalStrain<MS> thermal = {})
        : mode_(p.mode_), n_(p.n_), ginf_(p.ginf_), ratio_(p.ratio_), g_(p.g_), tau_(p.tau_),
          shift_(shift), thermal_(thermal), bulk_(bulk) {
        if (!bulk.strictly_increasing())
            throw std::invalid_argument("bulk_modulus: duplicate temperatures");
        fields_[0] = {"h_dev", 0, static_cast<std::uint16_t>(6 * n_), true};
        fields_[1] = {"e_dev_n", static_cast<std::uint16_t>(6 * n_), 6, true};
        fields_[2] = {"theta_n", static_cast<std::uint16_t>(6 * n_ + 6), 1, true};
        fields_[3] = {"xi_n", static_cast<std::uint16_t>(6 * n_ + 7), 1, false};
        fields_[4] = {
            "q_vol", static_cast<std::uint16_t>(6 * n_ + 8), static_cast<std::uint16_t>(n_), true};
    }
    PronyViscoelastic(const PronyHostParams& p,
                      const TemperatureTable& bulk,
                      ShiftFunction<MS> shift = {},
                      ThermalStrain<MS> thermal = {})
        : PronyViscoelastic(PronyParams<MS>{p}, bulk, shift, thermal) {}
    std::string_view approximation_notes() const {
        return mode_ == VolumetricMode::ConstantPoissonFromE ? "近似：常泊松比换算" : "";
    }
    StateLayout layout() const {
        StateLayout l{};
        l.n_reals = (mode_ == VolumetricMode::ElasticBulk ? 6 : 7) * n_ + 8;
        l.fields = std::span<const StateField>(fields_.data(),
                                               mode_ == VolumetricMode::ElasticBulk ? 4 : 5);
        l.layout_hash = state_layout_hash(model_id, l.n_reals, l.fields);
        return l;
    }
    KOKKOS_INLINE_FUNCTION void init_state(StateRef s) const noexcept {
        for (int i = 0; i < (mode_ == VolumetricMode::ElasticBulk ? 6 : 7) * n_ + 8; ++i)
            s[i] = 0;
    }
    KOKKOS_INLINE_FUNCTION StatusCode update(const StepContext& ctx,
                                             const PointInput& in,
                                             ConstStateRef old,
                                             StateRef next,
                                             PointOutput& out) const noexcept {
        out = PointOutput{};
        if (ctx.need_dstress_dT)
            return ThermoElastic<MS>::finish(out, StatusCode::NotImplemented);
        Real dxi;
        auto code = shift_.reduced_time_increment(in.T_n, in.T_np1, ctx.dt, dxi);
        if (code != StatusCode::Ok)
            return ThermoElastic<MS>::finish(out, code);
        Vec6 th;
        code = in.override_T_sf ? thermal_.evaluate(in.T_np1, in.T_sf, th)
                                : thermal_.evaluate(in.T_np1, th);
        if (code != StatusCode::Ok)
            return ThermoElastic<MS>::finish(out, code);
        Vec6 e = in.strain_np1;
        if (in.R)
            e = rotate_mandel(e, *in.R);
        for (int i = 0; i < 6; ++i)
            e[i] -= th[i];
        Real theta = mandel_trace(e);
        e = mandel_dev(e);
        const int off = 6 * n_;
        Real ghat = ginf_, khat = ginf_ * ratio_, pressure = khat * theta;
        if (mode_ == VolumetricMode::ElasticBulk) {
            code = bulk_.evaluate(0, in.T_np1, khat);
            if (code != StatusCode::Ok || !(khat > 0))
                return ThermoElastic<MS>::finish(
                    out, code == StatusCode::Ok ? StatusCode::Failed : code);
            pressure = khat * theta;
        }
        Vec6 stress{};
        for (int k = 0; k < 6; ++k)
            stress[k] = 2 * ginf_ * e[k];
        Real rate0 = 0, rate1 = 0;
        for (int i = 0; i < n_; ++i) {
            Real x = dxi / tau_(i), decay = x > 700 ? 0 : Kokkos::exp(-x);
            Real g = x < 1e-4  ? 1 - x / 2 + x * x / 6 - x * x * x / 24
                     : x > 700 ? 1 / x
                               : -Kokkos::expm1(-x) / x;
            ghat += g_(i) * g;
            for (int k = 0; k < 6; ++k) {
                Real h = decay * old[6 * i + k] + 2 * g_(i) * g * (e[k] - old[off + k]);
                next[6 * i + k] = h;
                stress[k] += h;
                if (g_(i) > 0) {
                    rate0 += old[6 * i + k] * old[6 * i + k] / (2 * g_(i) * tau_(i));
                    rate1 += h * h / (2 * g_(i) * tau_(i));
                }
            }
            if (mode_ == VolumetricMode::ConstantPoissonFromE) {
                int j = off + 8 + i;
                Real Ki = g_(i) * ratio_;
                next[j] = decay * old[j] + Ki * g * (theta - old[off + 6]);
                pressure += next[j];
                khat += Ki * g;
                if (Ki > 0) {
                    rate0 += old[j] * old[j] / (Ki * tau_(i));
                    rate1 += next[j] * next[j] / (Ki * tau_(i));
                }
            }
        }
        for (int k = 0; k < 6; ++k) {
            next[off + k] = e[k];
            if (k < 3)
                stress[k] += pressure;
        }
        next[off + 6] = theta;
        next[off + 7] = old[off + 7] + dxi;
        Mat6 C{};
        for (int i = 0; i < 6; ++i)
            for (int j = 0; j < 6; ++j)
                C[i * 6 + j] = 2 * ghat * ((i == j ? 1.0 : 0.0) - (i < 3 && j < 3 ? 1.0 / 3 : 0)) +
                               (i < 3 && j < 3 ? khat : 0);
        if (ctx.need_dissipation) {
            Real l0, l1;
            code = shift_.evaluate(in.T_n, l0);
            if (code != StatusCode::Ok)
                return ThermoElastic<MS>::finish(out, code);
            code = shift_.evaluate(in.T_np1, l1);
            if (code != StatusCode::Ok)
                return ThermoElastic<MS>::finish(out, code);
            out.dissipation =
                ctx.dt * 0.5 * (rate0 * Kokkos::pow(10., -l0) + rate1 * Kokkos::pow(10., -l1));
            if (!Kokkos::isfinite(out.dissipation))
                return ThermoElastic<MS>::finish(out, StatusCode::Failed);
        }
        if (in.R) {
            Mat6 Q = mandel_rotation_matrix(*in.R);
            stress = mat6_vec(mat6_transpose(Q), stress);
            th = mat6_vec(mat6_transpose(Q), th);
            C = rotate_stiffness_global(C, *in.R);
        }
        out.stress = stress;
        out.thermal_strain = th;
        if (ctx.need_tangent)
            out.tangent = C;
        for (int k = 0; k < (mode_ == VolumetricMode::ElasticBulk ? 6 : 7) * n_ + 8; ++k)
            if (!Kokkos::isfinite(next[k]))
                return ThermoElastic<MS>::finish(out, StatusCode::Failed);
        return ThermoElastic<MS>::finish(out, StatusCode::Ok);
    }

private:
    Kokkos::Array<StateField, 5> fields_{};
    VolumetricMode mode_ = VolumetricMode::ElasticBulk;
    int n_ = 0;
    Real ginf_ = 0, ratio_ = 0;
    Kokkos::View<const Real*, MS, Kokkos::MemoryTraits<Kokkos::RandomAccess>> g_, tau_;
    ShiftFunction<MS> shift_{};
    ThermalStrain<MS> thermal_{};
    TableView<MS> bulk_{};
};
static_assert(MaterialModel<PronyViscoelastic<Kokkos::HostSpace>>);
} // namespace chipcae::material
