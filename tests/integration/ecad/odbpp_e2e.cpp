#include "chipcae/geometry/ecad/ipc2581.hpp"
#include "chipcae/geometry/ecad/odbpp.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stdlib.h>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Paths = std::set<std::string>;
void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}
std::string quote(const std::string& value) {
    std::string result = "'";
    for (char c : value)
        result += c == '\'' ? "'\\''" : std::string(1, c);
    return result + "'";
}
std::vector<std::string> listing(const fs::path& archive, bool verbose) {
    const auto command =
        "cmake -E tar " + std::string(verbose ? "tvf " : "tf ") + quote(archive.string());
    FILE* pipe = popen(command.c_str(), "r");
    require(pipe != nullptr, "Cannot run " + command);
    std::string output;
    std::array<char, 4096> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe))
        output += buffer.data();
    const int status = pclose(pipe);
    require(status == 0, "Archive listing failed: " + command);
    std::vector<std::string> lines;
    std::istringstream stream(output);
    for (std::string line; std::getline(stream, line);)
        lines.push_back(line);
    return lines;
}
Paths archive_paths(const fs::path& archive) {
    require(fs::is_regular_file(archive), "Missing archive: " + archive.string());
    const auto names = listing(archive, false);
    const auto details = listing(archive, true);
    require(names.size() == details.size(), "Archive listings disagree: " + archive.string());
    Paths paths;
    std::set<std::string> roots;
    bool all_nested = true;
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto name = names[i];
        std::replace(name.begin(), name.end(), '\\', '/');
        while (name.starts_with("./"))
            name.erase(0, 2);
        if (name.empty())
            continue;
        const auto slash = name.find('/');
        roots.insert(name.substr(0, slash));
        if (details[i].empty() || details[i].front() != '-')
            continue; // verbose listing identifies directories and symbolic links
        all_nested = all_nested && slash != std::string::npos;
        require(paths.insert(name).second, "Duplicate archive file: " + name);
    }
    require(!paths.empty(), "Archive has no regular files: " + archive.string());
    if (roots.size() == 1 && all_nested) {
        Paths stripped;
        for (const auto& path : paths)
            stripped.insert(path.substr(path.find('/') + 1));
        return stripped;
    }
    return paths;
}
void odb_fixture(const fs::path& root) {
    const auto job = root / "kicad4l";
    require(fs::is_regular_file(job / "matrix/matrix"), "Missing fixture matrix/matrix");
    require(fs::is_regular_file(job / "misc/attrlist"), "Missing fixture misc/attrlist");
    Paths directory;
    int profiles = 0;
    for (const auto& entry : fs::recursive_directory_iterator(job)) {
        if (!fs::is_regular_file(entry.symlink_status()))
            continue;
        const auto relative = entry.path().lexically_relative(job);
        directory.insert(relative.generic_string());
        if (std::distance(relative.begin(), relative.end()) == 3 && *relative.begin() == "steps" &&
            relative.filename() == "profile")
            ++profiles;
    }
    require(profiles == 1, "Expected exactly one steps/<step>/profile");
    for (const auto* name : {"kicad4l.tgz", "kicad4l.zip"}) {
        const auto paths = archive_paths(root / name);
        require(paths == directory,
                std::string(name) + " regular-file paths differ from directory");
    }
}
void ipc_fixture(const fs::path& root) {
    const auto result = chipcae::geometry::ecad::read_ipc2581(root / "kicad4l.ipc2581.xml");
    require(static_cast<bool>(result), "IPC fixture import failed: " + result.error);
    const auto copper = std::count_if(result.model.layers.begin(),
                                      result.model.layers.end(),
                                      [](const auto& layer) { return layer.copper; });
    require(copper == 4, "Expected exactly four copper layers; got " + std::to_string(copper));
}
// Python's standard library supplies JSON, archive mutations and decoded subprocess exits.
// Keep the driver with the native fixture checks; no new test dependencies are needed.
// The project already uses Python for ECAD CLI/contract tests.
constexpr const char* acceptance_driver = R"ODBCLI(
import hashlib
import io
import json
import math
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import traceback
import zipfile

source, build, selector, executable = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], sys.argv[4]
data = source / 'tests/integration/ecad/data'
fixture = data / 'odbpp'
sys.path.insert(0, str(source / 'tools/stack'))
from polyset import read_polyset, layer_area_m2

CASES = []
def case(name, group='odb'):
    def register(fn):
        CASES.append((name, group, fn))
        return fn
    return register

def check(value, message):
    if not value:
        raise AssertionError(message)

def load(path):
    check(path.is_file(), f'missing required artifact: {path.name}')
    return json.loads(path.read_text())

def same_double(a, b):
    return struct.pack('<d', a) == struct.pack('<d', b)

def relative(a, b, tol, label):
    check(math.isfinite(a) and math.isfinite(b), f'{label}: nonfinite value')
    check(abs(a-b) <= tol * max(abs(a), abs(b)), f'{label}: {a} != {b} (relative {tol})')

def area(points):
    return abs(sum(x*v-u*y for (x,y),(u,v) in zip(points, points[1:]+points[:1]))) / 2

class Board:
    def __init__(self, root):
        self.root = root
        self.cli = build / 'kernel/chipcae-import'
        check(self.cli.is_file(), f'build chipcae-import first: {self.cli}')
        self.map = root / 'materials.json'
        self.map.write_text(json.dumps({'copper':'cu', 'Copper':'cu',
                                       'COPPER':'cu', 'FR4':'fr4_eq', 'fr4':'fr4_eq',
                                       'layer:DIELECTRIC_1':'fr4_eq', 'copper_base':'fr4_eq'}))
        self.job = root / 'job'
        shutil.copytree(fixture / 'kicad4l', self.job)
        # Minimal geometry derived from the owned fixture isolates each CLI behavior.
        matrix = self.job / 'matrix/matrix'
        text = matrix.read_text()
        text = re.sub(r'LAYER\s*\{[^}]*\}', lambda m: m[0] if re.search(
            r'TYPE=(SIGNAL|DIELECTRIC)\s', m[0]) else '', text)
        matrix.write_text(text)
        for path in (self.job / 'steps/pcb/layers').glob('*/features'):
            path.write_text('UNITS=MM\n$0 rect1000x1000\nP 10 -10 0 P 0 0\n')
        self.ipc = fixture / 'kicad4l.ipc2581.xml'

    def run(self, input=None, extra=(), expected=0, name='out', report=None, prior=False):
        out = self.root / name
        out.mkdir(parents=True, exist_ok=True)
        stack = out / 'board.stack.json'
        layers = out / 'import_layers.json'
        check(not stack.exists(), 'test must start without a stack')
        sentinel = b'previous layer report\x00\n'
        if prior:
            layers.write_bytes(sentinel)
        defects = report or out / 'defects.json'
        defects.parent.mkdir(parents=True, exist_ok=True)
        cmd = [str(self.cli), str(input or self.job), '-o', str(stack),
               '--material-map', str(self.map), '--report', str(defects), *map(str, extra)]
        result = subprocess.run(cmd, cwd=self.root, capture_output=True, text=True, timeout=20)
        check(result.returncode == expected,
              f'CLI expected exit {expected}, got {result.returncode}\n'
              f'command: {cmd}\nstdout: {result.stdout}\nstderr: {result.stderr}')
        if expected in (2,6,7,9):
            check(not stack.exists(), f'exit {expected} published a stack')
            check(not stack.with_suffix('.polyset').exists(), f'exit {expected} published polyset')
            check(layers.read_bytes() == sentinel if prior else not layers.exists(),
                  f'exit {expected} changed/published import_layers.json')
        else:
            check(stack.is_file() and stack.with_suffix('.polyset').is_file(),
                  'successful CLI must publish stack and binary polyset')
        return out, result, defects

    def override(self, layers, **keys):
        path = self.root / 'override.json'
        path.write_text(json.dumps(dict(schema='chipcae.stack_override/0.1', layers=layers, **keys)))
        return path

    def rows(self, out, material_overrides=()):
        report = load(out / 'import_layers.json')
        rows = report['layers']
        check(isinstance(rows, list), 'layers must be an array')
        names = [r['name'] for r in rows]
        check(len(names) == len(set(names)), 'report contains duplicate layer names')
        stack = load(out / 'board.stack.json')
        kept = [r for r in rows if r['status'] == 'kept']
        check([r['name'] for r in kept] == [l['name'] for l in stack['layers']],
              'kept report rows must follow mapped stack order')
        for row in rows:
            for key in ('name','status','function','copper','material_key','material_id'):
                check(key in row, f'missing layer report key {key}')
            check(isinstance(row['name'], str) and row['name'], 'empty source layer name')
            check(isinstance(row['function'], str), 'function must retain source spelling')
            check(type(row['copper']) is bool, 'copper must be a boolean')
            check(isinstance(row['material_key'], str) and isinstance(row['material_id'], str),
                  'material keys and IDs must be strings')
            check(row['status'] in ('kept','skipped'), 'invalid row status')
            if row['status'] == 'skipped':
                check(not row['copper'] and row['material_key'] == row['material_id'] == '',
                      'skipped layer carries material/copper')
                check(not {'thickness_m','thickness_source','odb_value_m'} & row.keys(),
                      'skipped layer must omit thickness fields')
            else:
                check(row['thickness_m'] > 0 and math.isfinite(row['thickness_m']),
                      'kept layer has invalid thickness')
                check(isinstance(row['thickness_source'], str), 'missing provenance')
                layer = next(l for l in stack['layers'] if l['name'] == row['name'])
                check(same_double(row['thickness_m'], layer['thickness']),
                      'layer report differs from written stack thickness')
                check(row['material_id'] == (layer['copper']['metal']
                      if row['copper'] and row['name'] not in material_overrides
                      else layer['material'])
                      if row['material_key'] or row['name'] in material_overrides
                      else row['material_id'] == '', 'report material ID/empty-source rule')
        return report, rows

    def defect_rows(self, out):
        ds = load(out / 'import_report.json')['defects']
        check(ds, 'import_report.json.defects must contain real unsupported entries')
        for d in ds:
            check(d['kind'] == 'IMPORT_UNSUPPORTED_ELEMENT', 'unsupported defect kind')
            check(d['tier'] in ('B','C'), 'unsupported defect tier')
            check(d['defect_id'].startswith('board/'), 'ECAD defect IDs require board/ prefix')
            check(str(self.root) not in d['defect_id'], 'defect ID depends on source path')
        return ds

    def set_thickness(self, name, value):
        path = self.job / f'steps/pcb/layers/{name.lower()}/attrlist'
        text, count = re.subn(r'(?m)^\.layer_dielectric=.*$', f'.layer_dielectric={value}',
                              path.read_text())
        check(count == 1, 'fixture thickness mutation must replace exactly one value')
        path.write_text(text)

