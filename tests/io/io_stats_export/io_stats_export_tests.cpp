#include "chipcae/io/io_stats_export.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <climits>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <locale>
#include <pwd.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <vector>
#ifdef CHIPCAE_ENABLE_MPI
#include <mpi.h>
#endif

namespace {
namespace fs = std::filesystem;
using namespace chipcae::io;
using Stats = ResultsWriter::IoStats;
using Clock = std::chrono::steady_clock;
static_assert(noexcept(write_io_stats_json(fs::path{}, Stats{})));
static_assert(std::is_same_v<decltype(IoStatsExport::bytes_written), std::uint64_t>);
static_assert(std::is_same_v<decltype(IoStatsExport::write_step_count), int>);

void require(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
bool same(double a, double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}
std::string read(const fs::path& file) {
    std::ifstream stream(file, std::ios::binary);
    require(stream.good(), "cannot read " + file.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void put(const fs::path& file, const std::string& text) {
    std::ofstream stream(file, std::ios::binary);
    stream << text;
    stream.close();
    require(!stream.fail(), "cannot write test setup " + file.string());
}
struct TemporaryRun {
    fs::path path;
    TemporaryRun() {
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = fs::temp_directory_path() / ("iose-" + std::to_string(random()));
            if (fs::create_directory(candidate)) {
                path = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("cannot create temporary directory");
    }
    ~TemporaryRun() {
        std::error_code ignored;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ignored);
        fs::remove_all(path, ignored);
    }
};
struct Suite {
    int failures = 0;
    int checks = 0;
    void run(const std::string& name, const std::function<void()>& test) {
        ++checks;
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
};
template <class F>
void invalid_argument(F&& call, const std::string& message) {
    try {
        call();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(message);
}
int child(const std::function<int()>& work) {
    std::cout.flush();
    std::cerr.flush();
    const pid_t pid = fork();
    require(pid >= 0, "fork failed");
    if (pid == 0) {
        int result = 1;
        try {
            result = work();
        } catch (const std::exception& error) {
            std::cerr << "child FAIL: " << error.what() << std::endl;
        }
        _exit(result);
    }
    int status = 0;
    require(waitpid(pid, &status, 0) == pid, "waitpid failed");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}
void checker(const fs::path& file = {}) {
    const auto script = (fs::path(IOSE_FIXTURE_DIR) / "check.py").string();
    require(child([&] {
                if (file.empty())
                    execl(IOSE_PYTHON, IOSE_PYTHON, script.c_str(), nullptr);
                else
                    execl(IOSE_PYTHON, IOSE_PYTHON, script.c_str(), file.c_str(), nullptr);
                return 127;
            }) == 0,
            "frozen checker must accept export");
}
void fixtures() {
    require(chipcae::mesh::sha256(read(fs::path(IOSE_FIXTURE_DIR) / "example_np2.json")) ==
                "038f7970ae8d3f2f2bc2a78eaf88dc13ef7551ea11bb7d0a9b7ee7174594915f",
            "example fixture SHA-256 differs from frozen digest");
    require(chipcae::mesh::sha256(read(fs::path(IOSE_FIXTURE_DIR) / "check.py")) ==
                "ddfc51ab349dd304cd716dba5b620dc44aa384accce2a86c9620423008e538a8",
            "checker fixture SHA-256 differs from frozen digest");
}
IoStatsExport example() {
    return {2, 0.412, 0.031, 0.205, 2, 1843200, 987654144};
}
Stats local_example() {
    Stats s;
    s.write_step_seconds = 0.412;
    s.finalize_seconds = 0.031;
    s.last_write_step_seconds = 0.205;
    s.write_step_count = 2;
    s.bytes_written = 1843200;
    s.peak_rss_bytes = 987654144;
    return s;
}
std::string token(const std::string& json, const std::string& key) {
    const auto marker = "\"" + key + "\": ";
    const auto begin = json.find(marker);
    require(begin != std::string::npos, "missing JSON key " + key);
    const auto start = begin + marker.size();
    return json.substr(start, json.find_first_of(",}", start) - start);
}
double duration(const std::string& json, const std::string& key) {
    const auto text = token(json, key);
    char* end = nullptr;
    const auto value = std::strtod(text.c_str(), &end);
    require(end == text.c_str() + text.size(), "invalid double token for " + key);
    return value;
}
constexpr std::array<double IoStatsExport::*, 3> export_times = {
    &IoStatsExport::write_step_seconds,
    &IoStatsExport::finalize_seconds,
    &IoStatsExport::last_write_step_seconds};
constexpr std::array<double Stats::*, 3> local_times = {
    &Stats::write_step_seconds, &Stats::finalize_seconds, &Stats::last_write_step_seconds};
constexpr std::array<const char*, 3> time_names = {
    "write_step_seconds", "finalize_seconds", "last_write_step_seconds"};
void expect_export(const IoStatsExport& value, const IoStatsExport& expected) {
    require(value.ranks == expected.ranks, "rank count must equal input rank count");
    for (std::size_t i = 0; i < export_times.size(); ++i)
        require(same(value.*export_times[i], expected.*export_times[i]),
                std::string(time_names[i]) + " must preserve the expected double bits");
    require(value.write_step_count == expected.write_step_count, "count must retain rank 0 value");
    require(value.bytes_written == expected.bytes_written, "bytes must be counted once or max");
    require(value.peak_rss_bytes == expected.peak_rss_bytes, "RSS must be independent maximum");
}
void expect_json(const std::string& json, const IoStatsExport& expected) {
    require(token(json, "schema") == "\"chipcae.io_stats/1.0\"", "schema must be I20");
    require(token(json, "ranks") == std::to_string(expected.ranks), "JSON ranks mismatch");
    for (std::size_t i = 0; i < export_times.size(); ++i)
        require(same(duration(json, time_names[i]), expected.*export_times[i]),
                std::string(time_names[i]) + " JSON bits mismatch");
    require(token(json, "write_step_count") == std::to_string(expected.write_step_count),
            "JSON count mismatch");
    require(token(json, "bytes_written") == std::to_string(expected.bytes_written),
            "JSON bytes mismatch");
    require(token(json, "peak_rss_bytes") == std::to_string(expected.peak_rss_bytes),
            "JSON RSS mismatch");
}
IoStatsExport serial_expected(const Stats& s) {
    return {1,
            s.write_step_seconds,
            s.finalize_seconds,
            s.last_write_step_seconds,
            s.write_step_count,
            s.bytes_written,
            s.peak_rss_bytes};
}
void no_tmp(const fs::path& run) {
    require(!fs::exists(run / "io_stats.json.tmp"), "temporary export must not remain");
}
void format_tests(Suite& suite) {
    suite.run("format/frozen-example-bytes", [] {
        require(format_io_stats_json(example()) ==
                    read(fs::path(IOSE_FIXTURE_DIR) / "example_np2.json"),
                "formatter must equal frozen example bytes, including spaces and newline");
    });
    suite.run("format/defaults", [] {
        require(format_io_stats_json({}) ==
                    "{\"schema\": \"chipcae.io_stats/1.0\", \"ranks\": 1, "
                    "\"write_step_seconds\": 0.0, \"finalize_seconds\": 0.0, "
                    "\"last_write_step_seconds\": 0.0, \"write_step_count\": 0, "
                    "\"bytes_written\": 0, \"peak_rss_bytes\": 0}\n",
                "default JSON must use exact key order, separators and floating zero tokens");
    });
    const std::vector<double> boundaries = {0.0,
                                            1e-9,
                                            0.1,
                                            1.0 / 3.0,
                                            std::numeric_limits<double>::max(),
                                            std::numeric_limits<double>::min(),
                                            std::numeric_limits<double>::denorm_min(),
                                            std::nextafter(std::numeric_limits<double>::min(), 0.0),
                                            std::nextafter(std::numeric_limits<double>::min(), 1.0),
                                            2 * std::numeric_limits<double>::denorm_min(),
                                            std::nextafter(1.0, 0.0),
                                            std::nextafter(1.0, 2.0)};
    for (std::size_t position = 0; position < export_times.size(); ++position) {
        suite.run(std::string("format/roundtrip-") + time_names[position], [&, position] {
            auto values = boundaries;
            std::mt19937_64 random(0x1020CAFE);
            for (int i = 0; i < 4096; ++i) {
                const auto bits = random() & UINT64_C(0x7fffffffffffffff);
                const auto value = std::bit_cast<double>(bits);
                if (std::isfinite(value))
                    values.push_back(value);
            }
            for (const auto input : values) {
                auto value = example();
                value.*export_times[position] = input;
                const auto json = format_io_stats_json(value);
                const auto text = token(json, time_names[position]);
                require(text.find_first_of(".eE") != std::string::npos,
                        "duration must have decimal point or exponent");
                require(same(duration(json, time_names[position]), input),
                        "duration must round-trip bit-for-bit, including positive zero/subnormal");
            }
        });
        suite.run(std::string("format/invalid-") + time_names[position], [position] {
            for (const auto input : {std::numeric_limits<double>::quiet_NaN(),
                                     std::numeric_limits<double>::infinity(),
                                     -std::numeric_limits<double>::infinity(),
                                     -0.001,
                                     -0.0}) {
                auto value = example();
                value.*export_times[position] = input;
                invalid_argument([&] { (void)format_io_stats_json(value); },
                                 "invalid timing must throw std::invalid_argument");
            }
        });
    }
    suite.run("format/mandated-15-then-17-precision", [] {
        auto value = example();
        value.write_step_seconds = 1.0 / 3.0;
        value.finalize_seconds = 1.0;
        value.last_write_step_seconds = 0.0;
        const auto json = format_io_stats_json(value);
        require(token(json, "write_step_seconds") == "0.33333333333333331",
                "1/3 needs the prescribed 17-digit fallback");
        require(token(json, "finalize_seconds") == "1.0", "integer duration needs .0 suffix");
        require(token(json, "last_write_step_seconds") == "0.0", "positive zero must survive");
        value.write_step_seconds = 1e-9;
        require(token(format_io_stats_json(value), "write_step_seconds") == "1e-09",
                "defaultfloat exponent spelling must follow classic streams");
    });
    suite.run("format/full-width-integers", [] {
        auto value = example();
        value.ranks = INT_MAX;
        value.write_step_count = INT_MAX;
        value.bytes_written = UINT64_MAX;
        value.peak_rss_bytes = UINT64_MAX;
        const auto json = format_io_stats_json(value);
        require(token(json, "bytes_written") == "18446744073709551615", "uint64 bytes truncated");
        require(token(json, "peak_rss_bytes") == "18446744073709551615", "uint64 RSS truncated");
        require(token(json, "write_step_count") == "2147483647", "INT_MAX count truncated");
        require(token(json, "ranks") == "2147483647", "INT_MAX ranks truncated");
    });
}
void reduction_tests(Suite& suite) {
    suite.run("reduce/np1-preserves-every-field", [] {
        const auto s = local_example();
        const auto result = reduce_io_stats({s});
        expect_export(result.value, serial_expected(s));
        require(!result.write_step_count_mismatch && !result.bytes_written_mismatch,
                "one rank cannot have a mismatch");
    });
    suite.run("reduce/np2-equal-bytes-counted-once", [] {
        const auto s = local_example();
        const auto result = reduce_io_stats({s, s});
        expect_export(result.value, example());
        require(!result.write_step_count_mismatch && !result.bytes_written_mismatch,
                "equal rank inputs must not warn");
    });
    suite.run("reduce/np1-preserves-extremes-and-positive-zero", [] {
        auto s = local_example();
        s.write_step_seconds = std::numeric_limits<double>::max();
        s.finalize_seconds = 0.0;
        s.last_write_step_seconds = std::numeric_limits<double>::denorm_min();
        s.write_step_count = INT_MAX;
        s.bytes_written = UINT64_MAX;
        s.peak_rss_bytes = UINT64_MAX;
        const auto result = reduce_io_stats({s});
        expect_export(result.value, serial_expected(s));
        require(!result.write_step_count_mismatch && !result.bytes_written_mismatch,
                "extreme values on one rank must not warn");
    });
    suite.run("reduce/bytes-mismatch-max-and-only-bytes-flag", [] {
        const auto first = local_example();
        auto second = first;
        second.bytes_written = UINT64_MAX;
        const auto result = reduce_io_stats({first, second});
        auto expected = example();
        expected.bytes_written = UINT64_MAX;
        expect_export(result.value, expected);
        require(result.bytes_written_mismatch && !result.write_step_count_mismatch,
                "only bytes mismatch flag must be set");
        const auto reverse = reduce_io_stats({second, first});
        expect_export(reverse.value, expected);
        require(reverse.bytes_written_mismatch && !reverse.write_step_count_mismatch,
                "bytes mismatch must detect a lower non-root value too");
    });
    suite.run("reduce/count-mismatch-keeps-rank0-and-only-count-flag", [] {
        const auto first = local_example();
        auto second = first;
        second.write_step_count = INT_MAX;
        const auto result = reduce_io_stats({first, second});
        expect_export(result.value, example());
        require(result.write_step_count_mismatch && !result.bytes_written_mismatch,
                "only count mismatch flag must be set");
    });
    suite.run("reduce/np3-independent-maxima-and-rss", [] {
        std::vector<Stats> ranks(3, local_example());
        ranks[0].write_step_seconds = 1;
        ranks[0].finalize_seconds = 8;
        ranks[0].last_write_step_seconds = 3;
        ranks[0].peak_rss_bytes = 9;
        ranks[1].write_step_seconds = 9;
        ranks[1].finalize_seconds = 2;
        ranks[1].last_write_step_seconds = 4;
        ranks[1].peak_rss_bytes = UINT64_MAX;
        ranks[2].write_step_seconds = 5;
        ranks[2].finalize_seconds = 6;
        ranks[2].last_write_step_seconds = 7;
        ranks[2].peak_rss_bytes = 7;
        const auto result = reduce_io_stats(ranks);
        expect_export(result.value, {3, 9, 8, 7, 2, 1843200, UINT64_MAX});
        require(!result.bytes_written_mismatch && !result.write_step_count_mismatch,
                "maxima must not fabricate mismatches");
    });
    suite.run("reduce/empty-rejected", [] {
        invalid_argument([] { (void)reduce_io_stats({}); }, "empty rank vector must throw");
    });
    for (std::size_t position = 0; position < local_times.size(); ++position)
        suite.run(std::string("reduce/invalid-nonroot-") + time_names[position], [position] {
            for (const auto bad : {std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity(),
                                   -std::numeric_limits<double>::infinity(),
                                   -1.0,
                                   -0.0}) {
                for (const std::size_t rank : {0U, 1U}) {
                    std::vector<Stats> inputs(2, local_example());
                    inputs[rank].*local_times[position] = bad;
                    invalid_argument([&] { (void)reduce_io_stats(inputs); },
                                     "invalid rank timing must not disappear under max");
                }
            }
        });
}
ResultsStep cube(int rank = 0) {
    ResultsStep result;
    auto& p = result.hex;
    p.points = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1};
    p.hex = {0, 1, 2, 3, 4, 5, 6, 7};
    p.T.assign(8, 193.15);
    p.U.assign(24, 0);
    p.layer_id = {0};
    p.material_id = {0};
    p.cell_kind = {0};
    p.elem_gid = {rank};
    p.volume = {1};
    return result;
}
void step(ResultsWriter& writer, int n, int rank = 0) {
    writer.write_step(n, static_cast<double>(n), cube(rank));
    writer.add_summary("warpage_pv", 0, "m", n);
    writer.add_summary("T_max", 193.15, "K", n);
    writer.add_summary("U_mag_max", 0, "m", n);
}
Stats generate_writer(const fs::path& stackup, const fs::path& run, int rank = 0, int size = 1) {
    ResultsWriter writer(run, stackup);
    writer.set_mesh({size, 1, 0, size, 1, std::string(64, '0'), 0});
    step(writer, 0, rank);
    step(writer, 1, rank);
    writer.finalize();
    return writer.io_stats();
}
void write_tests(Suite& suite, const fs::path& stackup) {
    suite.run("write/serial-success-exact-bytes-and-replacement", [] {
        TemporaryRun run;
        auto local = local_example();
        auto result = write_io_stats_json(run.path, local);
        require(result.written && result.error.empty() && result.warnings.empty(),
                "serial successful export must report written=true with no errors/warnings");
        const auto first = read(run.path / "io_stats.json");
        require(first == format_io_stats_json(serial_expected(local)), "published bytes mismatch");
        expect_json(first, serial_expected(local));
        checker(run.path / "io_stats.json");
        no_tmp(run.path);
        local = {};
        result = write_io_stats_json(run.path, local);
        require(result.written && result.error.empty() && result.warnings.empty(),
                "replacement must succeed without warnings");
        const auto second = read(run.path / "io_stats.json");
        require(second == format_io_stats_json({}) && second != first,
                "replacement must completely remove previous bytes");
        checker(run.path / "io_stats.json");
        no_tmp(run.path);
    });
    suite.run("write/nonexistent-directory", [] {
        TemporaryRun run;
        const auto missing = run.path / "absent";
        const auto result = write_io_stats_json(missing, local_example());
        require(!result.written && !result.error.empty(), "missing directory must return error");
        require(!fs::exists(missing), "helper must not create nonexistent run directory");
        no_tmp(missing);
    });
    suite.run("write/non-directory-path", [] {
        TemporaryRun run;
        put(run.path / "regular-file", "sentinel");
        const auto result = write_io_stats_json(run.path / "regular-file", local_example());
        require(!result.written && !result.error.empty(), "non-directory must return error");
        require(read(run.path / "regular-file") == "sentinel", "setup file must survive");
    });
    suite.run("write/permission-denied-preserves-published-file", [] {
        TemporaryRun run;
        put(run.path / "io_stats.json", "previously published\n");
        fs::permissions(run.path,
                        fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read |
                            fs::perms::group_exec | fs::perms::others_read |
                            fs::perms::others_exec);
        const int status = child([&] {
            if (geteuid() == 0) {
                const auto* nobody = getpwnam("nobody");
                require(nobody != nullptr, "unprivileged nobody account required");
                require(setgid(nobody->pw_gid) == 0 && setuid(nobody->pw_uid) == 0,
                        "cannot drop privileges for permission-denial coverage");
            }
            const auto result = write_io_stats_json(run.path, local_example());
            require(!result.written && !result.error.empty(),
                    "permission denial must return error");
            require(read(run.path / "io_stats.json") == "previously published\n",
                    "permission failure must preserve published bytes");
            no_tmp(run.path);
            return 0;
        });
        require(status == 0, "unprivileged permission-denial assertions failed");
        require(read(run.path / "io_stats.json") == "previously published\n",
                "permission test must preserve publication");
        no_tmp(run.path);
    });
    suite.run("write/rename-failure-removes-tmp", [] {
        TemporaryRun run;
        fs::create_directory(run.path / "io_stats.json");
        put(run.path / "io_stats.json" / "sentinel", "keep");
        const auto result = write_io_stats_json(run.path, local_example());
        require(!result.written && !result.error.empty(),
                "rename onto directory must return error");
        require(read(run.path / "io_stats.json" / "sentinel") == "keep",
                "rename failure must preserve target contents");
        no_tmp(run.path);
    });
    suite.run("write/tmp-open-failure-preserves-publication", [] {
        TemporaryRun run;
        put(run.path / "io_stats.json", "old publication\n");
        fs::create_directory(run.path / "io_stats.json.tmp");
        const auto result = write_io_stats_json(run.path, local_example());
        require(!result.written && !result.error.empty(),
                "temporary open failure must return error");
        require(read(run.path / "io_stats.json") == "old publication\n",
                "temporary open failure must preserve published file");
        no_tmp(run.path);
    });
    for (std::size_t position = 0; position < local_times.size(); ++position)
        suite.run(std::string("write/invalid-") + time_names[position], [position] {
            for (const auto bad : {std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity(),
                                   -std::numeric_limits<double>::infinity(),
                                   -1.0,
                                   -0.0}) {
                TemporaryRun run;
                auto local = local_example();
                local.*local_times[position] = bad;
                auto result = write_io_stats_json(run.path, local);
                require(!result.written && !result.error.empty(),
                        "invalid timing must return error");
                require(!fs::exists(run.path / "io_stats.json"), "invalid timing must not publish");
                no_tmp(run.path);
                put(run.path / "io_stats.json", "old valid export\n");
                result = write_io_stats_json(run.path, local);
                require(!result.written && !result.error.empty(), "invalid replacement must fail");
                require(read(run.path / "io_stats.json") == "old valid export\n",
                        "invalid replacement must preserve published file");
                no_tmp(run.path);
            }
        });
    suite.run("write/before-any-finalize-checker-accepts-zero-rss-and-time", [&] {
        TemporaryRun run;
        ResultsWriter writer(run.path, stackup);
        step(writer, 0);
        const auto local = writer.io_stats();
        require(same(local.finalize_seconds, 0) && local.peak_rss_bytes == 0,
                "input before any finalize must have zero finalize time and RSS");
        const auto result = write_io_stats_json(run.path, local);
        require(result.written && result.error.empty() && result.warnings.empty(),
                "export must work before any finalize");
        expect_json(read(run.path / "io_stats.json"), serial_expected(local));
        checker(run.path / "io_stats.json");
        no_tmp(run.path);
    });
}
struct CommaDecimal : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
};
void locale_tests(Suite& suite) {
    const auto baseline = read(fs::path(IOSE_FIXTURE_DIR) / "example_np2.json");
    const std::string old_c = std::setlocale(LC_NUMERIC, nullptr);
    bool german_available = false;
    for (const char* name : {"de_DE.UTF-8", "de_DE"}) {
        if (std::setlocale(LC_NUMERIC, name)) {
            german_available = true;
            suite.run(std::string("format/C-locale-") + name, [&] {
                require(format_io_stats_json(example()) == baseline, "C locale changed bytes");
            });
        }
        std::setlocale(LC_NUMERIC, old_c.c_str());
    }
    if (!german_available)
        std::cout << "INFO German numeric locales unavailable; remaining assertions still run\n";
    const auto old_cpp = std::locale();
    try {
        std::locale::global(std::locale(""));
        suite.run("format/global-environment-locale", [&] {
            require(format_io_stats_json(example()) == baseline, "global locale changed bytes");
        });
    } catch (const std::runtime_error& error) {
        std::cout << "INFO environment C++ locale unavailable: " << error.what() << '\n';
    }
    std::locale::global(old_cpp);
    std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
    suite.run("format/global-comma-and-grouping-locale", [&] {
        require(format_io_stats_json(example()) == baseline, "comma/grouping locale changed bytes");
    });
    std::locale::global(old_cpp);
    std::setlocale(LC_NUMERIC, old_c.c_str());
}
void print_inputs(const Stats& local) {
    // Independent full-precision snapshot, immediately before calling the helper.
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::setprecision(17)
        << "{\"ranks\":1,\"write_step_seconds\":" << local.write_step_seconds
        << ",\"finalize_seconds\":" << local.finalize_seconds
        << ",\"last_write_step_seconds\":" << local.last_write_step_seconds
        << ",\"write_step_count\":" << local.write_step_count
        << ",\"bytes_written\":" << local.bytes_written
        << ",\"peak_rss_bytes\":" << local.peak_rss_bytes << "}";
    std::cout << out.str() << std::endl;
}
long peak_rss_kib() {
    rusage usage{};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "getrusage failed");
#ifdef __APPLE__
    return usage.ru_maxrss / 1024;
#else
    return usage.ru_maxrss;
#endif
}
double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}
void bench(const fs::path& stackup, int count) {
    require(count == 30, "acceptance benchmark requires exactly 30 real generation samples");
    require(child([] {
                execl(IOSE_PYTHON, IOSE_PYTHON, IOSE_DRIVER, "bench_environment", nullptr);
                return 127;
            }) == 0,
            "benchmark environment invalid; rerun when memory/load satisfy the brief");
    std::vector<double> helper, total, finalize;
    for (int i = 0; i < count; ++i) {
        TemporaryRun run;
        const auto local = generate_writer(stackup, run.path);
        const auto before = peak_rss_kib();
        const auto start = Clock::now();
        const auto result = write_io_stats_json(run.path, local);
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        const auto after = peak_rss_kib();
        require(result.written && result.error.empty() && result.warnings.empty(),
                "benchmark helper export must succeed");
        expect_json(read(run.path / "io_stats.json"), serial_expected(local));
        helper.push_back(elapsed);
        total.push_back(local.write_step_seconds + local.finalize_seconds);
        finalize.push_back(local.finalize_seconds);
        std::cout << std::setprecision(17) << "{\"sample\":" << i
                  << ",\"helper_seconds\":" << elapsed
                  << ",\"write_step_seconds\":" << local.write_step_seconds
                  << ",\"finalize_seconds\":" << local.finalize_seconds
                  << ",\"peak_rss_before_kib\":" << before << ",\"peak_rss_after_kib\":" << after
                  << "}\n";
    }
    std::cout.flush();
    require(median(helper) <= 0.00969 * median(total), "E1: helper median exceeds 0.969% of total");
    require(median(helper) <= 0.344 * median(finalize),
            "E2: helper median exceeds 0.344 of finalize");
}
#ifdef CHIPCAE_ENABLE_MPI
void mpi_call(int code, const std::string& operation) {
    require(code == MPI_SUCCESS, "test MPI operation failed: " + operation);
}
int mpi_tests(const fs::path& stackup) {
    int rank = 0, size = 0;
    mpi_call(MPI_Comm_rank(MPI_COMM_WORLD, &rank), "rank");
    mpi_call(MPI_Comm_size(MPI_COMM_WORLD, &size), "size");
    require(size == 2, "iose_mpi_2 requires exactly two ranks");
    fixtures();
    TemporaryRun temporary;
    std::string shared = rank == 0 ? temporary.path.string() : std::string{};
    int length = static_cast<int>(shared.size());
    mpi_call(MPI_Bcast(&length, 1, MPI_INT, 0, MPI_COMM_WORLD), "path length");
    shared.resize(static_cast<std::size_t>(length));
    mpi_call(MPI_Bcast(shared.data(), length, MPI_CHAR, 0, MPI_COMM_WORLD), "shared path");
    const fs::path base(shared);
    const auto sentinel = temporary.path / "nonroot-private";
    fs::create_directory(sentinel);
    Suite suite;
    std::string bytes_warning, count_warning;
    auto local = local_example();
    local.write_step_seconds = rank == 0 ? 1 : 9;
    local.finalize_seconds = rank == 0 ? 8 : 2;
    local.last_write_step_seconds = rank == 0 ? 3 : 7;
    local.peak_rss_bytes = rank == 0 ? 3 : UINT64_MAX;
    for (const std::string mode : {"equal",
                                   "precision",
                                   "bytes",
                                   "count",
                                   "both",
                                   "root-failure",
                                   "root-unwritable",
                                   "invalid-rank1-write",
                                   "invalid-rank1-finalize",
                                   "invalid-rank1-last",
                                   "invalid-rank1-negative-zero-write",
                                   "invalid-rank1-negative-zero-finalize",
                                   "invalid-rank1-negative-zero-last"}) {
        auto input = local;
        if (mode == "precision") {
            input.write_step_seconds = rank == 0 ? 1.0 / 3.0 : std::nextafter(1.0 / 3.0, 1.0);
            input.finalize_seconds = rank == 0 ? 0.1 : std::nextafter(0.1, 1.0);
            input.last_write_step_seconds = rank == 0 ? 1e-9 : std::nextafter(1e-9, 1.0);
            input.write_step_count = INT_MAX;
        }
        if (rank == 1 && (mode == "bytes" || mode == "both"))
            input.bytes_written = UINT64_MAX;
        if (rank == 1 && (mode == "count" || mode == "both"))
            input.write_step_count = INT_MAX;
        if (rank == 1 && mode == "invalid-rank1-write")
            input.write_step_seconds = std::numeric_limits<double>::quiet_NaN();
        if (rank == 1 && mode == "invalid-rank1-finalize")
            input.finalize_seconds = -1;
        if (rank == 1 && mode == "invalid-rank1-last")
            input.last_write_step_seconds = std::numeric_limits<double>::infinity();
        if (rank == 1 && mode == "invalid-rank1-negative-zero-write")
            input.write_step_seconds = -0.0;
        if (rank == 1 && mode == "invalid-rank1-negative-zero-finalize")
            input.finalize_seconds = -0.0;
        if (rank == 1 && mode == "invalid-rank1-negative-zero-last")
            input.last_write_step_seconds = -0.0;
        const auto root_path = base / mode;
        const bool failure =
            mode == "root-failure" || mode == "root-unwritable" || mode.starts_with("invalid-");
        if (rank == 0 && mode != "root-failure")
            fs::create_directory(root_path);
        if (rank == 0 && mode == "root-unwritable")
            fs::permissions(root_path,
                            fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read |
                                fs::perms::group_exec | fs::perms::others_read |
                                fs::perms::others_exec);
        mpi_call(MPI_Barrier(MPI_COMM_WORLD), "scenario ready");
        const auto original_uid = geteuid();
        const bool lower_privilege = rank == 0 && mode == "root-unwritable" && original_uid == 0;
        if (lower_privilege) {
            const auto* nobody = getpwnam("nobody");
            require(nobody != nullptr && seteuid(nobody->pw_uid) == 0,
                    "cannot lower root privilege for collective permission-denial test");
        }
        const auto result = write_io_stats_json(rank == 0 ? root_path : sentinel, input);
        if (lower_privilege)
            require(seteuid(original_uid) == 0, "cannot restore test root identity");
        suite.run("mpi/" + mode, [&] {
            no_tmp(sentinel);
            require(!fs::exists(sentinel / "io_stats.json"), "nonroot must not write JSON");
            require(fs::is_empty(sentinel), "nonroot must not create any output");
            if (rank != 0) {
                require(!result.written && result.warnings.empty(),
                        "nonroot must return written=false and no warnings");
                if (failure)
                    require(result.error == "io_stats.json export failed on rank 0",
                            "nonroot failure text must match frozen contract exactly");
                else
                    require(result.error.empty(), "nonroot success must have empty error");
                return;
            }
            no_tmp(root_path);
            if (failure) {
                require(!result.written && !result.error.empty(),
                        "root failure must return written=false and specific error");
                require(result.warnings.empty(), "matching inputs must not produce warnings");
                require(result.error != "io_stats.json export failed on rank 0",
                        "root must retain the specific failure cause");
                require(!fs::exists(root_path / "io_stats.json"), "failure must not publish JSON");
                return;
            }
            require(result.written && result.error.empty(), "root export must succeed");
            IoStatsExport expected{2, 9, 8, 7, 2, 1843200, UINT64_MAX};
            if (mode == "precision") {
                expected.write_step_seconds = std::nextafter(1.0 / 3.0, 1.0);
                expected.finalize_seconds = std::nextafter(0.1, 1.0);
                expected.last_write_step_seconds = std::nextafter(1e-9, 1.0);
                expected.write_step_count = INT_MAX;
            }
            if (mode == "bytes" || mode == "both")
                expected.bytes_written = UINT64_MAX;
            expect_json(read(root_path / "io_stats.json"), expected);
            checker(root_path / "io_stats.json");
            const std::size_t warnings = mode == "equal" || mode == "precision" ? 0
                                         : mode == "both"                       ? 2
                                                                                : 1;
            require(result.warnings.size() == warnings, "root mismatch warning count incorrect");
            for (const auto& warning : result.warnings)
                require(!warning.empty(), "mismatch warning must be nonempty");
            if (mode == "bytes")
                bytes_warning = result.warnings[0];
            if (mode == "count")
                count_warning = result.warnings[0];
            if (mode == "both")
                require(result.warnings == std::vector<std::string>{count_warning, bytes_warning},
                        "combined warnings must be count then bytes");
        });
    }
    const auto real_path = base / "real";
    const auto real_local = generate_writer(stackup, real_path, rank, size);
    // Test-side capture only: homogeneous ranks execute this same test binary.
    static_assert(std::is_trivially_copyable_v<Stats>);
    std::array<Stats, 2> inputs{};
    mpi_call(MPI_Gather(&real_local,
                        static_cast<int>(sizeof(Stats)),
                        MPI_BYTE,
                        inputs.data(),
                        static_cast<int>(sizeof(Stats)),
                        MPI_BYTE,
                        0,
                        MPI_COMM_WORLD),
             "capture real helper inputs");
    const auto real_result = write_io_stats_json(rank == 0 ? real_path : sentinel, real_local);
    suite.run("mpi/real-distributed-generation", [&] {
        require(real_result.error.empty() && real_result.warnings.empty(),
                "real distributed export must succeed without mismatch warnings");
        no_tmp(sentinel);
        require(fs::is_empty(sentinel), "real generation helper must not touch nonroot path");
        require(real_result.written == (rank == 0), "real export written flag must be root-only");
        if (rank != 0)
            return;
        IoStatsExport expected{2, 0, 0, 0, inputs[0].write_step_count, inputs[0].bytes_written, 0};
        for (const auto& input : inputs) {
            for (std::size_t i = 0; i < export_times.size(); ++i)
                expected.*export_times[i] =
                    std::max(expected.*export_times[i], input.*local_times[i]);
            expected.peak_rss_bytes = std::max(expected.peak_rss_bytes, input.peak_rss_bytes);
            require(input.bytes_written == inputs[0].bytes_written,
                    "I19 real distributed bytes must already agree");
        }
        expect_json(read(real_path / "io_stats.json"), expected);
        checker(real_path / "io_stats.json");
        std::uint64_t bytes = fs::file_size(real_path / "manifest.json");
        int steps = 0;
        for (const auto& entry : fs::directory_iterator(real_path / "out"))
            if (entry.path().extension() == ".vtkhdf") {
                bytes += entry.file_size();
                ++steps;
            }
        require(steps == 2, "real distributed generation must have two steps");
        require(bytes > 0 && expected.bytes_written == bytes,
                "distributed export bytes must equal file sizes once, never 2X");
        no_tmp(real_path);
    });
    int total_failures = 0;
    mpi_call(MPI_Allreduce(&suite.failures, &total_failures, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD),
             "test failures");
    mpi_call(MPI_Barrier(MPI_COMM_WORLD), "finish before root cleanup");
    return total_failures == 0 ? 0 : 1;
}
#endif
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 3,
                "usage: iose_tool unit|generate|mpi|bench STACKUP [RUN|30] [--no-helper]");
        std::setlocale(LC_NUMERIC, "C");
        std::cout.imbue(std::locale::classic());
        const std::string mode = argv[1];
        if (mode == "unit") {
            Suite suite;
            // Frozen fixture integrity is checked before any feature assertion.
            fixtures();
            suite.run("fixture/frozen-sha256", fixtures);
            format_tests(suite);
            reduction_tests(suite);
            write_tests(suite, argv[2]);
            locale_tests(suite);
            std::cout << "unit: " << suite.failures << " failed of " << suite.checks << " groups\n";
            return suite.failures == 0 ? 0 : 1;
        }
        if (mode == "generate") {
            require(argc == 4 || (argc == 5 && std::string(argv[4]) == "--no-helper"),
                    "generate requires RUN and optional --no-helper");
            const auto local = generate_writer(argv[2], argv[3]);
            print_inputs(local);
            if (argc == 4) {
                const auto result = write_io_stats_json(argv[3], local);
                require(result.written && result.error.empty() && result.warnings.empty(),
                        "generate helper must publish successfully: " + result.error);
            }
            return 0;
        }
        if (mode == "bench") {
            require(argc == 4, "bench requires sample count 30");
            bench(argv[2], std::stoi(argv[3]));
            return 0;
        }
#ifdef CHIPCAE_ENABLE_MPI
        if (mode == "mpi") {
            mpi_call(MPI_Init(&argc, &argv), "init");
            const int status = mpi_tests(argv[2]);
            mpi_call(MPI_Finalize(), "finalize");
            return status;
        }
#endif
        throw std::runtime_error("unknown or unavailable mode: " + mode);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
#ifdef CHIPCAE_ENABLE_MPI
        int initialized = 0, finalized = 0;
        MPI_Initialized(&initialized);
        if (initialized)
            MPI_Finalized(&finalized);
        if (initialized && !finalized)
            MPI_Abort(MPI_COMM_WORLD, 1);
#endif
        return 1;
    }
}
