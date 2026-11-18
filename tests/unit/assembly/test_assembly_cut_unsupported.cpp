#include "assembly_test_support.hpp"
void unsupported(PlacedPart p, const std::string& reason, size_t slab = 0) {
    auto b = base();
    b.outline.box = {-.1, -.1, .1, .1};
    auto r = merge_parts(b, {p}, options());
    check(bool(r), "unsupported is a defect, not input error");
    size_t count = 0;
    for (auto& d : r.value().defects)
        if (d.reason == DefectReason::PART_NOT_PRISMATIC) {
            ++count;
            check(d.tier == Tier::C &&
                      d.defect_id == p.id + "/cut_unsupported#" + std::to_string(slab),
                  "unsupported tier/reason/id");
            check(d.details.starts_with("cut_unsupported: ") &&
                      d.details.find(reason) != std::string::npos,
                  "unsupported reason detail");
        }
    check(count == 1, "exactly one unsupported cut defect");
    if (p.slabs.size() == 1) {
        check(r.value().stack.layers.size() == 1 && r.value().stack.layers[0].name == "Core" &&
                  r.value().stack.layers[0].thickness == .001,
              "unsupported source cannot split or extend base");
    }
    for (auto& l : r.value().stack.layers)
        for (auto& r : l.regions)
            check(!r.name.starts_with(p.id + "#" + std::to_string(slab) + "#"),
                  "unsupported slab emits no region");
}
int main() {
    return run([] {
        for (bool hole : {true, false}) {
            auto p = part("cut");
            p.slabs[0].sections = {
                hole ? SlabSection{rect().outer, {rect(.001, .001, .002, .002).outer}} : ell()};
            AttachPlacement a;
            a.target = "layer:Core";
            a.rot_z_deg = 15;
            auto placed = place(p.slabs, a, base());
            check(bool(placed), "rotate negative fixture");
            p.slabs = placed.value().slabs;
            auto interior = p;
            interior.slabs[0].z0 = .0003;
            interior.slabs[0].z1 = .0007;
            unsupported(interior, hole ? "holed non-rectilinear section" : "non-convex section");
            unsupported(p, hole ? "holed non-rectilinear section" : "non-convex section");
        }
        SlabSection chamfer{Ring{{{.002, 0},
                                  {.054, 0},
                                  {.056, .002},
                                  {.056, .042},
                                  {.054, .044},
                                  {.002, .044},
                                  {0, .042},
                                  {0, .002}}},
                            {}};
        auto p = part("chamfer");
        p.slabs[0].sections = {chamfer};
        p.slabs.push_back({.002, .003, {chamfer}});
        auto& repeated = p.slabs[1].sections[0].outer.pts;
        std::reverse(repeated.begin(), repeated.end());
        std::rotate(repeated.begin(), repeated.begin() + 3, repeated.end());
        repeated.push_back(repeated.front());
        repeated.insert(repeated.begin() + 1, repeated.front());
        auto b = base();
        b.outline.box = {-.1, -.1, .1, .1};
        auto m = merge(b, {p}, .003);
        check(m.defects.empty(), "repeated chamfer polygon supported");
        for (auto& l : m.stack.layers)
            for (auto& r : l.regions)
                check(r.footprint.polygon.size() == 8, "normalized chamfer emitted");
        p.slabs[1].sections = {chamfer};
        p.slabs[1].sections[0].outer.pts[1][0] -= .001;
        unsupported(p, "multiple distinct cut sections", 1);
    });
}
