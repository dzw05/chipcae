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
        for (int bad = 0; bad < 7; ++bad) {
            auto invalid = arrays;
            if (bad == 0)
                invalid.vertices.push_back(0);
            if (bad == 1)
                invalid.bdr_quad.pop_back();
            if (bad == 2)
                invalid.hex[0] = -1;
            if (bad == 3)
                invalid.bdr_quad[0] = static_cast<Local>(arrays.node_gid.size());
            if (bad == 4)
                invalid.attribute[0] = 0;
            if (bad == 5)
                for (auto& v : invalid.attribute)
                    ++v;
            if (bad == 6)
                invalid.bdr_attribute[0] = 0;
            auto rejected = MfemAdapter::create(invalid, comm);
            expect(!rejected && rejected.error().path == "mfem", "invalid adapter input");
        }
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
        for (const auto& [name, attrs] : arrays.face_sets) {
            const auto& actual = local.bdr_attribute_sets.GetAttributeSet(name);
            expect(actual.Size() == static_cast<int>(attrs.size()), "boundary attribute set");
        }
        std::vector<double> gid_presence(arrays.elem_gid.size());
        for (int e = 0; e < local.GetNE(); ++e) {
            auto gid = a.elem_gid()[e];
            auto it = std::find(arrays.elem_gid.begin(), arrays.elem_gid.end(), gid);
            expect(it != arrays.elem_gid.end(), "source GID exists");
            auto source = static_cast<std::size_t>(it - arrays.elem_gid.begin());
            mfem::Array<int> vertices;
            local.GetElementVertices(e, vertices);
            expect(vertices.Size() == 8, "hex vertices");
            for (int j = 0; j < 8; ++j)
                for (int d = 0; d < 3; ++d)
                    expect(local.GetVertex(vertices[j])[d] ==
                               arrays.vertices[3 * arrays.hex[8 * source + j] + d],
                           "per element source coordinates");
            ++gid_presence[source];
        }
        comm.allreduce_sum(gid_presence);
        for (auto n : gid_presence)
            expect(n == 1, "complete unique global GID union");
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
        std::map<std::array<double, 3>, std::size_t> node_map;
        for (std::size_t n = 0; n < arrays.node_gid.size(); ++n)
            node_map[{
                arrays.vertices[3 * n], arrays.vertices[3 * n + 1], arrays.vertices[3 * n + 2]}] =
                n;
        std::vector<double> node_presence(arrays.node_gid.size());
        for (int n = 0; n < local.GetNV(); ++n) {
            const auto* v = local.GetVertex(n);
            auto it = node_map.find({v[0], v[1], v[2]});
            expect(it != node_map.end(), "node roundtrip coordinates");
            node_presence[it->second] = 1;
        }
        comm.allreduce_sum(node_presence);
        expect(std::count_if(
                   node_presence.begin(), node_presence.end(), [](double n) { return n > 0; }) ==
                   static_cast<std::ptrdiff_t>(arrays.node_gid.size()),
               "global unique vertex count");
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
        auto local_cut = ca.value().local_cut(cut.value());
        expect(bool(local_cut), "localized cut arrays");
        for (auto e : local_cut.value().cut_elem)
            expect(e >= 0 && e < ca.value().local().GetNE(), "local cut numbering");
        for (const auto& ls : local_cut.value().level_sets)
            expect(ls.phi.size() == static_cast<std::size_t>(ca.value().local().GetNV()),
                   "local-node level sets");
    }
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Finalize();
#endif
}
