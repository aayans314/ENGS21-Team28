"""
SeatNav backend

  1. Receives reports straight from the seat sensors (POST /api/report)
  2. Saves every state change in a SQLite database (seatnav.db)
  3. Serves the data to the app:
       GET  /api/seats                       all seats + summary (free / occupied / away / offline)
       GET  /api/seats/{seat_id}             one seat
       GET  /api/seats/{seat_id}/history     state changes, default last 24 h (?hours=...)
       GET  /api/seats/{seat_id}/usage       % of time free / occupied / away (?hours=...)
       GET  /api/health                      quick "is the server up" check
       WS   /ws                              live updates pushed the moment a seat changes
       GET  /                                test dashboard
       GET  /docs                            interactive API docs (try every endpoint in the browser)

Run locally:
  pip install -r requirements.txt
  uvicorn main:app --host 0.0.0.0 --port 8000 --reload

Settings (environment variables):
  DEVICE_KEYS   comma-separated keys sensors must send in the X-Device-Key header
                (default "dev-key-change-me", matching the sketch; change it before deploying)
  DB_PATH       where the database file lives (default seatnav.db)
"""

import asyncio
import os
import secrets
import time
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, Header, HTTPException, Query, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field

from store import VALID_STATES, Store

DEFAULT_KEY = "dev-key-change-me"
DEVICE_KEYS = [k.strip() for k in os.getenv("DEVICE_KEYS", DEFAULT_KEY).split(",") if k.strip()]
DB_PATH = os.getenv("DB_PATH", "seatnav.db")

STATIC_DIR = Path(__file__).parent / "static"

store = Store(DB_PATH)
ws_clients: set[WebSocket] = set()


# ---------- live updates to the app ----------

async def broadcast(message: dict):
    dead = []
    for ws in list(ws_clients):
        try:
            await ws.send_json(message)
        except Exception:
            dead.append(ws)
    for ws in dead:
        ws_clients.discard(ws)


def snapshot():
    now = time.time()
    seats = store.get_seats(now)
    return {"seats": seats, "summary": Store.summarize(seats), "server_time": now}


async def watch_for_silent_seats():
    """A sensor that loses power just stops reporting, so check regularly and
    mark seats offline once they've been silent for 90 s."""
    while True:
        await asyncio.sleep(10)
        for seat in store.mark_silent_seats_offline(time.time()):
            print(f"[seat] {seat['seat_id']}: went silent, now OFFLINE")
            await broadcast({"type": "seat", "seat": seat, "changed": True})


@asynccontextmanager
async def lifespan(app: FastAPI):
    if DEVICE_KEYS == [DEFAULT_KEY]:
        print("[warning] using the default device key; set DEVICE_KEYS before deploying")
    watcher = asyncio.create_task(watch_for_silent_seats())
    yield
    watcher.cancel()


app = FastAPI(title="SeatNav API", lifespan=lifespan)

# Lets the web version of the app call the API from a browser.
# Tighten allow_origins to your app's web address before going public.
app.add_middleware(CORSMiddleware, allow_origins=["*"], allow_methods=["*"], allow_headers=["*"])


# ---------- sensors report here ----------

class Report(BaseModel):
    seat: str = Field(pattern=r"^[A-Za-z0-9_-]{1,40}$")
    state: str
    near: bool = False
    moving_cm: int = -1
    still_cm: int = -1
    rssi: int | None = None
    reason: str = "change"
    uptime_s: int | None = None


def key_is_valid(key: str | None) -> bool:
    if not key:
        return False
    return any(secrets.compare_digest(key, k) for k in DEVICE_KEYS)


@app.post("/api/report")
async def report(r: Report, x_device_key: str | None = Header(default=None)):
    if not key_is_valid(x_device_key):
        raise HTTPException(401, "Missing or wrong X-Device-Key")
    if r.state.upper() not in VALID_STATES:
        raise HTTPException(422, f"state must be one of {', '.join(VALID_STATES)}")

    now = time.time()
    store.handle_status(r.seat, "online", now)          # logs 'online' if it was offline or new
    seat, changed = store.handle_state(r.seat, r.model_dump(), now)

    if changed:
        print(f"[seat] {r.seat}: {seat['display_state']} ({r.reason})")
    await broadcast({"type": "seat", "seat": seat, "changed": changed})
    return {"ok": True, "display_state": seat["display_state"]}


# ---------- the app reads from here ----------

@app.get("/")
def dashboard():
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/api/health")
def health():
    return {"ok": True, "server_time": time.time()}


@app.get("/api/seats")
def list_seats():
    return snapshot()


@app.get("/api/seats/{seat_id}")
def get_seat(seat_id: str):
    seat = store.get_seat(seat_id, time.time())
    if seat is None:
        raise HTTPException(404, f"No data for seat '{seat_id}' yet")
    return seat


@app.get("/api/seats/{seat_id}/history")
def seat_history(seat_id: str, hours: float = Query(24, gt=0, le=24 * 90)):
    since = time.time() - hours * 3600
    return {"seat_id": seat_id, "since": since, "events": store.get_history(seat_id, since)}


@app.get("/api/seats/{seat_id}/usage")
def seat_usage(seat_id: str, hours: float = Query(24, gt=0, le=24 * 90)):
    now = time.time()
    return store.get_usage(seat_id, now - hours * 3600, now)


@app.websocket("/ws")
async def live_updates(ws: WebSocket):
    await ws.accept()
    ws_clients.add(ws)
    try:
        await ws.send_json({"type": "snapshot", **snapshot()})
        while True:
            await ws.receive_text()       # we don't expect messages; this just waits for disconnect
    except WebSocketDisconnect:
        pass
    finally:
        ws_clients.discard(ws)
