#include "../../../schema/src/sha256.hpp"
#include "chipcae/geometry/ecad/ipc2581.hpp"
#include "chipcae/schema/stack.hpp"

#include <bit>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace chipcae::geometry::ecad {
namespace {
template <class U>
void put(std::ostream& out, U x) {
    for (std::size_t i = 0; i < sizeof(U); ++i)
        out.put(static_cast<char>((x >> (8 * i)) & 0xff));
}
void put_i64(std::ostream& out, std::int64_t x) {
    put(out, std::bit_cast<std::uint64_t>(x));
}
} // namespace
bool to_stack(const EcadModel& model,
              const std::filesystem::path& json_file,
              const std::filesystem::path& polyset_file,
              std::string* error) {
    auto fail = [&](std::string s) {
        if (error)
            *error = std::move(s);
        return false;
    };
    if (model.layers.empty() || model.outline.points.size() < 3 || !(model.dbu_m > 0))
        return fail("model requires layers and a closed outline");
    for (const auto& layer : model.layers)
        if (layer.copper && (layer.material.empty() || layer.metal.empty()))
            return fail("MATERIAL_MAPPING_MISSING: copper layer requires base and metal: " +
                        layer.name);
    if (!validate_mapped_targets(model, error))
        return false;
    chipcae::mesh::Polyset ps;
    ps.dbu_m = model.dbu_m;
    for (const auto& layer : model.layers) {
        chipcae::mesh::PolysetLayer pl;
        pl.name = layer.name;
        pl.ring_offsets.push_back(0);
        for (const auto& polygon : layer.polygons) {
            for (const auto& p : polygon.points) {
                pl.coordinates.push_back(p[0]);
                pl.coordinates.push_back(p[1]);
            }
            pl.ring_offsets.push_back(static_cast<std::int64_t>(pl.coordinates.size() / 2));
        }
        ps.layers.push_back(std::move(pl));
    }
    std::ofstream po(polyset_file, std::ios::binary);
    if (!po)
        return fail("cannot create polyset sidecar");
    po.write("CCPOLY01", 8);
    put(po, std::bit_cast<std::uint64_t>(ps.dbu_m));
    put(po, static_cast<std::uint32_t>(ps.layers.size()));
    for (const auto& l : ps.layers) {
        if (l.name.size() > UINT32_MAX || l.ring_offsets.size() > UINT32_MAX ||
            l.coordinates.size() / 2 > UINT32_MAX)
            return fail("polyset too large");
        put(po, static_cast<std::uint32_t>(l.name.size()));
        po.write(l.name.data(), static_cast<std::streamsize>(l.name.size()));
        put(po, static_cast<std::uint32_t>(l.ring_offsets.size() - 1));
        for (auto x : l.ring_offsets)
            put_i64(po, x);
        for (auto x : l.coordinates)
            put_i64(po, x);
    }
    po.close();
    if (!po)
        return fail("failed writing polyset sidecar");
    std::ifstream pin(polyset_file, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(pin)), {});
    const auto digest = chipcae::mesh::sha256(bytes);
    double xmin = 1e300, ymin = 1e300, xmax = -1e300, ymax = -1e300;
    for (auto p : model.outline.points) {
        xmin = std::min(xmin, p[0] * model.dbu_m);
        xmax = std::max(xmax, p[0] * model.dbu_m);
        ymin = std::min(ymin, p[1] * model.dbu_m);
        ymax = std::max(ymax, p[1] * model.dbu_m);
    }
    std::ofstream out(json_file);
    if (!out)
        return fail("cannot create stack JSON");
    out << std::setprecision(17) << "{\"id\":\"" << model.id
        << "\",\"unit\":\"m\",\"outline\":{\"box\":[" << xmin << ',' << ymin << ',' << xmax << ','
        << ymax << "],\"polygon\":[";
    for (std::size_t i = 0; i < model.outline.points.size(); ++i) {
        if (i)
            out << ',';
        out << '[' << model.outline.points[i][0] * model.dbu_m << ','
            << model.outline.points[i][1] * model.dbu_m << ']';
    }
    out << "]},\"layers\":[";
    for (std::size_t i = 0; i < model.layers.size(); ++i) {
        const auto& l = model.layers[i];
        if (i)
            out << ',';
        out << "{\"name\":\"" << l.name << "\",\"thickness\":" << l.thickness_m << ",\"kind\":\""
            << (l.copper ? "Patterned" : "Homogenized")
            << "\",\"n_elem_min\":1,\"grading\":1,\"material\":\""
            << (l.material.empty() ? "dielectric" : l.material) << '"';
        if (l.copper)
            out << ",\"copper\":{\"metal\":\"" << l.metal << "\",\"polygons\":{\"file\":\""
                << polyset_file.filename().string() << "\",\"layer\":\"" << l.name << "\"}}";
        out << '}';
    }
    out << "],\"symmetry\":\"none\",\"package_type\":\"ipc2581\",\"external_refs\":[{\"path\":\""
        << polyset_file.filename().string() << "\",\"sha256\":\"" << digest << "\"}]}\n";
    if (!out)
        return fail("failed writing stack JSON");
    return true;
}
} // namespace chipcae::geometry::ecad
