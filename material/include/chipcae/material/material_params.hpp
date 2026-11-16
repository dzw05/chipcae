// chipcae/material/material_params.hpp —— 校验后的不可变参数（D15：v0 手写）
#pragma once

#include "chipcae/material/eigenstrain.hpp"
#include "chipcae/material/material_batch.hpp"
#include "chipcae/material/prony_viscoelastic.hpp"

#include <bit>
#include <cstdint>
#include <string>
#include <vector>

namespace chipcae::material {

enum class WarningCode : std::uint8_t { ModulusRampApplied = 1 };

// v0 只带主机侧必要字段；后续 chunk 按 Schema 扩展。设备端参数另建只读 View。
struct MaterialParams {
    bool has_shrinkage = false, has_activation = false;
    ShrinkageSchedule shrinkage;
    TemperatureValidity fit_temperature;
    void bind_cure_step(std::uint32_t ordinal, Real t_start, Real t_end) {
        shrinkage = shrinkage.bind(ordinal, t_start, t_end);
        // Process binding is part of the effective parameter identity.
        for (auto value : {std::uint64_t(ordinal),
                           std::bit_cast<std::uint64_t>(t_start),
                           std::bit_cast<std::uint64_t>(t_end)})
            for (int byte = 0; byte < 8; ++byte) {
                parameter_hash ^= static_cast<unsigned char>(value >> (8 * byte));
                parameter_hash *= 1099511628211ull;
            }
    }
    ElasticSymmetry elastic_symmetry = ElasticSymmetry::Isotropic;
    TemperatureTable elastic_table;
    ThermalStrainParams thermal;
    TemperatureTable cte_table;
    PronyHostParams prony;
    TemperatureTable bulk_table;
    ShiftParams shift;
    std::vector<std::string> cure_step_ids;
    std::vector<std::string> warnings;
    std::vector<WarningCode> warning_codes;
    std::string id;
    ModelKind kind = ModelKind::ThermoElastic;
    std::string model_id; // 如 "thermo_elastic@1"
    Real density = 0.0;
    std::uint64_t parameter_hash = 0; // 主机侧参数哈希（D13）
};

} // namespace chipcae::material
