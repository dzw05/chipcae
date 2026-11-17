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
static_assert(std::is_same_v<decltype(chipcae::mesh::BodyLevelSet::body), chipcae::BodyId>);
static_assert(
    std::is_same_v<decltype(chipcae::mesh::BodyLevelSet::phi), std::vector<chipcae::Real>>);

static_assert(chipcae::mesh::RegionLSOptions{}.inside_tol_rel == 1e-12);

static_assert(std::is_aggregate_v<chipcae::mesh::BodyLevelSet>);
static_assert(std::is_same_v<decltype(chipcae::mesh::BodyLevelSet{chipcae::BodyId{},
                                                                  std::vector<chipcae::Real>{}}),
                             chipcae::mesh::BodyLevelSet>);

[[maybe_unused]] static void check_two_members(chipcae::mesh::BodyLevelSet& x) {
    auto& [b, p] = x;
    // decltype on an unparenthesized structured binding yields its referenced member type.
    static_assert(std::is_same_v<decltype(b), chipcae::BodyId>);
    static_assert(std::is_same_v<decltype(p), std::vector<chipcae::Real>>);
}

int main() {
    int failures = 0;
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
