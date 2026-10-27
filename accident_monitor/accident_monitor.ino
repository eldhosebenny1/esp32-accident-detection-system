#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <TinyGPS++.h>
#include <math.h>

const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

WebServer server(80);
bool webServerStarted = false;
bool wifiWasConnected = false;
unsigned long lastWiFiRetry = 0;

#define MPU_ADDR       0x68
#define MPU_PWR        0x6B
#define MPU_ACCEL_CFG  0x1C
#define MPU_ACCEL_X    0x3B

HardwareSerial GPS(1);
TinyGPSPlus gps;
#define GPS_TX 16
#define GPS_RX 17

#define TRIG_PIN   2
#define ECHO_PIN   34
#define BRAKE_SERVO 32
#define BUZZER     13
#define RED_LED    12
#define SOS_BUTTON 4
#define SAFE_BUTTON 5

Servo brake;

const int BRAKE_RELEASE = 0;
const int BRAKE_MAX = 110;
int brakePosition = BRAKE_RELEASE;

const float IMPACT_G = 4.0;
bool accidentCandidate = false;
bool accidentConfirmed = false;
bool manualSOS = false;
unsigned long accidentStart = 0;
const unsigned long CONFIRM_TIME = 20000;

const float DEMO_LAT = 11.2588;
const float DEMO_LON = 75.7804;

struct Hospital {
  String name;
  float lat;
  float lon;
};

Hospital hospitals[3];

float incidentLat = DEMO_LAT;
float incidentLon = DEMO_LON;
float nearestHospitalDistanceKm = 0.0;
int nearestHospitalIndex = 0;
bool incidentUsesDemoLocation = true;
bool incidentDetailsAvailable = false;
String incidentMapLink = "";
String incidentHospitalName = "Not calculated";

float currentImpactG = 1.0;
float peakImpactG = 0.0;
unsigned long lastImpactPrint = 0;
unsigned long lastHistorySample = 0;
const int GRAPH_POINTS = 60;
float impactHistory[GRAPH_POINTS];
int impactHistoryIndex = 0;
int impactHistoryCount = 0;

String lastEvent = "System started";
String incidentStatus = "NO ACTIVE INCIDENT";
unsigned long incidentNumber = 0;
unsigned long lastGPSPrint = 0;
unsigned long lastSOSPress = 0;
unsigned long lastSafePress = 0;
const unsigned long BUTTON_DELAY = 500;

void writeMPU(byte reg, byte value);
int16_t readMPU16(byte reg);
void readGPS();
void checkImpact();
void startAccident();
void checkConfirmation();
void checkSafeButton();
void checkSOSButton();
void obstacleControl();
float getDistance();
void smoothBrakeTo(int target);
void fastBrake();
void releaseBrake();
void emergencyState();
void prepareIncidentReport();
void createDemoHospitals(float lat, float lon);
int findNearestHospital(float lat, float lon);
float distanceKm(float lat1, float lon1, float lat2, float lon2);
void maintainWiFi();
void handleRoot();
void handleData();
String jsonEscape(const String& value);

void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println();
  Serial.println("=================================");
  Serial.println(" SMART ACCIDENT SYSTEM - WEB UI");
  Serial.println("=================================");

  Wire.begin(21, 22);

  writeMPU(MPU_PWR, 0x00);
  writeMPU(MPU_ACCEL_CFG, 0x10);
  delay(100);
  Serial.println("MPU6050 initialized (check readings in Serial Monitor).");

  GPS.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("GPS serial started: RX=17, TX=16");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  pinMode(BUZZER, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  digitalWrite(BUZZER, LOW);
  digitalWrite(RED_LED, LOW);

  pinMode(SOS_BUTTON, INPUT_PULLUP);
  pinMode(SAFE_BUTTON, INPUT_PULLUP);

  brake.setPeriodHertz(50);
  brake.attach(BRAKE_SERVO, 500, 2400);
  brakePosition = BRAKE_RELEASE;
  brake.write(BRAKE_RELEASE);

  for (int i = 0; i < GRAPH_POINTS; i++) {
    impactHistory[i] = 1.0;
  }
  impactHistoryCount = GRAPH_POINTS;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to Wi-Fi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(300);
    Serial.print('.');
  }
  Serial.println();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/favicon.ico", HTTP_GET, []() {
    server.send(204, "text/plain", "");
  });
  server.begin();
  webServerStarted = true;
  Serial.println("Local web server started.");

  if (WiFi.status() == WL_CONNECTED) {
    wifiWasConnected = true;
    Serial.print("Dashboard URL: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi not connected yet; will retry automatically.");
  }

  Serial.println("Open the dashboard address in a browser on the same Wi-Fi.");
  Serial.println("Telegram sending has been removed; reports are shown in Web UI.");
}

