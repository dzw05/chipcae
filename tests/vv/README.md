# tests/vv

- `test_pw_reference.py`：冻结 `tools/vv/pw_reference.py` 中 `run(..., 5000, 1600)` 的参考值：16.32000 / 16.24540 与 10.44021 / 9.35481 MPa（绝对容差 5e-5）。
- `test_yaml_numeric.py`：`yaml.safe_load` `cases/benchmarks` 下全部 YAML；任何字符串不得「像数字」（YAML 1.1 把 `3.10e10` 读成字符串，卡面须写 `3.10e+10`），材料卡数值字段（模量、α、ν、τ、C1/C2、温度、infinity、terms）必须是 int/float（允许占位 `tbd`）。
- `test_prony_scale.py`：`tools/vv/prony_scale.py` 的不变量——只缩放 E_i、τ_i 不变、E0 不变、E∞(s) = E0 − s(E0 − E∞_ref)、s = 0 ⇔ E ≡ E0、三端点卡合法且不含 s 字段；`I25D_UBUMP_UF_KIM2016` 与 `derive_cards.kim2016_homogenized` 一致；`tools.vv.*` 可按包导入。

运行：`python -m pytest -q tests/vv`（需要 pytest、pyyaml）。尚未并入仓库 CI。

## 许可

`tools/vv/` 与 `tests/vv/` 按 Apache-2.0 发布（见各目录 LICENSE）。
