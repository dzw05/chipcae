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
        return wrap<MS>(validated.elastic_table.num_temperatures()
                            ? ThermoElastic{validated.elastic_symmetry,
                                            validated.elastic_table,
                                            ThermalStrain{validated.thermal, validated.cte_table}}
                            : ThermoElastic{},
                        validated);
    case ModelKind::PronyViscoelastic:
        return wrap<MS>(PronyViscoelastic{validated.prony,
                                          validated.bulk_table,
                                          ShiftFunction{validated.shift},
                                          ThermalStrain{validated.thermal, validated.cte_table}},
                        validated);
    }
    return nullptr;
}

template std::unique_ptr<IMaterial<Kokkos::HostSpace>>
make_material<Kokkos::HostSpace>(const MaterialParams&);

} // namespace chipcae::material
