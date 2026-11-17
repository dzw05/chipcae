#pragma once
#include "chipcae/mesh/cut/cut_layer.hpp"

#include <memory>
#include <mfem.hpp>
#include <set>
namespace chipcae::fem {
// Indexed by the current (possibly compacted) background element number.
// Rules are copied from mesh-owned CSR, never generated in the solver.
class CutRules {
    std::vector<std::unique_ptr<mfem::IntegrationRule>> rules_;

public:
    CutRules(const mesh::cut::CutLayerArrays&, mfem::Mesh&);
    const mfem::IntegrationRule* get(int e) const { return rules_.at(e).get(); }
    std::vector<double> volumes;
};
class CutBilinear : public mfem::BilinearFormIntegrator {
    const CutRules& rules_;
    std::unique_ptr<mfem::BilinearFormIntegrator> kernel_;

public:
    CutBilinear(const CutRules& r, mfem::BilinearFormIntegrator* k) : rules_(r), kernel_(k) {}
    void AssembleElementMatrix(const mfem::FiniteElement&,
                               mfem::ElementTransformation&,
                               mfem::DenseMatrix&) override;
};
class CutLinear : public mfem::LinearFormIntegrator {
    const CutRules& rules_;
    std::unique_ptr<mfem::LinearFormIntegrator> kernel_;

public:
    CutLinear(const CutRules& r, mfem::LinearFormIntegrator* k) : rules_(r), kernel_(k) {}
    using mfem::LinearFormIntegrator::AssembleRHSElementVect;
    void AssembleRHSElementVect(const mfem::FiniteElement&,
                                mfem::ElementTransformation&,
                                mfem::Vector&) override;
};
// Each face is assembled once by MFEM's serial interior-face traversal. In the
// replicated direct backend each rank assembles a complete independent system.
class GhostPenalty : public mfem::BilinearFormIntegrator {
    std::set<std::pair<int, int>> faces_;
    std::vector<std::array<double, 3>> scale_;
    double gamma_;
    int components_;

public:
    GhostPenalty(const mesh::HexMeshArrays&,
                 const mesh::cut::CutLayerArrays&,
                 const std::vector<double>& scale,
                 double gamma,
                 int components = 1);
    GhostPenalty(const mesh::HexMeshArrays&,
                 const mesh::cut::CutLayerArrays&,
                 const std::vector<std::array<double, 3>>& scale,
                 double gamma,
                 int components = 1);
    using mfem::BilinearFormIntegrator::AssembleFaceMatrix;
    void AssembleFaceMatrix(const mfem::FiniteElement&,
                            const mfem::FiniteElement&,
                            mfem::FaceElementTransformations&,
                            mfem::DenseMatrix&) override;
};
} // namespace chipcae::fem