void loop() {
  server.handleClient();
  maintainWiFi();
  readGPS();
  checkSOSButton();
  checkSafeButton();

  if (accidentConfirmed) {
    emergencyState();
    delay(10);
    return;
  }

  if (accidentCandidate) {
    checkConfirmation();
    delay(10);
    return;
  }

  obstacleControl();
  checkImpact();
  delay(20);
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.print("Wi-Fi restored. Dashboard URL: http://");
      Serial.println(WiFi.localIP());
    }
    return;
  }

  wifiWasConnected = false;
  if (millis() - lastWiFiRetry >= 10000) {
    lastWiFiRetry = millis();
    Serial.println("Wi-Fi disconnected; reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

void writeMPU(byte reg, byte value) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

int16_t readMPU16(byte reg) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)2) != 2) return 0;
  uint8_t hi = Wire.read();
  uint8_t lo = Wire.read();
  return (int16_t)(((uint16_t)hi << 8) | lo);
}

void readGPS() {
  while (GPS.available()) {
    gps.encode(GPS.read());
  }

  if (millis() - lastGPSPrint < 3000) return;
  lastGPSPrint = millis();

  Serial.println();
  Serial.println("========== GPS STATUS ==========");
  Serial.print("Satellites: ");
  Serial.println(gps.satellites.value());
  Serial.print("GPS fix: ");
  Serial.println(gps.location.isValid() && gps.location.age() < 10000 ? "YES" : "NO");

  if (gps.location.isValid()) {
    Serial.print("Latitude: ");
    Serial.println(gps.location.lat(), 6);
    Serial.print("Longitude: ");
    Serial.println(gps.location.lng(), 6);
    Serial.print("Location age (ms): ");
    Serial.println(gps.location.age());
  } else {
    Serial.println("Location not available yet; demo location will be used for an alert.");
  }
  Serial.print("Characters processed: ");
  Serial.println(gps.charsProcessed());
  Serial.println("================================");
}

void checkImpact() {
  int16_t rawX = readMPU16(MPU_ACCEL_X);
  int16_t rawY = readMPU16(MPU_ACCEL_X + 2);
  int16_t rawZ = readMPU16(MPU_ACCEL_X + 4);

  float x = rawX / 4096.0;
  float y = rawY / 4096.0;
  float z = rawZ / 4096.0;
  currentImpactG = sqrtf(x * x + y * y + z * z);

  if (currentImpactG > peakImpactG) peakImpactG = currentImpactG;

  if (millis() - lastHistorySample >= 100) {
    lastHistorySample = millis();
    impactHistory[impactHistoryIndex] = currentImpactG;
    impactHistoryIndex = (impactHistoryIndex + 1) % GRAPH_POINTS;
    if (impactHistoryCount < GRAPH_POINTS) impactHistoryCount++;
  }

  if (millis() - lastImpactPrint >= 500) {
    lastImpactPrint = millis();
    Serial.print("Acceleration magnitude: ");
    Serial.print(currentImpactG, 2);
    Serial.println(" G");
  }

  if (currentImpactG >= IMPACT_G && !accidentCandidate && !accidentConfirmed) {
    startAccident();
  }
}

void startAccident() {
  accidentCandidate = true;
  accidentStart = millis();
  incidentStatus = "POSSIBLE ACCIDENT - WAITING FOR CONFIRMATION";
  lastEvent = "High impact detected. Waiting for the I'M SAFE response.";
  peakImpactG = max(peakImpactG, currentImpactG);

  Serial.println();
  Serial.println("!!! POSSIBLE ACCIDENT !!!");
  Serial.println("Applying brake progressively; confirmation timer started.");

  fastBrake();
  digitalWrite(RED_LED, HIGH);
  digitalWrite(BUZZER, HIGH);
}

