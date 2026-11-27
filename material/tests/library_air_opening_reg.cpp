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
        require(argc == 3, "expected air and soldermask material library paths");
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
        require(card.id == "air_opening_reg", "material id must be air_opening_reg");
        require(card.kind == ModelKind::ThermoElastic, "model must be thermo_elastic");
        require(card.elastic_symmetry == ElasticSymmetry::Isotropic,
                "elastic symmetry must be isotropic");
        require(card.thermal.symmetry == ThermalSymmetry::Isotropic,
                "thermal expansion symmetry must be isotropic");
        require(card.thermal.cte_type == CteType::Tangent, "CTE type must be tangent");
        // Exact decoded literals are required by this data-card acceptance brief.
        require(card.conductivity.has_value(), "conductivity must be present");
        require(*card.conductivity == 0.02551, "conductivity must be 0.02551 W/(m K)");
        require(card.density == 1.184, "density must be 1.184 kg/m3");
        require(card.thermal.T_sf == 298.15, "stress-free temperature must be 298.15 K");
        // Library validation does not propagate textual modulus warnings into issues.
        require(card.warnings.empty(), "parsed card must have no textual warnings");
        require(card.warning_codes.empty(), "parsed card must have no warning codes");

        const auto& elastic = card.elastic_table;
        require(elastic.num_temperatures() == 1 && elastic.num_columns() == 2,
                "elastic table must have one temperature and two columns");
        require(elastic.temperatures() == std::vector<Real>{298.15},
                "elastic temperature must be 298.15 K");
        require(elastic.columns()[0] == std::vector<Real>{2.7e6}, "E must be 2.7e6 Pa");
        require(elastic.columns()[1] == std::vector<Real>{0}, "Poisson ratio must be zero");
        // The importer normalizes a legal single-point constant table to Linear.
        require(elastic.interpolation() == Interpolation::Linear,
                "single-point elastic interpolation must normalize to linear");
        require(elastic.extrapolation() == Extrapolation::Clamp,
                "elastic extrapolation must clamp");

        const auto& cte = card.cte_table;
        require(cte.num_temperatures() == 1 && cte.num_columns() == 1,
                "CTE table must have one temperature and one column");
        require(cte.temperatures() == std::vector<Real>{298.15},
                "CTE temperature must be 298.15 K");
        require(cte.columns()[0] == std::vector<Real>{0}, "alpha must be zero");
        require(cte.interpolation() == Interpolation::Linear,
                "single-point CTE interpolation must normalize to linear");
        require(cte.extrapolation() == Extrapolation::Clamp, "CTE extrapolation must clamp");

        const auto source_cards = parse_material_library_json(read(argv[2]), true);
        std::size_t source_index = source_cards.size();
        std::size_t source_count = 0;
        for (std::size_t i = 0; i < source_cards.size(); ++i) {
            if (source_cards[i].id == "soldermask_lpi_eq") {
                source_index = i;
                ++source_count;
            }
        }
        require(source_count == 1, "source library must contain exactly one soldermask_lpi_eq");
        const auto& source = source_cards[source_index];
        require(source.elastic_symmetry == ElasticSymmetry::Isotropic,
                "source elastic symmetry must be isotropic");
        const auto& source_elastic = source.elastic_table;
        require(source_elastic.num_temperatures() == 1 && source_elastic.num_columns() == 2,
                "source elastic table must have one temperature and two columns");
        require(source_elastic.columns()[0].size() == 1,
                "source elastic table must have exactly one E entry");
        const Real source_E = source_elastic.columns()[0][0];
        require(source_E == 2.7e9, "source E must remain 2.7e9 Pa");
        require(elastic.columns()[0][0] == source_E / 1000.0,
                "air regularization E must equal soldermask E / 1000.0");

        // MaterialParams does not retain provenance. Inspect the decoded fixture after
        // the library APIs above have performed JSON and semantic validation.
        const auto document = jsoncons::json::parse(json);
        const auto& provenance = document.at("materials").at(0).at("provenance");
        require(provenance.at("source_type").as<std::string>() == "literature",
                "provenance source_type must be literature");
        require(provenance.at("confidence").as<std::string>() == "medium",
                "provenance confidence must be medium");
        const auto reference = provenance.at("reference").as<std::string>();
        for (const auto* source : {"Table A-9", "25", "me.psu.edu", "Touloukian", "正则化"})
            require(reference.find(source) != std::string_view::npos,
                    "reference must contain the air-property sources and regularization basis");
        const auto notes = provenance.at("notes").as<std::string>();
        require(notes.starts_with("近似"), "notes must start with 近似");
        for (const auto* limitation : {"数值正则化", "1007", "不是物理值"})
            require(notes.find(limitation) != std::string_view::npos,
                    "notes must record specific heat and nonphysical numerical regularization");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL material_library_air_opening_reg: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "FAIL material_library_air_opening_reg: unknown exception\n");
    }
    return 1;
}
