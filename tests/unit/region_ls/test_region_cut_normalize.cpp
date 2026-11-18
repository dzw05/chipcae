#include "region_cut_support.hpp"
using namespace sp5b;
int main() {
    return run([] {
        auto s = chamfer_stack();
        const auto m = registered_mesh();
        const auto reference = geometry(s, m, "clean CCW reference");
        require(reference.planes.size() == 8, "normalization reference has eight edges");
        std::vector<Polygon> variants;
        auto p = chamfer();
        std::reverse(p.begin(), p.end());
        variants.push_back(p);
        p = chamfer();
        p.insert(p.end(), {p.front(), p.front()});
        variants.push_back(p);
        p = chamfer();
        std::rotate(p.begin(), p.begin() + 3, p.end());
        variants.push_back(p);
        p = chamfer();
        p.insert(p.begin() + 1, {.028, 0});
        variants.push_back(p);
        // Short edge is on the interior of a straight edge: either retained
        // endpoint is subsequently collinear, so no endpoint policy is assumed.
        p.insert(p.begin() + 2, {.028 + 1e-16, 0});
        variants.push_back(p);
        std::reverse(p.begin(), p.end());
        variants.push_back(p);
        // Cleanup must also cross the cyclic seam, including repeated closures.
        p = chamfer();
        p.insert(p.begin() + 7, {0, .022});
        std::rotate(p.begin(), p.begin() + 7, p.end());
        p.push_back({0, .022 + 1e-16});
        p.push_back(p.front());
        p.push_back(p.front());
        variants.push_back(p);
        std::reverse(p.begin(), p.end());
        variants.push_back(p);
        for (const auto& variant : variants) {
            s.layers[0].regions[0].footprint.polygon = variant;
            exact_planes(geometry(s, m, "normalized variant"), reference);
            require(s.layers[0].regions[0].footprint.polygon == variant,
                    "normalization leaves the input ring unchanged");
        }
    });
}
