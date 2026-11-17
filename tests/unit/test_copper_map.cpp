#include "chipcae/mesh/background/column_hierarchy.hpp"
#include "chipcae/mesh/background/copper_map.hpp"
#include "chipcae/mesh/background/tensor_grid.hpp"

#include <cmath>
#include <iostream>

using namespace chipcae::mesh;
using chipcae::Real;
int main() {
    StackModel stack;
    stack.outline.box = {0, 0, 1, 1};
    Layer copper_layer;
    copper_layer.name = "cu";
    copper_layer.kind = "Patterned";
    copper_layer.material = "substrate";
    copper_layer.thickness = 1;
    copper_layer.n_elem_min = 1;
    copper_layer.copper = CopperSpec{"copper", Real(0.32), std::nullopt};
    stack.layers.push_back(copper_layer);
    GridOptions options;
    options.nx = options.ny = 2;
    auto grid = build_tensor_grid(stack, options);
    if (!grid || grid.value().copper_phi.size() != 4 ||
        grid.value().copper_layer_of_layer != std::vector<Local>{0})
        return 5;
    for (auto fraction : grid.value().copper_phi)
        if (fraction != Real(0.32))
            return 6;
    const auto device = upload<chipcae::HostSpace>(grid.value());
    if (device.copper_phi.extent(0) != 4)
        return 7;
    StackModel polygon_stack = stack;
    polygon_stack.layers[0].copper->fraction.reset();
    polygon_stack.layers[0].copper->polygons = CopperPolygons{"cu.poly", "cu"};
    Polyset polyset;
    polyset.dbu_m = 0.5;
    PolysetLayer poly_layer;
    poly_layer.name = "cu";
    poly_layer.ring_offsets = {0, 4};
    poly_layer.coordinates = {0, 0, 1, 0, 1, 2, 0, 2};
    polyset.layers.push_back(poly_layer);
    auto polygon_grid = build_tensor_grid(polygon_stack, options, polyset);
    if (!polygon_grid || polygon_grid.value().copper_phi.size() != 4)
        return 8;
    for (std::size_t j = 0; j < 2; ++j)
        for (std::size_t i = 0; i < 2; ++i) {
            const auto d =
                level0_dense(2, 2, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j));
            if (std::abs(polygon_grid.value().copper_phi[d] - (i == 0 ? 1 : 0)) > 1e-12)
                return 9;
        }
    auto partition_options = options;
    partition_options.nx = partition_options.ny = 4;
    partition_options.ranks = 1;
    auto serial_polygon_grid = build_tensor_grid(polygon_stack, partition_options, polyset);
    partition_options.ranks = 3;
    auto partitioned_polygon_grid = build_tensor_grid(polygon_stack, partition_options, polyset);
    if (!serial_polygon_grid || !partitioned_polygon_grid ||
        serial_polygon_grid.value().copper_phi != partitioned_polygon_grid.value().copper_phi)
        return 14;

    std::vector<Real> axis(41);
    for (std::size_t i = 0; i < axis.size(); ++i)
        axis[i] = Real(i) + Real(i * i) / 200;
    CopperPath rectangle{
        {{axis[8], axis[7]}, {axis[25], axis[7]}, {axis[25], axis[31]}, {axis[8], axis[31]}}};
    auto phi = rasterize_copper(axis, axis, {rectangle});
    const auto nx = axis.size() - 1;
    Real weighted = 0, total = 0;
    for (std::size_t j = 0; j < nx; ++j)
        for (std::size_t i = 0; i < nx; ++i) {
            const auto d = level0_dense(static_cast<std::uint32_t>(nx),
                                        static_cast<std::uint32_t>(nx),
                                        static_cast<std::uint32_t>(i),
                                        static_cast<std::uint32_t>(j));
            const Real expected = i >= 8 && i < 25 && j >= 7 && j < 31 ? 1 : 0;
            if (std::abs(phi[d] - expected) > 1e-12) {
                std::cerr << "unexpected copper fraction at " << i << ',' << j << ": " << phi[d]
                          << " expected " << expected << '\n';
                return 1;
            }
            const Real area = (axis[i + 1] - axis[i]) * (axis[j + 1] - axis[j]);
            weighted += phi[d] * area;
            total += area;
        }
    const Real exact = (axis[25] - axis[8]) * (axis[31] - axis[7]);
    if (std::abs(weighted - exact) > 1e-12 * exact ||
        !(weighted / total >= 0 && weighted / total <= 1))
        return 2;

    auto overlap = rasterize_copper(axis, axis, {rectangle, rectangle});
    for (std::size_t i = 0; i < phi.size(); ++i)
        if (std::abs(phi[i] - overlap[i]) > 1e-12)
            return 3;

    CopperPath l_shape{{{axis[2], axis[2]},
                        {axis[30], axis[2]},
                        {axis[30], axis[10]},
                        {axis[10], axis[10]},
                        {axis[10], axis[30]},
                        {axis[2], axis[30]}}};
    auto l_phi = rasterize_copper(axis, axis, {l_shape});
    for (std::size_t j = 0; j < nx; ++j)
        for (std::size_t i = 0; i < nx; ++i) {
            const auto d =
                level0_dense(40, 40, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j));
            const bool in_l =
                (i >= 2 && i < 30 && j >= 2 && j < 10) || (i >= 2 && i < 10 && j >= 10 && j < 30);
            if (std::abs(l_phi[d] - (in_l ? 1 : 0)) > 1e-12)
                return 10;
        }
    CopperPath outer{
        {{axis[2], axis[2]}, {axis[30], axis[2]}, {axis[30], axis[30]}, {axis[2], axis[30]}}};
    CopperPath hole{
        {{axis[10], axis[10]}, {axis[10], axis[20]}, {axis[20], axis[20]}, {axis[20], axis[10]}}};
    auto hole_phi = rasterize_copper(axis, axis, {outer, hole});
    for (std::size_t j = 0; j < nx; ++j)
        for (std::size_t i = 0; i < nx; ++i) {
            const auto d =
                level0_dense(40, 40, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j));
            const bool in_copper =
                i >= 2 && i < 30 && j >= 2 && j < 30 && !(i >= 10 && i < 20 && j >= 10 && j < 20);
            if (std::abs(hole_phi[d] - (in_copper ? 1 : 0)) > 1e-12)
                return 11;
        }
    CopperPath circle;
    constexpr std::size_t circle_points = 256;
    const Real cx = axis[20], cy = axis[20], radius = 8;
    for (std::size_t i = 0; i < circle_points; ++i) {
        const Real angle = 2 * std::acos(Real(-1)) * i / circle_points;
        circle.push_back({cx + radius * std::cos(angle), cy + radius * std::sin(angle)});
    }
    auto circle_phi = rasterize_copper(axis, axis, {circle});
    Real circle_area = 0;
    for (std::size_t j = 0; j < nx; ++j)
        for (std::size_t i = 0; i < nx; ++i) {
            const auto d =
                level0_dense(40, 40, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j));
            if (circle_phi[d] < 0 || circle_phi[d] > 1)
                return 12;
            circle_area += circle_phi[d] * (axis[i + 1] - axis[i]) * (axis[j + 1] - axis[j]);
        }
    if (std::abs(circle_area - std::acos(Real(-1)) * radius * radius) /
            (std::acos(Real(-1)) * radius * radius) >
        0.001)
        return 13;

    auto coarse = aggregate_phi(phi, axis, axis, 2);
    Real coarse_weighted = 0;
    const std::size_t scale = 4, cn = nx / scale;
    for (std::size_t j = 0; j < cn; ++j)
        for (std::size_t i = 0; i < cn; ++i) {
            const auto d = level0_dense(static_cast<std::uint32_t>(cn),
                                        static_cast<std::uint32_t>(cn),
                                        static_cast<std::uint32_t>(i),
                                        static_cast<std::uint32_t>(j));
            const Real area = (axis[(i + 1) * scale] - axis[i * scale]) *
                              (axis[(j + 1) * scale] - axis[j * scale]);
            coarse_weighted += coarse[d] * area;
        }
    if (std::abs(coarse_weighted - weighted) > 1e-14 * weighted)
        return 4;
    return 0;
}
