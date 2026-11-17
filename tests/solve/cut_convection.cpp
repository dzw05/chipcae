#include "../../apps/chipcae-solve/src/cut_convection.hpp"

#include "chipcae/mesh/background/tensor_grid.hpp"
#include "chipcae/mesh/cut/plane_cut.hpp"

#include <iostream>

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int status = 0;
    try {
        chipcae::mesh::StackModel s;
        s.outline.box = {0, 0, 1, 1};
        chipcae::mesh::Layer l;
        l.name = l.material = "lower";
        l.kind = "Explicit";
        l.thickness = .5;
        s.layers.push_back(l);
        l.name = l.material = "upper";
        s.layers.push_back(l);
        chipcae::mesh::GridOptions o;
        o.nx = o.ny = 1;
        o.levels = 1;
        o.max_ratio = 100;
        auto m = chipcae::mesh::build_tensor_grid(s, o).value();
        chipcae::mesh::cut::CutGeometry g;
        for (auto [name, body] : m.attr_table.bodies)
            g.planes.push_back({body, {1, 0, 0}, .75});
        auto c = chipcae::mesh::cut::build_cut_layer(m, g, {}).value();
        chipcae::solve::CutSurfaceRule r(m, c, "layer:lower");
        double area = 0, moment = 0;
        for (std::size_t e = 0; e < m.attribute.size(); ++e) {
            const auto& rule = r.get(e);
            for (int q = 0; q < rule.GetNPoints(); ++q) {
                const auto& p = rule.IntPoint(q);
                area += p.weight;
                moment += p.weight * .25 * p.z * p.z;
            }
        }
        // All six sides of the trimmed lower prism, including the interface.
        // This nonconstant Robin mass action uses T=z^2, h=1, T_inf=1.
        if (std::abs(area - 3.25) > 1e-12 || std::abs(moment - 1. / 3.) > 1e-12)
            throw std::runtime_error("trimmed interlayer Robin area/mass moment");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        status = 1;
    }
    MPI_Finalize();
    return status;
}