# Recurse over all keys, permitting only the source/id values named by the brief.
# Arc coordinates are compared geometrically below; ring vertex counts may differ.
def compare_stack(a, b, path=''):
    check(type(a) is type(b), f'{path}: JSON types differ')
    if isinstance(a, dict):
        check(a.keys() == b.keys(), f'{path}: recursive JSON keys differ')
        for key in a:
            if key in ('source','id'):
                continue
            if path == '/outline' and key == 'polygon':
                check(all(len(p) == 2 for p in a[key]+b[key]), 'outline point structure')
                relative(area(a[key]), area(b[key]), 1e-6, 'outline area')
            else:
                compare_stack(a[key], b[key], path+'/'+key)
    elif isinstance(a, list):
        check(len(a) == len(b), f'{path}: JSON array sizes differ')
        for i, (x,y) in enumerate(zip(a,b)):
            compare_stack(x,y,path+f'/{i}')
    elif isinstance(a, (int,float)) and not isinstance(a, bool):
        relative(a,b,1e-6,path)
    else:
        check(a == b, f'{path}: {a!r} != {b!r}')

# Invalid mapping targets must reject before B-tier policy or artifact publication.
# Explicit copper bases isolate a dielectric failure from nearest-dielectric inheritance.
for fmt in ('odb', 'ipc'):
    for field, target in (('dielectric', 'mat.bt'), ('dielectric', 'bt'),
                          ('material', 'mat.bt'), ('metal', 'mat.cu')):
        for policy in ('plain', 'unaccepted_B', 'accepted_B'):
            @case(f'map_target_{fmt}_{field}_{target}_{policy}', fmt)
            def map_target(b, fmt=fmt, field=field, target=target, policy=policy):
                mapping = load(b.map)
                copper_names = ('F.CU', 'IN1.CU', 'IN2.CU', 'B.CU') if fmt == 'odb' else (
                    'F.Cu', 'In1.Cu', 'In2.Cu', 'B.Cu')
                for name in copper_names:
                    mapping['copper_base@'+name] = 'fr4_eq'
                dielectric_name = 'DIELECTRIC_1' if fmt == 'odb' else 'Core'
                if field == 'dielectric':
                    if fmt == 'odb':
                        attr = b.job/'steps/pcb/layers/dielectric_1/attrlist'
                        attr.write_text(attr.read_text().replace('.material=fr4',
                                                                '.material=unique_core'))
                        mapping['unique_core'] = target
                    else:
                        mapping['FR4'] = target
                    layer, diagnostic_field = dielectric_name, 'material'
                elif field == 'material':
                    mapping['copper_base@'+copper_names[0]] = target
                    layer, diagnostic_field = copper_names[0], 'material'
                else:
                    for key in ('copper', 'Copper', 'COPPER'):
                        mapping[key] = target
                    layer, diagnostic_field = copper_names[0], 'metal'
                b.map.write_text(json.dumps(mapping))
                if fmt == 'odb':
                    input = b.job
                    if policy != 'plain':
                        path = b.job/'steps/pcb/layers/f.cu/features'
                        path.write_text(path.read_text()+'T 1 1 standard P 0 0 1 1 0 text\n')
                else:
                    input = b.root/'mapped.xml'
                    xml = (data/'board_4layer.xml').read_text()
                    if policy != 'plain':
                        xml = xml.replace('<Ecad>', '<VendorExtension/><Ecad>')
                    input.write_text(xml)
                rule = ('homogenized material must end with _eq' if target == 'bt' else
                        "must match ^[a-z0-9_-]+$ (lowercase letters, digits, '_' or '-'; "
                        'no dots, e.g. cu, fr4_eq)')
                expected = f"layer '{layer}' {diagnostic_field} '{target}': {rule}"
                for prior in (False, True):
                    out, result, defects = b.run(input,
                        ['--accept-b-tier'] if policy == 'accepted_B' else [],
                        expected=7, name=f'out{prior}', prior=prior)
                    report = load(defects)
                    check(report['code'] == 'IMPORT_REJECTED',
                          'ecad-map-targets: illegal target must not be missing-mapping code')
                    check(report['message'] == expected,
                          f'ecad-map-targets: expected {expected!r}, got {report["message"]!r}')
                    check(result.stderr == 'IMPORT_REJECTED: '+expected+'\n',
                          f'ecad-map-targets: diagnostic prefix/precedence: {result.stderr!r}')
                    check(not list(out.glob('.chipcae-import-*')),
                          'ecad-map-targets: rejected target leaked staged artifacts')

@case('cross_ipc')
def cross_ipc(b):
    a,_,_ = b.run(fixture / 'kicad4l.tgz', ['--accept-b-tier'], name='odb')
    c,_,_ = b.run(b.ipc, ['--accept-b-tier'], name='ipc')
    x,y = load(a/'board.stack.json'), load(c/'board.stack.json')
    compare_stack(x,y)
    px,py = read_polyset(a/'board.stack.polyset'), read_polyset(c/'board.stack.polyset')
    check(len(px.layers) == len(py.layers) == len(x['layers']), 'decoded layer counts differ')
    for lx,ly,sx,sy in zip(px.layers,py.layers,x['layers'],y['layers']):
        check(lx.name == sx['name'] and ly.name == sy['name'], 'polyset layer order')
        check(('copper' in sx) == ('copper' in sy), 'copper flags differ')
        if 'copper' in sx:
            relative(layer_area_m2(lx, px.dbu_m), layer_area_m2(ly, py.dbu_m), 1e-3,
                     f'copper area {lx.name}')
    for out, stack in ((a,x),(c,y)):
        for ref in stack['external_refs']:
            check(hashlib.sha256((out/ref['path']).read_bytes()).hexdigest() == ref['sha256'],
                  'sidecar hash does not match payload')
    result = subprocess.run([executable,'cross_models',str(b.map)], capture_output=True, text=True)
    check(result.returncode == 0, result.stdout+result.stderr)

@case('dir_and_zip')
def dir_and_zip(b):
    a,_,_ = b.run(fixture/'kicad4l', ['--accept-b-tier'], name='directory')
    c,_,_ = b.run(fixture/'kicad4l.zip', ['--accept-b-tier'], name='zip')
    for name in ('board.stack.json','board.stack.polyset'):
        check((a/name).read_bytes() == (c/name).read_bytes(), f'{name}: directory/ZIP bytes differ')

# The five fatal archive fixtures used by the parser acceptance suite.
for fatal in ('truncated','bad-crc','missing-matrix','missing-profile','traversal'):
    @case('corrupt_'+fatal)
    def corrupt(b, fatal=fatal):
        files = [(p.relative_to(b.job).as_posix(),p.read_bytes())
                 for p in sorted(b.job.rglob('*')) if p.is_file()]
        path = b.root / ('bad.zip' if fatal == 'bad-crc' else 'bad.tgz')
        if fatal == 'bad-crc':
            with zipfile.ZipFile(path,'w') as z:
                z.writestr('matrix/matrix',b'UNIQUE_PAYLOAD')
            path.write_bytes(path.read_bytes().replace(b'UNIQUE_PAYLOAD',b'BROKEN_PAYLOAD'))
        else:
            with tarfile.open(path,'w:gz') as t:
                if fatal == 'traversal':
                    files = [('../matrix/matrix',b'bad')]
                for name,content in files:
                    if fatal == 'missing-matrix' and name == 'matrix/matrix': continue
                    if fatal == 'missing-profile' and name == 'steps/pcb/profile': continue
                    entry = tarfile.TarInfo(name)
                    entry.size = len(content)
                    t.addfile(entry,io.BytesIO(content))
            if fatal == 'truncated': path.write_bytes(path.read_bytes()[:20])
        for prior in (False,True):
            _,_,d = b.run(path, expected=7, prior=prior, name=f'out{prior}')
            check(load(d)['code'] == 'IMPORT_REJECTED', 'fatal corruption code')

