#pragma once
#include "chipcae/mesh/background/tensor_grid.hpp"

#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
namespace k1 {
inline void expect(bool value, const char* reason) {
    if (!value)
        throw std::runtime_error(reason);
}
inline double threshold(const std::string& key) {
    std::ifstream in(std::string(K1_ROOT) + "/mesh/background/thresholds/k1.json");
    std::string text((std::istreambuf_iterator<char>(in)), {});
    auto pos = text.find('"' + key + '"');
    if (pos == std::string::npos)
        throw std::runtime_error("missing threshold: " + key);
    return std::stod(text.substr(text.find(':', pos) + 1));
}
inline chipcae::mesh::StackModel stack() {
    using namespace chipcae::mesh;
    StackModel s;
    s.id = "test";
    s.outline.box = {0, 0, 1, 1};
    Layer a;
    a.name = "base";
    a.kind = "Explicit";
    a.material = "organic";
    a.thickness = .5;
    a.n_elem_min = 2;
    a.regions.push_back({"die", "silicon", "die", {{.25, .25, .75, .75}, {}}});
    Layer b = a;
    b.name = "top";
    b.body = "cap";
    b.regions.clear();
    s.layers = {a, b};
    s.activation_groups = {{"both", {"layer:base", "body:cap"}},
                           {"overlap", {"region:die", "layer:base"}}};
    return s;
}
inline chipcae::mesh::GridOptions options() {
    chipcae::mesh::GridOptions o;
    o.snap_tol = threshold("snap_tol");
    o.max_ratio = threshold("max_ratio");
    return o;
}
} // namespace k1
