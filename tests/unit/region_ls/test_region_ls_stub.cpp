#include "chipcae/mesh/cut/region_level_set.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

static_assert(std::is_same_v<decltype(chipcae::mesh::region_level_sets({}, {}, {})),
                             decltype(chipcae::mesh::cut::CutLayerArrays{}.level_sets)>);

static_assert(chipcae::mesh::RegionLSOptions{}.inside_tol_rel == 1e-12);

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
