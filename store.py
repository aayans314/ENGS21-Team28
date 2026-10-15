"""
SeatNav storage: keeps the current state of every seat plus a history of changes.
Used by main.py; plain Python + SQLite, no extra libraries.

Two tables:
  seats   one row per seat, always the latest values (what the app shows right now)
  events  one row per state change or online/offline change (history, usage stats)

Heartbeats only refresh the seats row; they don't add history rows, so the
database stays small even with many sensors.
"""

import sqlite3
import threading

VALID_STATES = ("FREE", "OCCUPIED", "AWAY")

# A seat counts as offline if we haven't heard from it for this long.
# The sketch sends a heartbeat every 30 s, so 90 s = three missed heartbeats.
STALE_AFTER_S = 90

SCHEMA = """
CREATE TABLE IF NOT EXISTS seats (
    seat_id     TEXT PRIMARY KEY,
    state       TEXT,
    near        INTEGER,
    moving_cm   INTEGER,
    still_cm    INTEGER,
    rssi        INTEGER,
    online      INTEGER DEFAULT 0,
    last_seen   REAL,
    state_since REAL
);

CREATE TABLE IF NOT EXISTS events (
    id      INTEGER PRIMARY KEY AUTOINCREMENT,
    seat_id TEXT NOT NULL,
    ts      REAL NOT NULL,
    kind    TEXT NOT NULL,   -- 'state' (FREE/OCCUPIED/AWAY) or 'status' (online/offline)
    value   TEXT NOT NULL,
    rssi    INTEGER
);

CREATE INDEX IF NOT EXISTS idx_events_seat_ts ON events (seat_id, ts);
"""


def _int_or_none(value):
    """The sketch sends -1 when the radar sees no target of that kind."""
    try:
        number = int(value)
    except (TypeError, ValueError):
        return None
    return None if number == -1 else number


