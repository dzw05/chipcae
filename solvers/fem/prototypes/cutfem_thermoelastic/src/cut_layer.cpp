#include "cutfem/cut_layer.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
namespace chipcae::proto_cutfem {
Polygon clip_polygon(const Polygon& p, const HalfPlane& h) {
    Polygon out;
    if (p.empty())
        return out;
    Point a = p.back();
    double fa = h.value(a);
    for (Point b : p) {
        double fb = h.value(b);
        if ((fa <= 0) != (fb <= 0)) {
            double t = fa / (fa - fb);
            out.push_back(a + t * (b - a));
        }
        if (fb <= 0)
            out.push_back(b);
        a = b;
        fa = fb;
    }
    Polygon clean;
    for (Point v : out)
        if (clean.empty() || std::hypot(v.x - clean.back().x, v.y - clean.back().y) > 1e-15)
            clean.push_back(v);
    if (clean.size() > 1 &&
        std::hypot(clean.front().x - clean.back().x, clean.front().y - clean.back().y) < 1e-15)
        clean.pop_back();
    return clean;
}
double polygon_area(const Polygon& p) {
    double a = 0;
    if (p.size() < 3)
        return 0;
    Point o = p[0];
    for (size_t i = 1; i + 1 < p.size(); ++i) {
        Point u = p[i] - o, v = p[i + 1] - o;
        a += u.x * v.y - u.y * v.x;
    }
    return std::abs(a) / 2;
}
std::vector<QuadPoint> polygon_quadrature(const Polygon& p) {
    std::vector<QuadPoint> q;
    if (p.size() < 3)
        return q;
    // Dunavant degree-five rule, weights normalized to one triangle area.
    constexpr double a = .05971587178976982046, b = .47014206410511508977,
                     c = .79742698535308732240, d = .10128650732345633880;
    constexpr double bary[7][3] = {
        {1. / 3, 1. / 3, 1. / 3}, {a, b, b}, {b, a, b}, {b, b, a}, {c, d, d}, {d, c, d}, {d, d, c}};
    constexpr double w[7] = {.225,
                             .13239415278850618074,
                             .13239415278850618074,
                             .13239415278850618074,
                             .12593918054482715260,
                             .12593918054482715260,
                             .12593918054482715260};
    for (size_t i = 1; i + 1 < p.size(); ++i) {
        Point u = p[i] - p[0], v = p[i + 1] - p[0];
        double area = std::abs(u.x * v.y - u.y * v.x) / 2;
        if (area == 0)
            continue;
        for (int k = 0; k < 7; ++k)
            q.push_back(
                {bary[k][0] * p[0] + bary[k][1] * p[i] + bary[k][2] * p[i + 1], area * w[k]});
    }
    return q;
}
std::vector<QuadPoint> recursive_quadrature(const std::function<double(Point)>& phi, int depth) {
    if (depth < 0 || depth > 10)
        throw std::runtime_error("Subdivision depth outside [0,10]");
    std::vector<QuadPoint> q;
    int n = 1 << depth;
    double h = 1. / n;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            Point p[4] = {{i * h, j * h},
                          {(i + 1) * h, j * h},
                          {(i + 1) * h, (j + 1) * h},
                          {i * h, (j + 1) * h}};
            for (auto ids : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}}) {
                Point a = p[ids[0]], u = p[ids[1]] - a, v = p[ids[2]] - a;
                double f = phi(a), fu = phi(p[ids[1]]) - f, fv = phi(p[ids[2]]) - f,
                       det = u.x * v.y - u.y * v.x;
                double ax = (fu * v.y - fv * u.y) / det, ay = (u.x * fv - v.x * fu) / det;
                auto poly = clip_polygon({p[ids[0]], p[ids[1]], p[ids[2]]},
                                         {ax, ay, f - ax * a.x - ay * a.y, 0});
                auto qs = polygon_quadrature(poly);
                q.insert(q.end(), qs.begin(), qs.end());
            }
        }
    return q;
}
Body polygon_body(const Polygon& p, int m, int tag) {
    Body b;
    b.material_id = m;
    for (size_t i = 0; i < p.size(); ++i) {
        Point a = p[i], v = p[(i + 1) % p.size()] - a;
        double l = std::hypot(v.x, v.y);
        b.halfplanes.push_back({v.y / l, -v.x / l, (-v.y * a.x + v.x * a.y) / l, tag + int(i)});
    }
    return b;
}
Body rectangle_body(double x0, double x1, double y0, double y1, int m, int tag) {
    return polygon_body({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, m, tag);
}
bool inside(const Body& b, Point p, double tol) {
    for (auto h : b.halfplanes)
        if (h.value(p) > tol)
            return false;
    return true;
}
namespace {
struct BPoint {
    Point p, n;
    double w;
    int tag;
};
struct CellData {
    std::vector<QuadPoint> vq;
    std::vector<BPoint> bq;
    double vf = 0;
    int mat = -1, body = -1;
};
CellData cell_data(const HexMeshArrays& m, int e, const std::vector<Body>& bodies) {
    CellMap map(m, e);
    CellData out;
    for (size_t bi = 0; bi < bodies.size(); ++bi) {
        const auto& body = bodies[bi];
        Polygon poly = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        for (auto h : body.halfplanes)
            poly = clip_polygon(
                poly, {h.a * map.hx, h.b * map.hy, h.a * map.x + h.b * map.y + h.c, h.tag});
        double vf = polygon_area(poly);
        if (vf < 1e-18)
            continue;
        if (out.mat >= 0)
            throw std::runtime_error(
                "Bodies overlap within a grid cell: interfaces must be grid-aligned");
        out.mat = body.material_id;
        out.body = bi;
        out.vf = vf;
        if (vf > 1 - 1e-12) {
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    out.vq.push_back({{gauss3_x[i], gauss3_x[j]}, gauss3_w[i] * gauss3_w[j]});
        } else
            out.vq = polygon_quadrature(poly);
        for (size_t i = 0; i < poly.size(); ++i) {
            Point ra = poly[i], rb = poly[(i + 1) % poly.size()], a = map.physical(ra),
                  b = map.physical(rb);
            double l = std::hypot(a.x - b.x, a.y - b.y);
            if (l < 1e-15)
                continue;
            for (auto h : body.halfplanes) {
                double norm = std::hypot(h.a, h.b);
                double tol = 2e-12 * std::max(map.hx, map.hy) * norm;
                if (h.tag < 0 || std::abs(h.value(a)) > tol || std::abs(h.value(b)) > tol)
                    continue;
                // Include actual body boundaries coinciding with grid faces; internal grid edges
                // are never selected because they do not lie on a tagged body half-plane.
                for (int k = 0; k < 3; ++k)
                    out.bq.push_back({ra + gauss3_x[k] * (rb - ra),
                                      {h.a / norm, h.b / norm},
                                      l * gauss3_w[k],
                                      h.tag});
                break;
            }
        }
    }
    return out;
}
CellData recursive_cell_data(const HexMeshArrays& m,
                             int e,
                             const std::vector<Body>& bodies,
                             const CutIntegrationOptions& options) {
    CellMap map(m, e);
    CellData out;
    int n = 1 << options.max_depth;
    double step = 1. / n;
    for (size_t bi = 0; bi < bodies.size(); ++bi) {
        const auto& body = bodies[bi];
        CellData part;
        auto phi = [&](Point r) {
            Point x = map.physical(r);
            if (options.level_set)
                return options.level_set(x);
            double f = -std::numeric_limits<double>::infinity();
            for (auto hp : body.halfplanes)
                f = std::max(f, hp.value(x) / std::hypot(hp.a, hp.b));
            return f;
        };
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j) {
                Point points[4] = {{i * step, j * step},
                                   {(i + 1) * step, j * step},
                                   {(i + 1) * step, (j + 1) * step},
                                   {i * step, (j + 1) * step}};
                double values[4];
                for (int k = 0; k < 4; ++k)
                    values[k] = phi(points[k]);
                for (auto ids : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}}) {
                    Point a = points[ids[0]], u = points[ids[1]] - a, v = points[ids[2]] - a;
                    double f = values[ids[0]], fu = values[ids[1]] - f, fv = values[ids[2]] - f,
                           det = u.x * v.y - u.y * v.x;
                    double ax = (fu * v.y - fv * u.y) / det, ay = (u.x * fv - v.x * fu) / det;
                    HalfPlane hp{ax, ay, f - ax * a.x - ay * a.y, 0};
                    auto poly = clip_polygon({a, points[ids[1]], points[ids[2]]}, hp);
                    double area = polygon_area(poly);
                    if (area < 1e-20)
                        continue;
                    part.vf += area;
                    auto qs = polygon_quadrature(poly);
                    part.vq.insert(part.vq.end(), qs.begin(), qs.end());
                    double norm = std::hypot(ax / map.hx, ay / map.hy);
                    if (norm == 0)
                        continue;
                    for (size_t k = 0; k < poly.size(); ++k) {
                        Point ra = poly[k], rb = poly[(k + 1) % poly.size()];
                        double tol = 1e-12 * std::hypot(ax, ay);
                        if (std::abs(hp.value(ra)) > tol || std::abs(hp.value(rb)) > tol)
                            continue;
                        Point pa = map.physical(ra), pb = map.physical(rb);
                        double length = std::hypot(pa.x - pb.x, pa.y - pb.y);
                        if (length < 1e-15)
                            continue;
                        int tag = 0;
                        if (!options.level_set) {
                            Point mid = .5 * (pa + pb);
                            double best = -1e300;
                            for (auto h : body.halfplanes) {
                                double d = h.value(mid) / std::hypot(h.a, h.b);
                                if (d > best) {
                                    best = d;
                                    tag = h.tag;
                                }
                            }
                        }
                        if (tag < 0)
                            continue;
                        Point normal{ax / map.hx / norm, ay / map.hy / norm};
                        for (int q = 0; q < 3; ++q)
                            part.bq.push_back(
                                {ra + gauss3_x[q] * (rb - ra), normal, length * gauss3_w[q], tag});
                    }
                }
            }
        if (part.vf < 1e-14)
            continue;
        if (out.mat >= 0)
            throw std::runtime_error("Recursive bodies overlap inside a grid cell");
        out = std::move(part);
        out.mat = body.material_id;
        out.body = bi;
        if (out.vf > 1 - 1e-12) {
            out.vq.clear();
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    out.vq.push_back({{gauss3_x[i], gauss3_x[j]}, gauss3_w[i] * gauss3_w[j]});
        }
    }
    return out;
}
} // namespace
CutLayer build_cut_layer(HexMeshArrays& m,
                         const std::vector<Body>& bodies,
                         const CutIntegrationOptions& options) {
    if (options.max_depth < 0 || options.max_depth > 8)
        throw std::runtime_error("CutLayer max_depth must be in [0,8]");
    if (options.level_set && bodies.size() != 1)
        throw std::runtime_error("General level set expects one body");
    auto data = [&](int e) {
        return options.recursive ? recursive_cell_data(m, e, bodies, options)
                                 : cell_data(m, e, bodies);
    };
    CutLayer c;
    c.bodies = bodies;
    int ne = m.n_elem;
    c.cell_state.resize(ne);
    c.cell_material.assign(ne, -1);
    c.cell_volfrac.resize(ne);
    c.cv_id.assign(ne, -1);
    c.vq_offsets.assign(ne + 1, 0);
    c.bq_offsets.assign(ne + 1, 0);
    std::map<std::pair<int, int>, int> attrs;
    for (size_t b = 0; b < bodies.size(); ++b)
        attrs[{int(b), bodies[b].material_id}] = int(b) + 1;
    // Pass 1: count only. No variable-size quadrature storage is retained per cell.
    int64_t cv = 0;
    for (int e = 0; e < ne; ++e) {
        auto d = data(e);
        c.cell_volfrac[e] = d.vf;
        c.cell_material[e] = d.mat;
        if (d.mat < 0) {
            m.attribute[e] = 0;
            continue;
        }
        m.attribute[e] = attrs[{d.body, d.mat}];
        c.cv_id[e] = cv++;
        c.cell_state[e] = d.vf > 1 - 1e-12 ? 1 : 2;
        c.vq_offsets[e + 1] = d.vq.size();
        c.bq_offsets[e + 1] = d.bq.size();
    }
    // Pass 2: exclusive prefix sums over counts, offsets[0]=0.
    for (int e = 0; e < ne; ++e) {
        c.vq_offsets[e + 1] += c.vq_offsets[e];
        c.bq_offsets[e + 1] += c.bq_offsets[e];
    }
    c.vq_xi.resize(2 * c.vq_offsets.back());
    c.vq_w.resize(c.vq_offsets.back());
    c.bq_xi.resize(2 * c.bq_offsets.back());
    c.bq_w_phys.resize(c.bq_offsets.back());
    c.bq_normal.resize(2 * c.bq_offsets.back());
    c.bq_tag.resize(c.bq_offsets.back());
    // Pass 3: fill the flat CSR arrays using the fixed offsets.
    for (int e = 0; e < ne; ++e)
        if (c.cell_state[e]) {
            auto d = data(e);
            int64_t k = c.vq_offsets[e];
            for (auto q : d.vq) {
                c.vq_xi[2 * k] = q.p.x;
                c.vq_xi[2 * k + 1] = q.p.y;
                c.vq_w[k++] = q.w;
            }
            k = c.bq_offsets[e];
            for (auto q : d.bq) {
                c.bq_xi[2 * k] = q.p.x;
                c.bq_xi[2 * k + 1] = q.p.y;
                c.bq_w_phys[k] = q.w;
                c.bq_normal[2 * k] = q.n.x;
                c.bq_normal[2 * k + 1] = q.n.y;
                c.bq_tag[k++] = q.tag;
            }
        }
    for (int e = 0; e < ne; ++e)
        if (c.cell_state[e])
            for (int dir = 0; dir < 2; ++dir) {
                int nb = dir == 0 ? e + m.ny() : e + 1;
                if ((dir == 0 && e / m.ny() == m.nx() - 1) ||
                    (dir == 1 && e % m.ny() == m.ny() - 1))
                    continue;
                if (c.cell_state[nb] && c.cell_material[e] == c.cell_material[nb] &&
                    (c.cell_state[e] == 2 || c.cell_state[nb] == 2))
                    c.gp_faces.push_back({e, nb, dir});
            }
    return c;
}
CutLayer build_level_set_layer(HexMeshArrays& m,
                               const std::function<double(Point)>& phi,
                               int max_depth,
                               int material_id) {
    if (!phi)
        throw std::runtime_error("Empty level set");
    return build_cut_layer(m, {Body{{}, material_id}}, CutIntegrationOptions{true, max_depth, phi});
}
} // namespace chipcae::proto_cutfem
