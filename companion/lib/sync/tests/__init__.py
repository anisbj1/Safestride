"""F20 sync test package.

Package marker so pytest imports this suite as `lib.sync.tests.*` (prepend mode
walks up through sync/ and lib/ which both carry `__init__.py`) instead of
registering a second top-level module named `conftest`. That top-level
registration collided with F21's `companion/tests/conftest.py`
(`from conftest import run`) and broke the natural whole-package invocation
`python -m pytest companion` (audit F20-AUD-3; same defect and fix as
F16-AUD-1). Helper modules are imported package-relatively
(`from .sync_test_support import ...`) so nothing resolves to a bare
top-level name either.
"""
