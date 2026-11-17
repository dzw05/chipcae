#include "../../geometry/ecad/src/odbpp/archive.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <tuple>
#ifdef __linux__
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace chipcae::geometry::ecad;
using namespace chipcae::geometry::ecad::detail;
namespace {
void require(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}
void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
    require(out.good(), "fixture write failed: " + path.string());
}
std::string bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "read fixture " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
OdbJob job(const fs::path& path) {
    OdbJob out;
    const auto result = read_odbpp_job(path, out);
    require(bool(result), path.string() + ": " + result.error);
    return out;
}
ImportResult parse_job(const fs::path& path) {
    OdbJob parsed;
    return read_odbpp_job(path, parsed);
}
void rejected(const fs::path& path, const std::string& reason) {
    auto result = parse_job(path);
    require(!result && result.error.starts_with("IMPORT_REJECTED:") &&
                result.error.find(reason) != std::string::npos,
            path.filename().string() + ": expected rejection containing '" + reason + "', got '" +
                result.error + "'");
}
bool reported(const std::vector<UnsupportedElement>& reports,
              const std::string& name,
              std::uint64_t line = 0) {
    return std::any_of(reports.begin(), reports.end(), [&](const auto& report) {
        return report.code == "IMPORT_UNSUPPORTED_ELEMENT" &&
               report.element.find(name) != std::string::npos && (!line || report.line == line);
    });
}
// Visit every frozen field, preserving vector order and map keys. Compare doubles
// directly with ==, including its signed-zero and NaN semantics.
struct Snapshot {
    std::ostringstream out;
    std::vector<double> doubles;
    void add(double x) { doubles.push_back(x); }
    void add(const std::string& s) { out << s.size() << ':' << s << ';'; }
    template <class T>
    void add(const T& x) {
        out << x << ';';
    }
    void add(const OdbPoint& p) {
        add(p.x);
        add(p.y);
    }
    void add(const OdbSegment& s) {
        add(s.end);
        add(s.is_arc);
        add(s.center);
        add(s.cw);
    }
    void add(const OdbContour& c) {
        add(c.is_hole);
        add(c.start);
        add(c.segs);
    }
    void add(const OdbLayer& l) {
        add(l.name);
        add(l.context);
        add(l.type);
        add(l.polarity);
        add(l.row);
        add(l.start_name);
        add(l.end_name);
    }
    void add(const OdbSymbol& s) { add(s.name); }
    void add(const OdbPad& p) {
        add(p.at);
        add(p.sym);
        add(p.positive);
        add(p.rot_deg);
        add(p.mirror);
    }
    void add(const OdbLine& l) {
        add(l.s);
        add(l.e);
        add(l.sym);
        add(l.positive);
    }
    void add(const OdbArc& a) {
        add(a.s);
        add(a.e);
        add(a.c);
        add(a.sym);
        add(a.positive);
        add(a.cw);
    }
    void add(const OdbSurface& s) {
        add(s.positive);
        add(s.contours);
    }
    void add(const OdbFeatures& f) {
        add(f.symbols);
        add(f.pads);
        add(f.lines);
        add(f.arcs);
        add(f.surfaces);
    }
    void add(const OdbTool& t) {
        add(t.num);
        add(t.type);
        add(t.finish_size);
    }
    template <class T>
    void add(const std::vector<T>& v) {
        add(v.size());
        for (const auto& x : v)
            add(x);
    }
    template <class T>
    void add(const std::map<std::string, T>& m) {
        add(m.size());
        for (const auto& [k, v] : m) {
            add(k);
            add(v);
        }
    }
    std::pair<std::string, std::vector<double>> take(const OdbJob& j) {
        add(static_cast<int>(j.units));
        add(j.step);
        add(j.layers);
        add(j.attrs.job);
        add(j.attrs.layer);
        add(j.profile.contours);
        add(j.features);
        add(j.tools);
        return {out.str(), std::move(doubles)};
    }
};
std::map<std::string, std::string> archive(const fs::path& path) {
    ArchiveReader reader(path);
    require(reader.open_error().empty(), path.string() + ": " + reader.open_error());
    std::map<std::string, std::string> files;
    for (const auto& entry : reader.entries()) {
        if (entry.is_dir)
            continue;
        std::string bytes, error;
        require(reader.read_bytes(entry.path, bytes, error), entry.path + ": " + error);
        require(bytes.size() == entry.size, "entry metadata size: " + entry.path);
        require(files.emplace(entry.path, bytes).second, "duplicate exposed path");
    }
    return files;
}
void minimal(const fs::path& root) {
    write(root / "matrix/matrix",
          "UNITS=MM\nSTEP {\nCOL=1\nNAME=pcb\n}\nLAYER {\nROW=1\nNAME=top\nTYPE=SIGNAL\n}\n");
    write(root / "steps/pcb/profile",
          "UNITS=MM\nS P 0\nOB 0 0 I\nOS 1 0\nOS 1 1\nOS 0 1\nOS 0 0\nOE\nSE\n");
}
#ifdef __linux__
void measure_streaming_memory(const fs::path& p) {
    rusage before{}, after{};
    require(getrusage(RUSAGE_SELF, &before) == 0, "getrusage before");
    const auto start = std::chrono::steady_clock::now();
    auto result = parse_job(p);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    require(getrusage(RUSAGE_SELF, &after) == 0, "getrusage after");
    require(bool(result), result.error);
    std::cout << "MEASURE odbpp_streaming_memory RSS_delta_KiB="
              << after.ru_maxrss - before.ru_maxrss
              << " elapsed_s=" << std::chrono::duration<double>(elapsed).count() << '\n';
    require(after.ru_maxrss - before.ru_maxrss < 64 * 1024, "RSS delta must be <64 MiB");
    // Also bound the fresh process peak so a pre-import high-water mark cannot mask growth.
    require(after.ru_maxrss < 64 * 1024, "fresh-process RSS peak must be <64 MiB");
    require(elapsed < std::chrono::seconds(60), "streaming wall clock must be <60s");
}
#endif
} // namespace
int main(int argc, char** argv) {
#ifdef __linux__
    if (argc == 3 && std::string_view(argv[1]) == "--streaming-memory") {
        try {
            measure_streaming_memory(argv[2]);
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "FAIL odbpp_streaming_memory: " << e.what() << '\n';
            return 1;
        }
    }
#endif

    const auto source = fs::path(__FILE__).parent_path().parent_path().parent_path();
    const auto fixture = source / "tests/integration/ecad/data/odbpp/kicad4l";
    const auto temp =
        fs::temp_directory_path() /
        ("odb1-tdd-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(temp);
    const auto script = fixture.parent_path() / "gen_archives.py";
    const auto quote = [](const fs::path& p) {
        std::string s = "'";
        for (char c : p.string())
            s += c == '\'' ? "'\\''" : std::string(1, c);
        return s + "'";
    };
    const auto command = "python3 " + quote(script) + " " + quote(temp) + " " + quote(fixture);
    if (std::system(command.c_str()) != 0) {
        std::cerr << "fixture generator failed\n";
        fs::remove_all(temp);
        return 1;
    }
    int failed = 0;
    const auto test = [&](const std::string& name, const std::function<void()>& fn) {
        try {
            fn();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    };
    test("odbpp_review_symbol_indices", [&] {
        auto p = temp / "symbol-indices";
        minimal(p);
        write(p / "steps/pcb/layers/top/features", "$1 r100\n$1 r200\n");
        rejected(p, "duplicate symbol index");
        fs::remove_all(p);
        minimal(p);
        write(p / "steps/pcb/layers/top/features",
              "$0 r100\n$1 r200\nP 0 0 1 P 0 0\nP 0 0 -1 P 0 0\nP 0 0 2 P 0 0\n");
        OdbJob parsed;
        auto result = read_odbpp_job(p, parsed);
        require(bool(result), result.error);
        require(parsed.features.at("top").symbols.size() == 2, "sequential definitions retained");
        require(reported(result.model.report.unsupported, "unknown symbol -1") &&
                    reported(result.model.report.unsupported, "unknown symbol 2"),
                "out-of-range references reported");
    });
    test("odbpp_sparse_symbol_references", [&] {
        auto p = temp / "sparse-symbols";
        minimal(p);
        write(p / "steps/pcb/layers/top/features",
              "$7 r100\n$1 r200\nP 0 0 1 P 0 0\nL 0 0 1 1 7 P\nA 1 0 -1 0 0 0 1 P 0 Y\n");
        OdbJob parsed;
        auto result = read_odbpp_job(p, parsed);
        require(bool(result), result.error);
        const auto& f = parsed.features.at("top");
        require(f.pads.at(0).sym == 1 && f.lines.at(0).sym == 0 && f.arcs.at(0).sym == 1,
                "references resolve to definition order");
        require(!reported(result.model.report.unsupported, "unknown symbol"), "known sparse ids");
        write(p / "steps/pcb/profile", "OB 1 0 I\nOC 1 0 0 0 Y\nOE\n");
        require(job(p).profile.contours.at(0).segs.size() == 1, "full circle retained");
    });
    test("odbpp_unread_payload_and_early_stop", [&] {
        auto p = temp / "unread-payload";
        write(p / "small", "ok");
        write(p / "large", "stop\n" + std::string(200000, 'x'));
        ArchiveLimits limits;
        limits.file_bytes = 32768;
        limits.total_bytes = 32770;
        std::string out, error;
        for (const auto& input : {p, temp / "early-stop.tgz", temp / "early-stop.zip"}) {
            ArchiveReader reader(input, limits);
            require(reader.open_error().empty(), reader.open_error());
            require(reader.read_bytes("small", out, error) && out == "ok", error);
            require(reader.for_each_line(
                        "large", [](auto) { return false; }, error) &&
                        error.empty(),
                    "stop before unread tail exceeds budget: " + error);
            require(!reader.read_bytes("large", out, error) &&
                        error.find("file byte limit") != std::string::npos,
                    "consuming large payload still enforces limit");
        }
        for (const auto* name : {"limits.tgz", "job.zip"}) {
            limits.file_bytes = 1;
            limits.total_bytes = 1;
            ArchiveReader archive_reader(temp / name, limits);
            require(archive_reader.open_error().empty(), archive_reader.open_error());
            require(!archive_reader.read_bytes(archive_reader.entries().front().path, out, error),
                    "archive payload limit enforced on read");
        }
    });
    test("odbpp_review_padded_entry_ratio", [&] {
        for (const auto* order : {"before", "after"}) {
            ArchiveReader reader(temp / "corrupt" /
                                 (std::string("ratio-padded-") + order + ".tgz"));
            require(reader.open_error().empty(), reader.open_error());
            std::string out, error;
            require(reader.read_bytes("noise", out, error), error);
            require(!reader.read_bytes("bomb", out, error) &&
                        error.find("ratio limit") != std::string::npos,
                    "requested entry ratio cannot be diluted by neighbors");
        }
    });
    test("odbpp_ratio_deflate_boundary_headers", [&] {
        ArchiveReader reader(temp / "ratio-empty-blocks.tgz");
        require(reader.open_error().empty(), reader.open_error());
        std::string out, error;
        require(reader.read_bytes("neighbor", out, error) && out == "neighbor", error);
        require(!reader.read_bytes("bomb", out, error) &&
                    error.find("ratio limit") != std::string::npos,
                "deflate framing after payload cannot subsidize its ratio");
    });
    test("odbpp_review_all_entry_counts", [&] {
        ArchiveLimits limits;
        limits.entry_count = 2;
        for (const auto* name : {"dirs-only.zip", "unsupported-count.zip"}) {
            ArchiveReader reader(temp / name, limits);
            require(reader.open_error().find("entry count limit") != std::string::npos,
                    std::string(name) + " entry count bounded");
        }
        auto p = temp / "only-directories";
        fs::create_directories(p / "a/b/c");
        ArchiveReader reader(p, limits);
        require(reader.open_error().find("entry count limit") != std::string::npos,
                "directory-only tree bounded");
    });
    test("odbpp_review_optional_zip_encoding", [&] {
        for (const auto* name : {"optional-features.zip", "optional-tools.zip"}) {
            OdbJob parsed;
            const auto result = read_odbpp_job(temp / name, parsed);
            require(bool(result), result.error);
            require(parsed.features.at("IN1.CU").surfaces.size() == 1, "other layers retained");
            require(reported(result.model.report.unsupported, "steps/pcb/layers/f.cu/"),
                    "optional unsupported encoding reported");
        }
    });
    test("odbpp_review_required_zip_encoding", [&] {
        for (const auto* name : {"unsupported.zip", "encrypted.zip", "zip64.zip"})
            rejected(temp / name, "unsupported");
        rejected(temp / "zip64.zip", "zip64");
        rejected(temp / "required-profile.zip", "unsupported");
    });
    test("odbpp_review_gzip_zero_padding", [&] {
        auto data = bytes(temp / "boundary-32745.gz");
        data.append(40000, '\0');
        std::string out, error;
        require(!decode_gzip(data, out, error) &&
                    error.starts_with("IMPORT_REJECTED: gzip invalid header"),
                "zero tail must be parsed as a member header");
        data.back() = 'x';
        require(!decode_gzip(data, out, error), "nonzero tail rejected");
    });
    test("odbpp_zero_compressed_zip_entry", [&] {
        for (const auto* method : {"stored", "deflate"}) {
            ArchiveReader reader(temp / (std::string("zero-compressed-") + method + ".zip"));
            require(reader.open_error().starts_with("IMPORT_REJECTED:") &&
                        reader.open_error().find("ratio") == std::string::npos,
                    "zero compressed size is corruption, not a ratio violation");
        }
        ArchiveReader empty(temp / "empty-entry.zip");
        std::string out, error;
        require(empty.open_error().empty(), empty.open_error());
        require(empty.read_bytes("payload", out, error) && out.empty(), error);
    });
    test("odbpp_tar_total_requested_payload", [&] {
        ArchiveLimits limits;
        limits.total_bytes = 2;
        ArchiveReader exact(temp / "early-stop.tgz", limits);
        require(exact.open_error().empty(), exact.open_error());
        std::string out, error;
        require(exact.read_bytes("small", out, error) && out == "ok", error);
        require(exact.read_bytes("small", out, error) && out == "ok", "repeat read charged once");
        limits.total_bytes = 1;
        ArchiveReader over(temp / "early-stop.tgz", limits);
        require(over.open_error().empty(), over.open_error());
        require(!over.read_bytes("small", out, error) &&
                    error.find("total byte limit") != std::string::npos,
                "requested payload enforces exact total boundary");
        ArchiveReader framing(temp / "framing-only.tgz", limits);
        require(framing.open_error() == "IMPORT_REJECTED: empty input",
                "framing is not extracted content");
    });
    test("odbpp_gzip_sliding_match_history", [&] {
        std::string out, error;
        require(decode_gzip(bytes(temp / "sliding-match.gz"), out, error), error);
        require(out == std::string(1024 * 1024, 'x'),
                "every byte across suspended match and history slides");
    });
    test("odbpp_selected_payload_limits_and_Z", [&] {
        for (const auto* name : {"selected-job.tgz", "selected-job.zip"})
            require(bool(parse_job(temp / name)), "irrelevant high-ratio member permits import");
        for (const auto* name : {"selected.tgz", "selected.zip"}) {
            ArchiveLimits limits;
            limits.total_bytes = 2;
            limits.file_bytes = 2;
            ArchiveReader reader(temp / name, limits);
            require(reader.open_error().empty(), reader.open_error());
            std::string out, error;
            require(reader.read_bytes("small", out, error) && out == "ok", error);
            ArchiveReader ratio_reader(temp / name);
            require(ratio_reader.for_each_line(
                        "irrelevant", [](std::string_view) { return false; }, error),
                    "early stop before ratio limit: " + error);
            require(!ratio_reader.read_bytes("irrelevant", out, error) &&
                        error.find("ratio limit") != std::string::npos,
                    "ratio enforced when payload requested");
        }
        auto dir = temp / "nested-Z";
        write(dir / "payload.Z", "compressed bytes");
        for (const auto& input : {dir, temp / "nested-Z.tgz", temp / "nested-Z.zip"}) {
            ArchiveReader reader(input);
            require(reader.open_error().empty(), reader.open_error());
            require(reported(reader.unsupported(), "payload.Z: .Z compression"),
                    "nested .Z reported");
            require(!reader.is_supported("payload.Z"), "nested .Z unsupported status");
            std::string out, error;
            require(!reader.read_bytes("payload.Z", out, error) && out.empty() &&
                        error.find("unsupported .Z compression") != std::string::npos,
                    "nested .Z bytes never consumed");
            bool called = false;
            require(!reader.for_each_line(
                        "payload.Z",
                        [&](auto) {
                            called = true;
                            return true;
                        },
                        error) &&
                        !called && error.find("unsupported .Z compression") != std::string::npos,
                    "nested .Z never reaches text parser");
        }
    });
    test("odbpp_early_stop_preserves_total_budget", [&] {
        for (const auto* name : {"early-stop.tgz", "early-stop.zip"}) {
            ArchiveLimits limits;
            limits.file_bytes = 32768;
            limits.total_bytes = 32768;
            ArchiveReader reader(temp / name, limits);
            std::string out, error;
            require(reader.open_error().empty(), reader.open_error());
            require(reader.for_each_line("large", [](auto) { return false; }, error), error);
            require(reader.read_bytes("small", out, error) && out == "ok",
                    "stopped block must not exhaust later reads: " + error);
        }
    });
    test("odbpp_required_and_irrelevant_Z_archives", [&] {
        for (const auto* extension : {".tgz", ".zip"}) {
            auto irrelevant = parse_job(temp / (std::string("irrelevant-Z") + extension));
            require(bool(irrelevant) && reported(irrelevant.model.report.unsupported,
                                                 "irrelevant.Z: .Z compression"),
                    "irrelevant .Z reported while job imports: " + irrelevant.error);
            for (const auto* label : {"matrix-Z", "profile-Z"}) {
                const std::string path =
                    std::string(label) == "matrix-Z" ? "matrix/matrix" : "steps/pcb/profile";
                auto required = parse_job(temp / (std::string(label) + extension));
                require(
                    !required && required.error.starts_with("IMPORT_REJECTED:") &&
                        required.error ==
                            "IMPORT_REJECTED: required file uses unsupported .Z compression " +
                                path + ".Z" &&
                        reported(required.model.report.unsupported, path + ".Z: .Z compression"),
                    "required compressed file rejected with source diagnostic: " + required.error);
            }
        }
    });
    test("odbpp_required_Z_directory", [&] {
        for (const auto* path : {"matrix/matrix", "steps/pcb/profile"}) {
            auto dir =
                temp / (std::string(path) == "matrix/matrix" ? "matrix-Z-dir" : "profile-Z-dir");
            minimal(dir);
            fs::rename(dir / path, dir / (std::string(path) + ".Z"));
            auto result = parse_job(dir);
            require(!result &&
                        result.error ==
                            "IMPORT_REJECTED: required file uses unsupported .Z compression " +
                                std::string(path) + ".Z" &&
                        reported(result.model.report.unsupported,
                                 std::string(path) + ".Z: .Z compression"),
                    "required directory .Z source diagnostic: " + result.error);
            minimal(dir);
            require(bool(parse_job(dir)), "uncompressed required file takes precedence");
        }
    });
    test("odbpp_gzip_total_output_limit", [&] {
        for (const auto* name : {"boundary-32745.gz", "multi.tgz"}) {
            const auto compressed = bytes(temp / name);
            std::string full, error;
            require(decode_gzip(compressed, full, error), error);
            for (const auto limit : {full.size() - 1, full.size()}) {
                std::istringstream input(compressed);
                GzipCursor cursor(input, limit);
                std::string_view block;
                std::size_t delivered = 0;
                while (cursor.next(block, error, 17))
                    delivered += block.size();
                require(delivered <= limit, "gzip never exposes bytes beyond total limit");
                if (limit == full.size())
                    require(error.empty() && delivered == limit, "exact gzip total limit");
                else
                    require(error == "IMPORT_REJECTED: gzip total byte limit", error);
            }
        }
    });
    test("odbpp_empty_matrix", [&] {
        const auto dir = temp / "empty-matrix";
        for (const auto* text : {"", "# comment\n", "UNITS=MM\n"}) {
            minimal(dir);
            write(dir / "matrix/matrix", text);
            rejected(dir, "matrix contains no layers");
        }
        fs::remove(dir / "matrix/matrix");
        rejected(dir, "missing file matrix/matrix");
    });
    test("odbpp_gzip_live_output_limit", [&] {
        std::istringstream input(bytes(temp / "boundary-32745.gz"));
        GzipCursor cursor(input);
        std::string_view block;
        std::string error, decoded;
        std::size_t calls = 0;
        for (;;) {
            const std::size_t limit = calls++ % 3 == 0 ? 1 : (calls % 2 ? 7 : 513);
            if (!cursor.next(block, error, limit))
                break;
            require(block.size() <= limit, "live gzip output limit");
            decoded.append(block);
        }
        require(error.empty() && decoded == std::string(32745, 'x'), "bounded gzip bytes");
    });
    test("odbpp_gzip_multimember_tiny_chunks", [&] {
        std::istringstream input(bytes(temp / "multi.tgz"));
        GzipCursor cursor(input);
        std::string_view block;
        std::string decoded, error;
        std::size_t calls = 0;
        for (;;) {
            const std::size_t limit = calls++ % 2 ? 17 : 1;
            if (!cursor.next(block, error, limit))
                break;
            require(block.size() <= limit, "multimember chunk bound");
            decoded.append(block);
        }
        require(error.empty() && decoded == bytes(temp / "job.tar"),
                "buffered trailers and following members with tiny output");
    });
    test("odbpp_directory_crlf_lines", [&] {
        auto p = temp / "crlf-lines";
        // CR falls at the end of a 32 KiB input block.
        write(p / "text", std::string(32767, 'x') + "\r\nshort\r\nlast\r");
        ArchiveReader reader(p);
        std::vector<std::string> lines;
        std::string error;
        require(reader.for_each_line(
                    "text",
                    [&](auto line) {
                        lines.emplace_back(line);
                        return true;
                    },
                    error),
                error);
        require(lines == std::vector<std::string>{std::string(32767, 'x'), "short", "last"},
                "CRLF normalized across blocks and at EOF");
    });
    test("odbpp_line_callback_errors", [&] {
        auto p = temp / "callback-errors";
        for (const auto* content : {"stop\ntail\n", "stop"}) {
            write(p / "small", content);
            for (const auto& input : {p, temp / "early-stop.tgz", temp / "early-stop.zip"}) {
                ArchiveReader reader(input);
                require(reader.open_error().empty(), reader.open_error());
                std::string error;
                require(!reader.for_each_line(
                            "small",
                            [&](auto) {
                                error = "IMPORT_REJECTED: callback failure";
                                return false;
                            },
                            error),
                        "callback error propagated");
                require(error == "IMPORT_REJECTED: callback failure", "callback error retained");
            }
        }
    });
    test("odbpp_review_oldgnu_times", [&] {
        ArchiveReader reader(temp / "oldgnu-times.tgz");
        require(reader.open_error().empty(), reader.open_error());
        std::string out, error;
        require(reader.read_bytes("matrix/matrix", out, error) && out == "oldgnu", error);
    });
    test("odbpp_raw_duplicate_points_accepted",
         [&] { require(bool(parse_job(fixture)), "raw export accepted"); });
    test("odbpp_invalid_contour_arc_radius", [&] {
        auto p = temp / "invalid-contour-radius";
        for (bool feature : {false, true}) {
            for (const auto* records :
                 {"OB 0 0 I\nOC 0 0 0 0 Y\nOE\n", "OB 1 0 I\nOC 0 2 0 0 Y\nOS 1 0\nOE\n"}) {
                fs::remove_all(p);
                minimal(p);
                write(p / (feature ? "steps/pcb/layers/top/features" : "steps/pcb/profile"),
                      (feature ? "S P 0\n" : "") + std::string(records) + (feature ? "SE\n" : ""));
                rejected(p, "invalid arc geometry");
            }
        }
    });
    test("odbpp_review_contour_geometry", [&] {
        auto p = temp / "bad-geometry";
        for (bool feature : {false, true}) {
            for (const auto* records : {"OB 0 0 I\nOE\n",
                                        "OB 0 0 I\nOS 1 0\nOS 0 0\nOE\n",
                                        "OB 0 0 I\nOS 1 0\nOS 1 1\nOS 0 1\nOE\n",
                                        "OB 0 0 I\nOS 0 0\nOE\n",
                                        "OB 0 0 I\nOC 1 0 0 0 Y\nOE\n",
                                        "OB 0 0 I\nOC 2 0 0 1 Y\nOE\n",
                                        "OB nan 0 I\nOS 1 0\nOE\n",
                                        "OB 0 0 I\nOC 1 0 inf 0 Y\nOE\n",
                                        "OB 0 0 I\nOS 1e309 0\nOE\n"}) {
                fs::remove_all(p);
                minimal(p);
                write(p / (feature ? "steps/pcb/layers/top/features" : "steps/pcb/profile"),
                      "S P 0\n" + std::string(records) + "SE\n");
                rejected(p, "");
            }
        }
        for (const auto* record :
             {"L 0 0 0 0 0 P", "L 0 0 inf 1 0 P", "A 0 0 2 0 0 1 0 P 0 Y", "P nan 0 0 P 0 0"}) {
            fs::remove_all(p);
            minimal(p);
            write(p / "steps/pcb/layers/top/features", std::string(record) + "\n");
            rejected(p, "");
        }
        for (bool feature : {false, true}) {
            fs::remove_all(p);
            minimal(p);
            write(p / (feature ? "steps/pcb/layers/top/features" : "steps/pcb/profile"),
                  "S P 0\nOB 1 0 I\nOC -1 0 0 0 Y\nOC 1 0 0 0 Y\nOE\nSE\n");
            require(bool(parse_job(p)), "two semicircles form valid contour");
        }
    });
    test("odbpp_review_tar_forward_cursor", [&] {
        const auto path = temp / "forward.tgz";
        fs::copy_file(temp / "many-small.tgz", path);
        ArchiveReader reader(path);
        std::string out, error;
        require(reader.read_bytes("file0", out, error), error);
#ifdef __linux__
        // The retained descriptor must serve forward reads without reopening the path.
        fs::remove(path);
#endif
        for (int i = 1; i < 100; ++i)
            require(reader.read_bytes("file" + std::to_string(i), out, error) &&
                        out == std::string(1024, 'x'),
                    error);
    });
    test("odbpp_matrix_order", [&] {
        auto j = job(temp / "job");
        // 实测: 31 blocks in matrix order; KiCad copper table F.Cu/In1.Cu/In2.Cu/B.Cu.
        const std::vector<std::string> names = {"COMP_+_TOP",
                                                "F.SILKSCREEN",
                                                "F.PASTE",
                                                "F.MASK",
                                                "F.CU",
                                                "DIELECTRIC_1",
                                                "IN1.CU",
                                                "DIELECTRIC_2",
                                                "IN2.CU",
                                                "DIELECTRIC_3",
                                                "B.CU",
                                                "B.MASK",
                                                "B.PASTE",
                                                "B.SILKSCREEN",
                                                "F.ADHESIVE",
                                                "B.ADHESIVE",
                                                "USER.DRAWINGS",
                                                "USER.COMMENTS",
                                                "USER.ECO1",
                                                "USER.ECO2",
                                                "EDGE.CUTS",
                                                "MARGIN",
                                                "B.COURTYARD",
                                                "F.COURTYARD",
                                                "B.FAB",
                                                "F.FAB",
                                                "USER.1",
                                                "USER.2",
                                                "USER.3",
                                                "USER.4",
                                                "DRILL_PLATED_F.CU-B.CU"};
        const std::vector<std::string> types = {
            "component",    "silk_screen", "solder_paste", "solder_mask", "signal",   "dielectric",
            "signal",       "dielectric",  "signal",       "dielectric",  "signal",   "solder_mask",
            "solder_paste", "silk_screen", "document",     "document",    "document", "document",
            "document",     "document",    "document",     "document",    "document", "document",
            "document",     "document",    "document",     "document",    "document", "document",
            "drill"};
        require(j.layers.size() == names.size(), "31 matrix layers");
        for (std::size_t i = 0; i < names.size(); ++i)
            require(j.layers[i].name == names[i] && j.layers[i].type == types[i] &&
                        j.layers[i].row == int(i + 1),
                    "matrix row " + std::to_string(i + 1));
    });
    test("odbpp_matrix_four_types", [&] {
        auto p = temp / "four";
        minimal(p);
        write(p / "matrix/matrix",
              "UNITS=MM\nSTEP {\nNAME=pcb\n}\nLAYER {\nROW=1\nNAME=top\nTYPE=SIGNAL\n}\nLAYER "
              "{\nROW=2\nNAME=core\nTYPE=DIELECTRIC\n}\nLAYER "
              "{\nROW=3\nNAME=plane\nTYPE=POWER_GROUND\n}\nLAYER "
              "{\nROW=4\nNAME=bottom\nTYPE=SIGNAL\n}\n");
        auto j = job(p);
        require(j.layers.size() == 4, "four layers");
        const std::array<std::string, 4> names{"top", "core", "plane", "bottom"};
        const std::array<std::string, 4> types{"signal", "dielectric", "power_ground", "signal"};
        for (std::size_t i = 0; i < 4; ++i)
            require(j.layers[i].name == names[i] && j.layers[i].type == types[i] &&
                        j.layers[i].row == int(i + 1),
                    "four-layer order/type");
    });
    test("odbpp_matrix_shuffled_rows", [&] {
        const auto p = temp / "shuffled-rows";
        minimal(p);
        write(p / "matrix/matrix",
              "UNITS=MM\nLAYER {\nROW=4\nNAME=bottom\nTYPE=SIGNAL\n}\n"
              "LAYER {\nROW=2\nNAME=core\nTYPE=DIELECTRIC\n}\n"
              "LAYER {\nROW=1\nNAME=top\nTYPE=SIGNAL\n}\n"
              "LAYER {\nROW=3\nNAME=plane\nTYPE=POWER_GROUND\n}\n");
        const auto j = job(p);
        const std::array<std::string, 4> names{"top", "core", "plane", "bottom"};
        const std::array<std::string, 4> types{"signal", "dielectric", "power_ground", "signal"};
        require(j.layers.size() == names.size(), "four shuffled layers");
        for (std::size_t i = 0; i < names.size(); ++i)
            require(j.layers[i].name == names[i] && j.layers[i].type == types[i] &&
                        j.layers[i].row == int(i + 1),
                    "shuffled matrix row order/type");
        write(p / "matrix/matrix",
              "LAYER {\nROW=2\nNAME=last\n}\nLAYER {\nNAME=first\n}\n"
              "LAYER {\nROW=0\nNAME=second\n}\n");
        const auto tied = job(p);
        require(tied.layers.size() == 3 && tied.layers[0].name == "first" &&
                    tied.layers[1].name == "second" && tied.layers[2].name == "last",
                "equal and default rows preserve block order");
    });
    test("odbpp_review_tolerant_closure", [&] {
        const auto p = temp / "tolerant-closure";
        minimal(p);
        for (const auto* end : {"0.0000005 0.0000005", "-0.0000005 -0.0000005"}) {
            write(p / "steps/pcb/profile",
                  std::string("OB 0 0 I\nOS 10 0\nOS 10 10\nOS 0 10\nOS ") + end + "\nOE\n");
            require(bool(parse_job(p)), "closure within per-axis tolerance accepted");
        }
        write(p / "steps/pcb/profile", "OB 0 0 I\nOS 10 0\nOS 10 10\nOS 0 10\nOS 0.000002 0\nOE\n");
        rejected(p, "profile contour not closed");
    });
    test("odbpp_directory_read_size_limits", [&] {
        const auto p = temp / "directory-open-limits";
        write(p / "irrelevant-a", "123");
        write(p / "irrelevant-b", "456");
        ArchiveLimits limits;
        limits.file_bytes = 2;
        ArchiveReader file_reader(p, limits);
        require(file_reader.open_error().empty(), file_reader.open_error());
        std::string out, error;
        require(!file_reader.read_bytes("irrelevant-a", out, error) &&
                    error.find("file byte limit") != std::string::npos,
                "oversized selected file rejected on read");
        limits.file_bytes = 3;
        limits.total_bytes = 5;
        ArchiveReader total_reader(p, limits);
        require(total_reader.open_error().empty(), total_reader.open_error());
        require(total_reader.read_bytes("irrelevant-a", out, error), error);
        require(!total_reader.read_bytes("irrelevant-b", out, error) &&
                    error.find("total byte limit") != std::string::npos,
                "selected aggregate rejected on read");
        limits.total_bytes = 6;
        ArchiveReader exact_reader(p, limits);
        require(exact_reader.open_error().empty(), exact_reader.open_error());
        require(exact_reader.read_bytes("irrelevant-a", out, error), error);
        require(exact_reader.read_bytes("irrelevant-b", out, error),
                "exact directory limits accepted: " + error);
    });
    test("odbpp_profile_closed", [&] {
        auto j = job(temp / "job");
        require(j.profile.contours.size() == 1 && !j.profile.contours[0].is_hole,
                "one outer contour");
        const auto& c = j.profile.contours[0];
        require(!c.segs.empty(), "profile segments");
        double scale = j.units == OdbUnits::mm ? 1e-3 : .0254;
        require(std::abs(c.segs.back().end.x - c.start.x) * scale <= 1e-9 &&
                    std::abs(c.segs.back().end.y - c.start.y) * scale <= 1e-9,
                "per-axis closure");
        double twice = 0;
        auto previous = c.start;
        for (const auto& s : c.segs) {
            require(!s.is_arc, "KiCad reference rectangle uses straight edges");
            twice += previous.x * s.end.y - s.end.x * previous.y;
            previous = s.end;
        }
        // 实测: tools/fixtures/kicad_odbpp/kicad4l.kicad_pcb Edge.Cuts spans (10,10)-(50,40).
        constexpr double reference = .04 * .03;
        require(std::abs(std::abs(twice) * .5 * scale * scale - reference) / reference <= 1e-9,
                "relative KiCad outline area");
    });
    test("odbpp_profile_arcs", [&] {
        auto p = temp / "arcs";
        minimal(p);
        write(p / "steps/pcb/profile", "UNITS=MM\nS P 0\nOB 0 0 I\nOC 2 0 1 0 N\nOS 0 0\nOE\nSE\n");
        auto j = job(p);
        require(j.profile.contours.size() == 1, "arc contour");
        const auto& c = j.profile.contours[0];
        require(c.segs.size() == 2 && c.segs[0].is_arc && !c.segs[1].is_arc &&
                    c.segs[0].center.x == 1 && c.segs[0].center.y == 0 && c.segs[0].end.x == 2,
                "preserve arc geometry without tessellation");
    });
    test("odbpp_features_counts", [&] {
        auto j = job(temp / "job");
        // 实测: P/L/A/S records counted independently from checked-in layer files.
        const std::map<std::string, std::array<std::size_t, 4>> nonzero = {
            {"F.CU", {2, 0, 1, 1}},
            {"IN1.CU", {2, 0, 0, 1}},
            {"IN2.CU", {2, 0, 0, 1}},
            {"B.CU", {2, 0, 0, 1}},
            {"DRILL_PLATED_F.CU-B.CU", {2, 0, 0, 0}},
            {"EDGE.CUTS", {0, 4, 0, 0}},
            {"F.SILKSCREEN", {0, 10, 0, 0}}};
        require(j.layers.size() == 31, "all fixture layers");
        for (const auto& layer : j.layers) {
            auto it = nonzero.find(layer.name);
            const auto expected = it == nonzero.end() ? std::array<std::size_t, 4>{} : it->second;
            auto f = j.features.find(layer.name);
            const OdbFeatures empty;
            const auto& records = f == j.features.end() ? empty : f->second;
            require((std::array{records.pads.size(),
                                records.lines.size(),
                                records.arcs.size(),
                                records.surfaces.size()} == expected),
                    "实测 counts: " + layer.name);
        }
    });
    test("odbpp_duplicate_entry", [&] {
        auto r = parse_job(temp / "duplicate.tgz");
        require(r.error.starts_with("IMPORT_REJECTED: duplicate archive entry matrix/matrix"),
                r.error);
    });
    test("odbpp_duplicate_zip", [&] {
        auto r = parse_job(temp / "duplicate.zip");
        require(r.error.starts_with("IMPORT_REJECTED: duplicate archive entry matrix/matrix"),
                r.error);
    });
    test("odbpp_archive_normalized", [&] {
        const auto files = archive(temp / "normalized.tgz");
        require(files.size() == 1 && files.at("matrix/matrix") == "normalized",
                "canonical separators and leading ./");
    });
    test("odbpp_line_record_semantics", [&] {
        auto p = temp / "line-semantics";
        for (const std::string text : {"", "\n", "a\n", "a\n\n", "a", "\r"}) {
            write(p / "text", text);
            ArchiveReader reader(p);
            std::vector<std::string> actual, expected;
            std::istringstream input(text);
            std::string line, error;
            while (std::getline(input, line)) {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                expected.push_back(line);
            }
            require(reader.for_each_line(
                        "text",
                        [&](auto value) {
                            actual.emplace_back(value);
                            return true;
                        },
                        error),
                    error);
            require(actual == expected, "getline record semantics");
        }
    });
    test("odbpp_archive_lines", [&] {
        ArchiveReader reader(temp / "job.tgz");
        std::ifstream in(fixture / "matrix/matrix");
        std::vector<std::string> expected, actual;
        std::string line, error;
        while (std::getline(in, line))
            expected.push_back(line);
        require(reader.for_each_line(
                    "matrix/matrix",
                    [&](std::string_view value) {
                        actual.emplace_back(value);
                        return true;
                    },
                    error),
                error);
        require(actual == expected, "streamed line bytes/order");
        std::size_t seen = 0;
        require(reader.for_each_line(
                    "matrix/matrix",
                    [&](std::string_view) {
                        ++seen;
                        return false;
                    },
                    error) &&
                    seen == 1,
                "callback early stop");
    });
    test("odbpp_snapshot_double_equality", [&] {
        OdbJob a, b;
        a.profile.contours.push_back({});
        b.profile.contours.push_back({});
        b.profile.contours[0].start.x = -0.0;
        require(Snapshot().take(a) == Snapshot().take(b), "signed zeros compare equal");
        b.profile.contours[0].start.x = std::nextafter(0.0, 1.0);
        require(Snapshot().take(a) != Snapshot().take(b), "distinct doubles compare unequal");
        a.profile.contours[0].start.x = std::numeric_limits<double>::quiet_NaN();
        b = a;
        require(Snapshot().take(a) != Snapshot().take(b), "NaN compares unequal to itself");
    });
    test("odbpp_tgz_zip_dir_equal", [&] {
        auto expected = Snapshot().take(job(temp / "job"));
        for (const auto* name : {"job.tgz", "job.zip", "stored.zip", "JOB.TAR.GZ", "JOB.ZIP"})
            require(Snapshot().take(job(temp / name)) == expected,
                    std::string("all frozen fields: ") + name);
    });
    test("odbpp_archive_tar_formats", [&] {
        const auto expected = archive(temp / "ustar.tgz");
        require(expected.size() == 2, "regular files only");
        require(expected.contains(std::string(155, 'p') + "/" + std::string(100, 'f')),
                "maximum ustar prefix and basename represented across all three formats");
        require(expected == archive(temp / "gnu.tgz") && expected == archive(temp / "pax.tgz"),
                "ustar/GNU/pax bytes");
        const auto long_files = archive(temp / "gnu-long.tgz");
        require(long_files == archive(temp / "pax-long.tgz"), "GNU L / pax path bytes");
        require(std::any_of(long_files.begin(),
                            long_files.end(),
                            [](const auto& item) { return item.first.size() > 255; }),
                "path beyond ustar representable length");
    });
    test("odbpp_archive_gzip_multimember", [&] {
        std::string decoded, error;
        require(decode_gzip(bytes(temp / "multi.tgz"), decoded, error), error);
        require(decoded == bytes(temp / "job.tar"), "every decoded gzip byte");

        require(archive(temp / "multi.tgz") == archive(temp / "job.tgz"),
                "member bytes including optional headers");
        rejected(temp / "corrupt/crc.tgz", "CRC");
    });
    test("odbpp_gzip_validation_before_early_line_stop", [&] {
        const auto valid = bytes(temp / "job.tgz");
        require(valid.size() > 8, "gzip fixture has trailer");
        for (const auto& reason :
             {std::string("CRC"), std::string("ISIZE"), std::string("truncated trailer")}) {
            auto corrupt = valid;
            if (reason == "truncated trailer")
                corrupt.pop_back();
            else
                corrupt[corrupt.size() - (reason == "CRC" ? 8 : 4)] ^= 1;
            // A later member starts after the first tar's two end blocks.
            for (bool later_member : {false, true}) {
                const auto path = temp / "trailer-validation.tgz";
                write(path, (later_member ? valid : std::string{}) + corrupt);
                ArchiveReader reader(path);
                require(reader.open_error().find(reason) != std::string::npos,
                        "index validates every gzip trailer: " + reader.open_error());
                std::size_t callbacks = 0;
                std::string error;
                require(!reader.for_each_line(
                            "matrix/matrix",
                            [&](auto) {
                                ++callbacks;
                                return false;
                            },
                            error) &&
                            callbacks == 0 && error == reader.open_error(),
                        "early line stop cannot hide corrupt trailer: " + error);
            }
        }
        std::istringstream input(valid);
        std::string error;
        require(!stream_gzip(
                    input, [](auto) { return false; }, error),
                "standalone gzip cancellation never reports success");
    });
    test("odbpp_archive_gzip_bad_crc", [&] {
        std::string decoded, error;
        const bool ok = decode_gzip(bytes(temp / "corrupt/crc.tgz"), decoded, error);
        require(!ok && error.starts_with("IMPORT_REJECTED:") &&
                    error.find("CRC") != std::string::npos,
                "expected gzip CRC rejection, got " + error);
    });
    test("odbpp_archive_lzw", [&] {
        const auto dir = temp / "lzw-fallback";
        write(dir / "payload.Z", bytes(temp / "job.tar.Z"));
        ArchiveReader reader(dir);
        require(reader.open_error().empty(), reader.open_error());
        require(reported(reader.unsupported(), "payload.Z: .Z compression"),
                "nested .Z fallback reports its path");
    });
    for (const auto* reason : {"total", "file", "entry", "ratio"}) {
        test(std::string("odbpp_archive_limits_") + reason, [&, reason] {
            ArchiveLimits limits;
            if (std::string(reason) == "total")
                limits.total_bytes = 10000;
            if (std::string(reason) == "file")
                limits.file_bytes = 4096;
            if (std::string(reason) == "entry")
                limits.entry_count = 1;
            if (std::string(reason) == "ratio")
                limits.ratio = 2;
            ArchiveReader reader(
                temp / (std::string(reason) == "ratio" ? "limits-ratio.tgz" : "limits.tgz"),
                limits);
            std::string error = reader.open_error();
            if (error.empty())
                for (const auto& e : reader.entries()) {
                    std::string bytes;
                    if (!reader.read_bytes(e.path, bytes, error))
                        break;
                }
            require(error.starts_with("IMPORT_REJECTED:") &&
                        error.find(reason) != std::string::npos,
                    std::string("expected ") + reason + " limit rejection, got " + error);
        });
    }
    for (const auto& [name, reason] :
         std::vector<std::pair<std::string, std::string>>{{"absolute.tgz", "path"},
                                                          {"drive.tgz", "path"},
                                                          {"traversal.tgz", "path"},
                                                          {"checksum.tgz", "checksum"},
                                                          {"isize.tgz", "ISIZE"},
                                                          {"truncated.tgz", "trunc"},
                                                          {"bad-crc.zip", "CRC"},
                                                          {"missing-matrix.tgz", "matrix"},
                                                          {"missing-profile.tgz", "profile"},
                                                          {"crc.tgz", "CRC"},
                                                          {"empty", "empty input"},
                                                          {"empty.tgz", "empty input"},
                                                          {"empty.zip", "empty input"}})
        test("odbpp_corrupt_" + name, [&, name, reason] {
            rejected(fs::exists(temp / "corrupt" / name) ? temp / "corrupt" / name : temp / name,
                     reason);
        });
    test("odbpp_tar_unsupported_types", [&] {
        ArchiveReader reader(temp / "types.tgz");
        require(reader.open_error().empty(), reader.open_error());
        for (const auto* reason : {"hard: tar hardlink",
                                   "sym: tar symlink",
                                   "char: tar character device",
                                   "block: tar block device",
                                   "fifo: tar FIFO",
                                   "contiguous: tar contiguous file"})
            require(reported(reader.unsupported(), reason), reason);
    });
    test("odbpp_tar_all_entry_duplicates", [&] {
        for (const auto kind : {'1', '2', '3', '4', '6', '7', '5'}) {
            for (const auto* order : {"False", "True"}) {
                ArchiveReader reader(
                    temp / ("shared-path-" + std::string(1, kind) + "-" + order + ".tgz"));
                require(reader.open_error() == "IMPORT_REJECTED: duplicate archive entry shared",
                        reader.open_error());
            }
        }
        ArchiveReader reader(temp / "duplicate-directories.tgz");
        require(reader.open_error() == "IMPORT_REJECTED: duplicate archive entry shared",
                reader.open_error());
    });
    test("odbpp_archive_symlink", [&] {
        ArchiveReader reader(temp / "link.tgz");
        require(reader.open_error().empty(), reader.open_error());
        require(std::none_of(reader.entries().begin(),
                             reader.entries().end(),
                             [](const auto& e) { return e.path == "outside-link"; }),
                "symlink skipped");
        require(reported(reader.unsupported(), "outside-link"), "symlink unsupported diagnostic");
    });
    test("odbpp_unknown_record", [&] {
        auto result = parse_job(temp / "corrupt/unknown");
        require(bool(result), result.error);
        require(reported(result.model.report.unsupported, "steps/pcb/layers/f.cu/features", 3),
                "unknown record source filename and exact line 3");
    });
    test("odbpp_matrix_duplicate_layer", [&] {
        auto p = temp / "duplicate-layer";
        minimal(p);
        std::ofstream out(p / "matrix/matrix", std::ios::app);
        out << "LAYER {\nNAME=top\nROW=2\n}\n";
        out.close();
        rejected(p, "duplicate layer top");
    });
    test("odbpp_matrix_missing_name", [&] {
        auto p = temp / "missing-name";
        minimal(p);
        write(p / "matrix/matrix", "UNITS=MM\nLAYER {\nROW=1\n}\n");
        rejected(p, "matrix layer without NAME");
    });
    test("odbpp_no_step", [&] {
        auto p = temp / "no-step";
        minimal(p);
        fs::remove_all(p / "steps");
        rejected(p, "step");
    });
    test("odbpp_optional_defaults", [&] {
        auto p = temp / "defaults";
        minimal(p);
        write(p / "matrix/matrix", "UNITS=MM\nSTEP {\nNAME=pcb\n}\nLAYER {\nNAME=top\n}\n");
        auto j = job(p);
        require(j.layers.size() == 1, "default layer");
        const auto& l = j.layers[0];
        require(l.context.empty() && l.type.empty() && l.polarity.empty() && l.row == 0 &&
                    l.start_name.empty() && l.end_name.empty(),
                "frozen optional layer defaults");
        require(j.attrs.job.empty() && j.attrs.layer.empty() && j.tools.empty(),
                "absent optional maps");
        require(j.features.empty() || j.features.at("top").pads.empty(),
                "missing features allowed");
        write(p / "steps/pcb/layers/top/features", "");
        require(bool(parse_job(p)), "empty features allowed");
    });
    test("odbpp_attrs_tools", [&] {
        auto p = temp / "attrs";
        minimal(p);
        write(p / "misc/attrlist", "customer=sample\nrevision=B\n");
        write(p / "steps/pcb/layers/top/attrlist", "material=copper\n");
        write(p / "steps/pcb/layers/top/tools",
              "UNITS=MM\nTOOLS {\nNUM=9\nTYPE=PLATED\nFINISH_SIZE=1.25\n}\nTOOLS "
              "{\nNUM=2\nTYPE=NON_PLATED\n}\n");
        const auto j = job(p);
        require(j.attrs.job == (std::map<std::string, std::string>{{"customer", "sample"},
                                                                   {"revision", "B"}}) &&
                    j.attrs.layer.at("top").at("material") == "copper",
                "attrlist key/value pairs");
        const auto& t = j.tools.at("top");
        require(t.size() == 2 && t[0].num == 9 && t[0].type == "PLATED" &&
                    t[0].finish_size == 1.25 && t[1].num == 2 && t[1].finish_size == 0,
                "tool file order and defaults");
    });
    for (const auto& [name, end, accepted] :
         std::vector<std::tuple<std::string, std::string, bool>>{
             {"tolerance", "0.0000009 0.0000009", true},
             {"open-x", "0.0000011 0", false},
             {"open-y", "0 0.0000011", false}}) {
        test("odbpp_profile_" + name, [&, name, end, accepted] {
            auto p = temp / name;
            minimal(p);
            write(p / "steps/pcb/profile",
                  "UNITS=MM\nS P 0\nOB 0 0 I\nOS 1 0\nOS 1 1\nOS " + end + "\nOE\nSE\n");
            if (accepted)
                require(bool(parse_job(p)), "per-axis 1e-9 meter tolerance");
            else
                rejected(p, "profile contour not closed");
        });
    }
    test("odbpp_pax_size", [&] {
        const auto files = archive(temp / "pax-size.tgz");
        require(files.size() == 1 && files.at("matrix/matrix") == "pax!",
                "pax path and size override");
    });
    test("odbpp_gzip_header_crc", [&] { rejected(temp / "header-crc.tgz", "CRC"); });
    test("odbpp_zip_unsupported", [&] {
        for (const auto* name : {"unsupported.zip", "zip64.zip", "encrypted.zip"}) {
            ArchiveReader reader(temp / name);
            require(reader.open_error().empty(), reader.open_error());
            require(reported(reader.unsupported(), "matrix/matrix"),
                    std::string(name) + " diagnostic");
        }
    });
    test("odbpp_step_selection", [&] {
        auto p = temp / "step-selection";
        minimal(p);
        fs::rename(p / "steps/pcb", p / "steps/first");
        auto r = parse_job(p);
        require(bool(r) && reported(r.model.report.unsupported, "selected step first"),
                "fallback step report");
        require(job(p).step == "first", "first discovered step");
        minimal(p);
        require(job(p).step == "pcb", "prefer pcb over other steps");
    });
    test("odbpp_units_symbols_records", [&] {
        auto p = temp / "record-diagnostics";
        minimal(p);
        write(p / "steps/pcb/layers/top/features",
              "UNITS=INCH\n$0 vendor_shape\nT text\nB barcode\nP 0 0 7 P 0 0\n");
        const auto r = parse_job(p);
        require(bool(r), r.error);
        const auto& u = r.model.report.unsupported;
        require(reported(u, "features: unit mismatch", 1) &&
                    reported(u, "unsupported symbol vendor_shape", 2) &&
                    reported(u, "record T", 3) && reported(u, "record B", 4) &&
                    reported(u, "unknown symbol 7", 5),
                "diagnostic file and exact line for each unsupported element");
    });
    test("odbpp_directory_symlink", [&] {
        for (const auto* kind : {"external-directory", "external-file", "internal", "dangling"}) {
            auto p = temp / (std::string("directory-links-") + kind);
            minimal(p);
            std::error_code ec;
            const auto link = p / "link";
            if (std::string_view(kind) == "external-directory")
                fs::create_directory_symlink(temp / "job", link, ec);
            else if (std::string_view(kind) == "external-file")
                fs::create_symlink(temp / "job/matrix/matrix", link, ec);
            else if (std::string_view(kind) == "internal")
                fs::create_symlink(p / "matrix/matrix", link, ec);
            else
                fs::create_symlink(p / "missing", link, ec);
            if (ec)
                return; // Platforms without symlink privileges still exercise tar links.
            ArchiveReader reader(p);
            require(reader.open_error().empty(), reader.open_error());
            require(reported(reader.unsupported(), "link: directory input symlink unsupported"),
                    "directory symlink reported");
            require(std::none_of(reader.entries().begin(),
                                 reader.entries().end(),
                                 [](const auto& e) {
                                     return e.path == "link" || e.path.starts_with("link/");
                                 }),
                    "symlink never traversed or indexed");
            std::string output, error;
            require(reader.read_bytes("matrix/matrix", output, error), error);
            require(bool(parse_job(p)), "unrelated symlink permits import");
        }
    });
    test("odbpp_symbol_order_and_nested_Z", [&] {
        auto p = temp / "symbol-order";
        minimal(p);
        write(p / "steps/pcb/layers/top/features", "$0 r100\n$1 oval100x200\n");
        write(p / "steps/pcb/layers/top/tools.Z", "unsupported compressed file");
        auto j = job(p);
        const auto& symbols = j.features.at("top").symbols;
        require(symbols.size() == 2 && symbols[0].name == "r100" &&
                    symbols[1].name == "oval100x200",
                "symbol definition appearance order");
        const auto r = parse_job(p);
        require(reported(r.model.report.unsupported, "tools.Z"), "nested .Z fallback source path");
        write(p / "steps/pcb/layers/top/features", "$3 r100\n$1 oval100x200\n");
        require(bool(parse_job(p)), "sparse definitions accepted");
    });
    test("odbpp_review_contour_lifetime", [&] {
        auto p = temp / "contour-lifetime";
        minimal(p);
        for (const auto* record : {"S P", "SE", "OB 1 1 I"}) {
            write(p / "steps/pcb/layers/top/features",
                  std::string("S P\nOB 0 0 I\n") + record + "\nOS 0 0\nOE\nSE\n");
            rejected(p, "record while contour open");
        }
    });
    test("odbpp_review_case_sensitive_layers", [&] {
        auto p = temp / "layer-case";
        minimal(p);
        write(p / "matrix/matrix", "UNITS=MM\nLAYER {\nNAME=Top\n}\nLAYER {\nNAME=top\n}\n");
        write(p / "steps/pcb/layers/Top/features", "$0 r100\nP 0 0 0 P 0 0\n");
        write(p / "steps/pcb/layers/top/features", "$0 r200\n");
        write(p / "steps/pcb/layers/Top/attrlist", "label=upper\n");
        write(p / "steps/pcb/layers/top/attrlist", "label=lower\n");
        auto j = job(p);
        require(j.attrs.layer.at("Top").at("label") == "upper" &&
                    j.attrs.layer.at("top").at("label") == "lower",
                "case-sensitive layer attributes");
        require(j.layers[0].name == "Top" && j.layers[1].name == "top" &&
                    j.features.at("Top").pads.size() == 1 && j.features.at("top").pads.empty(),
                "distinct case-sensitive layer names and feature paths");
        auto r = parse_job(p);
        require(!reported(r.model.report.unsupported, "nonsequential symbol index"),
                "fixture-compatible zero-based symbol definitions");
    });
    test("odbpp_review_empty_unsupported", [&] {
        auto r = parse_job(temp / "job.tar.Z");
        require(!r && r.error == "IMPORT_REJECTED: unsupported archive extension" &&
                    r.model.report.unsupported.empty(),
                "top-level .Z is outside the supported archive formats");
        auto p = temp / "only-links";
        fs::create_directories(p);
        std::error_code ec;
        fs::create_symlink(temp / "job.tar.Z", p / "link", ec);
        if (!ec)
            rejected(p, "empty input");
    });
    test("odbpp_review_runtime_limits", [&] {
        for (bool lines : {false, true}) {
            for (bool total : {false, true}) {
                auto p = temp / "growing-input";
                fs::create_directories(p);
                write(p / "a", "a");
                write(p / "b", "b");
                ArchiveLimits limits;
                limits.file_bytes = total ? 100 : 4;
                limits.total_bytes = total ? 4 : 100;
                ArchiveReader reader(p, limits);
                require(reader.open_error().empty(), reader.open_error());
                std::string out, error;
                require(reader.read_bytes("b", out, error), error);
                // Reading the same file again must not consume its budget twice.
                require(reader.read_bytes("b", out, error), error);
                write(p / "a", "12345");
                bool ok = lines ? reader.for_each_line(
                                      "a", [](auto) { return true; }, error)
                                : reader.read_bytes("a", out, error);
                require(!ok && error.find(total ? "total byte limit" : "file byte limit") !=
                                   std::string::npos,
                        "runtime limit: " + error);
            }
        }
        auto p = temp / "long-line";
        minimal(p);
        write(p / "steps/pcb/layers/top/features", std::string(1024 * 1024 + 1, '#'));
        rejected(p, "line byte limit");
    });
    test("odbpp_review_archive_line_limits", [&] {
        for (const auto* extension : {".tgz", ".zip"}) {
            for (const auto* kind : {"boundary",
                                     "boundary-crlf",
                                     "boundary-cr",
                                     "overflow",
                                     "overflow-newline",
                                     "overflow-crlf"}) {
                ArchiveReader reader(temp / (std::string("line-") + kind + extension));
                require(reader.open_error().empty(), reader.open_error());
                std::string error;
                std::size_t calls = 0;
                const bool ok = reader.for_each_line(
                    "matrix/matrix",
                    [&](std::string_view line) {
                        ++calls;
                        require(line.size() == 1024 * 1024, "exact line boundary preserved");
                        return true;
                    },
                    error);
                if (std::string_view(kind).starts_with("boundary"))
                    require(ok && calls == 1, "line at limit accepted: " + error);
                else
                    require(!ok && calls == 0 &&
                                error.starts_with("IMPORT_REJECTED: line byte limit"),
                            "archive line overflow rejected before callback: " + error);
            }
        }
    });
    test("odbpp_review_directory_payload_count", [&] {
        auto p = temp / "directory-count";
        write(p / "a/b/c/payload", "x");
        fs::create_directories(p / "empty/nested");
        ArchiveLimits limits;
        limits.entry_count = 1;
        ArchiveReader reader(p, limits);
        require(reader.open_error().find("entry count limit") != std::string::npos,
                "directories count against entry cap");
    });
    test("odbpp_review_ratio_per_file", [&] {
        ArchiveLimits limits;
        limits.ratio = 2;
        ArchiveReader reader(temp / "many-small.tgz", limits);
        require(reader.open_error().empty(), reader.open_error());
        require(reader.entries().size() == 100, "all small entries accepted");
        for (const auto& entry : reader.entries()) {
            std::string out, error;
            require(reader.read_bytes(entry.path, out, error) && out == std::string(1024, 'x'),
                    error);
        }
    });
    test("odbpp_review_matrix_units", [&] {
        auto p = temp / "matrix-units";
        minimal(p);
        for (const auto* units : {"MIL", "", "unknown"}) {
            write(p / "matrix/matrix", std::string("UNITS=") + units + "\n");
            rejected(p, "unsupported matrix units");
        }
    });
    test("odbpp_review_pad_angles_and_whitespace", [&] {
        auto p = temp / "pad-angles";
        minimal(p);
        for (int orientation : {8, 9}) {
            for (const auto* angle : {"", "bad"}) {
                write(p / "steps/pcb/layers/top/features",
                      "$0 r100\nP 0 0 0 P 0 " + std::to_string(orientation) + " " + angle + "\n");
                rejected(p, "malformed pad angle");
            }
            write(p / "steps/pcb/layers/top/features",
                  "\v\f\t \n$0 r100\nP 0 0 0 P 0 " + std::to_string(orientation) + " 12.5\n");
            auto j = job(p);
            const auto& pad = j.features.at("top").pads.at(0);
            require(pad.rot_deg == 12.5 && pad.mirror == (orientation == 9),
                    "explicit angle and mirror preserved");
        }
    });
    test("odbpp_gzip_final_input_boundaries", [&] {
        for (const auto size : {0, 32745, 32746, 65513}) {
            std::string decoded, error;
            require(decode_gzip(
                        bytes(temp / ("boundary-" + std::to_string(size) + ".gz")), decoded, error),
                    error);
            require(decoded == std::string(size, 'x'), "gzip boundary bytes");
        }
    });
    test("odbpp_zip_payload_accounting", [&] {
        ArchiveLimits limits;
        limits.file_bytes = 1;
        limits.total_bytes = 1;
        limits.entry_count = 1;
        for (const auto* name : {"ratio-unsupported.zip",
                                 "ratio-zip64.zip",
                                 "ratio-encrypted.zip",
                                 "directories.zip"}) {
            limits.ratio = std::string(name) == "directories.zip" ? 200 : 0.00001;
            // Encrypted job has many entries; isolate payload budgets from the entry cap.
            limits.entry_count = std::string(name) == "directories.zip" ? 1 : 200000;
            ArchiveReader reader(temp / name, limits);
            if (std::string(name) == "directories.zip") {
                require(reader.open_error().find("entry count limit") != std::string::npos,
                        "zip directories count against entry cap");
            } else {
                require(reader.open_error().empty(), reader.open_error());
                require(reported(reader.unsupported(), "matrix/matrix"), "unsupported retained");
            }
        }
    });
    test("odbpp_malformed_numeric_fields", [&] {
        auto p = temp / "malformed-numbers";
        for (const auto* value : {"", "bad", "999999999999999999999999", "1junk"}) {
            minimal(p);
            write(p / "steps/pcb/layers/top/features", "$" + std::string(value) + " r100\n");
            rejected(p, "malformed symbol index steps/pcb/layers/top/features");
            write(p / "steps/pcb/layers/top/features", "");
            for (const auto* key : {"NUM", "FINISH_SIZE"}) {
                write(p / "steps/pcb/layers/top/tools",
                      "TOOLS {\n" + std::string(key) + "=" + value + "\n}\n");
                // Large finite values are legal doubles, but out of range for integer NUM.
                if (std::string(key) == "FINISH_SIZE" && std::string(value).starts_with("999"))
                    continue;
                rejected(p, "malformed tool " + std::string(key) + " steps/pcb/layers/top/tools");
            }
        }
        for (const auto* value : {"nan", "inf", "1e999"}) {
            minimal(p);
            write(p / "steps/pcb/layers/top/tools",
                  "TOOLS {\nFINISH_SIZE=" + std::string(value) + "\n}\n");
            rejected(p, "malformed tool FINISH_SIZE");
        }
        minimal(p);
        write(p / "matrix/matrix", "LAYER {\nNAME=top\nROW=1junk\n}\n");
        rejected(p, "malformed matrix ROW matrix/matrix");
    });
    test("odbpp_step_files_are_not_steps", [&] {
        auto p = temp / "step-files";
        write(p / "matrix/matrix", "UNITS=MM\nLAYER {\nNAME=top\n}\n");
        write(p / "steps/readme.txt", "text");
        write(p / "steps/matrix", "text");
        rejected(p, "no step");
    });
    test("odbpp_review_unreadable_subdirectory", [&] {
#ifdef __linux__
        const auto p = temp / "permissions";
        minimal(p);
        write(p / "blocked/payload", "unreadable");
        fs::permissions(p / "blocked", fs::perms::none);
        // A child drops root privileges when necessary so the permission failure
        // is exercised even in root-run CI, without changing the test process.
        const auto child = fork();
        require(child >= 0, "fork permission probe");
        if (child == 0) {
            if (getuid() == 0 && (setgid(65534) != 0 || setuid(65534) != 0))
                _exit(2);
            ArchiveReader reader(p);
            std::string out, error;
            const bool ok =
                reader.open_error().starts_with("IMPORT_REJECTED: cannot read directory") &&
                !reader.read_bytes("matrix/matrix", out, error) && error == reader.open_error();
            _exit(ok ? 0 : 1);
        }
        int status = 0;
        const auto waited = waitpid(child, &status, 0);
        fs::permissions(p / "blocked", fs::perms::owner_all);
        require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "unreadable subtree rejects the input");
#endif
    });
    test("odbpp_review_gzip_flags_and_final_buffer", [&] {
        for (int flag : {2, 4, 8, 16}) {
            std::string out, error;
            require(decode_gzip(bytes(temp / ("flag-" + std::to_string(flag) + ".gz")), out, error),
                    error);
            require(out == "flag payload", "individual gzip optional header flag");
        }
        for (const auto size : {0, 32745, 32746, 65513}) {
            const auto data = bytes(temp / ("boundary-" + std::to_string(size) + ".gz"));
            for (const auto limit : {1, 17, 32768}) {
                std::istringstream input(data + bytes(temp / "flag-2.gz"));
                GzipCursor cursor(input);
                std::string_view block;
                std::string out, error;
                while (cursor.next(block, error, limit)) {
                    require(block.size() <= static_cast<std::size_t>(limit), "bounded output");
                    out.append(block);
                }
                require(error.empty() && out == std::string(size, 'x') + "flag payload",
                        "buffered member boundary: " + error);
            }
        }
    });
    test("odbpp_review_surface_topology", [&] {
        const auto p = temp / "topology";
        const auto square = [](int x, int y, int size, char kind) {
            return "OB " + std::to_string(x) + " " + std::to_string(y) + " " + kind + "\nOS " +
                   std::to_string(x + size) + " " + std::to_string(y) + "\nOS " +
                   std::to_string(x + size) + " " + std::to_string(y + size) + "\nOS " +
                   std::to_string(x) + " " + std::to_string(y + size) + "\nOS " +
                   std::to_string(x) + " " + std::to_string(y) + "\nOE\n";
        };
        const auto outer = square(0, 0, 10, 'I');
        const auto hole = square(2, 2, 2, 'H');
        for (bool feature : {false, true}) {
            minimal(p);
            const auto path = p / (feature ? "steps/pcb/layers/top/features" : "steps/pcb/profile");
            const auto put = [&](const std::string& rings) {
                write(path, "S P\n" + rings + "SE\n");
            };
            for (const auto& rings : {outer,
                                      outer + hole,
                                      hole + outer,
                                      outer + square(20, 0, 10, 'I'),
                                      std::string("OB 10 0 I\nOC 10 0 0 0 N\nOE\n") + hole}) {
                put(rings);
                require(bool(parse_job(p)), "valid line/arc topology");
            }
            for (const auto& rings :
                 {hole,
                  outer + square(20, 20, 2, 'H'),
                  outer + square(0, 2, 2, 'H'),
                  outer + square(9, 2, 2, 'H'),
                  outer + square(2, 2, 2, 'I'),
                  outer + hole + square(3, 3, 2, 'H'),
                  outer + square(1, 1, 6, 'H') + hole,
                  outer + outer,
                  std::string("OB 0 0 I\nOS 4 4\nOS 0 4\nOS 3 0\nOS 0 0\nOE\n")}) {
                put(rings);
                rejected(p, "");
            }
            put("OB 10 0 I\nOC 10 0 0 0 N\nOE\nOB 11 0 H\nOC 11 0 9 0 N\nOE\n");
            rejected(p, "topology");
            put("OB 10 0 I\nOC 10 0 0 0 N\nOE\nOB 2 0 H\nOC 2 0 0 0 Y\nOE\n");
            require(bool(parse_job(p)), "concentric circular hole");
        }
    });
    test("odbpp_review_paths_and_tar_magic", [&] {
        require(archive(temp / "space-ustar.tgz").at("prefix/" + std::string(99, 'f')) ==
                    "space magic",
                "space ustar prefix retained");
        for (const auto* ext : {".tgz", ".zip"}) {
            require(archive(temp / (std::string("path-colon") + ext)).at("part:rev/file") == "x",
                    "POSIX colon path accepted");
            for (const auto* kind : {"absolute", "drive", "relative-drive", "traversal"}) {
                ArchiveReader reader(temp / (std::string("path-") + kind + ext));
                require(reader.open_error().starts_with("IMPORT_REJECTED: invalid archive path"),
                        reader.open_error());
            }
        }
        const auto p = temp / "colon-dir";
        write(p / "part:rev/file", "x");
        require(archive(p).at("part:rev/file") == "x", "directory colon path accepted");
    });
    test("odbpp_review_caps_all_containers", [&] {
        for (const auto* container : {"limits.tgz", "caps.zip", "caps-dir"}) {
            for (const auto* reason : {"file", "total", "entry", "ratio"}) {
                if (std::string(container) == "caps-dir" && std::string(reason) == "ratio")
                    continue; // Uncompressed directory input has no compression ratio.
                ArchiveLimits limits;
                if (std::string(reason) == "file")
                    limits.file_bytes = 4096;
                if (std::string(reason) == "total")
                    limits.total_bytes = 10000;
                if (std::string(reason) == "entry")
                    limits.entry_count = 1;
                if (std::string(reason) == "ratio")
                    limits.ratio = 0.5;
                ArchiveReader reader(temp / container, limits);
                std::string error = reader.open_error();
                if (error.empty()) {
                    for (const auto& e : reader.entries()) {
                        if (e.is_dir)
                            continue;
                        std::string out;
                        if (!reader.read_bytes(e.path, out, error))
                            break;
                    }
                }
                require(error.starts_with("IMPORT_REJECTED:") &&
                            error.find(reason) != std::string::npos,
                        std::string(container) + " " + reason + ": " + error);
            }
        }
    });
    test("odbpp_directory_selected_payload_limits", [&] {
        const auto p = temp / "selected-limits";
        write(p / "selected", "abc");
        write(p / "unrelated", std::string(100, 'x'));
        ArchiveLimits limits;
        limits.file_bytes = 3;
        limits.total_bytes = 3;
        ArchiveReader reader(p, limits);
        require(reader.open_error().empty(), reader.open_error());
        std::string out, error;
        require(reader.read_bytes("selected", out, error) && out == "abc", error);
        require(!reader.read_bytes("unrelated", out, error) &&
                    error.starts_with("IMPORT_REJECTED: file byte limit"),
                error);
        limits.file_bytes = 100;
        ArchiveReader total_reader(p, limits);
        require(total_reader.open_error().empty(), total_reader.open_error());
        require(total_reader.read_bytes("selected", out, error), error);
        require(!total_reader.read_bytes("unrelated", out, error) &&
                    error.starts_with("IMPORT_REJECTED: total byte limit"),
                error);
    });
    test("odbpp_tar_selected_content_total", [&] {
        ArchiveLimits limits;
        limits.total_bytes = 3;
        limits.file_bytes = 3;
        ArchiveReader reader(temp / "large-irrelevant.tgz", limits);
        require(reader.open_error().empty(), reader.open_error());
        std::string out, error;
        require(reader.read_bytes("matrix/matrix", out, error) && out == "abc", error);
        require(!reader.read_bytes("steps/pcb/profile", out, error) &&
                    error.starts_with("IMPORT_REJECTED: total byte limit"),
                error);
    });
    test("odbpp_gzip_stream_exception", [&] {
        std::istringstream input("truncated");
        input.exceptions(std::ios::failbit | std::ios::badbit);
        GzipCursor cursor(input);
        std::string_view block;
        std::string error;
        require(!cursor.next(block, error) &&
                    error.starts_with("IMPORT_REJECTED: gzip input failure"),
                error);
        require(!cursor.next(block, error) &&
                    error.starts_with("IMPORT_REJECTED: gzip input failure"),
                "failed cursor remains exhausted");
        std::istringstream stream("truncated");
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        require(!stream_gzip(
                    stream, [](std::string_view) { return true; }, error) &&
                    error.starts_with("IMPORT_REJECTED: gzip input failure"),
                error);
    });
    test("odbpp_streaming_memory", [&] {
#ifdef __linux__
        const auto p = temp / "stream";
        minimal(p);
        const auto features = p / "steps/pcb/layers/top/features";
        fs::create_directories(features.parent_path());
        {
            std::ofstream out(features);
            out << "UNITS=MM\n";
            // Valid header/comment lines isolate input buffering from retained geometry memory.
            for (int i = 0; i < 2000000; ++i)
                out << "# streaming feature comment 0123456789012345678901234567890123456789\n";
            require(out.good(), "large fixture write");
        }
        std::cout.flush();
        std::cerr.flush();
        const auto child = fork();
        require(child >= 0, "fork memory measurement");
        if (child == 0) {
            execl("/proc/self/exe",
                  "test_odbpp_parser",
                  "--streaming-memory",
                  p.c_str(),
                  static_cast<char*>(nullptr));
            _exit(127);
        }
        int status = 0;
        require(waitpid(child, &status, 0) == child, "wait for memory measurement");
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "fresh-process streaming memory measurement failed");
#else
        std::cout << "SKIP odbpp_streaming_memory: Linux ru_maxrss required\n";
#endif
    });
    fs::remove_all(temp);
    std::cout << failed << " acceptance cases failed\n";
    return failed ? 1 : 0;
}
