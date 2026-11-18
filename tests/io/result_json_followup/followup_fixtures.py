# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Schema-derived boundaries and temporary synthetic result fixtures."""
from copy import deepcopy
import json
import os
import re
import subprocess
import sys

# Preserve the shared fixture dependency checks (exit 77 for h5py/numpy).
import export_fixtures as shared

exporter = shared.exporter


class FollowupCase(shared.ExportCase):
    def expected(self, **overrides):
        summary = shared.summarize(self.run)
        obj = dict(schema='chipcae.vv_result/0.2', **shared.METADATA)
        # QoI precedes verdict in the frozen serialization convention.
        verdict = obj.pop('verdict')
        obj['qoi'] = {key: summary[key] for key in shared.QOI_KEYS}
        obj['verdict'] = verdict
        obj.update(overrides)
        return obj


def schema_for(root):
    return json.loads((root / 'tools/vv/schema/vv-result-0.2.schema.json').read_text(
        encoding='utf-8'))


def field_rule(schema, field):
    rule = schema
    for key in field.split('.'):
        rule = rule['properties'][key]
    return rule


def boundary_cases(schema):
    """Read authoritative enums/patterns; change one metadata field per case."""
    for field in ('data_class', 'label', 'verdict', 'reference.type'):
        members = field_rule(schema, field)['enum']
        for member in members:
            variants = [member, member + ' ']
            for variant in (member.upper(), member.capitalize()):
                if variant not in variants:
                    variants.append(variant)
            for value in variants:
                yield field, value, value in members
    patterns = {
        'case_id': ['A', '0', 'RS-RJ_1.0', '', '.hidden', '_leading',
                    'bad id', '中文', 'A\n', 'A '],
        'date': ['20261118', '00000000', '', '2026111', '202611188',
                 '2026-11-18', '２０２６１１１８', '20261118\n'],
        'commit': ['abcdef1', 'abcdef0123456789abcdef0123456789abcdef0123',
                   '0123456', '', 'abcdef', 'a' * 41, 'ABCDEF1', 'ggggggg',
                   'abcdef1\n'],
    }
    for field, values in patterns.items():
        pattern = field_rule(schema, field)['pattern']
        for value in values:
            # JSON Schema pattern uses search semantics, with schema-owned anchors.
            yield field, value, re.search(pattern, value) is not None


def candidate_for(base, field, value):
    candidate = deepcopy(base)
    target = candidate
    parts = field.split('.')
    for key in parts[:-1]:
        target = target[key]
    target[parts[-1]] = value
    return candidate


def exporter_accepts(test, summary, candidate):
    metadata = deepcopy(candidate)
    del metadata['schema']
    del metadata['qoi']
    try:
        exporter.result_json_from_summary(summary, **metadata)
    except exporter.ResultJsonError as exc:
        test.assertEqual(exc.code, 14)
        test.assertEqual(exc.name, 'INPUT_INVALID')
        return False
    return True


def cli_umask(run, target, mask):
    child_env = os.environ.copy()
    child_env.pop('CHIPCAE_GIT_SHA', None)
    return subprocess.run(
        [sys.executable, str(shared.SCRIPT), *shared.cli_args(run), '-o', str(target)],
        cwd=run.parent, env=child_env, umask=mask,
        capture_output=True, timeout=20)
