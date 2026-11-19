#include "chipcae/material/schema.hpp"
#include "chipcae/material/table_view.hpp"
#include "chipcae/material/thermo_elastic.hpp"

#include <Kokkos_Core.hpp>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace chipcae::material;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::string read(const std::filesystem::path& path) {
    std::ifstream file(path);
    require(bool(file), "fixture must be readable: " + path.string());
    std::ostringstream text;
    text << file.rdbuf();
    require(!file.bad(), "fixture read must succeed: " + path.string());
    return text.str();
}

std::string replace_once(std::string text, std::string_view from, std::string_view to) {
    require(!from.empty(), "fixture replacement target must not be empty");
    const auto pos = text.find(from);
    require(pos != std::string::npos, "fixture replacement must find its target");
    require(text.find(from, pos + from.size()) == std::string::npos,
            "fixture replacement target must be unique");
    text.replace(pos, from.size(), to);
    return text;
}

std::string replace_in_table(std::string card,
                             std::string_view owner,
                             std::string_view from,
                             std::string_view to) {
    const auto key = "\"" + std::string(owner) + "\": {";
    const auto begin = card.find(key);
    require(begin != std::string::npos, "fixture must contain the table owner");
    require(card.find(key, begin + key.size()) == std::string::npos,
            "fixture table owner must be unique");
    const auto end = card.find("\n  }", begin);
    require(end != std::string::npos, "fixture must contain the owner closing brace");
    const auto table = card.find(R"("table": {)", begin);
    require(table != std::string::npos && table < end, "fixture owner must contain a table");
    // Both fixtures use indented owner braces; check replacements inside that
    // owner's table only.
    const auto changed = replace_once(card.substr(table, end - table), from, to);
    card.replace(table, end - table, changed);
    return card;
}

const std::string single_card = R"({
  "id": "illustrative",
  "schema_version": "1",
  "unit_system": "SI",
  "density": 1000,
  "model": "thermo_elastic",
  "provenance": {
    "source_type": "illustrative",
    "confidence": "illustrative"
  },
  "elastic": {
    "symmetry": "isotropic",
    "table": {
      "temperatures": [250],
      "columns": {
        "E": [1e9],
        "nu": [0.3]
      },
      "interpolation": "linear",
      "extrapolation": "error"
    }
  },
  "thermal_expansion": {
    "cte_type": "tangent",
    "symmetry": "isotropic",
    "stress_free_temperature": 300,
    "table": {
      "temperatures": [250],
      "columns": {
        "alpha": [1e-5]
      },
      "interpolation": "linear",
      "extrapolation": "error"
    }
  }
})";

const char* extrapolation_name(Extrapolation extrapolation) {
    return extrapolation == Extrapolation::Error ? "error" : "clamp";
}

std::string with_policy(std::string card,
                        std::string_view owner,
                        std::string_view interpolation,
                        Extrapolation extrapolation) {
    card = replace_in_table(std::move(card),
                            owner,
                            R"("interpolation": "linear")",
                            "\"interpolation\": \"" + std::string(interpolation) + "\"");
    return replace_in_table(std::move(card),
                            owner,
                            R"("extrapolation": "error")",
                            "\"extrapolation\": \"" +
                                std::string(extrapolation_name(extrapolation)) + "\"");
}

std::string card_with_policies(std::string_view elastic,
                               std::string_view cte,
                               Extrapolation elastic_extrapolation = Extrapolation::Error,
                               Extrapolation cte_extrapolation = Extrapolation::Error) {
    return with_policy(with_policy(single_card, "elastic", elastic, elastic_extrapolation),
                       "thermal_expansion",
                       cte,
                       cte_extrapolation);
}

std::string with_id(const std::string& card, std::string_view id) {
    return replace_once(card, R"("id": "illustrative")", "\"id\": \"" + std::string(id) + "\"");
}

std::string library(const std::string& cards) {
    return R"({"schema":"chipcae.material_library/1","materials":[)" + cards + "]}";
}

