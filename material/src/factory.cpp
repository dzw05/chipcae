// chipcae/material/src/factory.cpp —— model_id → ModelKind → MaterialAdapter
#include "chipcae/material/activation.hpp"
#include "chipcae/material/copper_mixture.hpp"
#include "chipcae/material/material_batch.hpp"
#include "chipcae/material/material_params.hpp"
#include "chipcae/material/prony_viscoelastic.hpp"
#include "chipcae/material/thermo_elastic.hpp"

#include <memory>

namespace chipcae::material {

template <class MS, MaterialModel M>
std::unique_ptr<IMaterial<MS>> wrap(M m, const MaterialParams& p) {
    if (p.has_shrinkage) {
        using E = WithEigenstrains<M, EquivalentCureShrinkage>;
        static_assert(MaterialModel<E>);
        using A = WithActivation<E>;
        static_assert(MaterialModel<A>);
        if (!p.has_activation) {
            using P = WithActivation<E, false>;
            static_assert(MaterialModel<P>);
            return std::make_unique<MaterialAdapter<P, MS>>(
                P{E{m, EquivalentCureShrinkage{p.shrinkage}}},
                p.parameter_hash,
                p.fit_temperature,
                p.id);
        }
        return std::make_unique<MaterialAdapter<A, MS>>(
            A{E{m, EquivalentCureShrinkage{p.shrinkage}}},
            p.parameter_hash,
            p.fit_temperature,
            p.id);
    }
    using A = WithActivation<M>;
    static_assert(MaterialModel<A>);
    if (!p.has_activation) {
        using P = WithActivation<M, false>;
        static_assert(MaterialModel<P>);
        return std::make_unique<MaterialAdapter<P, MS>>(
            P{m}, p.parameter_hash, p.fit_temperature, p.id);
    }
    return std::make_unique<MaterialAdapter<A, MS>>(
        A{m}, p.parameter_hash, p.fit_temperature, p.id);
}
template <class MS>
std::unique_ptr<IMaterial<MS>> make_material(const MaterialParams& validated) {
    switch (validated.kind) {
    case ModelKind::ThermoElastic:
        return wrap<MS>(
            validated.elastic_table.num_temperatures()
                ? ThermoElastic<MS>{validated.elastic_symmetry,
                                    validated.elastic_table,
                                    ThermalStrain<MS>{validated.thermal, validated.cte_table}}
                : ThermoElastic<MS>{},
            validated);
    case ModelKind::PronyViscoelastic:
        if (!validated.prony.mode)
            throw std::invalid_argument("viscoelastic.volumetric_mode: required");
        return wrap<MS>(
            PronyViscoelastic<MS>{validated.prony,
                                  validated.bulk_table,
                                  ShiftFunction<MS>{validated.shift},
                                  ThermalStrain<MS>{validated.thermal, validated.cte_table}},
            validated);
    case ModelKind::CopperMixture:
        throw std::invalid_argument("copper mixture is runtime-only; use make_copper_mixture");
    }
    throw std::invalid_argument("unsupported material model");
}

template <class MS>
std::unique_ptr<IMaterial<MS>> make_copper_mixture(const MaterialParams& matrix,
                                                   const MaterialParams& copper,
                                                   const MaterialParams& options) {
    for (const auto* p : {&matrix, &copper}) {
        if (p->kind != ModelKind::ThermoElastic ||
            (!p->model_id.empty() && p->model_id != "thermo_elastic@1") ||
            p->elastic_symmetry != ElasticSymmetry::Isotropic ||
            p->thermal.symmetry != ThermalSymmetry::Isotropic)
            throw std::invalid_argument(
                "copper mixture requires isotropic thermo_elastic@1 components; use explicit *_eq "
                "materials (Prony is unsupported)");
        if (p->has_activation || p->has_shrinkage || p->fit_temperature.enabled)
            throw std::invalid_argument("copper mixture: put activation, shrinkage and temperature "
                                        "validity on the outer composition options");
    }
    auto component = [](const MaterialParams& p) {
        return p.elastic_table.num_temperatures()
                   ? ThermoElastic<MS>{p.elastic_symmetry,
                                       p.elastic_table,
                                       ThermalStrain<MS>{p.thermal, p.cte_table}}
                   : ThermoElastic<MS>{};
    };
    // Include both constituent identities in the runtime parameter identity.
    auto p = options;
    for (auto h : {matrix.parameter_hash, copper.parameter_hash})
        for (int i = 0; i < 8; ++i) {
            p.parameter_hash ^= static_cast<unsigned char>(h >> (8 * i));
            p.parameter_hash *= 1099511628211ull;
        }
    return wrap<MS>(CopperMixture<MS>{component(matrix), component(copper)}, p);
}

template <class MS, bool Distinct = !std::is_same_v<MS, Kokkos::HostSpace>>
struct InstantiateDefault {};
template <class MS>
struct InstantiateDefault<MS, true> {
    using Factory = std::unique_ptr<IMaterial<MS>> (*)(const MaterialParams&);
    static Factory const fn;
    using MixtureFactory = std::unique_ptr<IMaterial<MS>> (*)(const MaterialParams&,
                                                              const MaterialParams&,
                                                              const MaterialParams&);
    static MixtureFactory const mixture_fn;
    static std::unique_ptr<IMaterial<MS>> create(const MaterialParams& p) { return fn(p); }
};
template <class MS>
typename InstantiateDefault<MS, true>::Factory const InstantiateDefault<MS, true>::fn =
    &make_material<MS>;

template <class MS>
typename InstantiateDefault<MS, true>::MixtureFactory const
    InstantiateDefault<MS, true>::mixture_fn = &make_copper_mixture<MS>;
template struct InstantiateDefault<Kokkos::DefaultExecutionSpace::memory_space>;

template std::unique_ptr<IMaterial<Kokkos::HostSpace>>
make_material<Kokkos::HostSpace>(const MaterialParams&);

template std::unique_ptr<IMaterial<Kokkos::HostSpace>> make_copper_mixture<Kokkos::HostSpace>(
    const MaterialParams&, const MaterialParams&, const MaterialParams&);

} // namespace chipcae::material
