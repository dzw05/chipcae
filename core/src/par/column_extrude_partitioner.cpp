#include "chipcae/par/column_extrude_partitioner.hpp"

#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>
#ifdef CHIPCAE_HAVE_METIS
#include <metis.h>
#endif
namespace chipcae::par {
bool ColumnExtrudePartitioner::uses_metis() noexcept {
#ifdef CHIPCAE_HAVE_METIS
    return true;
#else
    return false;
#endif
}
View1<std::int32_t, HostSpace>
ColumnExtrudePartitioner::partition(std::size_t nx,
                                    std::size_t ny,
                                    std::size_t nz,
                                    int parts,
                                    std::span<const std::int32_t> column_weights) {
    if (!nx || !ny || !nz || parts < 1 || nx > std::numeric_limits<std::size_t>::max() / ny)
        throw std::invalid_argument("invalid column grid");
    const auto columns = nx * ny;
    if (nz > std::numeric_limits<std::size_t>::max() / columns ||
        static_cast<std::size_t>(parts) > columns)
        throw std::invalid_argument("invalid column partition count or size");
    if (!column_weights.empty() && column_weights.size() != columns)
        throw std::invalid_argument("column weights must match nx*ny");
    for (auto weight : column_weights)
        if (weight <= 0)
            throw std::invalid_argument("column weights must be positive");
    std::vector<std::size_t> offsets{0}, neighbors;
    for (std::size_t j = 0; j < ny; ++j)
        for (std::size_t i = 0; i < nx; ++i) {
            const auto c = j * nx + i;
            if (i)
                neighbors.push_back(c - 1);
            if (i + 1 < nx)
                neighbors.push_back(c + 1);
            if (j)
                neighbors.push_back(c - nx);
            if (j + 1 < ny)
                neighbors.push_back(c + nx);
            offsets.push_back(neighbors.size());
        }
    std::vector<std::int32_t> labels(columns);
#ifdef CHIPCAE_HAVE_METIS
    if (columns > static_cast<std::size_t>(std::numeric_limits<idx_t>::max()) ||
        neighbors.size() > static_cast<std::size_t>(std::numeric_limits<idx_t>::max()) ||
        nz > static_cast<std::size_t>(std::numeric_limits<idx_t>::max()) / columns)
        throw std::overflow_error("METIS graph size");
    std::vector<idx_t> xadj(offsets.begin(), offsets.end()),
        adj(neighbors.begin(), neighbors.end());
    std::vector<idx_t> weights(columns, static_cast<idx_t>(nz)), result(columns);
    if (!column_weights.empty())
        weights.assign(column_weights.begin(), column_weights.end());
    idx_t n = static_cast<idx_t>(columns), constraints = 1, np = parts, cut = 0;
    idx_t options[METIS_NOPTIONS];
    METIS_SetDefaultOptions(options);
    options[METIS_OPTION_SEED] = 0;
    options[METIS_OPTION_NUMBERING] = 0;
    if (parts > 1 && METIS_PartGraphKway(&n,
                                         &constraints,
                                         xadj.data(),
                                         adj.data(),
                                         weights.data(),
                                         nullptr,
                                         nullptr,
                                         &np,
                                         nullptr,
                                         nullptr,
                                         options,
                                         &cut,
                                         result.data()) != METIS_OK)
        throw std::runtime_error("METIS partition failed");
    for (std::size_t c = 0; c < columns; ++c)
        labels[c] = static_cast<std::int32_t>(result[c]);
#else
    // Weighted serpentine splitting is not supported; callers must use METIS.
    if (!column_weights.empty())
        throw std::invalid_argument("column weights require METIS");
    static std::once_flag warning;
    std::call_once(warning, [] {
        std::cerr << "warning: METIS disabled; using serpentine column partition fallback\n";
    });
    // Deterministic serpentine graph walk. Equal nz weights allow balanced
    // contiguous chunks with at most one column difference between parts.
    const auto base = columns / static_cast<std::size_t>(parts),
               extra = columns % static_cast<std::size_t>(parts);
    std::size_t walk = 0;
    for (int p = 0; p < parts; ++p) {
        const auto count = base + (static_cast<std::size_t>(p) < extra ? 1u : 0u);
        for (std::size_t q = 0; q < count; ++q, ++walk) {
            const auto j = walk / nx, x = walk % nx;
            labels[j * nx + (j % 2 ? nx - 1 - x : x)] = p;
        }
    }
#endif
    View1<std::int32_t, HostSpace> output("column partition", columns * nz);
    for (std::size_t k = 0; k < nz; ++k)
        for (std::size_t c = 0; c < columns; ++c)
            output(k * columns + c) = labels[c];
    return output;
}
} // namespace chipcae::par
