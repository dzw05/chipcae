#pragma once
#include <array>
#include <cstdint>
#include <vector>
#ifndef CHIPCAE_INLINE_FUNCTION
#define CHIPCAE_INLINE_FUNCTION inline
#endif
namespace chipcae::proto_cutfem {
struct Point {
    double x = 0, y = 0;
};
CHIPCAE_INLINE_FUNCTION Point operator+(Point a, Point b) {
    return {a.x + b.x, a.y + b.y};
}
CHIPCAE_INLINE_FUNCTION Point operator-(Point a, Point b) {
    return {a.x - b.x, a.y - b.y};
}
CHIPCAE_INLINE_FUNCTION Point operator*(double s, Point a) {
    return {s * a.x, s * a.y};
}
CHIPCAE_INLINE_FUNCTION double dot(Point a, Point b) {
    return a.x * b.x + a.y * b.y;
}
struct HexMeshArrays {
    int dim = 2, n_vert = 0, n_elem = 0;
    std::vector<double> vertices, xcoords, ycoords;
    std::vector<int32_t> elem_vert, attribute, partitioning;
    std::vector<int64_t> elem_gid, node_gid;
    int nx() const { return int(xcoords.size()) - 1; }
    int ny() const { return int(ycoords.size()) - 1; }
    Point node(int i) const { return {vertices[2 * i], vertices[2 * i + 1]}; }
    std::array<int, 4> nodes(int e) const {
        return {elem_vert[4 * e], elem_vert[4 * e + 1], elem_vert[4 * e + 2], elem_vert[4 * e + 3]};
    }
};
HexMeshArrays tensor_mesh(std::vector<double> x, std::vector<double> y);
std::vector<double> linspace(double a, double b, int n);
CHIPCAE_INLINE_FUNCTION void q1(double x, double y, double N[4], double dx[4], double dy[4]) {
    N[0] = (1 - x) * (1 - y);
    N[1] = x * (1 - y);
    N[2] = x * y;
    N[3] = (1 - x) * y;
    dx[0] = y - 1;
    dx[1] = 1 - y;
    dx[2] = y;
    dx[3] = -y;
    dy[0] = x - 1;
    dy[1] = -x;
    dy[2] = x;
    dy[3] = 1 - x;
}
struct CellMap {
    double x, y, hx, hy;
    explicit CellMap(const HexMeshArrays& m, int e) {
        auto n = m.nodes(e);
        auto a = m.node(n[0]);
        auto b = m.node(n[2]);
        x = a.x;
        y = a.y;
        hx = b.x - x;
        hy = b.y - y;
    }
    CHIPCAE_INLINE_FUNCTION Point physical(Point r) const { return {x + hx * r.x, y + hy * r.y}; }
    CHIPCAE_INLINE_FUNCTION Point reference(Point p) const {
        return {(p.x - x) / hx, (p.y - y) / hy};
    }
    CHIPCAE_INLINE_FUNCTION void shape(Point r, double N[4], double dx[4], double dy[4]) const {
        q1(r.x, r.y, N, dx, dy);
        for (int i = 0; i < 4; ++i) {
            dx[i] /= hx;
            dy[i] /= hy;
        }
    }
};
} // namespace chipcae::proto_cutfem
