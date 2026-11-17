#include "chipcae/solvers/fem/cut_fem.hpp"

#include <algorithm>
#include <cmath>
#include <map>
namespace chipcae::fem {
GhostPenalty::GhostPenalty(const mesh::HexMeshArrays& m,
                           const mesh::cut::CutLayerArrays& c,
                           const std::vector<double>& scale,
                           double gamma,
                           int components)
    : GhostPenalty(
          m,
          c,
          [&] {
              std::vector<std::array<double, 3>> values;
              for (double v : scale)
                  values.push_back({v, v, v});
              return values;
          }(),
          gamma,
          components) {}
GhostPenalty::GhostPenalty(const mesh::HexMeshArrays& m,
                           const mesh::cut::CutLayerArrays& c,
                           const std::vector<std::array<double, 3>>& scale,
                           double gamma,
                           int components)
    : scale_(scale), gamma_(gamma), components_(components) {
    // faces_ stores local element numbers. Cross-rank ghost faces are unsupported:
    // cut assembly uses PETSC_COMM_SELF. Use GID lookup before GMG/distributed use.
    constexpr int reference[8][3] = {
        {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
    for (std::size_t e = 0; e < m.elem_gid.size(); ++e) {
        auto origin = m.hex[8 * e];
        double h[3];
        for (int d = 0; d < 3; ++d) {
            h[d] = m.vertices[3 * m.hex[8 * e + 6] + d] - m.vertices[3 * origin + d];
            if (!(h[d] > 0))
                throw std::runtime_error("GhostPenalty requires positive reference orientation");
        }
        for (int v = 0; v < 8; ++v)
            for (int d = 0; d < 3; ++d)
                if (std::abs(m.vertices[3 * m.hex[8 * e + v] + d] - m.vertices[3 * origin + d] -
                             reference[v][d] * h[d]) > 1e-12 * h[d])
                    throw std::runtime_error("GhostPenalty requires affine axis-aligned cells");
    }
    std::map<Index, int> local;
    for (std::size_t e = 0; e < m.elem_gid.size(); ++e)
        local[m.elem_gid[e]] = e;
    for (std::size_t f = 0; f < c.gp_face_gid.size(); ++f) {
        int a = local.at(c.gp_elem_gid[0][f]), b = local.at(c.gp_elem_gid[1][f]);
        faces_.emplace(std::min(a, b), std::max(a, b));
    }
}
void GhostPenalty::AssembleFaceMatrix(const mfem::FiniteElement& a,
                                      const mfem::FiniteElement& b,
                                      mfem::FaceElementTransformations& t,
                                      mfem::DenseMatrix& matrix) {
    int na = a.GetDof(), nb = b.GetDof(), n = na + nb;
    matrix.SetSize(components_ * n);
    matrix = 0.0;
    if (!faces_.contains({std::min(t.Elem1No, t.Elem2No), std::max(t.Elem1No, t.Elem2No)}) ||
        gamma_ == 0)
        return;
    const auto& rule = mfem::IntRules.Get(t.GetGeometryType(), 8);
    mfem::DenseMatrix da(na, 3), db(nb, 3), ha(na, 6), hb(nb, 6);
    mfem::Vector normal(3), jump(n), jump2(n);
    for (int q = 0; q < rule.GetNPoints(); ++q) {
        const auto& ip = rule.IntPoint(q);
        t.SetAllIntPoints(&ip);
        mfem::CalcOrtho(t.Jacobian(), normal);
        double area = normal.Norml2();
        normal /= area;
        a.CalcPhysDShape(*t.Elem1, da);
        b.CalcPhysDShape(*t.Elem2, db);
        a.CalcHessian(t.GetElement1IntPoint(), ha);
        b.CalcHessian(t.GetElement2IntPoint(), hb);
        // Tensor-grid cells are affine axis-aligned. Use h perpendicular to F,
        // including anisotropic cells; never substitute the cell diameter.
        int axis = 0;
        for (int d = 1; d < 3; ++d)
            if (std::abs(normal[d]) > std::abs(normal[axis]))
                axis = d;
        const int diag[3] = {0, 3, 5};
        double h1 = std::abs(t.Elem1->Jacobian()(axis, axis)),
               h2 = std::abs(t.Elem2->Jacobian()(axis, axis));
        double h = 2 * h1 * h2 / (h1 + h2);
        double sa = scale_.at(t.Elem1->Attribute - 1)[axis];
        double sb = scale_.at(t.Elem2->Attribute - 1)[axis];
        double scale = std::max(sa, sb);
        for (int i = 0; i < n; ++i) {
            bool minus = i < na;
            int j = minus ? i : i - na;
            auto& d = minus ? da : db;
            auto& hs = minus ? ha : hb;
            double hi = minus ? h1 : h2, sign = (minus ? sa : -sb) / scale;
            jump[i] = 0;
            for (int k = 0; k < 3; ++k)
                jump[i] += sign * normal[k] * d(j, k);
            // Across elastic materials, the first normal displacement derivative
            // includes the physical strain (and thermal eigenstrain) jump. Extend
            // curvature there; do not penalize the piecewise affine strain mode.
            if (components_ == 3 && t.Elem1->Attribute != t.Elem2->Attribute)
                jump[i] = 0;
            jump2[i] = sign * hs(j, diag[axis]) / (hi * hi);
        }
        // Stabilize the gradient of the polynomial extension throughout the
        // two-cell patch. Normal traces alone poorly control Q2 modes with
        // tangential variation on a vanishing sliver. This consistent volume
        // jump also controls those modes, without adding energy to a global Q2.
        // At material interfaces retain the flux/curvature jump below: a
        // physical gradient jump must not be extended across that interface.
        if (t.Elem1->Attribute == t.Elem2->Attribute) {
            const auto& depth = mfem::IntRules.Get(mfem::Geometry::SEGMENT, 8);
            auto ia = t.GetElement1IntPoint(), ib = t.GetElement2IntPoint();
            auto coordinate = [axis](mfem::IntegrationPoint& p) -> double& {
                return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
            };
            const double direction = coordinate(ia) > .5 ? 1. : -1.;
            for (int r = 0; r < depth.GetNPoints(); ++r) {
                const auto& rp = depth.IntPoint(r);
                double distance = -h1 + (h1 + h2) * rp.x;
                auto xa = ia, xb = ib;
                coordinate(xa) += direction * distance / h1;
                coordinate(xb) += direction * distance / h2;
                t.Elem1->SetIntPoint(&xa);
                t.Elem2->SetIntPoint(&xb);
                a.CalcPhysDShape(*t.Elem1, da);
                b.CalcPhysDShape(*t.Elem2, db);
                double weight = gamma_ * scale * ip.weight * area * (h1 + h2) * rp.weight / 2;
                for (int i = 0; i < n; ++i)
                    for (int j = 0; j < n; ++j) {
                        double product = 0;
                        for (int d = 0; d < 3; ++d) {
                            double vi = i < na ? da(i, d) : -db(i - na, d);
                            double vj = j < na ? da(j, d) : -db(j - na, d);
                            product += vi * vj;
                        }
                        for (int c = 0; c < components_; ++c) {
                            auto index = [&](int k) {
                                return k < na ? c * na + k : components_ * na + c * nb + k - na;
                            };
                            matrix(index(i), index(j)) += weight * product;
                        }
                    }
            }
            t.SetAllIntPoints(&ip);
        }
        double w = gamma_ * scale * ip.weight * area;
        // MFEM face vector DOFs: [minus components][plus components].
        for (int c = 0; c < components_; ++c)
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    auto index = [&](int k) {
                        return k < na ? c * na + k : components_ * na + c * nb + k - na;
                    };
                    matrix(index(i), index(j)) +=
                        w * (h * jump[i] * jump[j] + h * h * h * jump2[i] * jump2[j]);
                }
    }
}
} // namespace chipcae::fem
