# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""Prony 形状缩放 s 的不变量（tools/vv/prony_scale.py）。"""
import importlib
import math
import sys
from pathlib import Path

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "vv"))
import prony_scale as ps  # noqa: E402

BENCH = ROOT / "cases" / "benchmarks"
PRONY_CARDS = sorted(p for p in BENCH.rglob("*.meta.yaml")
                     if yaml.safe_load(p.read_text(encoding="utf-8")).get("model") == "prony_viscoelastic")
S_VALUES = [1.0, 0.75, 0.5, 0.25, 1e-3]


def _ids(p):
    return str(p.relative_to(BENCH))


def test_cards_found():
    assert len(PRONY_CARDS) >= 4


@pytest.mark.parametrize("path", PRONY_CARDS, ids=_ids)
@pytest.mark.parametrize("s", S_VALUES)
def test_scaled_invariants(path, s):
    card = ps.load_card(path)
    e0, einf, terms = ps.prony_parts(card)
    out = ps.card_for_s(card, s)
    e0s, einfs, terms_s = ps.prony_parts(out)
    assert math.isclose(e0s, e0, rel_tol=1e-12)                       # E0 不变
    assert math.isclose(einfs, e0 - s * (e0 - einf), rel_tol=1e-12)   # E∞(s)
    assert [t for _, t in terms_s] == [t for _, t in terms]           # τ_i 不变
    for (e, _), (es, _) in zip(terms, terms_s):
        assert math.isclose(es, s * e, rel_tol=1e-12)                 # 只缩放 E_i
    assert ps.validate_schema_v1(out) == []
    assert "s" not in out and "p_series_sensitivity" not in out      # s 不是运行时字段


@pytest.mark.parametrize("path", PRONY_CARDS, ids=_ids)
def test_endpoints(path, tmp_path):
    card = ps.load_card(path)
    e0, einf, terms = ps.prony_parts(card)
    ends = ps.endpoint_cards(card)
    assert set(ends) == set(ps.ENDPOINTS)
    # s = 1：与原卡 Prony 数值完全相同
    assert ends["s1"]["viscoelastic"]["tensile"]["infinity"] == pytest.approx(card["viscoelastic"]["tensile"]["infinity"], rel=1e-12)
    assert [t["modulus"] for t in ends["s1"]["viscoelastic"]["tensile"]["terms"]] == pytest.approx(
        [t["modulus"] for t in card["viscoelastic"]["tensile"]["terms"]], rel=1e-15)
    # s = 0：E ≡ E0；E ≡ E∞_ref
    assert ends["s0"]["model"] == "thermo_elastic"
    assert ends["s0"]["elastic"]["table"]["columns"]["E"] == [pytest.approx(e0, rel=1e-15)]
    assert ends["Einf"]["elastic"]["table"]["columns"]["E"] == [pytest.approx(einf, rel=1e-15)]
    assert ps.card_for_s(card, 0.0)["elastic"]["table"]["columns"]["E"] == ends["s0"]["elastic"]["table"]["columns"]["E"]
    for c in ends.values():
        assert ps.validate_schema_v1(c) == []
        # 写出再读回仍是数字（守住 YAML 1.1 指数问题）
        f = tmp_path / f"{c['id']}.meta.yaml"
        ps.dump(c, f)
        back = yaml.safe_load(f.read_text(encoding="utf-8"))
        assert back == c
        assert ps.validate_schema_v1(back) == []
    # constant_poisson_from_E 近似已注明
    assert ps.APPROX_NOTE in ends["s1"]["viscoelastic"]["approximation_notes"]


@pytest.mark.parametrize("bad", [-0.1, 1.1])
def test_s_out_of_range(bad):
    card = ps.load_card(PRONY_CARDS[0])
    with pytest.raises(ValueError):
        ps.card_for_s(card, bad)


def test_package_imports_with_tools_init():
    """main 的 tools/__init__.py 存在时，tools.vv.* 仍可按包导入。"""
    sys.path.insert(0, str(ROOT))
    assert (ROOT / "tools" / "__init__.py").exists()
    for name in ("tools.vv.prony_scale", "tools.vv.derive_cards"):
        assert importlib.import_module(name)


def test_kim2016_rederivation():
    import derive_cards as dc
    hk = dc.kim2016_homogenized()
    u = hk["I25D_UBUMP_UF_KIM2016"]
    assert u["cs"] == 0.03 and u["glassy"]["nu"] == 0.3
    assert hk["I25D_C4_UF_KIM2016"]["status"] == "tbd"               # SnAg E(T) 未核对
    card = ps.load_card(BENCH / "interposer25d-kim2016" / "materials" / "I25D_UBUMP_UF_KIM2016.meta.yaml")
    e0, einf, _ = ps.prony_parts(card)
    assert e0 == pytest.approx(u["glassy"]["E_GPa"] * 1e9, rel=2e-6)
    assert einf == pytest.approx(u["rubbery"]["E_GPa"] * 1e9, rel=1e-12)
    alpha = card["thermal_expansion"]["table"]["columns"]["alpha"]
    assert alpha[0] == pytest.approx(u["glassy"]["alpha_ppm"] * 1e-6, rel=1e-6)
    assert alpha[-1] == pytest.approx(u["rubbery"]["alpha_ppm"] * 1e-6, rel=1e-6)
