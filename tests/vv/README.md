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

## CTest SKIP 审计

`tools.vv.ctest_skip_audit` 只用 Python 标准库，从一份 CTest JUnit 和对应构建目录的
`CMakeCache.txt` 汇总 Passed、Failed、Skipped、NotRun，不启动 CTest。

```sh
python3 -m tools.vv.ctest_skip_audit --junit build/junit.xml \
  --build-dir build --out build/skip-audit --required tests/vv/skip_audit_required.tsv
python3 -m unittest tests.vv.test_ctest_skip_audit -v
ctest --test-dir build -R '^vv_ctest_skip_audit_unit$' --output-on-failure
```

输出 `skip-audit.json` 与 `skip-audit.md`；已有输出目录中仅覆盖这两个文件。
退出码 0 表示没有启用的必跑项被跳过、未运行或缺失，1 表示存在这些违规，2 表示参数、
输入或输出读写错误。普通 Failed 仍按 Failed 计数，不产生 skip 违规；必须同时保留
原始 CTest 的退出码。省略 `--required` 时仅汇总，合法输入的审计退出码为 0。

必跑清单为三列 TSV：`test_name<TAB>requires<TAB>note`，忽略空行和 `#` 注释。
`requires` 为 `-` 时无条件必跑，否则其中逗号分隔的所有 BOOL 开关必须开启。
CMake 的 ON/TRUE/YES/Y/1 等真值不区分大小写；OFF/FALSE/NO/N/0、空值、IGNORE、
NOTFOUND 和 `*-NOTFOUND` 为关闭。缓存中缺少的开关按 OFF 处理并给出警告。
缺失测试始终列在 `missing_required`，只在启用时违规；未启用项列出 OFF/缺失开关。
生产清单固定为六项，增删须由 vv 裁定。

四条 CLI 链路的 CMake 环境门控均需 ECAD、MFEM、MPI、HDF5，且需要相应程序 target：
`f1130_uncut_cli_e2e`、`chain_ipc_uncut_chain`、`odbcli_odbpp_cli_e2e`、
`fvm_fvm_case_e2e`。`field_compare_ranks` 按 brief 仅以 MPI 为必跑门控，
但它的实际注册还依赖 MFEM、HDF5 和 solver target，因此 MPI 单独开启时可能报 Missing。
`vv_result_schema` 无条件必跑。审计器只根据清单中的 BOOL 判断，不推断 target 是否存在。

支持 `<testsuite>`/`<testsuites>` 根，精确测试名重复时按文档顺序取最后一次，并警告次数。
Disabled 优先归 NotRun；其他带 `<skipped>` 的测试归 Skipped，优先于 `status="notrun"`；
然后依次判断 notrun、failure/fail、成功 run。未知状态或无效 XML 结构报输入错误。
原因优先取 skipped 的非空 message，其次取文本，再取 system-out 原文；不解析控制台文字
判断状态。原因最多保留 500 字符，JSON 保留原始空白，Markdown 对表格换行、竖线做显示转义。
Skipped 的 `time` 写为 `elapsed_seconds`，缺失、非有限或负值写 null 并警告。

报告记录所有 `CHIPCAE_ENABLE_*` BOOL 的原始值与启用状态，以及 `shutil.which("mpiexec")`。
提交取缓存 `CMAKE_HOME_DIRECTORY:INTERNAL` 所指源码的当前 HEAD；发现失败写 null 和警告，
不会回退到审计器所在仓库。该 HEAD 可能不同于原构建时提交，复用旧 JUnit 时应另外记录
构建来源与时间。`data_class` 固定为 synthetic，报告列表按名称排序。
35 个验收测试使用合成样本及临时 Git 仓库，包括 500 条测试在 5 秒内完成的检查。

## 许可

`tools/vv/` 与 `tests/vv/` 按 Apache-2.0 发布（见各目录 LICENSE）。
