# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Shared field cases; schema/checker candidates and all data stay temporary."""
from copy import deepcopy
from dataclasses import dataclass

import export_fixtures as shared
from followup_fixtures import FollowupCase, schema_for

exporter = shared.exporter


@dataclass(frozen=True)
class ContractCase:
    name: str
    options: dict
    valid: bool
    emitted: dict
    omitted: tuple


def resolved_rule(schema, rule):
    """Resolve the schema's local JSON pointer without requiring jsonschema."""
    while '$ref' in rule:
        pointer = rule['$ref']
        assert pointer.startswith('#/'), pointer
        rule = schema
        for part in pointer[2:].split('/'):
            rule = rule[part.replace('~1', '/').replace('~0', '~')]
    return rule


def boundary_cases(schema):
    reference_rule = schema['properties']['reference']
    members = reference_rule['properties']['type']['enum']
    minimum_source = reference_rule['properties']['source']['minLength']
    full = dict(reference=dict(value=1.0, type='解析', source='1D'),
                error=dict(value=1e-8, metric='rel'),
                tolerance=dict(value=1e-6), notes=['实测'], src_dirty=False)
    cases = []

    def add(name, changes=None, valid=True, *, absent=(), omitted=()):
        options = deepcopy(full)
        options.update(deepcopy(changes or {}))
        for field in absent:
            options.pop(field)
        emitted = deepcopy(options)
        for field in omitted:
            emitted.pop(field, None)
        cases.append(ContractCase(name, options, valid, emitted,
                                  tuple(absent) + tuple(omitted)))

    add('full-legal-combination')
    for member in members:
        reference = dict(value=1.0, type=member, source='1D')
        add(f'reference.type={member!r}', dict(reference=reference))
        variants = {member.upper(), member.capitalize()} - {member}
        if any('\u4e00' <= char <= '\u9fff' for char in member):
            variants.add(member + ' ')
        for variant in sorted(variants):
            changed = dict(reference, type=variant)
            add(f'reference.type={variant!r}', dict(reference=changed),
                valid=variant in members)

    for field in reference_rule['required']:
        reference = deepcopy(full['reference'])
        del reference[field]
        add(f'reference-missing-{field}', dict(reference=reference), valid=False)
    add('reference-empty-source',
        dict(reference=dict(full['reference'], source='')), valid=False)
    add('reference-minimum-source',
        dict(reference=dict(full['reference'], source='s' * minimum_source)))

    for field in ('error', 'tolerance'):
        rule = resolved_rule(schema, schema['properties'][field])
        for required in rule['required']:
            value = deepcopy(full[field])
            del value[required]
            add(f'{field}-missing-{required}', {field: value}, valid=False)

    add('notes-empty', dict(notes=[]), omitted=('notes',))
    add('notes-none', dict(notes=None), omitted=('notes',))
    add('notes-absent', absent=('notes',))
    add('src_dirty-absent', absent=('src_dirty',))
    for value in (True, '1'):
        add(f'reference.value={value!r}',
            dict(reference=dict(full['reference'], value=value)), valid=False)
    add('error.metric=1', dict(error=dict(full['error'], metric=1)), valid=False)
    for value in ('ok', ['ok', 1]):
        add(f'notes={value!r}', dict(notes=value), valid=False)
    for value in (0, 'true'):
        add(f'src_dirty={value!r}', dict(src_dirty=value), valid=False)

    for field in ('reference', 'error', 'tolerance'):
        add(f'{field}-none', {field: None}, omitted=(field,))
    add('optional-objects-none', dict(reference=None, error=None, tolerance=None),
        omitted=('reference', 'error', 'tolerance'))
    add('optional-fields-absent', absent=tuple(full))
    add('real-policy-exception', dict(data_class='real'))
    return cases


def exporter_candidate(test, summary, case):
    """Compare accepted output independently; overlay rejected inputs verbatim."""
    try:
        obj = exporter.result_json_from_summary(
            summary, **dict(deepcopy(shared.METADATA), **deepcopy(case.options)))
    except exporter.ResultJsonError as exc:
        test.assertEqual(exc.code, 14, case.name)
        test.assertEqual(exc.name, 'INPUT_INVALID', case.name)
        # Do not normalize malformed fields before schema/checker validation.
        return False, test.expected(**deepcopy(case.options))

    test.assertEqual(obj, test.expected(**deepcopy(case.emitted)), case.name)
    for field in case.omitted:
        test.assertNotIn(field, obj, case.name)
    if case.emitted.get('src_dirty') is False:
        test.assertIs(obj['src_dirty'], False, case.name)
    return True, obj
