"""Leave-one-session-out split tests (F29 spec §9 "Split", D1/D2)."""

import pytest
import split
from split import leave_one_session_out

# Two synthetic sessions (the canonical fixture ids) — D1 demonstrated on
# session-level splits, never on windows within a session.
_SESSIONS = ["synthetic_sA", "synthetic_sB"]


def test_two_sessions_produce_two_folds() -> None:
    folds = leave_one_session_out(_SESSIONS)
    assert len(folds) == 2


def test_every_session_is_the_test_session_once() -> None:
    folds = leave_one_session_out(_SESSIONS)
    assert [fold.test_session for fold in folds] == _SESSIONS
    assert [fold.fold_index for fold in folds] == [0, 1]


def test_train_and_test_are_disjoint() -> None:
    for fold in leave_one_session_out(_SESSIONS):
        assert fold.test_session not in fold.train_sessions  # leakage guard (spec §5)
        assert set(fold.train_sessions) | {fold.test_session} == set(_SESSIONS)


def test_single_session_raises() -> None:
    with pytest.raises(ValueError):
        leave_one_session_out(["synthetic_sA"])


def test_no_sessions_raise() -> None:
    with pytest.raises(ValueError):
        leave_one_session_out([])


def test_duplicate_session_ids_raise() -> None:
    with pytest.raises(ValueError):
        leave_one_session_out(["synthetic_sA", "synthetic_sA"])


def test_three_sessions_each_trains_on_the_other_two() -> None:
    folds = leave_one_session_out(["sA", "sB", "sC"])
    assert [(fold.test_session, fold.train_sessions) for fold in folds] == [
        ("sA", ("sB", "sC")),
        ("sB", ("sA", "sC")),
        ("sC", ("sA", "sB")),
    ]


def test_fold_is_a_deterministic_value_type() -> None:
    assert leave_one_session_out(_SESSIONS) == leave_one_session_out(_SESSIONS)
    assert split.Fold is type(leave_one_session_out(_SESSIONS)[0])
