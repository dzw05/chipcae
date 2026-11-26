#include "chipcae/io/io_stats_export.hpp"

namespace chipcae::io {
// Deliberately empty linkable stubs for the acceptance-test-first commit.
IoStatsReduction reduce_io_stats(const std::vector<ResultsWriter::IoStats>&) {
    return {};
}
std::string format_io_stats_json(const IoStatsExport&) {
    return {};
}
IoStatsWriteResult write_io_stats_json(const std::filesystem::path&,
                                       const ResultsWriter::IoStats&) noexcept {
    return {};
}
} // namespace chipcae::io
