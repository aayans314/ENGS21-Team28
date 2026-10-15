"""
SeatNav backend

  1. Subscribes to the seat sensors on the MQTT broker
  2. Saves every state change in a SQLite database (seatnav.db)
  3. Serves the data to your app:
       GET  /api/seats                       all seats + summary (free / occupied / away / offline)
       GET  /api/seats/{seat_id}             one seat
       GET  /api/seats/{seat_id}/history     state changes, default last 24 h (?hours=...)
       GET  /api/seats/{seat_id}/usage       % of time free / occupied / away (?hours=...)
       GET  /api/health                      is the server connected to the broker?
       WS   /ws                              live updates pushed the moment a seat changes
       GET  /                                test dashboard
       GET  /docs                            interactive API docs (try every endpoint in the browser)

Run:
  pip install -r requirements.txt
  uvicorn main:app --reload

Settings (environment variables, defaults match seatnav_mqtt.ino):
  MQTT_HOST     broker.hivemq.com
  MQTT_PORT     1883
  TOPIC_PREFIX  seatnav-feldberg   <- must match the start of TOPIC_BASE in the sketch
  DB_PATH       seatnav.db
"""

import asyncio
import json
import os
import time
import uuid
from contextlib import asynccontextmanager
from pathlib import Path

import paho.mqtt.client as mqtt
from fastapi import FastAPI, HTTPException, Query, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse

from store import Store

MQTT_HOST = os.getenv("MQTT_HOST", "broker.hivemq.com")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
TOPIC_PREFIX = os.getenv("TOPIC_PREFIX", "seatnav-feldberg-x7k2")
DB_PATH = os.getenv("DB_PATH", "seatnav.db")

STATIC_DIR = Path(__file__).parent / "static"

store = Store(DB_PATH)
ws_clients: set[WebSocket] = set()
event_loop: asyncio.AbstractEventLoop | None = None
mqtt_connected = False


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


def push_from_mqtt_thread(message: dict):
    """MQTT callbacks run in a background thread; hand the message to the web server's loop."""
    if event_loop is not None:
        asyncio.run_coroutine_threadsafe(broadcast(message), event_loop)


def snapshot():
    now = time.time()
    seats = store.get_seats(now)
    return {"seats": seats, "summary": Store.summarize(seats), "server_time": now}


# ---------- MQTT ----------

def on_connect(client, userdata, flags, reason_code, properties):
    global mqtt_connected
    if reason_code.is_failure:
        print(f"[mqtt] connection refused: {reason_code}")
        return
    mqtt_connected = True
    print(f"[mqtt] connected to {MQTT_HOST}:{MQTT_PORT}")
    client.subscribe(f"{TOPIC_PREFIX}/seats/+/state", qos=1)
    client.subscribe(f"{TOPIC_PREFIX}/seats/+/status", qos=1)
    print(f"[mqtt] listening on {TOPIC_PREFIX}/seats/+/state and /status")


def on_disconnect(client, userdata, disconnect_flags, reason_code, properties):
    global mqtt_connected
    mqtt_connected = False
    print(f"[mqtt] disconnected ({reason_code}), reconnecting automatically...")


def on_message(client, userdata, msg):
    parts = msg.topic.split("/")          # <prefix>/seats/<seat_id>/<state|status>
    if len(parts) < 3:
        return
    seat_id, kind = parts[-2], parts[-1]
    text = msg.payload.decode("utf-8", errors="replace").strip()
    if not text:
        return                            # empty retained message = topic was cleared
    now = time.time()

    if kind == "state":
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            print(f"[mqtt] ignored bad JSON on {msg.topic}: {text[:80]}")
            return
        seat, changed = store.handle_state(seat_id, data, now, retained=msg.retain)
    elif kind == "status":
        seat, changed = store.handle_status(seat_id, text, now, retained=msg.retain)
    else:
        return

    if seat is None:
        print(f"[mqtt] ignored unexpected message on {msg.topic}: {text[:80]}")
        return

    if changed:
        print(f"[seat] {seat_id}: {seat['display_state']}")
    push_from_mqtt_thread({"type": "seat", "seat": seat, "changed": changed})


async def watch_for_silent_seats():
    """A sensor that loses power may not send anything, so check regularly
    and tell the app when a seat goes quiet (or comes back)."""
    previous = {}
    while True:
        await asyncio.sleep(10)
        now = time.time()
        for seat in store.get_seats(now):
            was_live = previous.get(seat["seat_id"])
            if was_live is not None and was_live != seat["live"]:
                print(f"[seat] {seat['seat_id']}: {'back online' if seat['live'] else 'went silent'}")
                await broadcast({"type": "seat", "seat": seat, "changed": True})
            previous[seat["seat_id"]] = seat["live"]


@asynccontextmanager
async def lifespan(app: FastAPI):
    global event_loop
    event_loop = asyncio.get_running_loop()

    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id=f"seatnav-backend-{uuid.uuid4().hex[:8]}",
    )
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=30)
    client.loop_start()                   # runs in a background thread, reconnects on its own

    watcher = asyncio.create_task(watch_for_silent_seats())
    yield
    watcher.cancel()
    client.loop_stop()
    client.disconnect()


# ---------- web API ----------

app = FastAPI(title="SeatNav API", lifespan=lifespan)

# Allow the app (or a dev server on another port) to call the API from a browser.
# Tighten allow_origins to your app's address before going public.
app.add_middleware(CORSMiddleware, allow_origins=["*"], allow_methods=["*"], allow_headers=["*"])


@app.get("/")
def dashboard():
    return FileResponse(STATIC_DIR / "index.html")


@app.get("/api/health")
def health():
    return {"mqtt_connected": mqtt_connected, "broker": MQTT_HOST, "topic_prefix": TOPIC_PREFIX}


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
