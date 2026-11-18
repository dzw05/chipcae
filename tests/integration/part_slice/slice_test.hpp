#pragma once
#include "chipcae/geometry/part_slice.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <tuple>
using namespace chipcae::geometry;
namespace slice_test {
inline void require(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error("FAIL: " + message);
}
inline SliceOptions options() {
    const auto t = compute_tolerances({0.04, 0.0005, 0.001, 0}, {}).value();
    return {t.tau_A, t.tau_A, t.tau_B};
}
template <class F>
int run(F test) {
    try {
        test();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: acceptance requires slice_prismatic to return without exception: "
                  << e.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "FAIL: unexpected exception\n";
        return 1;
    }
}
inline double area(const Ring& r) {
    require(r.pts.size() >= 3, "nondegenerate ring");
    double sum = 0;
    for (std::size_t i = 0; i < r.pts.size(); ++i) {
        const auto& a = r.pts[i];
        const auto& b = r.pts[(i + 1) % r.pts.size()];
        sum += a[0] * b[1] - b[0] * a[1];
    }
    return sum / 2;
}
inline double area(const Slab& s) {
    double sum = 0;
    for (const auto& section : s.sections) {
        require(area(section.outer) > 0, "outer is counterclockwise");
        sum += area(section.outer);
        for (const auto& h : section.holes) {
            require(area(h) < 0, "hole is clockwise");
            sum += area(h);
        }
    }
    return sum;
}
inline void near(double a, double b, double tol, const char* message) {
    require(std::isfinite(a) && std::abs(a - b) <= tol, message);
}
inline void rectangle(const Ring& r, double tol) {
    require(r.pts.size() == 4, "rectangle has exactly four vertices");
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& a = r.pts[i];
        const auto& b = r.pts[(i + 1) % 4];
        const double dx = std::abs(a[0] - b[0]), dy = std::abs(a[1] - b[1]);
        require((dx <= tol && dy > tol) || (dy <= tol && dx > tol), "axis aligned rectangle edge");
    }
    require(area(r) > 0, "positive rectangle area");
}
inline Tier tier(const SliceResult& r) {
    Tier t = Tier::A;
    for (const auto& d : r.defects)
        if (d.tier > t)
            t = d.tier;
    return t;
}
inline void rejection(const SliceResult& r, const char* details) {
    require(r.slabs.empty() && r.defects.size() == 1, "empty slabs and exactly one defect");
    const auto& d = r.defects.front();
    require(d.tier == Tier::C && d.reason == DefectReason::PART_NOT_PRISMATIC,
            "C PART_NOT_PRISMATIC");
    require(d.details == details, "exact rejection details");
}
inline void same(const SliceResult& a, const SliceResult& b) {
    require(a.slabs.size() == b.slabs.size() && a.defects.size() == b.defects.size(),
            "repeat counts");
    for (std::size_t i = 0; i < a.slabs.size(); ++i) {
        const auto& x = a.slabs[i];
        const auto& y = b.slabs[i];
        require(x.z0 == y.z0 && x.z1 == y.z1 && x.sections.size() == y.sections.size(),
                "repeat slab fields");
        for (std::size_t j = 0; j < x.sections.size(); ++j) {
            const auto& s = x.sections[j];
            const auto& t = y.sections[j];
            require(s.outer.pts == t.outer.pts && s.holes.size() == t.holes.size(),
                    "repeat section fields");
            for (std::size_t k = 0; k < s.holes.size(); ++k)
                require(s.holes[k].pts == t.holes[k].pts, "repeat hole points");
        }
    }
    const auto fields = [](const Defect& d) {
        return std::tie(d.tier,
                        d.reason,
                        d.dimension,
                        d.size,
                        d.suggestion,
                        d.defect_id,
                        d.kind,
                        d.action,
                        d.rejection_reason,
                        d.material_ambiguous,
                        d.details);
    };
    for (std::size_t i = 0; i < a.defects.size(); ++i)
        require(fields(a.defects[i]) == fields(b.defects[i]), "repeat all defect fields");
}
} // namespace slice_test
