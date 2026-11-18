#include "chipcae/material/schema.hpp"

#include <Kokkos_Core.hpp>
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
    return text.str();
}

std::string wrap(const std::string& cards) {
    return R"({"schema":"chipcae.material_library/1","materials":[)" + cards + "]}";
}

std::string with_member(std::string object, const std::string& member) {
    object.insert(object.rfind('}'), "," + member);
    return object;
}

std::string replace_once(std::string text, std::string_view from, std::string_view to) {
    const auto pos = text.find(from);
    require(pos != std::string::npos, "fixture replacement must find its target");
    text.replace(pos, from.size(), to);
    return text;
}

std::string duplicate_elastic_knots(std::string card) {
    const auto start = card.find(R"("temperatures")");
    const auto left = card.find('[', start);
    const auto right = card.find(']', left);
    require(left != std::string::npos && right != std::string::npos,
            "fixture must contain elastic temperature knots");
    card.replace(left, right - left + 1, "[250,250]");
    return card;
}

bool has_issue(const ValidationResult& result, std::string_view path, bool prefix = false) {
    return std::any_of(result.issues.begin(), result.issues.end(), [&](const auto& issue) {
        return !issue.message.empty() &&
               (prefix ? issue.field_path.starts_with(path) : issue.field_path == path);
    });
}

void valid(const ValidationResult& result) {
    require(result.ok && result.issues.empty(), "valid library must have ok=true and no issues");
}