void checkConfirmation() {
  unsigned long elapsed = millis() - accidentStart;

  if (elapsed < CONFIRM_TIME) {
    unsigned long remaining = (CONFIRM_TIME - elapsed) / 1000;
    static unsigned long lastCountdownPrint = 0;
    if (millis() - lastCountdownPrint >= 1000) {
      lastCountdownPrint = millis();
      Serial.printf("Accident confirmation: %lu seconds remaining\n", remaining);
    }

    digitalWrite(BUZZER, ((millis() / 400) % 2) ? HIGH : LOW);
    return;
  }

  accidentCandidate = false;
  accidentConfirmed = true;
  incidentStatus = "ACCIDENT CONFIRMED";
  digitalWrite(BUZZER, HIGH);

  Serial.println();
  Serial.println("================================");
  Serial.println("ACCIDENT CONFIRMED - updating Web UI");
  Serial.println("No I'M SAFE response received.");
  Serial.println("================================");

  prepareIncidentReport();
}

void checkSafeButton() {
  if (digitalRead(SAFE_BUTTON) != LOW) return;
  if (millis() - lastSafePress <= BUTTON_DELAY) return;

  lastSafePress = millis();

  if (accidentCandidate && !accidentConfirmed) {
    accidentCandidate = false;
    incidentStatus = "CANCELLED - USER PRESSED I'M SAFE";
    lastEvent = "Accident candidate cancelled by I'M SAFE button.";

    digitalWrite(BUZZER, LOW);
    digitalWrite(RED_LED, LOW);
    releaseBrake();

    Serial.println("I'M SAFE pressed. Accident candidate cancelled.");
  }
}

void checkSOSButton() {
  if (digitalRead(SOS_BUTTON) != LOW) return;
  if (millis() - lastSOSPress <= BUTTON_DELAY) return;

  lastSOSPress = millis();
  Serial.println("MANUAL SOS BUTTON PRESSED");

  manualSOS = true;
  accidentConfirmed = true;
  accidentCandidate = false;
  incidentStatus = "MANUAL SOS - EMERGENCY ALERT";

  fastBrake();
  digitalWrite(RED_LED, HIGH);
  digitalWrite(BUZZER, HIGH);
  prepareIncidentReport();
}

void obstacleControl() {
  static float previousDistance = -1;
  static unsigned long previousTime = 0;

  float distance = getDistance();
  if (distance <= 0) return;

  unsigned long now = millis();
  bool suddenObstacle = false;

  if (previousDistance > 0) {
    float drop = previousDistance - distance;
    unsigned long dt = now - previousTime;
    if (dt > 0 && dt < 300 && drop > 30) suddenObstacle = true;
  }

  previousDistance = distance;
  previousTime = now;

  if (suddenObstacle || distance < 35) {
    Serial.println("SUDDEN OBSTACLE: applying strong brake");
    fastBrake();
    return;
  }

  if (distance < 60) {
    smoothBrakeTo(70);
    return;
  }
  if (distance < 90) {
    smoothBrakeTo(40);
    return;
  }
  if (distance < 120) {
    smoothBrakeTo(20);
    return;
  }
  if (distance < 160) {
    smoothBrakeTo(5);
    return;
  }

  releaseBrake();
}

float getDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 25000);
  if (duration == 0) return -1;
  return duration * 0.0343 / 2.0;
}

void smoothBrakeTo(int target) {
  target = constrain(target, BRAKE_RELEASE, BRAKE_MAX);
  while (brakePosition != target) {
    if (brakePosition < target) {
      brakePosition += 2;
      if (brakePosition > target) brakePosition = target;
    } else {
      brakePosition -= 2;
      if (brakePosition < target) brakePosition = target;
    }
    brake.write(brakePosition);
    server.handleClient();
    delay(10);
  }
}

void fastBrake() {
  while (brakePosition < BRAKE_MAX) {
    brakePosition += 8;
    if (brakePosition > BRAKE_MAX) brakePosition = BRAKE_MAX;
    brake.write(brakePosition);
    server.handleClient();
    delay(15);
  }
}

