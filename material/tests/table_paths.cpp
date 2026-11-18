#include "chipcae/material/schema.hpp"

#include <Kokkos_Core.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace chipcae::material;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string read(const std::filesystem::path& path) {
    std::ifstream file(path);
    require(bool(file), "fixture must be readable");
    std::ostringstream text;
    text << file.rdbuf();
    require(!file.bad(), "fixture read must succeed");
    return text.str();
}

std::string replace_in_table(std::string card,
                             std::string_view owner,
                             std::string_view from,
                             std::string_view to) {
    // Scope each textual mutation to the named fixture object, including its nested table.
    const auto key = "\"" + std::string(owner) + "\": {";
    const auto begin = card.find(key);
    require(begin != std::string::npos, "fixture must contain the table owner");
    require(card.find(key, begin + key.size()) == std::string::npos,
            "fixture table owner must be unique");
    const auto end = card.find("\n  }", begin);
    require(end != std::string::npos, "fixture must contain the owner closing brace");
    const auto table = card.find(R"("table": {)", begin);
    require(table != std::string::npos && table < end, "fixture owner must contain a table");
    const auto pos = card.find(from, table);
    require(pos != std::string::npos && pos + from.size() <= end,
            "fixture replacement must find its target in the named table");
    const auto next = card.find(from, pos + from.size());
    require(next == std::string::npos || next >= end,
            "fixture replacement target must be unique within the table");
    card.replace(pos, from.size(), to);
    return card;
}

void valid(const ValidationResult& result) {
    require(result.ok && result.issues.empty(), "expected ok=true and no issues");
}

void invalid(const ValidationResult& result,
             const std::string& expected_path,
             const std::string& expected_message) {
    require(!result.ok, "expected ok=false");
    require(result.issues.size() == 1, "expected exactly one issue");
    const auto& issue = result.issues[0];
    if (issue.field_path != expected_path || issue.message != expected_message) {
        std::fprintf(stderr,
                     "  expected field_path: %s\n  actual field_path: %s\n"
                     "  expected message: %s\n  actual message: %s\n",
                     expected_path.c_str(),
                     issue.field_path.c_str(),
                     expected_message.c_str(),
                     issue.message.c_str());
        throw std::runtime_error("field_path and message must match exactly");
    }
}
} // namespace

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    int failures = 0;
    const auto run = [&](const char* name, const auto& test) {
        try {
            test();
            std::printf("PASS material_table_paths: %s\n", name);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL material_table_paths: %s\n  %s\n", name, e.what());
            ++failures;
        }
    };

    std::string card;
    run("fixtures", [&] {
        require(argc == 2, "expected the fixture directory");
        card = read(std::filesystem::path(argv[1]) / "valid_elastic.json");
    });
    if (failures != 0)
        return 1;

    run("interp_constant", [&] {
        invalid(
            validate_material_json(replace_in_table(
                card, "elastic", R"("interpolation": "linear")", R"("interpolation": "constant")")),
            "material.elastic.table.interpolation",
            "material.elastic.table.interpolation: v0 only supports linear (pchip "
            "NotImplemented; constant forbidden)");
    });
    run("interp_pchip_cte", [&] {
        invalid(validate_material_json(replace_in_table(card,
                                                        "thermal_expansion",
                                                        R"("interpolation": "linear")",
                                                        R"("interpolation": "pchip")")),
                "material.thermal_expansion.table.interpolation",
                "material.thermal_expansion.table.interpolation: v0 only supports linear (pchip "
                "NotImplemented; constant forbidden)");
    });
    run("extrap_bogus", [&] {
        invalid(validate_material_json(replace_in_table(
                    card, "elastic", R"("extrapolation": "error")", R"("extrapolation": "bogus")")),
                "material.elastic.table.extrapolation",
                "material.elastic.table.extrapolation: invalid value");
    });
    run("column_scalar", [&] {
        invalid(validate_material_json(replace_in_table(
                    card,
                    "elastic",
                    "\"E\": [\n          1000000000.0,\n          2000000000.0\n        ]",
                    R"("E": 5)")),
                "material.elastic.table.columns.E",
                "material.elastic.table.columns.E: expected array");
    });
    run("library_path", [&] {
        const auto bad_card = replace_in_table(
            card, "elastic", R"("interpolation": "linear")", R"("interpolation": "constant")");
        const auto library =
            R"({"schema":"chipcae.material_library/1","materials":[)" + bad_card + "]}";
        invalid(validate_material_library_json(library),
                "materials[0].elastic.table.interpolation",
                "material.elastic.table.interpolation: v0 only supports linear (pchip "
                "NotImplemented; constant forbidden)");
    });
    run("both_tables_bad", [&] {
        const auto elastic_bad = replace_in_table(
            card, "elastic", R"("interpolation": "linear")", R"("interpolation": "constant")");
        invalid(validate_material_json(replace_in_table(elastic_bad,
                                                        "thermal_expansion",
                                                        R"("interpolation": "linear")",
                                                        R"("interpolation": "constant")")),
                "material.thermal_expansion.table.interpolation",
                "material.thermal_expansion.table.interpolation: v0 only supports linear (pchip "
                "NotImplemented; constant forbidden)");
    });
    run("valid_still_ok", [&] { valid(validate_material_json(card)); });
    run("interp_case_sensitive", [&] {
        invalid(
            validate_material_json(replace_in_table(
                card, "elastic", R"("interpolation": "linear")", R"("interpolation": "Linear")")),
            "material.elastic.table.interpolation",
            "material.elastic.table.interpolation: v0 only supports linear (pchip "
            "NotImplemented; constant forbidden)");
    });
    run("missing_interpolation", [&] {
        invalid(validate_material_json(
                    replace_in_table(card, "elastic", "\"interpolation\": \"linear\",", "")),
                "material.elastic.table.interpolation",
                "material.elastic.table.interpolation: required field is missing");
    });
    run("missing_extrapolation", [&] {
        invalid(validate_material_json(
                    replace_in_table(card, "elastic", ",\n      \"extrapolation\": \"error\"", "")),
                "material.elastic.table.extrapolation",
                "material.elastic.table.extrapolation: required field is missing");
    });
    run("column_non_number", [&] {
        invalid(validate_material_json(replace_in_table(
                    card,
                    "elastic",
                    "\"E\": [\n          1000000000.0,\n          2000000000.0\n        ]",
                    R"("E": [1000000000,"bad"])")),
                "material.elastic.table.columns.E[1]",
                "material.elastic.table.columns.E[1]: expected finite number");
    });
    run("linear_clamp", [&] {
        const auto elastic_clamp = replace_in_table(
            card, "elastic", R"("extrapolation": "error")", R"("extrapolation": "clamp")");
        valid(validate_material_json(replace_in_table(elastic_clamp,
                                                      "thermal_expansion",
                                                      R"("extrapolation": "error")",
                                                      R"("extrapolation": "clamp")")));
    });

    return failures == 0 ? 0 : 1;
}
