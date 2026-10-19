"""anarolepsy-companion (F21): local-first desktop companion for the bracelet.

Not a medical device; it observes only — the bracelet decides and alerts on its own
(docs/00_PROJECT_CONTEXT.md / docs/00 §7.1: the app is never required for immediate alerting).

The source root ``lib/`` installs as package ``companion`` (pyproject ``package-dir``);
tests register the same mapping in ``tests/conftest.py`` so no install is needed.
"""

__all__ = ["app", "bench", "sync_port"]
