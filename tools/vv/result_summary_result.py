#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Export a results-directory summary as chipcae.vv_result/0.2 JSON.

QoIs come from the existing reader. The caller supplies the verdict and any
reference, error and tolerance; this tool does not compute or judge them.
"""
import argparse
from datetime import datetime
import json
import math
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import uuid

if __name__ == '__main__':
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.vv.result_summary import ResultSummaryError, summarize

SCHEMA = "chipcae.vv_result/0.2"
_QOI_KEYS = ('step_file', 'mesh_sha256', 'blocks', 'layer_names',
             'material_names', 'temperature', 'warpage', 'manifest_summary')


class ResultJsonError(Exception):
    def __init__(self, code: int, name: str, message: str):
        super().__init__(message)
        self.code = code
        self.name = name


def _require(condition, message):
    if not condition:
        raise ResultJsonError(14, 'INPUT_INVALID', message)


def _pattern(field, value, pattern):
    _require(isinstance(value, str) and re.fullmatch(pattern, value) is not None,
             f'{field}: invalid format')


def _enum(field, value, choices):
    _require(isinstance(value, str) and value in choices,
             f'{field}: expected one of {", ".join(choices)}')


def _number(field, value):
    _require(value is None or (
        isinstance(value, (int, float)) and not isinstance(value, bool)
        and (not isinstance(value, float) or math.isfinite(value))),
        f'{field}: expected a finite number or null')


def _require_finite(value, path=''):
    """Reject nested non-finite floats without converting integers or copying values."""
    if isinstance(value, dict):
        for key, item in value.items():
            _require_finite(item, f'{path}.{key}' if path else key)
    elif isinstance(value, (list, tuple)):
        for index, item in enumerate(value):
            _require_finite(item, f'{path}[{index}]')
    elif isinstance(value, float):
        _require(math.isfinite(value), f'{path}: expected a finite number')


def _value_object(field, value):
    _require(isinstance(value, dict) and 'value' in value,
             f'{field}: expected an object with value')
    _number(f'{field}.value', value['value'])
    if field == 'reference':
        _enum('reference.type', value.get('type'), ('解析', '独立代码', '原型', '回归'))
        _require(isinstance(value.get('source'), str) and bool(value['source']),
                 'reference.source: expected a nonempty string')
    elif 'metric' in value:
        _require(isinstance(value['metric'], str), f'{field}.metric: expected a string')


def qoi_from_summary(summary: dict) -> dict:
    """Copy exactly the eight frozen QoI fields, retaining values and nulls."""
    _require(isinstance(summary, dict), 'summary: expected an object')
    qoi = {}
    for key in _QOI_KEYS:
        _require(key in summary, f'summary.{key}: required field missing')
        qoi[key] = summary[key]
    return qoi


def result_json_from_summary(
    summary: dict,
    *,
    case_id: str,
    data_class: str,
    label: str,
    date: str,
    commit: str,
    verdict: str,
    reference: dict | None = None,
    error: dict | None = None,
    tolerance: dict | None = None,
    notes: list[str] | None = None,
    src_dirty: bool | None = None,
) -> dict:
    """Validate caller metadata and recursively reject non-finite values.

    Top-level field order is a frozen serialization stability guarantee,
    checked byte-for-byte by tests. JSON object member order has no semantic
    meaning. Changing this guarantee requires a schema version increase.
    For the CLI's default date, 本工具使用进程本地日历日.
    """
    _pattern('case_id', case_id, r'[A-Za-z0-9][A-Za-z0-9_.-]*')
    _enum('data_class', data_class, ('synthetic', 'public', 'real'))
    _enum('label', label, ('实测', '参考值', '计划值'))
    _pattern('date', date, r'[0-9]{8}')
    _pattern('commit', commit, r'[0-9a-f]{7,40}')
    _enum('verdict', verdict, ('pass', 'fail', '需讨论', 'skip'))
    for field, value in (('reference', reference), ('error', error),
                         ('tolerance', tolerance)):
        if value is not None:
            _value_object(field, value)
    _require(notes is None or (isinstance(notes, list)
                              and all(isinstance(note, str) for note in notes)),
             'notes: expected a list of strings')
    _require(src_dirty is None or isinstance(src_dirty, bool),
             'src_dirty: expected a bool')
    obj = dict(schema=SCHEMA, case_id=case_id, data_class=data_class, label=label,
               date=date, commit=commit, qoi=qoi_from_summary(summary), verdict=verdict)
    for field, value in (('reference', reference), ('error', error),
                         ('tolerance', tolerance), ('notes', notes),
                         ('src_dirty', src_dirty)):
        if value is not None and (field != 'notes' or value):
            obj[field] = value
    _require_finite(obj)
    return obj


def _serialize(obj):
    try:
        return json.dumps(obj, indent=2, sort_keys=False,
                          ensure_ascii=False, allow_nan=False) + '\n'
    except (ValueError, TypeError, OverflowError, RecursionError) as exc:
        raise ResultJsonError(14, 'INPUT_INVALID', f'JSON serialization: {exc}') from exc


def write_result_json(path: Path, obj: dict) -> None:
    """Atomically replace path using a closed temporary file in its parent.

    For an existing target, preserve stat.S_IMODE(os.stat(path).st_mode).
    For a new target, use mode 0o666 & ~umask, applied by the kernel through
    os.open. This function does not modify the process umask.
    A TOCTOU window exists between os.stat and os.replace. os.stat follows
    symlinks; os.replace replaces the symlink itself with a regular file.
    """
    content = _serialize(obj)
    temporary = None
    try:
        path = Path(path)
        try:
            mode = stat.S_IMODE(os.stat(path).st_mode)
        except FileNotFoundError:
            mode = None
        for _ in range(100):
            candidate = path.parent / f'.result-json-{uuid.uuid4().hex}.tmp'
            try:
                fd = os.open(candidate, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o666)
            except FileExistsError:
                continue
            temporary = candidate
            break
        else:
            raise OSError('could not create a unique result JSON temporary file')
        try:
            stream = os.fdopen(fd, 'w', encoding='utf-8', newline='\n')
        except BaseException:
            os.close(fd)
            raise
        with stream:
            stream.write(content)
        if mode is not None:
            os.chmod(temporary, mode)
        os.replace(temporary, path)
        temporary = None
    except (OSError, ValueError, TypeError) as exc:
        raise ResultJsonError(14, 'INPUT_INVALID', f'output {path}: {exc}') from exc
    finally:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError as exc:
                raise ResultJsonError(14, 'INPUT_INVALID',
                                      f'output temporary cleanup: {exc}') from exc


class _ArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        raise ResultJsonError(14, 'INPUT_INVALID', message)


def _step(value):
    if value == 'last':
        return value
    _require(re.fullmatch(r'[0-9]+', value) is not None,
             'step: expected a nonnegative decimal integer or "last"')
    try:
        return int(value)
    except ValueError as exc:
        raise ResultJsonError(14, 'INPUT_INVALID', f'step: {exc}') from exc


def _default_commit(run_dir):
    """Return (commit, used_placeholder), capturing all Git diagnostics."""
    if 'CHIPCAE_GIT_SHA' in os.environ:
        return os.environ['CHIPCAE_GIT_SHA'], False
    commands = (['git', '-C', run_dir.resolve().anchor, 'rev-parse', 'HEAD'],
                ['git', 'rev-parse', 'HEAD'])
    for command in commands:
        try:
            reply = subprocess.run(command, capture_output=True, text=True, check=False)
        except OSError:
            continue
        if reply.returncode == 0:
            return reply.stdout.strip(), False
    return '0000000', True


def _cli_value_object(args, field):
    value = getattr(args, field + '_value')
    if field == 'reference':
        kind, source = args.reference_type, args.reference_source
        if value is None and kind is None and source is None:
            return None
        _require(value is not None and kind is not None and source is not None,
                 'reference: value, type and source must be supplied together')
        return dict(value=value, type=kind, source=source)
    metric = getattr(args, field + '_metric')
    _require(value is not None or metric is None, f'{field}.metric: requires value')
    if value is None:
        return None
    obj = dict(value=value)
    if metric is not None:
        obj['metric'] = metric
    return obj


def main(argv: list[str] | None = None) -> int:
    parser = _ArgumentParser(description=__doc__)
    parser.add_argument('run_dir', type=Path)
    parser.add_argument('-o', type=Path)
    for field in ('case-id', 'data-class', 'label', 'verdict'):
        parser.add_argument('--' + field, required=True)
    parser.add_argument('--date')
    parser.add_argument('--commit')
    parser.add_argument('--step', type=_step,
                        help='select a nonnegative step n or "last" (default: first step)')
    parser.add_argument('--top-rel-tol', type=float, default=1e-12)
    parser.add_argument('--reference-value', type=float)
    parser.add_argument('--reference-type')
    parser.add_argument('--reference-source')
    for field in ('error', 'tolerance'):
        parser.add_argument('--' + field + '-value', type=float)
        parser.add_argument('--' + field + '-metric')
    parser.add_argument('--note', action='append', default=[])
    parser.add_argument('--src-dirty', action='store_true', default=None)
    try:
        args = parser.parse_args(argv)
        commit = args.commit
        if commit is None:
            commit, placeholder = _default_commit(args.run_dir)
            if placeholder:
                args.note.append('commit defaulted to 0000000')
        options = {field: _cli_value_object(args, field)
                   for field in ('reference', 'error', 'tolerance')}
        summary = summarize(args.run_dir, top_rel_tol=args.top_rel_tol, step=args.step)
        obj = result_json_from_summary(
            summary, case_id=args.case_id, data_class=args.data_class, label=args.label,
            date=args.date if args.date is not None else datetime.now().strftime('%Y%m%d'),
            commit=commit, verdict=args.verdict, notes=args.note,
            src_dirty=args.src_dirty, **options)
        if args.o is None:
            sys.stdout.write(_serialize(obj))
        else:
            write_result_json(args.o, obj)
        if args.data_class == 'real':
            print('result_summary_result: WARN: data_class=real must stay local (D03)',
                  file=sys.stderr)
    except (ResultJsonError, ResultSummaryError, OSError, ValueError, TypeError) as exc:
        protocol_error = isinstance(exc, (ResultJsonError, ResultSummaryError))
        name = exc.name if protocol_error else 'INPUT_INVALID'
        reason = ' '.join(str(exc).splitlines())
        print(f'result_summary_result: {name}: {reason}', file=sys.stderr)
        return exc.code if protocol_error else 14
    return 0


if __name__ == '__main__':
    sys.exit(main())
