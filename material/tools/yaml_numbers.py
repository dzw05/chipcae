"""PyYAML loader with YAML 1.2-style scientific float recognition."""
import re
import math

import yaml


class MaterialLoader(yaml.SafeLoader):
    pass


_FLOAT = re.compile(
    r"^(?:[-+]?(?:[0-9][0-9_]*)\.[0-9_]*(?:[eE][-+]?[0-9]+)?"
    r"|[-+]?(?:[0-9][0-9_]*)(?:[eE][-+]?[0-9]+)"
    r"|[-+]?\.[0-9_]+(?:[eE][-+]?[0-9]+)?|[-+]?\.[0-9_]+)$"
)
MaterialLoader.add_implicit_resolver(
    "tag:yaml.org,2002:float", _FLOAT, list("-+0123456789.")
)


def load(stream):
    return yaml.load(stream, Loader=MaterialLoader)


def number(value, path):
    if isinstance(value, bool):
        raise ValueError(f"{path}: expected finite number")
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str) and re.fullmatch(
        r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?", value
    ):
        result = float(value)
        if math.isfinite(result):
            return result
    raise ValueError(f"{path}: expected finite number")
