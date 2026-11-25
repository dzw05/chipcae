#include "chipcae/material/schema.hpp"

#include <Kokkos_Core.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <jsoncons/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
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
        require(argc == 3, "expected library and demo library paths");
        const std::filesystem::path path = argv[1];
        const std::filesystem::path demo_path = argv[2];
        const auto json = read(path);
        const auto demo_json = read(demo_path);
        for (const bool delivery : {false, true}) {
            const auto result = validate_material_library_file(path.string(), delivery);
            require(result.ok && result.issues.empty(),
                    "library must validate without issues in both delivery modes");
        }

        const auto cards = parse_material_library_json(json, true);
        require(cards.size() == 1, "library must contain exactly one card");
        const auto& card = cards[0];
        require(card.id == "copper", "material id must be copper");
        require(card.kind == ModelKind::ThermoElastic, "model must be thermo_elastic");
        require(card.elastic_symmetry == ElasticSymmetry::Isotropic,
                "elastic symmetry must be isotropic");
        require(card.thermal.symmetry == ThermalSymmetry::Isotropic,
                "thermal expansion symmetry must be isotropic");
        require(card.thermal.cte_type == CteType::Tangent, "CTE type must be tangent");

        // Exact floating-point equality is intentional: the brief locks decoded data-card
        // literals and their copied values, rather than computed numerical results.
        require(card.conductivity.has_value(), "conductivity must be present");
        require(*card.conductivity == 394, "conductivity must be 394 W/(m K)");
        require(card.density == 8940, "density must be 8940 kg/m3");
        require(card.thermal.T_sf == 298.15, "stress-free temperature must be 298.15 K");

        const auto& elastic = card.elastic_table;
        require(elastic.num_temperatures() == 2 && elastic.num_columns() == 2,
                "elastic table must have two temperatures and two columns");
        require(elastic.temperatures() == std::vector<Real>{250, 400},
                "elastic temperatures must be 250 K and 400 K");
        require(elastic.columns()[0] == std::vector<Real>{127e9, 127e9},
                "E must be 127e9 Pa at both temperatures");
        require(elastic.columns()[1] == std::vector<Real>{0.34, 0.34},
                "Poisson ratio must be 0.34 at both temperatures");
        require(elastic.interpolation() == Interpolation::Linear,
                "elastic interpolation must be linear");
        require(elastic.extrapolation() == Extrapolation::Clamp,
                "elastic extrapolation must clamp");

        const auto& cte = card.cte_table;
        require(cte.num_temperatures() == 2 && cte.num_columns() == 1,
                "CTE table must have two temperatures and one column");
        require(cte.temperatures() == std::vector<Real>{250, 400},
                "CTE temperatures must be 250 K and 400 K");
        require(cte.columns()[0] == std::vector<Real>{16.6e-6, 16.6e-6},
                "alpha must be 16.6e-6 1/K at both temperatures");
        require(cte.interpolation() == Interpolation::Linear, "CTE interpolation must be linear");
        require(cte.extrapolation() == Extrapolation::Clamp, "CTE extrapolation must clamp");

        // Other demo cards intentionally have illustrative provenance.
        const auto demo_cards = parse_material_library_json(demo_json, false);
        std::size_t copper_index = demo_cards.size();
        std::size_t copper_count = 0;
        for (std::size_t i = 0; i < demo_cards.size(); ++i) {
            if (demo_cards[i].id == "copper") {
                copper_index = i;
                ++copper_count;
            }
        }
        require(copper_count == 1, "demo library must contain exactly one copper card");
        const auto& source = demo_cards[copper_index];
        require(card.id == source.id, "copper id must match demo");
        require(card.kind == source.kind, "copper model must match demo");
        require(card.elastic_symmetry == source.elastic_symmetry,
                "copper elastic symmetry must match demo");
        require(card.thermal.symmetry == source.thermal.symmetry,
                "copper thermal symmetry must match demo");
        require(card.thermal.cte_type == source.thermal.cte_type,
                "copper CTE type must match demo");
        require(card.density == source.density, "copper density must match demo");
        require(card.conductivity == source.conductivity, "copper conductivity must match demo");
        require(card.thermal.T_sf == source.thermal.T_sf,
                "copper stress-free temperature must match demo");
        require(elastic.temperatures() == source.elastic_table.temperatures(),
                "copper elastic temperatures must match demo");
        require(elastic.columns() == source.elastic_table.columns(),
                "complete copper elastic columns must match demo");
        require(elastic.interpolation() == source.elastic_table.interpolation(),
                "copper elastic interpolation must match demo");
        require(elastic.extrapolation() == source.elastic_table.extrapolation(),
                "copper elastic extrapolation must match demo");
        require(cte.temperatures() == source.cte_table.temperatures(),
                "copper CTE temperatures must match demo");
        require(cte.columns() == source.cte_table.columns(),
                "complete copper CTE columns must match demo");
        require(cte.interpolation() == source.cte_table.interpolation(),
                "copper CTE interpolation must match demo");
        require(cte.extrapolation() == source.cte_table.extrapolation(),
                "copper CTE extrapolation must match demo");

        // MaterialParams does not retain provenance. Decode only to inspect it after
        // public API validation; library parsing preserves the source array order.
        const auto document = jsoncons::json::parse(json);
        const auto demo_document = jsoncons::json::parse(demo_json);
        const auto& provenance = document.at("materials").at(0).at("provenance");
        const auto& source_provenance =
            demo_document.at("materials").at(copper_index).at("provenance");
        const auto source_type = provenance.at("source_type").as<std::string>();
        const auto confidence = provenance.at("confidence").as<std::string>();
        const auto reference = provenance.at("reference").as<std::string>();
        const auto notes = provenance.at("notes").as<std::string>();
        require(source_type == "literature", "provenance source_type must be literature");
        require(confidence == "medium", "provenance confidence must be medium");
        require(!reference.empty(), "provenance reference must be nonempty");
        require(!notes.empty(), "provenance notes must be nonempty");
        require(notes.starts_with("近似"), "provenance notes must start with 近似");
        require(source_type == source_provenance.at("source_type").as<std::string>(),
                "copper provenance source_type must match demo exactly");
        require(confidence == source_provenance.at("confidence").as<std::string>(),
                "copper provenance confidence must match demo exactly");
        require(reference == source_provenance.at("reference").as<std::string>(),
                "copper provenance reference must match demo exactly");
        require(notes == source_provenance.at("notes").as<std::string>(),
                "copper provenance notes must match demo exactly");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "material_library_copper: %s\n", error.what());
        return 1;
    }
    return 0;
}
