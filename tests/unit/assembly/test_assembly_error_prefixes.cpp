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
