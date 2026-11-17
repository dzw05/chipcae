#include "support.hpp"
using namespace sp5;
int main() {
    return run([] {
        auto s = stack();
        s.layers[0].regions = {box("lid", 2, 2, 6, 6)};
        auto m = grid(s);
        require(m.elem_gid.size() == 128, "box lid must emit 8*8*2 cells, omitting void columns");
        require(m.grid_report.void_cells == 384,
                "void_cells must equal candidate minus emitted cells");
        compact(m);
        auto other = grid(s, 4, 2);
        require(chipcae::preview::detail::digest(m).sha256 ==
                    chipcae::preview::detail::digest(other).sha256,
                "void mesh hash must be rank independent");
        s.layers[0].regions.clear();
        auto empty = grid(s);
        require(empty.hex.empty() && empty.vertices.empty(),
                "empty material-free layer is entirely void");
        require(empty.grid_report.void_cells == 32, "entirely void layer count");
    });
}