ValidationResult invalid(const std::string& json,
                         std::string_view path = {},
                         bool prefix = false,
                         bool delivery = false,
                         ImportOptions options = {}) {
    const auto result = validate_material_library_json(json, delivery, options);
    bool threw = false;
    try {
        (void)parse_material_library_json(json, delivery, options);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    require(threw, "invalid library parse must throw std::runtime_error");
    require(!result.ok && !result.issues.empty(), "invalid library must report issues");
    require(std::all_of(result.issues.begin(),
                        result.issues.end(),
                        [](const auto& issue) { return !issue.message.empty(); }),
            "issue messages must be nonempty");
    if (!path.empty())
        require(has_issue(result, path, prefix), "expected issue path is missing");
    return result;
}

void same_card(const MaterialParams& actual, const MaterialParams& expected) {
    // These are decoded fixture values, so the brief requires exact comparisons.
    require(actual.id == expected.id && actual.density == expected.density &&
                actual.kind == expected.kind && actual.conductivity == expected.conductivity,
            "library card fields must match direct single-card parsing");
    require(actual.elastic_table.temperatures() == expected.elastic_table.temperatures() &&
                actual.elastic_table.columns() == expected.elastic_table.columns() &&
                actual.cte_table.temperatures() == expected.cte_table.temperatures() &&
                actual.cte_table.columns() == expected.cte_table.columns(),
            "library temperature tables must match direct single-card parsing");
    require(actual.warning_codes == expected.warning_codes,
            "library card warnings must match direct single-card parsing");
}
} // namespace

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    int failures = 0;
    const auto run = [&](const char* name, const auto& test) {
        try {
            test();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL material_library_parse: %s\n  %s\n", name, e.what());
            ++failures;
        } catch (...) {
            std::fprintf(stderr, "FAIL material_library_parse: %s\n", name);
            ++failures;
        }
    };

    std::string card;
    std::string second;
    std::string library;
    std::filesystem::path material_dir;
    run("fixtures", [&] {
        require(argc == 3, "expected material directory and optional demo path");
        material_dir = argv[1];
        card = read(material_dir / "tests/valid_elastic.json");
        second = replace_once(card, R"("illustrative")", R"("illustrative_2")");
        library = wrap(card);
    });
    if (failures != 0)
        return 1;

    run("single_card_wrapped", [&] {
        valid(validate_material_library_json(library));
        const auto parsed = parse_material_library_json(library);
        require(parsed.size() == 1, "one input card must yield one result");
        const auto direct = parse_material_json(card);
        same_card(parsed[0], direct);
        require(!parsed[0].conductivity.has_value() && !direct.conductivity.has_value(),
                "absent conductivity must remain absent");
    });
    run("two_ids", [&] {
        const auto json = wrap(card + "," + second);
        valid(validate_material_library_json(json));
        const auto parsed = parse_material_library_json(json);
        require(parsed.size() == 2, "two input cards must yield two results");
        require(parsed[0].id == "illustrative" && parsed[1].id == "illustrative_2",
                "input id order must be preserved");
        same_card(parsed[0], parse_material_json(card));
        same_card(parsed[1], parse_material_json(second));
    });
    run("x_prefix", [&] {
        const auto json = with_member(library, R"("x-note":"n")");
        valid(validate_material_library_json(json));
        const auto parsed = parse_material_library_json(json);
        require(parsed.size() == 1, "extension must preserve the card count");
        same_card(parsed[0], parse_material_json(card));
    });
    run("missing_schema", [&] {
        invalid(read(material_dir / "schema/examples/negative-library-missing-schema.json"),
                "schema");
    });
    run("invalid_card", [&] {
        invalid(read(material_dir / "schema/examples/negative-library-invalid-material.json"),
                "materials[0]",
                true);
    });
    run("duplicate_id", [&] {
        const auto json = wrap(card + "," + card);
        const auto first = invalid(json, "materials[1].id");
        const auto again = validate_material_library_json(json);
        require(first.ok == again.ok && first.issues.size() == again.issues.size(),
                "repeated validation must be stateless");
        require(has_issue(again, "materials[1].id"), "repeat must retain the duplicate issue");
    });
    run("empty_materials", [&] { invalid(wrap(""), "materials"); });
    run("unknown_key", [&] { invalid(with_member(library, R"("foo":1)"), "foo"); });
    run("card_not_library", [&] {
        valid(validate_material_json(card));
        valid(validate_material_file((material_dir / "tests/valid_elastic.json").string()));
        const auto parsed = parse_material_json(card);
        require(parsed.id == "illustrative" && parsed.density == 1000 &&
                    parsed.kind == ModelKind::ThermoElastic && !parsed.conductivity.has_value(),
                "existing single-card entry points must retain their behavior");
        invalid(card);
    });
    run("fcbga_demo", [&] {
        const std::filesystem::path path = argv[2];
        if (!std::filesystem::exists(path)) {
            std::puts("SKIP material_library_parse: fcbga_demo");
            return;
        }
        const auto json = read(path);
        valid(validate_material_library_json(json));
        const auto parsed = parse_material_library_json(json);
        const std::array<std::string_view, 5> ids = {
            "organic_eq", "sac305_ball_eq", "underfill", "silicon", "copper"};
        const std::array<Real, 5> conductivity = {3.1, 58, 0.5, 148, 394};
        require(parsed.size() == ids.size(), "demo must yield five materials");
        for (std::size_t i = 0; i < ids.size(); ++i) {
            require(parsed[i].id == ids[i], "demo must retain fixture id order");
            require(parsed[i].conductivity.has_value() &&
                        *parsed[i].conductivity == conductivity[i],
                    "demo conductivity must exactly match the fixture");
        }
    });
    run("delivery_illustrative",
        [&] { invalid(library, "materials[0].provenance.confidence", false, true); });
    run("cannot_open", [&] {
        const auto path =
            (material_dir / "tests/valid_elastic.json" / "missing-library.json").string();
        const auto result = validate_material_library_file(path);
        require(!result.ok && result.issues.size() == 1, "unopenable file must report one issue");
        require(result.issues[0].field_path == path &&
                    result.issues[0].message == "cannot open file",
                "unopenable file must preserve path and exact message without throwing");
    });
    run("valid_file", [&] {
        // The read-only negative fixtures above exercise JSON validation, not the file entry point.
        const auto path = std::filesystem::current_path() / "material_library_parse.valid.json";
        require(!std::filesystem::exists(path), "temporary fixture must not overwrite a file");
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code error;
                std::filesystem::remove(path, error);
            }
        } cleanup{path};
        {
            std::ofstream file(path);
            file << library;
            require(bool(file), "temporary library must be writable");
        }
        valid(validate_material_library_file(path.string()));
    });

    run("empty_input", [&] { invalid(""); });
    run("malformed_json", [&] { invalid("{\"schema\":"); });
    run("trailing_content", [&] { invalid(library + " false"); });
    run("non_object_root", [&] {
        for (const auto* json : {"[]", "null", "7", "true", R"("text")"})
            invalid(json);
    });
    run("wrong_schema", [&] {
        for (const auto* value : {R"("chipcae.material_library/2")",
                                  R"("Chipcae.material_library/1")",
                                  "1",
                                  "null",
                                  "true",
                                  "[]",
                                  "{}"})
            invalid("{\"schema\":" + std::string(value) + ",\"materials\":[" + card + "]}",
                    "schema");
    });
    run("schema_case", [&] {
        const auto result = invalid(replace_once(library, R"("schema")", R"("Schema")"), "schema");
        require(has_issue(result, "Schema"), "Schema must also be reported as an unknown key");
    });
    run("missing_materials",
        [&] { invalid(R"({"schema":"chipcae.material_library/1"})", "materials"); });
    run("wrong_materials_type", [&] {
        for (const auto* value : {"{}", "null", "1", "true", R"("text")"})
            invalid(R"({"schema":"chipcae.material_library/1","materials":)" + std::string(value) +
                        "}",
                    "materials");
    });
    run("non_object_card", [&] {
        for (const auto* value : {"null", "[]", "1", "true", R"("text")"})
            invalid(wrap(value), "materials[0]", true);
    });
    run("collect_across_cards", [&] {
        const auto bad_conductivity = with_member(card, R"("conductivity":0)");
        const auto bad_density = replace_once(second, R"("density": 1000)", R"("density": 0)");
        const auto result =
            invalid(wrap(bad_conductivity + "," + bad_density), "materials[0].conductivity");
        require(has_issue(result, "materials[1].density"), "later card errors must be collected");
    });
    run("normalize_material_path", [&] {
        const auto bad = with_member(second, R"("conductivity":true)");
        require(has_issue(validate_material_json(bad), "material.conductivity"),
                "fixture must exercise the single-card material prefix");
        const auto result = invalid(wrap(card + "," + bad), "materials[1].conductivity");
        require(!has_issue(result, "materials[1].material", true),
                "material prefix must be replaced, not nested");
    });
    run("duplicate_id_invalid_card", [&] {
        const auto bad = replace_once(card, R"("density": 1000)", R"("density": 0)");
        const auto result = invalid(wrap(bad + "," + card), "materials[0].density");
        require(has_issue(result, "materials[1].id"),
                "an invalid earlier card must still participate in duplicate id detection");
    });
    run("modulus_ramp_default", [&] {
        const auto duplicate = duplicate_elastic_knots(card);
        require(!validate_material_json(duplicate).ok,
                "derived fixture must be rejected by the default single-card importer");
        invalid(wrap(duplicate), "materials[0]", true);
    });
    run("modulus_ramp_options", [&] {
        const auto duplicate = duplicate_elastic_knots(card);
        const auto other = replace_once(duplicate, R"("illustrative")", R"("illustrative_2")");
        const auto json = wrap(duplicate + "," + other);
        const ImportOptions options{true, 2};
        const auto direct = parse_material_json(duplicate, false, options);
        require(direct.elastic_table.temperatures() == std::vector<Real>({249, 251}) &&
                    direct.warning_codes ==
                        std::vector<WarningCode>({WarningCode::ModulusRampApplied}),
                "derived fixture must exercise the existing modulus ramp");
        const auto result = validate_material_library_json(json, false, options);
        valid(result);
        const auto parsed = parse_material_library_json(json, false, options);
        require(parsed.size() == 2, "ramp options must preserve both cards");
        same_card(parsed[0], direct);
        same_card(parsed[1], parse_material_json(other, false, options));
        auto warnings = direct.warning_codes;
        warnings.insert(warnings.end(), direct.warning_codes.begin(), direct.warning_codes.end());
        require(result.warning_codes == warnings,
                "library validation must append warnings in card order and retain repetitions");
    });
    return failures == 0 ? 0 : 1;
}
