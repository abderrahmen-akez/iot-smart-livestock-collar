/*
 * ============================================================================
 * Collier IoT de Surveillance pour Bétail Intelligent
 * Smart Livestock Tracking & Health Monitoring Collar
 * ----------------------------------------------------------------------------
 * Microcontrôleur : ESP32-WROOM-32 (simulation Wokwi)
 * Protocole       : MQTT publish/subscribe (broker public EMQX) — JSON over TCP
 * Capteurs        : DS18B20 (température 1-Wire), potentiomètre ADC (BPM)
 *                   [le module GPS NEO-6M n'existe pas dans Wokwi :
 *                    les coordonnées sont simulées par dérive aléatoire]
 * Actionneurs     : LED + buzzer (alertes géofence / santé), OLED SSD1306
 * ============================================================================
 *
 * Architecture MQTT (publish/subscribe, Ch.2) :
 *
 *   [Collier ESP32] --PUB--> tekup/farm/data          (télémétrie JSON, 15 s)
 *                     --> tekup/farm/status         (état global, 15 s)
 *                     --> tekup/farm/alerts/*       (alertes immédiates)
 *   [Collier ESP32] <--SUB--- tekup/farm/cmd/COW_01 (commandes distantes)
 *
 * Sécurité (Ch.4) : authentification par token MQTT, checksum d'intégrité
 * dans le payload, horodatage anti-replay. Le chiffrement TLS (port 8883)
 * et l'échange de clés ECDH sont documentés dans le README (contraintes
 * du broker public de test).
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>

/* ----------------------------- Wi-Fi (Wokwi) ----------------------------- */
/* Sur Wokwi, réseau invité : "Wokwi-GUEST" (sans mot de passe).           */
/* Sur matériel réel : renseigner le SSID/mot de passe du ranch.           */
const char* WIFI_SSID     = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

/* ----------------------------- Broker MQTT ------------------------------- */
/* broker.emqx.io : broker public de TEST (stable). À remplacer en          */
/* production par un broker privé (Mosquitto auto-hébergé par exemple).     */
const char* MQTT_HOST       = "broker.emqx.io";
const uint16_t MQTT_PORT    = 1883;   // 8883 en production (MQTT over TLS)
const char* MQTT_USER       = "";     // <- AUTH : identifiant
const char* MQTT_TOKEN      = "";  // <- AUTH : token secret
const char* MQTT_CLIENT_ID  = "collier-COW_01"; // <- ID: exemple

/* Préfixe de topics propre au projet (évite les collisions sur le broker public partagé).                                                         */
const char* TOPIC_DATA          = "tekup/farm/data";            // PUB
const char* TOPIC_STATUS        = "tekup/farm/status";          // PUB
const char* TOPIC_ALERT_IMM     = "tekup/farm/alerts/immediate";// PUB
const char* TOPIC_ALERT_GEO     = "tekup/farm/alerts/geofence"; // PUB
const char* TOPIC_ALERT_HEALTH  = "tekup/farm/alerts/health";   // PUB
const char* TOPIC_CMD           = "tekup/farm/cmd/COW_01";      // SUB

const char* COW_ID = "COW_01";

/* ------------------------------- Brochage -------------------------------- */
#define PIN_ONEWIRE   4     // DS18B20 — bus 1-Wire
#define PIN_ADC_BPM   34    // Potentiomètre — ADC 12 bits (simule MAX30102)
#define PIN_LED       18    // LED rouge — alerte visuelle
#define PIN_BUZZER    19    // Buzzer — alerte sonore
#define OLED_SDA      21    // OLED SSD1306 — I2C SDA
#define OLED_SCL      22    // OLED SSD1306 — I2C SCL
#define OLED_ADDR     0x3C

/* ------------------------- Géofence (pâturage) --------------------------- */
/* Zone rectangulaire autour d'un point central (région de Béja, Tunisie).  */
const float BASE_LAT  = 36.80f;
const float BASE_LON  = 9.20f;
const float GF_LAT_MIN = BASE_LAT - 0.003f;   // ~ 300 m
const float GF_LAT_MAX = BASE_LAT + 0.003f;
const float GF_LON_MIN = BASE_LON - 0.003f;
const float GF_LON_MAX = BASE_LON + 0.003f;

/* --------------------------- Seuils de santé ----------------------------- */
const float TEMP_MIN = 38.0f;    // °C — frissons / fièvre du bétail
const float TEMP_MAX = 39.5f;
const int   BPM_MIN  = 45;
const int   BPM_MAX  = 100;

const unsigned long PUBLISH_INTERVAL = 15000UL;   // période télémétrie : 15 s
const unsigned long NTP_SYNC_TIMEOUT = 10000UL;

/* ------------------------------ Objets ----------------------------------- */
OneWire            oneWire(PIN_ONEWIRE);
DallasTemperature  sensors(&oneWire);
Adafruit_SSD1306   display(128, 64, &Wire, -1);
WiFiClient         wifiClient;
PubSubClient       mqtt(wifiClient);

