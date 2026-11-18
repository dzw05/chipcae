#include "chipcae/material/schema.hpp"

#include <Kokkos_Core.hpp>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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

std::string replace_once(std::string card, std::string_view from, std::string_view to) {
    require(!from.empty(), "fixture replacement target must not be empty");
    const auto pos = card.find(from);
    require(pos != std::string::npos, "fixture replacement must find its target");
    require(card.find(from, pos + from.size()) == std::string::npos,
            "fixture replacement target must be unique");
    card.replace(pos, from.size(), to);
    return card;
}

std::string with_id(const std::string& card, std::string_view id) {
    return replace_once(card, R"("id": "illustrative")", "\"id\": \"" + std::string(id) + "\"");
}

void valid(const ValidationResult& result) {
    require(result.ok && result.issues.empty(), "expected ok=true and no issues");
}

void invalid(const ValidationResult& result, std::initializer_list<ValidationIssue> expected) {
    bool matches = !result.ok && result.issues.size() == expected.size();
    if (matches) {
        std::size_t i = 0;
        for (const auto& issue : expected) {
            matches = matches && result.issues[i].field_path == issue.field_path &&
                      result.issues[i].message == issue.message;
            ++i;
        }
    }
    if (!matches) {
        std::fprintf(stderr,
                     "  expected ok=false, issues=%zu; actual ok=%s, issues=%zu\n",
                     expected.size(),
                     result.ok ? "true" : "false",
                     result.issues.size());
        std::size_t i = 0;
        for (const auto& issue : expected) {
            std::fprintf(stderr,
                         "  expected issue[%zu]: (%s, %s)\n",
                         i++,
                         issue.field_path.c_str(),
                         issue.message.c_str());
        }
        for (std::size_t n = 0; n < result.issues.size(); ++n) {
            const auto& issue = result.issues[n];
            std::fprintf(stderr,
                         "  actual issue[%zu]: (%s, %s)\n",
                         n,
                         issue.field_path.c_str(),
                         issue.message.c_str());
        }
        throw std::runtime_error("validation must match the expected ordered issues exactly");
    }
}
} // namespace

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    int failures = 0;
    const auto run = [&](const std::string& name, const auto& test) {
        try {
            test();
            std::printf("PASS material_id_pattern: %s\n", name.c_str());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL material_id_pattern: %s\n  %s\n", name.c_str(), e.what());
            ++failures;
        }
    };

    std::string card;
    run("fixtures", [&] {
        require(argc == 2, "expected the fixture directory");
        card = read(std::filesystem::path(argv[1]) / "valid_elastic.json");
        valid(validate_material_json(card));
    });
    if (failures != 0)
        return 1;

    const std::string expected_message =
        "material.id: must match ^[a-z0-9_-]+$ (lowercase letters, digits, '_' or '-'; no dots, "
        "e.g. cu, fr4_eq)";

    run("dotted_card", [&] {
        invalid(validate_material_json(with_id(card, "mat.cu")),
                {{"material.id", expected_message}});
    });
    run("dotted_eq_card", [&] {
        invalid(validate_material_json(with_id(card, "mat.fr4_eq")),
                {{"material.id", expected_message}});
    });
    run("uppercase", [&] {
        invalid(validate_material_json(with_id(card, "Cu")), {{"material.id", expected_message}});
    });
    run("empty", [&] {
        invalid(validate_material_json(with_id(card, "")), {{"material.id", expected_message}});
    });
    run("space", [&] {
        invalid(validate_material_json(with_id(card, "cu fr4")),
                {{"material.id", expected_message}});
    });
    for (const auto* id : {"cu/fr4", "cu:fr4", "铜", "SAC305_ball_eq", ".", "cu."}) {
        run(std::string("bad_ids/") + id, [&] {
            invalid(validate_material_json(with_id(card, id)), {{"material.id", expected_message}});
        });
    }
    run("library_path", [&] {
        const auto library = R"({"schema":"chipcae.material_library/1","materials":[)" +
                             with_id(card, "mat.cu") + "]}";
        invalid(validate_material_library_json(library), {{"materials[0].id", expected_message}});
    });
    run("library_duplicate_dotted", [&] {
        const auto dotted_card = with_id(card, "mat.cu");
        const auto library = R"({"schema":"chipcae.material_library/1","materials":[)" +
                             dotted_card + "," + dotted_card + "]}";
        invalid(validate_material_library_json(library),
                {{"materials[0].id", expected_message},
                 {"materials[1].id", "duplicate material id"},
                 {"materials[1].id", expected_message}});
    });
    for (const auto* id : {"cu", "fr4_eq", "sac305_ball_eq", "cu-1", "a", "0", "_", "-"}) {
        run(std::string("dotless_ok/") + id,
            [&] { valid(validate_material_json(with_id(card, id))); });
    }
    run("id_first", [&] {
        const auto bad_units =
            replace_once(card, R"("unit_system": "SI")", R"("unit_system": "CGS")");
        invalid(validate_material_json(bad_units),
                {{"schema_version/unit_system", "schema_version/unit_system: expected 1/SI"}});
        invalid(validate_material_json(with_id(bad_units, "mat.cu")),
                {{"material.id", expected_message}});
    });
    run("missing_id", [&] {
        invalid(validate_material_json(replace_once(card, R"("id": "illustrative",)", "")),
                {{"material.id", "material.id: required field is missing"}});
    });
    run("numeric_id", [&] {
        invalid(validate_material_json(replace_once(card, R"("id": "illustrative")", R"("id": 7)")),
                {{"material.id", "material.id: expected string"}});
    });

    return failures == 0 ? 0 : 1;
}
