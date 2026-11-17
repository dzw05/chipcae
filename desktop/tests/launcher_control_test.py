"""ControlReader supplemental acceptance, independent of machine samples."""
import importlib.util
import json
from pathlib import Path
import tempfile
import warnings

spec = importlib.util.spec_from_file_location('mock_solver', Path(__file__).parents[1] / 'tools/mock_solver.py')
mock = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mock)
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'control.jsonl'
    reader = mock.ControlReader(path)
    assert reader.poll() == []
    def command(seq):
        return json.dumps(dict(cmd_seq=str(seq), client_request_id='test', action='STOP')).encode()
    path.write_bytes(b'{bad}\n' + command(1) + b'\n' + command(1) + b'\n' + command(2))
    with warnings.catch_warnings(record=True) as seen:
        warnings.simplefilter('always')
        assert [c['cmd_seq'] for c in reader.poll()] == [1]
        assert len(seen) == 1
    with path.open('ab') as stream:
        stream.write(b'\n')
    assert [c['cmd_seq'] for c in reader.poll()] == [2]
    with path.open('ab') as stream:
        stream.write(b'{"cmd_seq":"999","action":"BAD"}\n' + command(3) + b'\n')
    with warnings.catch_warnings(record=True) as seen:
        assert [c['cmd_seq'] for c in reader.poll()] == [3]
        assert len(seen) == 1
    assert reader.seq == 3
print('illegal/partial/duplicate control reader checks passed')
