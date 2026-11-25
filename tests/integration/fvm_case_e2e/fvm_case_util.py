"""Synthetic fixtures, CLI execution, partition-aware readback and strict CAELOG1 decoding."""

from copy import deepcopy
import errno
import hashlib
import json
import math
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

THERMAL_ONLY = 'not solved: fvm thermal-only stage'
ENERGY_PREFIX = 'FVM_ENERGY '
ELASTIC_PREFIX = 'FVM_ELASTIC '
WARPAGE_METHOD = 'FVM top-face centroids; least-squares plane removal'
DISPLACEMENT_METHOD = 'FVM cell centers'
REQUIRE_COPPER_FIELDS = False


def _option_tokens(tokens):
    """Keep driver options and their operands together before CMake sees them."""
    paired = {'-arch', '-target', '--target', '-framework', '-Xlinker', '-isysroot',
              '--sysroot', '-F', '-L', '-iframework', '-isystem', '-I', '-iquote',
              '-o', '-c', '-MT', '-MF', '-MQ'}
    options = iter(tokens)
    for flag in options:
        if flag in paired:
            try:
                value = next(options)
            except StopIteration:
                raise ValueError(f'missing argument for {flag}') from None
            yield (flag, value)
        else:
            yield (flag,)


def build_adapter_probe(root, source, contract=None):
    """Compile private adapters in a temporary TU, using the CLI's actual build flags.

    No public hook or repository CMake change is needed. Ninja's command database
    and Make's link.txt also work when compile_commands.json was not requested.
    Build commands deliberately have no timeout: shared-slot queueing is expected.
    """
    binary = Path(os.environ['CHIPCAE_BINARY_DIR'])
    executable = Path(os.environ['CHIPCAE_SOLVE_EXE'])
    target = executable.relative_to(binary).as_posix()
    if (binary / 'build.ninja').exists():
        commands = subprocess.run(['ninja', '-C', str(binary), '-t', 'commands', target],
                                  capture_output=True, text=True, check=True).stdout.splitlines()
        compile_line = next(line for line in commands if ' -c ' in line
                            and 'fvm_pipeline.cpp' in line)
        link_line = commands[-1]
        # CMake surrounds a Ninja link command with ': && ... && :'.
        link_line = link_line.removeprefix(': && ').removesuffix(' && :')
        link = shlex.split(link_line)
        compile_flags = shlex.split(compile_line)
        compiler = link[0]
        compile_flags = compile_flags[compile_flags.index(compiler) + 1:]
    else:
        directory = executable.parent / 'CMakeFiles/chipcae-solve.dir'
        link = shlex.split((directory / 'link.txt').read_text())
        compiler = link[0]
        variables = dict(line.split(' = ', 1) for line in
                         (directory / 'flags.make').read_text().splitlines() if ' = ' in line)
        compile_flags = shlex.split(' '.join(variables[key] for key in
                                            ('CXX_DEFINES', 'CXX_INCLUDES', 'CXX_FLAGS')))
    flags = []
    for option in _option_tokens(compile_flags):
        flag = option[0]
        if flag in ('-o', '-c', '-MT', '-MF', '-MQ'):
            continue
        elif flag in ('-MD', '-MMD'):
            continue
        elif flag in ('-isystem', '-I', '-iquote'):
            # CMake de-duplicates options; keep each include flag with its path.
            flags.append(flag + option[1])
        elif len(option) == 2:
            flags.append('SHELL:' + shlex.join(option))
        else:
            flags.append(flag)
    libraries = []
    link_options = []
    linker_controls = {'--start-group', '--end-group', '--whole-archive', '--no-whole-archive',
                       '--push-state', '--pop-state', '--as-needed', '--no-as-needed',
                       '-Bstatic', '-Bdynamic'}
    for option in _option_tokens(link[1:]):
        flag = option[0]
        if flag == '-o' or (len(option) == 1 and flag.endswith(('.o', '.obj'))):
            continue
        # These controls must stay between their libraries. LINK_OPTIONS would
        # move them ahead of every archive and change the meaning of the link.
        if (flag in linker_controls or
                (flag.startswith('-Wl,') and linker_controls.intersection(flag[4:].split(','))) or
                (flag == '-Xlinker' and option[1] in linker_controls)):
            libraries.append(shlex.join(option) if len(option) == 2 else flag)
        elif flag.startswith('-l') or not flag.startswith('-'):
            libraries.append(str(binary / flag) if (binary / flag).is_file() else flag)
        else:
            link_options.append('SHELL:' + shlex.join(option) if len(option) == 2 else flag)
    quote = lambda value: '[=[' + str(value) + ']=]'
    # The linked loader resolves installed schemas relative to its executable.
    schemas = root / 'share/chipcae/schema'
    schemas.mkdir(parents=True)
    for relative in ('schema/stack-0.2.schema.json', 'material/schema/material.v1.schema.json'):
        shutil.copyfile(source / relative, schemas / Path(relative).name)
    (root / 'adapter.cpp').write_text(ADAPTER_PROBE, encoding='utf-8')
    (root / 'replay.cpp').write_text(ELASTIC_REPLAY, encoding='utf-8')
    (root / 'pipeline_failure.cpp').write_text(PIPELINE_FAILURE_PROBE, encoding='utf-8')
    cmake = ['cmake_minimum_required(VERSION 3.20)',
             'project(fvm_adapter_acceptance LANGUAGES CXX)',
             'add_executable(adapter_probe adapter.cpp)',
             'target_compile_options(adapter_probe PRIVATE ' + ' '.join(map(quote, flags)) + ')',
             'target_compile_definitions(adapter_probe PRIVATE ' +
             quote('CHIPCAE_FVM_ADAPTER_SOURCE="' +
                   str(source / 'apps/chipcae-solve/src/fvm_pipeline.cpp') + '"') + ')',
             'target_link_libraries(adapter_probe PRIVATE ' + ' '.join(map(quote, libraries)) + ')']
    cmake += ['add_executable(elastic_replay replay.cpp)',
              'target_compile_options(elastic_replay PRIVATE ' + ' '.join(map(quote, flags)) + ')',
              'target_link_libraries(elastic_replay PRIVATE ' + ' '.join(map(quote, libraries)) + ')']
    cmake += ['add_executable(pipeline_failure_probe pipeline_failure.cpp)',
              'target_compile_options(pipeline_failure_probe PRIVATE ' +
              ' '.join(map(quote, flags)) + ')',
              'target_compile_definitions(pipeline_failure_probe PRIVATE ' +
              quote('CHIPCAE_FVM_ADAPTER_SOURCE="' +
                    str(source / 'apps/chipcae-solve/src/fvm_pipeline.cpp') + '"') + ')',
              'target_link_libraries(pipeline_failure_probe PRIVATE ' +
              ' '.join(map(quote, libraries)) + ')']
    if contract is not None:
        cmake += ['add_executable(contract_probe ' + quote(contract / 'dump_actual.cpp') + ')',
                  'target_compile_options(contract_probe PRIVATE ' + ' '.join(map(quote, flags)) + ')',
                  'target_link_libraries(contract_probe PRIVATE ' + ' '.join(map(quote, libraries)) + ')']
    probe_targets = ['adapter_probe', 'elastic_replay', 'pipeline_failure_probe']
    if contract is not None:
        probe_targets.append('contract_probe')
    for probe in probe_targets:
        cmake.append('target_link_options(' + probe + ' PRIVATE ' +
                     ' '.join(map(quote, link_options)) + ')')
    (root / 'CMakeLists.txt').write_text('\n'.join(cmake) + '\n', encoding='utf-8')
    build = root / 'build'
    for args in (['cmake', '-G', 'Ninja', '-S', str(root), '-B', str(build), '-DCMAKE_BUILD_TYPE=Release',
                  '-DCMAKE_CXX_COMPILER=' + compiler],
                 ['cmake', '--build', str(build), '-j2']):
        result = subprocess.run(args, capture_output=True, text=True)
        assert result.returncode == 0, f'{shlex.join(args)}\n{result.stdout}\n{result.stderr}'
    return build / 'adapter_probe', build / 'contract_probe', build / 'elastic_replay'


