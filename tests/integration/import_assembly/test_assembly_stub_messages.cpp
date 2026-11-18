#include "chipcae/geometry/assembly.hpp"
#include "chipcae/geometry/step_import.hpp"

#include <stdexcept>
#include <string>

int main() {
    using namespace chipcae::geometry;
    auto check = [](auto call, const char* expected) {
        try {
            call();
        } catch (const std::logic_error& error) {
            return std::string(error.what()) == expected;
        }
        return false;
    };
    return check([] { (void)slice_prismatic({}, "", {}); }, "not implemented: slice_prismatic") &&
                   check([] { (void)place({}, FramePlacement{}, {}); }, "not implemented: place") &&
                   check([] { (void)merge_parts({}, {}, {}); }, "not implemented: merge_parts")
               ? 0
               : 1;
}
