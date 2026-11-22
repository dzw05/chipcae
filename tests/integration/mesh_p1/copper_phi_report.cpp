#include "../../../preview/src/mesh_digest.hpp"
#include "chipcae/mesh/background/column_hierarchy.hpp"
#include "chipcae/mesh/background/tensor_grid.hpp"
#ifdef CHIPCAE_TEST_JSONCONS
#include "chipcae/mesh/testing/mesh_fixture.hpp"

#include <jsoncons/json.hpp>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

using namespace chipcae::mesh;
using chipcae::Real;
namespace {
using Area = HexMeshArrays::GridReport::CopperArea;
constexpr Real dbu = 1e-6;
constexpr Real side = 1024 * dbu;
constexpr Real tight = 1e-12;
void require(bool ok, const std::string& reason) {
    if (!ok)
        throw std::runtime_error(reason);
}
Real relative_error(Real actual, Real reference) {
    require(std::isfinite(actual) && std::isfinite(reference), "area values must be finite");
    return std::abs(actual - reference) / std::max(std::abs(reference), Real(1e-30));
}
void close(Real actual, Real reference, Real tolerance, const std::string& reason) {
    const auto error = relative_error(actual, reference);
    std::ostringstream message;
    message << std::setprecision(17) << reason << ": actual=" << actual
            << " reference=" << reference << " relative error=" << error
            << " tolerance=" << tolerance;
    require(error <= tolerance, message.str());
}
GridOptions options(Local n = 32) {
    GridOptions o;
    o.nx = o.ny = n;
    o.levels = 1;
    o.cell_budget = 1000000;
    return o;
}
Layer layer(const std::string& name) {
    Layer l;
    l.name = name;
    l.kind = "Patterned";
    l.material = "substrate";
    l.thickness = 64 * dbu;
    return l;
}
StackModel fraction_stack() {
    StackModel s;
    s.id = "synthetic-area-report";
    s.outline.box = {0, 0, side, side};
    for (const auto& name : {"Cu", "cu"}) {
        auto l = layer(name);
        l.copper = CopperSpec{"copper", name == std::string("Cu") ? .25 : .6, std::nullopt};
        s.layers.push_back(l);
    }
    return s;
}
HexMeshArrays build(const StackModel& s, const GridOptions& o, const Polyset* p = nullptr) {
    auto result = p ? build_tensor_grid(s, o, *p) : build_tensor_grid(s, o);
    require(bool(result),
            result ? "" : "grid setup: " + result.error().path + ": " + result.error().reason);
    return std::move(result.value());
}
Area report(const HexMeshArrays& m, const std::string& name) {
    const auto& areas = m.grid_report.copper_area_m2;
    const auto found = areas.find(name);
    require(found != areas.end(), "missing copper_area_m2 entry for stack layer " + name);
    require(std::isfinite(found->second.polygons) && std::isfinite(found->second.raster),
            "reported areas must be finite");
    return found->second;
}
Real background_area(const HexMeshArrays& m) {
    return (m.axes[0].back() - m.axes[0].front()) * (m.axes[1].back() - m.axes[1].front());
}
// Independent integration reads the global dense Morton columns, including padding.
Real integral(const HexMeshArrays& m, std::size_t stack_layer) {
    require(m.copper_layer_of_layer.at(stack_layer) >= 0, "copper layer mapping exists");
    const auto nx = m.axes[0].size() - 1, ny = m.axes[1].size() - 1;
    const auto offset = static_cast<std::size_t>(m.copper_layer_of_layer[stack_layer]) * nx * ny;
    Real sum = 0;
    for (std::size_t j = 0; j < ny; ++j)
        for (std::size_t i = 0; i < nx; ++i) {
            const auto dense = level0_dense(static_cast<std::uint32_t>(nx),
                                            static_cast<std::uint32_t>(ny),
                                            static_cast<std::uint32_t>(i),
                                            static_cast<std::uint32_t>(j));
            const auto phi = m.copper_phi.at(offset + dense);
            require(std::isfinite(phi) && phi >= 0 && phi <= 1, "finite bounded column phi");
            sum += phi * (m.axes[0][i + 1] - m.axes[0][i]) * (m.axes[1][j + 1] - m.axes[1][j]);
        }
    return sum;
}
void check_area(const HexMeshArrays& m,
                std::size_t li,
                const std::string& name,
                Real analytic,
                Real tolerance = tight) {
    close(integral(m, li), analytic, tolerance, "existing raster versus analytic area");
    const auto& a = report(m, name);
    close(a.polygons, analytic, tolerance, name + " polygon analytic area");
    close(a.raster, a.polygons, tolerance, name + " area conservation");
    close(a.raster, integral(m, li), tight, name + " reported dense Morton integral");
}
void fractions() {
    const auto s = fraction_stack();
    const auto m = build(s, options());
    require(m.copper_layer_of_layer == std::vector<Local>{0, 1}, "fraction layer mapping");
    close(integral(m, 0), .25 * background_area(m), tight, "existing fraction .25 phi");
    close(integral(m, 1), .6 * background_area(m), tight, "existing fraction .6 phi");
    require(m.grid_report.copper_area_m2.size() == 2, "two case-sensitive fraction report entries");
    check_area(m, 0, "Cu", .25 * background_area(m));
    check_area(m, 1, "cu", .6 * background_area(m));
}
void no_copper() {
    auto s = fraction_stack();
    for (auto& l : s.layers) {
        l.kind = "Explicit";
        l.copper.reset();
    }
    const auto m = build(s, options());
    require(m.copper_phi.empty(), "no copper phi without copper layers");
    require(m.grid_report.copper_area_m2.empty(), "no copper report without copper layers");
}
void zero_fraction() {
    auto s = fraction_stack();
    s.layers.resize(1);
    s.layers[0].copper->fraction = 0;
    const auto m = build(s, options());
    check_area(m, 0, "Cu", 0);
}
#ifdef CHIPCAE_ENABLE_ECAD
struct PolygonCase {
    StackModel stack;
    Polyset polyset;
    PolygonCase() {
        stack.id = "synthetic-polygons";
        stack.outline.box = {0, 0, side, side};
        polyset.dbu_m = dbu;
    }
    void add(const std::string& stack_name,
             const std::string& poly_name,
             const std::vector<std::vector<std::int64_t>>& rings) {
        auto l = layer(stack_name);
        l.copper =
            CopperSpec{"copper", std::nullopt, CopperPolygons{"synthetic.polyset", poly_name}};
        stack.layers.push_back(l);
        PolysetLayer p;
        p.name = poly_name;
        p.ring_offsets = {0};
        for (const auto& ring : rings) {
            require(ring.size() % 2 == 0, "synthetic ring coordinate pairs");
            p.coordinates.insert(p.coordinates.end(), ring.begin(), ring.end());
            p.ring_offsets.push_back(static_cast<std::int64_t>(p.coordinates.size() / 2));
        }
        polyset.layers.push_back(std::move(p));
    }
    HexMeshArrays mesh(const GridOptions& o = options()) const { return build(stack, o, &polyset); }
};
PolygonCase polygons(bool nonuniform) {
    PolygonCase c;
    // Outer CCW, hole CW; L is CCW. Independent references use rectangle arithmetic.
    c.add("Cu",
          "outer-with-hole",
          {{113, 127, 877, 127, 877, 863, 113, 863}, {307, 337, 307, 697, 653, 697, 653, 337}});
    c.add("cu", "ell", {{151, 173, 809, 173, 809, 359, 367, 359, 367, 821, 151, 821}});
    if (nonuniform)
        c.stack.layers[0].regions.push_back(
            {"axis-seed", "substrate", "", {{257 * dbu, 281 * dbu, 761 * dbu, 743 * dbu}, {}}});
    return c;
}
bool nonuniform(const std::vector<Real>& axis) {
    const Real first = axis[1] - axis[0];
    for (std::size_t i = 2; i < axis.size(); ++i)
        if (std::abs(axis[i] - axis[i - 1] - first) > first * 1e-8)
            return true;
    return false;
}
void multiring() {
    const auto c = polygons(true);
    const auto m = c.mesh();
    require(nonuniform(m.axes[0]) && nonuniform(m.axes[1]), "A requires genuinely nonuniform axes");
    // nx/ny are per-segment minimum counts; use the actual final axes everywhere.
    require(m.axes[0].size() >= 33 && m.axes[1].size() >= 33, "A requested nx=ny=32");
    for (std::size_t li = 0; li < c.polyset.layers.size(); ++li)
        for (std::size_t xy = 0; xy < c.polyset.layers[li].coordinates.size(); ++xy) {
            const auto coordinate = c.polyset.layers[li].coordinates[xy] * dbu;
            for (auto line : m.axes[xy % 2])
                require(std::abs(coordinate - line) > 1e-16,
                        "A polygon edges must miss column lines");
        }
    check_area(m, 0, "Cu", (764. * 736. - 346. * 360.) * dbu * dbu);
    check_area(m, 1, "cu", (658. * 186. + 216. * 462.) * dbu * dbu);
    require(m.grid_report.copper_area_m2.size() == 2,
            "case-sensitive stack names, no polyset keys");
    require(!m.grid_report.copper_area_m2.contains("outer-with-hole") &&
                !m.grid_report.copper_area_m2.contains("ell"),
            "report uses stack names");
}
void aligned() {
    PolygonCase c;
    c.add("aligned",
          "p",
          {{128, 128, 896, 128, 896, 896, 128, 896}, {320, 320, 320, 704, 704, 704, 704, 320}});
    const auto m = c.mesh();
    check_area(m, 0, "aligned", (768. * 768. - 384. * 384.) * dbu * dbu);
}
void overlapping() {
    PolygonCase c;
    c.add("union",
          "overlap",
          {{101, 113, 701, 113, 701, 613, 101, 613}, {401, 313, 901, 313, 901, 813, 401, 813}});
    const auto m = c.mesh();
    constexpr Real sum = (600. * 500. + 500. * 500.) * dbu * dbu;
    constexpr Real united = sum - 300. * 300. * dbu * dbu;
    check_area(m, 0, "union", united);
    require(relative_error(report(m, "union").polygons, sum) > tight,
            "NonZero union must not sum overlapping rings");
}
void triangle() {
    PolygonCase c;
    c.add("triangle", "p", {{117, 139, 889, 211, 263, 907}});
    const auto m = c.mesh(options(64));
    constexpr Real analytic = .5 * (772. * 768. - 72. * 146.) * dbu * dbu;
    const auto raster_error = relative_error(integral(m, 0), analytic);
    std::cout << "C measured existing raster analytic relative error=" << std::setprecision(17)
              << raster_error << '\n';
    const auto& a = report(m, "triangle");
    std::cout << "C measured conservation relative error=" << relative_error(a.raster, a.polygons)
              << '\n';
    check_area(m, 0, "triangle", analytic, 5e-11);
}
void degenerate() {
    for (const auto& ring : std::vector<std::vector<std::int64_t>>{
             {337, 139, 337, 503, 337, 907}, {113, 337, 503, 337, 907, 337}}) {
        PolygonCase c;
        c.add("zero", "p", {ring});
        check_area(c.mesh(), 0, "zero", 0);
    }
}
void empty_rings() {
    PolygonCase c;
    c.add("empty", "p", {{}});
    check_area(c.mesh(), 0, "empty", 0);
}
void outside() {
    PolygonCase c;
    c.add("outside", "p", {{-101, 113, 701, 113, 701, 613, -101, 613}});
    const auto m = c.mesh();
    constexpr Real clipped = 701. * 500. * dbu * dbu;
    close(integral(m, 0), clipped, tight, "existing clipped raster analytic area");
    const auto& a = report(m, "outside");
    close(a.polygons, 802. * 500. * dbu * dbu, tight, "unclipped polygon area");
    close(a.raster, clipped, tight, "clipped raster report");
    require(a.raster < a.polygons, "out-of-grid copper must retain raster < polygons");
    auto inside = c;
    inside.polyset.layers[0].coordinates = {101, 113, 701, 113, 701, 613, 101, 613};
    require(m.grid_report.warnings == inside.mesh().grid_report.warnings,
            "out-of-grid copper must not add warnings");
}
void fraction_priority() {
    auto c = polygons(false);
    for (auto& l : c.stack.layers)
        l.copper->fraction = .25;
    const auto m = c.mesh();
    for (std::size_t li = 0; li < c.stack.layers.size(); ++li)
        check_area(m, li, c.stack.layers[li].name, .25 * background_area(m));
}
#endif
void partitioned() {
    auto o = options();
#ifdef CHIPCAE_ENABLE_ECAD
    const auto c = polygons(true);
    const auto serial = c.mesh(o);
    o.ranks = 2;
    const auto parallel = c.mesh(o);
    const auto& s = c.stack;
#else
    const auto s = fraction_stack();
    const auto serial = build(s, o);
    o.ranks = 2;
    const auto parallel = build(s, o);
#endif
    for (std::size_t dim = 0; dim < 3; ++dim) {
        require(serial.axes[dim].size() == parallel.axes[dim].size(), "E same axis lengths");
        for (std::size_t i = 0; i < serial.axes[dim].size(); ++i)
            require(std::abs(serial.axes[dim][i] - parallel.axes[dim][i]) <= 1e-15,
                    "E same global axes");
    }
    require(serial.copper_layer_of_layer == parallel.copper_layer_of_layer, "E same layer mapping");
    require(serial.copper_phi.size() == parallel.copper_phi.size(), "E same global phi length");
    require(serial.grid_report.copper_area_m2.size() == parallel.grid_report.copper_area_m2.size(),
            "E same report size");
    for (std::size_t i = 0; i < serial.copper_phi.size(); ++i) {
        require(std::isfinite(serial.copper_phi[i]) && std::isfinite(parallel.copper_phi[i]),
                "E finite phi");
        require(std::abs(serial.copper_phi[i] - parallel.copper_phi[i]) <= tight,
                "E per-dense-Morton phi invariant");
    }
    for (const auto& l : s.layers) {
        const auto& a = report(serial, l.name);
        const auto& b = report(parallel, l.name);
        close(b.raster, a.raster, tight, "E raster rank invariant");
        close(b.polygons, a.polygons, tight, "E polygons rank invariant");
    }
}
void digest_unchanged() {
    auto m = build(fraction_stack(), options());
    const auto before = chipcae::preview::detail::digest(m).sha256;
    m.grid_report.copper_area_m2 = {{"Cu", {.1, .2}}, {"cu", {.3, .4}}};
    require(chipcae::preview::detail::digest(m).sha256 == before,
            "area report must not change mesh SHA256");
}
#ifdef CHIPCAE_TEST_JSONCONS
using J = jsoncons::json;
namespace fs = std::filesystem;
using namespace chipcae::mesh::testing;
struct TempDirectory {
    fs::path path = fs::temp_directory_path() /
                    ("copper-phi-report-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDirectory() { require(fs::create_directory(path), "create unique temporary directory"); }
    ~TempDirectory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
std::string bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    require(bool(in), "open fixture for reading");
    return {std::istreambuf_iterator<char>(in), {}};
}
void write(const fs::path& p, const J& j) {
    std::ofstream out(p, std::ios::binary);
    out << j.to_string() << '\n';
    out.close();
    require(bool(out), "write temporary fixture");
}
fs::path old_fixture() {
    return fs::path(K1_ROOT) / "tests/fixtures/mesh/i5_fcbga_small.mesh.json";
}
J area_pairs() {
    // Reverse insertion order exercises the std::map ordering on re-encoding.
    return J::parse(
        R"([["cu",{"polygons":0.30000000000000004,"raster":0.3}],["Cu",{"polygons":0.12500000000000003,"raster":0.125}]])");
}
void bit_equal(const Area& a, const Area& b) {
    require(std::bit_cast<std::uint64_t>(a.polygons) == std::bit_cast<std::uint64_t>(b.polygons),
            "F polygon IEEE bits preserved");
    require(std::bit_cast<std::uint64_t>(a.raster) == std::bit_cast<std::uint64_t>(b.raster),
            "F raster IEEE bits preserved");
}
void fixture_roundtrip() {
    TempDirectory temp;
    auto m = build(fraction_stack(), options(4));
    // Supply nontrivial bits independently of the still-missing area producer.
    m.grid_report.copper_area_m2 = {{"cu", {std::nextafter(.3, 1.), .3}},
                                    {"Cu", {std::nextafter(.125, 1.), .125}}};
    const auto p = temp.path / "new.json";
    const auto saved = save_mesh_fixture(m, p);
    require(bool(saved), saved ? "" : "F save: " + saved.error().reason);
    const auto j = J::parse(bytes(p));
    require(j.at("mesh").at("grid_report").contains("copper_area_m2"),
            "F writer omitted nonempty copper_area_m2");
    const auto& pairs = j.at("mesh").at("grid_report").at("copper_area_m2");
    require(pairs.is_array() && pairs.size() == 2 && pairs[0][0].as<std::string>() == "Cu" &&
                pairs[1][0].as<std::string>() == "cu",
            "F sorted map pair encoding");
    require(pairs[0][1].size() == 2 && pairs[0][1].contains("polygons") &&
                pairs[0][1].contains("raster"),
            "F CopperArea has both fields");
    auto loaded = load_mesh_fixture(p);
    require(bool(loaded), loaded ? "" : "F reload: " + loaded.error().reason);
    require(loaded.value().grid_report.copper_area_m2.size() == 2,
            "F report entries survive roundtrip");
    for (const auto& [name, a] : m.grid_report.copper_area_m2)
        bit_equal(a, report(loaded.value(), name));
    require(j.at("mesh_sha256").as<std::string>() == chipcae::preview::detail::digest(m).sha256,
            "F unchanged saved digest");
}
void fixture_legacy() {
    TempDirectory temp;
    const auto original = bytes(old_fixture());
    require(sha256(original) == "3908cf161f69d0be8338c766a45b34ef12b58f788afba0aebdac5d80ed3e3cc4",
            "F immutable legacy reference bytes");
    require(!J::parse(original).at("mesh").at("grid_report").contains("copper_area_m2"),
            "F legacy fixture lacks new key");
    auto loaded = load_mesh_fixture(old_fixture());
    require(bool(loaded), "F load legacy fixture");
    require(loaded.value().grid_report.copper_area_m2.empty(),
            "F missing key defaults to empty map");
    const auto p = temp.path / "legacy.json";
    require(bool(save_mesh_fixture(loaded.value(), p)), "F save legacy fixture");
    const auto saved = bytes(p);
    require(!J::parse(saved).at("mesh").at("grid_report").contains("copper_area_m2"),
            "F legacy save omits empty optional map");
    // Independently generated input may use a different spelling. Repeated saves
    // must use the original writer consistently, including this legacy fixture.
    require(saved.starts_with(R"({"mesh":{"vertices":[)"),
            "F legacy fixture uses original writer declaration order");
    require(bool(save_mesh_fixture(loaded.value(), p)), "F save legacy fixture again");
    require(bytes(p) == saved, "F legacy repeated saves have identical bytes");
    auto reloaded = load_mesh_fixture(p);
    require(bool(reloaded), "F reload saved legacy fixture");
    require(bool(save_mesh_fixture(reloaded.value(), p)), "F resave loaded legacy fixture");
    require(bytes(p) == saved, "F legacy writer bytes stable across load/save");
}
void fixture_legacy_encoding() {
    TempDirectory temp;
    HexMeshArrays m;
    m.nodes = {.1, -0., 1.};
    m.snapped = {{'x', .1, .3, -0.}};
    const auto p = temp.path / "legacy-encoding.json";
    // Independently supplied sorted/shortest input must remain readable. All
    // meshes use the original writer, including signed zero and 17-digit reals.
    const std::string expected =
        R"({"mesh":{"attr_frame":[],"attr_table":{"bodies":[],"boundary_names":[],"entries":[],)"
        R"("layers":[],"materials":[],"references":[]},"attribute":[],"axes":[[],[],[]],)"
        R"("bdr_attribute":[],"bdr_quad":[],"copper_layer_of_layer":[],"copper_phi":[],)"
        R"("elem_gid":[],"face_sets":[],"grid_report":{"N_L":0,"auto_selected":false,)"
        R"("column_ranks":[],"level_axes":[],"levels":1,"max_aspect_ratio":[],"padding":[0,0],)"
        R"("partition_imbalance":0.0,"rule":0,"void_cells":0,"warnings":[]},"hex":[],)"
        R"("internal_interfaces":false,"node_column":[],"node_gid":[],"node_layer":[],)"
        R"("nodes":[0.1,-0.0,1.0],"order":1,"partitioning":[],"pending_cut":[],)"
        R"("preparation_report":[],"snapped":[{"axis":"x","displacement":-0.0,"from":0.1,)"
        R"("to":0.3}],"vertices":[]},"mesh_sha256":null})"
        "\n";
    {
        std::ofstream out(p, std::ios::binary);
        out << expected;
        require(bool(out), "F write independently encoded input");
    }
    auto loaded = load_mesh_fixture(p);
    require(bool(loaded), "F load byte-compatible encoder spelling");
    require(bool(save_mesh_fixture(loaded.value(), p)),
            "F resave byte-compatible encoder spelling");
    const auto canonical = bytes(p);
    require(bool(save_mesh_fixture(m, p)), "F save original writer control");
    require(bytes(p) == canonical, "F arbitrary sorted input uses original writer bytes");
}
void fixture_encoder_contract() {
    TempDirectory temp;
    HexMeshArrays m;
    m.nodes = {.1, -0., 1.};
    m.snapped = {{'x', .1, .3, -0.}};
    const auto p = temp.path / "encoder-contract.json";
    require(bool(save_mesh_fixture(m, p)), "F save original encoder contract");
    const auto text = bytes(p);
    require(text.starts_with(R"({"mesh":{"vertices":[],"hex":[],"attribute":[],)"),
            "F existing mesh fields retain declaration order");
    require(text.find(R"("grid_report":{"levels":1,"rule":0,"N_L":0,)") != std::string::npos,
            "F nested report fields retain declaration order");
    require(text.find(R"("nodes":[0.10000000000000001,-0.0,1.0])") != std::string::npos,
            "F existing reals retain 17-digit spelling and signed zero");
    require(
        text.find(
            R"("snapped":[{"axis":"x","from":0.10000000000000001,"to":0.29999999999999999,"displacement":-0.0}])") !=
            std::string::npos,
        "F nested objects retain declaration order and real spelling");
    require(text.find("copper_area_m2") == std::string::npos,
            "F empty optional map leaves original encoder bytes unchanged");
    auto loaded = load_mesh_fixture(p);
    require(bool(loaded), "F load original encoder output");
    require(bool(save_mesh_fixture(loaded.value(), p)), "F resave original encoder output");
    require(bytes(p) == text, "F original encoder load/save bytes unchanged");
}
void fixture_legacy_neighbours() {
    TempDirectory temp;
    auto loaded = load_mesh_fixture(old_fixture());
    require(bool(loaded), "F load legacy encoding boundary control");
    const auto p = temp.path / "legacy-neighbour.json";
    std::size_t reference_size = 0;
    for (const std::string change : {"unchanged", "report", "signed-zero", "copper-area"}) {
        auto m = loaded.value();
        if (change == "report")
            ++m.grid_report.void_cells;
        else if (change == "signed-zero")
            m.vertices.front() = -0.;
        else if (change == "copper-area")
            m.grid_report.copper_area_m2["Cu"] = {.1, .1};
        require(bool(save_mesh_fixture(m, p)), "F save modified legacy " + change);
        const auto text = bytes(p);
        require(text.starts_with(R"({"mesh":{"vertices":[)"),
                "F modified legacy uses original writer: " + change);
        if (change == "unchanged")
            reference_size = text.size();
        else if (change == "report")
            require(text.size() == reference_size, "F same-size report change uses same encoder");
        auto reloaded = load_mesh_fixture(p);
        require(bool(reloaded), "F reload modified legacy " + change);
        require(bool(save_mesh_fixture(reloaded.value(), p)), "F resave modified legacy " + change);
        require(bytes(p) == text, "F modified legacy byte stability: " + change);
        if (change == "report")
            require(reloaded.value().grid_report.void_cells == m.grid_report.void_cells,
                    "F modified report preserved");
        else if (change == "signed-zero")
            require(std::signbit(reloaded.value().vertices.front()), "F signed zero preserved");
        else if (change == "copper-area")
            bit_equal(report(reloaded.value(), "Cu"), report(m, "Cu"));
    }
}
void fixture_manual() {
    TempDirectory temp;
    auto j = J::parse(bytes(old_fixture()));
    j["mesh"]["grid_report"]["copper_area_m2"] = area_pairs();
    const auto p = temp.path / "manual.json";
    write(p, j);
    auto loaded = load_mesh_fixture(p);
    require(bool(loaded),
            loaded ? "" : "F reader rejected manually added area key: " + loaded.error().reason);
    require(loaded.value().grid_report.copper_area_m2.size() == 2, "F manual entries decoded");
    bit_equal(report(loaded.value(), "Cu"), {std::nextafter(.125, 1.), .125});
    bit_equal(report(loaded.value(), "cu"), {std::nextafter(.3, 1.), .3});
    j["mesh"]["grid_report"].erase("copper_area_m2");
    write(p, j);
    loaded = load_mesh_fixture(p);
    require(bool(loaded) && loaded.value().grid_report.copper_area_m2.empty(),
            "F manually removed key loads empty");
}
void fixture_empty_key() {
    TempDirectory temp;
    auto j = J::parse(bytes(old_fixture()));
    j["mesh"]["grid_report"]["copper_area_m2"] = J::array();
    const auto p = temp.path / "empty-key.json";
    write(p, j);
    auto loaded = load_mesh_fixture(p);
    require(bool(loaded),
            loaded ? "" : "F reader rejected explicit empty area map: " + loaded.error().reason);
    require(loaded.value().grid_report.copper_area_m2.empty(),
            "F explicit empty key decodes empty");
    require(bool(save_mesh_fixture(loaded.value(), p)), "F save explicit empty map");
    require(!J::parse(bytes(p)).at("mesh").at("grid_report").contains("copper_area_m2"),
            "F writer omits explicit empty map");
    const auto absent = load_mesh_fixture(old_fixture());
    require(bool(absent), "F load absent optional map control");
    const auto control = temp.path / "absent-key.json";
    require(bool(save_mesh_fixture(absent.value(), control)), "F save absent optional map control");
    require(bytes(p) == bytes(control), "F empty and absent optional maps encode identically");
}
void fixture_reject(const std::string& which) {
    TempDirectory temp;
    auto j = J::parse(bytes(old_fixture()));
    auto& grid = j["mesh"]["grid_report"];
    std::string reason;
    if (which == "unknown") {
        grid["unknown_copper_area"] = 0;
        reason = "unknown field";
    } else if (which == "missing-old") {
        grid.erase("void_cells");
        reason = "missing required field";
    } else {
        auto pairs = area_pairs();
        if (which == "duplicate") {
            pairs.push_back(pairs[0]);
            reason = "duplicate map key";
        } else if (which == "missing-area") {
            pairs[0][1].erase("raster");
            reason = "missing required field";
        } else if (which == "unknown-area") {
            pairs[0][1]["extra"] = 0;
            reason = "unknown field";
        } else if (which == "nonfinite") {
            pairs[0][1]["raster"] = J::null();
            reason = "expected real";
        } else
            throw std::runtime_error("unknown rejection subcase");
        grid["copper_area_m2"] = pairs;
    }
    const auto p = temp.path / "bad.json";
    write(p, j);
    const auto loaded = load_mesh_fixture(p);
    require(!loaded, "F malformed fixture must fail: " + which);
    require(loaded.error().path == "INPUT_INVALID", "F error code INPUT_INVALID");
    require(loaded.error().reason == reason,
            "F " + which + " expected reason '" + reason + "', got '" + loaded.error().reason +
                "'");
}
void fixture_nonfinite_save() {
    TempDirectory temp;
    for (const Real bad :
         {std::numeric_limits<Real>::infinity(), std::numeric_limits<Real>::quiet_NaN()})
        for (bool polygon : {false, true}) {
            auto m = build(fraction_stack(), options(4));
            m.grid_report.copper_area_m2["Cu"] = polygon ? Area{bad, 0} : Area{0, bad};
            const auto saved = save_mesh_fixture(m, temp.path / "nonfinite.json");
            require(!saved, "F writer must reject nonfinite CopperArea");
            require(saved.error().path == "INPUT_INVALID", "F nonfinite write INPUT_INVALID");
        }
}
#endif
// The brief retains this synthetic timing/RSS check after dropping real-substrate timing.
// One process per sample: ru_maxrss is a process high-water mark.
int benchmark(int argc, char** argv) {
#ifdef CHIPCAE_ENABLE_ECAD
    const auto c = polygons(false);
    auto o = options(256);
    const auto start = std::chrono::steady_clock::now();
    const auto m = c.mesh(o);
    const Real seconds =
        std::chrono::duration<Real>(std::chrono::steady_clock::now() - start).count();
    require(!m.copper_phi.empty(), "performance mesh built");
#if defined(__linux__) || defined(__APPLE__)
    rusage usage{};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "performance getrusage");
#ifdef __APPLE__
    const auto rss = static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    const auto rss = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
    std::cout << std::setprecision(17) << "MEASURED seconds=" << seconds
              << " peak_rss_bytes=" << rss << " nx=" << m.axes[0].size() - 1
              << " ny=" << m.axes[1].size() - 1 << " layers=2 rings=3\n";
    if (argc == 4) {
        const Real baseline = std::stod(argv[2]);
        const auto baseline_rss = std::stoull(argv[3]);
        require(std::isfinite(baseline) && baseline > 0, "positive finite baseline seconds");
        require(seconds - baseline <= std::max(baseline * .05, Real(.1)),
                "performance wall-clock increment exceeds max(5%,0.1s)");
        require(rss <= baseline_rss + 1024 * 1024, "performance RSS increment exceeds 1 MiB");
    } else
        require(argc == 2, "usage: --benchmark [baseline_seconds baseline_rss_bytes]");
#else
    (void)argc;
    (void)argv;
    std::cout << "SKIP RSS (getrusage unavailable)\n";
#endif
#else
    (void)argc;
    (void)argv;
    std::cout << "SKIP polygon (ECAD off)\n";
#endif
    return 0;
}
} // namespace
int main(int argc, char** argv) {
#ifdef CHIPCAE_ENABLE_KOKKOS
    Kokkos::ScopeGuard guard;
#endif
    int failed = 0, passed = 0;
    const auto run = [&](const std::string& name, const auto& test) {
        try {
            test();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& e) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << e.what() << '\n';
        }
    };
    if (argc > 1) {
        try {
            require(std::string(argv[1]) == "--benchmark", "unknown option");
            return benchmark(argc, argv);
        } catch (const std::exception& e) {
            std::cerr << "FAIL performance: " << e.what() << '\n';
            return 1;
        }
    }
