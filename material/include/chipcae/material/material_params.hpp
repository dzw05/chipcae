// chipcae/material/material_params.hpp —— 校验后的不可变参数（D15：v0 手写）
#pragma once

#include "chipcae/material/material_batch.hpp"
#include "chipcae/material/prony_viscoelastic.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace chipcae::material {

// v0 只带主机侧必要字段；后续 chunk 按 Schema 扩展。设备端参数另建只读 View。
struct MaterialParams {
    ElasticSymmetry elastic_symmetry = ElasticSymmetry::Isotropic;
    TemperatureTable elastic_table;
    ThermalStrainParams thermal;
    TemperatureTable cte_table;
    PronyParams prony;
    TemperatureTable bulk_table;
    ShiftParams shift;
    std::vector<std::string> cure_step_ids;
    std::vector<std::string> warnings;
    std::string id;
    ModelKind kind = ModelKind::ThermoElastic;
    std::string model_id; // 如 "thermo_elastic@1"
    Real density = 0.0;
    std::uint64_t parameter_hash = 0; // 主机侧参数哈希（D13）
};

} // namespace chipcae::material
