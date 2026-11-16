import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from yaml_numbers import load, number


values = [load("value: " + text)["value"] for text in ("3.10e10", "3.10e+10", "3.1E10")]
values = [number(value, "material.density") for value in values]
assert values[0] == values[1] == values[2] == 3.10e10
try:
    number(load("value: 3.1e10x")["value"], "material.density")
except ValueError as error:
    assert "material.density" in str(error)
else:
    raise AssertionError("trailing junk accepted")
assert math.isfinite(values[0])

for text in (".inf", ".nan", "+.inf", "-.inf", "nan", "inf", "1_000", "3.10e_10", "3.1e10x"):
    try:
        number(load("value: " + text)["value"], "material.density")
    except ValueError as error:
        assert "material.density" in str(error), (text, error)
    else:
        raise AssertionError(f"invalid number accepted: {text}")

for value in (float("inf"), float("-inf"), float("nan")):
    try:
        number(value, "material.density")
    except ValueError as error:
        assert "material.density" in str(error)
    else:
        raise AssertionError(f"non-finite typed number accepted: {value}")
