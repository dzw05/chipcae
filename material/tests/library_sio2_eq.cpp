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
                    "sio2_eq library must validate without issues in both delivery modes");
        }

        const auto json = read(path);
        const auto cards = parse_material_library_json(json, true);
        require(cards.size() == 1, "library must contain exactly one card");
        const auto& card = cards[0];
        require(card.id == "sio2_eq", "material id must be sio2_eq");
        require(card.kind == ModelKind::ThermoElastic, "model must be thermo_elastic");
        require(card.elastic_symmetry == ElasticSymmetry::Isotropic,
                "elastic symmetry must be isotropic");
        require(card.thermal.symmetry == ThermalSymmetry::Isotropic,
                "thermal expansion symmetry must be isotropic");
        require(card.thermal.cte_type == CteType::Tangent, "CTE type must be tangent");

        // Exact equality locks decoded data-card literals as required by the brief.
        // Independent unit conversions and the E-G-nu check below use its tolerances.
        require(card.conductivity.has_value(), "conductivity must be present");
        require(*card.conductivity == 1.30, "conductivity must be 1.30 W/(m K)");
        require(card.density == 2201, "density must be 2201 kg/m3");
        require(card.thermal.T_sf == 298.15, "stress-free temperature must be 298.15 K");

        const auto& elastic = card.elastic_table;
        require(elastic.num_temperatures() == 2 && elastic.num_columns() == 2,
                "elastic table must have two temperatures and two columns");
        require(elastic.temperatures() == std::vector<Real>{250, 400},
                "elastic temperatures must be 250 K and 400 K");
        require(elastic.columns()[0] == std::vector<Real>{72.7e9, 72.7e9},
                "E must be 72.7e9 Pa at both temperatures");
        require(elastic.columns()[1] == std::vector<Real>{0.16, 0.16},
                "Poisson ratio must be 0.16 at both temperatures");
        require(elastic.interpolation() == Interpolation::Linear,
                "elastic interpolation must be linear");
        require(elastic.extrapolation() == Extrapolation::Clamp,
                "elastic extrapolation must clamp");

        const auto& cte = card.cte_table;
        require(cte.num_temperatures() == 2 && cte.num_columns() == 1,
                "CTE table must have two temperatures and one column");
        require(cte.temperatures() == std::vector<Real>{250, 400},
                "CTE temperatures must be 250 K and 400 K");
        require(cte.columns()[0] == std::vector<Real>{5.2e-7, 5.2e-7},
                "alpha must be 5.2e-7 1/K at both temperatures");
        require(cte.interpolation() == Interpolation::Linear, "CTE interpolation must be linear");
        require(cte.extrapolation() == Extrapolation::Clamp, "CTE extrapolation must clamp");

        // Corning HPFS Fused Silica Standard Grade (7980), September 30, 2003,
        // p. 2: independent original units, not values inferred from the parsed card.
        constexpr Real rho_ref = 2.201 * 1000;
        require(std::abs(card.density - rho_ref) <= 1e-9,
                "density must match 2.201 g/cm3 within 1e-9 kg/m3 absolute");
        constexpr Real alpha_ref = 0.52 * 1e-6;
        for (std::size_t i = 0; i < cte.num_temperatures(); ++i) {
            require(std::abs(cte.columns()[0][i] - alpha_ref) / alpha_ref <= 1e-12,
                    "each alpha must match 0.52 ppm/K within 1e-12 relative");
        }

        constexpr Real E_ref = 72.7 * 1e9;
        constexpr Real G_ref = 31.4e9;
        // Printed-rounding budget only: nu +/-0.005, G +/-0.05 GPa, E +/-0.05 GPa.
        // 0.005 + 1.843e-3 + 7.96e-4 ~= 7.639e-3; round upward to 7.7e-3.
        // Unit-conversion tolerances cover representation; neither covers algorithm error.
        for (std::size_t i = 0; i < elastic.num_temperatures(); ++i) {
            const Real E = elastic.columns()[0][i];
            const Real nu = elastic.columns()[1][i];
            require(std::abs(E - E_ref) / E_ref <= 1e-12,
                    "each E must match 72.7 GPa within 1e-12 relative");
            const Real nu_G = E / (2 * G_ref) - 1;
            require(std::abs(nu - nu_G) <= 7.7e-3,
                    "each Poisson ratio must match E/(2G)-1 within 7.7e-3 absolute");
        }

        // MaterialParams does not retain provenance; inspect it after public API validation.
        const auto document = jsoncons::json::parse(json);
        const auto& provenance = document.at("materials").at(0).at("provenance");
        require(provenance.at("source_type").as<std::string>() == "literature",
                "provenance source_type must be literature");
        require(provenance.at("confidence").as<std::string>() == "low",
                "provenance confidence must be low");
        const auto reference = provenance.at("reference").as<std::string>();
        for (const auto* token : {"Corning",
                                  "7980",
                                  "HPFS",
                                  "2003",
                                  "tecsam.co.jp/products/tec_info/pdf/02-1.pdf",
                                  "1.30",
                                  "2.201",
                                  "72.7",
                                  "0.16",
                                  "31.4",
                                  "0.52"}) {
            require(reference.find(token) != std::string::npos,
                    ("reference must contain " + std::string(token)).c_str());
        }
        const auto notes = provenance.at("notes").as<std::string>();
        require(notes.starts_with("近似"), "provenance notes must start with 近似");
        for (const auto* token : {"5", "35", "0.57", "0.48", "298.15"}) {
            require(notes.find(token) != std::string::npos,
                    ("notes must contain " + std::string(token)).c_str());
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "material_library_sio2_eq: %s\n", error.what());
        return 1;
    }
    return 0;
}
