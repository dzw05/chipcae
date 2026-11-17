#include "chipcae/geometry/assembly.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

using PlaceResult =
    decltype(chipcae::geometry::place(std::declval<std::vector<chipcae::geometry::Slab>>(),
                                      std::declval<const chipcae::geometry::Placement&>(),
                                      std::declval<const chipcae::geometry::StackModel&>()));
using MergeResult = decltype(chipcae::geometry::merge_parts(
    std::declval<chipcae::mesh::StackModel>(),
    std::declval<const std::vector<chipcae::geometry::PlacedPart>&>(),
    std::declval<const chipcae::geometry::MergeOptions&>()));
static_assert(
    std::is_same_v<PlaceResult, chipcae::geometry::Expected<chipcae::geometry::PlacedPart>>);
static_assert(
    std::is_same_v<MergeResult, chipcae::geometry::Expected<chipcae::geometry::MergeResult>>);
static_assert(std::is_same_v<decltype(std::declval<const PlaceResult&>().error()),
                             const chipcae::geometry::ImportThresholdError&>);
static_assert(std::is_same_v<decltype(std::declval<const MergeResult&>().error()),
                             const chipcae::geometry::ImportThresholdError&>);

int main() {
    int failures = 0;
    try {
        (void)chipcae::geometry::place({}, {}, {});
        std::cerr << "FAIL: (void)chipcae::geometry::place({}, {}, {}); must throw "
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
    try {
        (void)chipcae::geometry::merge_parts({}, {}, {});
        std::cerr << "FAIL: (void)chipcae::geometry::merge_parts({}, {}, {}); must throw "
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
