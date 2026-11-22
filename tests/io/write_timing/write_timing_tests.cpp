#include "chipcae/io/detail/hdf5.hpp"
#include "chipcae/io/vtkhdf_results.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using chipcae::hdf_detail::check;
using chipcae::hdf_detail::H5;
using chipcae::io::ResultsStep;
using chipcae::io::ResultsWriter;
using Stats = ResultsWriter::IoStats;

static_assert(
    std::is_same_v<decltype(std::declval<const ResultsWriter&>().io_stats()), const Stats&>);
static_assert(noexcept(std::declval<const ResultsWriter&>().io_stats()));
static_assert(std::is_same_v<decltype(Stats::write_step_seconds), double>);
static_assert(std::is_same_v<decltype(Stats::last_write_step_seconds), double>);
static_assert(std::is_same_v<decltype(Stats::finalize_seconds), double>);
static_assert(std::is_same_v<decltype(Stats::peak_rss_bytes), std::uint64_t>);
static_assert(std::is_same_v<decltype(Stats::bytes_written), std::uint64_t>);
static_assert(std::is_same_v<decltype(Stats::write_step_count), int>);

// Exceptions keep every assertion active in Release builds.
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct TemporaryRun {
    fs::path path;
    TemporaryRun() {
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = fs::temp_directory_path() / ("iowt-" + std::to_string(random()));
            if (fs::create_directory(candidate)) {
                path = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("cannot create unique temporary run directory");
    }
    ~TemporaryRun() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

ResultsStep cube() {
    ResultsStep step;
    auto& p = step.hex;
    p.points = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1};
    p.hex = {0, 1, 2, 3, 4, 5, 6, 7};
    p.T.assign(8, 193.15);
    p.U.assign(24, 0);
    p.layer_id = {0};
    p.material_id = {0};
    p.cell_kind = {0};
    p.elem_gid = {0};
    p.volume = {1};
    return step;
}

void set_mesh(ResultsWriter& writer) {
    writer.set_mesh({1, 1, 0, 1, 1, std::string(64, '0'), 0});
}

void write_step(ResultsWriter& writer, int n) {
    writer.write_step(n, static_cast<double>(n), cube());
    writer.add_summary("warpage_pv", 0, "m", n);
    writer.add_summary("T_max", 193.15, "K", n);
    writer.add_summary("U_mag_max", 0, "m", n);
}

void zero_stats(const Stats& s) {
    require(s.write_step_count == 0, "fresh write_step_count must be zero");
    require(s.write_step_seconds == 0, "fresh write_step_seconds must be zero");
    require(s.last_write_step_seconds == 0, "fresh last_write_step_seconds must be zero");
    require(s.finalize_seconds == 0, "fresh finalize_seconds must be zero");
    require(s.peak_rss_bytes == 0, "fresh peak_rss_bytes must be zero");
    require(s.bytes_written == 0, "fresh bytes_written must be zero");
}

void finite_time(double value, const std::string& name) {
    require(std::isfinite(value) && value >= 0, name + " must be finite and nonnegative");
}

void finalized(const Stats& s) {
    finite_time(s.write_step_seconds, "write_step_seconds");
    finite_time(s.last_write_step_seconds, "last_write_step_seconds");
    finite_time(s.finalize_seconds, "finalize_seconds");
    // This is the whole process peak RSS, not an incremental I/O allocation measurement.
    require(s.peak_rss_bytes > 0, "peak_rss_bytes must be positive after finalize");
}

// Catch eager RSS/manifest accounting before any finalize, or lost internal-finalize stats.
void before_finalize(const fs::path& stackup) {
    TemporaryRun without_mesh;
    ResultsWriter meshless(without_mesh.path, stackup);
    zero_stats(meshless.io_stats());
    write_step(meshless, 0);
    const auto& pending = meshless.io_stats();
    require(pending.write_step_count == 1, "meshless write_step_count must be 1");
    finite_time(pending.write_step_seconds, "meshless write_step_seconds");
    finite_time(pending.last_write_step_seconds, "meshless last_write_step_seconds");
    require(pending.peak_rss_bytes == 0 && pending.finalize_seconds == 0,
            "RSS and finalize time must stay zero before any finalize call");
    require(!fs::exists(without_mesh.path / "manifest.json"),
            "write_step without set_mesh must not publish a manifest");
    require(pending.bytes_written == fs::file_size(without_mesh.path / "out/step-0.vtkhdf"),
            "before any finalize, bytes_written must count only step files");
    meshless.finalize();
    finalized(pending);
    require(pending.bytes_written == fs::file_size(without_mesh.path / "out/step-0.vtkhdf") +
                                         fs::file_size(without_mesh.path / "manifest.json"),
            "first explicit finalize must add manifest bytes");

    TemporaryRun run;
    ResultsWriter writer(run.path, stackup);
    const auto& s = writer.io_stats();
    zero_stats(s);
    set_mesh(writer);
    // set_mesh publishes the manifest by calling finalize, before the caller calls it.
    finalized(s);
    require(s.write_step_count == 0 && s.write_step_seconds == 0 && s.last_write_step_seconds == 0,
            "set_mesh must not fabricate step statistics");
    const auto manifest_bytes = fs::file_size(run.path / "manifest.json");
    require(s.bytes_written == manifest_bytes, "set_mesh must count its published manifest");
    const auto rss = s.peak_rss_bytes;
    const auto finalize_time = s.finalize_seconds;
    write_step(writer, 0);
    require(s.write_step_count == 1, "write_step after set_mesh must be counted");
    require(s.peak_rss_bytes == rss && s.finalize_seconds == finalize_time,
            "write_step must preserve the internal finalize's RSS and time samples");
    require(s.bytes_written == fs::file_size(run.path / "out/step-0.vtkhdf") + manifest_bytes,
            "before explicit finalize, bytes_written must include the published manifest");
    require(fs::file_size(run.path / "manifest.json") == manifest_bytes,
            "write_step must preserve the previously published manifest size");
    writer.finalize();
    finalized(s);
    require(s.bytes_written == fs::file_size(run.path / "out/step-0.vtkhdf") +
                                   fs::file_size(run.path / "manifest.json"),
            "explicit finalize must replace the old manifest byte contribution");
}

// Catch absent accounting, invalid timings, and accidental inclusion of stackup/tmp files.
void stats_after_write(const fs::path& stackup) {
    TemporaryRun run;
    ResultsWriter writer(run.path, stackup);
    zero_stats(Stats{});
    const auto& s = writer.io_stats();
    zero_stats(s);
    require(&s == &writer.io_stats(), "io_stats must return the same writer-owned object");
    set_mesh(writer);
    write_step(writer, 0);
    writer.finalize();
    require(s.write_step_count == 1, "write_step_count must be 1 after one successful write_step");
    finalized(s);
    require(s.write_step_seconds >= s.last_write_step_seconds, "total time must include last step");
    require(s.bytes_written == fs::file_size(run.path / "out/step-0.vtkhdf") +
                                   fs::file_size(run.path / "manifest.json"),
            "bytes_written must equal step plus manifest file sizes");
}

// Catch replacement instead of accumulation, and double-counting a republished manifest.
void two_steps(const fs::path& stackup) {
    TemporaryRun run;
    ResultsWriter writer(run.path, stackup);
    set_mesh(writer);
    write_step(writer, 0);
    const double first = writer.io_stats().last_write_step_seconds;
    require(writer.io_stats().write_step_count == 1, "first write_step_count must be 1");
    finite_time(first, "first step duration");
    require(writer.io_stats().write_step_seconds == first, "first total must equal first duration");
    write_step(writer, 1);
    const auto& s = writer.io_stats();
    require(s.write_step_count == 2, "write_step_count must be 2 after two successful calls");
    finite_time(s.last_write_step_seconds, "second step duration");
    // Exact double accumulation is explicitly required by the frozen contract.
    require(s.write_step_seconds == first + s.last_write_step_seconds,
            "total must exactly equal first duration plus second last duration");
    const double total = s.write_step_seconds;
    const double last = s.last_write_step_seconds;
    writer.finalize();
    finalized(s);
    require(s.write_step_count == 2 && s.write_step_seconds == total &&
                s.last_write_step_seconds == last,
            "finalize must preserve step statistics");
    const auto bytes = fs::file_size(run.path / "out/step-0.vtkhdf") +
                       fs::file_size(run.path / "out/step-1.vtkhdf") +
                       fs::file_size(run.path / "manifest.json");
    require(s.bytes_written == bytes, "bytes_written must include both steps and current manifest");
    for (int call = 0; call < 16; ++call) {
        const auto start = std::chrono::steady_clock::now();
        writer.finalize();
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        finalized(s);
        // A single call's internal interval fits within its caller's interval, even at zero ticks.
        require(s.finalize_seconds <= elapsed,
                "finalize_seconds must measure only the most recent finalize call");
        require(s.bytes_written == bytes, "repeated finalize must not double-count manifest bytes");
        require(s.write_step_count == 2 && s.write_step_seconds == total &&
                    s.last_write_step_seconds == last,
                "repeated finalize must preserve step statistics");
    }
}

// Catch fabricated step samples or accounting of the constructor's stackup copy.
void finalize_only(const fs::path& stackup) {
    TemporaryRun run;
    ResultsWriter writer(run.path, stackup);
    set_mesh(writer);
    writer.finalize();
    const auto& s = writer.io_stats();
    require(s.write_step_count == 0, "finalize-only write_step_count must be zero");
    require(s.write_step_seconds == 0 && s.last_write_step_seconds == 0,
            "finalize-only step timings must remain exactly zero");
    finalized(s);
    require(s.bytes_written == fs::file_size(run.path / "manifest.json"),
            "finalize-only bytes_written must equal manifest size");
    std::set<std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(run.path))
        if (entry.is_regular_file())
            files.insert(fs::relative(entry.path(), run.path).generic_string());
    require(files == std::set<std::string>{"stackup.json", "manifest.json"},
            "finalize-only file inventory changed");
    require(fs::is_directory(run.path / "out") && fs::is_empty(run.path / "out"),
            "finalize-only out directory must exist and be empty");
}

