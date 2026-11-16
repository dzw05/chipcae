// chipcae/material/src/factory.cpp —— model_id → ModelKind → MaterialAdapter
#include "chipcae/material/material_batch.hpp"
#include "chipcae/material/material_params.hpp"
#include "chipcae/material/prony_viscoelastic.hpp"
#include "chipcae/material/thermo_elastic.hpp"

#include <memory>

namespace chipcae::material {

template <class MS>
std::unique_ptr<IMaterial<MS>> make_material(const MaterialParams& validated) {
    switch (validated.kind) {
    case ModelKind::ThermoElastic:
        return std::make_unique<MaterialAdapter<ThermoElastic, MS>>(ThermoElastic{});
    case ModelKind::PronyViscoelastic:
        return std::make_unique<MaterialAdapter<PronyViscoelastic, MS>>(PronyViscoelastic{});
    }
    return nullptr;
}

template std::unique_ptr<IMaterial<Kokkos::HostSpace>>
make_material<Kokkos::HostSpace>(const MaterialParams&);

} // namespace chipcae::material