class Store:
    def __init__(self, path):
        self.lock = threading.Lock()   # MQTT runs in its own thread
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.executescript(SCHEMA)
        self.db.commit()

    # ---------- writing ----------

    def handle_state(self, seat_id, data, now, retained=False):
        """Process a message from <prefix>/seats/<seat_id>/state.
        Returns (seat_dict, changed) or (None, False) if the message is invalid."""
        state = str(data.get("state", "")).upper()
        if state not in VALID_STATES:
            return None, False

        rssi = _int_or_none(data.get("rssi"))

        with self.lock:
            row = self.db.execute(
                "SELECT state, online, last_seen, state_since FROM seats WHERE seat_id = ?",
                (seat_id,),
            ).fetchone()

            changed = row is None or row["state"] != state

            if retained:
                # A retained message is the broker replaying the last known value
                # (e.g. right after this server starts). It doesn't prove the
                # sensor is alive right now, so keep the old liveness info.
                last_seen = row["last_seen"] if row else None
                online = row["online"] if row else 0
            else:
                last_seen = now
                online = 1

            if changed:
                state_since = now
            else:
                state_since = row["state_since"] or now

            self.db.execute(
                """
                INSERT INTO seats (seat_id, state, near, moving_cm, still_cm, rssi,
                                   online, last_seen, state_since)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(seat_id) DO UPDATE SET
                    state = excluded.state,
                    near = excluded.near,
                    moving_cm = excluded.moving_cm,
                    still_cm = excluded.still_cm,
                    rssi = excluded.rssi,
                    online = excluded.online,
                    last_seen = excluded.last_seen,
                    state_since = excluded.state_since
                """,
                (
                    seat_id,
                    state,
                    1 if data.get("near") else 0,
                    _int_or_none(data.get("moving_cm")),
                    _int_or_none(data.get("still_cm")),
                    rssi,
                    online,
                    last_seen,
                    state_since,
                ),
            )

            if changed:
                self.db.execute(
                    "INSERT INTO events (seat_id, ts, kind, value, rssi) VALUES (?, ?, 'state', ?, ?)",
                    (seat_id, now, state, rssi),
                )

            self.db.commit()
            return self._seat(seat_id, now), changed

    def handle_status(self, seat_id, text, now, retained=False):
        """Process a message from <prefix>/seats/<seat_id>/status ("online" / "offline")."""
        value = text.strip().lower()
        if value not in ("online", "offline"):
            return None, False
        online = 1 if value == "online" else 0

        with self.lock:
            row = self.db.execute(
                "SELECT online, last_seen FROM seats WHERE seat_id = ?", (seat_id,)
            ).fetchone()

            changed = row is None or row["online"] != online

            last_seen = row["last_seen"] if row else None
            if online and not retained:
                last_seen = now

            self.db.execute(
                """
                INSERT INTO seats (seat_id, online, last_seen) VALUES (?, ?, ?)
                ON CONFLICT(seat_id) DO UPDATE SET
                    online = excluded.online,
                    last_seen = excluded.last_seen
                """,
                (seat_id, online, last_seen),
            )

            if changed:
                self.db.execute(
                    "INSERT INTO events (seat_id, ts, kind, value) VALUES (?, ?, 'status', ?)",
                    (seat_id, now, value),
                )

            self.db.commit()
            return self._seat(seat_id, now), changed

    def mark_silent_seats_offline(self, now):
        """Seats marked online that haven't reported for STALE_AFTER_S get marked
        offline (and an 'offline' history event). Returns the updated seats."""
        with self.lock:
            rows = self.db.execute(
                "SELECT seat_id FROM seats WHERE online = 1 AND (last_seen IS NULL OR last_seen < ?)",
                (now - STALE_AFTER_S,),
            ).fetchall()
        updated = []
        for r in rows:
            seat, _ = self.handle_status(r["seat_id"], "offline", now)
            updated.append(seat)
        return updated

    # ---------- reading ----------

    @staticmethod
    def _row_to_seat(row, now):
        live = (
            bool(row["online"])
            and row["last_seen"] is not None
            and now - row["last_seen"] <= STALE_AFTER_S
        )
        state = row["state"]
        return {
            "seat_id": row["seat_id"],
            "state": state,                                   # last reported state
            "display_state": state if (live and state) else "OFFLINE",  # what the app should show
            "live": live,
            "near": None if row["near"] is None else bool(row["near"]),
            "moving_cm": row["moving_cm"],
            "still_cm": row["still_cm"],
            "rssi": row["rssi"],
            "last_seen": row["last_seen"],
            "state_since": row["state_since"],
            "seconds_in_state": int(now - row["state_since"]) if row["state_since"] else None,
        }

    def _seat(self, seat_id, now):
        row = self.db.execute("SELECT * FROM seats WHERE seat_id = ?", (seat_id,)).fetchone()
        return self._row_to_seat(row, now) if row else None

    def get_seat(self, seat_id, now):
        with self.lock:
            return self._seat(seat_id, now)

    def get_seats(self, now):
        with self.lock:
            rows = self.db.execute("SELECT * FROM seats ORDER BY seat_id").fetchall()
        return [self._row_to_seat(r, now) for r in rows]

    @staticmethod
    def summarize(seats):
        summary = {"total": len(seats), "FREE": 0, "OCCUPIED": 0, "AWAY": 0, "OFFLINE": 0}
        for seat in seats:
            summary[seat["display_state"]] += 1
        return summary

    def get_history(self, seat_id, since):
        with self.lock:
            rows = self.db.execute(
                "SELECT ts, kind, value, rssi FROM events WHERE seat_id = ? AND ts >= ? ORDER BY ts",
                (seat_id, since),
            ).fetchall()
        return [dict(r) for r in rows]

    def get_usage(self, seat_id, since, now):
        """Seconds and percent of time spent in each state between `since` and `now`.
        Note: time while the sensor was offline counts toward its last known state."""
        with self.lock:
            before = self.db.execute(
                """SELECT value FROM events
                   WHERE seat_id = ? AND kind = 'state' AND ts < ?
                   ORDER BY ts DESC LIMIT 1""",
                (seat_id, since),
            ).fetchone()
            rows = self.db.execute(
                """SELECT ts, value FROM events
                   WHERE seat_id = ? AND kind = 'state' AND ts >= ? AND ts <= ?
                   ORDER BY ts""",
                (seat_id, since, now),
            ).fetchall()

        totals = {s: 0.0 for s in VALID_STATES}
        current = before["value"] if before else None
        current_since = since
        for r in rows:
            if current:
                totals[current] += r["ts"] - current_since
            current, current_since = r["value"], r["ts"]
        if current:
            totals[current] += now - current_since

        known = sum(totals.values())
        return {
            "seat_id": seat_id,
            "window_seconds": int(now - since),
            "seconds": {s: int(v) for s, v in totals.items()},
            "percent": {s: (round(100 * v / known, 1) if known else 0.0) for s, v in totals.items()},
        }