for mode in ('text','symbol','C_then_B','B_then_C'):
    @case('unsupported_tiers_'+mode)
    def unsupported(b, mode=mode):
        path = b.job/'steps/pcb/layers/f.cu/features'
        base = 'UNITS=MM\n$0 rect1000x1000\nP 10 -10 0 P 0 0\n'
        text = 'T 1 1 standard P 0 0 1 1 0 text\n'
        symbol = '$1 vendor_shape\nP 12 -10 1 P 0 0\n'
        added = {'text':text,'symbol':symbol,'C_then_B':symbol+text,'B_then_C':text+symbol}[mode]
        path.write_text(base+added)
        for accepted in (False,True):
            wanted = (0 if accepted else 6) if mode == 'text' else 7
            out,_,_ = b.run(extra=['--accept-b-tier'] if accepted else [], expected=wanted,
                            name=f'out{accepted}', prior=wanted != 0)
            ds = b.defect_rows(out)
            check(any(d['tier'] == ('B' if mode == 'text' else 'C') for d in ds),
                  'missing geometry/text classification')
            if '_then_' in mode:
                check(any(d['tier'] == 'B' for d in ds), 'mixed fixture did not report B')

# Unknown ODB records and component-like path substrings must never be accepted as B.
for loss in ('feature_TT', 'profile_T', 'misc_unknown', 'components_substring'):
    @case('odb_unknown_geometry_'+loss)
    def unknown_odb_geometry(b, loss=loss):
        if loss == 'feature_TT':
            path = b.job/'steps/pcb/layers/f.cu/features'
            path.write_text(path.read_text()+'TT vendor geometry\n')
        elif loss == 'profile_T':
            path = b.job/'steps/pcb/profile'
            path.write_text(path.read_text()+'T vendor profile geometry\n')
        else:
            path = b.job/('misc/unknown_geometry.Z' if loss == 'misc_unknown' else
                          'steps/pcb/layers/f.cu/components_geometry.Z')
            path.write_bytes(b'unmodeled compressed content')
        for accepted in (False, True):
            out,_,_ = b.run(extra=['--accept-b-tier'] if accepted else [], expected=7,
                            name=f'out{accepted}', prior=True)
            ds = b.defect_rows(out)
            check(ds and all(d['tier'] == 'C' for d in ds),
                  'unknown ODB geometry must remain C regardless of acceptance')

for tag,context in (('PolyStepCurve','profile'), ('Cutout','profile'),
                    ('ProfileHole','profile'), ('Contour','copper'), ('Oval','copper'),
                    ('RectCenter','copper'), ('Circle','copper'), ('Symbol','copper'), ('Trace','copper'),
                    ('Slot','copper'), ('ViaGeometry','copper'),
                    ('VendorExtension','metadata'), ('CopperVendorExtension','metadata'),
                    ('OutlineVendorExtension','metadata')):
    @case('ipc_bare_diagnostic_'+tag,'ipc')
    def ipc_bare_diagnostic(b, tag=tag, context=context):
        xml = (data/'board_4layer.xml').read_text()
        if context == 'profile':
            xml = xml.replace('</Polygon></Profile>', f'<{tag}/></Polygon></Profile>')
        elif context == 'copper':
            # #142 added one <Set> per copper layer; this case needs
            # one bare element and one defect.
            xml = xml.replace('<Set>', f'<Set><{tag}/>', 1)
            check(xml.count(f'<{tag}/>') == 1,
                  f'bare {tag}: expected one inserted element, got {xml.count(f"<{tag}/>")}')
        else:
            xml = xml.replace('<Ecad>', f'<{tag}/><Ecad>')
        path = b.root/'bare.xml'
        path.write_text(xml)
        geometry = context != 'metadata'
        for accepted in (False,True):
            wanted = 7 if geometry else (0 if accepted else 6)
            out,_,_ = b.run(path, ['--accept-b-tier'] if accepted else [], expected=wanted,
                            name=f'out{accepted}', prior=wanted != 0)
            ds = b.defect_rows(out)
            check(len(ds) == 1 and ds[0]['tier'] == ('C' if geometry else 'B'),
                  f'bare {tag} must be classified by geometry context')
            if geometry:
                check(ds[0]['suggestion'] == 'unsupported geometry must be modeled before import',
                      'geometry loss must retain geometry remediation')

@case('ipc_unsupported_now_reported','ipc')
def ipc_unsupported(b):
    out,_,_ = b.run(data/'unsupported.xml', expected=6, prior=True)
    ds = b.defect_rows(out)
    check(len(ds) == 3 and all(d['tier'] == 'B' for d in ds), 'three metadata losses must be B')
    # Generic ID suffix encoding is unspecified; assert source-independent, distinct IDs.
    check(len({d['defect_id'] for d in ds}) == 3, 'different metadata records share IDs')
    copy = b.root/'relocated.xml'
    shutil.copyfile(data/'unsupported.xml',copy)
    other,_,_ = b.run(copy, expected=6, name='relocated')
    check({d['defect_id'] for d in ds} == {d['defect_id'] for d in b.defect_rows(other)},
          'IDs depend on source filename')

@case('override_fill_missing')
def fill_missing(b):
    for name in ('DIELECTRIC_1','DIELECTRIC_2'): b.set_thickness(name,'0.0')
    for prior in (False,True):
        _,r,d = b.run(expected=7, prior=prior, name=f'out{prior}')
        report = load(d)
        check(report['code'] == 'IMPORT_REJECTED', 'missing thickness rejection code')
        for name in ('DIELECTRIC_1','DIELECTRIC_2'):
            check(name in report['message'], f'aggregate missing thickness omits {name}: {r.stderr}')

for invalid in ('0.0', '-0.1', 'inf', 'nan', 'missing'):
    @case('invalid_retained_thickness_'+invalid)
    def invalid_retained_thickness(b, invalid=invalid):
        for name in ('DIELECTRIC_1','DIELECTRIC_2'):
            if invalid == 'missing':
                path = b.job / f'steps/pcb/layers/{name.lower()}/attrlist'
                path.write_text(re.sub(r'(?m)^\.layer_dielectric=.*\n?', '', path.read_text()))
            else:
                b.set_thickness(name, invalid)
        out = b.root / 'existing'
        out.mkdir()
        paths = [out/'board.stack.json', out/'board.stack.polyset', out/'import_layers.json']
        sentinels = [b'previous stack\x00\n', b'previous polyset\x00\n', b'previous layers\x00\n']
        for path, sentinel in zip(paths, sentinels):
            path.write_bytes(sentinel)
        result = subprocess.run([str(b.cli), str(b.job), '-o', str(paths[0]),
                                 '--material-map', str(b.map)],
                                cwd=b.root, capture_output=True, text=True, timeout=20)
        check(result.returncode == 7, f'invalid retained thickness must reject: {result.stderr}')
        report = load(out/'defects.json')
        check(report['code'] == 'IMPORT_REJECTED', 'invalid thickness rejection code')
        for name in ('DIELECTRIC_1','DIELECTRIC_2'):
            check(name in report['message'] and name in result.stderr,
                  f'aggregate invalid thickness omits {name}: {result.stderr}')
        for path, sentinel in zip(paths, sentinels):
            check(path.read_bytes() == sentinel, f'invalid thickness changed {path.name}')
        check(not list(out.glob('.chipcae-import-*')), 'invalid thickness leaked staging output')

@case('override_fill')
def fill(b):
    values = {'DIELECTRIC_1':0.00028123456789,'DIELECTRIC_2':0.00106123456789}
    for name in values: b.set_thickness(name,'0.0')
    override = b.override([dict(name=n.swapcase(),thickness_m=v) for n,v in values.items()],
                          source='owned fixture thickness values')
    out,_,_ = b.run(extra=['--stack-override',override])
    report,rows = b.rows(out)
    check(report['source'] == 'owned fixture thickness values', 'source text changed/omitted')
    for row in rows:
        if row['status'] != 'kept': continue
        expected = 'override:override.json' if row['name'] in values else 'odb'
        check(row['thickness_source'] == expected, 'incorrect ODB provenance')
        check('odb_value_m' not in row, 'zero original must not be reported as a conflict')
        if row['name'] in values:
            check(same_double(row['thickness_m'],values[row['name']]), 'override lost double precision')

