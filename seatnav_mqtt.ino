/*
  SeatNav: radar + Wi-Fi broadcast (MQTT)

  Same seat logic as seatnav_single_led, but instead of driving an LED,
  every state change is sent over Wi-Fi to an MQTT broker. Your app
  subscribes to the broker and shows the seat's state.

  States (sent as text in the "state" field):
    OCCUPIED = someone is sitting here            (was: LED solid on)
    AWAY     = person stepped away, may come back (was: slow blink)
    FREE     = seat free                          (was: fast blink)

  How it decides:
    - Someone within SEAT_RANGE_CM for CONFIRM_MS        -> OCCUPIED
    - Nobody close for AWAY_DELAY_MS while OCCUPIED      -> AWAY
        (but if they only sat for less than SHORT_VISIT_MS, go straight to FREE)
    - Back within range while AWAY                       -> OCCUPIED
    - Still nobody after AWAY_TIMEOUT_MS while AWAY      -> FREE

  What gets sent:
    Topic <TOPIC_BASE>/state  (retained, so a new subscriber gets the current state instantly)
      {"seat":"seat-01","state":"OCCUPIED","near":true,"moving_cm":-1,"still_cm":42,
       "rssi":-58,"reason":"change","uptime_s":123}
      - rssi = Wi-Fi signal strength in dBm (-50..-67 good, below -80 expect dropouts)
      - sent immediately on every state change ("reason":"change")
      - repeated every HEARTBEAT_MS so the app knows the seat is still alive ("reason":"heartbeat")
    Topic <TOPIC_BASE>/status (retained)
      "online" when the ESP32 connects, "offline" sent automatically by the
      broker if the ESP32 loses power or Wi-Fi (MQTT "last will").

  Needs the PubSubClient library (by Nick O'Leary) in addition to MyLD2410.

  Wiring:
    Radar:   VCC -> 5V, GND -> GND, TX -> GPIO 16, RX -> GPIO 17
    Antenna: 2.4 GHz antenna clicked into the ESP32-32U's U.FL connector
             BEFORE powering on. Keep it away from metal and out of the radar's view.

  Serial Monitor: 115200 baud.
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <MyLD2410.h>

// ---------- Wi-Fi (2.4 GHz only) ----------
// Default: "Dartmouth Public" (open network, no password, no registration).
// For a phone hotspot, put its name and password below instead.
// Uncomment USE_EDUROAM only for a PEAP/TTLS eduroam login (Dartmouth's eduroam uses certificates, so it won't work there).
// #define USE_EDUROAM

#ifdef USE_EDUROAM
  // eduroam / WPA2-Enterprise. Check your institution's eduroam setup page for the method
  // (most use PEAP; change to WPA2_AUTH_TTLS if yours says TTLS).
  const char* WIFI_SSID         = "eduroam";
  #define     EAP_METHOD          WPA2_AUTH_PEAP
  const char* EAP_IDENTITY      = "anonymous@your-institution.edu"; // outer identity; if unsure, use the same as EAP_USERNAME
  const char* EAP_USERNAME      = "username@your-institution.edu";  // full username including @domain
  const char* EAP_PASSWORD      = "YOUR_EDUROAM_PASSWORD";
#else
  const char* WIFI_SSID         = "Dartmouth Public";
  const char* WIFI_PASSWORD     = "";        // leave empty for an open network
#endif

// ---------- MQTT ----------
// Free public test broker: anyone can read/write it, fine for testing only.
const char* MQTT_HOST = "broker.hivemq.com";
const int   MQTT_PORT = 1883;

#define SEAT_ID    "seat-01"                        // give every sensor its own ID
#define TOPIC_BASE "seatnav-feldberg-x7k2/seats/" SEAT_ID // make "seatnav-feldberg" unique to your team

const char* TOPIC_STATE  = TOPIC_BASE "/state";
const char* TOPIC_STATUS = TOPIC_BASE "/status";

#define HEARTBEAT_MS 30000UL   // re-send the current state every 30 s

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

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
char clientId[40];

enum SeatState { FREE, OCCUPIED, AWAY };
SeatState state = FREE;

unsigned long nearSince = 0;       // when someone first came close (0 = not close)
unsigned long goneSince = 0;       // when nobody was close anymore (0 = someone close)
unsigned long occupiedSince = 0;   // when the seat turned OCCUPIED
unsigned long awaySince = 0;       // when the seat turned AWAY
unsigned long lastPrint = 0;
unsigned long lastHeartbeat = 0;
unsigned long lastWifiAttempt = 0;
unsigned long lastMqttAttempt = 0;
bool wifiWasConnected = false;
bool pendingPublish = false;       // a state change happened while offline
bool lastNear = false;

const char* stateCode() {
  if (state == FREE) return "FREE";
  if (state == OCCUPIED) return "OCCUPIED";
  return "AWAY";
}

// ---------- broadcasting ----------
void publishState(const char* reason) {
  if (!mqtt.connected()) {
    pendingPublish = true;         // send it as soon as we're back online
    return;
  }

  int movingCm = sensor.movingTargetDetected()     ? (int)sensor.movingTargetDistance()     : -1;
  int stillCm  = sensor.stationaryTargetDetected() ? (int)sensor.stationaryTargetDistance() : -1;

  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"seat\":\"%s\",\"state\":\"%s\",\"near\":%s,\"moving_cm\":%d,\"still_cm\":%d,"
           "\"rssi\":%d,\"reason\":\"%s\",\"uptime_s\":%lu}",
           SEAT_ID, stateCode(), lastNear ? "true" : "false", movingCm, stillCm,
           (int)WiFi.RSSI(), reason, millis() / 1000);

  bool ok = mqtt.publish(TOPIC_STATE, payload, true);   // true = retained
  Serial.print(ok ? "Published -> " : "Publish FAILED -> ");
  Serial.println(payload);

  if (ok) {
    pendingPublish = false;
    lastHeartbeat = millis();
  }
}

void connectMqtt() {
  Serial.print("Connecting to MQTT broker ");
  Serial.print(MQTT_HOST);
  Serial.print(" ... ");
  // last will: the broker publishes "offline" for us if we disappear
  if (mqtt.connect(clientId, TOPIC_STATUS, 1, true, "offline")) {
    Serial.println("connected.");
    mqtt.publish(TOPIC_STATUS, "online", true);
    publishState(pendingPublish ? "change" : "connected");
  } else {
    Serial.print("failed (code ");
    Serial.print(mqtt.state());
    Serial.println("), retrying in 5 s");
  }
}

// Keeps Wi-Fi and MQTT alive without blocking the radar logic for long
void maintainNetwork() {
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
    lastMqttAttempt = 0;           // try MQTT right away
  }

  if (!mqtt.connected()) {
    if (lastMqttAttempt == 0 || now - lastMqttAttempt >= 5000) {
      lastMqttAttempt = now;
      connectMqtt();
    }
    return;
  }

  mqtt.loop();
}

// ---------- seat logic ----------
void setState(SeatState s) {
  state = s;
  Serial.print(">>> now ");
  Serial.println(stateCode());
  publishState("change");
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

  radarSerial.begin(LD2410_BAUD_RATE, SERIAL_8N1, RX_PIN, TX_PIN);
  delay(500);
  if (!sensor.begin()) {
    Serial.println("Could not talk to the radar. Check the wiring.");
    while (true) { delay(1000); }
  }

  // unique client ID per board (two devices with the same ID kick each other off)
  uint32_t chip = (uint32_t)(ESP.getEfuseMac() >> 24);
  snprintf(clientId, sizeof(clientId), "seatnav-%s-%06lX", SEAT_ID, (unsigned long)(chip & 0xFFFFFF));

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
#ifdef USE_EDUROAM
  WiFi.begin(WIFI_SSID, EAP_METHOD, EAP_IDENTITY, EAP_USERNAME, EAP_PASSWORD);
#else
  if (strlen(WIFI_PASSWORD) == 0) WiFi.begin(WIFI_SSID);           // open network
  else                            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
#endif
  lastWifiAttempt = millis();
  Serial.print("Connecting to Wi-Fi \"");
  Serial.print(WIFI_SSID);
  Serial.println("\"...");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(3);

  Serial.println("SeatNav running.");
  setState(FREE);                  // queued until MQTT connects
}

void loop() {
  sensor.check();
  maintainNetwork();

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

  // heartbeat so the app can tell a quiet seat from a dead sensor
  if (mqtt.connected() && now - lastHeartbeat >= HEARTBEAT_MS) {
    publishState("heartbeat");
  }

  // status line once per second
  if (now - lastPrint >= 1000) {
    lastPrint = now;
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
    Serial.print("  mqtt: ");
    Serial.print(mqtt.connected() ? "ok" : "--");
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("  rssi: ");
      Serial.print(WiFi.RSSI());
      Serial.print(" dBm");
    }
    Serial.println();
  }
}
