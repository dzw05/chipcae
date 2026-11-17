#include "chipcae/geometry/part_slice.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>

int main() {
    int failures = 0;
    try {
        (void)chipcae::geometry::slice_prismatic({}, "solid1", {});
        std::cerr
            << "FAIL: slice_prismatic must throw std::logic_error with prefix not implemented: \n";
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