for equal in (False,True):
    @case('override_equal' if equal else 'override_conflict')
    def conflict(b, equal=equal):
        original = 0.0002
        value = original if equal else 0.00028123456789
        override = b.override([dict(name='DIELECTRIC_1',thickness_m=value)])
        out,result,_ = b.run(extra=['--stack-override',override])
        report,rows = b.rows(out)
        row = next(r for r in rows if r['name'] == 'DIELECTRIC_1')
        # Acceptance explicitly requires override-wins, despite the prose precedence conflict.
        check(same_double(row['thickness_m'],value), 'acceptance requires override-wins')
        check(row['thickness_source'] == 'override:override.json', 'override provenance')
        warnings = report.get('warnings',[])
        check(len(warnings) == (0 if equal else 1), 'expected exactly one conflict warning, none if equal')
        if equal:
            check('odb_value_m' not in row, 'equal override is not a conflict')
        else:
            check(same_double(row['odb_value_m'],original), 'original ODB thickness missing/changed')
            check('DIELECTRIC_1' in warnings[0] and warnings[0] in result.stderr,
                  'conflict warning must identify layer and be printed to stderr')

@case('override_ipc','ipc')
def override_ipc(b):
    value = 0.00028123456789
    override = b.override([dict(name='dielectric_1',thickness_m=value)])
    out,_,_ = b.run(b.ipc,['--stack-override',override,'--accept-b-tier'])
    _,rows = b.rows(out)
    for r in rows:
        if r['status'] != 'kept': continue
        check('odb_value_m' not in r, 'IPC report must omit odb_value_m')
        check(r['thickness_source'] == ('override:override.json' if r['name'] == 'DIELECTRIC_1'
                                       else 'ipc'), 'incorrect IPC provenance')
        if r['name'] == 'DIELECTRIC_1':
            check(same_double(r['thickness_m'],value), 'IPC override lost double precision')

BAD = {
    'zero': {'layers':[{'name':'DIELECTRIC_1','thickness_m':0}]},
    'negative': {'layers':[{'name':'DIELECTRIC_1','thickness_m':-1}]},
    'nonfinite': {'layers':[{'name':'DIELECTRIC_1','thickness_m':float('inf')}]},
    'nan': {'layers':[{'name':'DIELECTRIC_1','thickness_m':float('nan')}]},
    'overflow': '{"schema":"chipcae.stack_override/0.1","layers":[{"name":"DIELECTRIC_1","thickness_m":1e999}]}',
    'duplicate': {'layers':[{'name':n,'thickness_m':0.0002} for n in ('DIELECTRIC_1','dielectric_1')]},
    'malformed': '{', 'wrong_schema': {'schema':'bad','layers':[]},
    'missing_schema': '{"layers":[]}', 'wrong_schema_type': {'schema':1,'layers':[]},
    'unknown_top': {'layers':[],'typo':True},
    'unknown_layer': {'layers':[{'name':'DIELECTRIC_1','thickness_m':0.0002,'typo':True}]},
    'missing_layers': {}, 'wrong_layers_type': {'layers':{}},
    'empty_name': {'layers':[{'name':'','thickness_m':0.0002}]},
    'missing_name': {'layers':[{'thickness_m':0.0002}]},
    'wrong_name_type': {'layers':[{'name':3,'thickness_m':0.0002}]},
    'missing_thickness': {'layers':[{'name':'DIELECTRIC_1'}]},
    'string_thickness': {'layers':[{'name':'DIELECTRIC_1','thickness_m':'0.0002'}]},
    'bool_thickness': {'layers':[{'name':'DIELECTRIC_1','thickness_m':True}]},
    'wrong_material_type': {'layers':[{'name':'DIELECTRIC_1','thickness_m':0.0002,'material':3}]},
    'wrong_source_type': {'layers':[],'source':3}, 'missing_file': None,
}
for name,payload in BAD.items():
    @case('override_bad_'+name)
    def override_bad(b, payload=payload):
        path = b.root/'bad.json'
        if payload is not None:
            path.write_text(payload if isinstance(payload,str) else json.dumps(
                dict({'schema':'chipcae.stack_override/0.1'}, **payload)))
        for prior in (False,True):
            _,_,d = b.run(extra=['--stack-override',path],expected=7,prior=prior,name=f'out{prior}')
            check(load(d)['code'] == 'IMPORT_REJECTED', 'invalid override rejection code')

@case('override_empty')
def empty(b):
    path = b.override([])
    a,_,_ = b.run(name='base')
    c,_,_ = b.run(extra=['--stack-override',path],name='empty')
    for name in ('board.stack.json','board.stack.polyset'):
        check((a/name).read_bytes() == (c/name).read_bytes(), 'empty override changes output')

@case('override_unknown_layer')
def unknown_layer(b):
    path = b.override([dict(name='NoSuchLayer',thickness_m=0.0002)])
    out,result,_ = b.run(extra=['--stack-override',path])
    report,_ = b.rows(out)
    warnings = report['warnings']
    check(len(warnings) == 1 and 'NoSuchLayer' in warnings[0], 'unknown override must warn once')
    check(warnings[0] in result.stderr, 'unknown-layer warning missing from stderr')

for fmt, source_material in ((f,m) for f in ('odb','ipc')
                             for m in ('unmapped','mapped','empty_source')):
    @case('override_material_'+fmt+'_'+source_material,fmt)
    def material(b, fmt=fmt, source_material=source_material):
        key = '' if source_material == 'empty_source' else 'unique_core'
        mapping = json.loads(b.map.read_text())
        mapping.pop('layer:DIELECTRIC_1',None)
        if source_material == 'mapped': mapping[key] = 'mat.wrong'
        # Only the overridden layer lacks a map entry: IPC's per-layer source key is explicit.
        if fmt == 'ipc':
            # Use owned minimal IPC below to ensure no shared source material key masks precedence.
            input = b.root/'material.xml'
            input.write_text('<IPC-2581><Ecad><CadData><Layer name="Core" layerFunction="DIELECTRIC"/>'
                '<Layer name="Cu" layerFunction="SIGNAL"/>'
                '<Stackup><StackupGroup name="S"><StackupLayer layerOrGroupRef="Core" matDes="'+key+'" thickness="0.2"/>'
                '<StackupLayer layerOrGroupRef="Cu" matDes="copper" thickness="0.035"/>'
                '</StackupGroup></Stackup><Step name="b"><Profile><Polygon><PolyBegin x="0" y="0"/>'
                '<PolyStepSegment x="1" y="0"/><PolyStepSegment x="0" y="1"/>'
                '</Polygon></Profile></Step></CadData></Ecad></IPC-2581>')
            layer,original = 'Core',0.0002
        else:
            input = b.job
            attr = b.job/'steps/pcb/layers/dielectric_1/attrlist'
            attr.write_text(attr.read_text().replace('.material=fr4','.material='+key))
            layer,original = 'DIELECTRIC_1',0.0002
        if source_material == 'empty_source':
            # A fallback map without an override retains the existing empty report ID rule.
            mapping['layer:'+layer] = 'fr4_eq'
            b.map.write_text(json.dumps(mapping))
            base,_,_ = b.run(input,['--accept-b-tier'],name='base')
            _,base_rows = b.rows(base)
            r = next(r for r in base_rows if r['name'] == layer)
            check(r['material_key'] == r['material_id'] == '',
                  'empty source without a material override must keep empty report strings')
            check(next(l for l in load(base/'board.stack.json')['layers']
                       if l['name'] == layer)['material'] == 'fr4_eq', 'fallback material ID')
            del mapping['layer:'+layer]
        b.map.write_text(json.dumps(mapping))
        path = b.override([dict(name=layer,thickness_m=original,material='special_eq')])
        out,_,_ = b.run(input,['--stack-override',path,'--accept-b-tier'])
        check(next(l for l in load(out/'board.stack.json')['layers']
                   if l['name'] == layer)['material'] == 'special_eq', 'override stack material ID')
        _,rows = b.rows(out,material_overrides=(layer,))
        if fmt == 'ipc':
            check([l['name'] for l in load(out/'board.stack.json')['layers']] == ['Core','Cu'],
                  'partial IPC material override reordered layers')
        r = next(r for r in rows if r['name'] == layer)
        check(r['material_key'] == key and r['material_id'] == 'special_eq',
              'explicit material must bypass mapping while preserving source key')
        for r in rows:
            if r['name'] != layer and r['status'] == 'kept':
                check(r['material_id'] in ('cu','fr4_eq'), 'other layer mapping changed')

@case('override_ascii_fold_only')
def ascii_fold_only(b):
    path = b.override([dict(name=n,thickness_m=0.0002) for n in ('Ä','ä')])
    out,_,_ = b.run(extra=['--stack-override',path])
    report,_ = b.rows(out)
    check(len(report['warnings']) == 2, 'ASCII folding must not merge distinct Unicode names')

