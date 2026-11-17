#include "occt_fixture.hpp"
using namespace slice_test;
int main() {
    return run([] {
        const auto box = BRepPrimAPI_MakeBox(40, 30, 0.5).Shape();
        BRepFilletAPI_MakeFillet fillet(box);
        for (TopExp_Explorer it(box, TopAbs_EDGE); it.More(); it.Next()) {
            const auto edge = TopoDS::Edge(it.Current());
            BRepAdaptor_Curve c(edge);
            const auto a = c.Value(c.FirstParameter()), b = c.Value(c.LastParameter());
            if (std::abs(a.X() - b.X()) < 1e-10 && std::abs(a.Y() - b.Y()) < 1e-10)
                fillet.Add(2, edge);
        }
        fillet.Build();
        require(fillet.IsDone(), "rounded fixture built");
        const auto input = fixture(fillet.Shape());
        const auto result = slice(input);
        const auto s = single(result);
        same(result, slice(input));
        require(s.sections[0].holes.empty(), "rounded plate no holes");
        const auto& pts = s.sections[0].outer.pts;
        const double pi = std::acos(-1.0), radius = 0.002;
        const double expected = 0.04 * 0.03 - (4 - pi) * radius * radius;
        near(area(s), expected, expected * 1e-4, "rounded relative area <= 1e-4");
        for (std::size_t i = 0; i < pts.size(); ++i) {
            const auto& a = pts[i];
            const auto& b = pts[(i + 1) % pts.size()];
            // Every non-straight chord belongs to one of the four circular corners.
            if ((std::abs(a[0] - b[0]) < 1e-14 &&
                 (std::abs(a[0]) < 1e-14 || std::abs(a[0] - 0.04) < 1e-14)) ||
                (std::abs(a[1] - b[1]) < 1e-14 &&
                 (std::abs(a[1]) < 1e-14 || std::abs(a[1] - 0.03) < 1e-14)))
                continue;
            const double cx = (a[0] + b[0]) / 2 < 0.02 ? radius : 0.04 - radius;
            const double cy = (a[1] + b[1]) / 2 < 0.015 ? radius : 0.03 - radius;
            near(std::hypot(a[0] - cx, a[1] - cy), radius, 1e-10, "arc endpoint on circle");
            near(std::hypot(b[0] - cx, b[1] - cy), radius, 1e-10, "arc endpoint on circle");
            const double chord = std::hypot(a[0] - b[0], a[1] - b[1]);
            require(chord <= 2 * radius, "valid circle chord");
            const double sag =
                radius - std::sqrt(std::max(0.0, radius * radius - chord * chord / 4));
            require(sag <= options().chord_tol, "each chord sagitta <= chord_tol");
        }
    });
}
