#pragma once
#include "mesh.hpp"

#include <cmath>
#include <functional>
namespace chipcae::proto_cutfem {
struct QuadPoint {
    Point p;
    double w;
};
inline constexpr double gauss3_x[3] = {0.11270166537925831148, 0.5, 0.88729833462074168852};
inline constexpr double gauss3_w[3] = {5. / 18, 4. / 9, 5. / 18};
using Polygon = std::vector<Point>;
struct HalfPlane {
    double a, b, c;
    int tag = 0;
    CHIPCAE_INLINE_FUNCTION double value(Point p) const { return a * p.x + b * p.y + c; }
};
Polygon clip_polygon(const Polygon&, const HalfPlane&);
double polygon_area(const Polygon&);
std::vector<QuadPoint> polygon_quadrature(const Polygon&);
// Corner-sampled general level set; leaf squares are split into two triangles,
// each clipped by its linear interpolant. Planar level sets are integrated exactly.
std::vector<QuadPoint> recursive_quadrature(const std::function<double(Point)>& phi, int max_depth);
} // namespace chipcae::proto_cutfem
