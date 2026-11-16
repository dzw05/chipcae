#include "chipcae/mesh/background/vtkhdf_writer.hpp"
#include "chipcae/mesh/cut/plane_cut.hpp"
#include "support.hpp"

#include <hdf5.h>
#ifdef CHIPCAE_ENABLE_MPI
#include <mpi.h>
#endif
#include <algorithm>
#include <numeric>
#include <set>
using namespace chipcae::mesh;
using chipcae::Index;
using chipcae::Real;
using k1::expect;
template <class T>
std::vector<T> read(hid_t file, const char* name, hid_t type, std::vector<hsize_t> shape) {
    hid_t d = H5Dopen2(file, name, H5P_DEFAULT);
    expect(d >= 0, "dataset exists");
    hid_t s = H5Dget_space(d);
    expect(H5Sget_simple_extent_ndims(s) == static_cast<int>(shape.size()), "dataset rank");
    std::vector<hsize_t> dims(shape.size());
    expect(H5Sget_simple_extent_dims(s, dims.data(), nullptr) >= 0 && dims == shape,
           "dataset shape");
    std::vector<T> values(static_cast<std::size_t>(
        std::accumulate(shape.begin(), shape.end(), hsize_t(1), std::multiplies<>())));
    if (!values.empty())
        expect(H5Dread(d, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, values.data()) >= 0, "dataset read");
    expect(H5Sclose(s) >= 0 && H5Dclose(d) >= 0, "dataset close");
    return values;
}
int main(int argc, char** argv) {
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Init(&argc, &argv);
#else
    (void)argc;
    (void)argv;
#endif
    {
        auto comm = chipcae::Communicator::world();
        auto s = k1::stack();
        auto o = k1::options();
        auto m = build_tensor_grid(s, o).value();
        cut::CutOptions co;
        co.snap_phi = k1::threshold("snap_phi");
        co.vf_min = k1::threshold("vf_min");
        auto c = cut::build_cut_layer(m, {{{0, {1, 1, 0}, 1}}}, co);
        expect(bool(c), "cut build");
        auto result = write_vtkhdf("k1-output", m, &c.value(), comm);
        expect(bool(result), "VTKHDF write");
        auto nc = static_cast<hsize_t>(m.attribute.size()),
             nv = static_cast<hsize_t>(std::set<Local>(m.hex.begin(), m.hex.end()).size());
        hid_t file = H5Fopen(result.value().grid.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
        expect(file >= 0, "open grid");
        auto types = read<unsigned char>(file, "VTKHDF/Types", H5T_NATIVE_UCHAR, {nc});
        for (auto t : types)
            expect(t == 12, "VTK_HEXAHEDRON");
        auto attr = read<Local>(file, "VTKHDF/CellData/attribute", H5T_NATIVE_INT32, {nc});
        expect(attr == m.attribute, "attribute values");
        for (auto name : {"layer", "region", "material", "body", "rank", "cell_kind"}) {
            auto path = std::string("VTKHDF/CellData/") + name;
            auto field = read<Local>(file, path.c_str(), H5T_NATIVE_INT32, {nc});
            for (std::size_t e = 0; e < field.size(); ++e) {
                auto a = m.attr_table.entries[m.attribute[e] - 1];
                if (std::string(name) == "material")
                    expect(field[e] == a.material, "material values");
                if (std::string(name) == "body")
                    expect(field[e] == a.body, "body values");
            }
        }
        auto vf = read<Real>(file, "VTKHDF/CellData/vol_frac", H5T_NATIVE_DOUBLE, {nc});
        for (auto x : vf)
            expect(x >= 0 && x <= 1, "volume fractions");
        auto pts = read<Real>(file, "VTKHDF/Points", H5T_NATIVE_DOUBLE, {nv, 3});
        expect(!pts.empty(), "points");
        auto offsets = read<Index>(file, "VTKHDF/Offsets", H5T_NATIVE_INT64, {nc + 1});
        expect(offsets.back() == static_cast<Index>(8 * nc), "offsets");
        read<Index>(file, "VTKHDF/Connectivity", H5T_NATIVE_INT64, {8 * nc});
        expect(H5Fclose(file) >= 0, "close grid");
        file = H5Fopen(result.value().embedded_boundary.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
        expect(file >= 0, "open EB");
        auto ebn = read<Index>(file, "VTKHDF/NumberOfPoints", H5T_NATIVE_INT64, {1})[0];
        expect(ebn == static_cast<Index>(2 * c.value().bq_w.size()), "EB points and facets");
        read<Real>(file, "VTKHDF/Points", H5T_NATIVE_DOUBLE, {static_cast<hsize_t>(ebn), 3});
        read<Real>(
            file, "VTKHDF/PointData/normal", H5T_NATIVE_DOUBLE, {static_cast<hsize_t>(ebn), 3});
        auto weights = read<Real>(
            file, "VTKHDF/PointData/area_weight", H5T_NATIVE_DOUBLE, {static_cast<hsize_t>(ebn)});
        expect(std::abs(std::accumulate(weights.begin(), weights.end(), 0.) -
                        std::accumulate(c.value().bq_w.begin(), c.value().bq_w.end(), 0.)) <=
                   k1::threshold("closure_relative"),
               "EB weights");
        expect(H5Fclose(file) >= 0, "close EB");
        std::filesystem::remove(result.value().grid);
        std::filesystem::remove(result.value().embedded_boundary);
    }
#ifdef CHIPCAE_ENABLE_MPI
    MPI_Finalize();
#endif
}
