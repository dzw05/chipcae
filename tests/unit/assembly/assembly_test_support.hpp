#pragma once
#include "chipcae/geometry/assembly.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <regex>
#include <set>
#include <stdexcept>
using namespace chipcae::geometry;
using namespace chipcae::mesh;
inline void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
inline void near(double a, double b, double tol = 1e-12) {
    check(std::abs(a - b) <= tol, "coordinate/thickness mismatch");
}
inline SlabSection rect(double x = 0, double y = 0, double w = .004, double h = .004) {
    return {Ring{{{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}}, {}};
}
inline StackModel base() {
    StackModel b;
    b.id = "board";
    b.outline.box = {0, 0, .02, .02};
    Layer l;
    l.name = "Core";
    l.kind = "Explicit";
    l.material = "core";
    l.body = "board";
    l.thickness = .001;
    b.layers = {l};
    return b;
}
inline MergeOptions options() {
    return {1e-5, 1e-7, 1e-8};
}
inline PlacedPart part(std::string id = "lid", double lo = .001, double hi = .002) {
    return {id, "metal", {{lo, hi, {rect(.002, .002)}}}};
}
inline double area(const std::vector<std::array<double, 2>>& p) {
    double a = 0;
    for (size_t i = 0; i < p.size(); ++i) {
        auto q = p[(i + 1) % p.size()];
        a += p[i][0] * q[1] - q[0] * p[i][1];
    }
    return a / 2;
}
inline bool covers(const Footprint& f, double x, double y) {
    if (f.polygon.empty())
        return x > f.box[0] && x < f.box[2] && y > f.box[1] && y < f.box[3];
    bool in = false;
    for (size_t i = 0, j = f.polygon.size() - 1; i < f.polygon.size(); j = i++) {
        auto a = f.polygon[i], b = f.polygon[j];
        if ((a[1] > y) != (b[1] > y) && x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0])
            in = !in;
    }
    return in;
}
inline void invariants(const MergeResult& r, double total) {
    std::set<std::string> names;
    double sum = 0;
    for (auto& l : r.stack.layers) {
        check(names.insert(l.name).second, "duplicate layer name");
        check(l.thickness > 0, "nonpositive layer");
        sum += l.thickness;
        std::set<std::string> regions;
        for (auto& v : l.regions) {
            check(regions.insert(v.name).second, "duplicate region");
            check(v.footprint.box[0] < v.footprint.box[2] &&
                      v.footprint.box[1] < v.footprint.box[3],
                  "invalid box");
            auto& poly = v.footprint.polygon;
            check(poly.empty() || poly.size() >= 3, "invalid polygon");
            if (!poly.empty()) {
                check(std::abs(area(poly)) > 0, "zero polygon area");
                for (size_t i = 0; i < poly.size(); ++i)
                    for (size_t j = i + 1; j < poly.size(); ++j) {
                        if (j == i + 1 || (i == 0 && j + 1 == poly.size()))
                            continue;
                        auto cross = [](auto a, auto b, auto c) {
                            return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
                        };
                        auto a = poly[i], b = poly[(i + 1) % poly.size()], c = poly[j],
                             d = poly[(j + 1) % poly.size()];
                        check(!(cross(a, b, c) * cross(a, b, d) < 0 &&
                                cross(c, d, a) * cross(c, d, b) < 0),
                              "self crossing polygon");
                    }
            }
        }
    }
    near(sum, total, 1e-15);
}
inline MergeResult merge(StackModel b, std::vector<PlacedPart> ps, double total = .002) {
    auto r = merge_parts(b, ps, options());
    check(bool(r), "merge must succeed");
    invariants(r.value(), total);
    return r.value();
}
inline bool defect(const MergeResult& r, DefectReason reason) {
    return std::any_of(r.defects.begin(), r.defects.end(), [&](auto& d) {
        return d.reason == reason && d.tier == Tier::C;
    });
}
template <class F>
int run(F f) {
    try {
        f();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: acceptance requires implemented behavior: " << e.what() << '\n';
        return 1;
    }
}
