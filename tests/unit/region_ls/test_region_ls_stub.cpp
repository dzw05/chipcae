#include "chipcae/mesh/cut/cut_layer.hpp"
#include "chipcae/mesh/cut/region_level_set.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

static_assert(std::is_same_v<decltype(chipcae::mesh::region_level_sets({}, {}, {})),
                             std::vector<chipcae::mesh::BodyLevelSet>>);
static_assert(std::is_same_v<chipcae::mesh::BodyLevelSet, chipcae::mesh::cut::BodyLevelSet>);
static_assert(std::is_same_v<decltype(chipcae::mesh::cut::CutLayerArrays::level_sets),
                             std::vector<chipcae::mesh::BodyLevelSet>>);
static_assert(std::is_same_v<decltype(chipcae::mesh::BodyLevelSet::body), chipcae::BodyId>);
static_assert(
    std::is_same_v<decltype(chipcae::mesh::BodyLevelSet::phi), std::vector<chipcae::Real>>);

static_assert(chipcae::mesh::RegionLSOptions{}.inside_tol_rel == 1e-12);

int main() {
    int failures = 0;
    // The same BodyId may occur in distinct source layers. Provenance travels
    // with each entry when consumers reorder or filter the returned vector.
    chipcae::mesh::BodyLevelSet first, second;
    first.body = second.body = chipcae::BodyId{};
    first.layer_index = 2;
    second.layer_index = 5;
    std::vector<chipcae::mesh::BodyLevelSet> entries{first, second};
    std::reverse(entries.begin(), entries.end());
    entries.erase(entries.begin() + 1);
    if (first.layer_index.has_value() == false || second.layer_index == first.layer_index ||
        entries.front().layer_index != second.layer_index || entries.front().body != second.body ||
        chipcae::mesh::BodyLevelSet{}.layer_index) {
        std::cerr << "FAIL: source layer identity must distinguish duplicate body IDs\n";
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
