#pragma once
#include "chipcae/io/vtkhdf_results.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace chipcae::io {
struct IoStatsExport {
    int ranks = 1;
    double write_step_seconds = 0;
    double finalize_seconds = 0;
    double last_write_step_seconds = 0;
    int write_step_count = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t peak_rss_bytes = 0;
};
struct IoStatsReduction {
    IoStatsExport value;
    bool write_step_count_mismatch = false;
    bool bytes_written_mismatch = false;
};
IoStatsReduction reduce_io_stats(const std::vector<ResultsWriter::IoStats>& per_rank);
std::string format_io_stats_json(const IoStatsExport& value);
struct IoStatsWriteResult {
    bool written = false;
    std::vector<std::string> warnings;
    std::string error;
};
IoStatsWriteResult write_io_stats_json(const std::filesystem::path& run_dir,
                                       const ResultsWriter::IoStats& local) noexcept;
} // namespace chipcae::io
