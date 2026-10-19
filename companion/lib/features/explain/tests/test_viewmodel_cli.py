"""ViewModel + CLI over a seeded DB (spec §9 items 29-31): real repository pipeline
(encode_event48 -> upsert), --seq/--limit/--device, exit codes, env default path."""

from __future__ import annotations

import pathlib

from companion.data import db
from companion.data.event_repository import EventRepository
from companion.data.models import EVENT_SCHEMA_VERSION, EventRecord
from companion.features.explain.builder import (
    AmbiguousSeq,
    EventExplanation,
    ExplanationViewModel,
    main,
    render_ambiguous,
)
from companion.transport.codec import encode_event48

DEVICE_A = "aa" * 6
DEVICE_B = "bb" * 6


def make_record(seq: int, *, device_hint: int = 0, **overrides) -> EventRecord:
    fields: dict[str, object] = dict(
        t_start_ms=1_000_000 + seq * 1000,
        seq=seq,
        boot_id=1,
        duration_ms=0,
        t_start_unix=0,
        confidence=0.7,
        reasons=(1 << 0) | (1 << 20),  # one known + one unknown bit: both render paths
        payload=b"\x00" * 8,
        quality=0,
        type=1,
        label=0,
        context=0,
        alertness_at_start=0,
        detector_id=device_hint,
        schema_version=EVENT_SCHEMA_VERSION,
        raw=b"\x00" * 48,
        wall_unix=0,
    )
    fields.update(overrides)
    return EventRecord(**fields)  # type: ignore[arg-type]


def seed(repo, device_id: str, seqs: range | list[int]) -> None:
    for seq in seqs:
        raw = encode_event48(make_record(seq))
        repo.upsert(device_id, [raw])


def seed_with(repo, device_id: str, seq: int, **overrides) -> None:
    """One row with overridden content, so two devices' rows are tellable apart."""
    repo.upsert(device_id, [encode_event48(make_record(seq, **overrides))])


def test_for_seq_returns_exact_event(repo):
    seed(repo, DEVICE_A, range(1, 4))
    explanation = ExplanationViewModel(repo).for_seq(2)
    assert explanation is not None
    assert explanation.seq == 2
    assert explanation.label == "possible drowsiness (confidence 0.70)"
    assert [item.bit_index for item in explanation.reasons] == [0, 20]
    assert explanation.reasons[1].text == "reason #20"


def test_for_seq_missing_is_none(repo):
    """A seq that was never synced resolves to None (never guessed)."""
    seed(repo, DEVICE_A, [1])
    assert ExplanationViewModel(repo).for_seq(99) is None


def test_for_seq_same_seq_on_two_devices_is_ambiguous(repo):
    """Audit F22-AUD-1: with no device scope, the same seq on devices A and B
    surfaces BOTH candidates as an ambiguity — never silently picks one."""
    seed_with(repo, DEVICE_A, 3, confidence=0.4)
    seed_with(repo, DEVICE_B, 3, confidence=0.9)
    result = ExplanationViewModel(repo).for_seq(3)
    assert isinstance(result, AmbiguousSeq)
    assert result.seq == 3
    assert len(result.candidates) == 2  # one row per device: both surfaced
    assert all(candidate.seq == 3 for candidate in result.candidates)
    # the two devices' rows are distinguishable (confidence), so neither was dropped
    assert sorted(c.confidence_text for c in result.candidates) == ["0.40", "0.90"]
    rendered = render_ambiguous(result)
    assert "ambiguous" in rendered
    assert "no row picked" in rendered
    assert "0.40" in rendered and "0.90" in rendered  # BOTH candidates rendered


def test_for_seq_with_device_scope_names_that_device(repo):
    """Audit F22-AUD-1: a device scope resolves the same seq to THAT device's
    row and names it (device_id in the result) instead of guessing."""
    seed_with(repo, DEVICE_A, 3, confidence=0.4)
    seed_with(repo, DEVICE_B, 3, confidence=0.9)
    scoped = ExplanationViewModel(repo).for_seq(3, device_id=DEVICE_B)
    assert isinstance(scoped, EventExplanation)
    assert scoped.seq == 3
    assert scoped.device_id == DEVICE_B  # named by the scope, not inferred
    assert scoped.confidence_text == "0.90"
    # the view-level scope (constructor seam) resolves the other device's row
    view_scoped = ExplanationViewModel(repo, device_id=DEVICE_A).for_seq(3)
    assert isinstance(view_scoped, EventExplanation)
    assert view_scoped.device_id == DEVICE_A
    assert view_scoped.confidence_text == "0.40"


