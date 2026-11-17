#include "assembly_test_support.hpp"
int main() {
    return run([] {
        auto b = base();
        b.unit = "m";
        b.symmetry = "none";
        b.package_type = "test";
        b.warnings = {"keep"};
        b.activation_groups["identity"] = {"layer:Core"};
        b.layers[0].regions.push_back(
            {"seed", "seed-material", "seed-body", {{0, 0, .001, .001}, {}}});
        auto r = merge_parts(b, {}, options());
        check(bool(r), "empty parts succeeds");
        check(r.value().defects.empty() && r.value().welded_interfaces == 0, "identity metadata");
        check(r.value().stack.id == b.id && r.value().stack.outline.box == b.outline.box &&
                  r.value().stack.layers.size() == 1 && r.value().stack.layers[0].name == "Core" &&
                  r.value().stack.layers[0].thickness == .001,
              "identity stack");
        const auto& identity = r.value().stack;
        check(identity.unit == b.unit && identity.symmetry == b.symmetry &&
                  identity.package_type == b.package_type && identity.warnings == b.warnings &&
                  identity.activation_groups == b.activation_groups,
              "identity preserves stack metadata");
        check(identity.layers[0].body == b.layers[0].body &&
                  identity.layers[0].material == b.layers[0].material &&
                  identity.layers[0].regions[0].name == "seed" &&
                  identity.layers[0].regions[0].body == "seed-body",
              "identity preserves layer and region fields");
        b.layers.clear();
        auto empty = merge_parts(b, {}, options());
        check(bool(empty), "empty parts accepts base without layers");
        check(empty.value().stack.layers.empty() && empty.value().stack.id == b.id &&
                  empty.value().stack.outline.box == b.outline.box &&
                  empty.value().stack.activation_groups == b.activation_groups &&
                  empty.value().stack.warnings == b.warnings &&
                  empty.value().stack.unit == b.unit &&
                  empty.value().stack.symmetry == b.symmetry &&
                  empty.value().stack.package_type == b.package_type &&
                  empty.value().defects.empty() && empty.value().welded_interfaces == 0,
              "empty base identity preserves metadata");
        b = base();
        invalid_input(merge_parts({}, {part()}, options()));
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