void valid(const ValidationResult& result) {
    if (!result.ok || !result.issues.empty()) {
        for (const auto& issue : result.issues)
            std::fprintf(stderr, "  %s: %s\n", issue.field_path.c_str(), issue.message.c_str());
        throw std::runtime_error("expected ok=true and no issues");
    }
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

const std::string interpolation_suffix =
    ": v0 only supports linear (pchip NotImplemented; constant forbidden)";

void interpolation_rejected(const std::string& card, const std::string& owner = "elastic") {
    const auto path = "material." + owner + ".table.interpolation";
    invalid(validate_material_json(card), path, path + interpolation_suffix);
}

void parsed_policies(const MaterialParams& params,
                     Extrapolation elastic_extrapolation,
                     Extrapolation cte_extrapolation) {
    require(params.elastic_table.num_temperatures() == 1 &&
                params.cte_table.num_temperatures() == 1,
            "parsed tables must remain single-point");
    require(params.elastic_table.interpolation() == Interpolation::Linear &&
                params.cte_table.interpolation() == Interpolation::Linear,
            "parsed tables must use Interpolation::Linear");
    require(params.elastic_table.extrapolation() == elastic_extrapolation &&
                params.cte_table.extrapolation() == cte_extrapolation,
            "parsed tables must preserve their requested extrapolation");
}

void accepted(const std::string& card,
              Extrapolation elastic_extrapolation = Extrapolation::Error,
              Extrapolation cte_extrapolation = Extrapolation::Error) {
    valid(validate_material_json(card));
    parsed_policies(parse_material_json(card), elastic_extrapolation, cte_extrapolation);
}

void single_point_values(const TemperatureTable& table) {
    require(table.num_temperatures() == 1, "evaluation must use a single-point table");
    for (std::size_t column = 0; column < table.num_columns(); ++column) {
        for (Real temperature : {150.0, 250.0, 350.0, 250.0 + 1e-9}) {
            Real value = std::numeric_limits<Real>::quiet_NaN();
            const auto code = table.evaluate(column, temperature, value);
            if (code != StatusCode::Ok || value != table.columns()[column][0] ||
                table.clamped_warning()) {
                std::fprintf(stderr,
                             "  column=%zu T=%.17g code=%d value=%.17g expected=%.17g "
                             "clamped=%s\n",
                             column,
                             temperature,
                             static_cast<int>(code),
                             value,
                             table.columns()[column][0],
                             table.clamped_warning() ? "true" : "false");
                throw std::runtime_error("single-point evaluation must return Ok, the exact stored "
                                         "value, and no clamped warning");
            }
        }
    }
}

void bad_inputs(const TemperatureTable& table) {
    Real value = 0;
    for (Real temperature : {std::numeric_limits<Real>::quiet_NaN(),
                             std::numeric_limits<Real>::infinity(),
                             -std::numeric_limits<Real>::infinity()})
        require(table.evaluate(0, temperature, value) == StatusCode::Failed,
                "nonfinite temperature must return Failed");
    for (std::size_t column : {table.num_columns(), std::size_t{5}})
        require(table.evaluate(column, 250, value) == StatusCode::Failed,
                "invalid column must return Failed");
}

// Absolute + relative tolerance for the table integrals and constitutive response.
KOKKOS_INLINE_FUNCTION bool near(Real actual, Real expected) {
    return Kokkos::abs(actual - expected) <= 1e-10 + 5.5e-6 * Kokkos::abs(expected);
}

template <class MS>
KOKKOS_INLINE_FUNCTION int single_point_view_checks(const TableView<MS>& table) {
    const Kokkos::Array<Real, 2> expected = {1e9, 0.3};
    const Kokkos::Array<Real, 4> temperatures = {150, 250, 350, 250 + 1e-9};
    for (int column = 0; column < 2; ++column) {
        for (Real temperature : temperatures) {
            Real value = 0;
            bool clamped = false;
            if (table.evaluate(column, temperature, value, &clamped) != StatusCode::Ok ||
                !near(value, expected[column]) || clamped)
                return 1;
        }
        // Span the knot, reverse the bounds, stay on either side, and use equal bounds.
        const Kokkos::Array<Kokkos::Array<Real, 3>, 5> intervals = {
            {{150, 350, 200}, {350, 150, -200}, {100, 200, 100}, {300, 400, 100}, {300, 300, 0}}};
        for (const auto& interval : intervals) {
            Real value = 0;
            if (table.integral(column, interval[0], interval[1], value) != StatusCode::Ok ||
                !near(value, interval[2] * expected[column]))
                return 2;
        }
    }
    Real value = 0;
    for (int column : Kokkos::Array<int, 2>{-1, 2})
        if (table.evaluate(column, 150, value) != StatusCode::Failed)
            return 3;
    const Kokkos::Array<Real, 3> nonfinite = {std::numeric_limits<Real>::quiet_NaN(),
                                              std::numeric_limits<Real>::infinity(),
                                              -std::numeric_limits<Real>::infinity()};
    for (Real temperature : nonfinite)
        if (table.evaluate(0, temperature, value) != StatusCode::Failed)
            return 4;
    return 0;
}

template <class MS>
ThermoElastic<MS> single_point_model(Extrapolation extrapolation) {
    const auto params = parse_material_json(
        card_with_policies("constant", "constant", extrapolation, extrapolation));
    return ThermoElastic<MS>(params.elastic_symmetry,
                             params.elastic_table,
                             ThermalStrain<MS>(params.thermal, params.cte_table));
}

template <class MS>
KOKKOS_INLINE_FUNCTION bool single_point_model_checks(const ThermoElastic<MS>& model) {
    const Kokkos::Array<Real, 3> temperatures = {150, 300, 350};
    const Kokkos::Array<Real, 3> thermal_strains = {-0.0015, 0, 0.0005};
    // Zero total strain: sigma = -E * alpha * (T - T_sf) / (1 - 2 * nu).
    const Kokkos::Array<Real, 3> stresses = {3.75e6, 0, -1.25e6};
    StepContext ctx{};
    ctx.dt = 1;
    for (int i = 0; i < 3; ++i) {
        PointInput in{};
        in.T_n = 300;
        in.T_np1 = temperatures[i];
        PointOutput out{};
        if (model.update(ctx, in, ConstStateRef{}, StateRef{}, out) != StatusCode::Ok ||
            out.status.code != StatusCode::Ok)
            return false;
        for (int d = 0; d < 6; ++d)
            if (!near(out.thermal_strain[d], d < 3 ? thermal_strains[i] : 0) ||
                !near(out.stress[d], d < 3 ? stresses[i] : 0))
                return false;
    }
    return true;
}

template <class ExecutionSpace>
void single_point_kernel(Extrapolation extrapolation) {
    using MS = typename ExecutionSpace::memory_space;
    const TableView<MS> table(
        TemperatureTable({250}, {{1e9}, {0.3}}, Interpolation::Linear, extrapolation));
    const auto model = single_point_model<MS>(extrapolation);
    Kokkos::View<int[2], MS> results("single_point_results");
    Kokkos::parallel_for(
        "single_point_paths", Kokkos::RangePolicy<ExecutionSpace>(0, 1), KOKKOS_LAMBDA(int) {
            results(0) = single_point_view_checks(table);
            results(1) = single_point_model_checks(model) ? 0 : 1;
        });
    ExecutionSpace().fence();
    const auto host = Kokkos::create_mirror_view_and_copy(Kokkos::HostSpace{}, results);
    require(host(0) == 0,
            "kernel TableView value/clamp, integral, column, or temperature check: " +
                std::to_string(host(0)));
    require(host(1) == 0, "kernel constitutive response must match constant elasticity and CTE");
}
} // namespace

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    int failures = 0;
    const auto run = [&](const std::string& name, const auto& test) {
        try {
            test();
            std::printf("PASS material_constant_single_point: %s\n", name.c_str());
        } catch (const std::exception& e) {
            std::fprintf(
                stderr, "FAIL material_constant_single_point: %s\n  %s\n", name.c_str(), e.what());
            ++failures;
        }
    };

    std::string two_point_card, s2_solve, bimetal;
    run("fixtures", [&] {
        require(argc == 4, "expected fixture directory, s2_solve library, and bimetal library");
        two_point_card = read(std::filesystem::path(argv[1]) / "valid_elastic.json");
        s2_solve = read(argv[2]);
        bimetal = read(argv[3]);
        valid(validate_material_json(two_point_card));
        accepted(single_card);
    });
    if (failures != 0)
        return 1;

    run("single_card_elastic_const", [&] { accepted(card_with_policies("constant", "linear")); });
    run("single_card_cte_const", [&] { accepted(card_with_policies("linear", "constant")); });
    for (auto elastic : {Extrapolation::Error, Extrapolation::Clamp}) {
        for (auto cte : {Extrapolation::Error, Extrapolation::Clamp}) {
            run(std::string("single_card_both_const/") + extrapolation_name(elastic) + "/" +
                    extrapolation_name(cte),
                [&] {
                    accepted(
                        card_with_policies("constant", "constant", elastic, cte), elastic, cte);
                });
        }
    }
    run("library_const", [&] {
        const auto card = card_with_policies("constant", "constant");
        const auto text = library(with_id(card, "a") + "," + with_id(card, "b"));
        valid(validate_material_library_json(text));
        const auto params = parse_material_library_json(text);
        require(params.size() == 2, "library must parse both cards");
        for (const auto& p : params)
            parsed_policies(p, Extrapolation::Error, Extrapolation::Error);
    });

    for (auto extrapolation : {Extrapolation::Error, Extrapolation::Clamp}) {
        const auto name = std::string("eval_") + extrapolation_name(extrapolation);
        run(name + "/view_host", [&] {
            const TableView<Kokkos::HostSpace> table(
                TemperatureTable({250}, {{1e9}, {0.3}}, Interpolation::Linear, extrapolation));
            const auto result = single_point_view_checks(table);
            require(result == 0,
                    "host TableView value/clamp, integral, column, or temperature check: " +
                        std::to_string(result));
        });
        run(name + "/model_host", [&] {
            require(single_point_model_checks(single_point_model<Kokkos::HostSpace>(extrapolation)),
                    "host constitutive response must match constant elasticity and CTE");
        });
        run(name + "/kernel_default",
            [&] { single_point_kernel<Kokkos::DefaultExecutionSpace>(extrapolation); });
#if defined(KOKKOS_ENABLE_SERIAL)
        run(name + "/kernel_serial", [&] { single_point_kernel<Kokkos::Serial>(extrapolation); });
#endif
        run(name, [&] {
            single_point_values(
                TemperatureTable({250}, {{1e9}, {0.3}}, Interpolation::Linear, extrapolation));
        });
        for (const auto* interpolation : {"linear", "constant"}) {
            run(name + "/parsed_" + interpolation, [&] {
                const auto params = parse_material_json(
                    card_with_policies(interpolation, interpolation, extrapolation, extrapolation));
                parsed_policies(params, extrapolation, extrapolation);
                single_point_values(params.elastic_table);
                single_point_values(params.cte_table);
            });
        }
        run(std::string("eval_bad_input/") + extrapolation_name(extrapolation), [&] {
            const TemperatureTable table(
                {250}, {{1e9}, {0.3}}, Interpolation::Linear, extrapolation);
            bad_inputs(table);
            const auto params = parse_material_json(
                card_with_policies("linear", "linear", extrapolation, extrapolation));
            bad_inputs(params.elastic_table);
            bad_inputs(params.cte_table);
        });
    }
    for (auto interpolation : {Interpolation::Constant, Interpolation::Pchip}) {
        run(interpolation == Interpolation::Constant ? "constructor_constant" : "constructor_pchip",
            [&] {
                bool rejected = false;
                try {
                    const TemperatureTable table({250}, {{1e9}, {0.3}}, interpolation);
                } catch (const std::invalid_argument& e) {
                    require(std::string(e.what()) == "interpolation: only linear implemented",
                            "constructor rejection message must remain unchanged");
                    rejected = true;
                }
                require(rejected, "constructor must still reject non-linear interpolation");
            });
    }

    const auto two_point_constant =
        with_policy(two_point_card, "elastic", "constant", Extrapolation::Error);
    run("two_point_const_card", [&] { interpolation_rejected(two_point_constant); });
    run("two_point_const_library", [&] {
        invalid(validate_material_library_json(library(two_point_constant)),
                "materials[0].elastic.table.interpolation",
                "material.elastic.table.interpolation" + interpolation_suffix);
    });
    run("two_point_const_cte", [&] {
        interpolation_rejected(
            with_policy(two_point_card, "thermal_expansion", "constant", Extrapolation::Error),
            "thermal_expansion");
    });
    run("three_point_const", [&] {
        auto card = card_with_policies("constant", "linear");
        card = replace_in_table(card, "elastic", "[250]", "[150, 250, 350]");
        card = replace_in_table(card, "elastic", "[1e9]", "[1e9, 1e9, 1e9]");
        card = replace_in_table(card, "elastic", "[0.3]", "[0.3, 0.3, 0.3]");
        interpolation_rejected(card);
    });
    run("zero_point_const", [&] {
        auto card = card_with_policies("constant", "linear");
        card = replace_in_table(card, "elastic", "[250]", "[]");
        card = replace_in_table(card, "elastic", "[1e9]", "[]");
        card = replace_in_table(card, "elastic", "[0.3]", "[]");
        interpolation_rejected(card);
    });
    run("missing_temperatures_const", [&] {
        interpolation_rejected(replace_in_table(
            card_with_policies("constant", "linear"), "elastic", R"("temperatures": [250],)", ""));
    });
    for (const auto* temperatures : {"250", "null", "{}", R"("250")"}) {
        run(std::string("non_array_temperatures_const/") + temperatures, [&] {
            interpolation_rejected(replace_in_table(
                card_with_policies("constant", "linear"), "elastic", "[250]", temperatures));
        });
    }
    run("single_point_pchip",
        [&] { interpolation_rejected(card_with_policies("pchip", "linear")); });
    for (const auto* interpolation : {"Constant", "CONSTANT", "Linear"}) {
        run(std::string("case_mismatched_interpolation/") + interpolation,
            [&] { interpolation_rejected(card_with_policies(interpolation, "linear")); });
    }
    run("mixed_single_and_two_point_const", [&] {
        auto card = card_with_policies("constant", "constant");
        card = replace_in_table(card, "thermal_expansion", "[250]", "[250, 400]");
        card = replace_in_table(card, "thermal_expansion", "[1e-5]", "[1e-5, 2e-5]");
        interpolation_rejected(card, "thermal_expansion");
    });
    run("single_point_bogus_extrapolation", [&] {
        invalid(validate_material_json(replace_in_table(card_with_policies("constant", "linear"),
                                                        "elastic",
                                                        R"("extrapolation": "error")",
                                                        R"("extrapolation": "bogus")")),
                "material.elastic.table.extrapolation",
                "material.elastic.table.extrapolation: invalid value");
    });
    run("single_point_column_mismatch", [&] {
        const auto result = validate_material_json(replace_in_table(
            card_with_policies("constant", "linear"), "elastic", "[1e9]", "[1e9, 2e9]"));
        require(!result.ok && result.issues.size() == 1, "expected ok=false and exactly one issue");
        const auto& issue = result.issues[0];
        require(issue.field_path == "material.elastic.table" &&
                    issue.message.starts_with("material.elastic.table: ") &&
                    issue.message.find("column length mismatch") != std::string::npos,
                "column mismatch must retain its table path and diagnostic; actual: " +
                    issue.field_path + ": " + issue.message);
    });
    run("multi_point_unchanged", [&] {
        auto params = parse_material_json(two_point_card);
        Real value = 0;
        require(params.elastic_table.evaluate(0, 200, value) == StatusCode::OutOfRange,
                "multi-point error extrapolation must still return OutOfRange");
        params = parse_material_json(
            with_policy(two_point_card, "elastic", "linear", Extrapolation::Clamp));
        require(params.elastic_table.evaluate(0, 200, value) == StatusCode::Ok && value == 1e9 &&
                    params.elastic_table.clamped_warning(),
                "multi-point clamp must still return the exact endpoint and set "
                "its warning");
    });
    run("repo_s2_solve", [&] {
        valid(validate_material_library_json(s2_solve));
        for (const auto& params : parse_material_library_json(s2_solve))
            parsed_policies(params, Extrapolation::Clamp, Extrapolation::Clamp);
    });
    run("repo_bimetal", [&] {
        valid(validate_material_library_json(bimetal));
        for (const auto& params : parse_material_library_json(bimetal))
            parsed_policies(params, Extrapolation::Clamp, Extrapolation::Clamp);
    });

    return failures == 0 ? 0 : 1;
}
