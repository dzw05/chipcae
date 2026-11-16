#include "chipcae/material/schema.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <type_traits>

using namespace chipcae::material;
void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::string read(const std::string& path) {
    std::ifstream file(path);
    check(bool(file), "fixture readable");
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}
void hashes() {
    auto elastic = ThermoElastic<Kokkos::HostSpace>{}.layout();
    check(elastic.n_reals == 0 && elastic.layout_hash == 8856394230026893734ull,
          "thermo_elastic golden layout");
    for (auto mode : {VolumetricMode::ElasticBulk, VolumetricMode::ConstantPoissonFromE}) {
        PronyHostParams card{mode, 2, .3, {{3, 1}}};
        PronyViscoelastic<Kokkos::HostSpace> model(card, TemperatureTable({300}, {{5}}));
        auto layout = model.layout();
        bool bulk = mode == VolumetricMode::ElasticBulk;
        check(layout.n_reals == (bulk ? 14 : 15) && layout.fields.size() == (bulk ? 4 : 5) &&
                  layout.layout_hash == (bulk ? 2854699465189332510ull : 5908853098120598084ull),
              "prony golden layout");
    }
    auto shrinkage = EquivalentCureShrinkage{}.layout();
    check(shrinkage.n_reals == 1 && shrinkage.layout_hash == 12923108040779221688ull,
          "shrinkage golden layout");
}
void ramps(const std::string& fixtures) {
    for (auto filename : {"modulus_duplicate.json", "duplicate_bulk.json"}) {
        auto json = read(fixtures + "/" + filename);
        check(!validate_material_json(json).ok, "duplicate modulus rejected by default");
        check(!validate_material_json(json, false, {true, .999}).ok, "ramp below 1 K rejected");
        auto fractional = parse_material_json(json, false, {true, 1.2});
        auto& ft = fractional.kind == ModelKind::ThermoElastic ? fractional.elastic_table
                                                               : fractional.bulk_table;
        check(ft.temperatures()[1] - ft.temperatures()[0] >= 1.2,
              "fractional ramp width rounds upward");
        auto result = validate_material_json(json, false, {true, 1});
        check(result.ok && result.warning_codes.size() == 1 &&
                  result.warning_codes[0] == WarningCode::ModulusRampApplied,
              "validation exposes coded ramp warning");
        auto p = parse_material_json(json, false, {true, 1});
        auto& table = p.kind == ModelKind::ThermoElastic ? p.elastic_table : p.bulk_table;
        check(table.strictly_increasing() && table.temperatures()[1] - table.temperatures()[0] >= 1,
              "imported ramp spacing");
        check(p.warning_codes.size() == 1 && p.warning_codes[0] == WarningCode::ModulusRampApplied,
              "card exposes coded ramp warning");
        check(table.columns()[0][0] == (p.kind == ModelKind::ThermoElastic ? 1e9 : 4e9) &&
                  table.columns()[0][1] == (p.kind == ModelKind::ThermoElastic ? 2e9 : 4e9),
              "ramp preserves values");
    }
    // Multiple jumps separated by knots closer than the requested ramp width.
    auto json = read(fixtures + "/modulus_duplicate.json");
    auto start = json.find("\"temperatures\"");
    auto left = json.find('[', start), right = json.find(']', left);
    json.replace(left, right - left + 1, "[250,250,250.25,250.25,250.5]");
    for (auto name : {"E", "nu"}) {
        start = json.find(std::string("\"") + name + "\"");
        left = json.find('[', start);
        right = json.find(']', left);
        json.replace(left,
                     right - left + 1,
                     std::string(name) == "E" ? "[1e9,2e9,3e9,4e9,5e9]" : "[0.3,0.3,0.3,0.3,0.3]");
    }
    auto p = parse_material_json(json, false, {true, 2});
    check(p.elastic_table.temperatures() == std::vector<Real>({250, 252, 252.25, 254.25, 254.5}),
          "later knots shift and multiple ramps remain increasing");
    check(p.warning_codes.size() == 2, "each ramp emits coded event");
    check(p.parameter_hash != parse_material_json(json, false, {true, 1}).parameter_hash,
          "ramp width affects parameter identity");
    // A CTE jump is untouched by the modulus import option.
    json = read(fixtures + "/modulus_duplicate.json");
    start = json.find("\"temperatures\"", json.find("\"thermal_expansion\""));
    left = json.find('[', start);
    right = json.find(']', left);
    json.replace(left, right - left + 1, "[300,300]");
    check(parse_material_json(json, false, {true, 1}).cte_table.has_duplicate_temperatures(),
          "CTE jump preserved");
}
template <class MS>
void spaces() {
    static_assert(std::is_same_v<typename decltype(TableView<MS>{}.t)::memory_space, MS>);
    static_assert(
        std::is_same_v<typename decltype(std::declval<PronyParams<MS>>().g_)::memory_space, MS>);
    MaterialParams p;
    p.elastic_table = TemperatureTable({250, 400}, {{1e9, 2e9}, {.3, .3}});
    check(bool(make_material<MS>(p)), "factory exported for memory space");
    p.kind = ModelKind::PronyViscoelastic;
    p.prony = PronyHostParams{VolumetricMode::ElasticBulk, 2, .3, {{3, 1}}};
    p.bulk_table = TemperatureTable({250, 400}, {{5, 5}});
    p.shift = ShiftParams{ShiftType::Table, 300, 300, 0, 0, 0, {250, 400}, {0, 0}};
    check(bool(make_material<MS>(p)), "Prony factory and table shift construction");
}
int main(int argc, char** argv) {
    Kokkos::ScopeGuard guard(argc, argv);
    try {
        hashes();
        ramps(argv[1]);
        spaces<Kokkos::HostSpace>();
        spaces<Kokkos::DefaultExecutionSpace::memory_space>();
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
