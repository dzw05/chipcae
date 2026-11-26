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
// Inputs are in rank order. Empty input and invalid durations throw std::invalid_argument.
IoStatsReduction reduce_io_stats(const std::vector<ResultsWriter::IoStats>& per_rank);
// Returns the complete I20 JSON bytes, including the final newline.
// Requires ranks >= 1, write_step_count >= 0, and finite nonnegative durations (no negative zero).
// When write_step_count > 0, last_write_step_seconds must be <= write_step_seconds + 1e-12;
// at count 0, no relationship between the durations is required. Byte counts accept all uint64
// values. Violations throw std::invalid_argument.
std::string format_io_stats_json(const IoStatsExport& value);
struct IoStatsWriteResult {
    bool written = false;
    std::vector<std::string> warnings;
    std::string error;
};
// Collective on initialized multi-rank MPI_COMM_WORLD; every rank must call in the same order.
// Only rank 0 publishes run_dir/io_stats.json via tmp + rename and returns written=true.
// Warnings are root-only. On root failure, other ranks report
// "io_stats.json export failed on rank 0"; returned MPI errors are reported locally.
IoStatsWriteResult write_io_stats_json(const std::filesystem::path& run_dir,
                                       const ResultsWriter::IoStats& local) noexcept;
} // namespace chipcae::io
