#!/usr/bin/env python3
"""P 系列 Prony 形状缩放系数 s 的灵敏度卡生成器（V&V，加载前离线生成）。

约定（连续介质求解器组 2026-11 核定）：
  - 只缩放 E_i，不缩放 τ_i：E_i(s) = s·E_i,ref。
  - E0 保持不变：E∞(s) = E0 − s·(E0 − E∞_ref)，于是 E∞(s) + Σ E_i(s) = E0 恒成立。
  - s = 1 即原卡（借用形状）；s = 0 即 E ≡ E0（无松弛弹性）。
  - 另报完全松弛端点 E ≡ E∞_ref。
  - 报告只给三个端点：s = 1、s = 0、E ≡ E∞。中间值只作灵敏度扫描，不是默认本构。
  - s 不是运行时字段：本脚本在加载前生成合法的 Schema v1 卡；生成卡里不出现 s 字段（只在 provenance 文字中记录）。
  - 原卡 volumetric_mode = constant_poisson_from_E 为近似（常泊松比换算，K 与 G 只在纯剪下一致），
    生成卡保留并在 approximation_notes 中注明。

用法：
  python tools/vv/prony_scale.py CARD.meta.yaml --out DIR            # 写三个端点卡
  python tools/vv/prony_scale.py CARD.meta.yaml --out DIR --s 0.5    # 另写一个 s = 0.5 的扫描卡（非默认）
"""
from __future__ import annotations

import argparse
import copy
import math
from pathlib import Path

import yaml

# V&V 扩展字段（不是 Schema v1 字段）；生成卡时去掉
VV_EXTENSION_KEYS = ("p_series_sensitivity", "vv_usage", "elastic_homogenized")
APPROX_NOTE = "constant_poisson_from_E 为近似：常泊松比换算体积响应，与 K 弹性主模式只在纯剪下一致"
ENDPOINTS = ("s1", "s0", "Einf")


def load_card(path) -> dict:
    with open(path, encoding="utf-8") as fh:
        return yaml.safe_load(fh)


def prony_parts(card: dict):
    """返回 (E0, E∞_ref, [(E_i, τ_i)])；E0 = E∞ + ΣE_i（由卡面数值算出）。"""
    if card.get("model") != "prony_viscoelastic":
        raise ValueError(f"{card.get('id')}: model 不是 prony_viscoelastic")
    tens = card["viscoelastic"]["tensile"]
    einf = tens["infinity"]
    terms = [(t["modulus"], t["tau"]) for t in tens["terms"]]
    for v in [einf] + [x for t in terms for x in t]:
        if isinstance(v, bool) or not isinstance(v, (int, float)):
            raise TypeError(f"{card.get('id')}: Prony 数值字段不是数字：{v!r}")
    if not terms:
        raise ValueError(f"{card.get('id')}: terms 为空")
    e0 = einf + math.fsum(e for e, _ in terms)
    return e0, einf, terms


def _strip_extensions(card: dict) -> dict:
    out = copy.deepcopy(card)
    for k in VV_EXTENSION_KEYS:
        out.pop(k, None)
    return out


def _note(card: dict, text: str) -> None:
    prov = card.setdefault("provenance", {})
    old = prov.get("notes")
    prov["notes"] = (str(old).strip() + " " if old else "") + text


def scaled_prony_card(card: dict, s: float) -> dict:
    """0 < s ≤ 1：Prony 卡，E_i → s·E_i，τ_i 不变，E∞(s) = E0 − s(E0 − E∞_ref)。"""
    if not (0.0 < s <= 1.0):
        raise ValueError("scaled_prony_card 需要 0 < s ≤ 1；s = 0 用 elastic_card(E0)")
    e0, einf, terms = prony_parts(card)
    out = _strip_extensions(card)
    tens = out["viscoelastic"]["tensile"]
    tens["infinity"] = e0 - s * (e0 - einf)
    tens["terms"] = [{"modulus": s * e, "tau": tau} for e, tau in terms]
    out["viscoelastic"].setdefault("approximation_notes", [])
    if APPROX_NOTE not in out["viscoelastic"]["approximation_notes"]:
        out["viscoelastic"]["approximation_notes"].append(APPROX_NOTE)
    out["id"] = f"{card['id']}__S{('%g' % s).replace('.', 'P')}"
    _note(out, f"[prony_scale] 由 {card['id']} 生成：Prony 形状缩放 s = {s:g}（只缩放 E_i，τ_i 不变，E0 = {e0:.6e} Pa 不变）。"
               + ("" if s == 1.0 else "灵敏度扫描点，不是默认本构。"))
    return out


