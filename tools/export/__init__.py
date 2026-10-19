"""F28 export half: turn recorded session directories into schema-annotated CSVs.

Host-only, stdlib-only (F28 spec §16, decisions D14–D18): session directories
on disk are the source (D14), output is per-source CSV with #META schema rows
plus an export manifest (D15), CSV only (D16), lossy sessions refused by
default (D17), binary events decoded solely by tools/decode_events.py (D18).
"""