# The implementation is included only in this temporary test translation unit.
# JSON exposes actual adapter outputs; the Python tests derive their own oracle.
ADAPTER_PROBE = r'''
#include CHIPCAE_FVM_ADAPTER_SOURCE
#include <iostream>

using namespace chipcae;
using namespace chipcae::solve;
using Json = jsoncons::json;

struct Fixture {
    RunInputs inputs;
    mesh::HexMeshArrays arrays;
    fv::FvGrid grid;
    Fixture() {
        for (const auto* name : {"base", "support#1", "support#0"}) {
            mesh::Layer layer;
            layer.name = name;
            layer.copper = mesh::CopperSpec{"metal", {}, {}};
            inputs.stack.layers.push_back(layer);
        }
        inputs.materials["matrix"].E = 25e9;
        inputs.materials["matrix"].nu = .2;
        inputs.materials["matrix"].alpha = 1e-5;
        inputs.materials["metal"].E = 110e9;
        inputs.materials["metal"].nu = .34;
        inputs.materials["metal"].alpha = 3e-5;
        inputs.T_ref = 300;
        inputs.linear.rtol = 2e-9;
        inputs.linear.max_iter = 37;
        inputs.linear.preconditioner = "gamg";
        Load load;
        load.type = "uniform_temperature";
        load.T = 357;
        inputs.loads.push_back(load);
        arrays.attr_table.materials = {{"matrix", 17}, {"metal", 41}};
        arrays.attr_table.entries = {
            {"support#0", "", 17, 0}, {"base", "", 17, 0}, {"support#1", "", 17, 0}};
        grid.axes = {{{0, 1}, {0, 2}, {-4, 9}}};
        grid.cv_volume = {3, 2, 7};
        grid.cv_centroid = {{.2, .3, 1}, {.4, .6, 2}, {.8, 1, 3}};
        grid.cv_elem_gid = {81, 17, 45};
        grid.cv_rank = {1, 0, 1};
        grid.cv_attribute = {3, 1, 2};
        grid.cv_layer = {99, 43, 7}; // Neither material IDs nor these IDs are stack indices.
        grid.cv_kind = {0, 1, 2};
        grid.cv_copper_fraction = {0, .25, .75};
        grid.faces = {{2, 0, 0, -1, 2, {2, 0, 0}, {.3, .5, 2}, 0},
                      {1, -1, -1, 6, 5, {0, 3, 4}, {.4, .5, 9}, 1},
                      {0, -1, 2, 5, 3, {0, 0, 3}, {.2, .3, 9}, 2}};
    }
};

int main(int argc, char** argv) {
    PetscSession session;
    try {
        const std::string mode = argv[1];
        Fixture f;
        Json out;
        if (mode == "mesh") {
            const auto m = to_elastic_mesh(f.grid, f.inputs, f.arrays);
            out["volume"] = m.volume;
            out["centroid"] = m.centroid;
            out["gid"] = m.gid;
            out["rank"] = m.rank;
            out["layer"] = m.layer;
            out["is_cut"] = std::vector<int>(m.is_cut.begin(), m.is_cut.end());
            out["is_embedded"] = std::vector<int>(m.is_embedded.begin(), m.is_embedded.end());
            out["z_top"] = m.z_top;
            out["faces"] = Json::array();
            for (const auto& face : m.faces) {
                Json j;
                j["lo"] = face.lo;
                j["hi"] = face.hi;
                j["side"] = face.side;
                j["area"] = face.area;
                j["normal"] = face.normal;
                j["centroid"] = face.centroid;
                out["faces"].push_back(j);
            }
        } else if (mode == "problem") {
            if (argc > 2)
                for (int i = 2; i < argc; ++i)
                    f.inputs.fixed.emplace_back(argv[i]);
            fv::ThermalResult thermal;
            thermal.T_cell = {310, 325, 292};
            const auto fractions = fv::copper_fraction(f.grid, f.arrays);
            const auto p = to_elastic_problem(f.inputs, f.grid, &thermal, f.arrays, fractions);
            const auto uniform = to_elastic_problem(f.inputs, f.grid, nullptr, f.arrays, fractions);
            out["C"] = p.C;
            out["alpha"] = p.alpha;
            out["dT"] = p.dT;
            out["uniform_dT"] = uniform.dT;
            out["rtol"] = p.rtol;
            out["max_iter"] = p.max_iter;
            out["preconditioner"] = p.preconditioner;
            out["fixed"] = p.support == fv::ElasticProblem::Support::Fixed;
            out["fixed_side"] = p.fixed_side;
            out["fixed_layer"] = p.fixed_layer;
        } else if (mode == "error") {
            try {
                fail({"fvm.elastic", argv[2]}, true);
            } catch (const InputError& e) {
                out["code"] = e.code;
                out["message"] = e.what();
            }
        } else if (mode == "snapshot") {
            const auto inputs = load_inputs(argv[2], argv[3], Communicator::world().size());
            std::optional<mesh::cut::CutLayerArrays> cut;
            const auto arrays = build_mesh(inputs, &cut);
            const auto built = fv::build_grid(arrays, cut ? &*cut : nullptr);
            if (!built)
                throw std::runtime_error(built.error().reason);
            const auto& grid = built.value();
            const auto fractions = fv::copper_fraction(grid, arrays);
            const auto thermal_input = thermal_problem(inputs, arrays, grid, fractions);
            fv::ThermalResult thermal;
            if (inputs.thermal) {
                const auto solved =
                    fv::solve_thermal(grid, arrays, thermal_input, Communicator::world());
                if (!solved)
                    throw std::runtime_error(solved.error().reason);
                thermal = solved.value();
            }
            const auto mesh = to_elastic_mesh(grid, inputs, arrays);
            const auto problem = to_elastic_problem(
                inputs, grid, inputs.thermal ? &thermal : nullptr, arrays, fractions);
            auto& m = out["mesh"];
            m["volume"] = mesh.volume;
            m["centroid"] = mesh.centroid;
            m["gid"] = mesh.gid;
            m["rank"] = mesh.rank;
            m["layer"] = mesh.layer;
            m["is_cut"] = std::vector<int>(mesh.is_cut.begin(), mesh.is_cut.end());
            m["is_embedded"] = std::vector<int>(mesh.is_embedded.begin(), mesh.is_embedded.end());
            m["z_top"] = mesh.z_top;
            m["faces"] = Json::array();
            for (const auto& face : mesh.faces) {
                Json j;
                j["lo"] = face.lo;
                j["hi"] = face.hi;
                j["side"] = face.side;
                j["area"] = face.area;
                j["normal"] = face.normal;
                j["centroid"] = face.centroid;
                m["faces"].push_back(j);
            }
            auto& p = out["problem"];
            p["C"] = problem.C;
            p["alpha"] = problem.alpha;
            p["dT"] = problem.dT;
            p["support"] =
                problem.support == fv::ElasticProblem::Support::Rigid321 ? "Rigid321" : "Fixed";
            p["fixed_side"] = problem.fixed_side;
            p["fixed_layer"] = problem.fixed_layer;
            p["rtol"] = problem.rtol;
            p["max_iter"] = problem.max_iter;
            p["preconditioner"] = problem.preconditioner;
            // Raw library inputs, independent of the mechanics adapters under test.
            auto& raw = out["raw"];
            raw["volume"] = grid.cv_volume;
            raw["centroid"] = grid.cv_centroid;
            raw["gid"] = grid.cv_elem_gid;
            raw["rank"] = grid.cv_rank;
            raw["kind"] = grid.cv_kind;
            raw["attribute"] = grid.cv_attribute;
            raw["attributes"] = Json::array();
            for (const auto& entry : arrays.attr_table.entries) {
                Json j;
                j["layer"] = entry.layer;
                for (const auto& [name, id] : arrays.attr_table.materials)
                    if (id == entry.material)
                        j["material"] = name;
                raw["attributes"].push_back(j);
            }
            raw["faces"] = Json::array();
            for (const auto& face : grid.faces) {
                Json j;
                j["lo"] = face.lo;
                j["hi"] = face.hi;
                j["side"] = face.side;
                j["area"] = face.area;
                j["area_vector"] = face.area_vector;
                j["centroid"] = face.centroid;
                raw["faces"].push_back(j);
            }
            raw["axes"] = grid.axes;
            raw["nz"] = grid.nz;
            raw["fraction"] = fractions;
            raw["thermal_k"] = thermal_input.k;
            raw["T_cell"] = thermal.T_cell;
            raw["constituent_off"] = grid.constituent_off;
            raw["constituent_gid"] = grid.constituent_gid;
            raw["copper_phi"] = arrays.copper_phi;
            raw["copper_layer_of_layer"] = arrays.copper_layer_of_layer;
            raw["parent_gid"] = arrays.elem_gid;
            raw["parent_attribute"] = arrays.attribute;
            raw["parent_volume"] = Json::array();
            raw["parent_physical_volume"] = Json::array();
            for (std::size_t e = 0; e < arrays.elem_gid.size(); ++e) {
                Point lo{1e100, 1e100, 1e100}, hi{-1e100, -1e100, -1e100};
                for (int v = 0; v < 8; ++v)
                    for (int d = 0; d < 3; ++d) {
                        const auto x = arrays.vertices[3 * arrays.hex[8 * e + v] + d];
                        lo[d] = std::min(lo[d], x);
                        hi[d] = std::max(hi[d], x);
                    }
                const double volume = (hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]);
                double fraction = 1;
                if (cut)
                    for (std::size_t k = 0; k < cut->cut_elem_gid.size(); ++k)
                        if (cut->cut_elem_gid[k] == arrays.elem_gid[e]) {
                            fraction = 0;
                            for (auto s = cut->sub_off[k]; s < cut->sub_off[k + 1]; ++s)
                                fraction += cut->sub_volfrac[s];
                        }
                raw["parent_volume"].push_back(volume);
                raw["parent_physical_volume"].push_back(volume * fraction);
            }
        } else if (mode == "lookup") {
            f.arrays.elem_gid = {81, 17, 45, 45, 93};
            f.grid.constituent_off = {0, 1, 2, 5};
            f.grid.constituent_elem = {0, 1, 3, 2, 4};
            f.grid.constituent_gid = {81, 17, 45, 45, 93};
            f.grid.constituent_subcell = {0, 0, 3, 0, 1};
            out = Json::array();
            for (const auto& [key, cv] : constituent_lookup(f.grid, f.arrays))
                out.push_back(std::array<std::int64_t, 3>{
                    key.first, key.second, static_cast<std::int64_t>(cv)});
        } else if (mode == "top") {
            fv::ElasticMesh mesh;
            mesh.volume = {1};
            mesh.gid = {81};
            mesh.z_top = 9;
            mesh.faces = {{0, -1, 5, 1, {0, 0, 1}, {0, 0, 9}},
                          {0, -1, -1, 1, {0, 0, 1}, {0, 0, 9 - 5e-14}},
                          {0, -1, -1, 1, {0, 0, .99}, {0, 0, 9}},
                          {0, -1, -1, 1, {0, 0, 1}, {0, 0, 9 - 2e-13}},
                          {0, -1, -1, 1, {0, 0, 1}, {0, 0, 9 + 2e-13}},
                          {0, 0, 5, 1, {0, 0, 1}, {0, 0, 9}}};
            mesh.is_embedded = {0, 1, 1, 1, 1, 0};
            fv::ElasticResult result;
            result.U_cell.resize(1);
            result.U_face.resize(6);
            result.face_prescribed.resize(6);
            result.min_dn_gid = 81;
            out = Json::array();
            for (int count = 0; count <= 6; ++count) {
                result.top_samples.resize(count);
                try {
                    validate_elastic_result(mesh, result);
                    out.push_back(count);
                } catch (const InputError&) {
                }
            }
        } else if (mode == "ring") {
            const std::vector<Point> ring{
                {0, 0, 0}, {2, 0, 2}, {2, 1, 2}, {1, 1, 1}, {1, 2, 1}, {0, 2, 0}};
            fv::FvFace face;
            face.side = 6;
            face.area = 3 * std::sqrt(2.);
            face.area_vector = {-3, 0, 3};
            face.centroid = {.8, .8, .8};
            out = Json::array();
            for (const Point p : {Point{.5, 1.5, .5},
                                  Point{1.5, 1.5, 1.5},
                                  Point{1, 1.5, 1},
                                  Point{1, 1, 1},
                                  Point{0, 0, 0},
                                  Point{.5, .5, .6}})
                out.push_back(on_ring(p, ring, face, 1e-12));
        } else if (mode == "reconstruct") {
            const auto inputs = load_inputs(argv[2], argv[3], 1);
            auto arrays = build_mesh(inputs);
            const auto built = fv::build_grid(arrays);
            if (!built)
                throw std::runtime_error(built.error().reason);
            auto grid = built.value();
            // Manufactured CV data isolates reconstruction from the elastic solve.
            // One merged CV has weight 3; its copied parent/subcell must count once.
            grid.cv_volume.assign(8, 1);
            grid.cv_volume[0] = 3;
            grid.cv_kind[0] = 2;
            fv::ElasticResult elastic;
            for (int c = 0; c < 8; ++c) {
                const double x = (c + 1) * 1e-6;
                elastic.U_cell.push_back({x, 2 * x, -3 * x});
            }
            for (const auto& face : grid.faces) {
                const double x = face.side == 4 ? 0 : (face.side + 1) * 1e-6;
                elastic.U_face.push_back({x, 2 * x, -3 * x});
                elastic.face_prescribed.push_back(face.hi < 0 && face.side == 4);
            }
            const auto original =
                vertex_U(arrays, grid, elastic, nullptr, constituent_lookup(grid, arrays), false);
            grid.constituent_elem.insert(grid.constituent_elem.begin(), 0);
            grid.constituent_gid.insert(grid.constituent_gid.begin(), arrays.elem_gid[0]);
            grid.constituent_subcell.insert(grid.constituent_subcell.begin(), 1);
            for (std::size_t c = 1; c < grid.constituent_off.size(); ++c)
                ++grid.constituent_off[c];
            // Copied coordinates have distinct storage indices, as cut output does.
            for (int v = 0; v < 8; ++v) {
                const auto old = arrays.hex[v];
                const Point p{arrays.vertices[3 * old],
                              arrays.vertices[3 * old + 1],
                              arrays.vertices[3 * old + 2]};
                arrays.hex[v] = static_cast<mesh::Local>(arrays.vertices.size() / 3);
                arrays.vertices.insert(arrays.vertices.end(), p.begin(), p.end());
            }
            const auto lookup = constituent_lookup(grid, arrays);
            for (const bool hex : {false, true}) {
                Json values = Json::array();
                for (const auto& [key, value] :
                     vertex_U(arrays, grid, elastic, nullptr, lookup, hex)) {
                    Json j;
                    j["key"] = key;
                    j["U"] = value;
                    j["before"] = original.at(key);
                    values.push_back(j);
                }
                out[hex ? "hex" : "physical"] = values;
            }
        } else if (mode == "fractions") {
            auto inputs = load_inputs(argv[2], argv[3], 1);
            std::optional<mesh::cut::CutLayerArrays> cut;
            auto arrays = build_mesh(inputs, &cut);
            const auto columns = (arrays.axes[0].size() - 1) * (arrays.axes[1].size() - 1);
            arrays.copper_layer_of_layer = {1, 0};
            arrays.copper_phi.clear();
            for (std::size_t layer = 0; layer < 2; ++layer)
                for (std::size_t col = 0; col < columns; ++col)
                    arrays.copper_phi.push_back(.05 + .1 * layer + .02 * col);
            if (cut) {
                cut->small_subs.clear();
                if (argc > 4)
                    cut->small_subs.push_back(0);
            }
            const auto result = fv::build_grid(arrays, cut ? &*cut : nullptr);
            if (!result)
                throw std::runtime_error(result.error().reason);
            const auto& g = result.value();
            out["fraction"] = fv::copper_fraction(g, arrays);
            out["kind"] = g.cv_kind;
            out["gid"] = g.cv_elem_gid;
            out["volume"] = g.cv_volume;
            out["nz"] = g.nz;
            out["columns"] = columns;
            out["off"] = g.constituent_off;
            out["constituent_gid"] = g.constituent_gid;
            out["constituent_subcell"] = g.constituent_subcell;
            out["parent_volume"] = Json::array();
            out["parent_fraction"] = Json::array();
            out["parent_layer"] = Json::array();
            for (std::size_t e = 0; e < arrays.elem_gid.size(); ++e) {
                const auto gid = arrays.elem_gid[e];
                const auto& record = arrays.attr_table.entries[arrays.attribute[e] - 1];
                out["parent_layer"].push_back(stack_layer(inputs, record.layer));
                Point lo{1e100, 1e100, 1e100}, hi{-1e100, -1e100, -1e100};
                for (int v = 0; v < 8; ++v)
                    for (int d = 0; d < 3; ++d) {
                        const double x = arrays.vertices[3 * arrays.hex[8 * e + v] + d];
                        lo[d] = std::min(lo[d], x);
                        hi[d] = std::max(hi[d], x);
                    }
                out["parent_volume"].push_back((hi[0] - lo[0]) * (hi[1] - lo[1]) * (hi[2] - lo[2]));
                double vf = 1;
                if (cut)
                    for (std::size_t k = 0; k < cut->cut_elem_gid.size(); ++k)
                        if (cut->cut_elem_gid[k] == gid)
                            vf = cut->sub_volfrac[cut->sub_off[k]];
                out["parent_fraction"].push_back(vf);
            }
            out["parent_gid"] = arrays.elem_gid;
        } else {
            throw std::runtime_error("unknown adapter probe mode");
        }
        if (Communicator::world().rank() == 0)
            std::cout << out << '\n';
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
'''


