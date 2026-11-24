# IoT Smart Livestock Collar — Collier IoT de Surveillance pour Bétail Intelligent

**Tracking + health monitoring collar for cattle, built on an ESP32 and an MQTT
publish/subscribe architecture.** 100% simulable on [Wokwi](https://wokwi.com) —
academic project for the *Sécurité de l'Internet des Objets* course (TEK-UP, ING-5 SSIR).

The collar periodically publishes geolocation and vital signs (body temperature,
heart rate) over **MQTT** to a cloud broker, raises **immediate alerts** on
geofence breaches or abnormal health metrics, and accepts **remote commands**
(subscribe channel) — the reference pattern for understanding decoupled
**publish/subscribe** IoT architectures.

---

## Architecture (3 IoT layers)

```
┌───────────────────── PERCEPTION ─────────────────────┐
│  DS18B20 (temp, 1-Wire)   Potentiometer (BPM, ADC)   │
│  GPS coords (simulated)   LED + Buzzer (actuators)   │
│  OLED SSD1306 (local dashboard, I2C)                 │
└───────────────────────┬──────────────────────────────┘
                        │ ESP32-WROOM-32 (Arduino / PlatformIO)
┌───────────────────────┴──────────── NETWORK ─────────┐
│  Wi-Fi (simulated) — in a real ranch: LoRaWAN/NB-IoT │
│  MQTT pub/sub — broker.emqx.io:1883 (public test)    │
└───────────────────────┬──────────────────────────────┘
┌───────────────────────┴─────── APPLICATION ──────────┐
│  Subscribers: MQTTx / Mosquitto / dashboard / farmer │
│  Publishers : any authorized client (cmd channel)    │
└──────────────────────────────────────────────────────┘
```

**Why MQTT and not HTTP (course Ch.2)?** MQTT adds a fixed 2-byte header per
message (vs 800+ bytes of HTTP overhead), uses a persistent TCP connection,
supports hierarchical topics and QoS levels, and natively decouples producers
and consumers through the broker — decisive for constrained, battery-powered
agricultural devices.

## Hardware (Wokwi-simulated)

| Component        | ESP32 pin        | Protocol | Role                                  |
|------------------|------------------|----------|---------------------------------------|
| ESP32-WROOM-32   | —                | Wi-Fi    | Microcontroller (dual-core)           |
| DS18B20          | GPIO 4           | 1-Wire   | Body temperature                      |
| Potentiometer    | GPIO 34          | ADC 12-bit | Simulates MAX30102 heart rate (BPM)   |
| Red LED          | GPIO 18 (220 Ω)  | GPIO out | Visual geofence/health alert          |
| Buzzer           | GPIO 19          | GPIO out | Audible alert                         |
| OLED SSD1306     | GPIO 21/22       | I2C 0x3C | Local dashboard (ID, GPS, temp, BPM)  |

> **Note:** Wokwi's library has **no GPS module (NEO-6M)** — coordinates are
> *simulated in software* (random walk ±30 m/cycle around the ranch center,
> Béja region, Tunisia). The geofence logic is fully functional. On real
> hardware, replace `simulateGps()` with TinyGPS++ over UART. The OLED
> displays `[GPS SIMULE]` as a reminder.

## MQTT topics (prefix `tekup/farm/` avoids collisions on the public broker)

| Topic                       | Direction | Content                                                        | Frequency        |
|-----------------------------|-----------|----------------------------------------------------------------|------------------|
| `tekup/farm/data`           | PUB       | Raw telemetry JSON (`lat, lon, temp, bpm, gf, t_ok, b_ok, ts, crc`) | Every 15 s       |
| `tekup/farm/status`         | PUB       | Global state JSON (`globalStatus, healthScore, activeAlerts[]`)   | Every 15 s       |
| `tekup/farm/alerts/immediate` | PUB     | Unified instant alert (`type, severity, desc, value, unit, location, ts`) | On event + 30 s while persistent |
| `tekup/farm/alerts/geofence`  | PUB     | Geofence-only filtered alerts                                  | Mirror of above  |
| `tekup/farm/alerts/health`    | PUB     | Temperature/BPM-only filtered alerts                           | Mirror of above  |
| `tekup/farm/cmd/COW_01`     | **SUB**   | Remote commands (`{"alert":"ON"}` / `{"alert":"OFF"}`)    | On reception     |

## Payload examples

**Telemetry — `tekup/farm/data`:**
```json
{
  "id": "COW_01", "lat": 36.8001, "lon": 9.2003,
  "temp": 38.5, "bpm": 65,
  "gf": true, "t_ok": true, "b_ok": true,
  "ts": "2026-05-21T13:45:00Z", "crc": 183
}
```

**Unified alert — `tekup/farm/alerts/immediate`:**
```json
{
  "id": "COW_01", "type": "TEMP_ANOMALY", "severity": "HIGH",
  "desc": "Température anormale", "value": 40.2, "unit": "C",
  "location": { "lat": 36.8001, "lon": 9.2003 },
  "ts": "2026-05-21T13:45:00Z"
}
```

## Security analysis (course Ch.3/Ch.4)

| Threat                    | Layer    | Impact                              | Countermeasure implemented / documented                          |
|---------------------------|----------|-------------------------------------|-------------------------------------------------------------------|
| Eavesdropping (MQTT sniffing) | Network | GPS position & herd data leakage   | **TLS on port 8883** in production (documented; public test broker is plaintext); ECDH/ECC key-exchange concept |
| Replay attack             | Network  | Old GPS frames re-injected          | **ISO-8601 timestamp** in every payload acts as a nonce — consumers reject stale frames |
| GPS spoofing              | Perception | False location, geofence bypass  | Bounding-box **geofence** anomaly detection; hardened GPS input validation on real hardware |
| Broker impersonation      | Network  | Command injection (`cmd` topic)     | **MQTT authentication** (username + token, Blynk-style); unique client ID |
| Payload tampering         | Application | Corrupted telemetry             | **8-bit checksum** (`crc` field) simulating the HMAC integrity concept |

> Production hardening: private Mosquitto broker, TLS + client certificates
> (PKI), per-device credentials, ACLs restricting `cmd/*` to authorized
> operators.

## Features

- **Geofencing** — hardcoded bounding box (~300 m around the pasture center);
  breach triggers a local LED/buzzer alarm + instant MQTT alert.
- **3 alert types** — geofence breach, abnormal temperature (< 38 °C or
  > 39.5 °C), abnormal heart rate (< 45 or > 100 BPM), with anti-spam
  throttling (30 s) and severity levels.
- **Remote commands** — subscribe to `tekup/farm/cmd/COW_01` to remotely
  trigger/clear the local locator alarm.
- **Local dashboard** — OLED shows collar ID, MQTT state, coordinates,
  temperature, BPM and geofence status.

## Getting started (Wokwi, < 5 minutes)

1. Create a new project on [wokwi.com](https://wokwi.com) and pick the **first
   "ESP32"** in the list — that *is* the ESP32-WROOM-32.
2. Add the 7 components from the table above (or open `wokwi/diagram.json`
   and let Wokwi rebuild the wiring automatically).
3. Paste `livestock_collar.ino` as `sketch.ino`.
4. Libraries: `PubSubClient`, `ArduinoJson`, `OneWire`, `DallasTemperature`,
   `Adafruit SSD1306`, `Adafruit GFX` (Wokwi resolves them automatically).
5. Start the simulation — telemetry appears on the serial monitor after Wi-Fi
   and broker connection.

**Verify published messages with MQTTx:**
1. Open [MQTTx Web](https://mqttx.app/web-client) (or the MQTTx desktop app).
2. Connect to `broker.emqx.io`, port `1883` (no credentials for reading).
   > HiveMQ's public broker (`broker.hivemq.com`) is permanently overloaded
   > since late 2024 — EMQX is the stable public alternative.
3. Subscribe to `tekup/farm/#` to see all traffic.
4. Publish `{"alert":"ON"}` to `tekup/farm/cmd/COW_01` to trigger the collar's
   locator alarm remotely.

## Course mapping (TEK-UP — Sécurité de l'Internet des Objets)

| Chapter | Concepts demonstrated by this project |
|---------|----------------------------------------|
| Ch.1 — IoT foundations     | 3-layer architecture, M2M, smart agriculture domain, I2C/1-Wire/UART buses |
| Ch.2 — IoT protocols       | MQTT vs CoAP/REST, 2-byte overhead, hierarchical topics, QoS, star topology, Wi-Fi vs LoRaWAN/NB-IoT trade-offs |
| Ch.3 — Threats by layer    | Eavesdropping, replay, spoofing (documented per layer) |
| Ch.4 — Countermeasures     | Authentication, encryption (TLS), integrity (checksum/HMAC concept), anti-replay timestamps, PKI/certificate concept |

## Files

| File                  | Description                                        |
|-----------------------|----------------------------------------------------|
| `livestock_collar.ino` | Firmware — ESP32 Arduino/PlatformIO, MQTT pub/sub  |
| `wokwi/diagram.json`   | Wokwi wiring (7 components, auto-loadable)         |

## Disclaimer

Isolated academic lab. The public EMQX broker is a shared test infrastructure:
never publish real farm data, real credentials or personal information to it.
All identifiers, tokens and coordinates are fictional.
