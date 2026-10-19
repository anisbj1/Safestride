"""CT10–CT16: SyncClient against F21's FakeTransport (subscriptions, MTU gate,
ack clamp, contiguity gate, failure behavior — spec §9/§10, D9/D10/D14)."""

from __future__ import annotations

import pytest
from companion.data.models import SyncStatus
from companion.sync import frames
from companion.sync.client import SyncClient, SyncMtuError
from companion.sync.frames import (
    decode_cursor,
    encode_chunk,
    encode_status,
    is_status_query,
)
from companion.transport.ports import MIN_SYNC_MTU_BYTES, SYNC_CONTROL_UUID, SYNC_DATA_UUID

from .sync_test_support import RecordingTransport, make_event48, record_seq, run


def _client(mtu: int = 255):
    transport = RecordingTransport(mtu=mtu)
    run(transport.connect(None))  # F21's fake guards every call on `connected`
    return SyncClient(transport), transport


def _deliver(client: SyncClient, transport: RecordingTransport, seqs: list[int], chunk_no: int = 1):
    """Simulate one Data notification carrying the given seqs."""
    transport.emit(SYNC_DATA_UUID, encode_chunk(chunk_no, [make_event48(s) for s in seqs]))


def test_CT10_first_call_subscribes_then_query_and_cursor_mtu_gate() -> None:
    client, transport = _client()
    run(client.request_from(1, 64))
    # subscription precedes any write (D14, §7 bench checklist)
    assert transport.events[:2] == [
        ("start_notify", SYNC_CONTROL_UUID),
        ("start_notify", SYNC_DATA_UUID),
    ]
    assert len(transport.writes) == 2
    first_char, first_frame = transport.writes[0]
    assert first_char == SYNC_CONTROL_UUID and is_status_query(first_frame)  # D14
    second_char, second_frame = transport.writes[1]
    assert second_char == SYNC_CONTROL_UUID
    request = decode_cursor(second_frame)
    assert (request.from_seq, request.max_events, request.ack_seq) == (1, 64, 0)
    # idempotent: a second request subscribes nothing and re-sends no query
    run(client.request_from(2))
    assert len(transport.writes) == 3
    assert transport.events.count(("start_notify", SYNC_CONTROL_UUID)) == 1
    # defense in depth behind F21's D8 gate: MTU < 55 => SyncMtuError, zero writes
    low_client, low_transport = _client(mtu=MIN_SYNC_MTU_BYTES - 1)
    with pytest.raises(SyncMtuError):
        run(low_client.request_from(1))
    with pytest.raises(SyncMtuError):
        run(low_client.ack(1))
    assert low_transport.writes == []
    assert low_transport.events == []


def test_CT11_ack_writes_base_monotonic() -> None:
    client, transport = _client()
    run(client.request_from(6, 64))  # committed prefix = 5
    run(client.ack(15))  # caller's arg is clamped to base (D10)
    char, frame = transport.writes[-1]
    assert char == SYNC_CONTROL_UUID
    assert decode_cursor(frame).ack_seq == 5 and decode_cursor(frame).from_seq == 0
    delivered: list[int] = []
    client.set_record_sink(lambda records: delivered.extend(record_seq(r) for r in records))
    _deliver(client, transport, [6, 7, 8])
    run(client.ack(8))
    assert decode_cursor(transport.writes[-1][1]).ack_seq == 8  # monotonic
    assert delivered == [6, 7, 8]


def test_CT12_duplicate_ack_suppressed_and_counted() -> None:
    client, transport = _client()
    run(client.request_from(1))
    client.set_record_sink(lambda records: None)  # committed delivery advances base
    _deliver(client, transport, [1, 2, 3])
    run(client.ack(3))
    writes_after_first_ack = len(transport.writes)
    run(client.ack(3))  # base unchanged: suppressed re-ack (D10)
    run(client.ack(99))  # arg can never push the wire value past base
    assert len(transport.writes) == writes_after_first_ack
    assert client.acks_suppressed == 2
    assert decode_cursor(transport.writes[-1][1]).ack_seq == 3


