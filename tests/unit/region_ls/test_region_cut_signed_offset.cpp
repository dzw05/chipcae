#include "region_cut_support.hpp"
using namespace sp5b;
int main() {
    return run([] {
        // Both signs of the boundary coordinate need signed d for outward -ex/-ey.
        for (double shift : {0., -8.}) {
            auto s = stack();
            s.outline.box = {shift, shift, shift + 8, shift + 8};
            s.layers[0].regions = {box("offset", shift, shift, shift + 8, shift + 8)};
            const auto m = grid(s, 1);
            s.layers[0].regions[0] = square("offset", shift + 2, shift + 3, shift + 6, shift + 7);
            const auto g = geometry(s, m, "nonzero negative-normal offsets");
            const auto body = m.attr_table.bodies.at("offset");
            const CutGeometry expected = shift == 0 ? CutGeometry{{{body, {0, -1, 0}, -3},
                                                                   {body, {1, 0, 0}, 6},
                                                                   {body, {0, 1, 0}, 7},
                                                                   {body, {-1, 0, 0}, -2}}}
                                                    : CutGeometry{{{body, {0, -1, 0}, 5},
                                                                   {body, {1, 0, 0}, -2},
                                                                   {body, {0, 1, 0}, -1},
                                                                   {body, {-1, 0, 0}, 6}}};
            exact_planes(g, expected);
            require(std::abs(clipped_area(s.outline.box, g) - 16) / 16 <= 1e-12,
                    "half-spaces retain the translated rectangle, relative area tolerance 1e-12");
            CutOptions options;
            options.export_polyhedra = true;
            const auto cut = build_cut_layer(m, g, options);
            require(bool(cut), "signed offsets are accepted by the actual cutter");
            require(!cut.value().cut_elem.empty() && cut.value().polyhedra.has_value(),
                    "nonzero lower boundaries exercise actual cell clipping");
            const auto& poly = *cut.value().polyhedra;
            require(!poly.points.empty(), "signed half-spaces retain nonempty solid");
            for (std::size_t i = 0; i < poly.points.size(); i += 3)
                require(poly.points[i] >= shift + 2 - 1e-12 &&
                            poly.points[i] <= shift + 6 + 1e-12 &&
                            poly.points[i + 1] >= shift + 3 - 1e-12 &&
                            poly.points[i + 1] <= shift + 7 + 1e-12,
                        "cutter retains x >= xmin and y >= ymin, absolute tolerance 1e-12");
            const double volume = std::accumulate(poly.volume.begin(), poly.volume.end(), 0.);
            require(std::abs(volume - 16) / 16 <= 1e-12,
                    "actual retained volume is 4*4*1, relative tolerance 1e-12");
        }
    });
}
