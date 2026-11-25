#pragma once
#include "chipcae/core/types.hpp"

#include <algorithm>
#include <hdf5.h>
#include <stdexcept>
#include <string>
#include <vector>
namespace chipcae::hdf_detail {
struct WriterContext {
    bool collective = false;
    int rank = 0, size = 1;
};
struct ErrorSilencer {
    H5E_auto2_t handler{};
    void* data{};
    ErrorSilencer() {
        H5Eget_auto2(H5E_DEFAULT, &handler, &data);
        H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
    }
    ~ErrorSilencer() { H5Eset_auto2(H5E_DEFAULT, handler, data); }
};
inline void check(herr_t status) {
    if (status < 0)
        throw std::runtime_error("HDF5 operation failed");
}
struct H5 {
    hid_t id;
    herr_t (*close)(hid_t);
    H5(hid_t value, herr_t (*closer)(hid_t)) : id(value), close(closer) {
        if (id < 0)
            throw std::runtime_error("HDF5 object creation failed");
    }
    ~H5() {
        if (id >= 0)
            close(id);
    }
    H5(const H5&) = delete;
    H5& operator=(const H5&) = delete;
    operator hid_t() const { return id; }
};
template <class T>
hid_t native();
template <>
inline hid_t native<Real>() {
    return H5T_NATIVE_DOUBLE;
}
template <>
inline hid_t native<std::int32_t>() {
    return H5T_NATIVE_INT32;
}
template <>
inline hid_t native<Index>() {
    return H5T_NATIVE_INT64;
}
template <>
inline hid_t native<unsigned char>() {
    return H5T_NATIVE_UCHAR;
}
template <class T>
void dataset(hid_t parent,
             const char* name,
             const std::vector<T>& data,
             const WriterContext& ctx,
             hsize_t components = 1,
             hid_t dataset_creation_plist = H5P_DEFAULT) {
    (void)ctx;
    hsize_t dims[2] = {data.size() / components, components};
    H5 space(H5Screate_simple(components == 1 ? 1 : 2, dims, nullptr), H5Sclose);
    H5 set(H5Dcreate2(
               parent, name, native<T>(), space, H5P_DEFAULT, dataset_creation_plist, H5P_DEFAULT),
           H5Dclose);
    H5 transfer(H5Pcreate(H5P_DATASET_XFER), H5Pclose);
#if defined(H5_HAVE_PARALLEL) && defined(CHIPCAE_ENABLE_MPI)
    if (ctx.collective) {
        check(H5Pset_dxpl_mpio(transfer, H5FD_MPIO_COLLECTIVE));
        hsize_t begin = dims[0] * static_cast<hsize_t>(ctx.rank) / static_cast<hsize_t>(ctx.size);
        hsize_t end = dims[0] * static_cast<hsize_t>(ctx.rank + 1) / static_cast<hsize_t>(ctx.size);
        hsize_t count[2] = {end - begin, components}, start[2] = {begin, 0};
        hsize_t memory_dims[2] = {std::max(hsize_t(1), end - begin), components};
        H5 memory(H5Screate_simple(components == 1 ? 1 : 2, memory_dims, nullptr), H5Sclose);
        if (end == begin) {
            check(H5Sselect_none(space));
            check(H5Sselect_none(memory));
        } else
            check(H5Sselect_hyperslab(space, H5S_SELECT_SET, start, nullptr, count, nullptr));
        check(H5Dwrite(set,
                       native<T>(),
                       memory,
                       space,
                       transfer,
                       data.empty() ? nullptr : data.data() + begin * components));
        return;
    }
#endif
    if (!data.empty())
        check(H5Dwrite(set, native<T>(), H5S_ALL, H5S_ALL, transfer, data.data()));
}
inline void header(hid_t root, const std::string& type, int major = 2, int minor = 0) {
    hsize_t two = 2;
    H5 vs(H5Screate_simple(1, &two, nullptr), H5Sclose);
    H5 v(H5Acreate2(root, "Version", H5T_NATIVE_INT, vs, H5P_DEFAULT, H5P_DEFAULT), H5Aclose);
    int version[2] = {major, minor};
    check(H5Awrite(v, H5T_NATIVE_INT, version));
    H5 ts(H5Tcopy(H5T_C_S1), H5Tclose);
    check(H5Tset_size(ts, type.size() + 1));
    check(H5Tset_strpad(ts, H5T_STR_NULLPAD));
    H5 scalar(H5Screate(H5S_SCALAR), H5Sclose);
    H5 a(H5Acreate2(root, "Type", ts, scalar, H5P_DEFAULT, H5P_DEFAULT), H5Aclose);
    check(H5Awrite(a, ts, type.c_str()));
}
} // namespace chipcae::hdf_detail
