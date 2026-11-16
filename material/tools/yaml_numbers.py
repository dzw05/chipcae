"""PyYAML loader with YAML 1.2-style scientific float recognition."""
import re
import math

import yaml


class MaterialLoader(yaml.SafeLoader):
    pass


_FLOAT = re.compile(
    r"^(?:[-+]?[0-9]+\.[0-9]*(?:[eE][-+]?[0-9]+)?"
    r"|[-+]?[0-9]+(?:[eE][-+]?[0-9]+)"
    r"|[-+]?\.[0-9]+(?:[eE][-+]?[0-9]+)?)$"
)
MaterialLoader.add_implicit_resolver(
    "tag:yaml.org,2002:float", _FLOAT, list("-+0123456789.")
)
# PyYAML's default integer resolver accepts digit separators. Preserve those
# spellings as strings so number() can reject them like the C++ parser does.
_UNDERSCORE_NUMBER = re.compile(r"^(?=.*_)[+-]?[0-9][0-9.eE+-_]*$")
MaterialLoader.yaml_implicit_resolvers = {
    _key: list(_resolvers) for _key, _resolvers in MaterialLoader.yaml_implicit_resolvers.items()
}
for _first in "-+0123456789":
    MaterialLoader.yaml_implicit_resolvers.setdefault(_first, []).insert(
        0, ("tag:yaml.org,2002:str", _UNDERSCORE_NUMBER)
    )


def load(stream):
    return yaml.load(stream, Loader=MaterialLoader)


def number(value, path):
    if isinstance(value, bool):
        raise ValueError(f"{path}: expected finite number")
    if isinstance(value, (int, float)):
        result = float(value)
        if math.isfinite(result):
            return result
    if isinstance(value, str) and re.fullmatch(
        r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?", value
    ):
        result = float(value)
        if math.isfinite(result):
            return result
    raise ValueError(f"{path}: expected finite number")
