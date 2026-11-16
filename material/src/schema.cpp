// chipcae/material/src/schema.cpp —— 结构校验（v0 轻量 JSON 子集检查）
#include "chipcae/material/schema.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace chipcae::material {
namespace {

// 轻量结构检查：括号/引号配对、顶层必需字段存在。完整 JSON 解析与第 5 节语义规则在
// CHUNK 5 实现；这里不引入第三方 JSON 库。
bool braces_balanced(std::string_view text) {
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            --depth;
            if (depth < 0)
                return false;
        }
    }
    return depth == 0 && !in_string;
}

bool has_key(std::string_view text, std::string_view key) {
    return text.find("\"" + std::string(key) + "\"") != std::string_view::npos;
}

} // namespace

ValidationResult validate_material_json(std::string_view json) {
    ValidationResult result;
    auto fail = [&result](std::string path, std::string message) {
        result.ok = false;
        result.issues.push_back(ValidationIssue{std::move(path), std::move(message)});
    };

    if (json.empty()) {
        fail("", "material card is empty");
        return result;
    }
    if (!braces_balanced(json)) {
        fail("", "malformed JSON: unbalanced braces/brackets/strings");
        return result;
    }

    for (const auto* key : {"id",
                            "schema_version",
                            "unit_system",
                            "model",
                            "density",
                            "thermal_expansion",
                            "provenance"}) {
        if (!has_key(json, key))
            fail(key, "required field is missing");
    }
    if (has_key(json, "prony_viscoelastic") && !has_key(json, "volumetric_mode"))
        fail("viscoelastic.volumetric_mode", "required field is missing (D14: no default)");

    return result;
}

ValidationResult validate_material_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        ValidationResult result;
        result.ok = false;
        result.issues.push_back(ValidationIssue{"", "cannot open file: " + path});
        return result;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return validate_material_json(ss.str());
}

MaterialParams parse_material_json(std::string_view json) {
    const ValidationResult vr = validate_material_json(json);
    if (!vr.ok)
        throw std::runtime_error("chipcae::material: invalid material card (TODO semantics)");
    // TODO(K3): 结构 + 语义解析为 MaterialParams（CHUNK 5）。
    throw std::runtime_error(
        "chipcae::material::parse_material_json: parser not implemented in CHUNK 1");
}

} // namespace chipcae::material
