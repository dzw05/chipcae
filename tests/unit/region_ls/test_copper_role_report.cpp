#include "support.hpp"

#include <bit>
#include <chrono>
#include <cstdint>
#include <sys/resource.h>

namespace {
using namespace chipcae::mesh;
using sp5::require;
using Report = HexMeshArrays::GridReport;

static_assert(std::tuple_size_v<Report> == 11);

StackModel fraction_stack() {
    auto s = sp5::stack();
    s.layers[0].name = "core";
    s.layers[0].material = "fr4_eq";
    for (const auto& [name, fraction] : {std::pair{"A", .25}, std::pair{"B", .6}}) {
        Layer layer;
        layer.name = name;
        layer.kind = "Patterned";
        layer.material = "fr4_eq";
        layer.thickness = .5;
        layer.n_elem_min = 1;
        layer.copper = CopperSpec{};
        layer.copper->metal = "cu";
        layer.copper->fraction = fraction;
        s.layers.push_back(layer);
    }
    return s;
}

template <class Map>
std::set<std::string> keys(const Map& map) {
    std::set<std::string> result;
    for (const auto& [name, area] : map) {
        (void)area;
        result.insert(name);
    }
    return result;
}

void same_area(const Report::CopperArea& actual, const Report::CopperArea& expected) {
    require(std::bit_cast<std::uint64_t>(actual.polygons) ==
                std::bit_cast<std::uint64_t>(expected.polygons),
            "polygon area bits changed");
    require(std::bit_cast<std::uint64_t>(actual.raster) ==
                std::bit_cast<std::uint64_t>(expected.raster),
            "raster area bits changed");
}

void acceptance() {
    auto s = fraction_stack();
    const auto baseline = sp5::grid(s);
    const auto& base = baseline.grid_report;
    require(keys(base.copper_area_m2) == std::set<std::string>{"A", "B"},
            "default roles must both report copper");
    require(base.opening_area_m2.empty(), "default roles must have no opening areas");
    require(&get<0>(base) == &base.levels, "I5 first binding changed");
    require(&get<10>(base) == &base.void_cells, "I5 last binding changed");

    s.layers[2].copper->role = "opening";
    const auto routed = sp5::grid(s);
    const auto& report = routed.grid_report;
    require(keys(report.copper_area_m2) == std::set<std::string>{"A"},
            "opening B must be excluded from copper_area_m2");
    require(keys(report.opening_area_m2) == std::set<std::string>{"B"},
            "opening B must appear in opening_area_m2");
    same_area(report.opening_area_m2.at("B"), base.copper_area_m2.at("B"));
    same_area(report.copper_area_m2.at("A"), base.copper_area_m2.at("A"));
    require(routed.copper_layer_of_layer == baseline.copper_layer_of_layer,
            "role must not change copper layer indices");
    require(routed.copper_phi.size() == baseline.copper_phi.size(), "phi size changed");
    for (std::size_t i = 0; i < routed.copper_phi.size(); ++i)
        require(std::bit_cast<std::uint64_t>(routed.copper_phi[i]) ==
                    std::bit_cast<std::uint64_t>(baseline.copper_phi[i]),
                "role must not change phi bits");

    s = fraction_stack();
    s.layers[1].copper->orientation = "vertical";
    const auto vertical = sp5::grid(s);
    require(keys(vertical.grid_report.copper_area_m2) == std::set<std::string>{"A", "B"},
            "vertical metal must remain copper");
    require(vertical.grid_report.opening_area_m2.empty(), "vertical metal is not opening");
    same_area(vertical.grid_report.copper_area_m2.at("A"), base.copper_area_m2.at("A"));

    s = fraction_stack();
    s.layers[1].copper->role = "opening";
    s.layers[2].copper->role = "opening";
    const auto all = sp5::grid(s);
    require(all.grid_report.copper_area_m2.empty(), "all-opening stack must have no copper areas");
    require(keys(all.grid_report.opening_area_m2) == std::set<std::string>{"A", "B"},
            "all-opening stack must report both openings");

    s = fraction_stack();
    for (auto& layer : s.layers)
        layer.copper.reset();
    const auto none = sp5::grid(s);
    require(none.grid_report.copper_area_m2.empty(), "no-copper copper report must be empty");
    require(none.grid_report.opening_area_m2.empty(), "no-copper opening report must be empty");
    require(none.copper_phi.empty(), "no-copper phi must be empty");

    for (const std::string role : {"Opening", "METAL", ""}) {
        s = fraction_stack();
        s.layers[2].copper->role = role;
        GridOptions options;
        options.nx = options.ny = 4;
        options.levels = 1;
        auto result = build_tensor_grid(s, options);
        require(!result, "unsupported role must fail");
        require(result.error().path == "copper", "unsupported role error path changed");
        require(result.error().reason == "unsupported copper role: " + role,
                "unsupported role must be preserved in exact reason");
    }
}
} // namespace

int main() {
    const auto start = std::chrono::steady_clock::now();
    const int status = sp5::run(acceptance);
    rusage usage{};
    const int measured = getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
    constexpr double rss_divisor = 1024. * 1024.;
#else
    constexpr double rss_divisor = 1024.;
#endif
    std::cout << "MEASURE wall_s="
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
              << " rss_mib=" << static_cast<double>(usage.ru_maxrss) / rss_divisor << '\n';
    return measured == 0 ? status : 1;
}
