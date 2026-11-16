#!/usr/bin/env python3
"""Check append-only error identities using the registry's YAML subset (stdlib only)."""
import argparse
import os
from pathlib import Path
import re
import subprocess


def parse(text):
    version = 0  # The initial registry predates schema_version.
    entries = []
    current = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        match = re.fullmatch(r'schema_version:\s*(\d+)\s*(?:#.*)?', line)
        if match:
            version = int(match[1])
        elif line.startswith('- '):
            current = {}
            entries.append(current)
            body = line[2:].strip()
            if body.startswith('{') and body.endswith('}'):
                parts = re.split(r",(?=(?:[^\"']|\"[^\"]*\"|'[^']*')*$)", body[1:-1])
            else:
                parts = [body]
            for part in parts:
                key, sep, value = part.partition(':')
                if not sep:
                    raise ValueError('Invalid registry mapping')
                current[key.strip()] = value.strip().strip('\"\'')
        elif current is not None and ':' in line:
            key, value = line.split(':', 1)
            current[key.strip()] = value.strip().split(' #', 1)[0].strip('\"\'')
        elif line not in ('errors:', 'errors: []'):
            raise ValueError(f'Unsupported registry syntax: {line}')
    identities = [(int(e['code']), e['name']) for e in entries]
    if len({c for c, _ in identities}) != len(identities) or len({n for _, n in identities}) != len(identities):
        raise ValueError('Duplicate error code or name')
    return version, identities


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', nargs='?', default='.')
    parser.add_argument('--base')
    args = parser.parse_args()
    root = Path(args.root).resolve()
    refs = []
    if args.base:
        refs.append(args.base)
    for key in ('CHIPCAE_PROTOCOL_BASE', 'GITHUB_BASE_SHA', 'GITHUB_BASE_REF'):
        value = os.environ.get(key)
        if value:
            refs.append(value)
            if key == 'GITHUB_BASE_REF' and not value.startswith('origin/'):
                refs.append(f'origin/{value}')
    refs.extend(['origin/main', 'main'])
    # Preserve order while dropping duplicates.
    refs = list(dict.fromkeys(ref for ref in refs if ref))
    baseline = None
    for ref in refs:
        result = subprocess.run(['git', 'show', f'{ref}:protocol/errors.yaml'], cwd=root, capture_output=True, text=True)
        if result.returncode == 0:
            baseline = result.stdout
            break
    if baseline is None:
        print('Warning: protocol baseline unavailable; skipping compatibility check.')
        return 0
    try:
        old_version, old = parse(baseline)
        version, new = parse((root / 'protocol/errors.yaml').read_text())
        if version < old_version:
            raise ValueError('schema_version must not be downgraded')
        if new[:len(old)] != old:
            raise ValueError('Published code/name entries must be preserved in order; append only')
        maximum = max((code for code, _ in old), default=0)
        for code, _ in new[len(old):]:
            if code <= maximum:
                raise ValueError('Appended codes must increase strictly above the previous maximum')
            maximum = code
    except (ValueError, KeyError) as error:
        print(f'Protocol compatibility failed: {error}')
        return 1
    print('Protocol compatibility check passed.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
