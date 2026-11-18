# tests/vv

- `test_pw_reference.py`：冻结 `tools/vv/pw_reference.py` 中 `run(..., 5000, 1600)` 的参考值：16.32000 / 16.24540 与 10.44021 / 9.35481 MPa（绝对容差 5e-5）。
- `test_yaml_numeric.py`：`yaml.safe_load` `cases/benchmarks` 下全部 YAML；任何字符串不得「像数字」（YAML 1.1 把 `3.10e10` 读成字符串，卡面须写 `3.10e+10`），材料卡数值字段（模量、α、ν、τ、C1/C2、温度、infinity、terms）必须是 int/float（允许占位 `tbd`）。
- `test_prony_scale.py`：`tools/vv/prony_scale.py` 的不变量——只缩放 E_i、τ_i 不变、E0 不变、E∞(s) = E0 − s(E0 − E∞_ref)、s = 0 ⇔ E ≡ E0、三端点卡合法且不含 s 字段；`I25D_UBUMP_UF_KIM2016` 与 `derive_cards.kim2016_homogenized` 一致；`tools.vv.*` 可按包导入。

运行上述检查可用 `python -m pytest -q tests/vv`（需要 pytest、pyyaml）。上述检查尚未并入仓库 CI。

## V&V 结果与 D03 入库检查

`tools/vv/schema/vv-result-0.2.schema.json` 定义 `chipcae.vv_result/0.2` 的结果结构。
按 D03，公开仓库仅接收 `data_class` 为 `synthetic` 或 `public` 的结果。
`real` 是合法的 schema 分类，真实案例结果必须留在本地。

```sh
python3 -m tools.vv.check_result_data_class
python3 -m tools.vv.check_result_data_class --repo . tests/vv/data/result_schema/valid_synthetic.json
python3 -m unittest tests.vv.test_result_schema -v
ctest --test-dir build -R '^vv_result_schema$' --output-on-failure
```

不传文件时，检查器从 Git 索引中选出文件名恰为 `result.json` 的路径，读取工作树中的文件。
空扫描输出 `0 result.json checked`。显式指定的文件可以使用其他文件名，相对路径以仓库根目录为准。
默认通过 Git 确定仓库根目录，也可用 `--repo` 指定。
解析符号链接和 `..` 后，文件必须在仓库内的 `cases/benchmarks/` 或 `tests/vv/data/` 下，
大小不得超过 2 MiB。指向同一解析路径的参数只检查一次。

退出码 `0` 表示通过，`1` 表示存在违规（每条以 `<path>: <reason>` 写入 stderr），
`2` 表示用法或 Git 仓库发现错误。检查器只使用 Python 标准库。
`vv_result_schema` 中只有 schema 与检查器的一致性测试需要可选的 `jsonschema`，
缺包时该项跳过，其余测试照常运行。

## 许可

`tools/vv/` 与 `tests/vv/` 按 Apache-2.0 发布（见各目录 LICENSE）。
