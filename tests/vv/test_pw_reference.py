# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Zhiwei Duan
"""PW 系列半解析参考：固化回归值（来自 tools/vv/pw_reference.py）。"""
import math
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "vv"))
from pw_reference import run, log_aT_single, log_aT_pw  # noqa: E402

# 冻结值（MPa）。来源：基准卡 v0.1，加密 4 倍相对变化 < 4e-6。
FROZEN = {
    "single_WLF": {"sigma_end_cool_MPa": 16.32000, "sigma_end_hold_MPa": 16.24540},
    "piecewise_k2_0.05": {"sigma_end_cool_MPa": 10.44021, "sigma_end_hold_MPa": 9.35481},
}
# 与卡面四位小数对齐的绝对容差；实现重构时不得无说明改动冻结值。
ABS_TOL = 5e-5


@pytest.mark.parametrize("name,fn", [("single_WLF", log_aT_single), ("piecewise_k2_0.05", log_aT_pw)])
def test_pw_frozen_values(name, fn):
    fine = run(fn, 5000, 1600)
    for key, expected in FROZEN[name].items():
        assert math.isfinite(fine[key])
        assert abs(fine[key] - expected) <= ABS_TOL, (name, key, fine[key], expected)
