"""YAML 数值字段类型检查。

YAML 1.1（PyYAML）把不带符号的指数（如 3.10e10、2.430e8）读成字符串；卡面必须写成 3.10e+10。
本测试 yaml.safe_load cases/benchmarks 下全部 *.yaml，检查：
  1) 任何字符串标量都不能「看起来像数字」（捕捉上述写法）；
  2) 材料卡里的数值字段（模量、α、ν、τ、C1/C2、温度、infinity、terms 等）必须是 int/float（允许显式占位 "tbd"）。
"""
import math
import re
from pathlib import Path

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[2]
BENCH = ROOT / "cases" / "benchmarks"
YAML_FILES = sorted(BENCH.rglob("*.yaml"))
CARD_FILES = sorted(BENCH.rglob("*.meta.yaml"))

NUMBER_LIKE = re.compile(r"^[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$")
NUMERIC_KEYS = {
    "E", "nu", "alpha", "temperatures", "modulus", "tau", "infinity", "C1", "C2",
    "T_g", "stress_free_temperature", "reference_temperature", "poisson_ratio", "density", "alpha_z",
}
PLACEHOLDERS = {"tbd"}
# 有意写成字符串的版本号字段（加引号）；单位说明子树不是数值
STRING_KEYS = {"schema_version", "format_version", "version", "date", "id", "legacy_id"}
UNIT_SUBTREES = {"original_units"}


def _walk(node, path=()):
    if isinstance(node, dict):
        for k, v in node.items():
            yield from _walk(v, path + (str(k),))
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from _walk(v, path + (f"[{i}]",))
    else:
        yield path, node


def _key(path):
    for p in reversed(path):
        if not p.startswith("["):
            return p
    return ""


def _is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)


def test_files_found():
    assert len(YAML_FILES) >= 4 and len(CARD_FILES) >= 4


@pytest.mark.parametrize("path", YAML_FILES, ids=lambda p: str(p.relative_to(BENCH)))
def test_no_number_like_strings(path):
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    bad = [("/".join(p), v) for p, v in _walk(data) if isinstance(v, str) and NUMBER_LIKE.match(v.strip()) and _key(p) not in STRING_KEYS]
    assert not bad, f"{path}: 数值被读成字符串（指数须带符号，如 3.10e+10）：{bad}"


@pytest.mark.parametrize("path", CARD_FILES, ids=lambda p: str(p.relative_to(BENCH)))
def test_card_numeric_fields(path):
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    bad = []
    for p, v in _walk(data):
        if UNIT_SUBTREES & set(p):
            continue
        if _key(p) in NUMERIC_KEYS and not _is_num(v) and v not in PLACEHOLDERS:
            bad.append(("/".join(p), v))
    assert not bad, f"{path}: 数值字段不是 int/float：{bad}"
    visc = data.get("viscoelastic")
    if visc:
        tens = visc["tensile"]
        assert _is_num(tens["infinity"])
        assert isinstance(tens["terms"], list) and tens["terms"], "terms 必须是非空列表"
        for term in tens["terms"]:
            assert _is_num(term["modulus"]) and _is_num(term["tau"]), term
        if visc.get("shift", {}).get("type") == "WLF":
            assert _is_num(visc["shift"]["C1"]) and _is_num(visc["shift"]["C2"])


def test_signed_exponent_regression():
    """守住本次修复：PyYAML 对无符号指数确实返回 str。"""
    assert isinstance(yaml.safe_load("x: 3.10e10")["x"], str)
    assert isinstance(yaml.safe_load("x: 3.10e+10")["x"], float)
