"""Collected by chipcae_auto_tests: proves test_*.py discovery and the argv/env contract."""
import os
import sys
from pathlib import Path

root = Path(sys.argv[1])
if not (root / "cmake" / "ChipcaeAutoTests.cmake").is_file():
    print(f"bad project root {root}", file=sys.stderr)
    sys.exit(1)
if os.environ.get("CHIPCAE_SOURCE_DIR") != str(root):
    print("CHIPCAE_SOURCE_DIR not set to the project root", file=sys.stderr)
    sys.exit(1)
print("autoglob py ok")
