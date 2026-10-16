/*
  SeatNav: radar + direct reports to the SeatNav backend (HTTP/HTTPS)

  Same seat logic as before. Every state change, plus a heartbeat every
  30 s, is sent straight to the backend as an HTTP POST. No MQTT broker.

  States (sent as text in the "state" field):
    OCCUPIED = someone is sitting here
    AWAY     = person stepped away, may come back
    FREE     = seat free

  How it decides:
    - Someone within SEAT_RANGE_CM for CONFIRM_MS        -> OCCUPIED
    - Nobody close for AWAY_DELAY_MS while OCCUPIED      -> AWAY
        (but if they only sat for less than SHORT_VISIT_MS, go straight to FREE)
    - Back within range while AWAY                       -> OCCUPIED
    - Still nobody after AWAY_TIMEOUT_MS while AWAY      -> FREE

  What gets sent (POST to REPORT_URL, header X-Device-Key):
    {"seat":"seat-01","state":"OCCUPIED","near":true,"moving_cm":-1,"still_cm":42,
     "rssi":-58,"reason":"change","uptime_s":123}
    reason = "boot" (just powered on), "change" (state changed), "heartbeat" (every 30 s)
    If a send fails, it retries every 5 s until it gets through.
    The backend marks the seat OFFLINE if it hears nothing for 90 s.

  Only needs the MyLD2410 library (WiFi and HTTPClient come with the ESP32 board package).

  Wiring:
    Radar:   VCC -> 5V, GND -> GND, TX -> GPIO 16, RX -> GPIO 17
    Antenna: 2.4 GHz antenna clicked into the ESP32-32U's U.FL connector
             BEFORE powering on. Keep it away from metal and out of the radar's view.

  Serial Monitor: 115200 baud.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <MyLD2410.h>
#include "secrets.h"

// ---------- Wi-Fi (2.4 GHz only) ----------
// "Dartmouth Public" is open: leave the password empty.
// For a phone hotspot, put its name and password here instead.
const char* WIFI_SSID     = "Dartmouth Public";
const char* WIFI_PASSWORD = "";

// ---------- Backend ----------
// Testing with laptop + ESP32 on the same phone hotspot:
//   "http://<your laptop's IP on the hotspot>:8000/api/report"
// After deploying:
//   "https://<your-app>.onrender.com/api/report"
const char* REPORT_URL = "https://engs21-team28.onrender.com/api/report";
const char* DEVICE_KEY = "DEVICE_KEYS";   // must match DEVICE_KEYS on the server

#define SEAT_ID "seat-01"                        // give every sensor its own ID (letters, numbers, - and _)

#define HEARTBEAT_MS 30000UL   // re-send the current state every 30 s
#define RETRY_MS      5000UL   // after a failed send, try again this often

// ---------- pins ----------
#define RX_PIN 16
#define TX_PIN 17

// ---------- detection ----------
#define SEAT_RANGE_CM 50       // anyone closer than this is "at the seat"

// ---------- TEST timings ----------
#define CONFIRM_MS        2000UL   // must be near this long before OCCUPIED (ignores people walking past)
#define AWAY_DELAY_MS    10000UL   // gone this long before AWAY
#define SHORT_VISIT_MS   20000UL   // sat less than this -> skip AWAY, go FREE
#define AWAY_TIMEOUT_MS  60000UL   // AWAY this long -> FREE

// ---------- REAL timings (use these for Feldberg) ----------
// #define CONFIRM_MS        3000UL          // 3 s
// #define AWAY_DELAY_MS    15000UL          // 15 s
// #define SHORT_VISIT_MS  120000UL          // 2 min
// #define AWAY_TIMEOUT_MS 900000UL          // 15 min

HardwareSerial radarSerial(1);
MyLD2410 sensor(radarSerial);

WiFiClient plainClient;          // for http://
WiFiClientSecure secureClient;   // for https://

enum SeatState { FREE, OCCUPIED, AWAY };
SeatState state = FREE;

unsigned long nearSince = 0;       // when someone first came close (0 = not close)
unsigned long goneSince = 0;       // when nobody was close anymore (0 = someone close)
unsigned long occupiedSince = 0;   // when the seat turned OCCUPIED
unsigned long awaySince = 0;       // when the seat turned AWAY
unsigned long lastPrint = 0;
unsigned long lastWifiAttempt = 0;
bool wifiWasConnected = false;
bool lastNear = false;

// report queue: only the latest state matters, so one pending slot is enough
bool pendingSend = false;
const char* pendingReason = "boot";
unsigned long lastSent = 0;        // last successful send
unsigned long lastAttempt = 0;
bool lastAttemptFailed = false;
int lastHttpCode = 0;

const char* stateCode() {
  if (state == FREE) return "FREE";
  if (state == OCCUPIED) return "OCCUPIED";
  return "AWAY";
}

// ---------- reporting ----------
void requestSend(const char* reason) {
  pendingSend = true;
  pendingReason = reason;
  lastAttemptFailed = false;       // a new state change goes out right away
}

bool sendReport(const char* reason) {
  int movingCm = sensor.movingTargetDetected()     ? (int)sensor.movingTargetDistance()     : -1;
  int stillCm  = sensor.stationaryTargetDetected() ? (int)sensor.stationaryTargetDistance() : -1;

  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"seat\":\"%s\",\"state\":\"%s\",\"near\":%s,\"moving_cm\":%d,\"still_cm\":%d,"
           "\"rssi\":%d,\"reason\":\"%s\",\"uptime_s\":%lu}",
           SEAT_ID, stateCode(), lastNear ? "true" : "false", movingCm, stillCm,
           (int)WiFi.RSSI(), reason, millis() / 1000);

  HTTPClient http;
  bool isHttps = strncmp(REPORT_URL, "https", 5) == 0;
  bool started = isHttps ? http.begin(secureClient, REPORT_URL)
                         : http.begin(plainClient, REPORT_URL);
  if (!started) {
    Serial.println("Send FAILED: REPORT_URL looks wrong.");
    lastHttpCode = -1;
    return false;
  }

  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY);

  int code = http.POST((uint8_t*)payload, strlen(payload));
  http.end();
  lastHttpCode = code;

  if (code >= 200 && code < 300) {
    Serial.print("Sent -> ");
    Serial.println(payload);
    return true;
  }

  Serial.print("Send FAILED (");
  if (code == 401)      Serial.print("401: server rejected DEVICE_KEY");
  else if (code == 422) Serial.print("422: server rejected the data");
  else if (code < 0)    { Serial.print("can't reach server: "); Serial.print(http.errorToString(code)); }
  else                  { Serial.print("HTTP "); Serial.print(code); }
  Serial.println("), retrying in 5 s");
  return false;
}

// Called every loop: sends queued reports and heartbeats, with retry
void serviceReports() {
  unsigned long now = millis();

  if (!pendingSend && lastSent != 0 && now - lastSent >= HEARTBEAT_MS) {
    pendingSend = true;
    pendingReason = "heartbeat";
  }

  if (!pendingSend || WiFi.status() != WL_CONNECTED) return;
  if (lastAttemptFailed && now - lastAttempt < RETRY_MS) return;

  lastAttempt = now;
  if (sendReport(pendingReason)) {
    pendingSend = false;
    lastAttemptFailed = false;
    lastSent = millis();
  } else {
    lastAttemptFailed = true;
  }
}

// ---------- Wi-Fi ----------
void maintainWifi() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasConnected) {
      Serial.println("Wi-Fi lost.");
      wifiWasConnected = false;
    }
    if (now - lastWifiAttempt >= 10000) {
      lastWifiAttempt = now;
      Serial.println("Waiting for Wi-Fi...");
      WiFi.reconnect();
    }
    return;
  }

  if (!wifiWasConnected) {
    wifiWasConnected = true;
    Serial.print("Wi-Fi connected, IP: ");
    Serial.println(WiFi.localIP());
  }
}

// ---------- seat logic ----------
void setState(SeatState s) {
  state = s;
  Serial.print(">>> now ");
  Serial.println(stateCode());
  requestSend("change");
}

bool someoneNear() {
  bool nearMoving = sensor.movingTargetDetected() &&
                    sensor.movingTargetDistance() <= SEAT_RANGE_CM;
  bool nearStill  = sensor.stationaryTargetDetected() &&
                    sensor.stationaryTargetDistance() <= SEAT_RANGE_CM;
  return nearMoving || nearStill;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // bigger buffer so radar data isn't lost while a report is being sent
  radarSerial.setRxBufferSize(1024);
  radarSerial.begin(LD2410_BAUD_RATE, SERIAL_8N1, RX_PIN, TX_PIN);
  delay(500);
  if (!sensor.begin()) {
    Serial.println("Could not talk to the radar. Check the wiring.");
    while (true) { delay(1000); }
  }

  // For https:// this skips checking the server's certificate. Fine for a
  // student project; for extra security, load the server's root certificate
  // with secureClient.setCACert(...) instead.
  secureClient.setInsecure();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (strlen(WIFI_PASSWORD) == 0) WiFi.begin(WIFI_SSID);           // open network
  else                            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiAttempt = millis();
  Serial.print("Connecting to Wi-Fi \"");
  Serial.print(WIFI_SSID);
  Serial.println("\"...");

  Serial.print("Reporting to ");
  Serial.println(REPORT_URL);
  Serial.println("SeatNav running.");

  state = FREE;
  requestSend("boot");             // sent as soon as Wi-Fi connects
}

void loop() {
  sensor.check();
  maintainWifi();

  unsigned long now = millis();
  bool near = someoneNear();
  lastNear = near;

  // keep track of how long someone has been near / gone
  if (near) {
    if (nearSince == 0) nearSince = now;
    goneSince = 0;
  } else {
    if (goneSince == 0) goneSince = now;
    nearSince = 0;
  }

  switch (state) {
    case FREE:
      if (near && now - nearSince >= CONFIRM_MS) {
        occupiedSince = now;
        setState(OCCUPIED);
      }
      break;

    case OCCUPIED:
      if (!near && now - goneSince >= AWAY_DELAY_MS) {
        if (now - occupiedSince < SHORT_VISIT_MS) {
          setState(FREE);          // short visit: no need to hold the seat
        } else {
          awaySince = now;
          setState(AWAY);
        }
      }
      break;

    case AWAY:
      if (near && now - nearSince >= CONFIRM_MS) {
        setState(OCCUPIED);        // they came back (keep original occupiedSince)
      } else if (now - awaySince >= AWAY_TIMEOUT_MS) {
        setState(FREE);
      }
      break;
  }

  serviceReports();

  // status line once per second
  if (millis() - lastPrint >= 1000) {
    lastPrint = millis();
    Serial.print(stateCode());
    Serial.print("   near: ");
    Serial.print(near ? "yes" : "no ");
    Serial.print("   (moving ");
    if (sensor.movingTargetDetected()) { Serial.print(sensor.movingTargetDistance()); Serial.print(" cm"); }
    else Serial.print("-");
    Serial.print(", still ");
    if (sensor.stationaryTargetDetected()) { Serial.print(sensor.stationaryTargetDistance()); Serial.print(" cm"); }
    else Serial.print("-");
    Serial.print(")   wifi: ");
    Serial.print(WiFi.status() == WL_CONNECTED ? "ok" : "--");
    Serial.print("  server: ");
    Serial.print(lastSent != 0 && !lastAttemptFailed ? "ok" : "--");
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("  rssi: ");
      Serial.print(WiFi.RSSI());
      Serial.print(" dBm");
    }
    Serial.println();
  }
}
