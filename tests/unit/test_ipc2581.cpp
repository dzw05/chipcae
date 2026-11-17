#include "chipcae/geometry/ecad/ipc2581.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace ecad = chipcae::geometry::ecad;
static std::string dump_model_for_golden(const ecad::EcadModel& m) {
    std::ostringstream o;
    auto d = [&](double v) {
        char b[128];
        std::snprintf(b, sizeof(b), "%a", v);
        o << b << '\n';
    };
    auto str = [&](const std::string& v) { o << std::quoted(v) << '\n'; };
    auto pos = [&](const auto& p) { o << p[0] << '\n' << p[1] << '\n'; };
    auto poly = [&](const ecad::Polygon& p) {
        o << p.points.size() << '\n';
        for (auto& v : p.points)
            pos(v);
    };
    str(m.id);
    d(m.dbu_m);
    o << m.layers.size() << '\n';
    for (auto& l : m.layers) {
        str(l.name);
        str(l.material);
        str(l.function);
        d(l.thickness_m);
        o << l.copper << '\n' << l.polygons.size() << '\n';
        for (auto& p : l.polygons)
            poly(p);
    }
    poly(m.outline);
    o << m.report.unsupported.size() << '\n';
    for (auto& u : m.report.unsupported) {
        str(u.code);
        str(u.element);
        o << u.line << '\n';
    }
    o << m.vias.size() << '\n';
    for (auto& v : m.vias) {
        str(v.id);
        str(v.padstack);
        str(v.start_layer);
        str(v.end_layer);
        pos(v.position);
        d(v.outer_radius_dbu);
        d(v.inner_radius_dbu);
        o << v.dielectric_layers.size() << '\n';
        for (auto i : v.dielectric_layers)
            o << i << '\n';
    }
    o << m.components.size() << '\n';
    for (auto& c : m.components) {
        str(c.id);
        str(c.package);
        pos(c.position);
        d(c.rotation_degrees);
        o << c.mirror << '\n';
        poly(c.outline);
        o << c.pads.size() << '\n';
        for (auto& p : c.pads) {
            str(p.id);
            pos(p.position);
        }
    }
    o << m.via_fractions.size() << '\n';
    for (auto& f : m.via_fractions) {
        o << f.layer << '\n';
        for (auto v : f.lower)
            d(v);
        for (auto v : f.upper)
            d(v);
        d(f.fraction);
        o << f.via_count << '\n';
    }
    return o.str();
}
static bool golden_regression() {
    const auto root = std::filesystem::path(CHIPCAE_TEST_SOURCE_DIR);
    const auto map = std::filesystem::temp_directory_path() / "ipc-golden-map.json";
    {
        std::ofstream out(map);
        out << R"({"FR4":"mat.fr4","copper":"mat.cu"})";
    }
    for (auto board : {"board_2layer", "board_4layer"}) {
        auto r = ecad::read_ipc2581(root / "tests/integration/ecad/data" /
                                    (std::string(board) + ".xml"));
        if (!r)
            return false;
        for (auto stage : {"read", "mapped"}) {
            std::string error;
            if (std::string(stage) == "mapped" && !ecad::apply_material_map(r.model, map, &error)) {
                std::cerr << error << '\n';
                return false;
            }
            auto path =
                root / "tests/unit/golden" / ("ipc_" + std::string(board) + "." + stage + ".txt");
            auto actual = dump_model_for_golden(r.model);
            std::ifstream in(path);
            std::string expected((std::istreambuf_iterator<char>(in)), {});
            if (!in || expected != actual) {
                std::istringstream a(actual), e(expected);
                std::string al, el;
                std::size_t line = 0;
                while (std::getline(a, al)) {
                    ++line;
                    if (!std::getline(e, el) || al != el)
                        break;
                }
                std::cerr << path << ": first differing line " << line << " expected [" << el
                          << "] actual [" << al << "]\n";
                return false;
            }
        }
    }
    std::filesystem::remove(map);
    return true;
}

int main() {
    if (!golden_regression())
        return 11;
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