void generate(const fs::path& stackup, const fs::path& run) {
    ResultsWriter writer(run, stackup);
    set_mesh(writer);
    write_step(writer, 0);
    write_step(writer, 1);
    writer.finalize();
}

// Inspect every object, including root attributes and attributes on datasets.
void inspect_object(hid_t object, const std::vector<std::string>& forbidden) {
    const auto reject = [&](const std::string& name) {
        require(std::find(forbidden.begin(), forbidden.end(), name) == forbidden.end(),
                "forbidden HDF5 attribute or dataset: " + name);
    };
    const int attributes = H5Aget_num_attrs(object);
    require(attributes >= 0, "cannot enumerate HDF5 attributes");
    for (int i = 0; i < attributes; ++i) {
        H5 attribute(H5Aopen_by_idx(object,
                                    ".",
                                    H5_INDEX_NAME,
                                    H5_ITER_INC,
                                    static_cast<hsize_t>(i),
                                    H5P_DEFAULT,
                                    H5P_DEFAULT),
                     H5Aclose);
        const auto length = H5Aget_name(attribute, 0, nullptr);
        require(length >= 0, "cannot read HDF5 attribute name length");
        std::vector<char> name(static_cast<std::size_t>(length) + 1);
        require(H5Aget_name(attribute, name.size(), name.data()) >= 0,
                "cannot read HDF5 attribute name");
        reject(name.data());
    }
    if (H5Iget_type(object) != H5I_GROUP)
        return;
    H5G_info_t info{};
    check(H5Gget_info(object, &info));
    for (hsize_t i = 0; i < info.nlinks; ++i) {
        const auto length =
            H5Lget_name_by_idx(object, ".", H5_INDEX_NAME, H5_ITER_INC, i, nullptr, 0, H5P_DEFAULT);
        require(length >= 0, "cannot read HDF5 link name length");
        std::vector<char> name(static_cast<std::size_t>(length) + 1);
        require(H5Lget_name_by_idx(object,
                                   ".",
                                   H5_INDEX_NAME,
                                   H5_ITER_INC,
                                   i,
                                   name.data(),
                                   name.size(),
                                   H5P_DEFAULT) >= 0,
                "cannot read HDF5 link name");
        H5 child(H5Oopen(object, name.data(), H5P_DEFAULT), H5Oclose);
        if (H5Iget_type(child) == H5I_DATASET)
            reject(name.data());
        inspect_object(child, forbidden);
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 3, "usage: write_timing_tests MODE STACKUP [RUN] | inspect FILE NAMES...");
        const std::string mode = argv[1];
        if (mode == "stats_after_write")
            stats_after_write(argv[2]);
        else if (mode == "before_finalize")
            before_finalize(argv[2]);
        else if (mode == "two_steps")
            two_steps(argv[2]);
        else if (mode == "finalize_only")
            finalize_only(argv[2]);
        else if (mode == "generate") {
            require(argc == 4, "generate requires a run directory");
            generate(argv[2], argv[3]);
        } else if (mode == "inspect") {
            require(argc > 3, "inspect requires forbidden names");
            H5 file(H5Fopen(argv[2], H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
            H5 root(H5Gopen2(file, "/", H5P_DEFAULT), H5Gclose);
            inspect_object(root, std::vector<std::string>(argv + 3, argv + argc));
        } else
            throw std::runtime_error("unknown mode: " + mode);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
