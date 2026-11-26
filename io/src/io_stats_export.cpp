#include "chipcae/io/io_stats_export.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#ifdef CHIPCAE_ENABLE_MPI
#include <mpi.h>
#endif

namespace chipcae::io {
namespace {
void validate_duration(double value) {
    if (!std::isfinite(value) || std::signbit(value))
        throw std::invalid_argument("io_stats.json duration must be finite and nonnegative");
}

std::string duration_token(double value, std::ostringstream& output, std::istringstream& input) {
    validate_duration(value);
    output.str("");
    output.clear();
    output << std::setprecision(15) << value;
    std::string text = output.str();
    input.str(text);
    input.clear();
    double parsed = 0;
    input >> parsed;
    if (input.fail() ||
        std::bit_cast<std::uint64_t>(value) != std::bit_cast<std::uint64_t>(parsed)) {
        output.str("");
        output.clear();
        output << std::setprecision(17) << value;
        text = output.str();
    }
    if (text.find_first_of(".eE") == std::string::npos)
        text += ".0";
    return text;
}

// Reporting is best effort if even allocating the error string fails.
void set_error(IoStatsWriteResult& result, const char* message) noexcept {
    try {
        result.error = message;
    } catch (...) {
    }
}

void publish(const std::filesystem::path& run_dir,
             const IoStatsReduction& reduction,
             IoStatsWriteResult& result) {
    if (reduction.write_step_count_mismatch)
        result.warnings.emplace_back(
            "io_stats.json write_step_count differs across ranks; using rank 0");
    if (reduction.bytes_written_mismatch)
        result.warnings.emplace_back(
            "io_stats.json bytes_written differs across ranks; using maximum");
    const auto bytes = format_io_stats_json(reduction.value);
    const auto temporary = run_dir / "io_stats.json.tmp";
    const auto destination = run_dir / "io_stats.json";
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            throw std::runtime_error("io_stats.json: cannot open temporary file");
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output)
            throw std::runtime_error("io_stats.json: cannot write temporary file");
        output.close();
        if (!output)
            throw std::runtime_error("io_stats.json: cannot close temporary file");
        std::error_code error;
        std::filesystem::rename(temporary, destination, error);
        if (error)
            throw std::runtime_error("io_stats.json: cannot publish temporary file: " +
                                     error.message());
        result.written = true;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

#ifdef CHIPCAE_ENABLE_MPI
void check_mpi(int code, const char* operation) {
    if (code != MPI_SUCCESS)
        throw std::runtime_error(operation);
}

// MPI types describe the fields, not struct padding or a byte serialization.
struct WireStats {
    double durations[3];
    int count;
    std::uint64_t bytes[2];
};
struct WireType {
    MPI_Datatype type = MPI_DATATYPE_NULL;
    ~WireType() {
        if (type != MPI_DATATYPE_NULL)
            MPI_Type_free(&type);
    }
    void initialize() {
        const int lengths[] = {3, 1, 2};
        const MPI_Aint offsets[] = {
            offsetof(WireStats, durations), offsetof(WireStats, count), offsetof(WireStats, bytes)};
        const MPI_Datatype types[] = {MPI_DOUBLE, MPI_INT, MPI_UINT64_T};
        WireType fields;
        check_mpi(MPI_Type_create_struct(3, lengths, offsets, types, &fields.type),
                  "io_stats.json: MPI wire type creation failed");
        check_mpi(MPI_Type_create_resized(fields.type, 0, sizeof(WireStats), &type),
                  "io_stats.json: MPI wire type resize failed");
        check_mpi(MPI_Type_commit(&type), "io_stats.json: MPI wire type commit failed");
    }
};
#endif
} // namespace

IoStatsReduction reduce_io_stats(const std::vector<ResultsWriter::IoStats>& per_rank) {
    if (per_rank.empty())
        throw std::invalid_argument("io_stats.json reduction requires at least one rank");
    if (per_rank.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("io_stats.json rank count exceeds int range");
    for (const auto& stats : per_rank) {
        validate_duration(stats.write_step_seconds);
        validate_duration(stats.finalize_seconds);
        validate_duration(stats.last_write_step_seconds);
    }
    const auto& first = per_rank.front();
    IoStatsReduction result;
    result.value = {static_cast<int>(per_rank.size()),
                    first.write_step_seconds,
                    first.finalize_seconds,
                    first.last_write_step_seconds,
                    first.write_step_count,
                    first.bytes_written,
                    first.peak_rss_bytes};
    for (const auto& stats : per_rank) {
        auto& value = result.value;
        value.write_step_seconds = std::max(value.write_step_seconds, stats.write_step_seconds);
        value.finalize_seconds = std::max(value.finalize_seconds, stats.finalize_seconds);
        value.last_write_step_seconds =
            std::max(value.last_write_step_seconds, stats.last_write_step_seconds);
        value.peak_rss_bytes = std::max(value.peak_rss_bytes, stats.peak_rss_bytes);
        value.bytes_written = std::max(value.bytes_written, stats.bytes_written);
        result.bytes_written_mismatch |= stats.bytes_written != first.bytes_written;
        result.write_step_count_mismatch |= stats.write_step_count != first.write_step_count;
    }
    return result;
}

std::string format_io_stats_json(const IoStatsExport& value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    std::istringstream input;
    input.imbue(std::locale::classic());
    const auto write_step = duration_token(value.write_step_seconds, output, input);
    const auto finalize = duration_token(value.finalize_seconds, output, input);
    const auto last_write_step = duration_token(value.last_write_step_seconds, output, input);
    output.str("");
    output.clear();
    output << "{\"schema\": \"chipcae.io_stats/1.0\", \"ranks\": " << value.ranks
           << ", \"write_step_seconds\": " << write_step << ", \"finalize_seconds\": " << finalize
           << ", \"last_write_step_seconds\": " << last_write_step
           << ", \"write_step_count\": " << value.write_step_count
           << ", \"bytes_written\": " << value.bytes_written
           << ", \"peak_rss_bytes\": " << value.peak_rss_bytes << "}\n";
    return std::move(output).str();
}

IoStatsWriteResult write_io_stats_json(const std::filesystem::path& run_dir,
                                       const ResultsWriter::IoStats& local) noexcept {
    IoStatsWriteResult result;
    try {
#ifdef CHIPCAE_ENABLE_MPI
        int initialized = 0;
        check_mpi(MPI_Initialized(&initialized), "io_stats.json: MPI initialization query failed");
        int rank = 0, size = 1;
        if (initialized) {
            check_mpi(MPI_Comm_rank(MPI_COMM_WORLD, &rank), "io_stats.json: MPI rank query failed");
            check_mpi(MPI_Comm_size(MPI_COMM_WORLD, &size), "io_stats.json: MPI size query failed");
        }
        if (size > 1) {
            WireType type;
            std::vector<WireStats> received;
            int ready = 1;
            try {
                type.initialize();
                if (rank == 0)
                    received.resize(static_cast<std::size_t>(size));
            } catch (const std::exception& error) {
                ready = 0;
                set_error(result, error.what());
            } catch (...) {
                ready = 0;
                set_error(result, "io_stats.json: MPI preparation failed");
            }
            // Preparation can allocate on root. Agree before entering Gather so a
            // failed allocation/type setup cannot strand the other ranks there.
            int all_ready = 0;
            check_mpi(MPI_Allreduce(&ready, &all_ready, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD),
                      "io_stats.json: MPI preparation agreement failed");
            if (all_ready) {
                const WireStats send{{local.write_step_seconds,
                                      local.finalize_seconds,
                                      local.last_write_step_seconds},
                                     local.write_step_count,
                                     {local.bytes_written, local.peak_rss_bytes}};
                check_mpi(
                    MPI_Gather(
                        &send, 1, type.type, received.data(), 1, type.type, 0, MPI_COMM_WORLD),
                    "io_stats.json: MPI gather failed");
                if (rank == 0) {
                    try {
                        std::vector<ResultsWriter::IoStats> inputs;
                        inputs.reserve(received.size());
                        for (const auto& wire : received) {
                            ResultsWriter::IoStats stats;
                            stats.write_step_seconds = wire.durations[0];
                            stats.finalize_seconds = wire.durations[1];
                            stats.last_write_step_seconds = wire.durations[2];
                            stats.write_step_count = wire.count;
                            stats.bytes_written = wire.bytes[0];
                            stats.peak_rss_bytes = wire.bytes[1];
                            inputs.push_back(stats);
                        }
                        publish(run_dir, reduce_io_stats(inputs), result);
                    } catch (const std::exception& error) {
                        set_error(result, error.what());
                    } catch (...) {
                        set_error(result, "io_stats.json: unexpected export failure");
                    }
                }
            } else if (rank == 0 && result.error.empty()) {
                set_error(result, "io_stats.json: MPI preparation failed on another rank");
            }
            int success = rank == 0 && result.written ? 1 : 0;
            check_mpi(MPI_Bcast(&success, 1, MPI_INT, 0, MPI_COMM_WORLD),
                      "io_stats.json: MPI status broadcast failed");
            if (rank != 0 && !success && result.error.empty())
                set_error(result, "io_stats.json export failed on rank 0");
            return result;
        }
#endif
        publish(run_dir, reduce_io_stats({local}), result);
    } catch (const std::exception& error) {
        result.written = false;
        set_error(result, error.what());
    } catch (...) {
        result.written = false;
        set_error(result, "io_stats.json: unexpected export failure");
    }
    return result;
}
} // namespace chipcae::io
