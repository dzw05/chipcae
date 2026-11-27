// Test-only public-API observer. Expected geometry is computed independently in Python.
#include "chipcae/mesh/cut/outline_cut.hpp"
#include "support.hpp"
#ifdef CHIPCAE_ENABLE_MFEM
#include "../../integration/mesh_p1/runtime.hpp"
#include "chipcae/mesh/background/mfem_adapter.hpp"

#include <mfem.hpp>
#endif

#include <array>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>

namespace {
using namespace chipcae::mesh;
using sp5::require;
#ifdef CHIPCAE_ENABLE_MFEM
struct RuntimeGuard {
    RuntimeGuard(int& argc, char** argv) { k1::init_runtime(argc, argv); }
    ~RuntimeGuard() { k1::finalize_runtime(); }
    RuntimeGuard(const RuntimeGuard&) = delete;
    RuntimeGuard& operator=(const RuntimeGuard&) = delete;
};
#endif
// A deliberately small parser for the controlled case schema. No optional JSON dependency.
struct Json {
    enum Kind { Number, String, Array, Object, Boolean } kind = Number;
    double number = 0;
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;
    const Json& at(const std::string& key) const { return object.at(key); }
    double real() const {
        require(kind == Number && std::isfinite(number), "finite JSON number required");
        return number;
    }
    int integer() const {
        double v = real();
        require(v >= 0 && v <= 1000000 && std::floor(v) == v, "bounded integer required");
        return static_cast<int>(v);
    }
};
struct Parser {
    std::string text;
    std::size_t pos = 0;
    void space() {
        while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
            ++pos;
    }
    bool take(char c) {
        space();
        if (pos < text.size() && text[pos] == c) {
            ++pos;
            return true;
        }
        return false;
    }
    std::string string() {
        require(take('"'), "JSON string required");
        std::string s;
        while (pos < text.size() && text[pos] != '"') {
            require(text[pos] != '\\' && static_cast<unsigned char>(text[pos]) >= 32,
                    "case strings must be plain ASCII");
            s += text[pos++];
        }
        require(pos < text.size(), "unterminated JSON string");
        ++pos;
        return s;
    }
    Json value(int depth = 0) {
        require(depth < 12, "case JSON nesting limit");
        Json j;
        space();
        require(pos < text.size(), "truncated case JSON");
        if (text[pos] == '"') {
            j.kind = Json::String;
            j.string = string();
        } else if (take('[')) {
            j.kind = Json::Array;
            if (!take(']')) {
                do {
                    j.array.push_back(value(depth + 1));
                } while (take(','));
                require(take(']'), "JSON array closing bracket required");
            }
        } else if (take('{')) {
            j.kind = Json::Object;
            if (!take('}')) {
                do {
                    auto key = string();
                    require(take(':'), "JSON colon required");
                    require(j.object.emplace(key, value(depth + 1)).second, "duplicate JSON key");
                } while (take(','));
                require(take('}'), "JSON object closing brace required");
            }
        } else if (text.compare(pos, 4, "true") == 0 || text.compare(pos, 5, "false") == 0) {
            j.kind = Json::Boolean;
            pos += text[pos] == 't' ? 4 : 5;
        } else {
            // Floating-point from_chars is absent in older Apple libc++. The
            // classic locale gives portable binary64 conversion of our schema.
            const auto start = pos;
            while (pos < text.size() &&
                   (std::isdigit(static_cast<unsigned char>(text[pos])) ||
                    std::string_view("+-.eE").find(text[pos]) != std::string_view::npos))
                ++pos;
            std::istringstream token(text.substr(start, pos - start));
            token.imbue(std::locale::classic());
            token >> j.number;
            require(bool(token) && token.eof() && std::isfinite(j.number), "invalid JSON number");
        }
        return j;
    }
};
constexpr std::array<std::array<int, 3>, 10> powers{{{0, 0, 0},
                                                     {1, 0, 0},
                                                     {0, 1, 0},
                                                     {0, 0, 1},
                                                     {2, 0, 0},
                                                     {1, 1, 0},
                                                     {1, 0, 1},
                                                     {0, 2, 0},
                                                     {0, 1, 1},
                                                     {0, 0, 2}}};
using Moments = std::array<long double, 10>;
void accumulate(Moments& m, const std::array<long double, 3>& x, long double w) {
    for (std::size_t i = 0; i < powers.size(); ++i) {
        long double v = w;
        for (int d = 0; d < 3; ++d)
            for (int p = 0; p < powers[i][d]; ++p)
                v *= x[d];
        m[i] += v;
    }
}
void moments(std::ostream& out, const Moments& m) {
    out << '{';
    for (std::size_t i = 0; i < m.size(); ++i) {
        if (i)
            out << ',';
        out << '"' << powers[i][0] << powers[i][1] << powers[i][2] << "\":" << std::setprecision(21)
            << m[i];
    }
    out << '}';
}
template <class T>
void array(std::ostream& out, const T& values) {
    out << '[';
    bool first = true;
    for (const auto& v : values) {
        if (!first)
            out << ',';
        first = false;
        out << std::setprecision(std::numeric_limits<typename T::value_type>::max_digits10) << v;
    }
    out << ']';
}
void dump(const Json& input, const char* path, bool use_mfem) {
    auto s = sp5::stack();
    require(input.kind == Json::Object, "case object required");
    require(input.at("box").array.size() == 4 && input.at("layers").array.size() == 2,
            "box and two layers required");
    for (int d = 0; d < 4; ++d)
        s.outline.box[d] = input.at("box").array[d].real();
    s.layers.clear();
    for (int i = 0; i < 2; ++i) {
        Layer l;
        l.name = i == 0 ? "a" : "b";
        l.kind = "Explicit";
        l.material = i == 0 ? "synthetic_a" : "synthetic_b";
        l.body = l.name;
        l.thickness = input.at("layers").array[i].real();
        l.n_elem_min = input.at("n_elem_min").integer();
        require(l.thickness > 0 && l.n_elem_min > 0, "positive layer required");
        s.layers.push_back(l);
    }
    GridOptions go;
    go.nx = input.at("nx").integer();
    go.ny = input.at("ny").integer();
    go.levels = input.at("levels").integer();
    go.max_ratio = input.at("max_ratio").real();
    go.cell_budget = 1000000;
    require(go.nx > 0 && go.ny > 0, "positive grid counts required");
    auto built = build_tensor_grid(s, go);
    if (!built)
        throw std::runtime_error(built.error().path + ": " + built.error().reason);
    auto mesh = std::move(built.value());
    Footprint outline;
    outline.box = s.outline.box;
    for (const auto& p : input.at("ring").array) {
        require(p.kind == Json::Array && p.array.size() == 2, "xy ring points required");
        outline.polygon.push_back({p.array[0].real(), p.array[1].real()});
    }
    cut::CutOptions co;
    co.snap_phi = input.at("snap_phi").real();
    co.quad_degree = input.at("quad_degree").integer();
    auto result = cut::apply_outline_cut(mesh, outline, co);
    if (!result)
        throw std::runtime_error(result.error().path + ": " + result.error().reason);
    require(result.value().has_value(), "outline must produce cut arrays");
    const auto& c = *result.value();
    sp5::compact(mesh);
    require(c.cut_elem.size() == c.cut_elem_gid.size(), "cut identity count");
    require(c.eb_points.size() == 3 * c.eb_parent_elem.size() &&
                c.eb_normals.size() == c.eb_points.size(),
            "embedded record shape");
    for (auto side : c.sq_side)
        require(side >= -1 && side <= 5, "unknown surface group");
#ifdef CHIPCAE_ENABLE_MFEM
    std::optional<MfemAdapter> adapter;
    if (use_mfem) {
        auto created = MfemAdapter::create(mesh, chipcae::Communicator::world());
        if (!created)
            throw std::runtime_error(created.error().path + ": " + created.error().reason);
        adapter.emplace(std::move(created.value()));
    }
#else
    (void)use_mfem;
#endif
    std::map<chipcae::Index, long double> poly_volume;
    require(c.polyhedra.has_value(), "public outline cut must export polyhedra");
    for (std::size_t p = 0; p < c.polyhedra->volume.size(); ++p)
        poly_volume[c.polyhedra->parent_elem_gid.at(p)] += c.polyhedra->volume[p];
    std::ofstream out(path);
    require(bool(out), "cannot open dump output");
    out.imbue(std::locale::classic());
    out << "{\"axes\":[";
    for (int d = 0; d < 3; ++d) {
        if (d)
            out << ',';
        array(out, mesh.axes[d]);
    }
    out << "],\"snap_phi\":" << std::setprecision(17) << co.snap_phi
        << ",\"quad_degree\":" << co.quad_degree << ",\"kept\":[";
    std::vector<std::array<long double, 3>> lower(mesh.elem_gid.size()), size(lower.size());
    for (std::size_t e = 0; e < mesh.elem_gid.size(); ++e) {
        if (e)
            out << ',';
        out << '[' << mesh.elem_gid[e];
        for (int d = 0; d < 3; ++d) {
            double lo = std::numeric_limits<double>::infinity(), hi = -lo;
            for (int v = 0; v < 8; ++v) {
                double x =
                    mesh.vertices.at(3 * static_cast<std::size_t>(mesh.hex.at(8 * e + v)) + d);
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
            const auto& axis = mesh.axes[d];
            auto it = std::lower_bound(axis.begin(), axis.end(), lo);
            // Grid vertices and axes are the same binary64 identities.
            require(it != axis.end() && *it == lo && it + 1 != axis.end() && *(it + 1) == hi,
                    "parent bounds must match axes");
            out << ',' << (it - axis.begin());
            lower[e][d] = lo;
            size[e][d] = static_cast<long double>(hi) - lo;
        }
        out << ']';
    }
    out << "],\"cut\":[";
    for (std::size_t sub = 0; sub < c.cut_elem.size(); ++sub) {
        if (sub)
            out << ',';
        const auto e = static_cast<std::size_t>(c.cut_elem[sub]);
        const auto gid = mesh.elem_gid.at(e);
        require(gid == c.cut_elem_gid.at(sub), "cut GID mapping");
        const long double parent = size[e][0] * size[e][1] * size[e][2];
        Moments mapped{}, physical{};
        long double weight_sum = 0;
#ifdef CHIPCAE_ENABLE_MFEM
        mfem::ElementTransformation* transform = nullptr;
        if (adapter) {
            const auto& gids = adapter->elem_gid();
            auto it = std::find(gids.begin(), gids.end(), gid);
            require(it != gids.end(), "MFEM parent GID missing");
            transform =
                adapter->local().GetElementTransformation(static_cast<int>(it - gids.begin()));
        }
#endif
        for (Local q = c.vq_off[sub]; q < c.vq_off[sub + 1]; ++q) {
            std::array<long double, 3> x{};
            for (int d = 0; d < 3; ++d)
                x[d] = lower[e][d] + c.vq_xi[d][q] * size[e][d];
            weight_sum += c.vq_w[q];
            accumulate(mapped, x, static_cast<long double>(c.vq_w[q]) * parent);
#ifdef CHIPCAE_ENABLE_MFEM
            if (transform) {
                mfem::IntegrationPoint ip;
                ip.Set3(c.vq_xi[0][q], c.vq_xi[1][q], c.vq_xi[2][q]);
                transform->SetIntPoint(&ip);
                mfem::Vector point(3);
                transform->Transform(ip, point);
                for (int d = 0; d < 3; ++d)
                    x[d] = point[d];
                accumulate(physical, x, static_cast<long double>(c.vq_w[q]) * transform->Weight());
            }
#endif
        }
        long double fraction = 0;
        for (Local sidx = c.sub_off[sub]; sidx < c.sub_off[sub + 1]; ++sidx)
            fraction += c.sub_volfrac[sidx];
        out << "{\"gid\":" << gid << ",\"volfrac\":" << std::setprecision(21) << fraction
            << ",\"poly_vol\":" << poly_volume.at(gid) << ",\"vq_w_sum\":" << weight_sum
            << ",\"mom\":";
        moments(out, mapped);
        if (use_mfem) {
            out << ",\"mom_mfem\":";
            moments(out, physical);
        }
        out << ",\"sq\":[";
        for (int group = 0; group < 7; ++group) {
            if (group)
                out << ',';
            int side = group < 6 ? group : -1;
            std::size_t count = 0;
            std::array<long double, 4> sums{};
            for (Local q = c.sq_off[sub]; q < c.sq_off[sub + 1]; ++q) {
                if (c.sq_side[q] != side)
                    continue;
                ++count; // Raw records, including zero weights.
                sums[0] += c.sq_w[q];
                for (int d = 0; d < 3; ++d)
                    sums[d + 1] += c.sq_w[q] * (lower[e][d] + c.sq_xi[d][q] * size[e][d]);
            }
            out << '[' << side << ',' << count;
            for (auto v : sums)
                out << ',' << std::setprecision(21) << v;
            out << ']';
        }
        out << "],\"eb\":[";
        bool first = true;
        for (std::size_t v = 0; v < c.eb_parent_elem.size(); ++v) {
            if (c.eb_parent_elem[v] != static_cast<Local>(e))
                continue;
            if (!first)
                out << ',';
            first = false;
            std::array<double, 6> record{};
            for (int d = 0; d < 3; ++d) {
                record[d] = c.eb_points.at(3 * v + d);
                record[d + 3] = c.eb_normals.at(3 * v + d);
            }
            array(out, record);
        }
        out << "],\"bq_n\":[";
        for (Local q = c.bq_off[sub]; q < c.bq_off[sub + 1]; ++q) {
            if (q != c.bq_off[sub])
                out << ',';
            array(out, std::array<double, 3>{c.bq_n[0][q], c.bq_n[1][q], c.bq_n[2][q]});
        }
        out << "],\"ext\":";
        array(out, c.exterior_area_vector.at(sub));
        out << '}';
    }
    out << "]}\n";
    out.flush();
    require(bool(out), "dump write failed");
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 4 || (argc == 4 && std::string_view(argv[3]) != "--mfem")) {
        std::cerr << "usage: cut_exact_oracle_dump <case.json> <out.json> [--mfem]\n";
        return 1;
    }
    const bool use_mfem = argc == 4;
#ifndef CHIPCAE_ENABLE_MFEM
    if (use_mfem)
        return 77;
#endif
    return sp5::run([&] {
#ifdef CHIPCAE_ENABLE_MFEM
        std::optional<RuntimeGuard> runtime;
        if (use_mfem)
            runtime.emplace(argc, argv);
        std::unique_ptr<mfem::Device> device;
        if (use_mfem)
            device = std::make_unique<mfem::Device>("cpu");
#endif
        std::ifstream in(argv[1]);
        require(bool(in), "cannot read case JSON");
        Parser p{std::string(std::istreambuf_iterator<char>(in), {})};
        auto input = p.value();
        p.space();
        require(p.pos == p.text.size(), "trailing JSON content");
        dump(input, argv[2], use_mfem);
    });
}