@case('override_supported_ipc','ipc')
def supported_ipc(b):
    input = data/'board_4layer.xml'
    value = 0.00028123456789
    path = b.override([dict(name='core',thickness_m=value)])
    out,_,_ = b.run(input,['--stack-override',path])
    _,rows = b.rows(out)
    core = next(r for r in rows if r['name'] == 'Core')
    check(same_double(core['thickness_m'],value), 'IPC override must retain selected double')
    check(core['thickness_source'] == 'override:override.json', 'IPC override provenance')
    check(all('odb_value_m' not in r for r in rows), 'IPC must omit ODB originals')
    path = b.override([])
    base,_,_ = b.run(input,name='base')
    empty,_,_ = b.run(input,['--stack-override',path],name='empty')
    for name in ('board.stack.json','board.stack.polyset'):
        check((base/name).read_bytes() == (empty/name).read_bytes(), 'empty IPC override changes output')

@case('override_empty_ipc','ipc')
def empty_ipc(b):
    path = b.override([])
    a,_,_ = b.run(b.ipc,['--accept-b-tier'],name='base')
    c,_,_ = b.run(b.ipc,['--accept-b-tier','--stack-override',path],name='empty')
    for name in ('board.stack.json','board.stack.polyset'):
        check((a/name).read_bytes() == (c/name).read_bytes(), 'empty IPC override changes output')

for fmt in ('odb','ipc'):
    @case('unmapped_material_'+fmt,fmt)
    def unmapped(b, fmt=fmt):
        b.map.write_text('{}')
        for prior in (False,True):
            _,result,_ = b.run(b.job if fmt == 'odb' else b.ipc,['--accept-b-tier'],expected=9,
                          name=f'out{prior}',prior=prior)
            check('MATERIAL_MAPPING_MISSING' in result.stderr, 'missing-material diagnostic')

for extension in ('.TgZ','.TaR.Gz','.ZiP'):
    @case('extension'+extension)
    def extension_case(b, extension=extension):
        path = b.root/('board'+extension)
        shutil.copyfile(fixture/('kicad4l.zip' if extension == '.ZiP' else 'kicad4l.tgz'),path)
        b.run(path,['--accept-b-tier'])

def report_contract(b, out):
    report = load(out/'import_report.json')
    summary = load(out/'defects.json')
    check(report.get('schema_version') == 'import-report/1.0', 'import report schema')
    check(summary.get('schema_version') == 'import-report/1.0', 'summary schema')
    check('welded_interfaces' not in report and 'welded_interfaces' not in summary,
          'ECAD must omit assembly weld count')
    check(summary['defects'] == [], 'ECAD summary must retain empty defects')
    ds = b.defect_rows(out)
    ids = [d['defect_id'] for d in ds]
    check(all(ids) and len(ids) == len(set(ids)), 'final IDs must be nonempty and unique')
    check(all(d.get('size_unit') == '1' for d in ds), 'ECAD count units')
    return ds

@case('acceptance','ipc')
def acceptance(b):
    local = b.root/'unsupported.xml'
    shutil.copyfile(data/'unsupported.xml',local)
    out,_,_ = b.run(local,expected=6)
    ds = report_contract(b, out)
    ids = [d['defect_id'] for d in ds]
    accept = b.root/'import.accept.json'
    accept.write_text(json.dumps({'accept':[{'defect_id':i} for i in ids+ids]}))
    b.run(local,expected=6,name='no_implicit_accept',prior=True)
    confirmed,_,_ = b.run(local,['--accept',accept],name='confirmed')
    confirmed_ds = report_contract(b, confirmed)
    check([d['defect_id'] for d in confirmed_ds] == ids, 'replay changed final IDs')
    check(all(d['action'] and not d['rejection_reason'] for d in confirmed_ds),
          'exact acceptance must repair every B defect')
    # Repeated source paths exercise the existing ECAD /n IDs unchanged.
    duplicate = b.root/'duplicate.xml'
    text = local.read_text()
    marker = '<MysteryA/>'
    check(marker in text, 'synthetic unsupported fixture marker changed')
    duplicate.write_text(text.replace(marker, marker + marker))
    repeated,_,_ = b.run(duplicate,expected=6,name='repeated')
    repeated_ds = report_contract(b, repeated)
    check([d['defect_id'] for d in repeated_ds] ==
          ['board/MysteryA/2', 'board/MysteryA/2/1', 'board/MysteryB/2', 'board/MysteryC/2'],
          'unique ECAD /1 suffix must remain unchanged')
    accept.write_text(json.dumps({'accept':[{'defect_id':d['defect_id']} for d in repeated_ds]}))
    replayed,_,_ = b.run(duplicate,['--accept',accept],name='repeated_confirmed')
    check([d['defect_id'] for d in report_contract(b, replayed)] ==
          [d['defect_id'] for d in repeated_ds], 'accept replay changed /n IDs')
    accept.write_text(json.dumps({'accept':[{'defect_id':i.upper()} for i in ids]}))
    b.run(local,['--accept',accept],expected=6,name='case_changed',prior=True)

for reverse in (False,True):
    @case('accept_conflict_'+str(reverse),'ipc')
    def accept_conflict(b, reverse=reverse):
        args = ['--accept',b.root/'unused.json','--accept-b-tier']
        if reverse: args = [args[-1],*args[:-1]]
        for prior in (False,True):
            b.run(b.ipc,args,expected=2,prior=prior,name=f'out{prior}')

@case('kicad_skipped_layers_reported','ipc')
def skipped(b):
    out,_,_ = b.run(data/'kicad_like/skip_nonstructural.xml',['--accept-b-tier'])
    _,rows = b.rows(out)
    skipped = {r['name']:r for r in rows if r['status'] == 'skipped'}
    check(set(skipped) == {'F.Silkscreen','F.Paste','B.Silkscreen'}, 'skipped layer names')
    check(skipped['F.Paste']['function'] == 'SOLDERPASTE', 'source paste function changed')
    check(skipped['F.Silkscreen']['function'] == 'SILKSCREEN', 'source silk function changed')
    ds = load(out/'import_report.json')['defects']
    check(ds == [], 'skipped-only fixture must not produce any defects')
    other,_,_ = b.run(data/'kicad_like/soldermask_b.xml',expected=6,name='mask')
    ds = b.defect_rows(other)
    check(len(ds) == 1 and ds[0]['tier'] == 'B' and
          ds[0]['suggestion'] == 'solder mask not modeled', 'soldermask B contract')

@case('odb_skipped_layers_reported')
def odb_skipped(b):
    out,_,_ = b.run(fixture/'kicad4l',['--accept-b-tier'])
    _,rows = b.rows(out)
    skipped = {r['name']:r for r in rows if r['status'] == 'skipped'}
    check(skipped['F.SILKSCREEN']['function'] == 'SILK_SCREEN', 'ODB source TYPE spelling')
    check(skipped['F.PASTE']['function'] == 'SOLDER_PASTE', 'ODB paste TYPE spelling')
    ds = b.defect_rows(out)
    check(any(d['tier'] == 'B' and d['suggestion'] == 'solder mask not modeled' for d in ds),
          'ODB soldermask must be reported as B')