/* --------------------------- Données du collier -------------------------- */
struct CollarData {
  float    lat, lon;      // position (simulée sous Wokwi)
  float    temp;          // °C — DS18B20
  int      bpm;           // battements/min — ADC
  bool     inGeofence;
  bool     tempOk, bpmOk;
  char     timestamp[25]; // ISO 8601 — anti-replay
  uint8_t  crc;           // checksum d'intégrité
};

unsigned long lastPublish = 0;
unsigned long lastAlert   = 0;
bool          alertActive = false;

/* ========================================================================= */
/*  Utilitaires                                                              */
/* ========================================================================= */

/* Checksum 8 bits (somme pondérée des champs du payload).
 * Simule le concept d'HMAC (Ch.4) : détection de corruption/altération.    */
uint8_t computeChecksum(const char* id, float lat, float lon, float temp,
                        int bpm, const char* ts) {
  uint32_t acc = 0;
  while (*id)   acc += *id++;
  while (*ts)   acc += *ts++;
  acc += (int32_t)(lat  * 10000);
  acc += (int32_t)(lon  * 10000);
  acc += (int32_t)(temp * 100);
  acc += bpm;
  return (uint8_t)(acc & 0xFF);
}

/* GPS simulé : dérive aléatoire autour du point central (±30 m par cycle),
 * reproduisant le bruit réel d'un déplacement de bovin en pâturage.
 * Sur matériel réel : remplacer par TinyGPS++ sur le NEO-6M (UART2).       */
void simulateGps(CollarData& d) {
  d.lat = BASE_LAT + random(-3, 4) / 10000.0f;
  d.lon = BASE_LON + random(-3, 4) / 10000.0f;
  d.inGeofence = (d.lat >= GF_LAT_MIN && d.lat <= GF_LAT_MAX &&
                  d.lon >= GF_LON_MIN && d.lon <= GF_LON_MAX);
}

void readSensors(CollarData& d) {
  sensors.requestTemperatures();
  d.temp = sensors.getTempCByIndex(0);
  if (d.temp < -40) d.temp = 38.5f;   // capteur absent -> valeur de secours

  int raw = analogRead(PIN_ADC_BPM);          // 0 .. 4095 (ADC 12 bits)
  d.bpm   = map(raw, 0, 4095, 40, 120);       // plage physiologique bovine

  d.tempOk = (d.temp >= TEMP_MIN && d.temp <= TEMP_MAX);
  d.bpmOk  = (d.bpm  >= BPM_MIN  && d.bpm  <= BPM_MAX);
}

/* Horodatage ISO 8601 (UTC) via NTP — sert de nonce anti-replay (Ch.3/4).  */
void buildTimestamp(char* out, size_t len) {
  time_t now = time(nullptr);
  if (now < 100000) {                          // NTP pas encore synchronisé
    snprintf(out, len, "BOOT+%lus", (unsigned long)(millis() / 1000));
    return;
  }
  struct tm* t = gmtime(&now);
  strftime(out, len, "%Y-%m-%dT%H:%M:%SZ", t);
}

/* ========================================================================= */
/*  Affichage OLED (dashboard local)                                         */
/* ========================================================================= */
void refreshDisplay(const CollarData& d, const char* mqttState) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(F("Collier ")); display.println(COW_ID);
  display.print(F("MQTT : "));  display.println(mqttState);
  display.print(F("Lat : "));   display.print(d.lat, 4);
  display.print(F("  [GPS SIMULE]\nLon : ")); display.println(d.lon, 4);
  display.print(F("Temp : "));  display.print(d.temp, 1); display.print(F(" C  "));
  display.print(F("BPM : "));   display.println(d.bpm);
  display.print(F("Geofence : "));
  display.println(d.inGeofence ? F("OK") : F("HORS ZONE !"));
  display.display();
}

/* ========================================================================= */
/*  Publication MQTT                                                         */
/* ========================================================================= */
void publishTelemetry(const CollarData& d) {
  char ts[25];
  buildTimestamp(ts, sizeof(ts));
  uint8_t crc = computeChecksum(COW_ID, d.lat, d.lon, d.temp, d.bpm, ts);

  /* Payload JSON compact (Ch.2 : faible overhead — en-tête MQTT = 2 octets) */
  StaticJsonDocument<256> doc;
  doc["id"]   = COW_ID;
  doc["lat"]  = d.lat;
  doc["lon"]  = d.lon;
  doc["temp"] = d.temp;
  doc["bpm"]  = d.bpm;
  doc["gf"]   = d.inGeofence;
  doc["t_ok"] = d.tempOk;
  doc["b_ok"] = d.bpmOk;
  doc["ts"]   = ts;        // anti-replay
  doc["crc"]  = crc;       // intégrité
  char payload[256];
  serializeJson(doc, payload);
  mqtt.publish(TOPIC_DATA, payload);

  /* Statut global agrégé */
  StaticJsonDocument<256> st;
  st["id"] = COW_ID;
  st["globalStatus"] = (d.inGeofence && d.tempOk && d.bpmOk) ? "OK" : "ALERT";
  st["healthScore"]  = (d.tempOk ? 50 : 0) + (d.bpmOk ? 50 : 0);
  JsonArray alerts = st.createNestedArray("activeAlerts");
  if (!d.inGeofence) alerts.add("GEOFENCE");
  if (!d.tempOk)     alerts.add("TEMP_ANOMALY");
  if (!d.bpmOk)      alerts.add("CARDIO_ANOMALY");
  char payload2[256];
  serializeJson(st, payload2);
  mqtt.publish(TOPIC_STATUS, payload2);
}

