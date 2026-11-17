"""Strict reader and signed areas for the CCPOLY01 sidecar format."""
from dataclasses import dataclass
import math
from pathlib import Path
import struct


class StackPrepError(Exception):
    def __init__(self, code: int, name: str, message: str):
        super().__init__(message)
        self.code = code
        self.name = name


@dataclass(frozen=True)
class PolysetLayer:
    name: str
    rings: tuple[tuple[tuple[int, int], ...], ...]


@dataclass(frozen=True)
class Polyset:
    dbu_m: float
    layers: tuple[PolysetLayer, ...]


def read_polyset(path: Path) -> Polyset:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise StackPrepError(12, 'INPUT_REFERENCE_MISSING', str(exc)) from exc
    position = 0

    def take(size):
        nonlocal position
        if size < 0 or size > len(data) - position:
            raise ValueError('truncated polyset')
        value = data[position:position + size]
        position += size
        return value

    def unpack(fmt):
        return struct.unpack(fmt, take(struct.calcsize(fmt)))

    try:
        if take(8) != b'CCPOLY01':
            raise ValueError('invalid polyset magic')
        dbu, count = unpack('<dI')
        if not math.isfinite(dbu) or dbu <= 0:
            raise ValueError('dbu_m must be finite and positive')
        layers = []
        names = set()
        for _ in range(count):
            name = take(unpack('<I')[0]).decode('utf-8')
            if name in names:
                raise ValueError('duplicate polyset layer name')
            names.add(name)
            ring_count = unpack('<I')[0]
            offsets = unpack('<' + str(ring_count + 1) + 'q')
            if offsets[0] != 0 or any(b - a < 3 for a, b in zip(offsets, offsets[1:])):
                raise ValueError('invalid ring offsets or short ring')
            coordinates = take(offsets[-1] * 16)
            points = tuple(struct.iter_unpack('<qq', coordinates))
            rings = tuple(points[a:b] for a, b in zip(offsets, offsets[1:]))
            layers.append(PolysetLayer(name, rings))
        if position != len(data):
            raise ValueError('trailing polyset data')
        return Polyset(dbu, tuple(layers))
    except (ValueError, struct.error, OverflowError) as exc:
        raise StackPrepError(14, 'INPUT_INVALID', str(exc)) from exc


def ring_area_m2(ring: tuple[tuple[int, int], ...], dbu_m: float) -> float:
    twice = sum(x * v - u * y for (x, y), (u, v) in zip(ring, ring[1:] + ring[:1]))
    return twice * 0.5 * dbu_m * dbu_m


def layer_area_m2(layer: PolysetLayer, dbu_m: float) -> float:
    return sum(ring_area_m2(ring, dbu_m) for ring in layer.rings)
