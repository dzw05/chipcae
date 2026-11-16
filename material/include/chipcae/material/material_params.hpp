// chipcae/material/material_params.hpp —— 校验后的不可变参数（D15：v0 手写）
#pragma once

#include "chipcae/material/material_batch.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace chipcae::material {

enum class ElasticSymmetry : std::uint8_t { Isotropic, TransverselyIsotropic, Orthotropic };

enum class VolumetricMode : std::uint8_t { ElasticBulk, ConstantPoissonFromE };

struct PronyTerm {
    Real G = 0.0;   // 剪切分支模量 [Pa]
    Real tau = 0.0; // 松弛时间 [s]
};

// v0 只带主机侧必要字段；后续 chunk 按 Schema 扩展。设备端参数另建只读 View。
struct MaterialParams {
    std::string id;
    ModelKind kind = ModelKind::ThermoElastic;
    std::string model_id; // 如 "thermo_elastic@1"
    Real density = 0.0;
    std::uint64_t parameter_hash = 0; // 主机侧参数哈希（D13）
};

} // namespace chipcae::material
