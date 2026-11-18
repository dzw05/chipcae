#include "chipcae/material/schema.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <utility>

using namespace chipcae::material;

namespace {
int fail(const char* name) {
    std::fprintf(stderr, "FAIL material_conductivity: %s\n", name);
    return 1;
}

std::string with_members(const std::string& card, const std::string& members) {
    auto result = card;
    result.insert(result.rfind('}'), "," + members);
    return result;
}
} // namespace

int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    const char* name = "present_value";
    try {
        const std::string card =
            R"({"id":"test","schema_version":"1","unit_system":"SI","density":1000,"model":"thermo_elastic","provenance":{"source_type":"illustrative","confidence":"illustrative"},"elastic":{"symmetry":"isotropic","table":{"temperatures":[250,400],"columns":{"E":[1e9,2e9],"nu":[0.3,0.3]},"interpolation":"linear","extrapolation":"error"}},"thermal_expansion":{"cte_type":"tangent","symmetry":"isotropic","stress_free_temperature":300,"table":{"temperatures":[250,400],"columns":{"alpha":[1e-5,2e-5]},"interpolation":"linear","extrapolation":"error"}},"chemical_shrinkage":{"model":"equivalent_isotropic","post_gel_shrinkage":{"value":0.002,"measure":"volumetric"},"schedule":{"cure_step_ids":["cure"]}}})";

        for (const auto& [value, expected] : {std::pair{"148", 148.0}, std::pair{"0.5", 0.5}}) {
            const auto input = with_members(card, std::string(R"("conductivity":)") + value);
            if (!validate_material_json(input).ok)
                return fail(name);
            const auto parsed = parse_material_json(input);
            if (!parsed.conductivity.has_value() || *parsed.conductivity != expected)
                return fail(name);
        }

        name = "absent";
        if (!validate_material_json(card).ok || parse_material_json(card).conductivity.has_value())
            return fail(name);

        name = "nonpositive";
        for (const auto* value : {"0", "-1"}) {
            const auto result = validate_material_json(
                with_members(card, std::string(R"("conductivity":)") + value));
            if (result.ok || result.issues.size() != 1 ||
                result.issues[0].field_path != "conductivity" ||
                result.issues[0].message != "conductivity: must be positive")
                return fail(name);
        }

        name = "non_number";
        for (const auto* value : {"true", R"("abc")", "null"}) {
            const auto result = validate_material_json(
                with_members(card, std::string(R"("conductivity":)") + value));
            if (result.ok || result.issues.size() != 1 ||
                result.issues[0].field_path != "material.conductivity" ||
                result.issues[0].message != "material.conductivity: expected finite number")
                return fail(name);
        }

        name = "numeric_string";
        const auto numeric_string = with_members(card, R"("conductivity":"3.1")");
        if (!validate_material_json(numeric_string).ok)
            return fail(name);
        const auto parsed_string = parse_material_json(numeric_string);
        if (!parsed_string.conductivity.has_value() || *parsed_string.conductivity != 3.1)
            return fail(name);

        name = "duplicate_key";
        const auto duplicate =
            validate_material_json(with_members(card, R"("conductivity":1,"conductivity":2)"));
        bool duplicate_message = false;
        for (const auto& issue : duplicate.issues)
            duplicate_message =
                duplicate_message ||
                issue.message.find("conductivity: duplicate key") != std::string::npos;
        if (duplicate.ok || !duplicate_message)
            return fail(name);

        name = "case_variant";
        const auto variant = with_members(card, R"("Conductivity":1)");
        if (!validate_material_json(variant).ok ||
            parse_material_json(variant).conductivity.has_value())
            return fail(name);

        name = "hash_unchanged";
        std::uint64_t expected_hash = 14695981039346656037ull;
        for (unsigned char byte : card) {
            expected_hash ^= byte;
            expected_hash *= 1099511628211ull;
        }
        if (parse_material_json(card).parameter_hash != expected_hash)
            return fail(name);

        name = "empty_input";
        if (validate_material_json("").ok)
            return fail(name);
    } catch (const std::exception&) {
        return fail(name);
    } catch (...) {
        return fail(name);
    }
    return 0;
}
