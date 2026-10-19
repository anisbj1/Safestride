"""F16 companion mirror constants (spec §15.5).

Values mirror `docs/features/F16_sleep_nap_history.md` §8 — the firmware cannot be
imported from Python, so each mirror cites its spec line; `[ASSUMED]` mirrors are an
H9 drift-review item (spec D7/§13). No detector_id / FaultCode / schema_id lives
here: this is a read-only view, not a producer (plan §15.8).
"""

from __future__ import annotations

# [USER] spec §8 kDayBoundaryHour mirror (D5): logical day = [04:00, 04:00) UTC,
# keyed by the calendar day it ENDS on — the phone must key days exactly like the
# device, or the recomputes disagree on attribution (spec D5 UTC convention).
DAY_BOUNDARY_HOUR = 4

# [USER] spec §8 / D6 daytime-window start, applied to the episode's START hour (UTC).
DAYTIME_START_HOUR = 9

# [USER] spec §8 / D6 daytime-window end (exclusive).
DAYTIME_END_HOUR = 21

# [DERIVED] seconds per calendar day — D5 day-key arithmetic (`kSecPerDay` mirror).
SEC_PER_DAY = 86400

# [DERIVED] seconds per hour — D5 boundary offset (24 − 4)·3600 s.
SEC_PER_HOUR = 3600

# [HW] EventType indices (data_decision.md §4.5, append-only enum): the only two
# event kinds this view consumes (spec D7).
EVENT_TYPE_SLEEP_EPISODE = 0
EVENT_TYPE_NAP_COMPLETED = 9

# [ASSUMED] D7 / spec §8 kSleepDetectorIdMirror: F08's producer id. A `SleepEpisode`
# row from any other detector_id is skipped entirely — never decoded, never counted
# (a future second producer must not be mis-decoded here).
SLEEP_DETECTOR_ID_MIRROR = 8

# [DERIVED] D7 payload-layout mirror of F08's config header (01 §1.1 forbids
# including it; the firmware mirrors the same three indices).
SLEEP_PAYLOAD_FLAGS_INDEX = 0
SLEEP_PAYLOAD_CLASS_INDEX = 1
SLEEP_PAYLOAD_SHARE_INDEX = 2

# [DERIVED] D7 flag mirror: bit0 = open marker (episode incomplete — its close event
# arrives later, skip); bit1 = recovered-from-buffer (counts as Uncertain, 0 ms).
SLEEP_FLAG_OPEN = 0x01
SLEEP_FLAG_RECOVERED = 0x02

# [HW] UserLabel::Confirmed (data_decision.md §4.5): labels ride on the event row
# itself (F14 writes them via IEventStore.setLabel on the device).
USER_LABEL_CONFIRMED = 1

# [HW] EpisodeClass names (data_decision.md §4.5, append-only). Uncertain == 0 on
# purpose: a recovered episode falls back to it (D7).
EPISODE_CLASS_UNCERTAIN = 0
EPISODE_CLASS_MAIN_SLEEP = 3
EPISODE_CLASS_NAMES = {0: "Uncertain", 1: "Rest", 2: "Nap", 3: "MainSleep"}

# [HW] UserLabel names for render (data_decision.md §4.5, append-only).
EPISODE_LABEL_NAMES = {0: "Unlabeled", 1: "Confirmed", 2: "Rejected"}

# [DERIVED] D4 fallback identity: day_key 0 = unknown wall time, grouped per boot
# (a session summary) — never rendered as a date.
DAY_KEY_SESSION = 0

# [DERIVED] one `EventRepository.timeline()` page; "a few dozen events/day" (spec §8)
# makes a month 1-2 queries and bounds working memory.
HISTORY_PAGE_SIZE = 200

# [DERIVED] fetch safety cap for a pathological DB: the view reports "truncated"
# instead of growing without bound — rows are dropped with a visible note, never
# silently fabricated (spec §10).
MAX_HISTORY_EVENTS = 5000

# [EXPERIMENTAL] default console window (--days overridable): bounds the printed
# day list; no synthetic empty days are ever emitted (spec §10).
DEFAULT_HISTORY_DAYS = 30
