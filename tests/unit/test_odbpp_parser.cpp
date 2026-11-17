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
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <tuple>
#ifdef __linux__
#include <sys/resource.h>
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
void rejected(const fs::path& path, const std::string& reason) {
    auto result = read_odbpp(path);
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
// Length-delimited, round-trip double serialization visits every frozen field, preserving
// vector order and map keys. This avoids adding equality operators to the frozen model.
struct Snapshot {
    std::ostringstream out;
    Snapshot() { out.precision(17); }
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
    std::string take(const OdbJob& j) {
        add(static_cast<int>(j.units));
        add(j.step);
        add(j.layers);
        add(j.attrs.job);
        add(j.attrs.layer);
        add(j.profile.contours);
        add(j.features);
        add(j.tools);
        return out.str();
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
} // namespace
int main() {
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
    test("odbpp_matrix_order", [&] {
        auto j = job(temp / "job");
        // 实测: 31 blocks in matrix order; KiCad copper table F.Cu/In1.Cu/In2.Cu/B.Cu.
        const std::vector<std::string> names = {"comp_+_top",
                                                "f.silkscreen",
                                                "f.paste",
                                                "f.mask",
                                                "f.cu",
                                                "dielectric_1",
                                                "in1.cu",
                                                "dielectric_2",
                                                "in2.cu",
                                                "dielectric_3",
                                                "b.cu",
                                                "b.mask",
                                                "b.paste",
                                                "b.silkscreen",
                                                "f.adhesive",
                                                "b.adhesive",
                                                "user.drawings",
                                                "user.comments",
                                                "user.eco1",
                                                "user.eco2",
                                                "edge.cuts",
                                                "margin",
                                                "b.courtyard",
                                                "f.courtyard",
                                                "b.fab",
                                                "f.fab",
                                                "user.1",
                                                "user.2",
                                                "user.3",
                                                "user.4",
                                                "drill_plated_f.cu-b.cu"};
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
            {"f.cu", {2, 0, 1, 1}},
            {"in1.cu", {2, 0, 0, 1}},
            {"in2.cu", {2, 0, 0, 1}},
            {"b.cu", {2, 0, 0, 1}},
            {"drill_plated_f.cu-b.cu", {2, 0, 0, 0}},
            {"edge.cuts", {0, 4, 0, 0}},
            {"f.silkscreen", {0, 10, 0, 0}}};
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
        auto r = read_odbpp(temp / "duplicate.tgz");
        require(r.error.starts_with("IMPORT_REJECTED: duplicate archive entry matrix/matrix"),
                r.error);
    });
    test("odbpp_duplicate_zip", [&] {
        auto r = read_odbpp(temp / "duplicate.zip");
        require(r.error.starts_with("IMPORT_REJECTED: duplicate archive entry matrix/matrix"),
                r.error);
    });
    test("odbpp_archive_normalized", [&] {
        const auto files = archive(temp / "normalized.tgz");
        require(files.size() == 1 && files.at("matrix/matrix") == "normalized",
                "canonical separators and leading ./");
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
    test("odbpp_tgz_zip_dir_equal", [&] {
        auto expected = Snapshot().take(job(temp / "job"));
        for (const auto* name : {"job.tgz", "job.zip", "stored.zip", "JOB.TAR.GZ", "JOB.ZIP"})
            require(Snapshot().take(job(temp / name)) == expected,
                    std::string("all frozen fields: ") + name);
    });
    test("odbpp_archive_tar_formats", [&] {
        const auto expected = archive(temp / "ustar.tgz");
        require(expected.size() == 2, "regular files only");
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
    test("odbpp_archive_gzip_bad_crc", [&] {
        std::string decoded, error;
        const bool ok = decode_gzip(bytes(temp / "corrupt/crc.tgz"), decoded, error);
        require(!ok && error.starts_with("IMPORT_REJECTED:") &&
                    error.find("CRC") != std::string::npos,
                "expected gzip CRC rejection, got " + error);
    });
    test("odbpp_archive_lzw", [&] {
        auto result = read_odbpp(temp / "job.tar.Z");
        if (reported(result.model.report.unsupported, (temp / "job.tar.Z").string()))
            return;
        require(bool(result), result.error);
        std::string decoded, error;
        require(decode_lzw(bytes(temp / "job.tar.Z"), decoded, error), error);
        require(decoded == bytes(temp / "job.tar"), "every decoded .Z byte");
        require(archive(temp / "job.tar.Z") == archive(temp / "job.tgz"), ".Z decoded bytes");
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
            ArchiveReader reader(temp / "limits.tgz", limits);
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
        auto result = read_odbpp(temp / "corrupt/unknown");
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
        require(bool(read_odbpp(p)), "empty features allowed");
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
                require(bool(read_odbpp(p)), "per-axis 1e-9 meter tolerance");
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
        for (const auto* name : {"unsupported.zip", "zip64.zip"}) {
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
        auto r = read_odbpp(p);
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
        const auto r = read_odbpp(p);
        require(bool(r), r.error);
        const auto& u = r.model.report.unsupported;
        require(reported(u, "features: unit mismatch", 1) &&
                    reported(u, "unknown symbol vendor_shape", 2) && reported(u, "record T", 3) &&
                    reported(u, "record B", 4) && reported(u, "unknown symbol 7", 5),
                "diagnostic file and exact line for each unsupported element");
    });
    test("odbpp_directory_symlink", [&] {
        auto p = temp / "directory-links";
        minimal(p);
        std::error_code ec;
        fs::create_directory_symlink(temp / "job", p / "external", ec);
        if (ec)
            return; // Platforms without symlink privileges still exercise tar links.
        ArchiveReader reader(p);
        require(reader.open_error().empty(), reader.open_error());
        require(reported(reader.unsupported(), "external"), "directory symlink reported");
        require(std::none_of(reader.entries().begin(),
                             reader.entries().end(),
                             [](const auto& e) { return e.path.starts_with("external"); }),
                "directory traversal does not follow symlink");
    });
    test("odbpp_symbol_order_and_nested_Z", [&] {
        auto p = temp / "symbol-order";
        minimal(p);
        write(p / "steps/pcb/layers/top/features", "$3 r100\n$1 oval100x200\n");
        write(p / "steps/pcb/layers/top/tools.Z", "unsupported compressed file");
        auto j = job(p);
        const auto& symbols = j.features.at("top").symbols;
        require(symbols.size() == 2 && symbols[0].name == "r100" &&
                    symbols[1].name == "oval100x200",
                "symbol definition appearance order");
        const auto r = read_odbpp(p);
        require(reported(r.model.report.unsupported, "tools.Z"), "nested .Z fallback source path");
        require(reported(r.model.report.unsupported, "nonsequential symbol index 3", 1),
                "unrepresentable symbol index reported");
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
        rusage before{}, after{};
        require(getrusage(RUSAGE_SELF, &before) == 0, "getrusage before");
        const auto start = std::chrono::steady_clock::now();
        auto result = read_odbpp(p);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        require(getrusage(RUSAGE_SELF, &after) == 0, "getrusage after");
        require(bool(result), result.error);
        std::cout << "MEASURE odbpp_streaming_memory RSS_delta_KiB="
                  << after.ru_maxrss - before.ru_maxrss
                  << " elapsed_s=" << std::chrono::duration<double>(elapsed).count() << '\n';
        require(after.ru_maxrss - before.ru_maxrss < 64 * 1024, "RSS delta must be <64 MiB");
        require(elapsed < std::chrono::seconds(60), "streaming wall clock must be <60s");
#else
        std::cout << "SKIP odbpp_streaming_memory: Linux ru_maxrss required\n";
#endif
    });
    fs::remove_all(temp);
    std::cout << failed << " acceptance cases failed\n";
    return failed ? 1 : 0;
}
