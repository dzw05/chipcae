"""SP6 solver acceptance (synthetic fixtures, not A0--A3 V&V integration).

--emit writes the C++ harness into the build tree. Only the mesh provider and
an optional pre-assembly observation barrier are replaced in the included app
sources; material resolution, target selection, assembly and output stay real.
No production CLI option, installed header or solver behavior is changed.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile


DRIVER = r'''
#include "chipcae/mesh/cut/outline_cut.hpp"
#include "chipcae/mesh/cut/region_cut.hpp"
#include "chipcae/solve/case_loader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <jsoncons/json.hpp>
#include <map>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace parts {
using namespace chipcae;
using namespace chipcae::mesh;
using Json = jsoncons::json;
Json object(std::initializer_list<std::pair<std::string, Json>> values) {
    return Json(jsoncons::json_object_arg, values);
}
std::string fixture;
std::optional<HexMeshArrays> supplied;
int provider_calls = 0, grid_calls = 0, plane_calls = 0, cut_calls = 0;
int thermal_assemblies = 0, mechanical_assemblies = 0;
bool barrier = false;
std::vector<std::string> calls;
struct AssemblyReached : std::runtime_error {
    AssemblyReached() : std::runtime_error("assembly reached") {}
};
Json read(const std::filesystem::path& path) {
    std::ifstream input(path);
    return Json::parse(input);
}
// Explicit field fixtures retain their supplied GIDs and node identities.
HexMeshArrays fields(const Json& spec) {
    HexMeshArrays m;
    std::map<std::array<double, 3>, Local> shared;
    m.attr_table.materials = {{"solid", 1}, {"lid_material", 2}, {"", 3}};
    for (const auto& cell : spec.at("cells").array_range()) {
        const auto layer = cell.at("layer").as<std::string>();
        const auto body = cell.at("body").as<std::string>();
        const auto material = cell.at("material").as<MaterialId>();
        m.attr_table.layers.try_emplace(layer, static_cast<Local>(m.attr_table.layers.size() + 1));
        BodyId body_id = 0;
        if (!body.empty()) {
            auto [it, added] = m.attr_table.bodies.try_emplace(
                body, static_cast<BodyId>(m.attr_table.bodies.size() + 1));
            (void)added;
            body_id = it->second;
        }
        m.attr_table.entries.push_back({layer, body.empty() ? "" : "region", material, body_id});
        m.attribute.push_back(static_cast<Local>(m.attr_table.entries.size()));
        m.elem_gid.push_back(cell.at("gid").as<Index>());
        m.partitioning.push_back(0);
        auto lo = cell.at("lo").as<std::vector<double>>();
        auto hi = cell.at("hi").as<std::vector<double>>();
        constexpr int corner[8][3] = {
            {0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}};
        for (const auto& c : corner) {
            std::array<double, 3> p{
                c[0] ? hi[0] : lo[0], c[1] ? hi[1] : lo[1], c[2] ? hi[2] : lo[2]};
            Local v = static_cast<Local>(m.node_gid.size());
            if (cell.at("share").as<bool>()) {
                const auto [it, added] = shared.emplace(p, v);
                if (!added) {
                    m.hex.push_back(it->second);
                    continue;
                }
            }
            m.hex.push_back(v);
            m.vertices.insert(m.vertices.end(), p.begin(), p.end());
            m.node_gid.push_back(v);
            m.node_layer.push_back(m.attr_table.layers.at(layer) - 1);
            m.node_column.push_back(v);
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        auto& values = m.axes[axis];
        for (std::size_t v = 0; v < m.node_gid.size(); ++v)
            values.push_back(m.vertices[3 * v + axis]);
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
    }
    // Outward face rings, in the app's physical face order xmin,...,zmax.
    constexpr int faces[6][4] = {
        {0, 4, 7, 3}, {1, 2, 6, 5}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 3, 2, 1}, {4, 5, 6, 7}};
    struct Face {
        std::array<Local, 4> nodes;
        Local attribute;
    };
    std::map<std::array<Local, 4>, std::vector<Face>> occurrences;
    for (std::size_t e = 0; e < m.elem_gid.size(); ++e)
        for (int side = 0; side < 6; ++side) {
            std::array<Local, 4> nodes;
            for (int j = 0; j < 4; ++j)
                nodes[j] = m.hex[8 * e + faces[side][j]];
            auto key = nodes;
            std::sort(key.begin(), key.end());
            const int axis = side / 2;
            const double coordinate = m.vertices[3 * nodes[0] + axis];
            const double exterior = side % 2 ? m.axes[axis].back() : m.axes[axis].front();
            occurrences[key].push_back(
                {nodes, std::abs(coordinate - exterior) < 1e-14 ? side + 1 : 7});
        }
    for (const auto& [key, faces_at_key] : occurrences) {
        (void)key;
        if (faces_at_key.size() == 1) {
            const auto& face = faces_at_key.front();
            m.bdr_quad.insert(m.bdr_quad.end(), face.nodes.begin(), face.nodes.end());
            m.bdr_attribute.push_back(face.attribute);
        }
    }
    m.grid_report.levels = 1;
    m.grid_report.level_axes.push_back({m.axes[0], m.axes[1]});
    m.grid_report.void_cells = spec.at("void_cells").as<std::int64_t>();
    if (spec.contains("unused")) {
        // Put the hole before retained attributes: coefficient tables must keep
        // their original attribute indices instead of compacting used entries.
        m.attr_table.entries.insert(m.attr_table.entries.begin() + 1, {"lid_layer", "", 3, 0});
        for (auto& attribute : m.attribute)
            if (attribute >= 2)
                ++attribute;
    }
    if (spec.contains("pending"))
        m.pending_cut = spec.at("pending").as<std::vector<Local>>();
    return m;
}
} // namespace parts

namespace chipcae::mesh {
Expected<HexMeshArrays> parts_tensor_grid(const StackModel& s, const GridOptions& o) {
    ++parts::grid_calls;
    parts::calls.push_back("grid");
    if (!s.outline.polygon.empty())
        throw std::runtime_error("bounding-grid provider received outline polygon");
    return parts::supplied ? Expected<HexMeshArrays>(*parts::supplied) : build_tensor_grid(s, o);
}
namespace cut {
Expected<CutGeometry> parts_region_cut_planes(const StackModel& s, const HexMeshArrays& m) {
    ++parts::plane_calls;
    parts::calls.push_back("planes");
    if (parts::fixture == "missing_planes")
        return CutGeometry{};
    return region_cut_planes(s, m);
}
Expected<std::optional<CutLayerArrays>>
parts_apply_outline_cut(HexMeshArrays& m, const Footprint& f, const CutOptions& o) {
    ++parts::cut_calls;
    parts::calls.push_back("cut3");
    return apply_outline_cut(m, f, o);
}
Expected<std::optional<CutLayerArrays>> parts_apply_outline_cut(HexMeshArrays& m,
                                                                const Footprint& f,
                                                                const CutOptions& o,
                                                                const CutGeometry& g) {
    ++parts::cut_calls;
    parts::calls.push_back("cut4");
    return apply_outline_cut(m, f, o, g);
}
} // namespace cut
} // namespace chipcae::mesh

// Compile the real mesh setup with providers observable only inside this test
// TU.
#define build_mesh parts_mesh_setup
#define build_tensor_grid parts_tensor_grid
#define region_cut_planes parts_region_cut_planes
#define apply_outline_cut parts_apply_outline_cut
#include "mesh_setup.cpp"
#undef apply_outline_cut
#undef region_cut_planes
#undef build_tensor_grid
#undef build_mesh

#ifdef PARTS_BACKEND
// Preinclude dependencies so macros affect only pipeline.cpp call sites.
#include "chipcae/io/event_log.hpp"
#include "chipcae/io/vtkhdf_results.hpp"
#include "chipcae/solve/mixture.hpp"
#include "chipcae/solvers/fem/column_gmg.hpp"
#include "chipcae/solvers/fem/cut_fem.hpp"
#include "column_gmg_assembly.hpp"
#include "cut_convection.hpp"
#include "cut_output.hpp"
#include "sha256.hpp"

#include <mfem.hpp>
#include <petscksp.h>
namespace mfem {
class PartsBilinearForm : public BilinearForm {
public:
    using BilinearForm::BilinearForm;
    void Assemble(int skip_zeros = 1) {
        if (FESpace()->GetVDim() == 1)
            ++parts::thermal_assemblies;
        else
            ++parts::mechanical_assemblies;
        if (parts::barrier)
            throw parts::AssemblyReached{};
        BilinearForm::Assemble(skip_zeros);
    }
};
} // namespace mfem
namespace chipcae::solve {
mesh::HexMeshArrays parts_mesh_provider(const RunInputs& in,
                                        std::optional<mesh::cut::CutLayerArrays>* cut) {
    ++parts::provider_calls;
    if (parts::fixture == "ranks_mismatch")
        throw InputError(ErrorCode::INPUT_INVALID, "mesh provider invoked before ranks assertion");
    if (parts::supplied) {
        cut->reset();
        auto mesh = *parts::supplied;
        if (parts::fixture == "cut") {
            mesh::cut::CutGeometry geometry;
            for (const auto& [name, body] : mesh.attr_table.bodies) {
                (void)name;
                geometry.planes.push_back({body, {1, 0, 0}, .5});
            }
            auto options = in.cut_options;
            options.export_polyhedra = true;
            auto clipped = mesh::cut::build_cut_layer(mesh, geometry, options);
            if (!clipped)
                throw std::runtime_error(clipped.error().path + ": " + clipped.error().reason);
            *cut = std::move(clipped.value());
        }
        return mesh;
    }
    return parts_mesh_setup(in, cut);
}
} // namespace chipcae::solve
#define build_mesh parts_mesh_provider
#define BilinearForm PartsBilinearForm
#include "pipeline.cpp"
#undef BilinearForm
#undef build_mesh

namespace parts {
void quadrature(chipcae::solve::RunInputs& in) {
    in.cut_options.quad_degree = 6;
    in.cut_options.export_polyhedra = true;
    std::optional<mesh::cut::CutLayerArrays> cut;
    auto m = chipcae::solve::parts_mesh_setup(in, &cut);
    if (!cut || cut->cut_elem.empty() || !cut->polyhedra)
        throw std::runtime_error("region cut must supply cut elements and polyhedra");
    mfem::Mesh grid(m.vertices.data(),
                    static_cast<int>(m.node_gid.size()),
                    m.hex.data(),
                    mfem::Geometry::CUBE,
                    m.attribute.data(),
                    static_cast<int>(m.elem_gid.size()),
                    m.bdr_quad.data(),
                    mfem::Geometry::SQUARE,
                    m.bdr_attribute.data(),
                    static_cast<int>(m.bdr_attribute.size()),
                    3,
                    3);
    grid.Finalize(false, false);
    fem::CutRules rules(*cut, grid);
    const auto& poly = *cut->polyhedra;
    double worst = 0, volume = 0;
    for (double value : rules.volumes) {
        if (!(value > 0))
            throw std::runtime_error("retained element has nonpositive volume");
        volume += value;
    }
    for (int e : cut->cut_elem) {
        const auto* rule = rules.get(e);
        if (!rule)
            throw std::runtime_error("cut element has no S2 rule");
        auto* tr = grid.GetElementTransformation(e);
        double integrated = 0, geometric = 0;
        for (int q = 0; q < rule->GetNPoints(); ++q) {
            const auto& ip = rule->IntPoint(q);
            tr->SetIntPoint(&ip);
            integrated += ip.weight * std::abs(tr->Weight());
        }
        if (!(integrated > 0) || !std::isfinite(integrated))
            throw std::runtime_error("cut quadrature has invalid physical volume");
        for (std::size_t p = 0; p < poly.volume.size(); ++p)
            if (poly.parent_elem_gid[p] == m.elem_gid[e])
                geometric += poly.volume[p];
        if (!(geometric > 0))
            throw std::runtime_error("cut polyhedron has nonpositive volume");
        worst = std::max(worst, std::abs(integrated - geometric) / geometric);
    }
    std::cout << object({{"max_quadrature_relative_error", worst},
                         {"volume", volume},
                         {"cut_cells", cut->cut_elem.size()}})
              << '\n';
}
template <class Summary>
void set_void(Summary& s, std::int64_t n) {
    // The absent additive member is observed as a manifest assertion, not a
    // compile error.
    if constexpr (requires { s.void_cells = n; })
        s.void_cells = n;
}
void summary(const std::filesystem::path& out,
             const std::filesystem::path& stack,
             bool explicit_value) {
    io::ResultsWriter writer(out, stack);
    io::ResultsWriter::MeshSummary s{2, 3, 0, 4, 1, std::string(64, 'a')};
    if (explicit_value)
        set_void(s, 7);
    writer.set_mesh(s);
    writer.finalize();
}
} // namespace parts
#endif

int main(int argc, char** argv) {
#ifdef PARTS_BACKEND
    MPI_Init(&argc, &argv);
#endif
    int code = 0;
    try {
        if (argc != 7)
            throw std::runtime_error("usage: driver MODE CASE PROFILE OUT FIXTURE SPEC");
        const std::string mode = argv[1];
        parts::fixture = argv[5];
        int ranks = 1;
#ifdef PARTS_BACKEND
        MPI_Comm_size(MPI_COMM_WORLD, &ranks);
#endif
        auto in = chipcae::solve::load_inputs(argv[2], argv[3], ranks);
        if (std::string(argv[6]) != "-")
            parts::supplied = parts::fields(parts::read(argv[6]));
        if (mode == "load") {
            parts::Json loads = parts::Json::array();
            for (const auto& l : in.loads)
                if (l.type == "heat_source")
                    loads.push_back(parts::object({{"source", l.source},
                                                   {"total_power", l.total_power},
                                                   {"target", l.target}}));
            std::cout << parts::object({{"heat_sources", loads}}) << '\n';
        } else if (mode == "mesh" || mode == "mesh_no_receiver") {
            std::optional<chipcae::mesh::cut::CutLayerArrays> cut;
            auto m = chipcae::solve::parts_mesh_setup(in, mode == "mesh" ? &cut : nullptr);
            std::cout << parts::object({{"cells", m.elem_gid.size()},
                                        {"cut_cells", cut ? cut->cut_elem.size() : 0}})
                      << '\n';
        }
#ifdef PARTS_BACKEND
        else if (mode == "quad")
            parts::quadrature(in);
        else if (mode == "summary" || mode == "summary_default")
            parts::summary(argv[4], in.stack_path, mode == "summary");
        else {
            parts::barrier = mode == "barrier";
            if (parts::fixture == "ranks_mismatch")
                in.grid.ranks = ranks + 1;
            else if (parts::fixture == "ranks_match")
                in.grid.ranks = ranks;
            if (parts::fixture == "used_unmapped")
                in.materials.erase("lid_material");
            code = chipcae::solve::run(in, argv[4]);
        }
#else
        else
            throw std::runtime_error("backend mode used without numerical backend");
#endif
    } catch (const parts::AssemblyReached& e) {
        std::cerr << e.what() << '\n';
        code = 80;
    } catch (const chipcae::solve::InputError& e) {
        std::cerr << e.what() << '\n';
        code = e.code;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        code = chipcae::ErrorCode::INTERNAL;
    }
    std::cout << parts::object({{"provider_calls", parts::provider_calls},
                                {"thermal_assemblies", parts::thermal_assemblies},
                                {"mechanical_assemblies", parts::mechanical_assemblies},
                                {"grid_calls", parts::grid_calls},
                                {"plane_calls", parts::plane_calls},
                                {"cut_calls", parts::cut_calls},
                                {"calls", parts::calls}})
              << '\n';
#ifdef PARTS_BACKEND
    MPI_Finalize();
#endif
    return code;
}
'''


def material(name, alpha=1e-5):
    def table(columns):
        return dict(temperatures=[300], columns={k: [v] for k, v in columns.items()},
                    interpolation='constant', extrapolation='clamp')
    return dict(id=name, schema_version='1', unit_system='SI', model='thermo_elastic',
                density=2000, conductivity=2,
                elastic=dict(symmetry='isotropic', table=table(dict(E=25e9, nu=.3))),
                thermal_expansion=dict(symmetry='isotropic', cte_type='tangent',
                                       stress_free_temperature=300,
                                       table=table(dict(alpha=alpha))),
                provenance=dict(source_type='illustrative', confidence='illustrative'))


def layer(name, thickness=1, mat='solid', regions=None, minimum=1):
    value = dict(name=name, kind='Explicit', thickness=thickness,
                 n_elem_min=minimum, grading=1)
    if mat is not None:
        value['material'] = mat
    if regions is not None:
        value['regions'] = regions
    return value


def region(body='lid', box=(.2, .2, .8, .8), polygon=None, mat='lid_material'):
    footprint = dict(box=list(box))
    if polygon is not None:
        footprint['polygon'] = polygon
    return dict(name=body+'-region', body=body, material=mat, footprint=footprint)


def cell(gid, lo, hi, name='base', body='', mat=1, share=True):
    return dict(gid=gid, lo=lo, hi=hi, layer=name, body=body, material=mat, share=share)


class Acceptance:
    def __init__(self, args, root):
        self.args, self.root = args, root
        self.codes = {name: int(code) for code, name in re.findall(
            r'code:\s*(\d+),\s*name:\s*(\w+)',
            (args.source / 'protocol/errors.yaml').read_text())}
        self.stack = dict(id='sp6-synthetic', unit='m', outline=dict(box=[0, 0, 1, 1]),
                          symmetry='none', package_type='FCBGA', layers=[layer('base')])
        self.case = dict(
            schema='chipcae.case/1.0', stack=dict(ref='stack.json'),
            materials=dict(ref='materials.json'),
            mesh=dict(type='aligned_cartesian_cut', nx=2, ny=2, levels=1,
                      min_elems_per_layer=1, max_ratio=100),
            physics=[dict(id='mech', solver='fem.solid')],
            loads=[dict(type='uniform_temperature', T_ref=300, T_target=310)],
            constraints=dict(fixed=[dict(target='layer:base')]),
            solver=dict(linear=dict(preconditioner='direct', rtol=1e-11)),
            output=dict(format='vtkhdf'))
        self.counter = 0

    def thermal(self):
        self.case['physics'] = [dict(id='thermal', solver='fem.conduction', mode='steady'),
                                dict(id='mech', solver='fem.solid',
                                     temp_from='thermal', T_ref=300)]
        self.case['loads'] = [dict(type='fixed_temperature', target='face:zmin', T=300)]

    def invoke(self, mode='pipeline', fixture='', spec=None, cli=False, ranks=1):
        self.counter += 1
        for name, value in [('stack', self.stack), ('case', self.case),
                            ('materials', dict(schema='chipcae.material_library/1',
                                               materials=[material('solid'),
                                                          material('lid_material', 2e-5)])),
                            ('profile', dict(schema='chipcae.run_profile/1.0',
                                             ranks=ranks, backend='cpu', gpus=0))]:
            (self.root / (name+'.json')).write_text(json.dumps(value))
        out = self.root / str(self.counter)
        if spec is not None:
            (self.root / 'fields.json').write_text(json.dumps(spec))
        if cli:
            command = [self.args.solve, '--case', str(self.root/'case.json'), '--profile',
                       str(self.root/'profile.json'), '--out', str(out)]
        else:
            command = [self.args.driver, mode, str(self.root/'case.json'),
                       str(self.root/'profile.json'), str(out), fixture,
                       str(self.root/'fields.json') if spec is not None else '-']
        if ranks > 1:
            command = [self.args.mpiexec, '-np', str(ranks), *command]
        environment = dict(os.environ, OMPI_ALLOW_RUN_AS_ROOT='1',
                           OMPI_ALLOW_RUN_AS_ROOT_CONFIRM='1', OMP_NUM_THREADS='2',
                           OMP_PROC_BIND='false')
        result = subprocess.run(command, capture_output=True, text=True, timeout=180,
                                env=environment)
        print(f'command: {command!r}\nexit: {result.returncode}\n'
              f'stdout: {result.stdout}\nstderr: {result.stderr}', flush=True)
        return result, out

    def expect(self, result, code=0, diagnostic=''):
        expected = self.codes[code] if isinstance(code, str) else code
        assert result.returncode == expected, (
            f'expected {code} ({expected}), got {result.returncode}: {result.stderr}')
        assert diagnostic in result.stderr, (diagnostic, result.stderr)

    @staticmethod
    def reports(result):
        # MPI stdout forwarding can concatenate complete per-rank JSON records
        # before forwarding their newlines. Decode every object; callers still
        # require exactly one report per rank and check each rank's counters.
        decoder = json.JSONDecoder()
        reports = []
        for line in result.stdout.splitlines():
            if not line.startswith('{'):
                continue
            pending = line
            while pending:
                report, end = decoder.raw_decode(pending)
                reports.append(report)
                pending = pending[end:].lstrip()
        return reports

    @staticmethod
    def fields(out):
        import h5py
        import numpy as np
        manifest = json.loads((out/'manifest.json').read_text())
        with h5py.File(out/manifest['steps'][0]['file']) as f:
            g = f['VTKHDF/hex']
            for block in ('hex', 'cut', 'eb_surface', 'particles'):
                group = f['VTKHDF/'+block]
                assert np.isfinite(group['Points'][:]).all(), (block, 'Points')
                for category in ('PointData', 'CellData'):
                    if category in group:
                        for name, dataset in group[category].items():
                            assert np.isfinite(dataset[:]).all(), (block, category, name)
            data = {k: g['CellData/'+k][:] for k in ('elem_gid', 'material_id', 'layer_id')}
            data.update(points=g['Points'][:], T=g['PointData/T'][:],
                        U=g['PointData/U'][:] if 'U' in g['PointData'] else np.empty((0, 3)),
                        cell_count=int(sum(g['NumberOfCells'][:])))
        return manifest, data

    def disconnected(self, reverse=False, plate=False):
        self.stack['layers'] = [layer('base'), layer('lid_layer')]
        rows = [cell(0, [0, 0, 0], [1, 1, 1]),
                cell(9, [0, 0, 2], [.5, 1, 3], 'lid_layer', '' if plate else 'zeta', 2),
                cell(11, [.5, 0, 2], [1, 1, 3], 'lid_layer', '' if plate else 'alpha', 2)]
        return dict(cells=list(reversed(rows)) if reverse else rows, void_cells=7)

    def test_region_material(self, missing=False, copper=False):
        self.stack['layers'] = [layer('base', mat='solid' if missing or copper else None,
                                      regions=[region(mat='absent' if missing else 'lid_material')])]
        if copper:
            self.stack['layers'][0]['kind'] = 'Patterned'
            self.stack['layers'][0].pop('regions')
            self.stack['layers'][0]['copper'] = dict(metal='absent', fraction=.5)
        result, _ = self.invoke('load')
        self.expect(result, 'MATERIAL_MAPPING_MISSING' if missing or copper else 0,
                    'absent' if missing or copper else '')

    def test_omitted(self, unused=False, used_unmapped=False, ranks=1):
        import numpy as np
        spec = self.disconnected()
        self.case['constraints'] = dict(fixed=[dict(target='layer:base'),
                                               dict(target='layer:lid_layer')])
        if unused:
            spec['unused'] = True
        if ranks > 1:
            # All global arrays remain replicated; this is a writer/summary check,
            # with the field fixture intentionally owned entirely by rank zero.
            spec['void_cells'] = 7
        result, out = self.invoke(fixture='used_unmapped' if used_unmapped else '',
                                  spec=spec, ranks=ranks)
        if used_unmapped:
            assert result.returncode != 0 and 'assembly reached' not in result.stderr
            report = self.reports(result)[-1]
            assert report['provider_calls'] == 1
            assert report['thermal_assemblies'] == report['mechanical_assemblies'] == 0
            return
        self.expect(result)
        manifest, data = self.fields(out)
        order = np.argsort(data['elem_gid'])
        np.testing.assert_array_equal(data['elem_gid'][order], [0, 9, 11])
        np.testing.assert_array_equal(data['material_id'][order], [1, 2, 2])
        # No GID is compacted into a cavity and no omitted element is regenerated.
        assert len(data['elem_gid']) == data['cell_count'] == 3
        if ranks > 1:
            self.check_mesh(manifest, spec)

    def test_floating(self, variant):
        spec = self.disconnected(reverse=variant in ('reverse', 'multiple_reverse'),
                                 plate=variant == 'plate')
        if variant != 'mechanical':
            self.thermal()
        self.case['constraints'] = dict(fixed=[dict(target='face:zmin')])
        if variant == 'constrained':
            self.case['constraints']['fixed'].append(dict(target='layer:lid_layer'))
        elif variant == 'thermal_constraint':
            self.case['loads'].append(dict(type='fixed_temperature', target='layer:lid_layer', T=310))
        elif variant in ('anchors', 'anchors_connected'):
            self.case['constraints'] = dict(rigid='3-2-1')
            if variant == 'anchors_connected':
                spec['cells'] = spec['cells'][:1]
        elif variant in ('multiple', 'multiple_reverse'):
            # A separate, later component has the lexicographically first body.
            spec['cells'].append(cell(15, [0, 0, 4], [1, 1, 5], 'lid_layer', 'aardvark', 2))
        expected_name = 'lid_layer' if variant == 'plate' else (
            'aardvark' if variant in ('multiple', 'multiple_reverse') else 'alpha')
        if variant == 'mixed':
            spec['cells'][2]['body'] = ''
            expected_name = 'zeta'
        if variant in ('lid', 'cut', 'constrained', 'thermal_constraint', 'mechanical'):
            spec['cells'] = [spec['cells'][0],
                             cell(9, [0, 0, 2], [1, 1, 3], 'lid_layer', 'lid', 2)]
            expected_name = 'lid'
        result, _ = self.invoke('barrier', fixture='cut' if variant == 'cut' else '', spec=spec)
        if variant in ('constrained', 'anchors_connected'):
            self.expect(result, 80, 'assembly reached')
        else:
            self.expect(result, 'INPUT_INVALID', 'floating body '+expected_name)
        report = self.reports(result)[-1]
        assert report['thermal_assemblies'] == int(variant in ('constrained', 'anchors_connected'))
        assert report['mechanical_assemblies'] == 0

    def test_node_identity(self, shared):
        self.stack['layers'] = [layer('base'), layer('lid_layer')]
        self.case['constraints'] = dict(fixed=[dict(target='layer:base')])
        self.thermal()
        # The first pair touches at exactly one node. The second pair has a
        # coincident face, but separate node IDs. Coordinates cannot define union.
        lo, hi = ([1, 1, 1], [2, 2, 2]) if shared else ([0, 0, 1], [1, 1, 2])
        spec = dict(cells=[cell(0, [0, 0, 0], [1, 1, 1]),
                           cell(5, lo, hi, 'lid_layer', 'lid', 2, share=shared)], void_cells=0)
        result, _ = self.invoke('barrier', spec=spec)
        self.expect(result, 80 if shared else 'INPUT_INVALID',
                    'assembly reached' if shared else 'floating body lid')
        report = self.reports(result)[-1]
        assert report['thermal_assemblies'] == int(shared)
        assert report['mechanical_assemblies'] == 0

    def test_thermal_only(self, no_constraints=False):
        import h5py
        import numpy as np
        self.thermal()
        self.case['physics'] = self.case['physics'][:1]
        if no_constraints:
            self.case.pop('constraints')
        # Keep a thermal anchor on each disconnected component.
        self.case['loads'].append(dict(type='fixed_temperature', target='layer:lid_layer', T=310))
        spec = self.disconnected()
        for rigid, fixture in ((False, ''), (True, ''), (True, 'cut')):
            if rigid:
                self.case['constraints'] = dict(rigid='3-2-1')
            # Keep the same temperature readback checks for cut meshes as well.
            result, out = self.invoke(spec=spec, fixture=fixture)
            self.expect(result)
            manifest = json.loads((out/'manifest.json').read_text())
            assert 'floating body' not in result.stderr
            assert not any(s['name'] == 'warpage_pv' for s in manifest['summary']), (
                'thermal-only execution must bypass mechanical assembly')
            report = self.reports(result)[-1]
            assert report['thermal_assemblies'] == 1 and report['mechanical_assemblies'] == 0
            with h5py.File(out/manifest['steps'][0]['file']) as f:
                temperatures = f['VTKHDF/hex/PointData/T'][:]
                z = f['VTKHDF/hex/Points'][:, 2]
                assert temperatures.size > 0 and np.isfinite(temperatures).all()
                np.testing.assert_allclose(temperatures[z <= 1], 300, rtol=0, atol=1e-10)
                np.testing.assert_allclose(temperatures[z >= 2], 310, rtol=0, atol=1e-10)

    def test_thermal_only_rigid_cut(self):
        self.thermal()
        self.case['physics'] = self.case['physics'][:1]
        self.case['constraints'] = dict(rigid='3-2-1')
        self.case['loads'].append(dict(type='fixed_temperature', target='layer:lid_layer', T=310))
        self.stack['layers'] = [layer('base'), layer('lid_layer')]
        # Every cell is cut, so a mechanical run cannot choose regular-cell
        # 3-2-1 anchors. Thermal assembly must still be reached.
        spec = dict(cells=[cell(0, [0, 0, 0], [1, 1, 1], 'base', 'base'),
                           cell(9, [0, 0, 2], [1, 1, 3], 'lid_layer', 'lid', 2)], void_cells=7)
        result, _ = self.invoke('barrier', fixture='cut', spec=spec)
        self.expect(result, 80, 'assembly reached')
        report = self.reports(result)[-1]
        assert report['thermal_assemblies'] == 1 and report['mechanical_assemblies'] == 0

    def alias_stack(self, names):
        self.stack['layers'] = [layer(name, 1/len(names)) for name in names]
        self.case['constraints'] = dict(fixed=[dict(target='layer:'+name) for name in names])
        self.thermal()

    def test_alias(self, variant, loader=False):
        names = ['substrate#0', 'substrate#1']
        target = 'substrate'
        if variant == 'exact':
            names = ['substrate', *names]
        elif variant == 'case':
            target = 'Substrate'
        elif variant == 'gap':
            names = ['substrate#0', 'substrate#2']
        elif variant == 'missing_zero':
            names = ['substrate#1', 'substrate#2']
        elif variant == 'prefix':
            names = ['substrate_extra', 'substrate_other']
        elif variant == 'bad_suffix':
            names = ['substrate#0', 'substrate#1', 'substrate#x']
        elif variant == 'leading_zero':
            names = ['substrate#0', 'substrate#01']
        elif variant == 'negative':
            names = ['substrate#0', 'substrate#-1']
        elif variant == 'exact_malformed':
            names = ['substrate', 'substrate#0', 'substrate#x']
        elif variant == 'numeric':
            names = ['substrate#'+str(i) for i in reversed(range(11))]
        self.alias_stack(names)
        self.case['loads'] = [dict(type='fixed_temperature', target='layer:'+target, T=333)]
        invalid = variant in ('case', 'gap', 'missing_zero', 'prefix', 'bad_suffix',
                              'leading_zero', 'negative')
        if invalid or loader:
            result, _ = self.invoke('load')
            self.expect(result, 'INPUT_INVALID' if invalid else 0,
                        'invalid target' if invalid else '')
            return
        import numpy as np
        if variant == 'exact':
            self.case['loads'].append(dict(type='fixed_temperature', target='face:zmax', T=300))
        else:
            # Heating every slice makes a missed constraint observable. With no
            # source, conduction from just one fixed slice could make the whole
            # domain 333 K and hide an incomplete alias expansion.
            self.case['loads'] += [dict(type='heat_source', target='layer:'+name, q_vol=64)
                                   for name in names]
        result, out = self.invoke(cli=True)
        self.expect(result)
        manifest, data = self.fields(out)
        if variant == 'exact':
            # Above the exact layer, homogeneous conduction drops linearly
            # from 333 K at z=1/3 to 300 K at z=1. Constraining similarly named
            # slices would incorrectly pin the z=2/3 interface to 333 K.
            expected = 333 - 49.5*np.maximum(data['points'][:, 2]-1/3, 0)
            np.testing.assert_allclose(data['T'], expected, rtol=0, atol=1e-10)
            print('exact-layer max temperature error K:', float(np.max(abs(data['T']-expected))))
        else:
            error = float(np.max(abs(data['T']-333)))
            assert error <= 1e-10, ('max exported nodal error K', error)
            maximum = next(s['value'] for s in manifest['summary'] if s['name'] == 'T_max')
            assert abs(maximum-333) <= 1e-10
            print('split-alias max temperature error K:', error)

    def test_mechanical_alias(self):
        self.alias_stack(['substrate#1', 'substrate#0'])
        # Nonzero thermal strain makes an ignored or partially expanded
        # mechanical target observable in U, even if detection accepted it.
        self.case['loads'] = [dict(type='fixed_temperature', target='face:zmin', T=333)]
        self.case['constraints'] = dict(fixed=[dict(target='layer:substrate')])
        result, out = self.invoke(cli=True)
        self.expect(result)
        _, data = self.fields(out)
        import numpy as np
        np.testing.assert_allclose(data['T'], 333, atol=1e-10, rtol=0)
        assert np.max(abs(data['U'])) <= 1e-14
        print('mechanical-alias max displacement m:', float(np.max(abs(data['U']))))

    def test_convection_alias(self, cut=False):
        self.alias_stack(['substrate#0', 'substrate#1'])
        spec = None
        if cut:
            spec = dict(cells=[cell(k, [0, 0, k/4], [1, 1, (k+1)/4],
                                    name='substrate#'+str(k//2), body='substrate')
                               for k in range(4)], void_cells=0)
        self.case['loads'] = [dict(type='convection', target='layer:substrate', h=20, T_inf=340)]
        # The loader requires a fixed thermal anchor as well as convection.
        self.case['loads'].append(dict(type='fixed_temperature', target='face:zmin', T=300))
        aliased, out = self.invoke(cli=not cut, fixture='cut' if cut else '', spec=spec)
        self.expect(aliased)
        a = self.temperatures(out, cut)
        self.case['loads'] = [dict(type='convection', target='layer:'+name, h=20, T_inf=340)
                              for name in ('substrate#0', 'substrate#1')]+[self.case['loads'][-1]]
        expanded, out = self.invoke(cli=not cut, fixture='cut' if cut else '', spec=spec)
        self.expect(expanded)
        b = self.temperatures(out, cut)
        import numpy as np
        assert np.isfinite(a).all() and np.isfinite(b).all()
        assert np.max(b) > 301, 'explicit convection must heat the slab'
        np.testing.assert_allclose(a, b, atol=1e-10, rtol=0)
        print('convection-alias max temperature difference K:', float(np.max(abs(a-b))))

    @staticmethod
    def temperatures(out, cut):
        import h5py
        import numpy as np
        manifest = json.loads((out/'manifest.json').read_text())
        with h5py.File(out/manifest['steps'][0]['file']) as f:
            if cut:
                assert sum(f['VTKHDF/cut/NumberOfCells'][:]) > 0
            return np.concatenate([f['VTKHDF/'+block+'/PointData/T'][:]
                                   for block in ('hex', 'cut') if 'T' in f['VTKHDF/'+block+'/PointData']])

    def test_heat(self, q_vol=False, additive=False):
        import numpy as np
        measurements = []
        for split in (False, True):
            names = ['substrate#0', 'substrate#1'] if split else ['substrate']
            self.alias_stack(names)
            for item in self.stack['layers']:
                item['n_elem_min'] = 2 if split else 4
            self.case['loads'] = [dict(type='fixed_temperature', target='face:zmin', T=300),
                                  dict(type='fixed_temperature', target='face:zmax', T=300)]
            heat = dict(type='heat_source', target='layer:substrate')
            heat['q_vol' if q_vol else 'power'] = 64
            self.case['loads'].append(heat)
            if additive:
                self.case['loads'].append(dict(type='heat_source', target='layer:substrate', q_vol=16))
            result, out = self.invoke(cli=True)
            self.expect(result)
            manifest, data = self.fields(out)
            # Exact 1D Q2 solution, k=2, unit height and area:
            # T(z)=300+P*z*(1-z)/4. Infer injected P from the boundary
            # derivatives of this quadratic, independent of the load normalizer.
            z = data['points'][:, 2]
            desired = 80 if additive else 64
            np.testing.assert_allclose(data['T'], 300+desired*z*(1-z)/4, rtol=0, atol=1e-10)
            middle = data['T'][abs(z-.5) < 1e-14]
            assert len(middle), 'fixture must export midplane nodes'
            injected = 16*float(np.mean(middle)-300)
            assert abs(injected/desired-1) <= 1e-12, ('injected power W', injected, desired)
            maximum = next(s['value'] for s in manifest['summary'] if s['name'] == 'T_max')
            measurements.append((injected, maximum))
            print('heat split, injected power W, T_max K, max temperature error K:',
                  split, injected, maximum,
                  float(np.max(abs(data['T']-(300+desired*z*(1-z)/4)))))
        assert abs(measurements[0][0]/measurements[1][0]-1) <= 1e-12
        assert abs(measurements[0][1]/measurements[1][1]-1) <= 1e-10

    def test_heat_load_intact(self):
        self.alias_stack(['substrate#1', 'substrate#0'])
        self.case['loads'].append(dict(type='heat_source', target='layer:substrate', power=64))
        result, _ = self.invoke('load')
        self.expect(result)
        loads = self.reports(result)[0]['heat_sources']
        assert len(loads) == 1, loads
        assert math.isclose(loads[0]['source'], 64, rel_tol=0, abs_tol=1e-12), loads
        assert loads[0]['total_power'] is True and loads[0]['target'] == 'layer:substrate', loads

    def test_heat_cut_volume(self):
        import h5py
        import numpy as np
        self.thermal()
        self.case['loads'] += [dict(type='fixed_temperature', target='face:zmax', T=300),
                               dict(type='heat_source', target='layer:base', power=64)]
        spec = dict(cells=[cell(k, [0,0,k/4], [1,1,(k+1)/4], body='base')
                           for k in range(4)], void_cells=0)
        result, out = self.invoke(fixture='cut', spec=spec)
        self.expect(result)
        manifest = json.loads((out/'manifest.json').read_text())
        with h5py.File(out/manifest['steps'][0]['file']) as f:
            g = f['VTKHDF/cut']
            assert sum(g['NumberOfCells'][:]) == 4
            z, T = g['Points'][:,2], g['PointData/T'][:]
            volume = float(np.sum(g['CellData/volume'][:]))
            assert abs(volume/.5-1) <= 1e-12
            # P=64 over a half-area extrusion gives q=128, Tmid-300=8.
            np.testing.assert_allclose(T, 300+32*z*(1-z), rtol=0, atol=1e-10)
            injected = 16*volume*float(np.mean(T[abs(z-.5) < 1e-14])-300)
            assert abs(injected/64-1) <= 1e-12
            print('cut volume m^3, injected power W, max temperature error K:',
                  volume, injected, float(np.max(abs(T-(300+32*z*(1-z))))))

    def test_zero_volume(self):
        self.stack['layers'].append(layer('empty'))
        self.thermal()
        self.case['loads'].append(dict(type='heat_source', target='layer:empty', power=64))
        spec = dict(cells=[cell(0, [0, 0, 0], [1, 1, 1])], void_cells=1)
        result, _ = self.invoke(spec=spec)
        self.expect(result, 'INPUT_INVALID', 'zero volume')

    @staticmethod
    def check_mesh(manifest, spec):
        mesh = manifest['mesh']
        assert type(mesh.get('void_cells')) is int, ('void_cells must be an integer', mesh)
        assert mesh['void_cells'] == spec['void_cells'], mesh
        assert list(mesh).index('void_cells') == list(mesh).index('padded_cells')+1

    def test_summary(self, variant):
        if variant in ('writer', 'default'):
            result, out = self.invoke('summary' if variant == 'writer' else 'summary_default')
            self.expect(result)
            manifest = json.loads((out/'manifest.json').read_text())
            self.check_mesh(manifest, dict(void_cells=7 if variant == 'writer' else 0))
            assert {k: v for k, v in manifest['mesh'].items() if k != 'void_cells'} == dict(
                nx0=2, ny0=3, L=0, padded_cells=4, nz=1, mesh_sha256='a'*64)
            return
        spec = dict(cells=[cell(0, [0, 0, 0], [1, 1, 1]),
                           cell(9, [1, 0, 0], [2, 1, 1])], void_cells=7)
        result, out = self.invoke(spec=spec)
        self.expect(result)
        manifest, _ = self.fields(out)
        self.check_mesh(manifest, spec)
        # Canonical digest of this literal two-cell fixture (GIDs 1..8 absent).
        points = [(0,0,0),(1,0,0),(1,1,0),(0,1,0),(0,0,1),(1,0,1),(1,1,1),(0,1,1),
                  (2,0,0),(2,1,0),(2,0,1),(2,1,1)]
        conn = list(range(8))+[1,8,9,2,5,10,11,6]
        raw = struct.pack('<6q', 2, 1, 0, 1, 12, 2)
        raw += b''.join(struct.pack('<3d', *p) for p in points)
        raw += struct.pack('<16q', *conn)
        assert manifest['mesh']['mesh_sha256'] == hashlib.sha256(raw).hexdigest()
        assert set(manifest['mesh']) == {'nx0', 'ny0', 'L', 'padded_cells', 'void_cells',
                                        'nz', 'mesh_sha256'}

    def test_ranks(self, mismatch, ranks):
        spec = dict(cells=[cell(0, [0, 0, 0], [1, 1, 1])], void_cells=0)
        result, _ = self.invoke('barrier', fixture='ranks_mismatch' if mismatch else 'ranks_match',
                                spec=spec, ranks=ranks)
        self.expect(result, 'INTERNAL' if mismatch else 80,
                    '' if mismatch else 'assembly reached')
        reports = self.reports(result)
        assert len(reports) == ranks, reports
        for report in reports:
            assert report['provider_calls'] == (0 if mismatch else 1), report
            assert report['thermal_assemblies'] == 0, report
            assert report['mechanical_assemblies'] == (0 if mismatch else 1), report

    def test_empty(self):
        result, _ = self.invoke('mesh', spec=dict(cells=[], void_cells=7))
        self.expect(result, 'INPUT_INVALID', 'empty mesh')

    def chamfer(self, outline=False, radius=.002):
        w, h, c = .056, .044, radius
        polygon = [[c,0],[w-c,0],[w,c],[w,h-c],[w-c,h],[c,h],[0,h-c],[0,c]]
        self.stack['outline'] = dict(box=[0,0,w,h])
        if outline:
            self.stack['outline']['polygon'] = polygon
        self.stack['layers'] = [layer('lid_layer', .001, mat=None,
                                      regions=[region(box=[0,0,w,h], polygon=polygon)])]
        self.case['constraints'] = dict(fixed=[dict(target='layer:lid_layer')])
        self.case['mesh'].update(nx=4, ny=4, cut=dict(quad_degree=6))

    def test_cut(self, variant):
        self.chamfer(outline=variant == 'solve')
        result, out = self.invoke('quad' if variant in ('quad6', 'volume') else 'pipeline',
                                  cli=variant == 'solve')
        self.expect(result)
        if variant in ('quad6', 'volume'):
            report = self.reports(result)[0]
            assert report['cut_cells'] > 0
            assert report['max_quadrature_relative_error'] <= 1e-12
            expected = (56*44-2*2**2)*1e-9
            assert abs(report['volume']/expected-1) <= 1e-12, (report, expected)
        else:
            import h5py
            manifest = json.loads((out/'manifest.json').read_text())
            with h5py.File(out/manifest['steps'][0]['file']) as f:
                assert sum(f['VTKHDF/cut/NumberOfCells'][:]) > 0

    def test_rotated(self):
        # One bridged ring, no polygon hole encoding; 15 degree rotation.
        ring = [(-.3,-.3),(.3,-.3),(.3,.3),(-.3,.3),(-.3,-.3),
                (-.2,-.2),(-.2,.2),(.2,.2),(.2,-.2),(-.2,-.2)]
        angle = math.pi/12
        polygon = [[.5+x*math.cos(angle)-y*math.sin(angle),
                    .5+x*math.sin(angle)+y*math.cos(angle)] for x, y in ring]
        self.stack['layers'] = [layer('lid_layer', mat=None,
                                      regions=[region(box=[.1,.1,.9,.9], polygon=polygon)])]
        self.case['constraints'] = dict(fixed=[dict(target='layer:lid_layer')])
        # Resolve the outer clearance with h <= .05 m, so the >= .13 m
        # separation passes the grid's 2h gate before nonconvex plane rejection.
        self.case['mesh'].update(nx=16, ny=16)
        result, _ = self.invoke(cli=True)
        self.expect(result, 'CUT_UNSUPPORTED', 'non-convex polygon')

    def test_gate(self, covered, reverse=False):
        self.chamfer()
        if covered:
            result, _ = self.invoke('mesh')
            self.expect(result)
            assert self.reports(result)[0]['cut_cells'] > 0, 'region-only cut on box outline'
            assert self.reports(result)[-1]['calls'] == ['grid', 'planes', 'cut4']
            return
        # No polygons in source; bypass the upstream mesh/plane precondition to
        # exercise specifically the solver's body coverage gate.
        self.stack['layers'] = [layer('base'), layer('lid_layer')]
        self.case['constraints'] = dict(fixed=[dict(target='layer:base')])
        spec = self.disconnected(reverse=reverse)
        spec['pending'] = [i for i, c in enumerate(spec['cells']) if c['body']]
        result, _ = self.invoke('mesh', fixture='missing_planes', spec=spec)
        self.expect(result, 'CUT_UNSUPPORTED', 'pending cut cell without cut planes: alpha')
        assert self.reports(result)[-1]['calls'] == ['grid', 'planes']

    def test_cut_receiver(self):
        self.chamfer()
        result, _ = self.invoke('mesh_no_receiver')
        self.expect(result, 'CUT_UNSUPPORTED', 'cut data receiver is required')

    def test_cut_convection_alias(self):
        import h5py
        import numpy as np
        self.chamfer(outline=True)
        original = self.stack['layers'][0]
        self.stack['layers'] = [dict(original, name='lid_layer#'+str(k), thickness=.0005,
                                    regions=[dict(r, name=r['name']+'#'+str(k))
                                             for r in original['regions']])
                                for k in range(2)]
        self.case['constraints'] = dict(fixed=[dict(target='layer:lid_layer')])
        self.thermal()
        fixed = dict(type='fixed_temperature', target='face:zmin', T=300)
        values = []
        for targets in (['lid_layer'], ['lid_layer#0', 'lid_layer#1']):
            self.case['loads'] = [fixed]+[dict(type='convection', target='layer:'+name,
                                              h=20, T_inf=340) for name in targets]
            result, out = self.invoke()
            self.expect(result)
            manifest = json.loads((out/'manifest.json').read_text())
            with h5py.File(out/manifest['steps'][0]['file']) as f:
                assert sum(f['VTKHDF/cut/NumberOfCells'][:]) > 0
                values.append(np.concatenate([f['VTKHDF/'+block+'/PointData/T'][:]
                                               for block in ('hex','cut')]))
        np.testing.assert_allclose(values[0],values[1],atol=1e-10,rtol=0)

    def test_rounded(self):
        # Constant-section lid bonded directly to a substrate. No hat profile.
        import numpy as np
        values = []
        for resolution in (2, 3):
            pair = []
            for rounded in (False, True):
                w, h, r = .056, .044, 1e-7
                polygon = None
                if rounded:
                    polygon = []
                    for cx, cy, start in ((w-r,r,-90),(w-r,h-r,0),
                                           (r,h-r,90),(r,r,180)):
                        for step in range(5):
                            a = math.radians(start+90*step/4)
                            polygon.append([cx+r*math.cos(a),cy+r*math.sin(a)])
                # Long straight lid edges coincide with the outline, as allowed
                # by the region precondition; coarse grids need no artificial
                # narrow clearance strip that could fail the upstream 2h gate.
                self.stack['outline'] = dict(box=[0,0,w,h])
                self.stack['layers'] = [layer('base',.002),
                    layer('lid_layer',.001,mat=None,
                          regions=[region(box=[0,0,w,h], polygon=polygon)])]
                self.case['constraints'] = dict(rigid='3-2-1')
                self.case['mesh'].update(nx=resolution,ny=resolution,cut=dict(quad_degree=6))
                result, out = self.invoke(cli=True)
                self.expect(result)
                manifest = json.loads((out/'manifest.json').read_text())
                value = next(s['value'] for s in manifest['summary'] if s['name'] == 'warpage_pv')
                assert np.isfinite(value) and value > 0
                pair.append(value)
            assert abs(pair[1]/pair[0]-1) <= .01, (resolution,pair)
            values.append(pair)
        print('two-grid rounded/box warpage:', values)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--emit', type=Path)
    parser.add_argument('--driver')
    parser.add_argument('--solve')
    parser.add_argument('--source', type=Path)
    parser.add_argument('--mpiexec', default='mpiexec')
    parser.add_argument('--test')
    args = parser.parse_args()
    if args.emit:
        args.emit.mkdir(parents=True, exist_ok=True)
        path = args.emit/'parts_driver.cpp'
        if not path.exists() or path.read_text() != DRIVER:
            path.write_text(DRIVER)
        return
    with tempfile.TemporaryDirectory(prefix='sp6-parts-') as tmp:
        t = Acceptance(args, Path(tmp))
        tests = {
            'region_material': lambda: t.test_region_material(),
            'region_material_missing': lambda: t.test_region_material(missing=True),
            'copper_material_missing': lambda: t.test_region_material(copper=True),
            'omitted_cells': lambda: t.test_omitted(),
            'unused_attribute': lambda: t.test_omitted(unused=True),
            'used_unmapped': lambda: t.test_omitted(used_unmapped=True),
            'omitted_mpi2': lambda: t.test_omitted(ranks=2),
            'omitted_mpi3': lambda: t.test_omitted(ranks=3),
            'shared_node': lambda: t.test_node_identity(True),
            'coincident_distinct_nodes': lambda: t.test_node_identity(False),
            'thermal_only': t.test_thermal_only,
            'thermal_only_no_constraints': lambda: t.test_thermal_only(no_constraints=True),
            'thermal_only_rigid_cut': t.test_thermal_only_rigid_cut,
            'mechanical_alias': t.test_mechanical_alias,
            'convection_alias': t.test_convection_alias,
            'convection_cut_alias': lambda: t.test_convection_alias(cut=True),
            'heat_split': lambda: t.test_heat(),
            'heat_q_vol': lambda: t.test_heat(q_vol=True),
            'heat_additive': lambda: t.test_heat(additive=True),
            'heat_load_intact': t.test_heat_load_intact,
            'heat_cut_volume': t.test_heat_cut_volume,
            'heat_zero_volume': t.test_zero_volume,
            'empty_mesh': t.test_empty,
            'region_ls_cut_quad6': lambda: t.test_cut('quad6'),
            'cut_chamfer_lid_volume': lambda: t.test_cut('volume'),
            'solve_chamfer_lid_cut': lambda: t.test_cut('solve'),
            'solve_ring_rotated_c': t.test_rotated,
            'solve_lid_rounded': t.test_rounded,
            'pending_covered': lambda: t.test_gate(True),
            'pending_missing': lambda: t.test_gate(False),
            'pending_missing_reverse': lambda: t.test_gate(False, reverse=True),
            'region_cut_receiver': t.test_cut_receiver,
            'cut_convection_alias': t.test_cut_convection_alias,
        }
        for variant in ('lid','reverse','plate','constrained','thermal_constraint','cut',
                        'anchors','anchors_connected','multiple','multiple_reverse','names','mixed',
                        'mechanical'):
            tests['floating_'+variant] = lambda v=variant: t.test_floating(v)
        for variant in ('split','exact','case','gap','missing_zero','prefix','numeric',
                        'bad_suffix','leading_zero','negative'):
            tests['alias_'+variant] = lambda v=variant: t.test_alias(v)
        for variant in ('split', 'exact', 'exact_malformed', 'numeric'):
            tests['alias_load_'+variant] = lambda v=variant: t.test_alias(v, loader=True)
        for variant in ('writer','default','pipeline'):
            tests['summary_'+variant] = lambda v=variant: t.test_summary(v)
        for ranks in (1,2,3):
            for mismatch in (False, True):
                tests[f'ranks_{"mismatch" if mismatch else "match"}_np{ranks}'] = (
                    lambda r=ranks,m=mismatch: t.test_ranks(m,r))
        tests[args.test]()


if __name__ == '__main__':
    main()
