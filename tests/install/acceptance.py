"""Black-box X2 acceptance. No install rules or runtime implementation live here."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(argv, cwd, env):
    result = subprocess.run([str(x) for x in argv], cwd=cwd, env=env,
                            text=True, capture_output=True, timeout=180)
    print(f"$ {' '.join(str(x) for x in argv)}\n{result.stdout}{result.stderr}", flush=True)
    return result


def success(argv, cwd, env):
    result = run(argv, cwd, env)
    assert result.returncode == 0, f"command exited {result.returncode}: {argv}"
    return result


@contextmanager
def hidden_schemas(source):
    """Hide the paths baked into today's binaries, even when running as root.

    Keep CTest's build tree and the helper readable. RUN_SERIAL prevents another
    CTest test from observing this temporary change. Restore on assertion/error.
    """
    moved = []
    try:
        for original in (source / 'schema', source / 'material/schema'):
            hidden = original.with_name(original.name + '.x2-install-hidden')
            assert not hidden.exists(), f"stale hidden schema directory: {hidden}"
            original.rename(hidden)
            moved.append((original, hidden))
        yield
    finally:
        for original, hidden in reversed(moved):
            hidden.rename(original)


def layout(prefix, source):
    for name in ('chipcae-solve', 'chipcae-preview', 'chipcae-import'):
        path = prefix / 'bin' / name
        assert path.is_file() and os.access(path, os.X_OK), f"missing installed executable: {path}"
    data = prefix / 'share/chipcae'
    for directory in ('schema', 'material/schema'):
        for schema in (source / directory).glob('*.schema.json'):
            target = data / 'schema' / schema.name
            assert target.is_file(), f"missing installed schema: {target}"
    for name in ('fcbga_demo.case.json', 'fcbga_demo.stack.json',
                 'fcbga_demo.materials.json', 'local.run-profile.json'):
        assert (data / 'examples' / name).is_file(), f"missing installed example: {name}"
    assert (data / 'protocol/errors.yaml').is_file(), 'missing protocol/errors.yaml'
    for name in ('LICENSE', 'THIRD_PARTY_NOTICES'):
        assert (prefix / 'share/doc/chipcae' / name).is_file(), f"missing license: {name}"


def preview(prefix, work, env):
    result = success([prefix / 'bin/chipcae-preview',
                      prefix / 'share/chipcae/examples/fcbga_demo.stack.json',
                      '--max-ratio', '10', '-o', work / 'preview'], work, env)
    events = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    reports = [event for event in events if event.get('stage') == 'success']
    assert reports, 'preview emitted no success JSON event'
    assert all(key in reports[-1] for key in ('nx0', 'ny0', 'L')), reports[-1]


def smoke(prefix, work, env, mpi, serial_profile):
    success([prefix / 'bin/chipcae-solve', '--version'], work, env)
    preview(prefix, work, env)
    success([prefix / 'bin/chipcae-import', '--help'], work, env)
    examples = prefix / 'share/chipcae/examples'
    ranks = json.loads((examples / 'local.run-profile.json').read_text())['ranks']
    profile = examples / 'local.run-profile.json'
    if not mpi or 'NOTFOUND' in mpi:
        # Reuse the existing serial fixture; the distributed local profile needs
        # four MPI processes and would otherwise fail input validation first.
        profile = serial_profile
    command = [prefix / 'bin/chipcae-solve', '--case', examples / 'fcbga_demo.case.json',
               '--profile', profile, '--out', work / 'solve']
    if mpi and 'NOTFOUND' not in mpi:
        command = [mpi, '-n', str(ranks)] + command
    result = run(command, work, env)
    # Only the published NOT_IMPLEMENTED code plus the specific backend diagnostic
    # may skip: malformed inputs, schema lookup and MPI failures must remain red.
    if result.returncode == 2 and 'numerical backend unavailable:' in result.stderr:
        print('SKIP: numerical backend unavailable (mapped NOT_IMPLEMENTED to 77)')
        return 77
    assert result.returncode == 0, f"installed solve exited {result.returncode}"
    assert (work / 'solve/manifest.json').is_file(), 'solve did not write manifest.json'
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=['install_smoke', 'install_relocate', 'cpack_tgz'])
    for key in ('build', 'source', 'version', 'os', 'arch', 'mpi'):
        parser.add_argument('--' + key, required=True)
    args = parser.parse_args()
    source, build = Path(args.source), Path(args.build)
    env = os.environ.copy()
    env.pop('CHIPCAE_DATA_DIR', None)
    with tempfile.TemporaryDirectory(prefix='chipcae-x2-') as temporary:
        work = Path(temporary)
        prefix = work / 'prefix'
        if args.mode == 'cpack_tgz':
            success(['cpack', '-G', 'TGZ', '--config', build / 'CPackConfig.cmake',
                     '-B', work / 'packages'], build, env)
            packages = list((work / 'packages').glob('*.tar.gz'))
            assert len(packages) == 1, f"expected one TGZ, got {packages}"
            expected = f'chipcae-{args.version}-{args.os.lower()}-{args.arch.lower()}.tar.gz'
            assert packages[0].name == expected, f"expected package name {expected}"
            prefix.mkdir()
            success(['cmake', '-E', 'tar', 'xzf', packages[0]], prefix, env)
            roots = list(prefix.iterdir())
            if not (prefix / 'bin').exists() and len(roots) == 1:
                prefix = roots[0]
        else:
            success(['cmake', '--install', build, '--prefix', prefix], work, env)
        layout(prefix, source)
        if args.mode == 'install_relocate':
            relocated = work / 'relocated/prefix'
            relocated.parent.mkdir()
            shutil.move(str(prefix), relocated)
            assert not prefix.exists(), 'original prefix still exists'
            prefix = relocated
        serial_profile = work / 'serial.run-profile.json'
        shutil.copyfile(source / 'examples/s2_solve/profile.json', serial_profile)
        with hidden_schemas(source):
            if args.mode == 'install_relocate':
                preview(prefix, work, env)
                return 0
            return smoke(prefix, work, env, args.mpi, serial_profile)


if __name__ == '__main__':
    raise SystemExit(main())
