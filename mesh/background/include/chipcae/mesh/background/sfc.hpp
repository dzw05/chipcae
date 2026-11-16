#pragma once
#include <cstdint>
namespace chipcae::mesh {
// Two-dimensional column codes. No dependency, fixed 31-bit coordinates.
inline std::uint64_t morton(std::uint32_t x, std::uint32_t y) {
    std::uint64_t code = 0;
    for (unsigned b = 0; b < 31; ++b) {
        code |= std::uint64_t((x >> b) & 1) << (2 * b);
        code |= std::uint64_t((y >> b) & 1) << (2 * b + 1);
    }
    return code;
}
inline std::uint64_t hilbert(std::uint32_t x, std::uint32_t y, unsigned bits = 16) {
    std::uint64_t code = 0;
    for (std::uint32_t s = std::uint32_t(1) << (bits - 1); s; s >>= 1) {
        auto rx = (x & s) != 0, ry = (y & s) != 0;
        code += std::uint64_t(s) * s * ((3 * unsigned(rx)) ^ unsigned(ry));
        if (!ry) {
            if (rx) {
                x = 2 * s - 1 - x;
                y = 2 * s - 1 - y;
            }
            auto t = x;
            x = y;
            y = t;
        }
    }
    return code;
}
} // namespace chipcae::mesh
