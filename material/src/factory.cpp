// chipcae/material/src/factory.cpp —— model_id → ModelKind → MaterialAdapter
#include "chipcae/material/activation.hpp"
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
        return wrap<MS>(
            PronyViscoelastic<MS>{validated.prony,
                                  validated.bulk_table,
                                  ShiftFunction<MS>{validated.shift},
                                  ThermalStrain<MS>{validated.thermal, validated.cte_table}},
            validated);
    }
    return nullptr;
}

template <class MS, bool Distinct = !std::is_same_v<MS, Kokkos::HostSpace>>
struct InstantiateDefault {};
template <class MS>
struct InstantiateDefault<MS, true> {
    using Factory = std::unique_ptr<IMaterial<MS>> (*)(const MaterialParams&);
    static Factory const fn;
    static std::unique_ptr<IMaterial<MS>> create(const MaterialParams& p) { return fn(p); }
};
template <class MS>
typename InstantiateDefault<MS, true>::Factory const InstantiateDefault<MS, true>::fn =
    &make_material<MS>;
template struct InstantiateDefault<Kokkos::DefaultExecutionSpace::memory_space>;

template std::unique_ptr<IMaterial<Kokkos::HostSpace>>
make_material<Kokkos::HostSpace>(const MaterialParams&);

} // namespace chipcae::material
