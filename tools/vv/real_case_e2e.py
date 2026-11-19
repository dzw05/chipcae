"""Import-only V-INT-12 stubs for the TEST-FIRST acceptance pass.

No pipeline or CLI behavior is implemented. Replace these empty bodies in the
implementation pass; the acceptance assertions intentionally fail against them.
"""
from pathlib import Path


def run_item(kind: str, params: dict, out_root: Path, nps: list[int], repeat: int,
             source_root: Path) -> dict:
    pass


def readiness(kind: str, source_root: Path) -> tuple[bool, str]:
    pass


def classify_solve_rc(rc: int | str) -> str | None:
    pass


def decode_events(path: Path) -> list[dict]:
    pass


def run_digest(run_dir: Path, events: list[dict]) -> dict:
    pass


def digest_sha256(d: dict) -> str:
    pass


def leak_check(diff_text: str, patterns: list[str]) -> list[str]:
    pass


def _check_c2(run_dir: Path, events: list[dict]) -> bool:
    pass


def _check_c3(events: list[dict]) -> bool:
    pass


def _check_c4(events: list[dict]) -> bool:
    pass


def _check_c5(run_dir: Path, energy: dict) -> bool:
    pass


def _check_c6(run_dir: Path, loads: list[dict]) -> bool | None:
    pass


def _check_c9(record: dict) -> bool | None:
    pass


def _check_c10(result_path: Path, data_class: str) -> bool:
    pass


def _check_w1(run_dir: Path, events: list[dict]) -> bool:
    pass


def _check_w2(run_dir: Path) -> bool:
    pass


def _check_w3(run_dir: Path, constraints: dict) -> bool | None:
    pass


def _check_a1(import_report_path: Path, stack_path: Path, import_rc: int,
              expected_body_ids: list[str], accept_b_tier: bool) -> bool:
    pass