# Supply an opaque I14 failure at the library boundary. The included production
# pipeline still builds the mesh, solves thermal, adapts inputs and writes events.
# The numerical solver has separate real-library coverage; it cannot reliably
# generate every frozen error prefix from a valid public case.
PIPELINE_FAILURE_PROBE = r'''
#include CHIPCAE_FVM_ADAPTER_SOURCE
#include <iostream>

namespace {
std::string failure_reason;
}

namespace chipcae::solvers::fvm {
Expected<ElasticResult> solve_elastic(const ElasticMesh&, const ElasticProblem&, MPI_Comm comm) {
    // Retain a collective boundary and fail visibly if a rank skips the call.
    MPI_Barrier(comm);
    return chipcae::mesh::Error{"fvm.elastic", failure_reason};
}
} // namespace chipcae::solvers::fvm

int main(int argc, char** argv) {
    chipcae::solve::PetscSession session;
    try {
        if (argc != 5)
            throw std::runtime_error("expected case, profile, output and library reason");
        failure_reason = argv[4];
        const auto inputs =
            chipcae::solve::load_inputs(argv[1], argv[2], chipcae::Communicator::world().size());
        return chipcae::solve::run_fvm(inputs, argv[3]);
    } catch (const chipcae::solve::InputError& error) {
        std::cerr << error.what() << '\n';
        return error.code;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
'''


# Deliberately includes only public I14, PETSc and JSON headers. It never calls
# private adapters, reconstruction, event writers or CLI result publication.
ELASTIC_REPLAY = r'''
#include "chipcae/solvers/fvm/elastic.hpp"

#include <fstream>
#include <iostream>
#include <jsoncons/json.hpp>
#include <petscsys.h>

using Json = jsoncons::json;
namespace fv = chipcae::solvers::fvm;

int main(int argc, char** argv) {
    PetscInitialize(&argc, &argv, nullptr, nullptr);
    int code = 0, rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    try {
        if (argc != 2)
            throw std::runtime_error("expected elastic input JSON path");
        std::ifstream input(argv[1]);
        const auto json = Json::parse(input);
        const auto& m = json.at("mesh");
        const auto& p = json.at("problem");
        fv::ElasticMesh mesh;
        mesh.volume = m.at("volume").as<decltype(mesh.volume)>();
        mesh.centroid = m.at("centroid").as<decltype(mesh.centroid)>();
        mesh.gid = m.at("gid").as<decltype(mesh.gid)>();
        mesh.layer = m.at("layer").as<decltype(mesh.layer)>();
        mesh.rank = m.at("rank").as<decltype(mesh.rank)>();
        mesh.is_cut = m.at("is_cut").as<decltype(mesh.is_cut)>();
        mesh.is_embedded = m.at("is_embedded").as<decltype(mesh.is_embedded)>();
        mesh.z_top = m.at("z_top").as<double>();
        for (const auto& f : m.at("faces").array_range())
            mesh.faces.push_back({f.at("lo").as<std::int64_t>(),
                                  f.at("hi").as<std::int64_t>(),
                                  f.at("side").as<int>(),
                                  f.at("area").as<double>(),
                                  f.at("normal").as<std::array<double, 3>>(),
                                  f.at("centroid").as<std::array<double, 3>>()});
        fv::ElasticProblem problem;
        problem.C = p.at("C").as<decltype(problem.C)>();
        problem.alpha = p.at("alpha").as<decltype(problem.alpha)>();
        problem.dT = p.at("dT").as<decltype(problem.dT)>();
        const auto support = p.at("support").as<std::string>();
        if (support != "Rigid321" && support != "Fixed")
            throw std::runtime_error("unknown replay support");
        problem.support = support == "Rigid321" ? fv::ElasticProblem::Support::Rigid321
                                                : fv::ElasticProblem::Support::Fixed;
        problem.fixed_side = p.at("fixed_side").as<decltype(problem.fixed_side)>();
        problem.fixed_layer = p.at("fixed_layer").as<decltype(problem.fixed_layer)>();
        problem.rtol = p.at("rtol").as<double>();
        problem.max_iter = p.at("max_iter").as<int>();
        problem.preconditioner = p.at("preconditioner").as<std::string>();
        const auto solved = fv::solve_elastic(mesh, problem, MPI_COMM_WORLD);
        Json result;
        result["ok"] = static_cast<bool>(solved);
        if (solved) {
            const auto& r = solved.value();
            result["warpage_pv"] = r.warpage_pv;
            result["U_mag_max"] = r.U_mag_max;
            result["warpage_method"] = r.warpage_method;
            result["U_mag_method"] = r.U_mag_method;
            result["min_dn_ratio"] = r.min_dn_ratio;
            result["min_dn_gid"] = r.min_dn_gid;
            result["gradient_corrections"] = r.gradient_corrections;
            result["max_identity_defect"] = r.max_identity_defect;
        } else {
            result["path"] = solved.error().path;
            result["reason"] = solved.error().reason;
        }
        if (rank == 0)
            std::cout << result << '\n';
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        code = 1;
    }
    PetscFinalize();
    return code;
}
'''


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def material(name, k):
    def table(columns):
        return dict(temperatures=[300.0], columns={key: [v] for key, v in columns.items()},
                    interpolation='constant', extrapolation='clamp')
    return dict(id=name, schema_version='1', unit_system='SI', model='thermo_elastic',
                density=2000, conductivity=k,
                elastic=dict(symmetry='isotropic', table=table(dict(E=25e9, nu=.3))),
                thermal_expansion=dict(symmetry='isotropic', cte_type='tangent',
                                       stress_free_temperature=300, table=table(dict(alpha=1e-5))),
                provenance=dict(source_type='illustrative', confidence='illustrative'))


def thermal_case(family='fvm'):
    return dict(
        schema='chipcae.case/1.0', stack=dict(ref='stack.json'),
        materials=dict(ref='materials.json'),
        mesh=dict(type='aligned_cartesian_cut', nx=4, ny=4, levels=0,
                  min_elems_per_layer=1, max_ratio=100,
                  cut=dict(stabilization='ghost_penalty', quad_degree=6, gamma_g=.1)),
        physics=[dict(id='thermal', solver=family + '.conduction', mode='steady'),
                 dict(id='mech', solver=family + '.solid', temp_from='thermal', T_ref=300)],
        loads=[dict(type='fixed_temperature', target='face:zmin', T=300),
               dict(type='convection', target='face:zmax', h=20, T_inf=400)],
        constraints=dict(rigid='3-2-1'),
        solver=dict(linear=dict(preconditioner='jacobi' if family == 'fvm' else 'direct',
                                rtol=1e-12, max_iter=2000)),
        output=dict(format='vtkhdf', warpage=dict(standard='JESD22-B112',
                                                remove_rigid='best_fit_plane')))


def solid_case(family='fvm'):
    case = thermal_case(family)
    case['physics'] = [dict(id='mech', solver=family + '.solid')]
    case['loads'] = [dict(type='uniform_temperature', T_ref=300, T_target=350)]
    return case


def bimaterial_strip(root):
    """Synthetic strip with equal moduli and an explicit 20 ppm/K CTE mismatch."""
    stack = fixture(root)
    stack['outline'] = dict(box=[0, 0, 1, .2])
    for layer in stack['layers']:
        layer.update(thickness=.05, n_elem_min=2)
    library = read_json(root / 'materials.json')
    for card, alpha in zip(library['materials'], (1e-5, 3e-5), strict=True):
        card['thermal_expansion']['table']['columns']['alpha'] = [alpha]
    write_json(root / 'stack.json', stack)
    write_json(root / 'materials.json', library)
    case = solid_case()
    case['mesh'].update(nx=12, ny=4)
    return case


def prescribed_temperature_strip(root):
    strip = bimaterial_strip(root)
    case = thermal_case()
    case['mesh'] = strip['mesh']
    # Prescribe every CV independently of mechanics. A finite heat flow between
    # layers keeps the unchanged relative energy gate away from a 0/0 ledger.
    case['loads'] = [dict(type='fixed_temperature', target='layer:a', T=340),
                     dict(type='fixed_temperature', target='layer:b', T=360)]
    return case


def fixture(root):
    root.mkdir(parents=True, exist_ok=True)
    stack = dict(id='fvm_synthetic', unit='m', symmetry='none', package_type='FCBGA',
                 outline=dict(box=[0, 0, 1, 1]), layers=[
                     dict(name=name, kind='Explicit', material=name, thickness=.05,
                          n_elem_min=1, grading=1) for name in ('a', 'b')])
    write_json(root / 'stack.json', stack)
    write_json(root / 'materials.json', dict(schema='chipcae.material_library/1',
                                            materials=[material('a', 2), material('b', 4)]))
    for ranks in (1, 2):
        write_json(root / f'profile{ranks}.json', dict(
            schema='chipcae.run_profile/1.0', backend='cpu', ranks=ranks, gpus=0))
    return stack


