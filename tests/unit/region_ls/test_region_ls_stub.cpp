#include "chipcae/mesh/cut/cut_layer.hpp"
#include "chipcae/mesh/cut/region_level_set.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

static_assert(std::is_same_v<decltype(chipcae::mesh::region_level_sets({}, {}, {})),
                             std::vector<chipcae::mesh::BodyLevelSet>>);
static_assert(std::is_same_v<chipcae::mesh::BodyLevelSet, chipcae::mesh::cut::BodyLevelSet>);
static_assert(std::is_same_v<decltype(chipcae::mesh::cut::CutLayerArrays::level_sets),
                             std::vector<chipcae::mesh::BodyLevelSet>>);
static_assert(std::is_same_v<decltype(chipcae::mesh::BodyLevelSet::layer_index), std::size_t>);
static_assert(std::is_same_v<decltype(chipcae::mesh::BodyLevelSet::body), chipcae::BodyId>);
static_assert(
    std::is_same_v<decltype(chipcae::mesh::BodyLevelSet::phi), std::vector<chipcae::Real>>);

static_assert(chipcae::mesh::RegionLSOptions{}.inside_tol_rel == 1e-12);

int main() {
    int failures = 0;
    const std::vector<chipcae::mesh::BodyLevelSet> groups{{chipcae::BodyId{7}, {-1.0, 2.0}, 0},
                                                          {chipcae::BodyId{7}, {3.0, -4.0}, 1}};
    if (groups[0].layer_index == groups[1].layer_index || groups[0].body != groups[1].body ||
        groups[0].phi != std::vector<chipcae::Real>{-1.0, 2.0} ||
        groups[1].phi != std::vector<chipcae::Real>{3.0, -4.0}) {
        std::cerr << "FAIL: repeated bodies must retain distinct layer identities and samples\n";
        ++failures;
    }
    try {
        (void)chipcae::mesh::region_level_sets({}, {}, {});
        std::cerr << "FAIL: (void)chipcae::mesh::region_level_sets({}, {}, {}); must throw "
                     "std::logic_error with prefix not implemented: \n";
        ++failures;
    } catch (const std::logic_error& error) {
        if (std::string_view(error.what()).find("not implemented: ") != 0) {
            std::cerr << "FAIL: wrong exception prefix: " << error.what() << '\n';
            ++failures;
        }
    } catch (...) {
        std::cerr << "FAIL: expected std::logic_error\n";
        ++failures;
    }
    return failures ? 1 : 0;
}
