#include "fvm_pipeline.hpp"

#include "chipcae/core/communicator.hpp"
#include "chipcae/io/event_log.hpp"
#include "chipcae/io/vtkhdf_results.hpp"
#include "chipcae/material/copper_laminate.hpp"
#include "chipcae/solve/mixture.hpp"
#include "chipcae/solvers/fvm/elastic.hpp"
#include "chipcae/solvers/fvm/thermal.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <jsoncons/json.hpp>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <numeric>
#include <petscsys.h>
#include <set>
#include <sstream>
#include <utility>

namespace chipcae::solve {
namespace {
namespace fv = solvers::fvm;
using Point = std::array<double, 3>;
using CellKey = std::pair<Index, int>;

void check(PetscErrorCode code) {
    if (code)
        throw InputError(ErrorCode::INTERNAL, "PETSc error " + std::to_string(code));
}
struct PetscSession {
    bool owned = false;
    PetscSession() {
        PetscBool initialized = PETSC_FALSE;
        check(PetscInitialized(&initialized));
        if (!initialized) {
            check(PetscInitializeNoArguments());
            owned = true;
        }
    }
    ~PetscSession() {
        if (owned)
            PetscFinalize();
    }
};
[[noreturn]] void fail(const mesh::Error& error, bool mechanical = false) {
    // I14 1.0 retains two producer errors with this double prefix.
    if (mechanical && error.reason.starts_with("INPUT_INVALID: INTERNAL:"))
        throw InputError(ErrorCode::INTERNAL, error.path + ": " + error.reason);
    // The FVM library carries existing code names as reason prefixes.
    for (auto value : {ErrorCode::INPUT_INVALID,
                       ErrorCode::NOT_IMPLEMENTED,
                       ErrorCode::CUT_UNSUPPORTED,
                       ErrorCode::COLUMN_SPLIT_ACROSS_RANKS,
                       ErrorCode::HIERARCHY_INVALID,
                       ErrorCode::INTERNAL}) {
        ErrorCode code(value);
        if (error.reason.starts_with(std::string(code.name()) + ":"))
            throw InputError(static_cast<int>(code.code()), error.path + ": " + error.reason);
    }
    throw InputError(mechanical ? 1 : static_cast<int>(ErrorCode::INTERNAL),
                     error.path + ": " + error.reason);
}
std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// The loader validates either a concrete layer or consecutive #k slices.
std::vector<std::string> layer_targets(const RunInputs& inputs, const std::string& target) {
    if (!target.starts_with("layer:"))
        return {target};
    const auto name = target.substr(6);
    std::vector<std::string> result;
    for (const auto& layer : inputs.stack.layers)
        if (layer.name == name)
            return {target};
    for (const auto& layer : inputs.stack.layers)
        if (layer.name.starts_with(name + "#"))
            result.push_back("layer:" + layer.name);
    if (result.empty())
        throw InputError(ErrorCode::INPUT_INVALID, "invalid target: " + target);
    return result;
}

fv::ThermalProblem thermal_problem(const RunInputs& inputs,
                                   const mesh::HexMeshArrays& arrays,
                                   const fv::FvGrid& grid,
                                   const std::vector<double>& fractions) {
    fv::ThermalProblem problem;
    problem.rtol = inputs.linear.rtol;
    problem.max_iter = inputs.linear.max_iter;
    problem.preconditioner = inputs.linear.preconditioner;
    problem.T_ref = inputs.T_ref;
    const auto nc = grid.cv_volume.size();
    problem.k.resize(nc);
    problem.q_vol.resize(nc, 0);
    std::map<MaterialId, const Material*> materials;
    for (const auto& [name, id] : arrays.attr_table.materials)
        materials.emplace(id, &inputs.materials.at(name));
    for (std::size_t c = 0; c < nc; ++c) {
        const auto& record = arrays.attr_table.entries.at(grid.cv_attribute[c] - 1);
        const auto& base = *materials.at(record.material);
        const auto layer = std::find_if(inputs.stack.layers.begin(),
                                        inputs.stack.layers.end(),
                                        [&](const auto& l) { return l.name == record.layer; });
        if (layer == inputs.stack.layers.end())
            throw InputError(ErrorCode::INPUT_INVALID, "attribute layer is missing");
        // load_inputs already evaluates E/nu/alpha with interpolate_property at
        // T_ref. Thermal conductivity is scalar; mechanical mixture() is deferred.
        problem.k[c] = {base.k, base.k, base.k};
        if (layer->copper)
            material::copper_laminate_conductivity(base.k,
                                                   inputs.materials.at(layer->copper->metal).k,
                                                   fractions.at(c),
                                                   problem.k[c].data());
    }
    std::map<int, double> fixed;
    for (const auto& load : inputs.loads) {
        const auto targets = layer_targets(inputs, load.target);
        auto selected = [&](std::size_t c) {
            const auto& layer = arrays.attr_table.entries.at(grid.cv_attribute[c] - 1).layer;
            return std::find(targets.begin(), targets.end(), "layer:" + layer) != targets.end();
        };
        if (load.type == "heat_source") {
            // This grid is replicated: sum physical volumes once, without MPI reduction.
            double volume = 0;
            for (std::size_t c = 0; c < nc; ++c)
                if (selected(c))
                    volume += grid.cv_volume[c];
            const double density = heat_source_density(load, volume);
            for (std::size_t c = 0; c < nc; ++c)
                if (selected(c))
                    problem.q_vol[c] += density;
        } else if (load.type == "fixed_temperature" && load.target.starts_with("layer:")) {
            for (std::size_t c = 0; c < nc; ++c)
                if (selected(c)) {
                    if (c > static_cast<std::size_t>(std::numeric_limits<int>::max()))
                        throw InputError(ErrorCode::INPUT_INVALID, "fixed CV index overflow");
                    auto [it, inserted] = fixed.emplace(static_cast<int>(c), load.T);
                    // Kelvin: allow round-off with absolute and relative tolerances of 1e-12.
                    const double tolerance =
                        1e-12 + 1e-12 * std::max(std::abs(it->second), std::abs(load.T));
                    if (!inserted && std::abs(it->second - load.T) > tolerance)
                        throw InputError(ErrorCode::INPUT_INVALID, "conflicting fixed CV");
                }
        } else if (load.type == "fixed_temperature" || load.type == "convection") {
            fv::ThermalBc bc{};
            bc.kind = load.type == "fixed_temperature" ? fv::ThermalBc::Kind::Dirichlet
                                                       : fv::ThermalBc::Kind::Robin;
            bc.T = load.T;
            bc.h = load.h;
            bc.T_inf = load.T_inf;
            if (load.target.starts_with("layer:")) {
                const std::array<std::string, 6> names{
                    "face:xmin", "face:xmax", "face:ymin", "face:ymax", "face:zmin", "face:zmax"};
                bool found = false;
                // A layer load covers its whole exterior, including embedded faces.
                // Select each face once across all slices of a split-layer alias.
                for (std::size_t f = 0; f < grid.faces.size(); ++f) {
                    const auto& face = grid.faces[f];
                    if (face.hi >= 0 || !selected(static_cast<std::size_t>(face.lo)))
                        continue;
                    bc.target = face.side == 6 ? "boundary:embedded" : names.at(face.side);
                    bc.face = f;
                    problem.bcs.push_back(bc);
                    found = true;
                }
                if (!found)
                    throw InputError(ErrorCode::INPUT_INVALID,
                                     "target has no boundary faces: " + load.target);
            } else
                for (const auto& target : targets) {
                    bc.target = target;
                    problem.bcs.push_back(bc);
                }
        }
    }
    for (const auto& [cv, temperature] : fixed) {
        problem.fixed_cv.push_back(cv);
        problem.fixed_T.push_back(temperature);
    }
    return problem;
}

int stack_layer(const RunInputs& inputs, const std::string& name) {
    const auto found = std::find_if(inputs.stack.layers.begin(),
                                    inputs.stack.layers.end(),
                                    [&](const auto& layer) { return layer.name == name; });
    if (found == inputs.stack.layers.end())
        throw InputError(ErrorCode::INPUT_INVALID, "attribute layer is missing: " + name);
    const auto index = std::distance(inputs.stack.layers.begin(), found);
    if (index > std::numeric_limits<int>::max())
        throw InputError(ErrorCode::INPUT_INVALID, "stack layer index overflow");
    return static_cast<int>(index);
}

bool finite_point(const Point& p) {
    return std::all_of(p.begin(), p.end(), [](double v) { return std::isfinite(v); });
}

fv::ElasticMesh to_elastic_mesh(const fv::FvGrid& grid,
                                const RunInputs& inputs,
                                const mesh::HexMeshArrays& arrays) {
    const auto nc = grid.cv_volume.size();
    if (nc == 0 || grid.cv_centroid.size() != nc || grid.cv_elem_gid.size() != nc ||
        grid.cv_rank.size() != nc || grid.cv_attribute.size() != nc || grid.cv_kind.size() != nc)
        throw InputError(ErrorCode::INPUT_INVALID, "invalid FVM mechanical cell array lengths");
    fv::ElasticMesh mesh;
    mesh.volume = grid.cv_volume;
    mesh.centroid = grid.cv_centroid;
    mesh.gid = grid.cv_elem_gid;
    mesh.rank = grid.cv_rank;
    std::set<std::int64_t> gids;
    for (std::size_t c = 0; c < nc; ++c) {
        if (!(mesh.volume[c] > 0) || !std::isfinite(mesh.volume[c]) ||
            !finite_point(mesh.centroid[c]) || !gids.insert(mesh.gid[c]).second)
            throw InputError(ErrorCode::INPUT_INVALID, "invalid FVM mechanical geometry or gid");
        const auto& record = arrays.attr_table.entries.at(grid.cv_attribute[c] - 1);
        mesh.layer.push_back(stack_layer(inputs, record.layer));
        mesh.is_cut.push_back(grid.cv_kind[c] != 0);
    }
    for (const auto& face : grid.faces) {
        if (!(face.area > 0) || !std::isfinite(face.area) || !finite_point(face.centroid))
            throw InputError(ErrorCode::INPUT_INVALID, "invalid FVM mechanical face geometry");
        Point normal{};
        for (int d = 0; d < 3; ++d)
            normal[d] = face.area_vector[d] / face.area;
        if (!finite_point(normal))
            throw InputError(ErrorCode::INPUT_INVALID, "nonfinite FVM mechanical face normal");
        // FvGrid currently uses side 6 for EB; I14 uses -1 plus is_embedded.
        const bool embedded = face.hi < 0 && (face.side == -1 || face.side == 6);
        mesh.faces.push_back(
            {face.lo, face.hi, embedded ? -1 : face.side, face.area, normal, face.centroid});
        mesh.is_embedded.push_back(embedded ? 1 : 0);
    }
    if (grid.axes[2].empty())
        throw InputError(ErrorCode::INPUT_INVALID, "missing FVM z axis");
    mesh.z_top = grid.axes[2].back();
    return mesh;
}

fv::ElasticProblem to_elastic_problem(const RunInputs& inputs,
                                      const fv::FvGrid& grid,
                                      const fv::ThermalResult* thermal,
                                      const mesh::HexMeshArrays& arrays,
                                      const std::vector<double>& fractions) {
    fv::ElasticProblem problem;
    problem.support = fv::ElasticProblem::Support::Rigid321;
    problem.rtol = inputs.linear.rtol;
    problem.max_iter = inputs.linear.max_iter;
    problem.preconditioner = inputs.linear.preconditioner;
    const auto nc = grid.cv_volume.size();
    if (fractions.size() != nc || (thermal && thermal->T_cell.size() != nc))
        throw InputError(ErrorCode::INTERNAL, "invalid FVM mechanical thermal array lengths");
    double uniform = inputs.T_ref;
    if (!thermal) {
        const auto found =
            std::find_if(inputs.loads.begin(), inputs.loads.end(), [](const auto& load) {
                return load.type == "uniform_temperature";
            });
        if (found == inputs.loads.end())
            throw InputError(ErrorCode::INPUT_INVALID, "inconsistent temperature sources");
        uniform = found->T;
    }
    std::map<MaterialId, const Material*> materials;
    for (const auto& [name, id] : arrays.attr_table.materials)
        materials.emplace(id, &inputs.materials.at(name));
    problem.C.resize(nc);
    problem.alpha.resize(nc);
    problem.dT.resize(nc);
    for (std::size_t c = 0; c < nc; ++c) {
        const auto& record = arrays.attr_table.entries.at(grid.cv_attribute[c] - 1);
        const auto& base = *materials.at(record.material);
        const auto& layer = inputs.stack.layers.at(stack_layer(inputs, record.layer));
        const auto& metal = layer.copper ? inputs.materials.at(layer.copper->metal) : base;
        const auto constitutive = mixture(base, metal, fractions[c]);
        for (int i = 0; i < 6; ++i)
            for (int j = 0; j < 6; ++j)
                problem.C[c][6 * i + j] = constitutive.C(i, j);
        for (int d = 0; d < 3; ++d)
            problem.alpha[c][d] = constitutive.alpha[d];
        problem.dT[c] = (thermal ? thermal->T_cell[c] : uniform) - inputs.T_ref;
    }
    if (!inputs.fixed.empty()) {
        problem.support = fv::ElasticProblem::Support::Fixed;
        const std::array<std::string, 6> names{
            "face:xmin", "face:xmax", "face:ymin", "face:ymax", "face:zmin", "face:zmax"};
        for (const auto& target : inputs.fixed)
            for (const auto& expanded : layer_targets(inputs, target)) {
                if (expanded.starts_with("layer:"))
                    problem.fixed_layer.push_back(stack_layer(inputs, expanded.substr(6)));
                else {
                    const auto found = std::find(names.begin(), names.end(), expanded);
                    if (found == names.end())
                        throw InputError(ErrorCode::INPUT_INVALID, "invalid target: " + expanded);
                    problem.fixed_side.push_back(static_cast<int>(found - names.begin()));
                }
            }
    }
    return problem;
}

void validate_elastic_result(const fv::ElasticMesh& mesh, const fv::ElasticResult& result) {
    std::size_t top_count = 0;
    for (std::size_t f = 0; f < mesh.faces.size(); ++f) {
        const auto& face = mesh.faces[f];
        if (face.hi < 0 && ((!mesh.is_embedded[f] && face.side == 5) ||
                            (mesh.is_embedded[f] && face.normal[2] > 0.99 &&
                             face.centroid[2] >= mesh.z_top - 1e-13)))
            ++top_count;
    }
    if (result.U_cell.size() != mesh.volume.size() || result.U_face.size() != mesh.faces.size() ||
        result.face_prescribed.size() != mesh.faces.size() ||
        result.top_samples.size() != top_count || result.iterations < 0 ||
        !std::isfinite(result.residual) || result.residual < 0 ||
        !std::isfinite(result.warpage_pv) || result.warpage_pv < 0 ||
        !std::isfinite(result.U_mag_max) || result.U_mag_max < 0 ||
        !std::isfinite(result.min_dn_ratio) ||
        std::find(mesh.gid.begin(), mesh.gid.end(), result.min_dn_gid) == mesh.gid.end() ||
        result.gradient_corrections < 0 || !std::isfinite(result.max_identity_defect) ||
        result.max_identity_defect < 0 ||
        !std::all_of(result.U_cell.begin(), result.U_cell.end(), finite_point) ||
        !std::all_of(result.U_face.begin(), result.U_face.end(), finite_point) ||
        !std::all_of(result.top_samples.begin(), result.top_samples.end(), finite_point) ||
        !std::all_of(
            result.face_prescribed.begin(),
            result.face_prescribed.end(),
            [](char value) { return value == 0 || value == 1; }))
        throw InputError(ErrorCode::INTERNAL, "invalid FVM elastic result");
}

std::map<CellKey, std::size_t> constituent_lookup(const fv::FvGrid& grid,
                                                  const mesh::HexMeshArrays& arrays) {
    const auto nc = grid.cv_volume.size(), n = grid.constituent_gid.size();
    if (grid.constituent_off.size() != nc + 1 || grid.constituent_off.front() != 0 ||
        grid.constituent_off.back() != static_cast<std::int64_t>(n) ||
        !std::is_sorted(grid.constituent_off.begin(), grid.constituent_off.end()) ||
        grid.constituent_subcell.size() != n || grid.constituent_elem.size() != n)
        throw InputError(ErrorCode::INTERNAL, "invalid FVM constituent CSR");
    std::map<CellKey, std::size_t> result;
    for (std::size_t c = 0; c < nc; ++c) {
        if (grid.constituent_off[c] == grid.constituent_off[c + 1])
            throw InputError(ErrorCode::INTERNAL, "empty FVM constituent set");
        for (auto j = grid.constituent_off[c]; j < grid.constituent_off[c + 1]; ++j) {
            const auto e = grid.constituent_elem[j];
            if (e < 0 || static_cast<std::size_t>(e) >= arrays.elem_gid.size() ||
                arrays.elem_gid[e] != grid.constituent_gid[j] || grid.constituent_subcell[j] < 0 ||
                !result.emplace(CellKey{grid.constituent_gid[j], grid.constituent_subcell[j]}, c)
                     .second)
                throw InputError(ErrorCode::INTERNAL, "invalid or duplicate FVM constituent");
        }
    }
    return result;
}

// Kept local to this adapter: no changes to the FEM pipeline's digest contract.
io::ResultsWriter::MeshSummary mesh_summary(const mesh::HexMeshArrays& arrays) {
    io::ResultsWriter::MeshSummary summary{};
    summary.nx0 = arrays.grid_report.level_axes.front()[0].size() - 1;
    summary.ny0 = arrays.grid_report.level_axes.front()[1].size() - 1;
    summary.L = arrays.grid_report.levels - 1;
    summary.nz = arrays.axes[2].size() - 1;
    summary.void_cells = arrays.grid_report.void_cells;
    const auto nx = arrays.axes[0].size() - 1, ny = arrays.axes[1].size() - 1;
    summary.padded_cells =
        (nx * ny - (nx - arrays.grid_report.padding[0]) * (ny - arrays.grid_report.padding[1])) *
        summary.nz;
    std::string raw;
    auto append = [&](std::uint64_t bits) {
        for (int i = 0; i < 8; ++i)
            raw.push_back(static_cast<char>((bits >> (8 * i)) & 255));
    };
    for (auto value : {summary.nx0,
                       summary.ny0,
                       summary.L,
                       summary.nz,
                       static_cast<std::int64_t>(arrays.node_gid.size()),
                       static_cast<std::int64_t>(arrays.elem_gid.size())})
        append(static_cast<std::uint64_t>(value));
    std::vector<std::size_t> nodes(arrays.node_gid.size()), elements(arrays.elem_gid.size());
    std::iota(nodes.begin(), nodes.end(), 0);
    std::iota(elements.begin(), elements.end(), 0);
    std::sort(nodes.begin(), nodes.end(), [&](auto a, auto b) {
        return arrays.node_gid[a] < arrays.node_gid[b];
    });
    std::sort(elements.begin(), elements.end(), [&](auto a, auto b) {
        return arrays.elem_gid[a] < arrays.elem_gid[b];
    });
    for (auto node : nodes)
        for (int d = 0; d < 3; ++d)
            append(std::bit_cast<std::uint64_t>(arrays.vertices[3 * node + d]));
    for (auto element : elements)
        for (int j = 0; j < 8; ++j)
            append(static_cast<std::uint64_t>(arrays.node_gid[arrays.hex[8 * element + j]]));
    summary.mesh_sha256 = mesh::sha256(raw);
    return summary;
}

// New points use incident CV reconstructions. Quantized coordinate keys reconcile
// independently copied mesh rings at 1e-12 of each domain extent, in stable order.
class Reconstruction {
    const mesh::HexMeshArrays& arrays;
    const fv::FvGrid& grid;
    const fv::ThermalResult& thermal;
    std::vector<Point> gradient;
    using PointKey = std::array<std::int64_t, 3>;
    std::map<PointKey, std::set<std::size_t>> incident;
    std::set<std::size_t> dirichlet;
    PointKey key(const Point& p) const {
        PointKey result{};
        for (int d = 0; d < 3; ++d)
            result[d] = std::llround(1e12 * (p[d] - grid.axes[d].front()) /
                                     (grid.axes[d].back() - grid.axes[d].front()));
        return result;
    }
    bool on_face(const Point& p, const fv::FvFace& face) const {
        if (face.axis < 0 || face.side < 0 || face.side >= 6)
            return false;
        const auto e = face.parent_elem;
        if (e < 0)
            throw InputError(ErrorCode::INTERNAL, "missing FVM face parent");
        for (int d = 0; d < 3; ++d) {
            const double tolerance = 1e-12 * (grid.axes[d].back() - grid.axes[d].front());
            double lo = std::numeric_limits<double>::infinity(), hi = -lo;
            for (int j = 0; j < 8; ++j) {
                const auto v = arrays.hex.at(8 * e + j);
                lo = std::min(lo, arrays.vertices.at(3 * v + d));
                hi = std::max(hi, arrays.vertices.at(3 * v + d));
            }
            if (p[d] < lo - tolerance || p[d] > hi + tolerance ||
                (d == face.axis && std::abs(p[d] - face.centroid[d]) > tolerance))
                return false;
        }
        return true;
    }

public:
    Reconstruction(const mesh::HexMeshArrays& m,
                   const fv::FvGrid& g,
                   const fv::ThermalResult& t,
                   const fv::ThermalProblem& problem,
                   const mesh::cut::CutLayerArrays* cut,
                   const std::map<CellKey, std::size_t>& lookup)
        : arrays(m), grid(g), thermal(t), gradient(g.cv_volume.size()) {
        const std::array<std::string, 6> names{
            "face:xmin", "face:xmax", "face:ymin", "face:ymax", "face:zmin", "face:zmax"};
        for (std::size_t c = 0; c < gradient.size(); ++c)
            for (auto j = g.cv_face_off[c]; j < g.cv_face_off[c + 1]; ++j) {
                const auto f = g.cv_faces[j];
                const auto& face = g.faces[f];
                const double sign = face.lo == static_cast<std::int64_t>(c) ? 1 : -1;
                for (int d = 0; d < 3; ++d)
                    gradient[c][d] +=
                        sign * (t.T_face[f] - t.T_cell[c]) * face.area_vector[d] / g.cv_volume[c];
                for (const auto& bc : problem.bcs)
                    if (face.hi < 0 && face.side >= 0 && face.side < 6 &&
                        bc.kind == fv::ThermalBc::Kind::Dirichlet &&
                        bc.target == names[face.side] &&
                        std::abs(face.centroid[face.side / 2] -
                                 (face.side % 2 ? g.axes[face.side / 2].back()
                                                : g.axes[face.side / 2].front())) <=
                            1e-12 * (g.axes[face.side / 2].back() - g.axes[face.side / 2].front()))
                        dirichlet.insert(static_cast<std::size_t>(f));
            }
        auto add = [&](const Point& p, std::size_t cv) { incident[key(p)].insert(cv); };
        for (std::size_t c = 0; c < gradient.size(); ++c) {
            // Include every constituent, also when its representative is another parent.
            for (auto j = g.constituent_off[c]; j < g.constituent_off[c + 1]; ++j) {
                const auto e = g.constituent_elem[j];
                for (int v = 0; v < 8; ++v) {
                    const auto n = m.hex[8 * e + v];
                    add({m.vertices[3 * n], m.vertices[3 * n + 1], m.vertices[3 * n + 2]}, c);
                }
            }
        }
        if (cut && cut->polyhedra) {
            const auto& p = *cut->polyhedra;
            std::map<Index, int> ordinal;
            for (std::size_t cell = 0; cell < p.volume.size(); ++cell) {
                const auto cv =
                    lookup.at({p.parent_elem_gid[cell], ordinal[p.parent_elem_gid[cell]]++});
                for (auto j = p.poly_off[cell]; j < p.poly_off[cell + 1]; ++j) {
                    const auto f = p.poly_faces[j];
                    for (auto k = p.face_off[f]; k < p.face_off[f + 1]; ++k) {
                        const auto v = p.face_conn[k];
                        add({p.points[3 * v], p.points[3 * v + 1], p.points[3 * v + 2]}, cv);
                    }
                }
            }
        }
    }
    double at(const Point& p, std::size_t fallback) const {
        const auto found = incident.find(key(p));
        const std::set<std::size_t> single{fallback};
        const auto& cells = found == incident.end() ? single : found->second;
        long double sum = 0, weight = 0, dirichlet_sum = 0, dirichlet_weight = 0;
        long double boundary_sum = 0, boundary_weight = 0;
        std::set<std::size_t> visited;
        for (auto c : cells) {
            double value = thermal.T_cell[c];
            for (int d = 0; d < 3; ++d)
                value += gradient[c][d] * (p[d] - grid.cv_centroid[c][d]);
            sum += grid.cv_volume[c] * value;
            weight += grid.cv_volume[c];
            for (auto j = grid.cv_face_off[c]; j < grid.cv_face_off[c + 1]; ++j) {
                const auto f = static_cast<std::size_t>(grid.cv_faces[j]);
                const auto& face = grid.faces[f];
                if (face.hi < 0 && visited.insert(f).second && on_face(p, face)) {
                    if (dirichlet.contains(f)) {
                        dirichlet_sum += face.area * thermal.T_face[f];
                        dirichlet_weight += face.area;
                    } else {
                        boundary_sum += face.area * thermal.T_face[f];
                        boundary_weight += face.area;
                    }
                }
            }
        }
        return static_cast<double>(dirichlet_weight > 0  ? dirichlet_sum / dirichlet_weight
                                   : boundary_weight > 0 ? boundary_sum / boundary_weight
                                                         : sum / weight);
    }
};

using PointKey = std::array<std::int64_t, 3>;
PointKey point_key(const fv::FvGrid& grid, const Point& p) {
    PointKey key{};
    for (int d = 0; d < 3; ++d)
        key[d] = std::llround(1e12 * (p[d] - grid.axes[d].front()) /
                              (grid.axes[d].back() - grid.axes[d].front()));
    return key;
}

// Test the actual planar ring, including its edges, rather than its parent box.
// Projection onto the dominant normal plane also handles nonconvex face rings.
bool on_ring(const Point& p,
             const std::vector<Point>& ring,
             const fv::FvFace& face,
             double tolerance) {
    if (ring.size() < 3)
        throw InputError(ErrorCode::INTERNAL, "incomplete FVM boundary ring");
    Point n{};
    for (int d = 0; d < 3; ++d)
        n[d] = face.area_vector[d] / face.area;
    auto distance = [&](const Point& q) {
        double value = 0;
        for (int d = 0; d < 3; ++d)
            value += (q[d] - face.centroid[d]) * n[d];
        return std::abs(value);
    };
    if (distance(p) > tolerance || std::any_of(ring.begin(), ring.end(), [&](const auto& q) {
            return distance(q) > tolerance;
        }))
        return false;
    int axis = 0;
    for (int d = 1; d < 3; ++d)
        if (std::abs(n[d]) > std::abs(n[axis]))
            axis = d;
    const int x = (axis + 1) % 3, y = (axis + 2) % 3;
    bool inside = false;
    for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const auto& a = ring[j];
        const auto& b = ring[i];
        const double dx = b[x] - a[x], dy = b[y] - a[y];
        const double length2 = dx * dx + dy * dy;
        const double t =
            length2 > 0 ? std::clamp(((p[x] - a[x]) * dx + (p[y] - a[y]) * dy) / length2, 0.0, 1.0)
                        : 0;
        if (std::hypot(p[x] - a[x] - t * dx, p[y] - a[y] - t * dy) <= tolerance)
            return true;
        if ((a[y] > p[y]) != (b[y] > p[y]) && p[x] < a[x] + (p[y] - a[y]) * dx / dy)
            inside = !inside;
    }
    return inside;
}

std::map<PointKey, Point> vertex_U(const mesh::HexMeshArrays& arrays,
                                   const fv::FvGrid& grid,
                                   const fv::ElasticResult& elastic,
                                   const mesh::cut::CutLayerArrays* cut,
                                   const std::map<CellKey, std::size_t>& lookup,
                                   bool cut_as_hex) {
    struct Incident {
        Point point{};
        std::set<std::size_t> cells, faces;
    };
    std::map<PointKey, Incident> incident;
    auto add = [&](const Point& p, std::size_t cv) {
        auto [it, inserted] = incident.try_emplace(point_key(grid, p));
        if (inserted)
            it->second.point = p;
        it->second.cells.insert(cv);
    };
    using Ring = std::vector<Point>;
    std::map<Index, std::vector<std::pair<std::size_t, Ring>>> cut_rings;
    if (cut && cut->polyhedra) {
        const auto& poly = *cut->polyhedra;
        std::map<Index, int> ordinal;
        for (std::size_t cell = 0; cell < poly.volume.size(); ++cell) {
            const auto gid = poly.parent_elem_gid[cell];
            const auto cv = lookup.at({gid, ordinal[gid]++});
            for (auto j = poly.poly_off[cell]; j < poly.poly_off[cell + 1]; ++j) {
                const auto f = poly.poly_faces[j];
                Ring ring;
                for (auto k = poly.face_off[f]; k < poly.face_off[f + 1]; ++k) {
                    const auto v = poly.face_conn[k];
                    Point p{poly.points[3 * v], poly.points[3 * v + 1], poly.points[3 * v + 2]};
                    ring.push_back(p);
                    add(p, cv);
                }
                cut_rings[gid].emplace_back(cv, std::move(ring));
            }
        }
    }
    for (std::size_t c = 0; c < grid.cv_volume.size(); ++c)
        for (auto j = grid.constituent_off[c]; j < grid.constituent_off[c + 1]; ++j) {
            const auto e = grid.constituent_elem[j];
            if (!cut_as_hex && cut_rings.contains(arrays.elem_gid[e]))
                continue;
            for (int v = 0; v < 8; ++v) {
                const auto n = arrays.hex[8 * e + v];
                add({arrays.vertices[3 * n],
                     arrays.vertices[3 * n + 1],
                     arrays.vertices[3 * n + 2]},
                    c);
            }
        }
    // Each face retains its original parent even when CVs have been merged.
    std::vector<std::vector<Ring>> rings(grid.faces.size());
    constexpr int corners[6][4] = {
        {0, 3, 7, 4}, {1, 2, 6, 5}, {0, 1, 5, 4}, {3, 2, 6, 7}, {0, 1, 2, 3}, {4, 5, 6, 7}};
    double tolerance = 0;
    for (const auto& axis : grid.axes)
        tolerance = std::max(tolerance, 1e-12 * (axis.back() - axis.front()));
    std::map<Index, std::vector<std::size_t>> parent_faces;
    for (std::size_t f = 0; f < grid.faces.size(); ++f) {
        const auto& face = grid.faces[f];
        if (face.hi >= 0)
            continue;
        const auto e = face.parent_elem;
        const auto gid = arrays.elem_gid.at(e);
        parent_faces[e].push_back(f);
        if (const auto found = cut_rings.find(gid); found != cut_rings.end()) {
            for (const auto& [cv, ring] : found->second)
                if (cv == static_cast<std::size_t>(face.lo) &&
                    on_ring(ring.front(), ring, face, tolerance))
                    rings[f].push_back(ring);
        } else {
            if (face.side < 0 || face.side >= 6)
                throw InputError(ErrorCode::INTERNAL, "missing embedded boundary ring");
            Ring ring;
            for (const auto corner : corners[face.side]) {
                const auto v = arrays.hex.at(8 * e + corner);
                ring.push_back({arrays.vertices[3 * v],
                                arrays.vertices[3 * v + 1],
                                arrays.vertices[3 * v + 2]});
            }
            rings[f].push_back(std::move(ring));
        }
        if (rings[f].empty())
            throw InputError(ErrorCode::INTERNAL, "missing FVM boundary face geometry");
    }
    auto on_face = [&](const Point& p, std::size_t f) {
        return std::any_of(rings[f].begin(), rings[f].end(), [&](const auto& ring) {
            return on_ring(p, ring, grid.faces[f], tolerance);
        });
    };
    if (cut)
        for (std::size_t v = 0; v < cut->eb_parent_elem.size(); ++v) {
            const Point p{
                cut->eb_points[3 * v], cut->eb_points[3 * v + 1], cut->eb_points[3 * v + 2]};
            bool found = false;
            for (auto f : parent_faces[cut->eb_parent_elem[v]])
                if (on_face(p, f)) {
                    add(p, static_cast<std::size_t>(grid.faces[f].lo));
                    found = true;
                }
            if (!found)
                throw InputError(ErrorCode::INTERNAL, "embedded vertex has no incident face");
        }
    std::map<PointKey, Point> values;
    for (auto& [key, item] : incident) {
        for (auto c : item.cells)
            for (auto j = grid.cv_face_off[c]; j < grid.cv_face_off[c + 1]; ++j) {
                const auto f = static_cast<std::size_t>(grid.cv_faces[j]);
                if (grid.faces[f].hi < 0 && on_face(item.point, f))
                    item.faces.insert(f);
            }
        const bool prescribed = std::any_of(item.faces.begin(), item.faces.end(), [&](auto f) {
            return elastic.face_prescribed[f];
        });
        std::array<long double, 3> sum{};
        long double weight = 0;
        if (!item.faces.empty()) {
            for (auto f : item.faces)
                if (!prescribed || elastic.face_prescribed[f]) {
                    for (int d = 0; d < 3; ++d)
                        sum[d] +=
                            grid.faces[f].area * static_cast<long double>(elastic.U_face[f][d]);
                    weight += grid.faces[f].area;
                }
        } else {
            for (auto c : item.cells) {
                for (int d = 0; d < 3; ++d)
                    sum[d] += grid.cv_volume[c] * static_cast<long double>(elastic.U_cell[c][d]);
                weight += grid.cv_volume[c];
            }
        }
        Point value{};
        for (int d = 0; d < 3; ++d)
            value[d] = static_cast<double>(sum[d] / weight);
        if (!(weight > 0) || !finite_point(value))
            throw InputError(ErrorCode::INTERNAL, "invalid reconstructed FVM displacement");
        values.emplace(key, value);
    }
    return values;
}

template <class Partition>
void diagnostic_fields(std::vector<std::string>& fields) {
    if constexpr (requires(Partition p) {
                      p.copper_fraction;
                      p.conductivity_diag;
                  }) {
        for (const auto* name : {"copper_fraction", "conductivity_diag"})
            if (std::find(fields.begin(), fields.end(), name) == fields.end())
                fields.emplace_back(name);
    }
}

template <class Partition>
void diagnostics(Partition& out, double fraction, const Point& k, bool conductivity_available) {
    if constexpr (requires {
                      out.copper_fraction;
                      out.conductivity_diag;
                  }) {
        out.copper_fraction.push_back(fraction);
        // Mechanical-only materials may legitimately omit conductivity.
        if (conductivity_available)
            out.conductivity_diag.insert(out.conductivity_diag.end(), k.begin(), k.end());
    }
}

io::ResultsStep results(const RunInputs& inputs,
                        const mesh::HexMeshArrays& arrays,
                        const mesh::cut::CutLayerArrays* cut,
                        const fv::FvGrid& grid,
                        const fv::ThermalProblem& problem,
                        const fv::ThermalResult& thermal,
                        const fv::ElasticResult& elastic,
                        const std::vector<double>& fractions,
                        int rank) {
    const auto lookup = constituent_lookup(grid, arrays);
    const bool conductivity_available =
        std::all_of(problem.k.begin(), problem.k.end(), [](const auto& k) {
            return std::all_of(
                k.begin(), k.end(), [](double v) { return v > 0 && std::isfinite(v); });
        });
    Reconstruction reconstruction(arrays, grid, thermal, problem, cut, lookup);
    const auto displacement = vertex_U(arrays, grid, elastic, cut, lookup, inputs.cut_as_hex);
    io::ResultsStep step;
    step.cut_as_hex = inputs.cut_as_hex;
    std::map<Index, double> cut_volumes;
    std::map<Index, int> cut_ordinals;
    std::map<Index, std::pair<int, int>> cut_metadata;
    auto sample = [&](io::ResultsPartition& out, const Point& p, std::size_t cv) {
        out.points.insert(out.points.end(), p.begin(), p.end());
        out.T.push_back(reconstruction.at(p, cv));
        const auto& U = displacement.at(point_key(grid, p));
        out.U.insert(out.U.end(), U.begin(), U.end());
    };
    if (cut) {
        if (!cut->polyhedra)
            throw InputError(ErrorCode::INTERNAL, "cut polyhedron output is required");
        const auto& p = *cut->polyhedra;
        for (std::size_t cell = 0; cell < p.volume.size(); ++cell) {
            const auto gid = p.parent_elem_gid[cell];
            const auto cv = lookup.at({gid, cut_ordinals[gid]++});
            cut_volumes[gid] += p.volume[cell];
            const auto metadata = std::pair{p.layer_id[cell], p.material_id[cell]};
            auto [entry, inserted] = cut_metadata.emplace(gid, metadata);
            if (!inserted && entry->second != metadata)
                throw InputError(ErrorCode::INTERNAL, "ambiguous cut parent metadata");
            const auto parent = p.point_parent_elem.at(
                p.face_conn.at(p.face_off.at(p.poly_faces.at(p.poly_off[cell]))));
            if (arrays.partitioning.at(parent) != rank || inputs.cut_as_hex)
                continue;
            auto& out = step.cut;
            std::map<Index, Index> remap;
            for (auto j = p.poly_off[cell]; j < p.poly_off[cell + 1]; ++j) {
                const auto f = p.poly_faces[j];
                out.poly_faces.push_back(static_cast<Index>(out.face_off.size() - 1));
                for (auto k = p.face_off[f]; k < p.face_off[f + 1]; ++k) {
                    const auto v = p.face_conn[k];
                    auto [it, added] = remap.emplace(v, static_cast<Index>(out.points.size() / 3));
                    if (added) {
                        sample(
                            out, {p.points[3 * v], p.points[3 * v + 1], p.points[3 * v + 2]}, cv);
                        out.connectivity.push_back(it->second);
                    }
                    out.face_conn.push_back(it->second);
                }
                out.face_off.push_back(static_cast<Index>(out.face_conn.size()));
            }
            out.offsets.push_back(static_cast<Index>(out.connectivity.size()));
            out.poly_off.push_back(static_cast<Index>(out.poly_faces.size()));
            out.elem_gid.push_back(gid);
            out.layer_id.push_back(p.layer_id[cell]);
            out.material_id.push_back(p.material_id[cell]);
            out.cell_kind.push_back(p.cell_kind[cell]);
            out.volume.push_back(p.volume[cell]);
            diagnostics(out, fractions[cv], problem.k[cv], conductivity_available);
        }
        if (cut->eb_parent_elem.size() % 3 != 0)
            throw InputError(ErrorCode::INTERNAL, "incomplete embedded triangle");
        for (std::size_t v = 0; v < cut->eb_parent_elem.size(); ++v) {
            const auto parent = cut->eb_parent_elem[v];
            if (arrays.partitioning.at(parent) != rank)
                continue;
            const auto gid = arrays.elem_gid.at(parent);
            const auto cv = lookup.at({gid, 0});
            auto& out = step.embedded_boundary;
            out.connectivity.push_back(static_cast<Index>(out.points.size() / 3));
            sample(out,
                   {cut->eb_points[3 * v], cut->eb_points[3 * v + 1], cut->eb_points[3 * v + 2]},
                   cv);
            if (v % 3 == 0) {
                const auto& record = arrays.attr_table.entries.at(grid.cv_attribute[cv] - 1);
                out.elem_gid.push_back(gid);
                out.layer_id.push_back(arrays.attr_table.layers.at(record.layer));
                out.material_id.push_back(record.material);
                out.cell_kind.push_back(1);
                diagnostics(out, fractions[cv], problem.k[cv], conductivity_available);
            }
        }
    }
    auto& out = step.hex;
    std::map<int, Index> vertices;
    for (std::size_t e = 0; e < arrays.elem_gid.size(); ++e) {
        const auto gid = arrays.elem_gid[e];
        const auto found = lookup.find({gid, 0});
        if (arrays.partitioning[e] != rank || found == lookup.end() ||
            (!inputs.cut_as_hex && cut_volumes.contains(gid)))
            continue;
        const auto cv = found->second;
        for (int j = 0; j < 8; ++j) {
            const auto v = arrays.hex[8 * e + j];
            auto [it, added] = vertices.emplace(v, static_cast<Index>(out.points.size() / 3));
            out.hex.push_back(it->second);
            if (added) {
                out.points.insert(out.points.end(),
                                  arrays.vertices.begin() + 3 * v,
                                  arrays.vertices.begin() + 3 * v + 3);
                out.T.push_back(thermal.T_vertex.at(v));
                const Point p{
                    arrays.vertices[3 * v], arrays.vertices[3 * v + 1], arrays.vertices[3 * v + 2]};
                const auto& U = displacement.at(point_key(grid, p));
                out.U.insert(out.U.end(), U.begin(), U.end());
            }
        }
        const auto& record = arrays.attr_table.entries.at(arrays.attribute[e] - 1);
        out.elem_gid.push_back(gid);
        out.material_id.push_back(cut_metadata.contains(gid) ? cut_metadata.at(gid).second
                                                             : record.material);
        out.layer_id.push_back(cut_metadata.contains(gid)
                                   ? cut_metadata.at(gid).first
                                   : arrays.attr_table.layers.at(record.layer));
        out.cell_kind.push_back(cut_volumes.contains(gid) ? 1 : 0);
        double volume = 1;
        for (int d = 0; d < 3; ++d)
            volume *= std::abs(arrays.vertices[3 * arrays.hex[8 * e + 6] + d] -
                               arrays.vertices[3 * arrays.hex[8 * e] + d]);
        out.volume.push_back(cut_volumes.contains(gid) ? cut_volumes.at(gid) : volume);
        diagnostics(out, fractions[cv], problem.k[cv], conductivity_available);
    }
    return step;
}
} // namespace
int run_fvm(const RunInputs& inputs, const std::filesystem::path& output) {
    PetscSession petsc;
    const auto world = Communicator::world();
    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    std::unique_ptr<io::EventLogWriter> events;
    int prepared = 1;
    if (rank == 0)
        try {
            std::filesystem::create_directories(output);
            events = std::make_unique<io::EventLogWriter>(output / "events.bin");
        } catch (const std::exception&) {
            prepared = 0;
        }
    MPI_Bcast(&prepared, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (!prepared)
        throw std::runtime_error("cannot create run directory or event log");
    std::uint64_t sequence = 0, window = 0, command = 0;
    auto emit = [&](io::EventPayload payload) {
        if (events)
            events->append({++sequence,
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count(),
                            std::move(payload)});
    };
    auto state = [&](io::State::Value value) { emit(io::State{value, window, 0}); };
    auto stop = [&]() {
        int stopped = 0;
        if (rank == 0) {
            auto content = file_bytes(output / "control.jsonl");
            std::size_t begin = 0, end;
            while ((end = content.find('\n', begin)) != std::string::npos) {
                auto line = content.substr(begin, end - begin);
                begin = end + 1;
                try {
                    auto c = jsoncons::json::parse(line);
                    const auto& seq_value =
                        c.contains("cmd_seq") ? c.at("cmd_seq") : c.at("cmdSeq");
                    if (!seq_value.is_string() && !seq_value.is_integer<std::uint64_t>())
                        throw std::runtime_error("invalid cmd_seq");
                    auto seq = seq_value.as<std::uint64_t>();
                    if (seq == 0 || seq <= command)
                        continue;
                    auto type = c.at("action").as<std::string>();
                    bool applied = type == "STOP" || type == "3";
                    emit(io::ControlAck{seq,
                                        c.contains("client_request_id")
                                            ? c.at("client_request_id").as<std::string>()
                                        : c.contains("clientRequestId")
                                            ? c.at("clientRequestId").as<std::string>()
                                            : "",
                                        static_cast<std::uint8_t>(applied ? 1 : 3),
                                        window,
                                        applied ? "STOP applied" : "unsupported command",
                                        0});
                    command = seq;
                    if (applied) {
                        stopped = 1;
                        break;
                    }
                } catch (const std::exception&) {
                    emit(io::LogLine{2, 0, "invalid control line", "", 0});
                }
            }
        }
        MPI_Bcast(&stopped, 1, MPI_INT, 0, MPI_COMM_WORLD);
        if (stopped)
            emit(io::KernelExit{0, window, "user_stop"});
        return stopped != 0;
    };
    state(io::State::Value::starting);
    try {
        int ranks = 1;
        MPI_Comm_size(MPI_COMM_WORLD, &ranks);
        if (inputs.grid.ranks != ranks)
            throw InputError(ErrorCode::INTERNAL, "grid ranks do not match MPI communicator size");
        std::optional<mesh::cut::CutLayerArrays> clipped;
        auto arrays = build_mesh(inputs, &clipped);
        auto built = fv::build_grid(arrays, clipped ? &*clipped : nullptr);
        if (!built)
            fail(built.error());
        auto grid = std::move(built.value());
        auto fractions = fv::copper_fraction(grid, arrays);
        auto problem = thermal_problem(inputs, arrays, grid, fractions);
        std::vector<std::string> layers, materials(arrays.attr_table.materials.size());
        for (const auto& layer : inputs.stack.layers)
            layers.push_back(layer.name);
        for (const auto& [name, id] : arrays.attr_table.materials)
            materials.at(id - 1) = name;
        auto fields = inputs.fields;
        diagnostic_fields<io::ResultsPartition>(fields);
        if (std::find(fields.begin(), fields.end(), "volume") == fields.end())
            fields.push_back("volume");
        io::ResultsWriter writer(output, inputs.stack_path, layers, materials, fields);
        writer.set_mesh(mesh_summary(arrays));
        state(io::State::Value::running);
        fv::ThermalResult thermal;
        if (inputs.thermal) {
            auto solved = fv::solve_thermal(grid, arrays, problem, world);
            if (!solved)
                fail(solved.error());
            thermal = std::move(solved.value());
            if (!std::isfinite(thermal.heat_in) || thermal.heat_in < 0 ||
                !std::isfinite(thermal.heat_out) || thermal.heat_out < 0 ||
                !std::isfinite(thermal.imbalance) || thermal.imbalance < 0 ||
                !std::isfinite(thermal.residual) || thermal.residual < 0 || thermal.iterations < 0)
                throw InputError(ErrorCode::INTERNAL, "invalid FVM thermal ledger or residual");
            ++window;
            emit(io::ResidualBatch{window,
                                   0,
                                   {{"fvm.conduction",
                                     "T",
                                     static_cast<std::uint32_t>(thermal.iterations),
                                     thermal.residual,
                                     64,
                                     static_cast<std::uint32_t>(thermal.iterations),
                                     0}},
                                   0});
            if (rank == 0) {
                std::ostringstream line;
                line.imbue(std::locale::classic());
                line << std::setprecision(17) << "FVM_ENERGY {\"heat_in\":" << thermal.heat_in
                     << ",\"heat_out\":" << thermal.heat_out
                     << ",\"imbalance\":" << thermal.imbalance
                     << ",\"iterations\":" << thermal.iterations << '}';
                emit(io::LogLine{1, 0, line.str(), "", 0});
            }
        } else {
            const auto uniform =
                std::find_if(inputs.loads.begin(), inputs.loads.end(), [](const auto& load) {
                    return load.type == "uniform_temperature";
                });
            if (uniform == inputs.loads.end())
                throw InputError(ErrorCode::INPUT_INVALID, "inconsistent temperature sources");
            thermal.T_cell.assign(grid.cv_volume.size(), uniform->T);
            thermal.T_face.assign(grid.faces.size(), uniform->T);
            thermal.T_vertex.assign(arrays.vertices.size() / 3, uniform->T);
        }
        if (stop())
            return 0;
        const auto elastic_mesh = to_elastic_mesh(grid, inputs, arrays);
        const auto elastic_problem = to_elastic_problem(
            inputs, grid, inputs.thermal ? &thermal : nullptr, arrays, fractions);
        auto solved = fv::solve_elastic(elastic_mesh, elastic_problem, MPI_COMM_WORLD);
        // Match FEM checkpoints: STOP received during the collective solve wins
        // over either failure handling or successful mechanics publication.
        if (stop())
            return 0;
        // I14 1.0 exposes no structured failure residual and prohibits parsing
        // its numerical error text. Failure ResidualBatch needs a producer API patch.
        if (!solved)
            fail(solved.error(), true);
        const auto& elastic = solved.value();
        validate_elastic_result(elastic_mesh, elastic);
        ++window;
        emit(io::ResidualBatch{window,
                               0,
                               {{"fvm.solid",
                                 "U",
                                 static_cast<std::uint32_t>(elastic.iterations),
                                 elastic.residual,
                                 64,
                                 static_cast<std::uint32_t>(elastic.iterations),
                                 0}},
                               0});
        if (rank == 0) {
            std::ostringstream line;
            line.imbue(std::locale::classic());
            line << std::setprecision(17) << "FVM_ELASTIC {\"iterations\":" << elastic.iterations
                 << ",\"residual\":" << elastic.residual << ",\"warpage_pv\":" << elastic.warpage_pv
                 << ",\"U_mag_max\":" << elastic.U_mag_max << '}';
            emit(io::LogLine{1, 0, line.str(), "", 0});
        }
        const auto step = results(inputs,
                                  arrays,
                                  clipped ? &*clipped : nullptr,
                                  grid,
                                  problem,
                                  thermal,
                                  elastic,
                                  fractions,
                                  rank);
        writer.write_step(0, 0, step);
        writer.add_summary("T_max",
                           *std::max_element(thermal.T_cell.begin(), thermal.T_cell.end()),
                           "K",
                           0,
                           "FVM cell centers");
        writer.add_summary("warpage_pv", elastic.warpage_pv, "m", 0, elastic.warpage_method);
        writer.add_summary("U_mag_max", elastic.U_mag_max, "m", 0, elastic.U_mag_method);
        writer.finalize();
        emit(io::OutputAvailable{window,
                                 0,
                                 "out/step-0.vtkhdf",
                                 rank == 0 ? mesh::sha256(file_bytes(output / "out/step-0.vtkhdf"))
                                           : ""});
        state(io::State::Value::completed);
        emit(io::KernelExit{0, window, "end_reached"});
        return 0;
    } catch (const InputError& error) {
        emit(io::LogLine{3, rank, error.what(), "", static_cast<std::uint32_t>(error.code)});
        state(io::State::Value::failed);
        emit(io::KernelExit{error.code, window, "error"});
        throw;
    } catch (const std::exception& error) {
        emit(io::LogLine{3, rank, error.what(), "", 1});
        state(io::State::Value::failed);
        emit(io::KernelExit{1, window, "error"});
        throw;
    }
}
} // namespace chipcae::solve
