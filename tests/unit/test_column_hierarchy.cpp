#include "chipcae/mesh/background/column_hierarchy.hpp"
#include "chipcae/mesh/background/sfc.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

int main() {
    using namespace chipcae::mesh;
    constexpr auto max_coordinate = std::numeric_limits<std::uint32_t>::max();
    if (morton2d(0, 0) != morton(0, 0) ||
        morton2d(max_coordinate, max_coordinate) != morton(max_coordinate, max_coordinate)) {
        std::cerr << "morton2d edge mismatch\n";
        return EXIT_FAILURE;
    }
    std::mt19937 random(0xC01A4);
    std::uniform_int_distribution<std::uint32_t> coordinate(0, max_coordinate);
    for (int n = 0; n < 10000; ++n) {
        const auto i = coordinate(random);
        const auto j = coordinate(random);
        if (morton2d(i, j) != morton(i, j)) {
            std::cerr << "morton2d mismatch\n";
            return EXIT_FAILURE;
        }
    }
    constexpr std::array<std::uint32_t, 6> sizes{1, 2, 3, 5, 7, 8};
    for (const auto nx0 : sizes) {
        for (const auto ny0 : sizes) {
            for (std::uint32_t level = 0; level <= 4; ++level) {
                const auto nx = nx0 << level;
                const auto ny = ny0 << level;
                for (std::uint32_t i = 0; i <= nx; ++i) {
                    if (finest_axis_index(i, level, 4) != (i << (4 - level))) {
                        std::cerr << "axis subset mapping mismatch\n";
                        return EXIT_FAILURE;
                    }
                }
                std::vector<std::uint64_t> order(std::uint64_t{nx} * ny);
                std::iota(order.begin(), order.end(), 0);
                std::sort(order.begin(), order.end(), [=](auto a, auto b) {
                    return morton(static_cast<std::uint32_t>(a % nx),
                                  static_cast<std::uint32_t>(a / nx)) <
                           morton(static_cast<std::uint32_t>(b % nx),
                                  static_cast<std::uint32_t>(b / nx));
                });
                std::vector<std::uint64_t> expected(order.size());
                for (std::uint64_t d = 0; d < order.size(); ++d)
                    expected[order[d]] = d;
                for (std::uint32_t j = 0; j < ny; ++j) {
                    for (std::uint32_t i = 0; i < nx; ++i) {
                        const auto dense = dense_index(nx0, ny0, level, i, j);
                        if (dense != expected[std::uint64_t{j} * nx + i]) {
                            std::cerr << "dense Morton mismatch nx0=" << nx0 << " ny0=" << ny0
                                      << " level=" << level << " i=" << i << " j=" << j
                                      << " actual=" << dense
                                      << " expected=" << expected[std::uint64_t{j} * nx + i]
                                      << "\n";
                            return EXIT_FAILURE;
                        }
                        if (level > 0 &&
                            parent(dense) != dense_index(nx0, ny0, level - 1, i >> 1, j >> 1)) {
                            std::cerr << "parent mismatch\n";
                            return EXIT_FAILURE;
                        }
                        if (level < 4) {
                            const auto child = children(dense);
                            for (std::uint32_t b = 0; b < 4; ++b) {
                                const auto ci = child_coordinate(i, b & 1);
                                const auto cj = child_coordinate(j, b >> 1);
                                if (child.d[b] != dense_index(nx0, ny0, level + 1, ci, cj)) {
                                    std::cerr << "child mismatch\n";
                                    return EXIT_FAILURE;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return EXIT_SUCCESS;
}
