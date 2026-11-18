#include "region_cut_support.hpp"
using namespace sp5b;
int main() {
    return run([] {
        auto s = stack();
        HexMeshArrays m; // Empty registry: box-only regions must not look up bodies.
        s.layers[0].regions = {box("unregistered", 2, 2, 6, 6)};
        require(geometry(s, m, "box-only source").planes.empty(), "box produces no cut planes");
        s.layers[0].material = "solid";
        require(geometry(s, m, "box in material layer").planes.empty(),
                "material-layer rejection applies only to polygon sources");
        s.layers[0].regions.clear();
        require(geometry(s, m, "empty region list").planes.empty(), "empty region list succeeds");
        s.layers.clear();
        require(geometry(s, m, "empty stack").planes.empty(), "empty stack succeeds");
    });
}
