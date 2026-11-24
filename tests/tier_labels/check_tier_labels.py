"""Check the labels of every configured test, including this check itself."""

import json
import subprocess
import sys

TIERS = {"unit", "module", "highmem", "perf", "acceptance"}
OWNERS = {f"owner:{code}" for code in
          ("M1", "M2", "M3", "M4", "M5", "M6", "M7", "INT", "ARCH", "INFRA")}


def main():
    result = subprocess.run(
        ["ctest", "--test-dir", sys.argv[1], "-N", "--show-only=json-v1"],
        check=True, capture_output=True, text=True,
    )
    tests = json.loads(result.stdout)["tests"]
    errors = []
    for test in tests:
        props = {prop["name"]: prop["value"] for prop in test.get("properties", [])}
        labels = props.get("LABELS", [])
        if isinstance(labels, str):
            labels = labels.split(";")
        tiers = [label for label in labels if label in TIERS]
        owners = [label for label in labels if label.startswith("owner:")]
        if len(tiers) != 1 or len(owners) != 1 or owners[0] not in OWNERS:
            errors.append(f"{test['name']}: tiers={tiers}, owners={owners}")
    if errors:
        raise SystemExit("Invalid CTest tier/owner labels:\n" + "\n".join(errors))
    print(f"Validated tier and owner labels for {len(tests)} tests")


if __name__ == "__main__":
    main()