def measured_process(args, root):
    if not hasattr(os, 'wait4'):
        return subprocess.run(args, cwd=root, capture_output=True, text=True, timeout=500), None
    # Capture into files so neither pipe can block while wait4 obtains this child's
    # actual peak RSS. getrusage(RUSAGE_CHILDREN) would retain previous runs' peaks.
    with tempfile.TemporaryFile(mode='w+', encoding='utf-8') as stdout, \
            tempfile.TemporaryFile(mode='w+', encoding='utf-8') as stderr:
        with subprocess.Popen(args, cwd=root, stdout=stdout, stderr=stderr) as child:
            deadline = time.monotonic() + 500
            while True:
                pid, status, usage = os.wait4(child.pid, os.WNOHANG)
                if pid:
                    child.returncode = os.waitstatus_to_exitcode(status)
                    break
                if time.monotonic() > deadline:
                    child.kill()
                    child.wait()
                    raise subprocess.TimeoutExpired(args, 500)
                time.sleep(.01)
        stdout.seek(0)
        stderr.seek(0)
        result = subprocess.CompletedProcess(args, child.returncode, stdout.read(), stderr.read())
    rss = usage.ru_maxrss / 1024 if sys.platform == 'darwin' else usage.ru_maxrss
    return result, rss


def command(args, root, expected=0):
    args = [str(x) for x in args]
    print('COMMAND ' + shlex.join(args), flush=True)
    start = time.monotonic()
    try:
        result, rss = measured_process(args, root)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise AssertionError(f'command could not complete: {args!r}: {error}') from error
    print(f'实测 wall={time.monotonic() - start:.6f}s peak_rss_KiB={rss} '
          f'exit={result.returncode}', flush=True)
    if expected is not None:
        assert result.returncode == expected, (
            f'{shlex.join(args)}\nexit={result.returncode}, expected={expected}\n'
            f'stdout:\n{result.stdout}\nstderr:\n{result.stderr}')
    return result


def probe(root, case, expected=0, ranks=1):
    write_json(root / 'case.json', case)
    result = command([os.environ['CHIPCAE_FVM_LOADER_PROBE'], root / 'case.json',
                      root / f'profile{ranks}.json', ranks], root, expected)
    return (json.loads(result.stdout) if result.returncode == 0 else None), result


def normalize_paths(value, root):
    if isinstance(value, dict):
        return {k: normalize_paths(v, root) for k, v in value.items()}
    if isinstance(value, list):
        return [normalize_paths(v, root) for v in value]
    return value.replace(str(root.resolve()), '<fixture>') if isinstance(value, str) else value


def snapshot_fixture(root, name):
    stack = fixture(root)
    if name == 'copper':
        stack['layers'][0].update(kind='Patterned', copper=dict(metal='cu', fraction=.5))
        stack['layers'][1]['regions'] = [dict(
            name='patch', material='a', footprint=dict(box=[.25, .25, .75, .75]))]
        library = read_json(root / 'materials.json')
        library['materials'].append(material('cu', 400))
        write_json(root / 'materials.json', library)
        write_json(root / 'stack.json', stack)


def mpi_command(args, ranks):
    if ranks == 1:
        return args
    pre = [x for x in os.environ.get('CHIPCAE_MPIEXEC_PREFLAGS', '').split('|') if x]
    post = [x for x in os.environ.get('CHIPCAE_MPIEXEC_POSTFLAGS', '').split('|') if x]
    return [os.environ['CHIPCAE_MPIEXEC'],
            os.environ['CHIPCAE_MPIEXEC_NUMPROC_FLAG'], str(ranks), *pre, *args, *post]


def solve(root, case, name, ranks=1, extra=(), control=None, expected=0):
    write_json(root / 'case.json', case)
    out = root / name
    out.mkdir(exist_ok=True)
    if control is not None:
        (out / 'control.jsonl').write_text(control, encoding='utf-8')
    args = [os.environ['CHIPCAE_SOLVE_EXE'], '--case', root / 'case.json', '--profile',
            root / f'profile{ranks}.json', '--out', out, *extra]
    return out, command(mpi_command(args, ranks), root, expected)


def solve_with_late_stop(root, case, name, ranks=1):
    """Queue STOP after the first control checkpoint without a timing race.

    A FIFO supplies an unsupported PAUSE at the pre-mechanics checkpoint.
    Once its acknowledgment is visible, STOP is queued for the next read.
    The writer cannot leak STOP into the first read, and the solve needs a
    post-mechanics checkpoint to acknowledge it before publishing output.
    """
    out = root / name
    out.mkdir()
    fifo = out / 'control.jsonl'
    os.mkfifo(fifo)
    done = threading.Event()
    errors = []
    def write_control(text):
        while not done.is_set():
            try:
                descriptor = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
            except OSError as error:
                if error.errno != errno.ENXIO:  # No reader at this checkpoint yet.
                    raise
                done.wait(.001)
                continue
            with os.fdopen(descriptor, 'w') as stream:
                stream.write(text)
            return True
        return False
    def control():
        try:
            if not write_control(json.dumps(dict(cmd_seq='1', action='PAUSE')) + '\n'):
                return
            while not done.is_set():
                try:
                    events = decode_events((out / 'events.bin').read_bytes())
                except (FileNotFoundError, AssertionError):
                    done.wait(.001)
                    continue
                if any(e['kind'] == 17 and e['cmd_seq'] == 1 for e in events):
                    write_control(json.dumps(dict(cmd_seq='2', action='STOP')) + '\n')
                    return
                done.wait(.001)
        except Exception as error:
            errors.append(error)
    worker = threading.Thread(target=control, daemon=True)
    worker.start()
    try:
        result = solve(root, case, name, ranks)
    finally:
        done.set()
        worker.join(timeout=2)
    assert not worker.is_alive(), 'control fixture did not finish'
    assert not errors, f'control fixture failed: {errors}'
    return result


class Cursor:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, fmt):
        size = struct.calcsize('<' + fmt)
        assert self.pos + size <= len(self.data), 'truncated event payload'
        values = struct.unpack_from('<' + fmt, self.data, self.pos)
        self.pos += size
        return values[0] if len(values) == 1 else values

    def string(self):
        size = self.take('I')
        assert self.pos + size <= len(self.data), 'truncated event string'
        data = self.data[self.pos:self.pos + size]
        self.pos += size
        try:
            return data.decode('utf-8', errors='strict')
        except UnicodeDecodeError as error:
            raise AssertionError('invalid UTF-8 in event') from error

    def strings(self):
        return [self.string() for _ in range(self.take('I'))]


def decode_events(data):
    """Reject corrupt or partial completed logs, including payload trailing bytes."""
    result, pos, previous = [], 0, 0
    while pos < len(data):
        assert pos + 16 <= len(data), 'truncated event header'
        assert data[pos:pos + 8] == b'CAELOG1\0', 'bad event magic'
        size, crc = struct.unpack_from('<II', data, pos + 8)
        assert 17 <= size <= 64 * 1024 * 1024, 'invalid event length'
        body = data[pos + 16:pos + 16 + size]
        assert len(body) == size, 'truncated event record'
        assert zlib.crc32(body) == crc, 'event CRC mismatch'
        c = Cursor(body)
        seq, wall, kind = c.take('QqB')
        assert seq > previous, 'event sequence must increase'
        previous = seq
        e = dict(seq=seq, wall=wall, kind=kind)
        if kind == 20:
            e['level'], e['rank'] = c.take('ii')
            e['text'], e['code'], e['error_code'] = c.string(), c.string(), c.take('I')
        elif kind == 19:
            e['exit_code'], e['last_window'] = c.take('iQ')
            e['reason'] = c.string()
        elif kind == 21:
            e['state'], e['window'], e['time'] = c.take('BQd')
            assert 0 <= e['state'] <= 5, 'invalid state'
        elif kind == 22:
            e['window'], e['time'] = c.take('Qd')
            e['path'], e['checksum'] = c.string(), c.string()
        elif kind == 17:
            e['cmd_seq'], e['request'] = c.take('Q'), c.string()
            e['status'], e['step'] = c.take('IQ')
            e['text'], e['time'] = c.string(), c.take('d')
        elif kind == 18:
            e['window'], e['time'] = c.take('Qd')
            e['points'] = []
            for _ in range(c.take('I')):
                solver, field = c.string(), c.string()
                iteration, norm, flags, linear, window = c.take('IdIII')
                e['points'].append(dict(solver=solver, field=field, iteration=iteration,
                                        norm=norm, flags=flags, linear=linear, window=window))
            e['dropped'] = c.take('I')
        elif kind == 11:
            e['solver'], e['field'] = c.string(), c.string()
            e['window'], e['iteration'], e['norm'], e['flags'] = c.take('QIdI')
            e['code'], e['error_code'] = c.string(), c.take('I')
        elif kind == 10:
            c.take('Qddd')
            c.string()
        elif kind == 15:
            c.string()
            c.take('I')
            c.strings()
            c.string()
            c.string()
            for _ in range(c.take('I')):
                c.string()
                c.string()
            c.take('II')
            for _ in range(c.take('I')):
                c.take('I')
            for _ in range(3):
                c.string()
            c.strings()
        else:
            raise AssertionError(f'unknown event kind {kind}')
        assert c.pos == len(body), 'event payload has trailing bytes'
        result.append(e)
        pos += 16 + size
    assert result, 'empty event log'
    return result


def energy(events):
    records = [e for e in events if e['kind'] == 20 and e['text'].startswith(ENERGY_PREFIX)]
    assert len(records) == 1, f'expected exactly one FVM_ENERGY LogLine, got {len(records)}'
    event = records[0]
    assert event['level'] == 1 and event['rank'] == 0, event
    def pairs(items):
        assert len(items) == len({k for k, _ in items}), 'duplicate energy JSON keys'
        return dict(items)
    def constant(text):
        raise AssertionError(f'nonfinite JSON constant {text}')
    text = event['text'][len(ENERGY_PREFIX):]
    assert '\n' not in text and '\r' not in text, 'energy JSON must be one line'
    try:
        value = json.loads(text, object_pairs_hook=pairs, parse_constant=constant)
    except (ValueError, TypeError) as error:
        raise AssertionError(f'malformed energy JSON: {text!r}') from error
    assert isinstance(value, dict) and set(value) == {
        'heat_in', 'heat_out', 'imbalance', 'iterations'}, value
    for key in ('heat_in', 'heat_out', 'imbalance'):
        assert type(value[key]) in (int, float), (key, value)
        assert math.isfinite(value[key]) and value[key] >= 0, (key, value)
    assert type(value['iterations']) is int and value['iterations'] >= 0, value
    recomputed = abs(value['heat_in'] - value['heat_out']) / max(
        value['heat_in'] + value['heat_out'], 1e-300)
    assert math.isfinite(recomputed), value
    assert abs(value['imbalance'] - recomputed) <= max(1e-15, 1e-12 * abs(recomputed)), value
    assert value['imbalance'] <= 1e-9 and recomputed <= 1e-9, (value, recomputed)
    return value


def elastic(events):
    records = [e for e in events if e['kind'] == 20 and e['text'].startswith(ELASTIC_PREFIX)]
    assert len(records) == 1, f'expected exactly one FVM_ELASTIC LogLine, got {len(records)}'
    event = records[0]
    assert event['level'] == 1 and event['rank'] == 0, event
    def pairs(items):
        assert len(items) == len({k for k, _ in items}), 'duplicate elastic JSON keys'
        return dict(items)
    def constant(text):
        raise AssertionError(f'nonfinite JSON constant {text}')
    text = event['text'][len(ELASTIC_PREFIX):]
    assert '\n' not in text and '\r' not in text, 'elastic JSON must be one line'
    try:
        value = json.loads(text, object_pairs_hook=pairs, parse_constant=constant)
    except (ValueError, TypeError) as error:
        raise AssertionError(f'malformed elastic JSON: {text!r}') from error
    assert isinstance(value, dict) and set(value) == {
        'iterations', 'residual', 'warpage_pv', 'U_mag_max'}, value
    assert type(value['iterations']) is int and value['iterations'] >= 0, value
    for key in ('residual', 'warpage_pv', 'U_mag_max'):
        assert type(value[key]) in (int, float), (key, value)
        assert math.isfinite(value[key]) and value[key] >= 0, (key, value)
    return value