@case('odb_norm_bridged_surface')
def odb_norm_bridged_surface(b):
    import time
    started = time.monotonic()
    from fractions import Fraction

    def twice_area(ring):
        return sum(x*v-u*y for (x,y),(u,v) in zip(ring, ring[1:]+ring[:1]))

    def orient(a, b, c):
        return (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0])

    def on_segment(a, b, p):
        return (orient(a,b,p) == 0 and
                min(a[0],b[0]) <= p[0] <= max(a[0],b[0]) and
                min(a[1],b[1]) <= p[1] <= max(a[1],b[1]))

    def intersects(a, b, c, d):
        return ((orient(a,b,c)*orient(a,b,d) < 0 and
                 orient(c,d,a)*orient(c,d,b) < 0) or
                on_segment(a,b,c) or on_segment(a,b,d) or
                on_segment(c,d,a) or on_segment(c,d,b))

    def valid_ring(ring, label):
        check(len(ring) >= 3, f'{label}: fewer than three vertices: {ring}')
        check(twice_area(ring) != 0, f'{label}: zero signed area: {ring}')
        edges = list(zip(ring, ring[1:]+ring[:1]))
        for i,(a,c) in enumerate(edges):
            check(a != c, f'{label}: duplicate adjacent vertices at edge {i}: {ring}')
            d = ring[(i+2) % len(ring)]
            dot = (c[0]-a[0])*(d[0]-c[0]) + (c[1]-a[1])*(d[1]-c[1])
            check(not (orient(a,c,d) == 0 and dot < 0),
                  f'{label}: adjacent edges retrace at vertex {(i+1) % len(ring)}: {ring}')
            for j in range(i+1, len(edges)):
                if j == i+1 or (i == 0 and j == len(edges)-1):
                    continue
                check(not intersects(a,c,*edges[j]),
                      f'{label}: nonadjacent edges {i}/{j} intersect or touch: {ring}')

    def strictly_inside(p, ring):
        inside = False
        for a,c in zip(ring, ring[1:]+ring[:1]):
            if on_segment(a,c,p):
                return False
            # Half-open Y intervals count a ray/vertex crossing once. The cross
            # product locates the crossing without division or float rounding.
            if (a[1] > p[1]) != (c[1] > p[1]):
                if (orient(a,c,p) > 0) == (c[1] > a[1]):
                    inside = not inside
        return inside

    features = b.job/'steps/pcb/layers/f.cu/features'
    # One island follows two clockwise holes through coincident return bridges.
    features.write_text('''UNITS=MM
S P 0
OB 20 -30 I
OS 22 -30
OS 22 -28
OS 22 -26
OS 24 -26
OS 24 -28
OS 22 -28
OS 22 -30
OS 26 -30
OS 26 -28
OS 26 -26
OS 28 -26
OS 28 -28
OS 26 -28
OS 26 -30
OS 30 -30
OS 30 -20
OS 20 -20
OS 20 -30
OE
SE
''')
    a1,_,_ = b.run(name='a1')
    a2,_,_ = b.run(name='a2')
    features.write_text('''UNITS=MM
S P 0
OB 20 -30 I
OS 30 -30
OS 30 -20
OS 20 -20
OS 20 -30
OE
OB 22 -28 H
OS 22 -26
OS 24 -26
OS 24 -28
OS 22 -28
OE
OB 26 -28 H
OS 26 -26
OS 28 -26
OS 28 -28
OS 26 -28
OE
SE
''')
    explicit,_,_ = b.run(name='b')

    results = {}
    for name,out in (('a1',a1),('a2',a2),('b',explicit)):
        stack = load(out/'board.stack.json')
        names = [layer['name'] for layer in stack['layers']]
        check('F.CU' in names, f'{name}: F.CU absent from stack; layers={names}')
        polyset = read_polyset(out/'board.stack.polyset')
        names = [layer.name for layer in polyset.layers]
        layer = next((layer for layer in polyset.layers if layer.name == 'F.CU'), None)
        check(layer is not None, f'{name}: F.CU absent from polyset; layers={names}')
        rings = layer.rings
        check(rings, f'{name}: F.CU has no rings; layers={names}')
        for i,ring in enumerate(rings):
            valid_ring(ring, f'{name} F.CU ring {i}')
        signed = sorted(twice_area(ring) for ring in rings)
        outer = [ring for ring in rings if twice_area(ring) > 0]
        holes = [ring for ring in rings if twice_area(ring) < 0]
        check(len(rings) == 3 and len(outer) == 1 and len(holes) == 2,
              f'{name}: expected one CCW exterior and two CW holes; twice_areas={signed}')
        for i,hole in enumerate(holes):
            check(all(strictly_inside(p,outer[0]) for p in hole),
                  f'{name}: hole {i} must lie strictly inside the exterior: {hole}')

        # Decimal rational DBU conversion keeps the expected integer area exact.
        dbu = Fraction(str(polyset.dbu_m))
        def expected_twice(mm2):
            value = 2 * Fraction(mm2, 1000000) / (dbu * dbu)
            check(value.denominator == 1,
                  f'{name}: {mm2} mm^2 is not an integer twice-area at dbu_m={dbu}')
            return value.numerator

        expected = sorted(expected_twice(mm2) for mm2 in (100,-4,-4))
        actual_m2 = [value * 0.5 * polyset.dbu_m**2 for value in signed]
        expected_m2 = [-4e-6,-4e-6,100e-6]
        check(signed == expected,
              f'{name}: twice_areas={signed}, expected={expected}; dbu_m={polyset.dbu_m}; '
              f'areas_m2={actual_m2}, expected_m2={expected_m2}')
        check(sum(signed) == expected_twice(92),
              f'{name}: net twice-area={sum(signed)}, expected={expected_twice(92)}')
        for actual,want in zip(actual_m2,expected_m2):
            check(abs(actual-want) <= 1e-12 * abs(want),
                  f'{name}: area_m2={actual}, expected={want}, relative tolerance=1e-12')
        net_m2 = sum(signed) * 0.5 * polyset.dbu_m**2
        check(abs(net_m2-92e-6) <= 1e-12 * 92e-6,
              f'{name}: net area_m2={net_m2}, expected=92e-6, relative tolerance=1e-12')
        results[name] = (polyset.dbu_m, len(rings), signed)
        print(f'GEOMETRY odb_norm_bridged_surface run={name} '
              f'dbu_m={polyset.dbu_m:.17g} rings={len(rings)} twice_areas={signed} '
              f'areas_m2={actual_m2} net_m2={net_m2:.17g}')

    # Bridge feet may leave collinear vertices; compare signed areas and counts
    # without constraining ring start points or the retained intermediate points.
    for name in ('a1','a2'):
        check(same_double(results[name][0],results['b'][0]),
              f'{name}/b: dbu_m differs: {results[name][0]} vs {results["b"][0]}')
        check(results[name][1:] == results['b'][1:],
              f'{name}/b: bridged and explicit-hole ring counts/areas differ: '
              f'{results[name][1:]} vs {results["b"][1:]}')
    for name in ('board.stack.json','board.stack.polyset','import_layers.json',
                 'import_report.json','import_report.csv','defects.json'):
        check((a1/name).is_file() and (a2/name).is_file(),
              f'a1/a2: missing determinism artifact: {name}')
        check((a1/name).read_bytes() == (a2/name).read_bytes(),
              f'a1/a2: nondeterministic artifact: {name}')
    elapsed = time.monotonic() - started
    print(f'MEASURE odb_norm_bridged_surface seconds={elapsed:.6f}')
    check(elapsed <= 5, f'odb_norm_bridged_surface took {elapsed:.6f}s; limit=5s')

@case('via_plating_missing','ipc')
def plating(b):
    out,_,_ = b.run(data/'kicad_like/via_plating_mixed.xml',expected=6)
    ds = report_contract(b, out)
    d = next((d for d in ds if d['defect_id'] == 'board/via_plating_missing/F.Cu-B.Cu'),None)
    check(d is not None, 'missing board-scoped via-plating defect ID')
    check(d['tier'] == 'B' and d['kind'] == 'IMPORT_UNSUPPORTED_ELEMENT' and d['size'] == 2,
          'missing-plating count/tier/kind')
    accepted,_,_ = b.run(data/'kicad_like/via_plating_mixed.xml',
                         ['--accept-b-tier'],name='accepted')
    accepted_ds = report_contract(b, accepted)
    check([d['defect_id'] for d in ds] == [d['defect_id'] for d in accepted_ds],
          'acceptance changed via-plating IDs')

@case('via_plating_exact_acceptance','ipc')
def plating_exact_acceptance(b):
    out,_,_ = b.run(data/'kicad_like/via_plating_mixed.xml',expected=6)
    ds = report_contract(b, out)
    accept = b.root/'via.accept.json'
    accept.write_text(json.dumps({'accept':[{'defect_id':d['defect_id']} for d in ds]}))
    replayed,_,_ = b.run(data/'kicad_like/via_plating_mixed.xml',
                         ['--accept',accept],name='exact_accepted')
    check([d['defect_id'] for d in report_contract(b, replayed)] ==
          [d['defect_id'] for d in ds], 'exact via-plating replay changed IDs')

@case('report_and_preview')
def report_preview(b):
    # The stack loader requires an explicit homogenized ID for dielectric layers.
    mapping = load(b.map)
    b.map.write_text(json.dumps({key: ('fr4_eq' if value == 'fr4_eq' else value)
                                for key,value in mapping.items()}))
    custom = b.root/'reports/custom.json'
    out,_,d = b.run(report=custom)
    check(load(d)['code'] == 'OK', 'custom defects report success code')
    check(not (out/'defects.json').exists(), '--report must move only defects.json')
    b.rows(out)
    check((out/'import_report.json').is_file() and not (custom.parent/'import_layers.json').exists(),
          '--report moved layer/import report')
    preview = build/'preview/chipcae-preview'
    check(preview.is_file(), 'build chipcae-preview for smoke acceptance')
    # Refine Z counts for the thick core and explicitly use the fixture's ratio 10.
    # Preserve imported geometry and keep the original import artifacts untouched.
    mesh_stack = load(out/'board.stack.json')
    spacing = min(layer['thickness'] for layer in mesh_stack['layers'])
    for layer in mesh_stack['layers']:
        layer['n_elem_min'] = math.ceil(layer['thickness']/spacing)
    mesh_input = out/'preview.stack.json'
    mesh_input.write_text(json.dumps(mesh_stack))
    result = subprocess.run([str(preview),str(mesh_input),'--nx','4','--ny','4',
                             '--max-ratio','10','-o',str(out/'mesh')],capture_output=True,text=True,timeout=20)
    check(result.returncode == 0, f'preview exit {result.returncode}: {result.stdout}{result.stderr}')
    meshes = list(out.glob('mesh*.vtkhdf'))
    check(meshes and all(p.stat().st_size > 0 for p in meshes), 'preview generated no mesh')

