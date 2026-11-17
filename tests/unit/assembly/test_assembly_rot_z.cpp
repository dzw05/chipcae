#include "assembly_test_support.hpp"
int main() {
    return run([] {
        std::vector<Slab> src = {
            {0,
             .001,
             {rect(-.005, -.002, .01, .004),
              {Ring{{{0, 0}, {.004, 0}, {.004, .001}, {.001, .001}, {.001, .004}, {0, .004}}},
               {}}}}};
        AttachPlacement a;
        a.target = "layer:Core";
        a.xy = {.005, .007};
        a.rot_z_deg = 30;
        auto p = place(src, a, base());
        check(bool(p), "rotation succeeds");
        double c = std::cos(std::acos(-1.) / 6), s = .5;
        for (size_t k = 0; k < 2; ++k) {
            auto& out = p.value().slabs[0].sections[k].outer.pts;
            auto& in = src[0].sections[k].outer.pts;
            check(out.size() == in.size(), "vertex count");
            for (size_t i = 0; i < in.size(); ++i) {
                near(out[i][0], c * in[i][0] - s * in[i][1] + .005, 1e-15);
                near(out[i][1], s * in[i][0] + c * in[i][1] + .007, 1e-15);
            }
            check(area(out) > 0, "CCW preserved");
        }
        auto m = merge(base(), {{"lid", "metal", p.value().slabs}});
        for (auto& v : m.stack.layers.back().regions) {
            auto& q = v.footprint.polygon;
            check(!q.empty(), "rotated polygon");
            double xmin = q[0][0], xmax = xmin, ymin = q[0][1], ymax = ymin;
            for (auto x : q) {
                xmin = std::min(xmin, x[0]);
                xmax = std::max(xmax, x[0]);
                ymin = std::min(ymin, x[1]);
                ymax = std::max(ymax, x[1]);
            }
            near(v.footprint.box[0], xmin);
            near(v.footprint.box[2], xmax);
            near(v.footprint.box[1], ymin);
            near(v.footprint.box[3], ymax);
        }
    });
}
