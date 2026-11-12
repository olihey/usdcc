"""Builds usdcc and launches it.

build.py with --run; it takes build.py's options (python tools/build.py --help).

    python tools/run.py
    python tools/run.py --configuration Debug --clean
    python tools/run.py --scene test_data/cube_and_sphere.usda
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build  # noqa: E402

if __name__ == "__main__":
    build.main(sys.argv[1:] + ["--run"], description=__doc__.split("\n\n")[0])
