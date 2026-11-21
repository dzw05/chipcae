# import-report/1.0 test fixtures

These four synthetic samples are vendored from the import-report/1.0 fixture
bundle frozen on 2026-11-21. The JSON and exit-code files are byte-for-byte copies.
They contain no customer data or real board geometry.

Each sample contains `defects.json` (the `--report` summary),
`import_report.json` (the output sidecar), and `exit_code`:

| Sample | Exit | Purpose |
| --- | --- | --- |
| `board_2layer` | 0 | Successful import with empty defects and no B-tier acceptance |
| `accept_b_tier` | 0 | Successful import with `b_tier_accepted` set to `"all"` |
| `confirm_b_tier_synthetic` | 6 | Three B-tier sidecar defects with an empty summary defect list |
| `assembly_parts_synthetic` | 0 | Assembly split/rewrite summaries and `welded_interfaces: 0` |

The board and accepted-B samples originated from the synthetic import CLI
goldens and were updated in the frozen bundle for the 1.0 fields. The confirmation
sample follows `tests/integration/import_cli/cli_test.py`; the assembly sample
follows `tests/integration/import_assembly/test_assembly_parts_summary.cpp`.

Desktop tests read these files from the checkout and replay temporary copies.
No external fixture directory or fixture environment variable is required.
