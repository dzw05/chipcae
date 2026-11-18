#pragma once
#include "chipcae/mesh/cut/region_cut.hpp"
#include "support.hpp"

#include <string_view>

namespace sp5b {
using chipcae::BodyId;
using chipcae::Real;
using namespace sp5;
using namespace chipcae::mesh::cut;
using Point = std::array<Real, 2>;
using Polygon = std::vector<Point>;

inline Polygon chamfer() {
    return {{.002, 0},
            {.054, 0},
            {.056, .002},
            {.056, .042},
            {.054, .044},
            {.002, .044},
            {0, .042},
            {0, .002}};
}
inline StackModel chamfer_stack() {
    auto s = stack();
    s.id = "sp5b-chamfer";
    s.outline.box = {0, 0, .056, .044};
    s.layers[0].name = "L#0";
    s.layers[0].thickness = .001;
    auto r = box("lid-region", 0, 0, .056, .044);
    r.body = "lid";
    r.footprint.polygon = chamfer();
    s.layers[0].regions = {r};
    return s;
}
// Plane extraction needs only axes and the body registry. Invalid-source tests
// deliberately bypass tensor_grid's earlier input validation.
inline HexMeshArrays registered_mesh() {
    HexMeshArrays m;
    m.axes = {{{0, .002, .028, .054, .056}, {0, .002, .022, .042, .044}, {0, .001}}};
    m.attr_table.bodies = {{"lid", 17}};
    return m;
}
inline CutGeometry geometry(const StackModel& s, const HexMeshArrays& m, std::string_view context) {
    auto result = region_cut_planes(s, m);
    if (!result)
        throw std::runtime_error(std::string(context) + ": expected CutGeometry, got " +
                                 result.error().path + ": " + result.error().reason);
    return result.value();
}
inline void
error(const StackModel& s, const HexMeshArrays& m, std::string_view path, std::string_view reason) {
    auto result = region_cut_planes(s, m);
    require(!result, "invalid polygon source must return Error");
    if (result.error().path != path || result.error().reason.find(reason) == std::string::npos)
        throw std::runtime_error("expected " + std::string(path) + ": " + std::string(reason) +
                                 "; got " + result.error().path + ": " + result.error().reason);
}
// Exact equality is part of the brief's plane canonicalization contract.
inline void exact_planes(const CutGeometry& actual, const CutGeometry& expected) {
    require(actual.planes.size() == expected.planes.size(), "ordered plane count");
    for (std::size_t k = 0; k < actual.planes.size(); ++k) {
        const auto& a = actual.planes[k];
        const auto& e = expected.planes[k];
        require(a.body == e.body && a.n == e.n && a.d == e.d,
                "ordered (body, n, d) fields must be exactly equal");
    }
}
// Independent Sutherland-Hodgman oracle. It consumes public planes directly;
// it does not normalize polygons or interpolate region level sets.
inline double clipped_area(const std::array<Real, 4>& box, const CutGeometry& g) {
    Polygon poly{{box[0], box[1]}, {box[2], box[1]}, {box[2], box[3]}, {box[0], box[3]}};
    for (const auto& p : g.planes) {
        Polygon next;
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const auto a = poly[i], b = poly[(i + 1) % poly.size()];
            const double fa = p.n[0] * a[0] + p.n[1] * a[1] - p.d;
            const double fb = p.n[0] * b[0] + p.n[1] * b[1] - p.d;
            if (fa <= 0)
                next.push_back(a);
            if ((fa <= 0) != (fb <= 0)) {
                const double t = fa / (fa - fb);
                next.push_back({a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])});
            }
        }
        poly = std::move(next);
    }
    double twice_area = 0;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        const auto a = poly[i], b = poly[(i + 1) % poly.size()];
        twice_area += a[0] * b[1] - a[1] * b[0];
    }
    return std::abs(twice_area) / 2;
}
} // namespace sp5b
