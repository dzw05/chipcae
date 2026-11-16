// chipcae/material/schema.hpp —— 材料卡 JSON 结构 / 语义校验入口
#pragma once

#include "chipcae/material/material_params.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace chipcae::material {

struct ValidationIssue {
    std::string field_path; // 如 "viscoelastic.volumetric_mode"
    std::string message;
};

struct ValidationResult {
    bool ok = true;
    std::vector<ValidationIssue> issues;
};

// 结构校验（JSON 语法 + required 字段存在性）。语义校验（第 5 节规则表）在 CHUNK 5 补全。
ValidationResult validate_material_json(std::string_view json);

// 结构 + 语义校验后构造不可变参数；失败抛 std::runtime_error（主机侧）。
MaterialParams parse_material_json(std::string_view json);

// 读取文件并校验的便捷入口。
ValidationResult validate_material_file(const std::string& path);

} // namespace chipcae::material
