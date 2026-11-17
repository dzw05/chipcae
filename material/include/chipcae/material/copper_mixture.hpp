#pragma once
#include "chipcae/material/thermo_elastic.hpp"

namespace chipcae::material {
// Engineering laminate rules in the material frame: z is the layer normal.
// No history; φ is supplied at each integration point, never stored in the card.
template <class MS>
class CopperMixture {
public:
    static constexpr std::string_view model_id = "copper_mixture@1";
    static constexpr bool tangent_symmetric = true;
    static constexpr bool supports(Kinematics k) { return ThermoElastic<MS>::supports(k); }
    CopperMixture(ThermoElastic<MS> matrix, ThermoElastic<MS> copper)
        : matrix_(matrix), copper_(copper) {
        if (!matrix.isotropic() || !copper.isotropic())
            throw std::invalid_argument("copper mixture requires isotropic thermo_elastic@1 "
                                        "components; use explicit *_eq materials");
    }
    StateLayout layout() const { return {0, {}, state_layout_hash(model_id, 0, {})}; }
    KOKKOS_INLINE_FUNCTION void init_state(StateRef) const noexcept {}
    KOKKOS_INLINE_FUNCTION StatusCode update(const StepContext& ctx,
                                             const PointInput& in,
                                             ConstStateRef old,
                                             StateRef next,
                                             PointOutput& out) const noexcept {
        out = {};
        const Real f = in.point_param, m = 1 - f;
        if (!(f >= 0 && f <= 1))
            return ThermoElastic<MS>::finish(out, StatusCode::Failed);
        // Exact isotropic limits, including rotation and thermal-zero overrides.
        if (f == 0)
            return matrix_.update(ctx, in, old, next, out);
        if (f == 1)
            return copper_.update(ctx, in, old, next, out);
        PointInput local = in;
        local.R = nullptr;
        PointOutput a{}, b{};
        auto code = matrix_.update(ctx, local, old, next, a);
        if (code != StatusCode::Ok)
            return ThermoElastic<MS>::finish(out, code);
        code = copper_.update(ctx, local, old, next, b);
        if (code != StatusCode::Ok)
            return ThermoElastic<MS>::finish(out, code);
        Real Em, nm, Ec, nc;
        code = matrix_.isotropic_properties(in, Em, nm);
        if (code != StatusCode::Ok)
            return ThermoElastic<MS>::finish(out, code);
        code = copper_.isotropic_properties(in, Ec, nc);
        if (code != StatusCode::Ok)
            return ThermoElastic<MS>::finish(out, code);
        const Real Gm = Em / (2 * (1 + nm)), Gc = Ec / (2 * (1 + nc));
        const Real E = f * Ec + m * Em, n = f * nc + m * nm;
        const Real G = f * Gc + m * Gm, Gz = 1 / (f / Gc + m / Gm);
        const Real p[9] = {E, E, 1 / (f / Ec + m / Em), n, n, n, G, Gz, Gz};
        Vec6 th{};
        th[0] = th[1] = (f * Ec * b.thermal_strain[0] + m * Em * a.thermal_strain[0]) / E;
        th[2] = f * (1 + nc) * b.thermal_strain[0] + m * (1 + nm) * a.thermal_strain[0] - n * th[0];
        return ThermoElastic<MS>::response(ctx, in, ElasticSymmetry::Orthotropic, p, th, out);
    }

private:
    ThermoElastic<MS> matrix_, copper_;
};
static_assert(MaterialModel<CopperMixture<Kokkos::HostSpace>>);
} // namespace chipcae::material