void releaseBrake() {
  smoothBrakeTo(BRAKE_RELEASE);
}

void emergencyState() {
  brake.write(BRAKE_MAX);
  brakePosition = BRAKE_MAX;
  digitalWrite(RED_LED, HIGH);
  digitalWrite(BUZZER, HIGH);
  delay(100);
}

void prepareIncidentReport() {
  bool useRealGPS = gps.location.isValid() && gps.location.age() < 10000;

  if (useRealGPS) {
    incidentLat = gps.location.lat();
    incidentLon = gps.location.lng();
    incidentUsesDemoLocation = false;
  } else {
    incidentLat = DEMO_LAT;
    incidentLon = DEMO_LON;
    incidentUsesDemoLocation = true;
  }

  createDemoHospitals(incidentLat, incidentLon);
  nearestHospitalIndex = findNearestHospital(incidentLat, incidentLon);
  incidentHospitalName = hospitals[nearestHospitalIndex].name;
  nearestHospitalDistanceKm = distanceKm(
    incidentLat, incidentLon,
    hospitals[nearestHospitalIndex].lat,
    hospitals[nearestHospitalIndex].lon
  );

  incidentMapLink = "https://maps.google.com/?q=";
  incidentMapLink += String(incidentLat, 6);
  incidentMapLink += ",";
  incidentMapLink += String(incidentLon, 6);

  incidentDetailsAvailable = true;
  incidentNumber++;
  lastEvent = incidentUsesDemoLocation
    ? "Emergency recorded in Web UI; demo GPS location used."
    : "Emergency recorded in Web UI with current GPS location.";

  Serial.println();
  Serial.println("========== WEB UI INCIDENT REPORT ==========");
  Serial.println(incidentStatus);
  Serial.print("Location source: ");
  Serial.println(incidentUsesDemoLocation ? "DEMO LOCATION (GPS FIX UNAVAILABLE)" : "REAL GPS");
  Serial.printf("Latitude: %.6f\n", incidentLat);
  Serial.printf("Longitude: %.6f\n", incidentLon);
  Serial.print("Nearest demo hospital: ");
  Serial.println(incidentHospitalName);
  Serial.printf("Distance: %.2f km\n", nearestHospitalDistanceKm);
  Serial.print("Map: ");
  Serial.println(incidentMapLink);
  Serial.println("Open the ESP32 dashboard to see this incident.");
  Serial.println("============================================");
}

void createDemoHospitals(float lat, float lon) {
  hospitals[0] = {"Demo Hospital A", lat + 0.0020, lon + 0.0010};
  hospitals[1] = {"Demo Hospital B", lat - 0.0012, lon + 0.0020};
  hospitals[2] = {"Demo Hospital C", lat + 0.0010, lon - 0.0025};
}

int findNearestHospital(float lat, float lon) {
  int nearest = 0;
  float best = distanceKm(lat, lon, hospitals[0].lat, hospitals[0].lon);

  for (int i = 1; i < 3; i++) {
    float d = distanceKm(lat, lon, hospitals[i].lat, hospitals[i].lon);
    if (d < best) {
      best = d;
      nearest = i;
    }
  }
  return nearest;
}

float distanceKm(float lat1, float lon1, float lat2, float lon2) {
  float dLat = radians(lat2 - lat1);
  float dLon = radians(lon2 - lon1);
  float a = sinf(dLat / 2) * sinf(dLat / 2) +
            cosf(radians(lat1)) * cosf(radians(lat2)) *
            sinf(dLon / 2) * sinf(dLon / 2);
  float c = 2 * atan2f(sqrtf(a), sqrtf(1 - a));
  return 6371.0 * c;
}

String jsonEscape(const String& value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}