def test_rows_newest_first_and_limit(repo):
    seed(repo, DEVICE_A, range(1, 6))
    vm = ExplanationViewModel(repo, limit=3)
    assert [row.seq for row in vm.rows()] == [5, 4, 3]
    assert [row.seq for row in vm.rows(limit=2)] == [5, 4]


def test_device_filter(repo):
    seed(repo, DEVICE_A, [1, 2])
    seed(repo, DEVICE_B, [1])
    vm = ExplanationViewModel(repo, device_id=DEVICE_B)
    assert [row.seq for row in vm.rows()] == [1]
    assert ExplanationViewModel(repo, device_id=DEVICE_B).for_seq(2) is None


def test_cli_seq_found(repo, tmp_path: pathlib.Path, capsys):
    seed(repo, DEVICE_A, [1, 2])
    code = main(["--seq", "2"], db_path=tmp_path / "companion.sqlite3")
    assert code == 0
    out = capsys.readouterr().out
    assert "seq 2" in out
    assert "possible drowsiness (confidence 0.70)" in out
    assert "SustainedLowMotion:" in out
    assert "reason #20" in out


def test_cli_seq_missing_exits_nonzero(tmp_path: pathlib.Path, capsys):
    """Spec §10: --seq miss -> exit 1 + named message, no traceback."""
    path = tmp_path / "companion.sqlite3"
    db.open_db(path).close()
    code = main(["--seq", "42"], db_path=path)
    assert code == 1
    assert "event seq 42 not found" in capsys.readouterr().out


def test_cli_seq_ambiguous_exits_nonzero_and_resolves_with_device(
    repo, tmp_path: pathlib.Path, capsys
):
    """Audit F22-AUD-1 + spec §10: --seq on a seq recorded by two devices without
    --device -> exit 1, every candidate rendered, no row picked; the same lookup
    with --device -> exit 0 and only that device's row."""
    seed_with(repo, DEVICE_A, 3, confidence=0.4)
    seed_with(repo, DEVICE_B, 3, confidence=0.9)
    path = tmp_path / "companion.sqlite3"
    code = main(["--seq", "3"], db_path=path)
    assert code == 1
    out = capsys.readouterr().out
    assert "ambiguous" in out
    assert "no row picked" in out
    assert "0.40" in out and "0.90" in out  # both candidates rendered
    code = main(["--seq", "3", "--device", DEVICE_B], db_path=path)
    assert code == 0
    out = capsys.readouterr().out
    assert f"device {DEVICE_B}" in out  # the row is named by the scope
    assert "0.90" in out and "0.40" not in out  # only B's row


def test_cli_list_newest_first(tmp_path: pathlib.Path, capsys):
    path = tmp_path / "companion.sqlite3"
    conn = db.open_db(path)
    try:
        seed(EventRepository(conn), DEVICE_A, [1, 2])
    finally:
        conn.close()
    code = main(["--limit", "1"], db_path=path)
    assert code == 0
    out = capsys.readouterr().out
    assert "seq 2" in out
    assert "seq 1" not in out


def test_cli_env_default_path(tmp_path: pathlib.Path, monkeypatch, capsys):
    """Spec §9 item 31: ANAROLEPSY_COMPANION_DB is the same env contract as app.py."""
    path = tmp_path / "env.sqlite3"
    monkeypatch.setenv(db.ENV_DB_PATH, str(path))
    conn = db.open_db(path)
    try:
        seed(EventRepository(conn), DEVICE_A, [1])
    finally:
        conn.close()
    assert main([]) == 0
    assert "seq 1" in capsys.readouterr().out


def test_cli_empty_db_is_not_an_error(tmp_path: pathlib.Path, capsys):
    path = tmp_path / "companion.sqlite3"
    db.open_db(path).close()
    assert main([], db_path=path) == 0
    assert "no events synced yet" in capsys.readouterr().out
