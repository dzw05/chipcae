#pragma once
#include "chipcae/solve/case_loader.hpp"

#include <cmath>
#include <mfem.hpp>
#include <set>
namespace chipcae::solve {
class CutSurfaceRule {
    std::vector<mfem::IntegrationRule> rules_;

public:
    CutSurfaceRule(const mesh::HexMeshArrays& m,
                   const mesh::cut::CutLayerArrays& c,
                   const std::string& target)
        : rules_(m.attribute.size()) {
        const std::vector<std::string> faces = {
            "face:xmin", "face:xmax", "face:ymin", "face:ymax", "face:zmin", "face:zmax"};
        for (std::size_t s = 0; s < c.cut_elem.size(); ++s) {
            int e = c.cut_elem[s];
            for (int q = c.sq_off[s]; q < c.sq_off[s + 1]; ++q) {
                int side = c.sq_side[q];
                bool selected = target == "boundary:embedded" && side < 0;
                if (side >= 0) {
                    int axis = side / 2;
                    double bound = side % 2 ? m.axes[axis].back() : m.axes[axis].front();
                    int vertex = m.hex[8 * e + (side % 2 ? 6 : 0)];
                    bool exterior = std::abs(m.vertices[3 * vertex + axis] - bound) < 1e-14;
                    selected =
                        exterior &&
                        (target == faces[side] ||
                         target == "layer:" + m.attr_table.entries[m.attribute[e] - 1].layer);
                } else if (target == "layer:" + m.attr_table.entries[m.attribute[e] - 1].layer)
                    selected = true;
                if (!selected)
                    continue;
                mfem::IntegrationPoint ip;
                ip.Set3(c.sq_xi[0][q], c.sq_xi[1][q], c.sq_xi[2][q]);
                ip.weight = c.sq_w[q];
                rules_[e].Append(ip);
            }
        }
    }
    const mfem::IntegrationRule& get(int e) const { return rules_.at(e); }
};
class CutSurfaceMass : public mfem::BilinearFormIntegrator {
    const CutSurfaceRule& rule_;
    double h_;

public:
    CutSurfaceMass(const CutSurfaceRule& r, double h) : rule_(r), h_(h) {}
    void AssembleElementMatrix(const mfem::FiniteElement& el,
                               mfem::ElementTransformation& t,
                               mfem::DenseMatrix& a) override {
        int n = el.GetDof();
        a.SetSize(n);
        a = 0.;
        mfem::Vector shape(n);
        const auto& r = rule_.get(t.ElementNo);
        for (int q = 0; q < r.GetNPoints(); ++q) {
            auto& ip = r.IntPoint(q);
            el.CalcShape(ip, shape);
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j)
                    a(i, j) += h_ * ip.weight * shape[i] * shape[j];
        }
    }
};
class CutSurfaceLoad : public mfem::LinearFormIntegrator {
    const CutSurfaceRule& rule_;
    double hT_;

public:
    CutSurfaceLoad(const CutSurfaceRule& r, double hT) : rule_(r), hT_(hT) {}
    using mfem::LinearFormIntegrator::AssembleRHSElementVect;
    void AssembleRHSElementVect(const mfem::FiniteElement& el,
                                mfem::ElementTransformation& t,
                                mfem::Vector& b) override {
        int n = el.GetDof();
        b.SetSize(n);
        b = 0.;
        mfem::Vector shape(n);
        const auto& r = rule_.get(t.ElementNo);
        for (int q = 0; q < r.GetNPoints(); ++q) {
            auto& ip = r.IntPoint(q);
            el.CalcShape(ip, shape);
            b.Add(hT_ * ip.weight, shape);
        }
    }
};
// Existing Cartesian boundary integrators handle regular cells only; the
// surviving parts of cut-cell faces use the mesh-owned surface rule above.
class RegularBoundaryMass : public mfem::MassIntegrator {
    mfem::Mesh& mesh_;
    const std::set<int>& cut_;

public:
    RegularBoundaryMass(mfem::Coefficient& c, mfem::Mesh& m, const std::set<int>& cut)
        : MassIntegrator(c), mesh_(m), cut_(cut) {}
    void AssembleElementMatrix(const mfem::FiniteElement& el,
                               mfem::ElementTransformation& t,
                               mfem::DenseMatrix& a) override {
        int e, info;
        mesh_.GetBdrElementAdjacentElement(t.ElementNo, e, info);
        if (cut_.contains(e)) {
            a.SetSize(el.GetDof());
            a = 0.;
        } else
            MassIntegrator::AssembleElementMatrix(el, t, a);
    }
};
class RegularBoundaryLoad : public mfem::BoundaryLFIntegrator {
    mfem::Mesh& mesh_;
    const std::set<int>& cut_;

public:
    RegularBoundaryLoad(mfem::Coefficient& c, mfem::Mesh& m, const std::set<int>& cut)
        : BoundaryLFIntegrator(c), mesh_(m), cut_(cut) {}
    using mfem::BoundaryLFIntegrator::AssembleRHSElementVect;
    void AssembleRHSElementVect(const mfem::FiniteElement& el,
                                mfem::ElementTransformation& t,
                                mfem::Vector& b) override {
        int e, info;
        mesh_.GetBdrElementAdjacentElement(t.ElementNo, e, info);
        if (cut_.contains(e)) {
            b.SetSize(el.GetDof());
            b = 0.;
        } else
            BoundaryLFIntegrator::AssembleRHSElementVect(el, t, b);
    }
};
} // namespace chipcae::solve
