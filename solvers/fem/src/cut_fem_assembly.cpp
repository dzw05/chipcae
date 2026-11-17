#include "chipcae/solvers/fem/cut_fem.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace chipcae::fem {
CutRules::CutRules(const mesh::cut::CutLayerArrays& cut, mfem::Mesh& mesh)
    : rules_(mesh.GetNE()), volumes(mesh.GetNE()) {
    for (int e = 0; e < mesh.GetNE(); ++e)
        volumes[e] = mesh.GetElementVolume(e);
    for (std::size_t s = 0; s < cut.cut_elem.size(); ++s) {
        int e = cut.cut_elem[s];
        auto rule = std::make_unique<mfem::IntegrationRule>(cut.vq_off[s + 1] - cut.vq_off[s]);
        double volume = 0;
        auto* t = mesh.GetElementTransformation(e);
        for (int q = cut.vq_off[s]; q < cut.vq_off[s + 1]; ++q) {
            auto& ip = rule->IntPoint(q - cut.vq_off[s]);
            ip.Set3(cut.vq_xi[0][q], cut.vq_xi[1][q], cut.vq_xi[2][q]);
            ip.weight = cut.vq_w[q];
            t->SetIntPoint(&ip);
            volume += ip.weight * t->Weight();
        }
        if (!(volume > 0) || !std::isfinite(volume))
            throw std::runtime_error("nonpositive cut integral volume");
        if (cut.polyhedra) {
            const auto& p = *cut.polyhedra;
            auto it =
                std::find(p.parent_elem_gid.begin(), p.parent_elem_gid.end(), cut.cut_elem_gid[s]);
            if (it == p.parent_elem_gid.end())
                throw std::runtime_error("missing cut polyhedron");
            double geometric = p.volume[it - p.parent_elem_gid.begin()];
            if (std::abs(volume - geometric) > 1e-12 * volume)
                throw std::runtime_error("cut polyhedron/integral volume mismatch");
        }
        volumes[e] = volume;
        rules_.at(e) = std::move(rule);
    }
}
void CutBilinear::AssembleElementMatrix(const mfem::FiniteElement& el,
                                        mfem::ElementTransformation& t,
                                        mfem::DenseMatrix& a) {
    kernel_->SetIntRule(rules_.get(t.ElementNo));
    kernel_->AssembleElementMatrix(el, t, a);
}
void CutLinear::AssembleRHSElementVect(const mfem::FiniteElement& el,
                                       mfem::ElementTransformation& t,
                                       mfem::Vector& b) {
    kernel_->SetIntRule(rules_.get(t.ElementNo));
    kernel_->AssembleRHSElementVect(el, t, b);
}
} // namespace chipcae::fem