def mechanical_residuals(events):
    # Selecting only correctly labelled points would hide additional U points
    # accidentally published with the default FEM solver name (or wrong field).
    points = [p for e in events if e['kind'] == 18 for p in e['points']
              if p['field'] == 'U' or p['solver'] == 'fvm.solid']
    assert points, 'mechanics success must publish measured fvm.solid/U residuals'
    for point in points:
        assert (point['solver'], point['field']) == ('fvm.solid', 'U'), point
        assert math.isfinite(point['norm']) and point['norm'] >= 0, point
    return points


def no_mechanical_publication(out, events):
    assert not any(e['kind'] == 22 for e in events), 'unexpected successful output event'
    assert not any(e['kind'] == 21 and e['state'] == 4 for e in events), 'unexpected completed state'
    assert not any(e['kind'] == 20 and e['text'].startswith(ELASTIC_PREFIX)
                   for e in events), 'unexpected elastic success record'
    assert not (out / 'out/step-0.vtkhdf').exists(), 'unexpected successful step file'
    if (out / 'manifest.json').exists():
        manifest = read_json(out / 'manifest.json')
        assert manifest['steps'] == [], manifest['steps']
        assert not any(s['name'] in ('warpage_pv', 'U_mag_max')
                       for s in manifest.get('summary', [])), 'unexpected mechanical summary'


def inspect_elastic(out, blocks, manifest, nonzero=True):
    import numpy as np
    events = decode_events((out / 'events.bin').read_bytes())
    assert THERMAL_ONLY not in json.dumps(manifest), 'placeholder in manifest'
    assert all(THERMAL_ONLY not in e.get('text', '') for e in events), 'placeholder in event text'
    ledger = elastic(events)
    summaries = {x['name']: x for x in manifest['summary']}
    for key, method in (('warpage_pv', WARPAGE_METHOD), ('U_mag_max', DISPLACEMENT_METHOD)):
        entry = summaries[key]
        assert entry['method'] == method, entry
        assert type(entry['value']) in (int, float) and math.isfinite(entry['value']), entry
        assert entry['value'] >= 0, entry
        if nonzero:
            assert entry['value'] > 0, f'{key}: expected solved nonzero mechanics, got {entry}'
        assert abs(ledger[key] - entry['value']) <= 1e-12 * abs(entry['value']), (ledger, entry)
        # The 17-significant-digit diagnostic must round-trip the same binary64
        # result as the manifest, including digits beyond the relative PV gate.
        assert struct.pack('<d', ledger[key]) == struct.pack('<d', entry['value']), (
            f'FVM_ELASTIC {key} lost binary64 precision', ledger, entry)
    residuals = mechanical_residuals(events)
    final = residuals[-1]
    assert final['iteration'] == ledger['iterations'], (final, ledger)
    assert math.isfinite(final['norm']) and final['norm'] >= 0, final
    assert abs(final['norm'] - ledger['residual']) <= 1e-12 * abs(ledger['residual']), (final, ledger)
    assert struct.pack('<d', final['norm']) == struct.pack('<d', ledger['residual']), (
        'FVM_ELASTIC residual lost binary64 precision', final, ledger)
    for name, data in blocks.items():
        assert np.all(np.isfinite(data['PointData/U'])), name
    if nonzero:
        assert any(np.any(data['PointData/U'] != 0) for data in blocks.values()), 'all output U is zero'
    assert events[-1]['kind'] == 19 and events[-1]['exit_code'] == 0
    assert events[-1]['reason'] == 'end_reached'
    return ledger, events


def read_result(out, require_volume=True):
    import h5py
    import numpy as np
    manifest = read_json(out / 'manifest.json')
    assert manifest['schema_version'] == 'chipcae.results/1.0', manifest
    digest = manifest['mesh']['mesh_sha256']
    assert len(digest) == 64 and all(c in '0123456789abcdef' for c in digest), digest
    assert len(manifest['steps']) == 1, manifest['steps']
    step = out / manifest['steps'][0]['file']
    assert step.is_file() and step.stat().st_size > 0, step
    blocks = {}
    with h5py.File(step, 'r') as f:
        for name in ('hex', 'cut', 'eb_surface'):
            g = f['VTKHDF/' + name]
            data = {}
            g.visititems(lambda key, obj: data.update({key: obj[:]})
                         if isinstance(obj, h5py.Dataset) else None)
            blocks[name] = data
            points = data['Points']
            assert points.ndim == 2 and points.shape[1] == 3 and np.all(np.isfinite(points))
            assert data['PointData/T'].shape == (len(points),)
            assert data['PointData/U'].shape == (len(points), 3)
            assert np.all(np.isfinite(data['PointData/T']))
            assert np.all(np.isfinite(data['PointData/U']))
            if name == 'eb_surface':
                count = int(data['Polygons/NumberOfCells'].sum())
                assert data['Polygons/Connectivity'].size == 3 * count
                for field in ('layer_id', 'material_id', 'cell_kind', 'elem_gid'):
                    a = data['CellData/' + field]
                    assert a.shape == (count,) and np.issubdtype(a.dtype, np.integer), (name, field)
                continue
            count = int(data['NumberOfCells'].sum())
            assert len(data['Types']) == count
            assert np.all(data['Types'] == (12 if name == 'hex' else 42)), name
            for field in ('layer_id', 'material_id', 'cell_kind', 'elem_gid'):
                a = data['CellData/' + field]
                assert a.shape == (count,) and np.issubdtype(a.dtype, np.integer), (name, field)
            if name == 'cut':
                assert np.all(data['CellData/cell_kind'] == 1), name
            else:
                assert np.all(np.isin(data['CellData/cell_kind'], [0, 1])), name
            if require_volume and count:
                assert 'CellData/volume' in data, f'{name}: missing required physical volume output'
                volumes = data['CellData/volume']
                assert volumes.shape == (count,) and np.all(np.isfinite(volumes))
                assert np.all(volumes > 0), (name, volumes)
            for optional, width in (('copper_fraction', 1), ('conductivity_diag', 3)):
                key = 'CellData/' + optional
                if require_volume and REQUIRE_COPPER_FIELDS and count:
                    assert key in data, f'{name}: missing required upstream field {optional}'
                if key in data:
                    shape = (count,) if width == 1 else (count, width)
                    assert data[key].shape == shape and np.all(np.isfinite(data[key]))
    return manifest, blocks


def canonical_cells(block, field='T'):
    """Key by parent gid and cell's coordinates, interpreting each rank's local IDs."""
    cells, p0, c0, o0, k0 = {}, 0, 0, 0, 0
    counts = zip(block['NumberOfPoints'], block['NumberOfCells'],
                 block['NumberOfConnectivityIds'], strict=True)
    for npoints, ncells, nconn in counts:
        npoints, ncells, nconn = int(npoints), int(ncells), int(nconn)
        offsets = block['Offsets'][o0:o0 + ncells + 1]
        conn = block['Connectivity'][k0:k0 + nconn]
        assert len(offsets) == ncells + 1 and offsets[0] == 0 and offsets[-1] == nconn
        for local in range(ncells):
            indices = conn[int(offsets[local]):int(offsets[local + 1])].astype(int)
            assert len(indices) > 0 and min(indices) >= 0 and max(indices) < npoints
            pairs = sorted((tuple(block['Points'][p0 + i]),
                            block['PointData/' + field][p0 + i]) for i in indices)
            key = (int(block['CellData/elem_gid'][c0 + local]), tuple(p for p, _ in pairs))
            assert key not in cells, f'duplicate owned volume cell {key}'
            cells[key] = tuple(t for _, t in pairs)
        p0, c0, o0, k0 = p0 + npoints, c0 + ncells, o0 + ncells + 1, k0 + nconn
    assert p0 == len(block['Points']) and c0 == len(block['Types'])
    assert o0 == len(block['Offsets']) and k0 == len(block['Connectivity'])
    return cells


def compare_parallel(first, second):
    import numpy as np
    for name in ('hex', 'cut'):
        a, b = canonical_cells(first[name]), canonical_cells(second[name])
        assert a.keys() == b.keys(), f'{name}: owned gid/coordinate sets differ'
        for key in a:
            x, y = np.asarray(a[key]), np.asarray(b[key])
            error = float(np.max(np.abs(x - y))) / max(float(np.max(np.abs(x))), 1e-300)
            assert error <= 1e-8, (name, key, error)
    # Use one global vector norm; a zero component or a small local displacement
    # must not amplify roundoff. Cell keys preserve parent gids and vertex coordinates.
    x, y = [], []
    for name in ('hex', 'cut'):
        a, b = canonical_cells(first[name], 'U'), canonical_cells(second[name], 'U')
        assert a.keys() == b.keys(), f'{name}: displacement gid/coordinate sets differ'
        for key in sorted(a):
            x.extend(np.asarray(a[key]).ravel())
            y.extend(np.asarray(b[key]).ravel())
    x, y = np.asarray(x), np.asarray(y)
    assert np.all(np.isfinite(x)) and np.all(np.isfinite(y)), 'nonfinite MPI displacement'
    error = float(np.linalg.norm(x - y)) / max(float(np.linalg.norm(x)), 1e-300)
    assert error <= 1e-8, f'MPI global U relative difference {error:.17g} > 1e-8'


def inspect_fvm(out, blocks, manifest, stopped=False):
    events = decode_events((out / 'events.bin').read_bytes())
    ledger = energy(events)
    assert events[-1]['kind'] == 19 and events[-1]['exit_code'] == 0
    assert events[-1]['reason'] == ('user_stop' if stopped else 'end_reached')
    points = [p for e in events if e['kind'] == 18 for p in e['points']
              if p['solver'] == 'fvm.conduction']
    assert points, 'missing fvm.conduction ResidualBatch'
    assert points[-1]['iteration'] == ledger['iterations'], (points[-1], ledger)
    assert math.isfinite(points[-1]['norm']) and points[-1]['norm'] >= 0
    if stopped:
        assert manifest['steps'] == []
        assert not any(e['kind'] == 22 for e in events)
        return ledger, events
    summaries = {x['name']: x for x in manifest['summary']}
    assert summaries['T_max']['method'] == 'FVM cell centers'
    assert math.isfinite(summaries['T_max']['value'])
    inspect_elastic(out, blocks, manifest, nonzero=False)
    states = [e['state'] for e in events if e['kind'] == 21]
    assert states[0] == 1 and states[-1] == 4 and 2 in states, states
    outputs = [e for e in events if e['kind'] == 22]
    assert len(outputs) == 1
    actual = hashlib.sha256((out / outputs[0]['path']).read_bytes()).hexdigest()
    assert outputs[0]['checksum'] == actual
    return ledger, events


def summary(manifest):
    return {x['name']: x['value'] for x in manifest['summary']}


def laminate_oracle(base, metal, fraction):
    """Independent I1 1.2 engineering compliance inversion; no production mixture call."""
    import numpy as np
    matrix, nm, am = (base[key] for key in ('E', 'nu', 'alpha'))
    copper, nc, ac = (metal[key] for key in ('E', 'nu', 'alpha'))
    rest = 1-fraction
    exy = ez = rest*matrix + fraction*copper
    nu = rest*nm + fraction*nc
    gxy = exy/(2*(1+nu))
    gz = 1/(rest/(matrix/(2*(1+nm))) + fraction/(copper/(2*(1+nc))))
    compliance = np.diag([1/exy, 1/exy, 1/ez, 1/gz, 1/gz, 1/gxy])
    compliance[0, 1] = compliance[1, 0] = -nu/exy
    compliance[0, 2] = compliance[2, 0] = -nu/exy
    compliance[1, 2] = compliance[2, 1] = -nu/exy
    axy = ((rest*matrix*am/(1-nm) + fraction*copper*ac/(1-nc)) /
           (rest*matrix/(1-nm) + fraction*copper/(1-nc)))
    az = (rest*matrix*am + fraction*copper*ac)/ez
    return np.linalg.inv(compliance), [axy, axy, az]


