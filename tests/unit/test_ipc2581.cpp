#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
int main() {
    namespace ecad = chipcae::geometry::ecad;
    const auto root =
        std::filesystem::path(CHIPCAE_TEST_SOURCE_DIR) / "tests/integration/ecad/data";
    auto a = ecad::read_ipc2581(root / "board_2layer.xml");
    if (!a || a.model.layers.size() != 3 || a.model.outline.points.size() != 4)
        return 1;
    const double expected[] = {35e-6, 1.6e-3, 35e-6};
    for (std::size_t i = 0; i < 3; ++i)
        if (std::abs(a.model.layers[i].thickness_m - expected[i]) > 1e-15)
            return 2;
    if (a.model.layers[0].polygons.size() != 1 || a.model.layers[2].polygons.size() != 1)
        return 6;
    auto area = [](const ecad::Polygon& p, double dbu) {
        long double sum = 0;
        for (std::size_t i = 0; i < p.points.size(); ++i) {
            auto a = p.points[i], b = p.points[(i + 1) % p.points.size()];
            sum += static_cast<long double>(a[0]) * b[1] - static_cast<long double>(b[0]) * a[1];
        }
        return std::abs(static_cast<double>(sum)) * dbu * dbu / 2;
    };
    if (std::abs(area(a.model.layers[0].polygons[0], a.model.dbu_m) - 6e-6) > 1e-14)
        return 7;
    auto u = ecad::read_ipc2581(root / "unsupported.xml");
    if (!u || u.model.report.unsupported.size() != 3)
        return 3;
    auto b = ecad::read_ipc2581(root / "board_4layer.xml");
    if (!b || b.model.layers.size() != 5 || b.model.layers[0].polygons.empty())
        return 4;
    const auto trace_area = area(b.model.layers[0].polygons.front(), b.model.dbu_m);
    const double analytic_trace_area = (3.0 * 0.1 + std::acos(-1.0) * 0.05 * 0.05) * 1e-6;
    if (std::abs(trace_area - analytic_trace_area) / analytic_trace_area > 1e-4)
        return 10;
    auto c = ecad::read_ipc2581(root / "board_arc.xml");
    if (!c || c.model.layers.size() != 1 || c.model.layers[0].polygons.empty())
        return 5;
    const auto output = std::filesystem::temp_directory_path() / "chipcae-ipc2581-test";
    std::filesystem::create_directories(output);
    std::string error;
    if (!ecad::to_stack(a.model, output / "board.stack.json", output / "board.polyset", &error))
        return 8;
    if (!std::filesystem::exists(output / "board.stack.json") ||
        std::filesystem::file_size(output / "board.polyset") == 0)
        return 9;
    std::filesystem::remove_all(output);
    std::cout << "IPC-2581 SAX reader checks passed\n";
    return 0;
}
