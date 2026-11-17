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
        for (int i = 0; i < n; ++i) {
            bool minus = i < na;
            int j = minus ? i : i - na;
            auto& d = minus ? da : db;
            auto& hs = minus ? ha : hb;
            double hi = minus ? h1 : h2, sign = minus ? 1 : -1;
            jump[i] = 0;
            for (int k = 0; k < 3; ++k)
                jump[i] += sign * normal[k] * d(j, k);
            jump2[i] = sign * hs(j, diag[axis]) / (hi * hi);
        }
        double scale = std::max(scale_.at(t.Elem1->Attribute - 1)[axis],
                                scale_.at(t.Elem2->Attribute - 1)[axis]);
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
