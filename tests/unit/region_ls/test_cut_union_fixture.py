"""Run the checked-in I10 fixture validator; propagate every failure."""

import os
from pathlib import Path
import subprocess
import sys


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else os.environ["CHIPCAE_SOURCE_DIR"])
    checker = root / "tests" / "contract" / "fixtures" / "cut-union-1.0" / "check.py"
    return subprocess.run([sys.executable, str(checker)], check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
