"""Leave-one-session-out fold generation (F29, spec §6 D1, D2).

Random splits within a session leak temporal correlation (spec §5), so the
only split the skeleton offers is leave-one-session-out; leave-one-subject-out
is an M5 extension behind the same :class:`Fold` shape.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass


@dataclass(frozen=True)
class Fold:
    """One test fold: evaluate on ``test_session``, train on ``train_sessions``."""

    fold_index: int
    test_session: str
    train_sessions: tuple[str, ...]


def leave_one_session_out(session_ids: Sequence[str]) -> list[Fold]:
    """One fold per session; every session is the test session exactly once.

    Guarantees ``train_sessions ∩ {test_session} = ∅`` by construction.
    """
    ids = list(session_ids)
    if len(set(ids)) != len(ids):
        raise ValueError(f"duplicate session ids in fold generation: {ids}")
    if len(ids) < 2:
        raise ValueError(
            f"leave-one-session-out needs at least 2 sessions, got {len(ids)}: {ids}"
        )
    return [
        Fold(
            fold_index=i,
            test_session=session,
            train_sessions=tuple(o for o in ids if o != session),
        )
        for i, session in enumerate(ids)
    ]
