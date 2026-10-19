"""F22 explain test package.

Package marker so pytest imports this suite as `lib.features.explain.tests.*`
(prepend mode walks up through explain/, features/, lib/ which all carry
`__init__.py`) instead of registering a second top-level module named `conftest`.
That top-level registration collided with F21's `companion/tests/conftest.py`
(`from conftest import run`) and broke the natural whole-package invocation
`python -m pytest companion` (audit F22-AUD-4, same class as F16-AUD-1).
"""