@case('publication_failure')
def publication_failure(b):
    out = b.root/'publish'
    out.mkdir()
    stack, polyset, layers = out/'board.stack.json', out/'board.stack.polyset', out/'import_layers.json'
    # Force failure on the final publication, after both geometry files moved.
    layers.mkdir()
    (layers/'sentinel').write_bytes(b'previous layer directory')
    for previous in (False, True):
        if previous:
            stack.write_bytes(b'previous stack')
            polyset.write_bytes(b'previous polyset')
        result = subprocess.run([str(b.cli), str(b.job), '-o', str(stack),
                                 '--material-map', str(b.map)], capture_output=True, text=True)
        check(result.returncode == 7, 'publication failure must reject')
        check(load(out/'defects.json')['code'] == 'IMPORT_REJECTED', 'publication rejection code')
        check((layers/'sentinel').read_bytes() == b'previous layer directory',
              'publication failure changed previous layer destination')
        if previous:
            check(stack.read_bytes() == b'previous stack' and polyset.read_bytes() == b'previous polyset',
                  'publication failure did not restore previous geometry files')
        else:
            check(not stack.exists() and not polyset.exists(), 'partial geometry publication survived failure')
        check(not list(out.glob('.chipcae-import-*')), 'staging directories leaked')

@case('help')
def help_case(b):
    result = subprocess.run([str(b.cli),'--help'],capture_output=True,text=True)
    check(result.returncode == 0, 'help must succeed')
    for token in ('ODB','--stack-override','--accept'):
        check(token in result.stdout, f'help omits {token}')

@case('dispatcher_values')
def dispatcher_values(b):
    # Valued ECAD options consume flag-shaped paths and reach the importer.
    for option in ('--accept','--stack-override'):
        for i,value in enumerate(('missing.json','--accept-b-tier','--report','--help','-1e-5')):
            _,result,_ = b.run(data/'board_4layer.xml', [option,value], expected=7,
                                name=option[2:]+str(i), prior=True)
            check(result.stdout == '' and 'not implemented yet' not in result.stderr,
                  'ECAD option was rejected before importer validation')
    # STEP and the remaining reserved switches retain their usage diagnostic.
    for option in ('--accept','--stack-override','--t-min','--assembly'):
        result = subprocess.run([str(b.cli),str(b.root/'part.step'),'-o',str(b.root/'part.json'),
                                 option,'missing.json'],capture_output=True,text=True,timeout=20)
        check(result.returncode == 2 and result.stdout == '' and
              result.stderr == 'not implemented yet: '+option+'\n', 'STEP reserved behavior changed')
        check(not (b.root/'part.json').exists(), 'reserved STEP option published a stack')

# Registered fixture groups assert the current prerequisite outcomes. Strict
# success selectors below remain available unchanged for the upstream follow-up.
for fmt in ('odb', 'ipc'):
    @case('dispatcher_prerequisites_'+fmt, fmt)
    def dispatcher_prerequisites(b):
        for option in ('--accept', '--stack-override'):
            _,result,_ = b.run(data/'board_4layer.xml', [option,b.root/'missing.json'],
                              expected=2, name=option[2:], prior=True)
            check(result.stdout == '' and result.stderr == 'not implemented yet: '+option+'\n',
                  'dispatcher prerequisite changed; enable strict flag selectors')

    @case('full_board_rejected_'+fmt, fmt)
    def full_board_rejected(b, fmt=fmt):
        input = fixture/'kicad4l' if fmt == 'odb' else b.ipc
        for accepted in (False, True):
            out,_,d = b.run(input, ['--accept-b-tier'] if accepted else [], expected=7,
                            name=f'full{accepted}', prior=True)
            check(load(d)['code'] == 'IMPORT_REJECTED', 'full-board rejection code')
            ds = b.defect_rows(out)
            check(any(d['tier'] == 'C' for d in ds), 'full-board geometry loss must remain C')
            if fmt == 'odb':
                check(any(d['defect_id'].startswith('board/Slot%3A') for d in ds),
                      'full-board oval slot diagnostic disappeared')

@case('archive_root_rejected')
def archive_root_rejected(b):
    for extension in ('.TgZ', '.TaR.Gz', '.ZiP'):
        path = b.root/('board'+extension)
        shutil.copyfile(fixture/('kicad4l.zip' if extension == '.ZiP' else 'kicad4l.tgz'),path)
        _,_,d = b.run(path, ['--accept-b-tier'], expected=7, name=extension[1:], prior=True)
        summary = load(d)
        check(summary['code'] == 'IMPORT_REJECTED' and 'matrix/matrix' in summary['message'],
              'archive root prerequisite changed; enable strict archive selectors')

selected = [(n,g,f) for n,g,f in CASES if (selector in ('all',g,n) or
            (selector == 'stack_override_fill' and n in ('override_fill','override_fill_missing')) or
            (selector == 'stack_override_conflict' and n in ('override_conflict','override_equal')) or
            (selector == 'stack_override_ipc' and n == 'override_ipc') or
            (selector == 'stack_override_bad' and n.startswith('override_bad_')) or
            (selector == 'odbpp_e2e_cross_ipc' and n == 'cross_ipc') or
            (selector == 'odbpp_e2e_dir_and_zip' and n == 'dir_and_zip') or
            (selector == 'corrupt' and n.startswith('corrupt_')) or
            (selector == 'override_bad' and n.startswith('override_bad_')) or
            (selector == 'unsupported_tiers' and n.startswith('unsupported_tiers_')))]
# Only the registered fixture groups are gated. 'all' and named acceptance
# selectors still run the original strict success assertions and tolerances.
if selector in ('odb', 'ipc'):
    def follow_up(name):
        if (name.startswith('override_') and name != 'override_fill_missing') or name in (
                'acceptance', 'via_plating_exact_acceptance', 'dispatcher_values'):
            return 'dispatcher forwarding of --accept/--stack-override'
        if name in ('cross_ipc', 'dir_and_zip', 'odb_skipped_layers_reported'):
            return 'full-board geometry/model support and archive root handling'
        if name.startswith('extension.'):
            return 'archive common-root handling'
        return None
    for name,_,_ in selected:
        reason = follow_up(name)
        if reason:
            print(f'FOLLOW-UP {name}: {reason}', flush=True)
    selected = [(n,g,f) for n,g,f in selected if follow_up(n) is None]
check(selected, f'unknown scenario {selector}; choose: '+', '.join(n for n,_,_ in CASES))
failures = 0
for name,_,fn in selected:
    with tempfile.TemporaryDirectory(prefix='chipcae-odbpp-cli-') as directory:
        try:
            fn(Board(Path(directory)))
        except Exception:
            failures += 1
            print('FAIL '+name,flush=True)
            traceback.print_exc(file=sys.stdout)
        else:
            print('PASS '+name,flush=True)
print(f'{len(selected)} scenarios, {failures} failed',flush=True)
sys.exit(bool(failures))
)ODBCLI";