void handleRoot() {
  const char page[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="theme-color" content="#0b1220">
<title>Smart Accident System</title>
<style>
:root{color-scheme:dark;--bg:#08111f;--panel:#111f32;--panel2:#172942;--text:#eef5ff;--muted:#9cafc9;--cyan:#51d7f5;--green:#42d6a4;--yellow:#ffcc66;--red:#ff657a;--line:#263b56}
*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 15% 0%,#17314e 0,#08111f 38%);color:var(--text);font:15px/1.5 system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif}
main{width:min(1120px,calc(100% - 28px));margin:25px auto 40px}.top{display:flex;align-items:center;justify-content:space-between;gap:14px;flex-wrap:wrap;margin-bottom:20px}
.brand{display:flex;align-items:center;gap:13px}.logo{width:48px;height:48px;border-radius:15px;display:grid;place-items:center;background:linear-gradient(135deg,#51d7f5,#478bff);font-size:25px;color:#071426;font-weight:900}.eyebrow{color:var(--cyan);font-size:11px;letter-spacing:.16em;text-transform:uppercase;font-weight:800}.top h1{font-size:clamp(23px,4vw,31px);margin:0;letter-spacing:-.03em}.sub{color:var(--muted);margin:2px 0 0}.pill{border:1px solid var(--line);background:#122139;color:var(--muted);padding:8px 12px;border-radius:999px;font-weight:700;font-size:12px;display:flex;align-items:center;gap:8px}.dot{width:8px;height:8px;border-radius:50%;background:var(--green);box-shadow:0 0 12px currentColor}.pill.off .dot{background:var(--red)}
.banner{display:none;padding:17px 19px;border-radius:17px;margin:0 0 16px;border:1px solid #70404a;background:linear-gradient(110deg,#431c2b,#211b2b)}.banner.show{display:block}.banner h2{font-size:18px;margin:0 0 3px}.banner p{margin:0;color:#ffd9df}.banner.pending{border-color:#765b30;background:linear-gradient(110deg,#3c2c16,#211f2b)}.banner.pending p{color:#ffe7b8}
.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:13px}.card{border:1px solid var(--line);background:linear-gradient(145deg,#14243a,#0f1b2c);border-radius:18px;padding:17px;min-width:0;box-shadow:0 8px 28px #00000012}.label{color:var(--muted);font-size:12px;font-weight:700;letter-spacing:.04em;text-transform:uppercase}.value{font-size:clamp(22px,3vw,30px);font-weight:800;letter-spacing:-.03em;margin:8px 0 2px;overflow-wrap:anywhere}.unit{font-size:12px;color:var(--muted)}.status{font-weight:750;color:var(--green)}.warn{color:var(--yellow)!important}.danger{color:var(--red)!important}.section-grid{display:grid;grid-template-columns:1.4fr 1fr;gap:13px;margin-top:13px}.section-title{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:12px}.section-title h2{font-size:16px;margin:0}.mini{font-size:11px;color:var(--muted);border:1px solid var(--line);border-radius:7px;padding:3px 7px}.chart-wrap{height:220px;position:relative}.chart-wrap canvas{width:100%;height:100%;display:block}.location{display:grid;gap:12px}.row{display:flex;align-items:flex-start;justify-content:space-between;gap:12px;border-bottom:1px solid #23364d;padding:7px 0}.row:last-child{border-bottom:0}.row span:first-child{color:var(--muted)}.row strong{text-align:right;overflow-wrap:anywhere}.mapbtn{display:inline-flex;align-items:center;justify-content:center;text-decoration:none;margin-top:8px;padding:10px 13px;background:#183d5a;border:1px solid #2d7194;color:#dff8ff;border-radius:10px;font-weight:750}.mapbtn.disabled{opacity:.45;pointer-events:none}.events{margin-top:13px}.eventline{display:flex;gap:12px;align-items:flex-start}.event-icon{width:34px;height:34px;display:grid;place-items:center;background:#1b334c;border-radius:10px;flex-shrink:0}.eventline p{margin:0}.eventline small{color:var(--muted)}.footer{color:#7287a4;font-size:11px;margin:16px 3px 0}.muted{color:var(--muted)}
@media(max-width:820px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}.section-grid{grid-template-columns:1fr}}@media(max-width:420px){main{width:calc(100% - 18px);margin-top:15px}.grid{gap:8px}.card{padding:13px}.value{font-size:23px}}
</style>
</head>
<body><main>
<header class="top"><div class="brand"><div class="logo">✚</div><div><div class="eyebrow">Connected safety system</div><h1>Smart Accident Monitor</h1><p class="sub">ESP32 live status · GPS · impact detection</p></div></div><div class="pill" id="connection"><span class="dot"></span><span id="connectionText">Connecting to ESP32…</span></div></header>
<section class="banner" id="incidentBanner"><h2 id="bannerTitle">Incident alert</h2><p id="bannerText">Waiting for data…</p></section>
<section class="grid">
 <div class="card"><div class="label">System status</div><div class="value status" id="systemStatus">READY</div><div class="unit" id="systemDetail">Monitoring impact sensor</div></div>
 <div class="card"><div class="label">Acceleration</div><div class="value" id="currentG">--</div><div class="unit">Current magnitude · G</div></div>
 <div class="card"><div class="label">Peak acceleration</div><div class="value" id="peakG">--</div><div class="unit">Peak since ESP32 boot</div></div>
 <div class="card"><div class="label">Brake position</div><div class="value" id="brakePosition">--</div><div class="unit">Servo command · degrees</div></div>
</section>
<section class="section-grid">
 <div class="card"><div class="section-title"><h2>Impact history</h2><span class="mini">Recent ~6 seconds</span></div><div class="chart-wrap"><canvas id="impactChart" aria-label="Acceleration history graph"></canvas></div><p class="unit">The graph plots acceleration samples collected by the ESP32.</p></div>
 <div class="card"><div class="section-title"><h2>GPS & location</h2><span class="mini" id="gpsBadge">Waiting for fix</span></div><div class="location">
  <div class="row"><span>Latitude</span><strong id="latitude">Waiting…</strong></div>
  <div class="row"><span>Longitude</span><strong id="longitude">Waiting…</strong></div>
  <div class="row"><span>Satellites</span><strong id="satellites">--</strong></div>
  <div class="row"><span>Location source</span><strong id="locationSource">No incident yet</strong></div>
  <a id="mapLink" class="mapbtn disabled" href="#" target="_blank" rel="noopener">Open location in Google Maps ↗</a>
 </div></div>
</section>
<section class="section-grid">
 <div class="card"><div class="section-title"><h2>Emergency report</h2><span class="mini" id="incidentCount">0 incidents</span></div>
  <div class="row"><span>Incident</span><strong id="incidentStatus">No active incident</strong></div>
  <div class="row"><span>Nearest demo hospital</span><strong id="hospital">Not calculated</strong></div>
  <div class="row"><span>Estimated distance</span><strong id="hospitalDistance">--</strong></div>
  <div class="row"><span>Alert type</span><strong id="alertType">None</strong></div>
 </div>
 <div class="card"><div class="section-title"><h2>Vehicle safety</h2><span class="mini">Live</span></div>
  <div class="row"><span>Accident candidate</span><strong id="candidate">NO</strong></div>
  <div class="row"><span>Accident confirmed</span><strong id="confirmed">NO</strong></div>
  <div class="row"><span>Manual SOS</span><strong id="manualSOS">NO</strong></div>
  <div class="row"><span>LED / buzzer</span><strong id="alarmState">OFF</strong></div>
  <div class="row"><span>GPS mode</span><strong id="gpsMode">Live GPS</strong></div>
 </div>
</section>
<section class="card events"><div class="section-title"><h2>Latest event</h2><span class="mini" id="updateTime">No updates yet</span></div><div class="eventline"><div class="event-icon">ⓘ</div><div><p id="lastEvent">System started. Waiting for sensor events.</p><small>Updates automatically while this page is open.</small></div></div></section>
<p class="footer">Exhibition prototype only. Demo hospitals are simulated placeholders, not real emergency-service listings. Keep this page open on the same trusted Wi-Fi as the ESP32 to see live updates.</p>
</main>
<script>
const $ = id => document.getElementById(id);
let lastData = null;
function setStatus(el, text, kind='') { el.textContent=text; el.classList.remove('warn','danger','status'); if(kind) el.classList.add(kind); }
function fmtCoord(v) { return v === null || v === undefined ? 'No GPS fix' : Number(v).toFixed(6); }
function drawGraph(values) {
 const canvas=$('impactChart'), rect=canvas.getBoundingClientRect(), dpr=window.devicePixelRatio||1;
 if(!rect.width||!rect.height)return;
 canvas.width=Math.floor(rect.width*dpr);canvas.height=Math.floor(rect.height*dpr);
 const ctx=canvas.getContext('2d');ctx.scale(dpr,dpr);
 const w=rect.width,h=rect.height,padL=34,padR=10,padT=12,padB=23;
 ctx.clearRect(0,0,w,h);
 const plotW=w-padL-padR,plotH=h-padT-padB,maxY=8,minY=0;
 ctx.font='10px system-ui';ctx.lineWidth=1;
 for(let y=0;y<=8;y+=2){const yy=padT+plotH-(y-minY)/(maxY-minY)*plotH;ctx.strokeStyle='#263b56';ctx.beginPath();ctx.moveTo(padL,yy);ctx.lineTo(w-padR,yy);ctx.stroke();ctx.fillStyle='#8fa4c0';ctx.fillText(y+'G',3,yy+3);}
 const threshold=4, ty=padT+plotH-(threshold-minY)/(maxY-minY)*plotH;ctx.setLineDash([5,4]);ctx.strokeStyle='#ff657a';ctx.beginPath();ctx.moveTo(padL,ty);ctx.lineTo(w-padR,ty);ctx.stroke();ctx.setLineDash([]);
 if(!values||values.length<2)return;
 ctx.strokeStyle='#51d7f5';ctx.lineWidth=2;ctx.beginPath();
 values.forEach((v,i)=>{const x=padL+(i/(values.length-1))*plotW;const val=Math.min(maxY,Math.max(minY,Number(v)||0));const y=padT+plotH-(val-minY)/(maxY-minY)*plotH;if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);});ctx.stroke();
 }
async function refresh() {
 try {
  const response=await fetch('/data?_='+Date.now(),{cache:'no-store'});
  if(!response.ok)throw new Error('HTTP '+response.status);
  const d=await response.json();lastData=d;
  $('connection').classList.remove('off');$('connectionText').textContent='ESP32 connected';
  setStatus($('systemStatus'),d.incidentStatus||'READY',d.accidentConfirmed?'danger':d.accidentCandidate?'warn':'status');
  $('systemDetail').textContent=d.accidentConfirmed?'Emergency condition latched':d.accidentCandidate?'Waiting for safe confirmation':'Monitoring impact sensor';
  $('currentG').textContent=Number(d.currentG).toFixed(2)+' G';
  $('peakG').textContent=Number(d.peakG).toFixed(2)+' G';
  $('brakePosition').textContent=d.brakePosition+'°';
  $('latitude').textContent=fmtCoord(d.lat);
  $('longitude').textContent=fmtCoord(d.lon);
  $('satellites').textContent=d.satellites;
  $('gpsBadge').textContent=d.gpsFix?'GPS fix available':'No GPS fix';
  $('locationSource').textContent=d.incidentDetailsAvailable?(d.demoLocation?'Demo location':'Incident GPS location'):(d.gpsFix?'Live GPS position':'Waiting for GPS');
  $('gpsMode').textContent=d.incidentDetailsAvailable?(d.demoLocation?'DEMO':'REAL GPS'):(d.gpsFix?'REAL GPS':'DEMO fallback on alert');
  if(d.incidentDetailsAvailable&&d.mapLink){$('mapLink').href=d.mapLink;$('mapLink').classList.remove('disabled');}else{$('mapLink').href='#';$('mapLink').classList.add('disabled');}
  $('incidentStatus').textContent=d.incidentStatus;
  $('hospital').textContent=d.hospitalName||'Not calculated';
  $('hospitalDistance').textContent=d.incidentDetailsAvailable?Number(d.hospitalDistanceKm).toFixed(2)+' km':'--';
  $('alertType').textContent=d.manualSOS?'Manual SOS':d.accidentConfirmed?'Impact confirmed':d.accidentCandidate?'Impact / awaiting confirmation':'None';
  $('candidate').textContent=d.accidentCandidate?'YES':'NO';$('candidate').className=d.accidentCandidate?'warn':'';
  $('confirmed').textContent=d.accidentConfirmed?'YES':'NO';$('confirmed').className=d.accidentConfirmed?'danger':'';
  $('manualSOS').textContent=d.manualSOS?'YES':'NO';$('manualSOS').className=d.manualSOS?'danger':'';
  $('alarmState').textContent=d.alarmOn?'ON':'OFF';$('alarmState').className=d.alarmOn?'danger':'';
  $('incidentCount').textContent=d.incidentNumber+' event'+(d.incidentNumber===1?'':'s');
  $('lastEvent').textContent=d.lastEvent;
  $('updateTime').textContent='Updated '+new Date().toLocaleTimeString();
  const banner=$('incidentBanner');banner.classList.remove('show','pending');
  if(d.accidentConfirmed){banner.classList.add('show');$('bannerTitle').textContent='🚨 EMERGENCY ALERT';$('bannerText').textContent=d.manualSOS?'Manual SOS activated. Brake and alarm are latched.': 'Accident confirmed. Brake, buzzer and red LED are active.';}
  else if(d.accidentCandidate){banner.classList.add('show','pending');$('bannerTitle').textContent='⚠ Possible impact detected';$('bannerText').textContent='Waiting for the physical I’M SAFE button. If no response within 20 seconds, accident status will be confirmed.';}
  if(d.demoLocation&&d.incidentDetailsAvailable){$('locationSource').classList.add('warn');}else{$('locationSource').classList.remove('warn');}
  drawGraph(d.history||[]);
 } catch(e) {
  $('connection').classList.add('off');$('connectionText').textContent='Dashboard disconnected';
 }
}
window.addEventListener('resize',()=>drawGraph(lastData?lastData.history:[]));
refresh();setInterval(refresh,1000);
</script>
</body></html>
)rawliteral";
  server.send(200, "text/html; charset=utf-8", page);
}

void handleData() {
  bool gpsFix = gps.location.isValid() && gps.location.age() < 10000;
  String json = "{";
  json += "\"gpsFix\":" + String(gpsFix ? "true" : "false") + ",";
  json += "\"satellites\":" + String(gps.satellites.value()) + ",";
  json += "\"lat\":";
  json += gpsFix ? String(gps.location.lat(), 6) : String("null");
  json += ",\"lon\":";
  json += gpsFix ? String(gps.location.lng(), 6) : String("null");
  json += ",\"currentG\":" + String(currentImpactG, 3) + ",";
  json += "\"peakG\":" + String(peakImpactG, 3) + ",";
  json += "\"brakePosition\":" + String(brakePosition) + ",";
  json += "\"accidentCandidate\":" + String(accidentCandidate ? "true" : "false") + ",";
  json += "\"accidentConfirmed\":" + String(accidentConfirmed ? "true" : "false") + ",";
  json += "\"manualSOS\":" + String(manualSOS ? "true" : "false") + ",";
  json += "\"alarmOn\":" + String((accidentCandidate || accidentConfirmed) ? "true" : "false") + ",";
  json += "\"incidentDetailsAvailable\":" + String(incidentDetailsAvailable ? "true" : "false") + ",";
  json += "\"demoLocation\":" + String(incidentUsesDemoLocation ? "true" : "false") + ",";
  json += "\"incidentLat\":" + String(incidentLat, 6) + ",";
  json += "\"incidentLon\":" + String(incidentLon, 6) + ",";
  json += "\"hospitalDistanceKm\":" + String(nearestHospitalDistanceKm, 3) + ",";
  json += "\"hospitalName\":\"" + jsonEscape(incidentHospitalName) + "\",";
  json += "\"mapLink\":\"" + jsonEscape(incidentMapLink) + "\",";
  json += "\"incidentNumber\":" + String(incidentNumber) + ",";
  json += "\"incidentStatus\":\"" + jsonEscape(incidentStatus) + "\",";
  json += "\"lastEvent\":\"" + jsonEscape(lastEvent) + "\",";
  json += "\"history\":[";

  int start = (impactHistoryIndex - impactHistoryCount + GRAPH_POINTS) % GRAPH_POINTS;
  for (int i = 0; i < impactHistoryCount; i++) {
    int index = (start + i) % GRAPH_POINTS;
    if (i) json += ',';
    json += String(impactHistory[index], 2);
  }
  json += "]}";

  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", json);
}