def check_replay_input(snapshot, inputs):
    """Check adapter arrays against raw grid data and independently loaded SI cards."""
    import numpy as np
    mesh, problem, raw = (snapshot[key] for key in ('mesh', 'problem', 'raw'))
    layers = inputs['stack']['layers']
    names = [layer['name'] for layer in layers]
    attrs = raw['attributes']
    for key in ('gid', 'rank'):
        assert mesh[key] == raw[key], key
    for key in ('volume', 'centroid'):
        np.testing.assert_allclose(mesh[key], raw[key], rtol=0, atol=1e-14, err_msg=key)
    assert mesh['layer'] == [names.index(attrs[a-1]['layer']) for a in raw['attribute']]
    assert mesh['is_cut'] == [int(kind != 0) for kind in raw['kind']]
    np.testing.assert_allclose(mesh['z_top'], raw['axes'][2][-1], rtol=0, atol=1e-14)
    expected_embedded = []
    for face, original in zip(mesh['faces'], raw['faces'], strict=True):
        embedded = original['hi'] < 0 and original['side'] == 6
        expected_embedded.append(int(embedded))
        assert (face['lo'], face['hi'], face['side']) == (
            original['lo'], original['hi'], -1 if embedded else original['side'])
        for key in ('area', 'centroid'):
            np.testing.assert_allclose(face[key], original[key], rtol=0, atol=1e-14)
        np.testing.assert_allclose(face['normal'], np.asarray(original['area_vector'])/original['area'],
                                   rtol=0, atol=1e-14)
    assert mesh['is_embedded'] == expected_embedded
    parents = {gid: (volume, physical, attrs[attr-1])
               for gid, volume, physical, attr in zip(
                   raw['parent_gid'], raw['parent_volume'], raw['parent_physical_volume'],
                   raw['parent_attribute'], strict=True)}
    ncol = (len(raw['axes'][0])-1)*(len(raw['axes'][1])-1)
    fractions = []
    for begin, end in zip(raw['constituent_off'], raw['constituent_off'][1:]):
        values, weights = [], []
        for gid in dict.fromkeys(raw['constituent_gid'][begin:end]):
            volume, physical, attr = parents[gid]
            layer = names.index(attr['layer'])
            copper = raw['copper_layer_of_layer'][layer] if raw['copper_layer_of_layer'] else -1
            phi = raw['copper_phi'][copper*ncol + gid//raw['nz']] if copper >= 0 else 0.
            values.append(min(1., phi*volume/physical))
            weights.append(physical)
        fractions.append(np.average(values, weights=weights))
    np.testing.assert_allclose(raw['fraction'], fractions, rtol=1e-12, atol=1e-14)
    for c, (attribute, fraction) in enumerate(zip(raw['attribute'], fractions, strict=True)):
        attr = attrs[attribute-1]
        layer = layers[names.index(attr['layer'])]
        base = inputs['materials'][attr['material']]
        metal = inputs['materials'][layer['copper']['metal']] if layer['copper'] else base
        stiffness, alpha = laminate_oracle(base, metal, fraction)
        np.testing.assert_allclose(np.asarray(problem['C'][c]).reshape(6, 6), stiffness,
                                   rtol=1e-12, atol=1e-5)
        np.testing.assert_allclose(problem['alpha'][c], alpha, rtol=1e-12, atol=1e-16)
        k = (1-fraction)*base['k'] + fraction*metal['k']
        np.testing.assert_allclose(raw['thermal_k'][c], [k, k, k], rtol=1e-12, atol=1e-14)
    if inputs['thermal']:
        temperature = np.asarray(raw['T_cell'])
        # Layer-prescribed CVs give an oracle independent of the thermal solver.
        for load in inputs['loads']:
            if load['type'] == 'fixed_temperature' and load['target'].startswith('layer:'):
                index = names.index(load['target'][6:])
                mask = np.asarray(mesh['layer']) == index
                assert np.any(mask), load
                np.testing.assert_allclose(temperature[mask], load['T'], rtol=0, atol=1e-14)
    else:
        temperature = next(load['T'] for load in inputs['loads']
                           if load['type'] == 'uniform_temperature')
    np.testing.assert_allclose(problem['dT'], temperature-inputs['T_ref'], rtol=0, atol=1e-14)
    for key in ('max_iter', 'preconditioner'):
        assert problem[key] == inputs['linear'][key], key
    np.testing.assert_allclose(problem['rtol'], inputs['linear']['rtol'], rtol=0, atol=1e-23)
    assert problem['support'] == ('Fixed' if inputs['fixed'] else 'Rigid321')
    sides = ('xmin', 'xmax', 'ymin', 'ymax', 'zmin', 'zmax')
    assert problem['fixed_side'] == [sides.index(target[5:]) for target in inputs['fixed']
                                     if target.startswith('face:')]
    assert problem['fixed_layer'] == [i for target in inputs['fixed'] if target.startswith('layer:')
                                      for i, name in enumerate(names)
                                      if name == target[6:] or name.startswith(target[6:]+'#')]


def direct_elastic(root, case, name, adapter, replay, ranks=1):
    inputs, _ = probe(root, case, ranks=ranks)
    captured = command(mpi_command([adapter, 'snapshot', root / 'case.json',
                                    root / f'profile{ranks}.json'], ranks), root)
    snapshot = json.loads(captured.stdout)
    check_replay_input(snapshot, inputs)
    path = root / (name + '-elastic-input.json')
    write_json(path, snapshot)
    result = command(mpi_command([replay, path], ranks), root)
    return json.loads(result.stdout)


def compare_replay(manifest, direct):
    assert direct['ok'], direct
    assert direct['warpage_method'] == WARPAGE_METHOD, direct
    assert direct['U_mag_method'] == DISPLACEMENT_METHOD, direct
    values = summary(manifest)
    for key in ('warpage_pv', 'U_mag_max'):
        assert math.isfinite(direct[key]) and direct[key] > 0, direct
        error = abs(values[key]-direct[key])/abs(direct[key])
        print(f'实测 replay {key} CLI={values[key]:.17g} direct={direct[key]:.17g} '
              f'relative={error:.17g}', flush=True)
        assert math.isfinite(error) and error <= 1e-8, (key, values[key], direct[key], error)
    diagnostics = {key: direct[key] for key in (
        'min_dn_ratio', 'min_dn_gid', 'gradient_corrections', 'max_identity_defect')}
    print('实测 replay diagnostics=' + json.dumps(diagnostics), flush=True)
    assert all(math.isfinite(value) for value in diagnostics.values()), diagnostics


def inspect_elastic_failure(out, process, direct):
    assert not direct['ok'], 'fixture must reach the library failure path'
    assert process.returncode == 1, process.stderr
    # Compare the complete opaque reason. I14 1.0 exposes no failure residual
    # structure; parsing the diagnostic numbers would fabricate an API.
    original = direct['path'] + ': ' + direct['reason']
    assert original in process.stderr, (original, process.stderr)
    events = decode_events((out / 'events.bin').read_bytes())
    assert (events[-1]['kind'], events[-1]['exit_code']) == (19, 1), events[-1]
    assert any(e['kind'] == 20 and e['level'] == 3 and original in e['text']
               for e in events), ('missing original library error LogLine', original)
    no_mechanical_publication(out, events)
    return events


def curvature(blocks):
    """Signed quadratic curvature after removing the top surface's rigid plane."""
    import numpy as np
    samples = {}
    for name in ('hex', 'cut'):
        for point, displacement in zip(blocks[name]['Points'], blocks[name]['PointData/U'], strict=True):
            key = tuple(point)
            if key in samples:
                np.testing.assert_allclose(samples[key], displacement[2], rtol=1e-8, atol=1e-14)
            samples[key] = displacement[2]
    top = max(p[2] for p in samples)
    points = np.array([(*p, value) for p, value in samples.items() if abs(p[2] - top) <= 1e-14])
    xy = points[:, :2] - points[:, :2].mean(axis=0)
    xy /= max(float(np.ptp(xy[:, 0])), float(np.ptp(xy[:, 1])))
    x, y = xy.T
    plane = np.column_stack((np.ones(len(x)), x, y))
    uz = points[:, 3]
    detrended = uz - plane @ np.linalg.lstsq(plane, uz, rcond=None)[0]
    quadratic = np.column_stack((x*x, x*y, y*y, plane))
    coefficients, _, rank, _ = np.linalg.lstsq(quadratic, detrended, rcond=None)
    assert rank == 6, 'top surface cannot resolve a quadratic curvature'
    signed = float(coefficients[0] + coefficients[2])
    assert math.isfinite(signed) and abs(signed) > 1e-14, 'unresolved curvature sign'
    return signed


# Retained verbatim for solve-elastic-accuracy, per the 18:02 ruling. This
# stricter accuracy check is deliberately not called by this card's suite.
def compare_mechanics(fvm, fem, fvm_blocks, fem_blocks):
    a, b = summary(fvm), summary(fem)
    assert b['warpage_pv'] > 0 and b['U_mag_max'] > 0, b
    error = abs(a['warpage_pv'] - b['warpage_pv']) / b['warpage_pv']
    u_error = abs(a['U_mag_max'] - b['U_mag_max']) / b['U_mag_max']
    print(f'实测 mechanics warpage_fvm={a["warpage_pv"]:.17g} '
          f'warpage_fem={b["warpage_pv"]:.17g} relative={error:.17g} '
          f'U_mag_max_fvm={a["U_mag_max"]:.17g} U_mag_max_fem={b["U_mag_max"]:.17g} '
          f'U_relative={u_error:.17g}', flush=True)
    assert error <= .10, f'FVM/FEM warpage relative difference {error:.17g} > 0.10'
    assert curvature(fvm_blocks) * curvature(fem_blocks) > 0, 'opposite FVM/FEM curvature signs'


def compare_mechanics_sign(fvm, fem, fvm_blocks, fem_blocks):
    a, b = summary(fvm), summary(fem)
    assert b['warpage_pv'] > 0 and b['U_mag_max'] > 0, b
    error = abs(a['warpage_pv'] - b['warpage_pv']) / b['warpage_pv']
    u_error = abs(a['U_mag_max'] - b['U_mag_max']) / b['U_mag_max']
    print(f'实测 mechanics warpage_fvm={a["warpage_pv"]:.17g} '
          f'warpage_fem={b["warpage_pv"]:.17g} relative={error:.17g} '
          f'U_mag_max_fvm={a["U_mag_max"]:.17g} U_mag_max_fem={b["U_mag_max"]:.17g} '
          f'U_relative={u_error:.17g}', flush=True)
    assert curvature(fvm_blocks) * curvature(fem_blocks) > 0, 'opposite FVM/FEM curvature signs'


def import_board(root, source):
    fixture(root)
    command([os.environ['CHIPCAE_IMPORT_EXE'],
             source / 'tests/integration/ecad/data/board_4layer.xml',
             '-o', 'stack.json', '--polyset', 'stack.polyset', '--report', 'defects.json',
             '--material-map', source / 'tests/integration/f1130_uncut_cli/material_map.json'], root)
    # Deliberately use import artifacts unchanged: fraction averaging hides the prerequisite.
    stack = read_json(root / 'stack.json')
    assert any('polygons' in l.get('copper', {}) for l in stack['layers'])
    materials = [material('cu', 390), material('fr4_eq', .3)]
    # Distinct mechanical cards exercise the copper laminate adapter as well as
    # the thermal solve. FVM and FEM consume this identical synthetic library.
    for card, youngs, alpha in zip(materials, (110e9, 25e9), (17e-6, 12e-6), strict=True):
        card['elastic']['table']['columns']['E'] = [youngs]
        card['thermal_expansion']['table']['columns']['alpha'] = [alpha]
    write_json(root / 'materials.json', dict(schema='chipcae.material_library/1', materials=materials))
    originals = {p: (root / p).read_bytes() for p in ('stack.json', 'stack.polyset')}
    result = command([os.environ['CHIPCAE_PREVIEW_EXE'], root / 'stack.json',
                      '-o', root / 'preview.vtkhdf', '--nx', '4', '--ny', '4',
                      '--levels', '0', '--max-ratio', '100'], root)
    successes = [e for line in result.stdout.splitlines() if line.strip()
                 for e in [json.loads(line)] if e.get('stage') == 'success']
    assert len(successes) == 1, result.stdout
    report = successes[0]
    assert type(report['cell_count']) is int and report['cell_count'] > 0, report
    assert (root / 'preview.vtkhdf').is_file()
    return stack, report, originals


def copy_demo(root, source):
    shutil.copytree(source / 'examples/demo_gate', root, dirs_exist_ok=True)
    for ranks in (1, 2):
        profile = read_json(root / 'profile.json')
        profile['ranks'] = ranks
        write_json(root / f'profile{ranks}.json', profile)
    return read_json(root / 'case.json')


def wiener_fixture(root):
    stack = fixture(root)
    sidecar = root / 'half.polyset'
    command([os.environ['CHIPCAE_FVM_LOADER_PROBE'], '--half-polyset', sidecar], root)
    stack['layers'][0].update(kind='Patterned', copper=dict(
        metal='cu', polygons=dict(file=sidecar.name, layer='trace')))
    stack['external_refs'] = [dict(path=sidecar.name,
                                  sha256=hashlib.sha256(sidecar.read_bytes()).hexdigest())]
    library = read_json(root / 'materials.json')
    library['materials'].append(material('cu', 400))
    write_json(root / 'materials.json', library)
    write_json(root / 'stack.json', stack)
    return {name: (root / name).read_bytes() for name in ('stack.json', 'half.polyset')}


# Captured from the unmodified loader at 6eca460; only temporary paths normalized.
FEM_SNAPSHOTS = {'copper': {'T_ref': 300.0,
            'backend': 'cpu',
            'case_path': '<fixture>/case.json',
            'cut_as_hex': False,
            'cut_options': {'export_polyhedra': False,
                            'keep_void': False,
                            'quad_degree': 6,
                            'small_sub_vf': 1e-08,
                            'snap_phi': 1e-14},
            'fields': ['T', 'U', 'layer_id', 'material_id', 'cell_kind', 'elem_gid',
                       'copper_fraction', 'conductivity_diag'],
            'fixed': [],
            'ghost_penalty': 0.1,
            'grid': {'ar_max': 'inf',
                     'ar_warn': 'inf',
                     'cell_budget': 50000,
                     'levels': 0,
                     'max_ratio': 100.0,
                     'nx': 4,
                     'ny': 4,
                     'ranks': 1,
                     'snap_tol': 1e-14,
                     'use_metis': False},
            'linear': {'max_iter': 2000, 'preconditioner': 'direct', 'rtol': 1e-12},
            'loads': [{'T': 300.0,
                       'T_inf': 0.0,
                       'T_ref': 0.0,
                       'h': 0.0,
                       'id': '',
                       'source': 0.0,
                       'target': 'face:zmin',
                       'total_power': False,
                       'type': 'fixed_temperature'},
                      {'T': 0.0,
                       'T_inf': 400.0,
                       'T_ref': 0.0,
                       'h': 20.0,
                       'id': '',
                       'source': 0.0,
                       'target': 'face:zmax',
                       'total_power': False,
                       'type': 'convection'}],
            'material_path': '<fixture>/materials.json',
            'materials': {'a': {'E': 25000000000.0,
                                'E_clamped': False,
                                'E_table': {'extrapolation': 'clamp',
                                            'interpolation': 'constant',
                                            'temperatures': [300.0],
                                            'values': [25000000000.0]},
                                'alpha': 1e-05,
                                'alpha_clamped': False,
                                'alpha_table': {'extrapolation': 'clamp',
                                                'interpolation': 'constant',
                                                'temperatures': [300.0],
                                                'values': [1e-05]},
                                'id': 'a',
                                'k': 2.0,
                                'nu': 0.3,
                                'nu_clamped': False,
                                'nu_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [0.3]}},
                          'b': {'E': 25000000000.0,
                                'E_clamped': False,
                                'E_table': {'extrapolation': 'clamp',
                                            'interpolation': 'constant',
                                            'temperatures': [300.0],
                                            'values': [25000000000.0]},
                                'alpha': 1e-05,
                                'alpha_clamped': False,
                                'alpha_table': {'extrapolation': 'clamp',
                                                'interpolation': 'constant',
                                                'temperatures': [300.0],
                                                'values': [1e-05]},
                                'id': 'b',
                                'k': 4.0,
                                'nu': 0.3,
                                'nu_clamped': False,
                                'nu_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [0.3]}},
                          'cu': {'E': 25000000000.0,
                                 'E_clamped': False,
                                 'E_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [25000000000.0]},
                                 'alpha': 1e-05,
                                 'alpha_clamped': False,
                                 'alpha_table': {'extrapolation': 'clamp',
                                                 'interpolation': 'constant',
                                                 'temperatures': [300.0],
                                                 'values': [1e-05]},
                                 'id': 'cu',
                                 'k': 400.0,
                                 'nu': 0.3,
                                 'nu_clamped': False,
                                 'nu_table': {'extrapolation': 'clamp',
                                              'interpolation': 'constant',
                                              'temperatures': [300.0],
                                              'values': [0.3]}}},
            'profile_path': '<fixture>/profile1.json',
            'stack': {'activation_groups': {},
                      'id': 'fvm_synthetic',
                      'layers': [{'body': 'a',
                                  'copper': {'fraction': 0.5, 'metal': 'cu', 'polygons': None},
                                  'grading': 1.0,
                                  'kind': 'Patterned',
                                  'material': 'a',
                                  'n_elem_min': 1,
                                  'name': 'a',
                                  'regions': [],
                                  'thickness': 0.05},
                                 {'body': 'b',
                                  'copper': None,
                                  'grading': 1.0,
                                  'kind': 'Explicit',
                                  'material': 'b',
                                  'n_elem_min': 1,
                                  'name': 'b',
                                  'regions': [{'body': 'patch',
                                               'footprint': {'box': [0.25, 0.25, 0.75, 0.75],
                                                             'polygon': []},
                                               'material': 'a',
                                               'name': 'patch'}],
                                  'thickness': 0.05}],
                      'outline': {'box': [0.0, 0.0, 1.0, 1.0], 'polygon': []},
                      'package_type': 'FCBGA',
                      'symmetry': 'none',
                      'unit': 'm',
                      'warnings': []},
            'stack_path': '<fixture>/stack.json',
            'thermal': True},
 'custom': {'T_ref': 300.0,
            'backend': 'cpu',
            'case_path': '<fixture>/case.json',
            'cut_as_hex': False,
            'cut_options': {'export_polyhedra': False,
                            'keep_void': False,
                            'quad_degree': 4,
                            'small_sub_vf': 1e-08,
                            'snap_phi': 1e-14},
            'fields': ['T', 'U', 'elem_gid'],
            'fixed': ['face:xmin'],
            'ghost_penalty': 0.25,
            'grid': {'ar_max': 'inf',
                     'ar_warn': 'inf',
                     'cell_budget': 12345,
                     'levels': 2,
                     'max_ratio': 100.0,
                     'nx': 3,
                     'ny': 5,
                     'ranks': 1,
                     'snap_tol': 1e-14,
                     'use_metis': False},
            'linear': {'max_iter': 321, 'preconditioner': 'column_gmg', 'rtol': 2e-09},
            'loads': [{'T': 300.0,
                       'T_inf': 0.0,
                       'T_ref': 0.0,
                       'h': 0.0,
                       'id': '',
                       'source': 0.0,
                       'target': 'face:zmin',
                       'total_power': False,
                       'type': 'fixed_temperature'},
                      {'T': 0.0,
                       'T_inf': 400.0,
                       'T_ref': 0.0,
                       'h': 20.0,
                       'id': '',
                       'source': 0.0,
                       'target': 'face:zmax',
                       'total_power': False,
                       'type': 'convection'}],
            'material_path': '<fixture>/materials.json',
            'materials': {'a': {'E': 25000000000.0,
                                'E_clamped': False,
                                'E_table': {'extrapolation': 'clamp',
                                            'interpolation': 'constant',
                                            'temperatures': [300.0],
                                            'values': [25000000000.0]},
                                'alpha': 1e-05,
                                'alpha_clamped': False,
                                'alpha_table': {'extrapolation': 'clamp',
                                                'interpolation': 'constant',
                                                'temperatures': [300.0],
                                                'values': [1e-05]},
                                'id': 'a',
                                'k': 2.0,
                                'nu': 0.3,
                                'nu_clamped': False,
                                'nu_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [0.3]}},
                          'b': {'E': 25000000000.0,
                                'E_clamped': False,
                                'E_table': {'extrapolation': 'clamp',
                                            'interpolation': 'constant',
                                            'temperatures': [300.0],
                                            'values': [25000000000.0]},
                                'alpha': 1e-05,
                                'alpha_clamped': False,
                                'alpha_table': {'extrapolation': 'clamp',
                                                'interpolation': 'constant',
                                                'temperatures': [300.0],
                                                'values': [1e-05]},
                                'id': 'b',
                                'k': 4.0,
                                'nu': 0.3,
                                'nu_clamped': False,
                                'nu_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [0.3]}}},
            'profile_path': '<fixture>/profile1.json',
            'stack': {'activation_groups': {},
                      'id': 'fvm_synthetic',
                      'layers': [{'body': 'a',
                                  'copper': None,
                                  'grading': 1.0,
                                  'kind': 'Explicit',
                                  'material': 'a',
                                  'n_elem_min': 1,
                                  'name': 'a',
                                  'regions': [],
                                  'thickness': 0.05},
                                 {'body': 'b',
                                  'copper': None,
                                  'grading': 1.0,
                                  'kind': 'Explicit',
                                  'material': 'b',
                                  'n_elem_min': 1,
                                  'name': 'b',
                                  'regions': [],
                                  'thickness': 0.05}],
                      'outline': {'box': [0.0, 0.0, 1.0, 1.0], 'polygon': []},
                      'package_type': 'FCBGA',
                      'symmetry': 'none',
                      'unit': 'm',
                      'warnings': []},
            'stack_path': '<fixture>/stack.json',
            'thermal': True},
 'default': {'T_ref': 300.0,
             'backend': 'cpu',
             'case_path': '<fixture>/case.json',
             'cut_as_hex': False,
             'cut_options': {'export_polyhedra': False,
                             'keep_void': False,
                             'quad_degree': 6,
                             'small_sub_vf': 1e-08,
                             'snap_phi': 1e-14},
             'fields': ['T', 'U', 'layer_id', 'material_id', 'cell_kind', 'elem_gid',
                        'copper_fraction', 'conductivity_diag'],
             'fixed': [],
             'ghost_penalty': 0.1,
             'grid': {'ar_max': 'inf',
                      'ar_warn': 'inf',
                      'cell_budget': 50000,
                      'levels': 0,
                      'max_ratio': 100.0,
                      'nx': 4,
                      'ny': 4,
                      'ranks': 1,
                      'snap_tol': 1e-14,
                      'use_metis': False},
             'linear': {'max_iter': 500, 'preconditioner': 'column_gmg', 'rtol': 1e-08},
             'loads': [{'T': 300.0,
                        'T_inf': 0.0,
                        'T_ref': 0.0,
                        'h': 0.0,
                        'id': '',
                        'source': 0.0,
                        'target': 'face:zmin',
                        'total_power': False,
                        'type': 'fixed_temperature'},
                       {'T': 0.0,
                        'T_inf': 400.0,
                        'T_ref': 0.0,
                        'h': 20.0,
                        'id': '',
                        'source': 0.0,
                        'target': 'face:zmax',
                        'total_power': False,
                        'type': 'convection'}],
             'material_path': '<fixture>/materials.json',
             'materials': {'a': {'E': 25000000000.0,
                                 'E_clamped': False,
                                 'E_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [25000000000.0]},
                                 'alpha': 1e-05,
                                 'alpha_clamped': False,
                                 'alpha_table': {'extrapolation': 'clamp',
                                                 'interpolation': 'constant',
                                                 'temperatures': [300.0],
                                                 'values': [1e-05]},
                                 'id': 'a',
                                 'k': 2.0,
                                 'nu': 0.3,
                                 'nu_clamped': False,
                                 'nu_table': {'extrapolation': 'clamp',
                                              'interpolation': 'constant',
                                              'temperatures': [300.0],
                                              'values': [0.3]}},
                           'b': {'E': 25000000000.0,
                                 'E_clamped': False,
                                 'E_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [25000000000.0]},
                                 'alpha': 1e-05,
                                 'alpha_clamped': False,
                                 'alpha_table': {'extrapolation': 'clamp',
                                                 'interpolation': 'constant',
                                                 'temperatures': [300.0],
                                                 'values': [1e-05]},
                                 'id': 'b',
                                 'k': 4.0,
                                 'nu': 0.3,
                                 'nu_clamped': False,
                                 'nu_table': {'extrapolation': 'clamp',
                                              'interpolation': 'constant',
                                              'temperatures': [300.0],
                                              'values': [0.3]}}},
             'profile_path': '<fixture>/profile1.json',
             'stack': {'activation_groups': {},
                       'id': 'fvm_synthetic',
                       'layers': [{'body': 'a',
                                   'copper': None,
                                   'grading': 1.0,
                                   'kind': 'Explicit',
                                   'material': 'a',
                                   'n_elem_min': 1,
                                   'name': 'a',
                                   'regions': [],
                                   'thickness': 0.05},
                                  {'body': 'b',
                                   'copper': None,
                                   'grading': 1.0,
                                   'kind': 'Explicit',
                                   'material': 'b',
                                   'n_elem_min': 1,
                                   'name': 'b',
                                   'regions': [],
                                   'thickness': 0.05}],
                       'outline': {'box': [0.0, 0.0, 1.0, 1.0], 'polygon': []},
                       'package_type': 'FCBGA',
                       'symmetry': 'none',
                       'unit': 'm',
                       'warnings': []},
             'stack_path': '<fixture>/stack.json',
             'thermal': True},
 'solid': {'T_ref': 300.0,
           'backend': 'cpu',
           'case_path': '<fixture>/case.json',
           'cut_as_hex': False,
           'cut_options': {'export_polyhedra': False,
                           'keep_void': False,
                           'quad_degree': 6,
                           'small_sub_vf': 1e-08,
                           'snap_phi': 1e-14},
           'fields': ['T', 'U', 'layer_id', 'material_id', 'cell_kind', 'elem_gid',
                      'copper_fraction', 'conductivity_diag'],
           'fixed': [],
           'ghost_penalty': 0.1,
           'grid': {'ar_max': 'inf',
                    'ar_warn': 'inf',
                    'cell_budget': 50000,
                    'levels': 0,
                    'max_ratio': 100.0,
                    'nx': 4,
                    'ny': 4,
                    'ranks': 1,
                    'snap_tol': 1e-14,
                    'use_metis': False},
           'linear': {'max_iter': 2000, 'preconditioner': 'direct', 'rtol': 1e-12},
           'loads': [{'T': 350.0,
                      'T_inf': 0.0,
                      'T_ref': 300.0,
                      'h': 0.0,
                      'id': '',
                      'source': 0.0,
                      'target': '',
                      'total_power': False,
                      'type': 'uniform_temperature'}],
           'material_path': '<fixture>/materials.json',
           'materials': {'a': {'E': 25000000000.0,
                               'E_clamped': False,
                               'E_table': {'extrapolation': 'clamp',
                                           'interpolation': 'constant',
                                           'temperatures': [300.0],
                                           'values': [25000000000.0]},
                               'alpha': 1e-05,
                               'alpha_clamped': False,
                               'alpha_table': {'extrapolation': 'clamp',
                                               'interpolation': 'constant',
                                               'temperatures': [300.0],
                                               'values': [1e-05]},
                               'id': 'a',
                               'k': 2.0,
                               'nu': 0.3,
                               'nu_clamped': False,
                               'nu_table': {'extrapolation': 'clamp',
                                            'interpolation': 'constant',
                                            'temperatures': [300.0],
                                            'values': [0.3]}},
                         'b': {'E': 25000000000.0,
                               'E_clamped': False,
                               'E_table': {'extrapolation': 'clamp',
                                           'interpolation': 'constant',
                                           'temperatures': [300.0],
                                           'values': [25000000000.0]},
                               'alpha': 1e-05,
                               'alpha_clamped': False,
                               'alpha_table': {'extrapolation': 'clamp',
                                               'interpolation': 'constant',
                                               'temperatures': [300.0],
                                               'values': [1e-05]},
                               'id': 'b',
                               'k': 4.0,
                               'nu': 0.3,
                               'nu_clamped': False,
                               'nu_table': {'extrapolation': 'clamp',
                                            'interpolation': 'constant',
                                            'temperatures': [300.0],
                                            'values': [0.3]}}},
           'profile_path': '<fixture>/profile1.json',
           'stack': {'activation_groups': {},
                     'id': 'fvm_synthetic',
                     'layers': [{'body': 'a',
                                 'copper': None,
                                 'grading': 1.0,
                                 'kind': 'Explicit',
                                 'material': 'a',
                                 'n_elem_min': 1,
                                 'name': 'a',
                                 'regions': [],
                                 'thickness': 0.05},
                                {'body': 'b',
                                 'copper': None,
                                 'grading': 1.0,
                                 'kind': 'Explicit',
                                 'material': 'b',
                                 'n_elem_min': 1,
                                 'name': 'b',
                                 'regions': [],
                                 'thickness': 0.05}],
                     'outline': {'box': [0.0, 0.0, 1.0, 1.0], 'polygon': []},
                     'package_type': 'FCBGA',
                     'symmetry': 'none',
                     'unit': 'm',
                     'warnings': []},
           'stack_path': '<fixture>/stack.json',
           'thermal': False},
 'sources': {'T_ref': 300.0,
             'backend': 'cpu',
             'case_path': '<fixture>/case.json',
             'cut_as_hex': False,
             'cut_options': {'export_polyhedra': False,
                             'keep_void': False,
                             'quad_degree': 6,
                             'small_sub_vf': 1e-08,
                             'snap_phi': 1e-14},
             'fields': ['T', 'U', 'layer_id', 'material_id', 'cell_kind', 'elem_gid',
                        'copper_fraction', 'conductivity_diag'],
             'fixed': [],
             'ghost_penalty': 0.1,
             'grid': {'ar_max': 'inf',
                      'ar_warn': 'inf',
                      'cell_budget': 50000,
                      'levels': 0,
                      'max_ratio': 100.0,
                      'nx': 4,
                      'ny': 4,
                      'ranks': 1,
                      'snap_tol': 1e-14,
                      'use_metis': False},
             'linear': {'max_iter': 2000, 'preconditioner': 'direct', 'rtol': 1e-12},
             'loads': [{'T': 350.0,
                        'T_inf': 0.0,
                        'T_ref': 0.0,
                        'h': 0.0,
                        'id': 'pin',
                        'source': 0.0,
                        'target': 'layer:a',
                        'total_power': False,
                        'type': 'fixed_temperature'},
                       {'T': 0.0,
                        'T_inf': 300.0,
                        'T_ref': 0.0,
                        'h': 20.0,
                        'id': 'cool',
                        'source': 0.0,
                        'target': 'layer:b',
                        'total_power': False,
                        'type': 'convection'},
                       {'T': 0.0,
                        'T_inf': 0.0,
                        'T_ref': 0.0,
                        'h': 0.0,
                        'id': 'power',
                        'source': 7.0,
                        'target': 'layer:a',
                        'total_power': True,
                        'type': 'heat_source'},
                       {'T': 0.0,
                        'T_inf': 0.0,
                        'T_ref': 0.0,
                        'h': 0.0,
                        'id': 'density',
                        'source': 3.0,
                        'target': 'layer:b',
                        'total_power': False,
                        'type': 'heat_source'}],
             'material_path': '<fixture>/materials.json',
             'materials': {'a': {'E': 25000000000.0,
                                 'E_clamped': False,
                                 'E_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [25000000000.0]},
                                 'alpha': 1e-05,
                                 'alpha_clamped': False,
                                 'alpha_table': {'extrapolation': 'clamp',
                                                 'interpolation': 'constant',
                                                 'temperatures': [300.0],
                                                 'values': [1e-05]},
                                 'id': 'a',
                                 'k': 2.0,
                                 'nu': 0.3,
                                 'nu_clamped': False,
                                 'nu_table': {'extrapolation': 'clamp',
                                              'interpolation': 'constant',
                                              'temperatures': [300.0],
                                              'values': [0.3]}},
                           'b': {'E': 25000000000.0,
                                 'E_clamped': False,
                                 'E_table': {'extrapolation': 'clamp',
                                             'interpolation': 'constant',
                                             'temperatures': [300.0],
                                             'values': [25000000000.0]},
                                 'alpha': 1e-05,
                                 'alpha_clamped': False,
                                 'alpha_table': {'extrapolation': 'clamp',
                                                 'interpolation': 'constant',
                                                 'temperatures': [300.0],
                                                 'values': [1e-05]},
                                 'id': 'b',
                                 'k': 4.0,
                                 'nu': 0.3,
                                 'nu_clamped': False,
                                 'nu_table': {'extrapolation': 'clamp',
                                              'interpolation': 'constant',
                                              'temperatures': [300.0],
                                              'values': [0.3]}}},
             'profile_path': '<fixture>/profile1.json',
             'stack': {'activation_groups': {},
                       'id': 'fvm_synthetic',
                       'layers': [{'body': 'a',
                                   'copper': None,
                                   'grading': 1.0,
                                   'kind': 'Explicit',
                                   'material': 'a',
                                   'n_elem_min': 1,
                                   'name': 'a',
                                   'regions': [],
                                   'thickness': 0.05},
                                  {'body': 'b',
                                   'copper': None,
                                   'grading': 1.0,
                                   'kind': 'Explicit',
                                   'material': 'b',
                                   'n_elem_min': 1,
                                   'name': 'b',
                                   'regions': [],
                                   'thickness': 0.05}],
                       'outline': {'box': [0.0, 0.0, 1.0, 1.0], 'polygon': []},
                       'package_type': 'FCBGA',
                       'symmetry': 'none',
                       'unit': 'm',
                       'warnings': []},
             'stack_path': '<fixture>/stack.json',
             'thermal': True}}
