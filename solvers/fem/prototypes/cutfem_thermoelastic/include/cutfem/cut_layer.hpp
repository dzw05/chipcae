#pragma once
#include "quadrature.hpp"
namespace chipcae::proto_cutfem {
struct Body {
    std::vector<HalfPlane> halfplanes;
    int material_id = 0;
};
struct GhostFace {
    int cell1, cell2;
    int orientation;
}; // 0: x-normal, 1: y-normal
struct CutLayer {
    std::vector<Body> bodies;
    std::vector<int32_t> cell_state, cell_material;
    std::vector<double> cell_volfrac;
    std::vector<int64_t> vq_offsets, bq_offsets, cv_id;
    std::vector<double> vq_xi, vq_w, bq_xi, bq_w_phys, bq_normal;
    std::vector<int32_t> bq_tag;
    std::vector<GhostFace> gp_faces;
};
Body polygon_body(const Polygon& p, int material = 0, int first_tag = 0);
Body rectangle_body(
    double x0, double x1, double y0, double y1, int material = 0, int first_tag = 0);
struct CutIntegrationOptions {
    bool recursive = false;
    int max_depth = 2;
    // Optional general scalar level set for a single body. If empty, use max(phi_k).
    std::function<double(Point)> level_set;
};
CutLayer
build_cut_layer(HexMeshArrays&, const std::vector<Body>&, const CutIntegrationOptions& = {});
CutLayer build_level_set_layer(HexMeshArrays&,
                               const std::function<double(Point)>&,
                               int max_depth,
                               int material_id = 0);
bool inside(const Body&, Point, double tolerance = 0);
} // namespace chipcae::proto_cutfem
