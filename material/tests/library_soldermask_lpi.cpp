#include "chipcae/material/schema.hpp"

#include <Kokkos_Core.hpp>
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
    require(bool(file), "material library fixture must be readable");
    std::ostringstream text;
    text << file.rdbuf();
    require(!file.bad() && bool(text), "material library fixture read must succeed");
    return text.str();
}

// MaterialParams does not retain provenance. Check literal, single-line fields in this
// controlled fixture; the existing library APIs perform JSON and semantic validation.
std::string_view fixture_line(std::string_view text, std::string_view prefix) {
    const auto start = text.find(prefix);
    require(start != std::string_view::npos, "expected literal provenance field is missing");
    const auto value = text.substr(start + prefix.size());
    return value.substr(0, value.find('\n'));
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
        require(card.id == "soldermask_lpi_eq", "material id must be soldermask_lpi_eq");
        require(card.kind == ModelKind::ThermoElastic, "model must be thermo_elastic");
        // Exact decoded literals are required by this data-card acceptance brief.
        require(card.conductivity.has_value(), "conductivity must be present");
        require(*card.conductivity == 0.23, "conductivity must be 0.23 W/(m K)");
        require(card.density == 1390, "density placeholder must be 1390 kg/m3");
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
        require(elastic.columns()[0][0] == 2.7e9, "E must be 2.7e9 Pa");
        require(elastic.columns()[1][0] == 0.3, "Poisson ratio must be 0.3");
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
        require(cte.columns()[0][0] == 55e-6, "alpha must be 55e-6 1/K");
        require(cte.extrapolation() == Extrapolation::Clamp, "CTE extrapolation must clamp");
        require(cte.interpolation() == Interpolation::Linear,
                "single-point CTE interpolation must normalize to linear");

        require(json.find(R"("source_type": "literature")") != std::string::npos,
                "provenance source_type must be literature");
        require(json.find(R"("confidence": "low")") != std::string::npos,
                "provenance confidence must be low");
        const auto reference = fixture_line(json, R"("reference": ")");
        for (const auto* source : {"13_5_396", "oar.a-star.edu.sg", "PSR-4000BN", "非固化膜密度"})
            require(reference.find(source) != std::string_view::npos,
                    "reference must contain all three sources and the density limitation");
        const auto notes = fixture_line(json, R"("notes": ")");
        require(notes.starts_with("近似"), "notes must start with 近似");
        require(notes.find("非固化膜密度") != std::string_view::npos,
                "notes must state that density is not cured-film density");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL material_library_soldermask_lpi: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "FAIL material_library_soldermask_lpi: unknown exception\n");
    }
    return 1;
}
