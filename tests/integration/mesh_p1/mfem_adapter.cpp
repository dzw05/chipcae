#include "chipcae/mesh/background/mfem_adapter.hpp"

#include "chipcae/mesh/cut/plane_cut.hpp"
#include "support.hpp"

#include <algorithm>
#include <mfem.hpp>
#include <set>
using namespace chipcae::mesh;
using k1::expect;
int main(int argc, char** argv) {
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Init(&argc, &argv);
#else
    (void)argc;
    (void)argv;
#endif
    {
        const auto comm = chipcae::Communicator::world();
        auto s = k1::stack();
        auto o = k1::options();
        o.nx = 4;
        o.ny = 4;
        o.ranks = comm.size();
        auto mesh = build_tensor_grid(s, o);
        expect(bool(mesh), "background");
        auto arrays = mesh.value();
        auto adapter = MfemAdapter::create(arrays, comm);
        expect(bool(adapter), "adapter");
        auto& a = adapter.value();
        auto& local = a.local();
        expect(a.serial().GetNV() == static_cast<int>(arrays.node_gid.size()),
               "serial vertex roundtrip");
        expect(a.serial().GetNBE() == static_cast<int>(arrays.bdr_attribute.size()),
               "serial boundary roundtrip");
        expect(std::set<chipcae::Index>(a.elem_gid().begin(), a.elem_gid().end()).size() ==
                   a.elem_gid().size(),
               "local GID bijection");
        expect(a.elem_gid().size() == static_cast<std::size_t>(local.GetNE()), "local GID count");
        std::vector<double> counts(arrays.attr_table.entries.size() + 2);
        counts[0] = local.GetNE();
        counts[1] = local.GetNBE();
        for (int e = 0; e < local.GetNE(); ++e) {
            ++counts[static_cast<std::size_t>(local.GetAttribute(e) + 1)];
            auto* t = local.GetElementTransformation(e);
            const auto& rule = mfem::IntRules.Get(mfem::Geometry::CUBE, 3);
            for (int q = 0; q < rule.GetNPoints(); ++q) {
                t->SetIntPoint(&rule.IntPoint(q));
                expect(t->Jacobian().Det() > 0, "MFEM det J");
            }
        }
        comm.allreduce_sum(counts);
        expect(counts[0] == arrays.attribute.size(), "global element count");
        expect(counts[1] == arrays.bdr_attribute.size(), "global boundary count");
        for (std::size_t attr = 1; attr <= arrays.attr_table.entries.size(); ++attr)
            expect(counts[attr + 1] == std::count(arrays.attribute.begin(),
                                                  arrays.attribute.end(),
                                                  static_cast<Local>(attr)),
                   "global attribute count");
        auto cut = cut::build_cut_layer(arrays, {{{0, {1, 1, 0}, 1}}}, {});
        expect(bool(cut), "cut");
        auto ca = MfemAdapter::create(arrays, comm);
        expect(bool(ca), "cut adapter");
        ActivationMask active;
        for (std::size_t i = 1; i <= arrays.attr_table.entries.size(); ++i)
            active.push_back(static_cast<Local>(i));
        auto cache = cut::GhostPenaltyCache::prepare(arrays, cut.value(), {active});
        expect(bool(cache), "GP cache");
        expect(bool(ca.value().backfill_gp(cut.value(), cache.value())), "GP backfill");
    }
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Finalize();
#endif
}