// Exercise the public composition boundary independently of the CLI.
void public_api() {
    using namespace chipcae::geometry::ecad;
    auto pattern = (fs::temp_directory_path() / "chipcae-odbpp-api-XXXXXX").string();
    require(mkdtemp(pattern.data()) != nullptr, "cannot create public API temporary directory");
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } cleanup{pattern};
    const fs::path root(pattern);
    const auto write = [&](const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream out(path);
        out << text;
        require(out.good(), "cannot write public API fixture");
    };
    const std::string matrix = "UNITS=MM\nSTEP {\nNAME=pcb\n}\n"
                               "LAYER {\nROW=2\nNAME=Core\nTYPE=DiElEcTrIc\n}\n"
                               "LAYER {\nROW=1\nNAME=Top\nTYPE=SIGNAL\n}\n"
                               "LAYER {\nROW=3\nNAME=Silk\nTYPE=SiLk_ScReEn\n}\n";
    write(root / "matrix/matrix", matrix);
    write(root / "steps/pcb/profile",
          "UNITS=MM\nS P 0\nOB 0 0 I\nOS 1 0\nOS 1 1\nOS 0 1\nOS 0 0\nOE\nSE\n");
    write(root / "steps/pcb/layers/top/features", "UNITS=MM\nT 0 0 standard P 0 0 1 1 0 text\n");
    write(root / "steps/pcb/layers/top/attrlist", ".layer_dielectric=0.035\n");
    auto missing = read_odbpp(root);
    require(!missing && missing.error.find("Core") != std::string::npos,
            "one-argument ODB read must reject missing thickness");
    const double thickness = 0.00028123456789;
    std::vector<ThicknessRecord> records{{"stale", 1, "stale", {}}};
    auto result = read_odbpp(root, {{{"cOrE", thickness}}}, &records);
    require(bool(result), result.error);
    require(records.size() == 2 && records[0].layer == "Top" && records[1].layer == "Core",
            "records must replace caller contents in mapped stack order");
    require(records[0].source == "odb" && records[1].source == "override" &&
                !records[1].odb_value_m,
            "public provenance must omit filename and zero original");
    require(std::bit_cast<std::uint64_t>(records[1].thickness_m) ==
                std::bit_cast<std::uint64_t>(thickness),
            "public override must preserve the selected double bits");
    require(result.model.layers[1].function == "DiElEcTrIc",
            "retained function spelling must survive composition");
    const auto& unsupported = result.model.report.unsupported;
    require(std::any_of(unsupported.begin(),
                        unsupported.end(),
                        [](const auto& entry) {
                            return entry.element.find("unsupported record T") != std::string::npos;
                        }),
            "parser unsupported entries must survive mapping");
    require(std::any_of(
                unsupported.begin(),
                unsupported.end(),
                [](const auto& entry) { return entry.element == "SkippedLayer:Silk:SiLk_ScReEn"; }),
            "mapper skipped entries must survive with original function spelling");
    write(root / "steps/pcb/layers/core/attrlist", ".layer_dielectric=0.2\n");
    constexpr double original = 0.0002;
    const auto same_double = [](double a, double b) {
        return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
    };
    for (const double selected : {thickness, original, std::nextafter(original, 1.0)}) {
        const bool equal = same_double(selected, original);
        auto overridden = read_odbpp(root, {{{"cOrE", selected}}}, &records);
        require(bool(overridden), overridden.error);
        require(records.size() == overridden.model.layers.size() && records.size() == 2,
                "positive overrides must preserve one record per retained layer");
        require(same_double(overridden.model.layers[1].thickness_m, selected) &&
                    same_double(records[1].thickness_m, selected),
                "positive ODB thickness must be replaced by the exact override double");
        require(records[1].layer == "Core" && records[1].source == "override",
                "positive and equal overrides must have override provenance");
        require(records[1].odb_value_m.has_value() == !equal,
                "only differing positive originals are ODB conflicts");
        if (!equal)
            require(same_double(*records[1].odb_value_m, original),
                    "ODB conflict must preserve the original thickness");
        require(records[0].source == "odb" && !records[0].odb_value_m,
                "untouched layer must keep ODB provenance without a conflict");
    }
    // A skipped source TYPE containing ':' cannot be recovered by the report's
    // delimiter-based name lookup. Reporting must preserve the mapper fallback.
    write(root / "matrix/matrix", matrix + "LAYER {\nROW=4\nNAME=Notes\nTYPE=Custom:Document\n}\n");
    auto fallback = read_odbpp(root, {}, &records);
    require(bool(fallback), "missing report function lookup must not reject: " + fallback.error);
    require(std::any_of(fallback.model.report.unsupported.begin(),
                        fallback.model.report.unsupported.end(),
                        [](const auto& entry) {
                            return entry.element == "SkippedLayer:Notes:custom:document";
                        }),
            "missing report lookup must retain the mapper's deterministic function");
    for (const auto value : {0.0,
                             -1.0,
                             std::numeric_limits<double>::infinity(),
                             std::numeric_limits<double>::quiet_NaN()}) {
        auto invalid = read_odbpp(root, {{{"Core", value}}}, &records);
        require(!invalid && records.empty(),
                "invalid public overrides must reject and clear records");
    }
    records.push_back({"stale", 1, "stale", {}});
    auto duplicate = read_odbpp(root, {{{"Core", thickness}, {"core", thickness}}}, &records);
    require(!duplicate && records.empty(), "folded duplicate public overrides must reject");
    records.push_back({"stale", 1, "stale", {}});
    auto absent = read_odbpp(root / "absent", {}, &records);
    require(!absent && records.empty(), "parser failure must clear caller records");
    std::cout << "PASS public_api\n";
}
// Full-board reader composition stays testable even when the CLI must reject
// unsupported slot geometry. Do not reinterpret that C-tier loss as success.
void full_board_reader(const fs::path& root) {
    using namespace chipcae::geometry::ecad;
    auto directory = read_odbpp(root / "kicad4l");
    require(bool(directory), "full-board directory reader: " + directory.error);
    require(directory.model.layers.size() == 7, "full-board reader must retain seven layers");
    require(!directory.model.layers.front().polygons.empty(),
            "retraced copper contours must produce mapped polygons");
    require(std::any_of(directory.model.layers.front().polygons.begin(),
                        directory.model.layers.front().polygons.end(),
                        [](const auto& polygon) {
                            long double twice_area = 0;
                            for (std::size_t i = 0; i < polygon.points.size(); ++i) {
                                const auto& a = polygon.points[i];
                                const auto& b = polygon.points[(i + 1) % polygon.points.size()];
                                twice_area += static_cast<long double>(a[0]) * b[1] -
                                              static_cast<long double>(a[1]) * b[0];
                            }
                            // The fixture has a 6 mm square hole. Preserve its
                            // negative winding and area to one square DBU.
                            return std::abs(twice_area / 2 + 36e6L) <= 1;
                        }),
            "retraced bridge must preserve the 6 mm square copper hole");
    require(std::any_of(directory.model.report.unsupported.begin(),
                        directory.model.report.unsupported.end(),
                        [](const auto& e) { return e.element.starts_with("Slot:"); }),
            "unsupported oval drill must remain observable");
    require(std::none_of(directory.model.report.unsupported.begin(),
                         directory.model.report.unsupported.end(),
                         [](const auto& e) { return e.element.starts_with("Symbol:"); }),
            "standard r apertures must map without symbol loss");
    std::cout << "PASS full_board_reader\n";
}
void cross_models(const fs::path& root, const fs::path& material_map) {
    auto odb = chipcae::geometry::ecad::read_odbpp(root / "kicad4l.tgz");
    auto ipc = chipcae::geometry::ecad::read_ipc2581(root / "kicad4l.ipc2581.xml");
    require(static_cast<bool>(odb), "ODB model import failed: " + odb.error);
    require(static_cast<bool>(ipc), "IPC model import failed: " + ipc.error);
    std::string error;
    for (auto* model : {&odb.model, &ipc.model}) {
        require(chipcae::geometry::ecad::apply_material_map(*model, material_map, &error), error);
    }
    const auto& a = odb.model;
    const auto& b = ipc.model;
    require(!a.layers.empty() && a.layers.size() == b.layers.size(), "Cross-model layer count");
    for (std::size_t i = 0; i < a.layers.size(); ++i) {
        const auto& x = a.layers[i];
        const auto& y = b.layers[i];
        require(x.copper == y.copper && x.material == y.material, "Cross-model copper/material");
        require(std::abs(x.thickness_m - y.thickness_m) <=
                    1e-6 * std::max(std::abs(x.thickness_m), std::abs(y.thickness_m)),
                "Cross-model thickness relative error exceeds 1e-6");
    }
    // Vias are not serialized into stack/polyset: compare the real imported models too.
    require(!a.vias.empty() && a.vias.size() == b.vias.size(), "Cross-model via count");
    auto positions = [](const auto& model) {
        std::vector<std::array<double, 2>> result;
        for (const auto& via : model.vias) {
            result.push_back({via.position[0] * model.dbu_m, via.position[1] * model.dbu_m});
        }
        std::sort(result.begin(), result.end());
        return result;
    };
    const auto x = positions(a), y = positions(b);
    for (std::size_t i = 0; i < x.size(); ++i) {
        require(std::abs(x[i][0] - y[i][0]) <= std::max(a.dbu_m, b.dbu_m) &&
                    std::abs(x[i][1] - y[i][1]) <= std::max(a.dbu_m, b.dbu_m),
                "Cross-model via position differs by more than 1 dbu");
    }
}
void acceptance(const fs::path& executable, const std::string& selector) {
    const auto binary = fs::absolute(executable);
    const auto build = binary.parent_path().parent_path();
    const auto command = "python3 -c " + quote(acceptance_driver) + " " +
                         quote(CHIPCAE_TEST_SOURCE_DIR) + " " + quote(build.string()) + " " +
                         quote(selector) + " " + quote(binary.string());
    const int status = std::system(command.c_str());
    require(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "ODB++ CLI acceptance scenarios failed: " + selector);
}
} // namespace
int main(int argc, char** argv) {
    try {
        const auto root = fs::path(CHIPCAE_TEST_SOURCE_DIR) / "tests/integration/ecad/data/odbpp";
        require(argc >= 2, "Expected odb, ipc, all, or a named acceptance scenario");
        if (std::string(argv[1]) == "cross_models") {
            require(argc == 3, "cross_models requires material map path");
            cross_models(root, argv[2]);
            return 0;
        }
        require(argc == 2, "Expected exactly one acceptance selector");
        const std::string mode = argv[1];
        if (mode == "full_board_reader") {
            full_board_reader(root);
            return 0;
        }
        if (mode == "odb" || mode == "all" || mode == "public_api") {
            public_api();
            full_board_reader(root);
            if (mode == "public_api")
                return 0;
        }
        if (mode == "odb")
            odb_fixture(root);
        else if (mode == "ipc")
            ipc_fixture(root);
        acceptance(argv[0], mode);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
    return 0;
}
