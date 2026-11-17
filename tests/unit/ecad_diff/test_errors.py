"""API codes and CLI diagnostics must agree and never leave a report."""
import copy
from pathlib import Path
import subprocess
import sys
import tempfile
from test_identical import ROOT, EcadDiffError, board, diff_stacks, polyset, save, write_report


def main():
    with tempfile.TemporaryDirectory() as tmp:
        directory = Path(tmp)
        good = save(directory / 'good.json', board())
        for case, code, name in (
                ('missing_stack', 12, 'INPUT_REFERENCE_MISSING'),
                ('missing_polyset', 12, 'INPUT_REFERENCE_MISSING'),
                ('hash', 10, 'INPUT_HASH_MISMATCH'),
                ('trailing', 14, 'INPUT_INVALID'),
                ('delta', 14, 'INPUT_INVALID'),
                ('unit', 14, 'INPUT_INVALID'),
                ('output_file', 14, 'INPUT_INVALID')):
            folder = directory / case
            folder.mkdir()
            b = copy.deepcopy(board())
            path = folder / 'stack.json'
            out = folder / 'reports'
            delta = 0 if case == 'delta' else 1e-9
            if case in ('missing_polyset', 'hash', 'trailing'):
                p = polyset(folder, b, trailing=b'x' if case == 'trailing' else b'')
                if case == 'missing_polyset':
                    p.unlink()
                if case == 'hash':
                    b['external_refs'][0]['sha256'] = '0' * 64
            if case == 'unit':
                b['unit'] = 'mm'
            if case != 'missing_stack':
                save(path, b)
            if case == 'output_file':
                out.write_text('existing file', encoding='utf-8')
            try:
                result = diff_stacks(good, path, delta)
                write_report(result, out)
            except EcadDiffError as exc:
                assert exc.code == code and exc.name == name, (case, exc)
            else:
                raise AssertionError(f'{case}: API accepted invalid input')
            command = [sys.executable, str(ROOT / 'tools/vv/ecad_diff.py'),
                       '--odb', str(good), '--ipc', str(path), '-o', str(out),
                       '--delta-m', str(delta)]
            proc = subprocess.run(command, capture_output=True, text=True)
            assert proc.returncode == code, (case, proc.stderr)
            assert len(proc.stderr.splitlines()) == 1, proc.stderr
            assert proc.stderr.startswith(f'ecad_diff: {name}: '), proc.stderr
            assert not (out / 'diff.json').exists()
            assert not (out / 'diff.md').exists()


if __name__ == '__main__':
    main()
