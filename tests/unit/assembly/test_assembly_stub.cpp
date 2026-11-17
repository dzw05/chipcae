#include "assembly_test_support.hpp"
int main() {
    return run([] {
        auto b = base();
        auto r = merge_parts(b, {}, options());
        check(bool(r), "empty parts succeeds");
        check(r.value().defects.empty() && r.value().welded_interfaces == 0, "identity metadata");
        check(r.value().stack.id == b.id && r.value().stack.outline.box == b.outline.box &&
                  r.value().stack.layers.size() == 1 && r.value().stack.layers[0].name == "Core" &&
                  r.value().stack.layers[0].thickness == .001,
              "identity stack");
        check(!merge_parts({}, {part()}, options()), "empty base invalid");
        auto p = part();
        p.slabs.clear();
        check(!merge_parts(b, {p}, options()), "empty slabs invalid");
        check(!merge_parts(b, {part(), part()}, options()), "duplicate IDs invalid");
        check(!place({}, FramePlacement{}, b), "empty source invalid");
        auto invalid = part();
        invalid.slabs[0].z1 = invalid.slabs[0].z0;
        check(!merge_parts(b, {invalid}, options()), "zero thickness rejected");
        invalid = part();
        invalid.slabs[0].sections.clear();
        check(!place(invalid.slabs, FramePlacement{}, b), "empty sections rejected");
        invalid = part();
        invalid.slabs[0].sections[0].outer.pts[0][0] = std::numeric_limits<double>::infinity();
        check(!place(invalid.slabs, FramePlacement{}, b), "nonfinite vertex rejected");
        check(!place(part().slabs,
                     FramePlacement{{0, 0, std::numeric_limits<double>::quiet_NaN()}},
                     b),
              "nonfinite frame rejected");
        AttachPlacement a;
        a.target = "layer:missing";
        check(!place(part().slabs, a, b), "missing target invalid");
        a.target = "part:missing";
        check(!place(part().slabs, a, b), "unresolved part invalid");
        a.resolved_z = .002;
        check(bool(place(part().slabs, a, b)), "resolved part target");
    });
}
