#include "assembly_test_support.hpp"
int main() {
    return run([] {
        auto src = part().slabs;
        FramePlacement f{{.003, .004, .005}};
        auto p = place(src, f, base());
        check(bool(p), "frame succeeds");
        near(p.value().slabs[0].z0, src[0].z0 + .005, 1e-15);
        near(p.value().slabs[0].z1, src[0].z1 + .005, 1e-15);
        for (size_t i = 0; i < 4; ++i)
            for (size_t j = 0; j < 2; ++j)
                near(p.value().slabs[0].sections[0].outer.pts[i][j],
                     src[0].sections[0].outer.pts[i][j] + f.origin[j],
                     1e-15);
        auto flush = merge(base(), {part("flush", .001, .002)});
        check(flush.welded_interfaces == 1 && flush.defects.empty(),
              "exact base coincidence counts once; isolated upper endpoint does not");
        auto stacked = merge(base(), {part("lower", .001, .002), part("upper", .002, .003)}, .003);
        check(stacked.welded_interfaces == 2 && stacked.defects.empty(),
              "exact part coincidence and base coincidence each count once");
        auto nonweld = merge(base(), {part("aligned", .001 + 2 * options().eps_weld, .002)});
        check(nonweld.welded_interfaces == 0 && nonweld.defects.empty(),
              "alignment beyond eps_weld does not count as a weld");
        auto m = merge(base(), {part("lid", .001 + options().eps_weld / 2, .002)});
        near(m.stack.layers.front().thickness, .001);
        near(m.stack.layers.back().thickness, .001);
        check(m.welded_interfaces == 1 && m.defects.empty(), "weld only counts");
        auto a = part("a", .001 + options().eps_weld / 4, .002);
        auto b = part("b", .001 + options().eps_weld / 2, .002);
        b.slabs[0].sections = {rect(.01, .01)};
        auto shared = merge(base(), {a, b});
        check(shared.welded_interfaces == 2 && shared.defects.empty(),
              "shared snapped lower and exact upper interfaces each count once");
        a.slabs[0].z1 += options().eps_weld / 4;
        b.slabs[0].z1 += options().eps_weld / 2;
        auto distinct = merge(base(), {part("anchor"), a, b});
        check(distinct.welded_interfaces == 2 && distinct.defects.empty(),
              "distinct snapped interfaces each count once");
    });
}
