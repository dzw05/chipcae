#include "../../geometry/ecad/src/odbpp/archive.hpp"
#include "chipcae/geometry/ecad/ipc2581_zip.hpp"

#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
namespace fs = std::filesystem;
namespace ecad = chipcae::geometry::ecad;
static void require(bool ok, const std::string& message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Temporary {
    fs::path dir;
    Temporary() {
        std::random_device r;
        do {
            dir = fs::temp_directory_path() / ("ipc-zip-" + std::to_string(r()));
        } while (!fs::create_directory(dir));
    }
    ~Temporary() { fs::remove_all(dir); }
};
static std::string bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    require(bool(in), "cannot read " + p.string());
    return {std::istreambuf_iterator<char>(in), {}};
}
static void write(const fs::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary);
    out << data;
    require(bool(out), "cannot write " + p.string());
}
// Arguments go through exec, never through shell quoting. Python's standard library
// generates fixtures at runtime; it is not an importer dependency.
static void zip(const fs::path& output,
                const std::vector<std::pair<std::string, fs::path>>& entries,
                bool deflate = false) {
    std::vector<std::string> args = {
        CHIPCAE_ZIP_PYTHON,
        "-c",
        "import sys,zipfile; z=zipfile.ZipFile(sys.argv[1],'w',compression=int(sys.argv[2])); "
        "[z.writestr(sys.argv[i],open(sys.argv[i+1],'rb').read()) for i in "
        "range(3,len(sys.argv),2)]; z.close()",
        output.string(),
        deflate ? "8" : "0"};
    for (const auto& [name, path] : entries) {
        args.push_back(name);
        args.push_back(path.string());
    }
    std::vector<char*> argv;
    for (auto& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);
    auto pid = fork();
    require(pid >= 0, "fork zip generator");
    if (!pid) {
        execv(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    require(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "Python ZIP generation failed");
}
static std::string dump_model_for_golden(const ecad::EcadModel& m) {
    std::ostringstream o;
    auto d = [&](double v) { o << std::bit_cast<std::uint64_t>(v) << '\n'; };
    auto str = [&](const std::string& v) { o << std::quoted(v) << '\n'; };
    auto pos = [&](const auto& p) { o << p[0] << '\n' << p[1] << '\n'; };
    auto poly = [&](const ecad::Polygon& p) {
        o << p.points.size() << '\n';
        for (auto& v : p.points)
            pos(v);
    };
    str(m.id);
    d(m.dbu_m);
    o << m.layers.size() << '\n';
    for (auto& l : m.layers) {
        str(l.name);
        str(l.material);
        str(l.function);
        d(l.thickness_m);
        o << l.copper << '\n' << l.polygons.size() << '\n';
        for (auto& p : l.polygons)
            poly(p);
    }
    poly(m.outline);
    o << m.report.unsupported.size() << '\n';
    for (auto& u : m.report.unsupported) {
        str(u.code);
        str(u.element);
        o << u.line << '\n';
    }
    o << m.vias.size() << '\n';
    for (auto& v : m.vias) {
        str(v.id);
        str(v.padstack);
        str(v.start_layer);
        str(v.end_layer);
        pos(v.position);
        d(v.outer_radius_dbu);
        d(v.inner_radius_dbu);
        o << v.dielectric_layers.size() << '\n';
        for (auto i : v.dielectric_layers)
            o << i << '\n';
    }
    o << m.components.size() << '\n';
    for (auto& c : m.components) {
        str(c.id);
        str(c.package);
        pos(c.position);
        d(c.rotation_degrees);
        o << c.mirror << '\n';
        poly(c.outline);
        o << c.pads.size() << '\n';
        for (auto& p : c.pads) {
            str(p.id);
            pos(p.position);
        }
    }
    o << m.via_fractions.size() << '\n';
    for (auto& f : m.via_fractions) {
        o << f.layer << '\n';
        for (auto v : f.lower)
            d(v);
        for (auto v : f.upper)
            d(v);
        d(f.fraction);
        o << f.via_count << '\n';
    }
    return o.str();
}
// All public fields, including reports and ordered collections; no exclusions.
static void equal(const ecad::EcadModel& a, const ecad::EcadModel& b) {
    require(dump_model_for_golden(a) == dump_model_for_golden(b),
            "full ordered model differs (double bits)");
}
static void rejected(const ecad::ImportResult& r, const std::vector<std::string>& words) {
    require(!r && r.error.starts_with("IMPORT_REJECTED:"), "expected IMPORT_REJECTED: " + r.error);
    for (const auto& word : words)
        require(r.error.find(word) != std::string::npos,
                "missing diagnostic [" + word + "]: " + r.error);
}
static void
equivalent(const fs::path& xml, const fs::path& archive, const fs::path& dir, bool outputs) {
    auto direct = ecad::read_ipc2581(xml);
    require(bool(direct), "direct XML: " + direct.error);
    auto zipped = ecad::read_ipc2581_zip(archive);
    require(bool(zipped), "ZIP import should succeed: " + zipped.error);
    equal(direct.model, zipped.model);
    auto again = ecad::read_ipc2581_zip(archive);
    require(bool(again), "second ZIP read: " + again.error);
    equal(zipped.model, again.model);
    if (!outputs)
        return;
    write(dir / "map.json", R"({"FR4":"mat.fr4","copper":"mat.cu"})");
    std::string error;
    require(ecad::apply_material_map(direct.model, dir / "map.json", &error), error);
    require(ecad::apply_material_map(zipped.model, dir / "map.json", &error), error);
    equal(direct.model, zipped.model);
    for (const auto& name : {"direct", "zipped"}) {
        auto out = dir / name;
        fs::create_directories(out);
        require(ecad::to_stack(std::string(name) == "direct" ? direct.model : zipped.model,
                               out / "stack.json",
                               out / "board.polyset",
                               &error),
                error);
    }
    for (const auto& name : {"stack.json", "board.polyset"})
        require(bytes(dir / "direct" / name) == bytes(dir / "zipped" / name),
                std::string(name) + " bytes differ");
}
static long peak(const fs::path& file, bool zipped) {
    auto pid = fork();
    require(pid >= 0, "fork RSS worker");
    if (!pid) {
        auto r = zipped ? ecad::read_ipc2581_zip(file) : ecad::read_ipc2581(file);
        if (!r)
            std::cerr << "RSS import failure: " << r.error << std::endl;
        _exit(r ? 0 : 1);
    }
    int status = 0;
    rusage usage{};
    require(wait4(pid, &status, 0, &usage) == pid, "wait4 RSS worker");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "RSS worker import must succeed");
    return usage.ru_maxrss;
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected test case");
        std::string test = argv[1];
        Temporary temp;
        auto dir = temp.dir;
        auto fixture =
            fs::path(CHIPCAE_TEST_SOURCE_DIR) / "tests/integration/ecad/data/board_4layer.xml";
        auto xml = dir / "input.xml", archive = dir / "input.zip";
        if (test == "ipc_zip_equals_xml") {
            for (bool deflate : {false, true}) {
                zip(archive, {{"board_4layer.xml", fixture}}, deflate);
                equivalent(fixture, archive, dir, true);
            }
        } else if (test == "ipc_zip_line_endings") {
            auto original = bytes(fixture);
            for (bool crlf : {true, false}) {
                std::string modified;
                if (crlf) {
                    for (char c : original) {
                        if (c == '\n')
                            modified += '\r';
                        modified += c;
                    }
                } else {
                    modified = original;
                    while (!modified.empty() &&
                           (modified.back() == '\n' || modified.back() == '\r'))
                        modified.pop_back();
                }
                write(xml, modified);
                zip(archive, {{"board.xml", xml}}, true);
                equivalent(xml, archive, dir, false);
            }
        } else if (test == "ipc_zip_no_xml") {
            zip(archive, {});
            rejected(ecad::read_ipc2581_zip(archive), {"no .xml entry"});
            write(xml, "");
            zip(archive, {{"folder/", xml}});
            rejected(ecad::read_ipc2581_zip(archive), {"no .xml entry"});
            zip(archive, {{"folder/", xml}, {"__MACOSX/x.xml", fixture}, {"._a.xml", fixture}});
            rejected(ecad::read_ipc2581_zip(archive), {"no .xml entry"});
            zip(archive,
                {{"__MACOSX/x.xml", fixture}, {"._a.xml", fixture}, {"a.xml.bak", fixture}});
            rejected(ecad::read_ipc2581_zip(archive),
                     {"no .xml entry", "__MACOSX/x.xml", "._a.xml", "a.xml.bak"});
            std::vector<std::pair<std::string, fs::path>> entries;
            for (int i = 0; i < 25; ++i)
                entries.emplace_back("raw-" + std::to_string(i) + ".txt", fixture);
            zip(archive, entries);
            auto r = ecad::read_ipc2581_zip(archive);
            rejected(r, {"no .xml entry", "25"});
            std::size_t previous = 0;
            for (int i = 0; i < 20; ++i) {
                auto pos = r.error.find(entries[i].first);
                require(pos != std::string::npos && pos >= previous, "entry listing order");
                previous = pos;
            }
            require(r.error.find(entries[20].first) == std::string::npos, "listing capped at 20");
        } else if (test == "ipc_zip_two_xml") {
            zip(archive, {{"out/first.XML", fixture}, {"second.Xml", fixture}});
            auto r = ecad::read_ipc2581_zip(archive);
            rejected(r, {"out/first.XML", "second.Xml"});
            require(r.error.find("out/first.XML") < r.error.find("second.Xml"),
                    "matching archive order");
        } else if (test == "ipc_zip_filtered_entries") {
            write(xml, "");
            zip(archive,
                {{"folder/", xml},
                 {"__MACOSX/x.xml", fixture},
                 {"nested/._a.xml", fixture},
                 {"a.xml.bak", fixture},
                 {"a.xmlx", fixture},
                 {"matrix/matrix", fixture},
                 {"out/A.XML", fixture}});
            equivalent(fixture, archive, dir, false);
        } else if (test == "ipc_zip_limits") {
            write(xml, std::string(1024 * 1024, 'x'));
            zip(archive, {{"bomb.xml", xml}}, true);
            for (bool path : {false, true}) {
                if (path)
                    zip(archive, {{"../board.xml", fixture}});
                ecad::detail::ArchiveReader reader(archive);
                auto expected = reader.open_error();
                if (expected.empty())
                    reader.for_each_line(
                        "bomb.xml", [](std::string_view) { return true; }, expected);
                require(expected.starts_with("IMPORT_REJECTED:"),
                        "ArchiveReader must reject test fixture");
                rejected(ecad::read_ipc2581_zip(archive), {path ? "path" : "ratio"});
                require(ecad::read_ipc2581_zip(archive).error == expected,
                        "ArchiveReader rejection must propagate verbatim");
            }
        } else if (test == "ipc_zip_bad_xml") {
            for (bool deflate : {false, true}) {
                for (const auto& data : {std::string(),
                                         bytes(fixture).substr(0, bytes(fixture).size() / 2),
                                         std::string("<IPC-2581>\n</wrong>\n"),
                                         std::string("<IPC-2581>\n</wrong>")}) {
                    write(xml, data);
                    zip(archive, {{"broken.xml", xml}}, deflate);
                    auto a = ecad::read_ipc2581(xml), b = ecad::read_ipc2581_zip(archive);
                    require(!a && !b, "both malformed XML imports must fail");
                    rejected(b, {});
                    require(a.error == b.error,
                            "XML diagnostics differ: direct=" + a.error + " ZIP=" + b.error);
                }
            }
            // Valid XML with an incorrect CRC must retain the archive diagnostic,
            // even though finalizing the partially delivered XML can also fail.
            zip(archive, {{"board.xml", fixture}});
            auto data = bytes(archive);
            auto central = data.find(std::string("PK\1\2", 4));
            require(central != std::string::npos, "ZIP central directory");
            data[14] ^= 1;
            data[central + 16] ^= 1;
            write(archive, data);
            ecad::detail::ArchiveReader reader(archive);
            std::string expected;
            require(!reader.for_each_line(
                        "board.xml", [](std::string_view) { return true; }, expected),
                    "incorrect CRC must fail the archive read");
            require(!expected.empty(), "CRC failure must provide an archive diagnostic");
            require(ecad::read_ipc2581_zip(archive).error == expected,
                    "archive read error must propagate verbatim");
        } else if (test == "ipc_zip_unsupported") {
            zip(archive, {{"unsupported.xml", fixture}});
            auto data = bytes(archive);
            auto central = data.find(std::string("PK\1\2", 4));
            require(central != std::string::npos, "ZIP central directory");
            // Flag both headers encrypted. No encryption/decompression implementation.
            data[6] = static_cast<char>(data[6] | 1);
            data[central + 8] = static_cast<char>(data[central + 8] | 1);
            write(archive, data);
            ecad::detail::ArchiveReader reader(archive);
            require(reader.open_error().empty() && !reader.is_supported("unsupported.xml"),
                    "ArchiveReader must identify unsupported encrypted member");
            std::string unused, expected;
            reader.for_each_line(
                "unsupported.xml", [](std::string_view) { return true; }, expected);
            require(expected.find("zip encryption") != std::string::npos,
                    "ArchiveReader encrypted reason");
            rejected(ecad::read_ipc2581_zip(archive), {"unsupported.xml", "zip encryption"});
        } else if (test == "ipc_zip_invalid_input") {
            rejected(ecad::read_ipc2581_zip(fixture), {});
            rejected(ecad::read_ipc2581_zip(dir), {});
            zip(archive, {});
            auto empty_zip = bytes(archive);
            zip(archive, {{"folder/", fixture}});
            auto damaged_directory_zip = bytes(archive);
            damaged_directory_zip[0] = 'X';
            for (const auto& data : {std::string(),
                                     std::string("not a ZIP"),
                                     std::string("PK truncated"),
                                     empty_zip.substr(0, empty_zip.size() - 1),
                                     damaged_directory_zip}) {
                write(archive, data);
                ecad::detail::ArchiveReader reader(archive);
                require(!reader.open_error().empty(), "invalid ZIP fixture");
                require(ecad::read_ipc2581_zip(archive).error == reader.open_error(),
                        "invalid ZIP error must propagate verbatim");
            }
        } else if (test == "ipc_zip_streaming") {
            auto data = bytes(fixture);
            auto end = data.rfind("</IPC-2581>");
            require(end != std::string::npos, "fixture root closing tag");
            // Repeat fixture geometry outside LayerFeature to exercise SAX parsing
            // without quadratic unions of coincident copper.
            auto begin = data.find("<Line "), close = data.find("</Line>", begin);
            require(begin != std::string::npos && close != std::string::npos,
                    "fixture line geometry");
            // Whitespace keeps retained parser geometry small compared with input,
            // so buffering a 200 MiB entry cannot hide under model memory.
            auto line = "<Set>" + data.substr(begin, close + 7 - begin) + "</Set>" +
                        std::string(4096, ' ') + "\n";
            {
                std::ofstream out(xml, std::ios::binary);
                out << data.substr(0, end);
                for (std::size_t n = 0; n < 200ULL * 1024 * 1024; n += line.size())
                    out << line;
                out << data.substr(end);
                require(bool(out), "write heavy XML");
            }
            zip(archive, {{"large.xml", xml}}); // stored avoids ratio-limit interference
            long direct = peak(xml, false);
            std::cout << "measured direct_peak_kib=" << direct << std::endl;
            long zipped = peak(archive, true);
            double ratio = static_cast<double>(zipped) / static_cast<double>(direct);
            std::cout << "measured direct_peak_kib=" << direct << " zip_peak_kib=" << zipped
                      << " ratio=" << ratio << std::endl;
            require(ratio <= 1.2, "ZIP/direct peak RSS ratio exceeds 1.2");
        } else
            throw std::runtime_error("unknown test case");
        std::cout << "PASS " << test << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << argv[1] << ": " << e.what() << '\n';
        return 1;
    }
}