/* Alerte unifiée : publie sur immediate/ + le topic spécifique (santé ou     */
/* géofence), et déclenche les actionneurs locaux.                            */
void raiseAlert(CollarData& d, const char* type, const char* severity,
                const char* desc, float value, const char* unit) {
  char ts[25];
  buildTimestamp(ts, sizeof(ts));

  StaticJsonDocument<256> doc;
  doc["id"] = COW_ID;
  doc["type"] = type;
  doc["severity"] = severity;
  doc["desc"] = desc;
  doc["value"] = value;
  doc["unit"] = unit;
  doc["location"]["lat"] = d.lat;
  doc["location"]["lon"] = d.lon;
  doc["ts"] = ts;
  char payload[256];
  serializeJson(doc, payload);

  mqtt.publish(TOPIC_ALERT_IMM, payload);
  if (strcmp(type, "GEOFENCE") == 0)
    mqtt.publish(TOPIC_ALERT_GEO, payload);
  else
    mqtt.publish(TOPIC_ALERT_HEALTH, payload);

  /* Actionneurs locaux (Ch.1 : couche perception/action) */
  digitalWrite(PIN_LED, HIGH);
  tone(PIN_BUZZER, 2000, 800);
  alertActive = true;
}

void clearAlert() {
  digitalWrite(PIN_LED, LOW);
  noTone(PIN_BUZZER);
  alertActive = false;
}

/* ========================================================================= */
/*  MQTT : callback subscribe (commandes distantes)                          */
/* ========================================================================= */
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, payload, length)) return;   // JSON invalide

  const char* alert = doc["alert"];
  if (alert && strcmp(alert, "ON") == 0) {
    digitalWrite(PIN_LED, HIGH);
    tone(PIN_BUZZER, 2000, 800);       // commande distante : localiser l'animal
  } else if (alert && strcmp(alert, "OFF") == 0) {
    clearAlert();
  }
}

/* ========================================================================= */
/*  Connexions                                                               */
/* ========================================================================= */
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print(F("[WiFi] Connexion"));
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(400);
  }
  Serial.println(F(" OK"));
  Serial.print(F("[WiFi] IP : "));
  Serial.println(WiFi.localIP());
}

void connectMqtt() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  while (!mqtt.connected()) {
    Serial.print(F("[MQTT] Connexion au broker..."));
    /* Authentification identifiant + token (Ch.4 : contrôle d'accès) */
    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_TOKEN)) {
      Serial.println(F(" OK"));
      mqtt.subscribe(TOPIC_CMD);       // attente des commandes distantes
      Serial.print(F("[MQTT] Subscribe : "));
      Serial.println(TOPIC_CMD);
    } else {
      Serial.print(F(" echec (rc="));
      Serial.print(mqtt.state());
      Serial.println(F(") — nouvelle tentative dans 5 s"));
      delay(5000);
    }
  }
}

/* ========================================================================= */
/*  Setup & boucle principale                                                */
/* ========================================================================= */
void setup() {
  Serial.begin(115200);
  Serial.println(F("=== Collier IoT de Surveillance pour Bétail ==="));

  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_ADC_BPM, INPUT);

  sensors.begin();

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
    Serial.println(F("[OLED] Echec initialisation SSD1306"));

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");   // NTP (anti-replay)

  connectWiFi();
  connectMqtt();

  randomSeed(esp_random());   // départ aléatoire du bruit GPS
}

void loop() {
  if (!mqtt.connected()) connectMqtt();
  mqtt.loop();

  CollarData d;
  simulateGps(d);
  readSensors(d);

  unsigned long now = millis();

  /* --- Publication périodique (15 s) --- */
  if (now - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = now;
    publishTelemetry(d);
    Serial.println(F("[MQTT] Télémétrie publiée sur tekup/farm/data"));
  }

  /* --- Détection et signalement des alertes --- */
  bool anyAlert = (!d.inGeofence || !d.tempOk || !d.bpmOk);
  if (anyAlert && now - lastAlert >= 30000) {   // anti-spam : 30 s mini
    lastAlert = now;
    if (!d.inGeofence)
      raiseAlert(d, "GEOFENCE", "HIGH", "Animal hors zone de pâturage",
                 0, "");
    if (!d.tempOk)
      raiseAlert(d, "TEMP_ANOMALY", "HIGH",
                 "Température anormale", d.temp, "C");
    if (!d.bpmOk)
      raiseAlert(d, "CARDIO_ANOMALY", "MEDIUM",
                 "Rythme cardiaque anormal", (float)d.bpm, "bpm");
  } else if (!anyAlert && alertActive) {
    clearAlert();
  }

  refreshDisplay(d, mqtt.connected() ? "connecte" : "deconnecte");
  delay(500);
}