#ifdef CHIPCAE_ENABLE_ECAD
    run("A nonuniform multiring, hole, L, stack names and case", multiring);
    run("aligned multiring", aligned);
    run("B NonZero overlap union", overlapping);
    run("C oblique triangle n=64", triangle);
    run("degenerate rings", degenerate);
    run("empty rings", empty_rings);
    run("out-of-grid clipping", outside);
    run("fraction priority", fraction_priority);
#else
    std::cout << "SKIP polygon (ECAD off)\n";
#endif
    run("D fractions .25/.6", fractions);
    run("D no copper", no_copper);
    run("D zero fraction", zero_fraction);
    run("E ranks=1/2", partitioned);
    run("mesh SHA256 unchanged by reports", digest_unchanged);
#ifdef CHIPCAE_TEST_JSONCONS
    run("F new report bit-exact roundtrip", fixture_roundtrip);
    run("F legacy bytes unchanged", fixture_legacy);
    run("F independently encoded input", fixture_legacy_encoding);
    run("F original encoder contract", fixture_encoder_contract);
    run("F legacy encoding boundary", fixture_legacy_neighbours);
    run("F manually add/remove area key", fixture_manual);
    run("F explicit empty optional map", fixture_empty_key);
    for (const std::string which :
         {"unknown", "missing-old", "duplicate", "missing-area", "unknown-area", "nonfinite"})
        run("F reject " + which, [&] { fixture_reject(which); });
    run("F reject nonfinite area on save", fixture_nonfinite_save);
#else
    std::cout << "SKIP fixture (JSONCONS off)\n";
#endif
    std::cout << "copper phi report: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
