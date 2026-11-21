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

## F-1130 一键彩排

驱动只用 Python 标准库，使用已有合成夹具，固定按以下顺序报告：
`f1130_uncut_cli_e2e`、`f1130_uncut_cli_records`、
`chain_ipc_uncut_chain`、`odbcli_odbpp_cli_e2e`。

```sh
python -m tools.vv.f1130_rehearsal --build-dir "$BUILD_DIR" --out "$OUT_DIR" --ranks 1,2
```

`BUILD_DIR` 必须是已构建的 Release 目录，四个缓存项
`CHIPCAE_ENABLE_ECAD`、`CHIPCAE_ENABLE_MFEM`、`CHIPCAE_ENABLE_MPI`、
`CHIPCAE_ENABLE_HDF5` 均须开启。驱动保留缓存原值，接受标准 CMake 真值
（ON/YES/TRUE/Y、非零数值，大小写不敏感），缺项与假值均列出诊断。
MPI 仅用 `shutil.which("mpiexec")` 查找，`mpirun` 不作替代。
这四个开关不保证运行依赖完整；实测的求解流水线还需要启用 JSONCONS，
测试解释器需有 NumPy、h5py。实际缺依赖时的失败或跳过会保留在报告中。
先以 `ctest --test-dir "$BUILD_DIR" -N` 检查四个名字的精确注册，
再以以下 argv 执行（不经 shell）：

```sh
ctest --test-dir "$BUILD_DIR" -R '^(f1130_uncut_cli_e2e|f1130_uncut_cli_records|chain_ipc_uncut_chain|odbcli_odbpp_cli_e2e)$' -j2 --output-on-failure --output-junit "$OUT_DIR/junit.xml"
```

`--ranks` 默认 `1,2`，接受仅由 `1`、`2` 组成的非空逗号列表；空项、其他整数和
其他字符串均报参数错。该参数只改变报告元数据，各测试保持自身的 rank 行为。

退出码与判定：

- `0`：`PASS`，四个精确匹配且各只有一个 JUnit 结果，均为 `status="run"`，
  没有 failure/error/skipped，CTest 返回 0，且不存在依赖缺口。
- `1`：有效 XML 含 Failed、Skipped 或 Not Run，或 CTest 返回非零，报告 `FAIL`。
  failure/error 优先记 Failed，skipped 记 Skipped，其余非 run（含缺失 status）记
  Not Run。缺失、空或损坏的 JUnit 报告 `NOT_RUN`，保留 CTest 实际退出码；
  启动 CTest 失败时退出码为 null。缺失或重复的 testcase 不会被默认为通过。
- `2`：参数、目录、缓存、Git 来源、发现或报告写入失败。普通模式下缺后端或
  mpiexec 在任何 CTest 调用之前退出；发现失败或缺精确名字则只调用发现，
  不执行测试。输出目录可写时，操作性前检失败仍写 `NOT_RUN` 诊断报告。

`--allow-missing-backend` 允许带依赖缺口发现并执行测试，仍不能 PASS。
每项缺口在 `preflight_errors` 和状态为 Skipped 的 `readiness_gaps` 中保留。
按现有 CMake 注册条件，缺任何后端会阻止三个 e2e 的可执行文件注入；
若它们的 XML 仍为 Passed，则有效状态记 Skipped，原始 status 属性保留为 `junit_status`，
并附 readiness 原因。已有 Failed、Skipped、Not Run 不被覆盖。
records 测试没有后端要求。mpiexec 缺失只作为全局 readiness 缺口，
不推断每个测试与 MPI 的依赖关系；各项实际判定仍来自 XML。

报告 UTF-8 文件为 `rehearsal.json`、`rehearsal.md`；原始结果为 `junit.xml`。
只覆盖这三个文件，执行前移除旧的 `junit.xml`，避免复用旧通过结果。
其他文件保持不变。并发调用须使用不同输出目录。
Markdown 表格按清单排序并转义竖线和换行，围栏保留完整诊断原文，
并附完整 JSON，使两种格式包含相同信息。

JSON 字段：

| 字段 | 内容 |
| --- | --- |
| commit / build_dir | 脚本仓库根目录的 `git rev-parse HEAD` / 解析后的构建路径；未取得 commit 时为 null |
| backends | 上述四个完整 cache key 对应原始字符串；缺项为 null |
| mpiexec | 查找得到的路径，未找到为 null |
| ranks / allow_missing_backend | 请求的 rank 列表 / 是否启用允许缺口模式 |
| preflight_errors | 所有前检错误和 readiness 缺口 |
| ctest_listing / ctest_listing_returncode | 发现的 stdout、stderr 原文 / 退出码；未调用时为 null |
| ctest_command / ctest_returncode | 执行 argv 数组 / 实际退出码；未执行时为空数组 / null |
| tests | 清单顺序的 name、status、time_s、skip_or_fail_message；未测时间为 null |
| wall_time_s | monotonic 总墙钟秒数，包含共享构建槽的排队时间，不是纯求解耗时 |
| data_class / overall | 固定 synthetic / PASS、FAIL、NOT_RUN |

附加证据包括执行 stdout/stderr `ctest_output`、有效 XML 原文 `junit_xml`、
无法读取 XML 时的 `junit_error` 和允许缺口模式下的 `readiness_gaps`。
每项的诊断保留 testcase 属性、failure/error/skipped 属性与文本、system-out/err。
此报告不是 `chipcae.vv_result/0.2` schema，不对它调用 `check_file()`。

验收单元测试（临时目录、mock 子进程，不需要后端）：

```sh
python -m py_compile tools/vv/f1130_rehearsal.py tests/vv/test_f1130_rehearsal.py
ctest --test-dir "$BUILD_DIR" -N -R '^vv_f1130_rehearsal_unit$'
ctest --test-dir "$BUILD_DIR" -R '^vv_f1130_rehearsal_unit$' --output-on-failure
```

实测前查看 `MemAvailable`，低于 4 GB 时使用指定 Mac 自托管环境。
共享构建环境须保留 shim 的 PATH 优先级，再追加依赖环境 bin，并设置
`LIBRARY_PATH` 为依赖环境 lib；不绕过包装器，驱动自身提供 `-j2`。
记录 commit、完整命令、退出码和实际结果，报告标「实测」，不将 Skipped 当 Passed。
负向实测用保留 shim 但排除所有 mpiexec 目录的受控 PATH，预期退出 `2`。
仓库内不保存本地环境绝对路径或真实算例数据。

## 许可

`tools/vv/` 与 `tests/vv/` 按 Apache-2.0 发布（见各目录 LICENSE）。
