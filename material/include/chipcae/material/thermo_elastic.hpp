#pragma once
#include "chipcae/material/mandel.hpp"
#include "chipcae/material/thermal_strain.hpp"
namespace chipcae::material {
enum class ElasticSymmetry : std::uint8_t { Isotropic, TransverselyIsotropic, Orthotropic };
template <class MS>
class ThermoElastic {
public:
    static constexpr std::string_view model_id = "thermo_elastic@1";
    static constexpr bool tangent_symmetric = true;
    static constexpr bool supports(Kinematics) { return true; }
    ThermoElastic() = default;
    ThermoElastic(ElasticSymmetry symmetry,
                  const TemperatureTable& constants,
                  ThermalStrain<MS> thermal = {})
        : symmetry_(symmetry), constants_(constants), thermal_(thermal) {
        if (!constants.strictly_increasing())
            throw std::invalid_argument("elastic.table: duplicate temperatures");
        int expected = symmetry == ElasticSymmetry::Isotropic               ? 2
                       : symmetry == ElasticSymmetry::TransverselyIsotropic ? 5
                                                                            : 9;
        if (constants.num_columns() != static_cast<std::size_t>(expected))
            throw std::invalid_argument("elastic.table: column count");
    }
    StateLayout layout() const { return {0, {}, state_layout_hash(model_id, 0, {})}; }
    KOKKOS_INLINE_FUNCTION void init_state(StateRef) const noexcept {}
    KOKKOS_INLINE_FUNCTION StatusCode update(const StepContext& ctx,
                                             const PointInput& in,
                                             ConstStateRef,
                                             StateRef,
                                             PointOutput& out) const noexcept {
        out = PointOutput{};
        if (!Kokkos::isfinite(in.T_np1))
            return finish(out, StatusCode::Failed);
        if (ctx.need_dstress_dT)
            return finish(out, StatusCode::NotImplemented);
        if (!Kokkos::isfinite(ctx.dt) || ctx.dt < 0)
            return finish(out, StatusCode::Failed);
        Real p[9] = {1e9, 0.3};
        for (int i = 0; i < constants_.columns; ++i) {
            auto code = constants_.evaluate(i, in.T_np1, p[i]);
            if (code != StatusCode::Ok)
                return finish(out, code);
        }
        Vec6 th;
        auto code = in.override_T_sf ? thermal_.evaluate(in.T_np1, in.T_sf, th)
                                     : thermal_.evaluate(in.T_np1, th);
        if (code != StatusCode::Ok)
            return finish(out, code);
        return response(ctx, in, symmetry_, p, th, out);
    }
    // Device-side constituent access preserves each thermal convention and zero override.
    KOKKOS_INLINE_FUNCTION StatusCode isotropic_properties(const PointInput& in,
                                                           Real& E,
                                                           Real& nu) const noexcept {
        E = 1e9;
        nu = .3;
        if (constants_.columns == 0)
            return StatusCode::Ok;
        auto code = constants_.evaluate(0, in.T_np1, E);
        return code == StatusCode::Ok ? constants_.evaluate(1, in.T_np1, nu) : code;
    }
    bool isotropic() const noexcept {
        return symmetry_ == ElasticSymmetry::Isotropic && thermal_.isotropic();
    }
    KOKKOS_INLINE_FUNCTION static StatusCode response(const StepContext& ctx,
                                                      const PointInput& in,
                                                      ElasticSymmetry symmetry_,
                                                      const Real* p,
                                                      Vec6 th,
                                                      PointOutput& out) noexcept {
        Real E[3], nu[3], G[3];
        if (symmetry_ == ElasticSymmetry::Isotropic) {
            for (int i = 0; i < 3; ++i) {
                E[i] = p[0];
                nu[i] = p[1];
                G[i] = p[0] / (2 * (1 + p[1]));
            }
        } else if (symmetry_ == ElasticSymmetry::TransverselyIsotropic) {
            E[0] = E[1] = p[0];
            E[2] = p[1];
            nu[0] = p[2];
            nu[1] = nu[2] = p[3];
            G[0] = p[0] / (2 * (1 + p[2]));
            G[1] = G[2] = p[4];
        } else {
            for (int i = 0; i < 3; ++i) {
                E[i] = p[i];
                nu[i] = p[i + 3];
                G[i] = p[i + 6];
            }
        }
        Mat6 S{};
        for (int i = 0; i < 3; ++i) {
            if (!(E[i] > 0) || !(G[i] > 0))
                return finish(out, StatusCode::Failed);
            S[i * 6 + i] = 1 / E[i];
        }
        S[1] = S[6] = -nu[0] / E[0];
        S[2] = S[12] = -nu[1] / E[0];
        S[8] = S[13] = -nu[2] / E[1];
        S[21] = 1 / (2 * G[2]);
        S[28] = 1 / (2 * G[1]);
        S[35] = 1 / (2 * G[0]);
        // Cholesky inversion also rejects nonphysical compliance.
        Mat6 L{}, C{};
        for (int i = 0; i < 6; ++i)
            for (int j = 0; j <= i; ++j) {
                Real v = S[i * 6 + j];
                for (int k = 0; k < j; ++k)
                    v -= L[i * 6 + k] * L[j * 6 + k];
                if (i == j) {
                    if (!(v > 0) || !Kokkos::isfinite(v))
                        return finish(out, StatusCode::Failed);
                    L[i * 6 + j] = Kokkos::sqrt(v);
                } else
                    L[i * 6 + j] = v / L[j * 6 + j];
            }
        for (int col = 0; col < 6; ++col) {
            Real y[6]{}, x[6]{};
            for (int i = 0; i < 6; ++i) {
                Real v = i == col ? 1 : 0;
                for (int j = 0; j < i; ++j)
                    v -= L[i * 6 + j] * y[j];
                y[i] = v / L[i * 6 + i];
            }
            for (int i = 5; i >= 0; --i) {
                Real v = y[i];
                for (int j = i + 1; j < 6; ++j)
                    v -= L[j * 6 + i] * x[j];
                x[i] = v / L[i * 6 + i];
            }
            for (int i = 0; i < 6; ++i)
                C[i * 6 + col] = x[i];
        }
        if (in.R) {
            auto Q = mandel_rotation_matrix(*in.R);
            th = mat6_vec(mat6_transpose(Q), th);
            C = rotate_stiffness_global(C, *in.R);
        }
        out.thermal_strain = th;
        Vec6 e;
        for (int i = 0; i < 6; ++i)
            e[i] = in.strain_np1[i] - th[i];
        out.stress = mat6_vec(C, e);
        if (ctx.need_tangent)
            out.tangent = C;
        return finish(out, StatusCode::Ok);
    }
    KOKKOS_INLINE_FUNCTION static StatusCode finish(PointOutput& out, StatusCode code) {
        if (code == StatusCode::Ok) {
            for (Real v : out.stress)
                if (!Kokkos::isfinite(v))
                    code = StatusCode::Failed;
            for (Real v : out.tangent)
                if (!Kokkos::isfinite(v))
                    code = StatusCode::Failed;
        }
        out.status = {code, code == StatusCode::CutStep ? 0.5 : 1.0};
        return code;
    }

private:
    ElasticSymmetry symmetry_ = ElasticSymmetry::Isotropic;
    TableView<MS> constants_{};
    ThermalStrain<MS> thermal_{};
};
static_assert(MaterialModel<ThermoElastic<Kokkos::HostSpace>>);
} // namespace chipcae::material
