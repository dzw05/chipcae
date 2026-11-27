#include "chipcae/material/schema.hpp"

#include <Kokkos_Core.hpp>
#include <cmath>
#include <cstddef>
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
        require(argc == 2, "expected one material library path");
        const std::filesystem::path path = argv[1];
        for (const bool delivery : {false, true}) {
            const auto result = validate_material_library_file(path.string(), delivery);
            for (const auto& issue : result.issues) {
                std::fprintf(stderr,
                             "validation (%s): %s: %s\n",
                             delivery ? "delivery" : "normal",
                             issue.field_path.c_str(),
                             issue.message.c_str());
            }
            require(result.ok && result.issues.empty(),
                    "silicon library must validate without issues in both delivery modes");
        }

        const auto json = read(path);
        const auto cards = parse_material_library_json(json, true);
        require(cards.size() == 1, "library must contain exactly one card");
        const auto& card = cards[0];
        require(card.id == "silicon", "material id must be silicon");
        require(card.kind == ModelKind::ThermoElastic, "model must be thermo_elastic");
        require(card.elastic_symmetry == ElasticSymmetry::Isotropic,
                "elastic symmetry must be isotropic");
        require(card.thermal.symmetry == ThermalSymmetry::Isotropic,
                "thermal expansion symmetry must be isotropic");
        require(card.thermal.cte_type == CteType::Tangent, "CTE type must be tangent");

        // Exact equality locks the decoded data-card literals. Independently computed
        // literature values below use the rounding tolerances specified by the brief.
        require(card.conductivity.has_value(), "conductivity must be present");
        require(*card.conductivity == 148, "conductivity must be 148 W/(m K)");
        require(card.density == 2329, "density must be 2329 kg/m3");
        require(card.thermal.T_sf == 298.15, "stress-free temperature must be 298.15 K");

        const auto& elastic = card.elastic_table;
        require(elastic.num_temperatures() == 2 && elastic.num_columns() == 2,
                "elastic table must have two temperatures and two columns");
        require(elastic.temperatures() == std::vector<Real>{250, 400},
                "elastic temperatures must be 250 K and 400 K");
        require(elastic.columns()[0] == std::vector<Real>{130e9, 130e9},
                "E must be 130e9 Pa at both temperatures");
        require(elastic.columns()[1] == std::vector<Real>{0.28, 0.28},
                "Poisson ratio must be 0.28 at both temperatures");
        require(elastic.interpolation() == Interpolation::Linear,
                "elastic interpolation must be linear");
        require(elastic.extrapolation() == Extrapolation::Clamp,
                "elastic extrapolation must clamp");

        const auto& cte = card.cte_table;
        require(cte.num_temperatures() == 4 && cte.num_columns() == 1,
                "CTE table must have four temperatures and one column");
        require(cte.temperatures() == std::vector<Real>{250, 300, 350, 400},
                "CTE temperatures must be 250 K, 300 K, 350 K and 400 K");
        require(cte.columns()[0] == std::vector<Real>{2.088e-6, 2.568e-6, 2.933e-6, 3.212e-6},
                "alpha must match the four rounded Okada-Tokumaru samples");
        require(cte.interpolation() == Interpolation::Linear, "CTE interpolation must be linear");
        require(cte.extrapolation() == Extrapolation::Clamp, "CTE extrapolation must clamp");

        // Okada and Tokumaru (1984), DOI 10.1063/1.333965: independent empirical CTE formula.
        for (std::size_t i = 0; i < cte.num_temperatures(); ++i) {
            const Real T = cte.temperatures()[i];
            const Real alpha_ref =
                (3.725 * (1 - std::exp(-5.88e-3 * (T - 124))) + 5.548e-4 * T) * 1e-6;
            require(std::abs(cte.columns()[0][i] - alpha_ref) / alpha_ref <= 5e-4,
                    "each alpha sample must match the literature formula within 5e-4 relative");
        }

        // Eight atoms per cell, Okada-Tokumaru lattice parameter, CIAAW molar mass,
        // and the SI-defined Avogadro constant; none is taken from the material card.
        constexpr Real a = 0.5430741e-9;
        constexpr Real rho_ref = 8 * 28.0855e-3 / (6.02214076e23 * a * a * a);
        require(std::abs(card.density - rho_ref) / rho_ref <= 5e-4,
                "density must match the lattice calculation within 5e-4 relative");

        // Hopcroft et al. (2010), DOI 10.1109/JMEMS.2009.2039697: cubic compliance for <100>.
        constexpr Real C11 = 165.7e9;
        constexpr Real C12 = 63.9e9;
        constexpr Real D = (C11 - C12) * (C11 + 2 * C12);
        constexpr Real S11 = (C11 + C12) / D;
        constexpr Real S12 = -C12 / D;
        constexpr Real E100 = 1 / S11;
        constexpr Real nu100 = -S12 / S11;
        for (std::size_t i = 0; i < elastic.num_temperatures(); ++i) {
            require(std::abs(elastic.columns()[0][i] - E100) / E100 <= 2e-3,
                    "each E value must match cubic compliance within 2e-3 relative");
            require(std::abs(elastic.columns()[1][i] - nu100) <= 3e-3,
                    "each Poisson ratio must match cubic compliance within 3e-3 absolute");
        }

        // MaterialParams does not retain provenance; inspect it after public API validation.
        const auto document = jsoncons::json::parse(json);
        const auto& provenance = document.at("materials").at(0).at("provenance");
        require(provenance.at("source_type").as<std::string>() == "literature",
                "provenance source_type must be literature");
        require(provenance.at("confidence").as<std::string>() == "medium",
                "provenance confidence must be medium");
        const auto reference = provenance.at("reference").as<std::string>();
        require(reference.find("Okada") != std::string::npos,
                "reference must identify Okada and Tokumaru");
        require(reference.find("10.1063/1.333965") != std::string::npos,
                "reference must contain the Okada-Tokumaru DOI");
        require(reference.find("Hopcroft") != std::string::npos,
                "reference must identify Hopcroft");
        require(reference.find("10.1109/JMEMS.2009.2039697") != std::string::npos,
                "reference must contain the Hopcroft DOI");
        require(reference.find("Ho") != std::string::npos, "reference must identify Ho");
        require(reference.find("10.1063/1.3253100") != std::string::npos,
                "reference must contain the Ho-Powell-Liley DOI");
        require(reference.find("0.5430741") != std::string::npos,
                "reference must contain the lattice parameter used for density");
        const auto notes = provenance.at("notes").as<std::string>();
        require(notes.starts_with("近似"), "provenance notes must start with 近似");
        require(notes.find("<100>") != std::string::npos ||
                    notes.find("〈100〉") != std::string::npos,
                "notes must identify the <100> isotropic approximation");
        require(notes.find("300 K") != std::string::npos,
                "notes must state the 300 K reference temperature");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "material_library_silicon: %s\n", error.what());
        return 1;
    }
    return 0;
}
