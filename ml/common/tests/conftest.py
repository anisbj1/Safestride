"""Make the ml/common modules importable as flat modules in tests.

``ml/common`` is not a Python package (``ml/pyproject.toml`` is F00-owned and
declares no packages), so tests import ``schema`` / ``reference`` /
``export_cpp`` as top-level modules; this conftest puts their directory on
``sys.path`` before any test module is imported. Keeping the path fix here
avoids E402 (imports after code) inside the test modules.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
