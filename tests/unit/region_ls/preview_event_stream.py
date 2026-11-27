"""Strict reader for preview's dedicated, rank-zero JSON event channel."""

import json
from pathlib import Path


class EventStreamError(AssertionError):
    """An event file or its serial stdout mirror violates the stream contract."""


def read_events(path, *, object_pairs_hook=None) -> list:
    """Read complete JSON objects separated by LF, with no recovery or filtering."""
    path = Path(path)
    try:
        raw = path.read_bytes()
    except OSError as error:
        raise EventStreamError(f'{path}: {error}\nevents file: <unreadable>') from error
    try:
        contents = raw.decode('utf-8')
    except UnicodeDecodeError as error:
        raise EventStreamError(f'{path}: {error}\nevents file bytes:\n{raw!r}') from error
    try:
        if not contents or not contents.endswith('\n'):
            raise ValueError('expected nonempty LF-terminated events')
        events = []
        for line in contents[:-1].split('\n'):
            if not line.strip():
                raise ValueError('blank event line')
            # Check the original type before a hook can turn objects into lists.
            if not isinstance(json.loads(line), dict):
                raise ValueError('event must be a JSON object')
            events.append(json.loads(line, object_pairs_hook=object_pairs_hook))
        return events
    except (ValueError, TypeError) as error:
        raise EventStreamError(f'{path}: {error}\nevents file:\n{contents}') from error


def check_serial_stdout(stdout: str, path) -> None:
    """Require stdout from a direct invocation to mirror the exact file bytes."""
    path = Path(path)
    raw = path.read_bytes()
    if stdout.encode('utf-8') != raw:
        raise EventStreamError(f'{path}: serial stdout differs from events file'
                               f'\nevents file:\n{raw.decode("utf-8", errors="backslashreplace")}'
                               f'\nstdout:\n{stdout}')


def stdout_mixed(stdout: str, path) -> bool:
    """Report launcher mixing, independently of strict event-file acceptance."""
    return stdout.encode('utf-8') != Path(path).read_bytes()
