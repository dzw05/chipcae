#include "assembly_test_support.hpp"
int main() {
    return run([] {
        auto b = base();
        auto src = part().slabs;
        for (auto side : {AttachSide::top, AttachSide::bottom}) {
            AttachPlacement a;
            a.target = "part:lid";
            a.side = side;
            invalid_input(place(src, a, b), "part attach requires resolved_z");
            a.resolved_z = .003;
            check(bool(place(src, a, b)), "resolved part attach succeeds");
        }
        AttachPlacement a;
        a.target = "layer:Core";
        check(bool(place(src, a, b)), "layer resolves without resolved_z");
        a.target = "layer:missing";
        a.resolved_z = .003;
        auto resolved = place(src, a, b);
        check(bool(resolved), "resolved face bypasses target lookup per placement brief");
        near(resolved.value().slabs.front().z0, .003);
        a.gap = 5e-5;
        a.bond = Bond{"glue", 1e-4};
        invalid_input(place(src, a, b), "bond gap != thickness");
        for (double t : {0., -1e-5}) {
            a.gap = t;
            a.bond->thickness = t;
            invalid_input(place(src, a, b), "bond thickness must be > 0");
        }
        a.bond->thickness = 5e-5;
        for (double gap :
             {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
            a.gap = gap;
            invalid_input(place(src, a, b), "invalid attachment gap");
            a.bond.reset();
            invalid_input(place(src, a, b), "invalid attachment gap");
            a.bond = Bond{"glue", 5e-5};
        }
        invalid_input(place({}, FramePlacement{}, b));
        invalid_input(merge_parts({}, {part()}, options()), "base has no layers");
        for (bool region : {false, true}) {
            for (const auto& footprint : std::vector<Footprint>{
                     Footprint{{0, 0, 0, 1}, {}},
                     Footprint{{0, 0, std::numeric_limits<double>::quiet_NaN(), 1}, {}},
                     Footprint{{0, 0, 1, std::numeric_limits<double>::infinity()}, {}},
                     Footprint{{}, {{0, 0}, {1, 0}}},
                     Footprint{{}, {{0, 0}, {1, 1}, {0, 1}, {1, 0}, {2, 0}}},
                     Footprint{{},
                               {{0, 0}, {1, 0}, {0, std::numeric_limits<double>::quiet_NaN()}}}}) {
                auto bad = b;
                if (region) {
                    Inclusion inclusion;
                    inclusion.name = "bad";
                    inclusion.footprint = footprint;
                    bad.layers[0].regions.push_back(inclusion);
                } else
                    bad.outline = footprint;
                invalid_input(merge_parts(bad, {part()}, options()));
                invalid_input(merge_parts(bad, {}, options()));
            }
        }
        // A polygon is authoritative even when the unused box is degenerate.
        for (bool clockwise : {false, true}) {
            auto polygon_base = b;
            auto polygon = rect(0, 0, .02, .02).outer.pts;
            if (clockwise)
                std::reverse(polygon.begin(), polygon.end());
            polygon_base.outline = Footprint{{}, polygon};
            Inclusion inclusion;
            inclusion.name = "valid";
            inclusion.footprint = Footprint{{}, rect(.01, .01).outer.pts};
            polygon_base.layers[0].regions.push_back(inclusion);
            check(bool(merge_parts(polygon_base, {part()}, options())),
                  "valid polygon outlines and regions remain accepted");
        }
        auto empty = part();
        empty.slabs.clear();
        invalid_input(merge_parts(b, {empty}, options()));
        for (double v : {0.,
                         -1.,
                         std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()})
            for (int field = 0; field < 3; ++field) {
                auto o = options();
                if (field == 0)
                    o.t_min = v;
                if (field == 1)
                    o.eps_weld = v;
                if (field == 2)
                    o.tau_a = v;
                invalid_input(merge_parts(b, {part()}, o), "invalid MergeOptions");
            }
    });
}