def test_CT13_contiguity_gate_holds_gap_releases_in_order_no_ack_past_base() -> None:
    client, transport = _client()
    run(client.request_from(6, 64))  # base = 5 (committed prefix)
    sink_calls: list[list[int]] = []
    client.set_record_sink(
        lambda records: sink_calls.append([record_seq(r) for r in records])
    )
    _deliver(client, transport, [11, 12, 13, 14, 15])  # gap 6..10 still missing
    assert sink_calls == []  # nothing beyond the hole is delivered (D9)
    assert client.records_delivered == 0
    run(client.ack(15))  # trigger only: wire value clamped to base=5 (D10)
    assert decode_cursor(transport.writes[-1][1]).ack_seq == 5
    # seq <= base duplicates are dropped + counted
    _deliver(client, transport, [3, 4, 5], chunk_no=2)
    assert sink_calls == [] and client.duplicate_records == 3
    _deliver(client, transport, [6, 7, 8, 9, 10], chunk_no=3)  # gap fills
    assert sink_calls == [[6, 7, 8, 9, 10, 11, 12, 13, 14, 15]]  # one sink call, in order
    assert client.records_delivered == 10
    run(client.ack(15))
    assert decode_cursor(transport.writes[-1][1]).ack_seq == 15  # base advanced


def test_CT13b_hold_buffer_overflow_drops_held_and_counts() -> None:
    """§10 overflow row: > MAX_HELD_RECORDS held ⇒ drop held + count; nothing
    beyond `base` was acked, so the device retransmits (safe by construction)."""
    client, transport = _client()
    run(client.request_from(1))
    delivered: list[int] = []
    client.set_record_sink(lambda records: delivered.extend(record_seq(r) for r in records))
    bound = frames.MAX_HELD_RECORDS  # 64
    seqs = list(range(100, 100 + bound + 6))  # a hole at 1..99 holds every record
    for i in range(0, len(seqs), 5):
        _deliver(client, transport, seqs[i : i + 5], chunk_no=i // 5 + 1)
    assert delivered == []  # the hole never filled: nothing released
    # the 13th chunk (65th record) trips the bound: all 65 held dropped + counted,
    # the final 5 records sit in the now-empty hold buffer (spec §10 overflow row).
    assert client.held_dropped == bound + 1
    assert len(client._held) == 5


def test_CT14_sink_exception_no_base_advance_no_ack() -> None:
    client, transport = _client()
    run(client.request_from(1))

    def broken_sink(records: list[bytes]) -> None:
        raise RuntimeError("db locked")

    client.set_record_sink(broken_sink)
    _deliver(client, transport, [1, 2, 3])
    assert client.sink_errors == 1
    assert client.records_delivered == 0
    writes = len(transport.writes)
    run(client.ack(3))  # base never advanced: nothing acked (P04 mirror, §10)
    assert len(transport.writes) == writes
    assert client.acks_suppressed == 1
    # session stays alive: a working sink receives the retransmission
    delivered: list[int] = []
    client.set_record_sink(lambda records: delivered.extend(record_seq(r) for r in records))
    _deliver(client, transport, [1, 2, 3], chunk_no=2)
    assert delivered == [1, 2, 3]


def test_CT15_status_frame_updates_status_and_sink() -> None:
    client, transport = _client()
    assert client.status() == SyncStatus(linked=True)  # linked from construction
    run(client.request_from(1))  # first call subscribes (D14)
    seen: list[SyncStatus] = []
    client.set_status_sink(seen.append)
    transport.emit(
        SYNC_CONTROL_UUID,
        encode_status(100, 32, 0x01 | 0x02 | 0x04 | 0x08 | 0xF0),  # reserved bits set
    )
    status = client.status()
    assert (status.last_seq, status.last_synced) == (100, 32)
    assert status.link_up and status.streaming
    assert status.mtu_too_low and status.transfer_aborted  # flags surfaced (§10)
    assert status.linked
    assert seen == [status]


def test_CT16_malformed_data_frames_dropped_then_good_chunk_works() -> None:
    client, transport = _client()
    run(client.request_from(1))  # first call subscribes (D14)
    delivered: list[int] = []
    client.set_record_sink(lambda records: delivered.extend(record_seq(r) for r in records))
    transport.emit(SYNC_DATA_UUID, b"garbage!")  # not even a length rule
    corrupt = bytearray(encode_chunk(1, [make_event48(1)]))
    corrupt[-1] ^= 0xFF  # bad CRC (§4.7)
    transport.emit(SYNC_DATA_UUID, bytes(corrupt))
    off_pattern = encode_chunk(1, [make_event48(1)]) + b"\x00\x00\x00\x00"  # 56 B: % 48 != 4
    transport.emit(SYNC_DATA_UUID, off_pattern)
    assert client.frames_dropped == 3
    assert delivered == []  # the sink is never called with invalid bytes (§10)
    _deliver(client, transport, [1, 2])  # next good chunk is processed normally
    assert delivered == [1, 2] and client.chunks_received == 1
