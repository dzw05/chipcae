// chipcae/material/schema.hpp —— 材料卡 JSON 结构 / 语义校验入口
#pragma once

#include "chipcae/material/material_params.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace chipcae::material {

struct ImportOptions {
    bool modulus_ramp = false;
    Real modulus_ramp_width = 1.0; // K; later knots shift to preserve their original gaps.
};

struct ValidationIssue {
    std::string field_path; // 如 "viscoelastic.volumetric_mode"
    std::string message;
};

struct ValidationResult {
    bool ok = true;
    std::vector<ValidationIssue> issues;
    std::vector<WarningCode> warning_codes;
};

// JSON subset structure and semantic checks; delivery mode rejects illustrative provenance.
ValidationResult validate_material_json(std::string_view json,
                                        bool customer_delivery = false,
                                        ImportOptions options = {});

// 结构 + 语义校验后构造不可变参数；失败抛 std::runtime_error（主机侧）。
MaterialParams parse_material_json(std::string_view json,
                                   bool customer_delivery = false,
                                   ImportOptions options = {});

// 读取文件并校验的便捷入口。
ValidationResult validate_material_file(const std::string& path,
                                        bool customer_delivery = false,
                                        ImportOptions options = {});

// Validate the chipcae.material_library/1 envelope and every material card.
ValidationResult validate_material_library_json(std::string_view json,
                                                bool customer_delivery = false,
                                                ImportOptions options = {});
// Parse the library in input order; any validation issue throws std::runtime_error.
std::vector<MaterialParams> parse_material_library_json(std::string_view json,
                                                        bool customer_delivery = false,
                                                        ImportOptions options = {});
// Read a file and validate it as a library.
ValidationResult validate_material_library_file(const std::string& path,
                                                bool customer_delivery = false,
                                                ImportOptions options = {});

} // namespace chipcae::material
