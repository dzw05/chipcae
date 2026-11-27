#include "cutfem/mesh.hpp"

#include <algorithm>
#include <stdexcept>
namespace chipcae::proto_cutfem {
std::vector<double> linspace(double a, double b, int n) {
    if (n < 1 || !(b > a))
        throw std::runtime_error("Invalid linspace interval");
    std::vector<double> v(n + 1);
    for (int i = 0; i <= n; ++i)
        v[i] = a + (b - a) * i / n;
    v.front() = a;
    v.back() = b;
    return v;
}
HexMeshArrays tensor_mesh(std::vector<double> x, std::vector<double> y) {
    if (x.size() < 2 || y.size() < 2)
        throw std::runtime_error("Empty grid");
    for (auto* v : {&x, &y})
        for (size_t i = 1; i < v->size(); ++i)
            if ((*v)[i] <= (*v)[i - 1])
                throw std::runtime_error("Nonmonotone grid");
    HexMeshArrays m;
    m.xcoords = x;
    m.ycoords = y;
    m.n_vert = x.size() * y.size();
    m.n_elem = (x.size() - 1) * (y.size() - 1);
    for (double a : x)
        for (double b : y) {
            m.vertices.push_back(a);
            m.vertices.push_back(b);
            m.node_gid.push_back(m.node_gid.size());
        }
    int ny = m.ny();
    for (int i = 0; i < m.nx(); ++i)
        for (int j = 0; j < ny; ++j) {
            int a = i * (ny + 1) + j;
            for (int n : {a, a + ny + 1, a + ny + 2, a + 1})
                m.elem_vert.push_back(n);
            m.elem_gid.push_back(m.elem_gid.size());
        }
    m.attribute.assign(m.n_elem, 1);
    m.partitioning.assign(m.n_elem, 0);
    return m;
}
} // namespace chipcae::proto_cutfem
