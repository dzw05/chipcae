#include "chipcae/mesh/background/mfem_adapter.hpp"

#include "chipcae/mesh/cut/cut_layer.hpp"

#include <algorithm>
#include <mfem.hpp>
#include <set>
#include <type_traits>
namespace chipcae::mesh {
struct MfemAdapter::Impl {
    HexMeshArrays arrays; // Declared first: lives longer than the borrowing Mesh.
    std::unique_ptr<mfem::Mesh> serial;
#ifdef CHIPCAE_ENABLE_MPI
    std::unique_ptr<mfem::ParMesh> parallel;
#endif
    std::vector<Index> gids;
    std::vector<Local> source_elem;
};
MfemAdapter::MfemAdapter(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
MfemAdapter::~MfemAdapter() = default;
MfemAdapter::MfemAdapter(MfemAdapter&&) noexcept = default;
MfemAdapter& MfemAdapter::operator=(MfemAdapter&&) noexcept = default;
Expected<MfemAdapter> MfemAdapter::create(HexMeshArrays arrays, const Communicator& comm) {
    static_assert(std::is_same_v<Local, int> && std::is_same_v<Real, mfem::real_t>);
    if (arrays.attribute.empty() || arrays.partitioning.size() != arrays.attribute.size() ||
        arrays.hex.size() != 8 * arrays.attribute.size() ||
        arrays.elem_gid.size() != arrays.attribute.size())
        return Error{"mfem", "invalid array dimensions"};
    for (auto p : arrays.partitioning)
        if (p < 0 || p >= comm.size())
            return Error{"partitioning", "rank out of communicator range"};
    auto p = std::make_unique<Impl>();
    p->arrays = std::move(arrays);
    auto& a = p->arrays;
    p->serial = std::make_unique<mfem::Mesh>(a.vertices.data(),
                                             static_cast<int>(a.vertices.size() / 3),
                                             a.hex.data(),
                                             mfem::Geometry::CUBE,
                                             a.attribute.data(),
                                             static_cast<int>(a.attribute.size()),
                                             a.bdr_quad.data(),
                                             mfem::Geometry::SQUARE,
                                             a.bdr_attribute.data(),
                                             static_cast<int>(a.bdr_attribute.size()),
                                             3,
                                             3);
    p->serial->Finalize(false, false);
    auto sets = [&](mfem::Mesh& mesh) {
        for (const auto& [name, attrs] : a.attr_table.references) {
            mfem::Array<int> list;
            for (auto v : attrs)
                list.Append(v);
            mesh.attribute_sets.SetAttributeSet(name, list);
        }
    };
    sets(*p->serial);
#ifdef CHIPCAE_ENABLE_MPI
#ifndef MFEM_USE_MPI
#error "CHIPCAE_ENABLE_MPI requires a parallel MFEM build"
#endif
    p->parallel =
        std::make_unique<mfem::ParMesh>(MPI_COMM_WORLD, *p->serial, a.partitioning.data());
    sets(*p->parallel);
#endif
    for (std::size_t e = 0; e < a.attribute.size(); ++e)
        if (a.partitioning[e] == comm.rank()) {
            p->gids.push_back(a.elem_gid[e]);
            p->source_elem.push_back(static_cast<Local>(e));
        }
    return MfemAdapter(std::move(p));
}
mfem::Mesh& MfemAdapter::serial() {
    return *impl_->serial;
}
mfem::Mesh& MfemAdapter::local() {
#ifdef CHIPCAE_ENABLE_MPI
    return *impl_->parallel;
#else
    return *impl_->serial;
#endif
}
#ifdef CHIPCAE_ENABLE_MPI
mfem::ParMesh& MfemAdapter::parallel() {
    return *impl_->parallel;
}
#endif
const std::vector<Index>& MfemAdapter::elem_gid() const {
    return impl_->gids;
}
const HexMeshArrays& MfemAdapter::arrays() const {
    return impl_->arrays;
}
Expected<bool> MfemAdapter::backfill_gp(cut::CutLayerArrays& c, cut::GhostPenaltyCache& cache) {
    auto& mesh = local();
    const auto& a = impl_->arrays;
    std::map<Index, Local> global;
    for (std::size_t e = 0; e < a.elem_gid.size(); ++e)
        global[a.elem_gid[e]] = static_cast<Local>(e);
    auto locate = [&](Index ga, Index gb) -> Expected<Local> {
        if (!global.contains(ga) || !global.contains(gb))
            return Error{"gp", "unknown GID"};
        auto ea = global[ga], eb = global[gb];
        std::set<Local> common;
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 8; ++j)
                if (a.hex[8 * ea + i] == a.hex[8 * eb + j])
                    common.insert(a.hex[8 * ea + i]);
        if (common.size() != 4)
            return Error{"gp", "GID pair is not face adjacent"};
        for (std::size_t le = 0; le < impl_->source_elem.size(); ++le)
            if (impl_->source_elem[le] == ea || impl_->source_elem[le] == eb) {
                mfem::Array<int> faces, ori;
                mesh.GetElementFaces(static_cast<int>(le), faces, ori);
                for (auto f : faces) {
                    mfem::Array<int> fv;
                    mesh.GetFaceVertices(f, fv);
                    bool match = true;
                    for (int v : fv) {
                        const auto* x = mesh.GetVertex(v);
                        bool found = false;
                        for (auto n : common) {
                            bool equal = true;
                            for (int k = 0; k < 3; ++k)
                                equal &= x[k] == a.vertices[3 * n + k];
                            found |= equal;
                        }
                        match &= found;
                    }
                    if (match)
                        return static_cast<Local>(f);
                }
                return Error{"gp", "local face not found"};
            }
        return Local(-1); // Pair does not touch this rank.
    };
    for (std::size_t i = 0; i < c.gp_face.size(); ++i) {
        auto f = locate(c.gp_elem_gid[0][i], c.gp_elem_gid[1][i]);
        if (!f)
            return f.error();
        c.gp_face[i] = f.value();
    }
    for (std::size_t i = 0; i < cache.faces.face.size(); ++i) {
        auto f = locate(cache.faces.elem_gid[0][i], cache.faces.elem_gid[1][i]);
        if (!f)
            return f.error();
        cache.faces.face[i] = f.value();
    }
    return true;
}
} // namespace chipcae::mesh
