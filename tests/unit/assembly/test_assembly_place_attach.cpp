#include "assembly_test_support.hpp"
int main() {
    return run([] {
        auto b = base();
        std::vector<Slab> src = {{.003, .004, {rect()}}};
        for (auto side : {AttachSide::top, AttachSide::bottom})
            for (double gap : {0., 5e-5}) {
                AttachPlacement a;
                a.target = "layer:Core";
                a.side = side;
                a.gap = gap;
                a.xy = {.002, .003};
                a.rot_z_deg = 30;
                auto r = place(src, a, b);
                check(bool(r), "attach succeeds");
                check(r.value().id.empty() && r.value().material.empty(), "place identity empty");
                auto s = r.value().slabs;
                check(s.size() == 1, "no synthetic bond");
                near(s[0].z0, side == AttachSide::top ? .001 + gap : -.001 - gap, 6e-18);
                near(s[0].z1 - s[0].z0, .001, 6e-18);
                near(src[0].z0, .003, 0);
                near(src[0].z1, .004, 0);
                check(src[0].sections[0].outer.pts == rect().outer.pts, "source section unchanged");
                if (gap > 0) {
                    a.bond = Bond{"adhesive", gap};
                    auto bonded = place(src, a, b);
                    check(bool(bonded), "bond attach succeeds");
                    check(bonded.value().id.empty() && bonded.value().material.empty(),
                          "bonded place identity empty");
                    auto t = bonded.value().slabs;
                    check(t.size() == s.size(), "bond does not add source slabs");
                    near(t[0].z0, s[0].z0, 0);
                    near(t[0].z1, s[0].z1, 0);
                    check(t[0].sections[0].outer.pts == s[0].sections[0].outer.pts,
                          "same transformed source");
                    PlacedPart original = {"lid", "metal", s};
                    Slab bond = side == AttachSide::top
                                    ? Slab{s[0].z0 - gap, s[0].z0, s[0].sections}
                                    : Slab{s[0].z1, s[0].z1 + gap, s[0].sections};
                    near(bond.z1 - bond.z0, gap, 6e-18);
                    near(bond.sections[0].outer.pts[0][0], .002, 6e-18);
                    auto m = merge(b, {original, {"lid.bond", "adhesive", {bond}}}, .002 + gap);
                    bool found = false;
                    for (auto& l : m.stack.layers)
                        for (auto& v : l.regions)
                            if (v.body == "lid.bond") {
                                found = true;
                                check(v.material == "adhesive" && v.name == "lid.bond#0#0",
                                      "bond identity");
                            }
                    check(found, "bond region");
                    a.bond->thickness = gap * 2;
                    invalid_input(place(src, a, b), "bond gap != thickness");
                } else
                    check(!a.bond, "zero gap without bond");
                if (gap > 0) {
                    auto m = merge(b, {{"lid", "metal", s}}, .002 + gap);
                    double z = side == AttachSide::top ? 0 : -.001 - gap;
                    double midpoint = side == AttachSide::top ? .001 + gap / 2 : -gap / 2;
                    bool gapLayer = false;
                    for (auto& l : m.stack.layers) {
                        if (z < midpoint && midpoint < z + l.thickness) {
                            gapLayer = true;
                            check(l.regions.empty() && l.material.empty(),
                                  "gap is void without regions");
                        }
                        z += l.thickness;
                    }
                    check(gapLayer, "gap interval retained");
                }
            }
    });
}