def elastic_card(card: dict, E: float, tag: str, what: str) -> dict:
    """弹性端点卡（thermo_elastic，单点 E，ν 取原卡 poisson_ratio，热膨胀原样保留）。"""
    visc = card["viscoelastic"]
    nu = visc["poisson_ratio"]
    out = {
        "id": f"{card['id']}__{tag}",
        "schema_version": card.get("schema_version", "1"),
        "unit_system": card.get("unit_system", "SI"),
        "model": "thermo_elastic",
        "elastic": {
            "symmetry": visc.get("symmetry", "isotropic"),
            "table": {
                "temperatures": [visc["reference_temperature"]],
                "columns": {"E": [float(E)], "nu": [nu]},
                "interpolation": "linear",
                "extrapolation": "clamp",
            },
        },
        "thermal_expansion": copy.deepcopy(card["thermal_expansion"]),
        "provenance": copy.deepcopy(card.get("provenance", {})),
    }
    _note(out, f"[prony_scale] 由 {card['id']} 生成的弹性端点：{what}，E = {E:.6e} Pa（温度无关）。")
    return out


def endpoint_cards(card: dict) -> dict:
    """三个报告端点：s1（原卡借用形状）、s0（E ≡ E0）、Einf（E ≡ E∞_ref）。"""
    e0, einf, _ = prony_parts(card)
    return {
        "s1": scaled_prony_card(card, 1.0),
        "s0": elastic_card(card, e0, "S0_E0", "s = 0，无松弛 E ≡ E0"),
        "Einf": elastic_card(card, einf, "EINF", "完全松弛 E ≡ E∞_ref"),
    }


def card_for_s(card: dict, s: float) -> dict:
    if not (0.0 <= s <= 1.0):
        raise ValueError("s 必须在 [0, 1]")
    if s == 0.0:
        return endpoint_cards(card)["s0"]
    return scaled_prony_card(card, s)


def _num(x) -> bool:
    return isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x)


def validate_schema_v1(card: dict) -> list[str]:
    """V&V 侧最小合法性检查（字段名与本目录已有 Schema v1 卡一致；正式规则以规范页为准）。"""
    err = []
    for k in ("id", "schema_version", "unit_system", "model", "thermal_expansion", "provenance"):
        if k not in card:
            err.append(f"缺 {k}")
    if str(card.get("schema_version")) != "1":
        err.append("schema_version 不是 1")
    for k in VV_EXTENSION_KEYS + ("s", "prony_scale_s"):
        if k in card:
            err.append(f"含非 Schema 字段 {k}")
    te = card.get("thermal_expansion", {}).get("table", {})
    temps = te.get("temperatures", [])
    alphas = te.get("columns", {}).get("alpha", [])
    if not temps or len(temps) != len(alphas) or not all(_num(v) for v in temps + alphas):
        err.append("thermal_expansion 表非法")
    model = card.get("model")
    if model == "thermo_elastic":
        tb = card.get("elastic", {}).get("table", {})
        T = tb.get("temperatures", [])
        cols = tb.get("columns", {})
        E, nu = cols.get("E", []), cols.get("nu", [])
        if not T or not (len(T) == len(E) == len(nu)):
            err.append("elastic 表长度不一致")
        if not all(_num(v) for v in T + E + nu):
            err.append("elastic 表含非数字")
        elif any(v <= 0 for v in E) or any(not (-1.0 < v < 0.5) for v in nu):
            err.append("elastic 数值越界")
    elif model == "prony_viscoelastic":
        v = card.get("viscoelastic", {})
        t = v.get("tensile", {})
        terms = t.get("terms", [])
        if not _num(t.get("infinity")) or t.get("infinity") <= 0:
            err.append("infinity 非法")
        if not terms:
            err.append("terms 为空")
        for term in terms:
            if not (_num(term.get("modulus")) and _num(term.get("tau"))) or term["modulus"] <= 0 or term["tau"] <= 0:
                err.append(f"Prony 项非法：{term}")
        if not _num(v.get("poisson_ratio")) or not _num(v.get("reference_temperature")):
            err.append("poisson_ratio / reference_temperature 非法")
        sh = v.get("shift", {})
        if sh.get("type") == "WLF" and not (_num(sh.get("C1")) and _num(sh.get("C2"))):
            err.append("WLF C1/C2 非数字")
    else:
        err.append(f"未知 model {model}")
    return err


def dump(card: dict, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        yaml.safe_dump(card, fh, allow_unicode=True, sort_keys=False)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("card")
    ap.add_argument("--out", required=True)
    ap.add_argument("--s", type=float, action="append", default=[], help="额外扫描点（非默认本构）")
    a = ap.parse_args(argv)
    card = load_card(a.card)
    cards = dict(endpoint_cards(card))
    for s in a.s:
        cards[f"s{s:g}"] = card_for_s(card, s)
    out = Path(a.out)
    for tag, c in cards.items():
        problems = validate_schema_v1(c)
        if problems:
            raise SystemExit(f"{c['id']}: {problems}")
        dump(c, out / f"{c['id']}.meta.yaml")
        print(out / f"{c['id']}.meta.yaml")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
