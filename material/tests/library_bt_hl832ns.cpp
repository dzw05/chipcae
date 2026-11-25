#include "chipcae/material/schema.hpp"

#include <Kokkos_Core.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <jsoncons/json.hpp>
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
    require(bool(file), "material library fixture must be readable");
    std::ostringstream text;
    text << file.rdbuf();
    require(!file.bad() && bool(text), "material library fixture read must succeed");
    return text.str();
}

} // namespace

int main(int argc, char** argv) {
    try {
        Kokkos::ScopeGuard guard(argc, argv);
        require(argc == 2, "expected one material library path");
        const std::filesystem::path path = argv[1];
        const auto json = read(path);
        for (const bool delivery : {false, true}) {
            const auto result = validate_material_library_file(path.string(), delivery);
            require(result.ok && result.issues.empty(),
                    "library must validate without issues in both delivery modes");
        }

        const auto cards = parse_material_library_json(json, true);
        require(cards.size() == 1, "library must contain exactly one card");
        const auto& card = cards[0];
        require(card.id == "bt_hl832ns_eq", "material id must be bt_hl832ns_eq");
        require(card.id.ends_with("_eq"), "material id must end with _eq");
        require(card.kind == ModelKind::ThermoElastic, "model must be thermo_elastic");
        // Exact decoded literals are required by this data-card acceptance brief.
        require(card.conductivity.has_value(), "conductivity must be present");
        require(*card.conductivity == 0.6, "conductivity must be 0.6 W/(m K)");
        require(card.density == 1900, "density must be 1900 kg/m3");
        require(card.thermal.T_sf == 298.15, "stress-free temperature must be 298.15 K");
        require(card.elastic_symmetry == ElasticSymmetry::Isotropic,
                "elastic symmetry must be isotropic");
        require(card.thermal.symmetry == ThermalSymmetry::Isotropic,
                "thermal expansion symmetry must be isotropic");
        require(card.thermal.cte_type == CteType::Tangent, "CTE type must be tangent");

        const auto& elastic = card.elastic_table;
        require(elastic.num_temperatures() == 1 && elastic.num_columns() == 2,
                "elastic table must have one temperature and two columns");
        require(elastic.temperatures() == std::vector<Real>{298.15},
                "elastic temperature must be 298.15 K");
        require(elastic.columns()[0][0] == 27e9, "E must be 27e9 Pa");
        require(elastic.columns()[1][0] == 0.18, "Poisson ratio must be 0.18");
        require(elastic.extrapolation() == Extrapolation::Clamp,
                "elastic extrapolation must clamp");
        // The importer normalizes a legal single-point constant table to Linear.
        require(elastic.interpolation() == Interpolation::Linear,
                "single-point elastic interpolation must normalize to linear");

        const auto& cte = card.cte_table;
        require(cte.num_temperatures() == 1 && cte.num_columns() == 1,
                "CTE table must have one temperature and one column");
        require(cte.temperatures() == std::vector<Real>{298.15},
                "CTE temperature must be 298.15 K");
        require(cte.columns()[0][0] == 10e-6, "alpha must be 10e-6 1/K");
        require(cte.extrapolation() == Extrapolation::Clamp, "CTE extrapolation must clamp");
        require(cte.interpolation() == Interpolation::Linear,
                "single-point CTE interpolation must normalize to linear");

        // MaterialParams does not retain provenance or the raw interpolation declaration.
        // Inspect the decoded fixture after public API JSON and semantic validation.
        const auto document = jsoncons::json::parse(json);
        const auto& material = document.at("materials").at(0);
        require(material.at("elastic").at("table").at("interpolation").as<std::string>() ==
                    "constant",
                "raw elastic interpolation must be constant");
        require(
            material.at("thermal_expansion").at("table").at("interpolation").as<std::string>() ==
                "constant",
            "raw CTE interpolation must be constant");
        const auto& provenance = material.at("provenance");
        require(provenance.at("source_type").as<std::string>() == "datasheet",
                "provenance source_type must be datasheet");
        require(provenance.at("confidence").as<std::string>() == "low",
                "provenance confidence must be low");
        const auto reference = provenance.at("reference").as<std::string>();
        require(reference.find("HL832NS") != std::string_view::npos,
                "reference must identify the HL832NS grade");
        require(reference.find("mgc.co.jp/eng/products/sc/btprint/lineup/hfbt.html") !=
                    std::string_view::npos,
                "reference must contain the manufacturer's source URL");
        const auto notes = provenance.at("notes").as<std::string>();
        require(notes.starts_with("近似（典型值）："), "notes must start with 近似（典型值）：");
        require(notes.find("Z 向 CTE") != std::string_view::npos,
                "notes must state the Z-direction CTE limitation");
        require(notes.find("230") != std::string_view::npos,
                "notes must state the 230 C applicability limit");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL material_library_bt_hl832ns: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "FAIL material_library_bt_hl832ns: unknown exception\n");
    }
    return 1;
}
