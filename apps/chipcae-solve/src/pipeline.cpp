#include "chipcae/io/event_log.hpp"
#include "chipcae/io/vtkhdf_results.hpp"
#include "chipcae/solve/case_loader.hpp"
#include "chipcae/solve/mixture.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <jsoncons/json.hpp>
#include <limits>
#include <memory>
#include <mfem.hpp>
#include <numeric>
#include <petscksp.h>
#include <set>
#include <tuple>

namespace chipcae::solve {
namespace {
using namespace mfem;
using Json = jsoncons::json;
void check(PetscErrorCode code) {
    if (code)
        throw std::runtime_error("PETSc error " + std::to_string(code));
}
struct PetscSession {
    PetscSession() { check(PetscInitializeNoArguments()); }
    ~PetscSession() { PetscFinalize(); }
};
struct Direct {
    Mat mat = nullptr;
    Vec rhs = nullptr, solution = nullptr;
    KSP ksp = nullptr;
    std::uint32_t iterations = 0;
    double assembly_seconds = 0, factor_seconds = 0;
    ~Direct() {
        KSPDestroy(&ksp);
        VecDestroy(&rhs);
        VecDestroy(&solution);
        MatDestroy(&mat);
    }
    double solve(const SparseMatrix& a, const Vector& b, Vector& x, const LinearOptions& options) {
        const int n = a.Height();
        const auto begin = std::chrono::steady_clock::now();
        // Copy indices explicitly: MFEM int and PetscInt need not have the same width.
        std::vector<PetscInt> rows(a.GetI(), a.GetI() + n + 1);
        std::vector<PetscInt> columns(a.GetJ(), a.GetJ() + a.GetI()[n]);
        std::vector<PetscScalar> values(a.GetData(), a.GetData() + a.GetI()[n]);
        check(MatCreateSeqAIJ(PETSC_COMM_SELF, n, n, 0, nullptr, &mat));
        check(MatSeqAIJSetPreallocationCSR(mat, rows.data(), columns.data(), values.data()));
        assembly_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        check(VecCreateSeqWithArray(PETSC_COMM_SELF, 1, n, b.GetData(), &rhs));
        x.SetSize(n);
        x = 0.0;
        check(VecCreateSeqWithArray(PETSC_COMM_SELF, 1, n, x.GetData(), &solution));
        check(KSPCreate(PETSC_COMM_SELF, &ksp));
        check(KSPSetOperators(ksp, mat, mat));
        check(KSPSetType(ksp, KSPPREONLY));
        PC pc;
        check(KSPGetPC(ksp, &pc));
        check(PCSetType(pc, PCLU));
        check(PCFactorSetMatSolverType(pc, MATSOLVERPETSC));
        const auto factor_begin = std::chrono::steady_clock::now();
        check(KSPSetUp(ksp));
        factor_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - factor_begin).count();
        check(KSPSolve(ksp, rhs, solution));
        KSPConvergedReason reason;
        check(KSPGetConvergedReason(ksp, &reason));
        if (reason < 0)
            throw std::runtime_error("PETSc LU failed");
        Vector residual(n), correction(n);
        double relative = 0;
        // Reuse LU for iterative refinement, checking the actual unpreconditioned residual.
        for (int i = 0; i < std::min(options.max_iter, 8); ++i) {
            iterations = static_cast<std::uint32_t>(i + 1);
            a.Mult(x, residual);
            residual *= -1;
            residual += b;
            relative = residual.Norml2() / std::max(b.Norml2(), 1.0);
            if (relative <= options.rtol)
                return relative;
            Vec r, d;
            check(VecCreateSeqWithArray(PETSC_COMM_SELF, 1, n, residual.GetData(), &r));
            check(VecCreateSeqWithArray(PETSC_COMM_SELF, 1, n, correction.GetData(), &d));
            auto code = KSPSolve(ksp, r, d);
            VecDestroy(&r);
            VecDestroy(&d);
            check(code);
            x += correction;
        }
        throw std::runtime_error("LU refinement failed residual tolerance: " +
                                 std::to_string(relative));
    }
};
class ThermalForce : public LinearFormIntegrator {
    Coefficient& stress;

public:
    explicit ThermalForce(Coefficient& s) : stress(s) {}
    using LinearFormIntegrator::AssembleRHSElementVect;
    void
    AssembleRHSElementVect(const FiniteElement& el, ElementTransformation& t, Vector& v) override {
        int nd = el.GetDof();
        DenseMatrix gradient(nd, 3);
        v.SetSize(nd * 3);
        v = 0.0;
        const auto& rule = IntRules.Get(el.GetGeomType(), 2 * el.GetOrder() + 1);
        for (int q = 0; q < rule.GetNPoints(); ++q) {
            const auto& ip = rule.IntPoint(q);
            t.SetIntPoint(&ip);
            el.CalcPhysDShape(t, gradient);
            double w = ip.weight * t.Weight() * stress.Eval(t, ip);
            for (int c = 0; c < 3; ++c)
                for (int i = 0; i < nd; ++i)
                    v(i + c * nd) += w * gradient(i, c);
        }
    }
};
class MixtureElasticity : public BilinearFormIntegrator {
    const std::vector<Constitutive>& materials;

public:
    explicit MixtureElasticity(const std::vector<Constitutive>& values) : materials(values) {}
    void AssembleElementMatrix(const FiniteElement& el,
                               ElementTransformation& t,
                               DenseMatrix& a) override {
        int nd = el.GetDof();
        a.SetSize(3 * nd);
        a = 0.0;
        DenseMatrix d(nd, 3), B(6, 3 * nd), CB(6, 3 * nd), local(3 * nd);
        const auto& C = materials.at(t.Attribute - 1).C;
        const auto& rule = IntRules.Get(el.GetGeomType(), 2 * el.GetOrder() + 1);
        for (int q = 0; q < rule.GetNPoints(); ++q) {
            const auto& ip = rule.IntPoint(q);
            t.SetIntPoint(&ip);
            el.CalcPhysDShape(t, d);
            B = 0.0;
            for (int i = 0; i < nd; ++i) {
                B(0, i) = d(i, 0);
                B(1, i + nd) = d(i, 1);
                B(2, i + 2 * nd) = d(i, 2);
                B(3, i + nd) = d(i, 2);
                B(3, i + 2 * nd) = d(i, 1);
                B(4, i) = d(i, 2);
                B(4, i + 2 * nd) = d(i, 0);
                B(5, i) = d(i, 1);
                B(5, i + nd) = d(i, 0);
            }
            Mult(C, B, CB);
            MultAtB(B, CB, local);
            a.Add(ip.weight * t.Weight(), local);
        }
    }
};
class MixtureForce : public LinearFormIntegrator {
    const std::vector<Constitutive>& materials;
    GridFunction& temperature;
    double reference;

public:
    MixtureForce(const std::vector<Constitutive>& v, GridFunction& T, double ref)
        : materials(v), temperature(T), reference(ref) {}
    using LinearFormIntegrator::AssembleRHSElementVect;
    void
    AssembleRHSElementVect(const FiniteElement& el, ElementTransformation& t, Vector& v) override {
        int nd = el.GetDof();
        v.SetSize(3 * nd);
        v = 0.0;
        DenseMatrix d(nd, 3);
        const auto& m = materials.at(t.Attribute - 1);
        Vector stress(3);
        stress = 0.0;
        for (int a = 0; a < 3; ++a)
            for (int b = 0; b < 3; ++b)
                stress[a] += m.C(a, b) * m.alpha[b];
        const auto& rule = IntRules.Get(el.GetGeomType(), 2 * el.GetOrder() + 1);
        for (int q = 0; q < rule.GetNPoints(); ++q) {
            const auto& ip = rule.IntPoint(q);
            t.SetIntPoint(&ip);
            el.CalcPhysDShape(t, d);
            double w = ip.weight * t.Weight() * (temperature.GetValue(t, ip) - reference);
            for (int c = 0; c < 3; ++c)
                for (int i = 0; i < nd; ++i)
                    v[i + c * nd] += w * stress[c] * d(i, c);
        }
    }
};
class AttributeCoefficient : public Coefficient {
    std::vector<double> values;
    GridFunction* temperature;
    double reference;

public:
    AttributeCoefficient(std::vector<double> v, GridFunction* t = nullptr, double ref = 0)
        : values(std::move(v)), temperature(t), reference(ref) {}
    double Eval(ElementTransformation& t, const IntegrationPoint& ip) override {
        double v = values.at(static_cast<std::size_t>(t.Attribute - 1));
        return temperature ? v * (temperature->GetValue(t, ip) - reference) : v;
    }
};
class ConductivityCoefficient : public MatrixCoefficient {
    const std::vector<std::array<double, 3>>& values;

public:
    explicit ConductivityCoefficient(const std::vector<std::array<double, 3>>& v)
        : MatrixCoefficient(3), values(v) {}
    void Eval(DenseMatrix& matrix, ElementTransformation& t, const IntegrationPoint&) override {
        matrix.SetSize(3);
        matrix = 0.0;
        for (int i = 0; i < 3; ++i)
            matrix(i, i) = values.at(t.Attribute - 1)[i];
    }
};
Array<int>
boundary(const mesh::HexMeshArrays& arrays, Mesh& mesh, const std::string& target, int max) {
    Array<int> mark(max);
    mark = 0;
    const std::vector<std::string> faces{
        "face:xmin", "face:xmax", "face:ymin", "face:ymax", "face:zmin", "face:zmax"};
    auto face = std::find(faces.begin(), faces.end(), target);
    bool found = false;
    for (int b = 0; b < mesh.GetNBE(); ++b) {
        int e, info;
        mesh.GetBdrElementAdjacentElement(b, e, info);
        const auto& layer = arrays.attr_table.entries[arrays.attribute[e] - 1].layer;
        if ((face != faces.end() && arrays.bdr_attribute[b] == face - faces.begin() + 1) ||
            target == "layer:" + layer) {
            mark[mesh.GetBdrAttribute(b) - 1] = 1;
            found = true;
        }
    }
    if (!found)
        throw InputError(ErrorCode::INPUT_INVALID, "target has no boundary faces: " + target);
    return mark;
}
std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
} // namespace
int run(const RunInputs& inputs, const std::filesystem::path& output) {
    PetscSession petsc;
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
                    auto c = Json::parse(line);
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
        auto arrays = build_mesh(inputs);
        io::ResultsWriter writer(output, inputs.stack_path, {}, {}, inputs.fields);
        io::ResultsWriter::MeshSummary summary{};
        summary.nx0 = arrays.grid_report.level_axes.front()[0].size() - 1;
        summary.ny0 = arrays.grid_report.level_axes.front()[1].size() - 1;
        // The digest contract uses the finest level index, not the level count.
        summary.L = arrays.grid_report.levels - 1;
        summary.nz = arrays.axes[2].size() - 1;
        const auto nx = arrays.axes[0].size() - 1, ny = arrays.axes[1].size() - 1;
        summary.padded_cells = (nx * ny - (nx - arrays.grid_report.padding[0]) *
                                              (ny - arrays.grid_report.padding[1])) *
                               summary.nz;
        // Each rank has the same global background arrays; hash in canonical GID order.
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
            for (int c = 0; c < 3; ++c)
                append(std::bit_cast<std::uint64_t>(arrays.vertices[3 * node + c]));
        for (auto element : elements)
            for (int c = 0; c < 8; ++c)
                append(static_cast<std::uint64_t>(arrays.node_gid[arrays.hex[8 * element + c]]));
        summary.mesh_sha256 = mesh::sha256(raw);
        writer.set_mesh(std::move(summary));

        // Demo-scale redundant LU: every rank assembles the same global Q2 problem;
        // output ownership still follows the unsplit Morton column partition.
        Mesh mesh(arrays.vertices.data(),
                  static_cast<int>(arrays.vertices.size() / 3),
                  arrays.hex.data(),
                  Geometry::CUBE,
                  arrays.attribute.data(),
                  static_cast<int>(arrays.attribute.size()),
                  arrays.bdr_quad.data(),
                  Geometry::SQUARE,
                  arrays.bdr_attribute.data(),
                  static_cast<int>(arrays.bdr_attribute.size()),
                  3,
                  3);
        mesh.Finalize(false, false);
        // Distinct physical-face/layer attributes make layer and face targets exact.
        for (int b = 0; b < mesh.GetNBE(); ++b) {
            int e, info;
            mesh.GetBdrElementAdjacentElement(b, e, info);
            const auto& name = arrays.attr_table.entries[arrays.attribute[e] - 1].layer;
            auto layer = std::find_if(inputs.stack.layers.begin(),
                                      inputs.stack.layers.end(),
                                      [&](const auto& value) { return value.name == name; });
            mesh.GetBdrElement(b)->SetAttribute(
                6 * static_cast<int>(layer - inputs.stack.layers.begin()) +
                arrays.bdr_attribute[b]);
        }
        mesh.SetAttributes();
        H1_FECollection collection(2, 3);
        FiniteElementSpace scalar(&mesh, &collection),
            vector(&mesh, &collection, 3, Ordering::byVDIM);
        GridFunction temperature(&scalar), displacement(&vector);
        temperature = inputs.T_ref;
        displacement = 0.0;
        const std::size_t na = arrays.attr_table.entries.size();
        std::vector<Constitutive> mixtures;
        std::vector<std::array<double, 3>> conductivities;
        bool has_copper = false;
        std::vector<double> lambda(na), mu(na), stress(na), source(na, 0);
        for (std::size_t a = 0; a < na; ++a) {
            const auto& record = arrays.attr_table.entries[a];
            std::string id;
            for (const auto& [name, number] : arrays.attr_table.materials)
                if (number == record.material)
                    id = name;
            const auto& m = inputs.materials.at(id);
            lambda[a] = m.E * m.nu / ((1 + m.nu) * (1 - 2 * m.nu));
            mu[a] = m.E / (2 * (1 + m.nu));
            stress[a] = (3 * lambda[a] + 2 * mu[a]) * m.alpha;
            conductivities.push_back({m.k, m.k, m.k});
            auto layer =
                std::find_if(inputs.stack.layers.begin(),
                             inputs.stack.layers.end(),
                             [&](const auto& value) { return value.name == record.layer; });
            if (layer->copper) {
                has_copper = true;
                const auto& metal = inputs.materials.at(layer->copper->metal);
                double f = *layer->copper->fraction;
                mixtures.push_back(mixture(m, metal, f));
                if (inputs.thermal)
                    conductivities.back() = {f * metal.k + (1 - f) * m.k,
                                             f * metal.k + (1 - f) * m.k,
                                             1 / (f / metal.k + (1 - f) / m.k)};
            } else
                mixtures.push_back(mixture(m, m, 0));
        }
        state(io::State::Value::running);
        if (inputs.linear.preconditioner == "column_gmg")
            emit(io::LogLine{
                2, 0, "column_gmg not yet available; falling back to PETSc direct LU", "", 0});
        auto solve = [&](BilinearForm& form,
                         LinearForm& rhs,
                         GridFunction& field,
                         Array<int>& essential,
                         const std::string& name) {
            form.Assemble();
            rhs.Assemble();
            form.Finalize(0);
            OperatorPtr op;
            Vector x, b;
            form.FormLinearSystem(essential, field, rhs, op, x, b);
            auto* matrix = op.As<SparseMatrix>();
            if (!matrix)
                throw std::runtime_error("expected serial sparse matrix");
            Direct direct;
            double residual = direct.solve(*matrix, b, x, inputs.linear);
            emit(io::LogLine{
                1,
                rank,
                name + " PETSc CSR assembly_seconds=" + std::to_string(direct.assembly_seconds) +
                    " factor_seconds=" + std::to_string(direct.factor_seconds),
                "",
                0});
            form.RecoverFEMSolution(x, rhs, field);
            ++window;
            emit(io::Residual{"PETSc.LU", name, window, direct.iterations, residual, 64, "", 0});
            emit(io::Progress{window, 0, 0, name == "T" ? 0.5 : 1.0, name + " complete"});
        };
        if (inputs.thermal) {
            ConductivityCoefficient k(conductivities);
            BilinearForm form(&scalar);
            form.AddDomainIntegrator(new DiffusionIntegrator(k));
            LinearForm rhs(&scalar);
            Array<int> essential, all(mesh.bdr_attributes.Max());
            all = 0;
            std::vector<std::unique_ptr<ConstantCoefficient>> coefficients;
            std::vector<std::unique_ptr<Array<int>>> markers;
            for (const auto& load : inputs.loads) {
                if (load.type == "heat_source") {
                    double volume = 0;
                    for (int e = 0; e < mesh.GetNE(); ++e)
                        if (arrays.attr_table.entries[arrays.attribute[e] - 1].layer ==
                            load.target.substr(6))
                            volume += mesh.GetElementVolume(e);
                    const double density = heat_source_density(load, volume);
                    for (std::size_t a = 0; a < na; ++a)
                        if (arrays.attr_table.entries[a].layer == load.target.substr(6))
                            source[a] += density;
                } else if (load.type == "fixed_temperature" && load.target.starts_with("layer:")) {
                    for (int e = 0; e < mesh.GetNE(); ++e)
                        if (arrays.attr_table.entries[arrays.attribute[e] - 1].layer ==
                            load.target.substr(6)) {
                            Array<int> dofs;
                            scalar.GetElementDofs(e, dofs);
                            for (int d : dofs) {
                                temperature[d] = load.T;
                                essential.Append(d);
                            }
                        }
                } else {
                    markers.push_back(std::make_unique<Array<int>>(
                        boundary(arrays, mesh, load.target, all.Size())));
                    auto& marker = *markers.back();
                    if (load.type == "fixed_temperature") {
                        ConstantCoefficient value(load.T);
                        temperature.ProjectBdrCoefficient(value, marker);
                        for (int i = 0; i < all.Size(); ++i)
                            all[i] |= marker[i];
                    } else if (load.type == "convection") {
                        coefficients.push_back(std::make_unique<ConstantCoefficient>(load.h));
                        form.AddBoundaryIntegrator(new MassIntegrator(*coefficients.back()),
                                                   marker);
                        coefficients.push_back(
                            std::make_unique<ConstantCoefficient>(load.h * load.T_inf));
                        rhs.AddBoundaryIntegrator(new BoundaryLFIntegrator(*coefficients.back()),
                                                  marker);
                    }
                }
            }
            AttributeCoefficient q(source);
            rhs.AddDomainIntegrator(new DomainLFIntegrator(q));
            Array<int> boundary_dofs;
            scalar.GetEssentialTrueDofs(all, boundary_dofs);
            essential.Append(boundary_dofs);
            essential.Sort();
            essential.Unique();
            solve(form, rhs, temperature, essential, "T");
            if (stop())
                return 0;
        } else
            for (const auto& load : inputs.loads)
                if (load.type == "uniform_temperature")
                    temperature = load.T;
        AttributeCoefficient l(lambda), m(mu), s(stress, &temperature, inputs.T_ref);
        BilinearForm form(&vector);
        if (has_copper)
            form.AddDomainIntegrator(new MixtureElasticity(mixtures));
        else
            form.AddDomainIntegrator(new ElasticityIntegrator(l, m));
        LinearForm rhs(&vector);
        if (has_copper)
            rhs.AddDomainIntegrator(new MixtureForce(mixtures, temperature, inputs.T_ref));
        else
            rhs.AddDomainIntegrator(new ThermalForce(s));
        Array<int> essential;
        if (inputs.fixed.empty()) {
            // Only vertices incident to real cells can anchor rigid modes (padding is empty).
            std::set<int> active;
            for (int e = 0; e < mesh.GetNE(); ++e) {
                Array<int> vertices;
                mesh.GetElementVertices(e, vertices);
                for (int v : vertices)
                    active.insert(v);
            }
            int a = -1, b = -1, c = -1;
            for (int v : active) {
                const auto* p = mesh.GetVertex(v);
                if (a < 0 ||
                    std::tie(p[2], p[1], p[0]) <
                        std::tie(mesh.GetVertex(a)[2], mesh.GetVertex(a)[1], mesh.GetVertex(a)[0]))
                    a = v;
            }
            if (a >= 0) {
                const auto* origin = mesh.GetVertex(a);
                for (int v : active) {
                    const auto* p = mesh.GetVertex(v);
                    if (p[2] == origin[2] && p[1] == origin[1] && p[0] > origin[0] &&
                        (b < 0 || p[0] > mesh.GetVertex(b)[0]))
                        b = v;
                    if (p[2] == origin[2] && p[1] > origin[1] &&
                        (c < 0 || p[1] > mesh.GetVertex(c)[1]))
                        c = v;
                }
            }
            for (auto [vertex, count] : {std::pair{a, 3}, {b, 2}, {c, 1}}) {
                if (vertex < 0)
                    throw InputError(
                        ErrorCode::INPUT_INVALID,
                        "3-2-1 constraint error: cannot select three noncollinear active vertices");
                Array<int> dofs;
                vector.GetVertexVDofs(vertex, dofs);
                for (int component = 3 - count; component < 3; ++component)
                    essential.Append(dofs[component]);
            }
            essential.Sort();
            essential.Unique();
            if (essential.Size() != 6)
                throw InputError(ErrorCode::INPUT_INVALID,
                                 "3-2-1 constraint error: expected exactly 6 constrained DOFs");
            emit(io::LogLine{1, rank, "3-2-1 constrained_dofs=6 (3+2+1)", "", 0});
        } else {
            Array<int> all(mesh.bdr_attributes.Max());
            all = 0;
            for (const auto& target : inputs.fixed) {
                if (target.starts_with("layer:")) {
                    for (int e = 0; e < mesh.GetNE(); ++e)
                        if (arrays.attr_table.entries[arrays.attribute[e] - 1].layer ==
                            target.substr(6)) {
                            Array<int> dofs;
                            vector.GetElementVDofs(e, dofs);
                            essential.Append(dofs);
                        }
                } else {
                    auto marker = boundary(arrays, mesh, target, all.Size());
                    for (int i = 0; i < all.Size(); ++i)
                        all[i] |= marker[i];
                }
            }
            Array<int> boundary_dofs;
            vector.GetEssentialTrueDofs(all, boundary_dofs);
            essential.Append(boundary_dofs);
            essential.Sort();
            essential.Unique();
        }
        solve(form, rhs, displacement, essential, "U");
        if (stop())
            return 0;
        // Evaluate summaries on all Q2 nodes, including midside/face/interior nodes.
        GridFunction coordinates(&vector);
        VectorFunctionCoefficient position(3, [](const Vector& x, Vector& v) { v = x; });
        coordinates.ProjectCoefficient(position);
        DenseMatrix normal(3);
        normal = 0.0;
        Vector fit_rhs(3);
        fit_rhs = 0.0;
        int nd = scalar.GetNDofs();
        double tmax = -std::numeric_limits<double>::infinity(), umax = 0;
        std::vector<std::array<double, 3>> top;
        for (int i = 0; i < nd; ++i) {
            double x = coordinates[vector.DofToVDof(i, 0)], y = coordinates[vector.DofToVDof(i, 1)],
                   z = coordinates[vector.DofToVDof(i, 2)];
            tmax = std::max(tmax, temperature[i]);
            double mag = 0;
            for (int c = 0; c < 3; ++c)
                mag += std::pow(displacement[vector.DofToVDof(i, c)], 2);
            umax = std::max(umax, std::sqrt(mag));
            if (std::abs(z - arrays.axes[2].back()) < 1e-13) {
                double w = displacement[vector.DofToVDof(i, 2)];
                top.push_back({x, y, w});
                double phi[3] = {1, x, y};
                for (int a = 0; a < 3; ++a) {
                    fit_rhs[a] += phi[a] * w;
                    for (int b = 0; b < 3; ++b)
                        normal(a, b) += phi[a] * phi[b];
                }
            }
        }
        Vector plane(3);
        DenseMatrixInverse inverse(normal);
        inverse.Mult(fit_rhs, plane);
        double lo = std::numeric_limits<double>::infinity(), hi = -lo;
        for (const auto& p : top) {
            double w = p[2] - plane[0] - plane[1] * p[0] - plane[2] * p[1];
            lo = std::min(lo, w);
            hi = std::max(hi, w);
        }
        // The O1 writer stores linear hexes. Explicitly interpolate the Q2
        // solution into Q1; summaries above retain the full Q2 nodal field.
        H1_FECollection output_collection(1, 3);
        FiniteElementSpace output_scalar(&mesh, &output_collection),
            output_vector(&mesh, &output_collection, 3, Ordering::byVDIM);
        GridFunction output_temperature(&output_scalar), output_displacement(&output_vector);
        output_temperature.ProjectGridFunction(temperature);
        output_displacement.ProjectGridFunction(displacement);
        io::ResultsStep result;
        auto& part = result.hex;
        std::map<int, Index> local_nodes;
        for (std::size_t e = 0; e < arrays.elem_gid.size(); ++e)
            if (arrays.partitioning[e] == rank) {
                for (int j = 0; j < 8; ++j) {
                    int v = arrays.hex[8 * e + j];
                    auto [it, inserted] =
                        local_nodes.emplace(v, static_cast<Index>(local_nodes.size()));
                    part.hex.push_back(it->second);
                    if (inserted) {
                        Array<int> dofs;
                        output_scalar.GetVertexDofs(v, dofs);
                        int d = dofs[0];
                        const auto* point = mesh.GetVertex(v);
                        part.points.insert(part.points.end(), point, point + 3);
                        part.T.push_back(output_temperature[d]);
                        for (int c = 0; c < 3; ++c)
                            part.U.push_back(output_displacement[output_vector.DofToVDof(d, c)]);
                    }
                }
                part.elem_gid.push_back(arrays.elem_gid[e]);
                part.cell_kind.push_back(0);
                auto& record = arrays.attr_table.entries[arrays.attribute[e] - 1];
                part.material_id.push_back(record.material);
                auto it =
                    std::find_if(inputs.stack.layers.begin(),
                                 inputs.stack.layers.end(),
                                 [&](const auto& layer) { return layer.name == record.layer; });
                part.layer_id.push_back(static_cast<int>(it - inputs.stack.layers.begin()));
            }
        writer.write_step(0, 0, result);
        writer.add_summary("warpage_pv", hi - lo, "m", 0);
        writer.add_summary("T_max", tmax, "K", 0);
        writer.add_summary("U_mag_max", umax, "m", 0);
        writer.finalize();
        emit(io::OutputAvailable{window,
                                 0,
                                 "out/step-0.vtkhdf",
                                 rank == 0 ? mesh::sha256(file_bytes(output / "out/step-0.vtkhdf"))
                                           : ""});
        state(io::State::Value::completed);
        emit(io::KernelExit{0, window, "end_reached"});
        return 0;
    } catch (const std::exception& error) {
        emit(io::LogLine{3, rank, error.what(), "", 1});
        state(io::State::Value::failed);
        emit(io::KernelExit{1, window, "error"});
        throw;
    }
}
} // namespace chipcae::solve
