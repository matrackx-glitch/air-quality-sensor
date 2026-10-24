/*
  ESP32-C3 + Sensirion SGP41 Air Quality Monitor
  -------------------------------------------------
  Reads VOC Index + NOx Index from an SGP41 and serves a live dashboard
  over WiFi, with a ~10-minute history line graph for each metric.

  WHAT THIS BOARD CAN AND CAN'T MEASURE
  ----------------------------------------
  The SGP41 only outputs VOC and NOx. It has no CO2 sensor, no
  particulate (PM) sensor, and no temperature/humidity sensor on board -
  those are separate physical chips (e.g. an SCD4x for CO2, a PMS5003/
  SPS30 for particulates, an SHT4x for climate). This sketch only shows
  what this hardware actually measures. Adding those other readouts
  would mean either fabricating numbers or bolting on new sensors -
  if you want that later, say so and I'll help wire/code the extra
  sensor(s); until then this intentionally stays VOC/NOx only.

  HARDWARE
  --------
  - ESP32-C3 (e.g. Seeed XIAO ESP32C3)
  - SGP41 breakout via I2C:
      VDD -> 3V3, GND -> GND, SDA -> D4 (GPIO6), SCL -> D5 (GPIO7)
      (change SDA_PIN/SCL_PIN below for a different board/wiring)

  LIBRARIES (Arduino Library Manager)
  ------------------------------------
  - "Sensirion I2C SGP41"           by Sensirion
  - "Sensirion Gas Index Algorithm" by Sensirion
  WiFi, WebServer, and ESPmDNS are built into the ESP32 Arduino core.

  SETUP
  -----
  1. Fill in WIFI_SSID / WIFI_PASSWORD below.
  2. Upload, then open Serial Monitor (115200 baud) once to confirm it
     connects and to see its IP.
  3. After that, you can just visit http://airmonitor.local from any
     device on the same network - no need to look up the IP again,
     even if it changes.
  4. The board reconnects automatically if WiFi drops, so once it's
     plugged into permanent power it just stays up.
*/

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SensirionI2CSgp41.h>
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>

// ---------- User-configurable settings ----------
#define WIFI_SSID     "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define MDNS_HOSTNAME "airmonitor"   // reachable at http://airmonitor.local

#define SDA_PIN 6   // XIAO ESP32C3 D4
#define SCL_PIN 7   // XIAO ESP32C3 D5
#define SAMPLING_INTERVAL_MS 1000UL
#define CONDITIONING_S 10

#define HISTORY_INTERVAL_MS 5000UL   // one history point every 5s
#define HISTORY_SIZE 120             // -> 10 minutes of history

#define WIFI_CHECK_INTERVAL_MS 15000UL

#define DEFAULT_RH_PERCENT 50.0f
#define DEFAULT_TEMP_C     25.0f

// ---------- Globals ----------
SensirionI2CSgp41 sgp41;
VOCGasIndexAlgorithm vocAlgorithm;
NOxGasIndexAlgorithm noxAlgorithm;
WebServer server(80);

static int32_t conditioningSecondsElapsed = 0;
static bool conditioningDone = false;

static int32_t latestVocIndex = -1;
static int32_t latestNoxIndex = -1;
static bool haveReading = false;

static float vocHistory[HISTORY_SIZE];
static float noxHistory[HISTORY_SIZE];
static int historyIndex = 0;   // next slot to write (= oldest slot once full)
static int historyCount = 0;   // how many slots are populated so far

uint16_t compensationRh() {
  return static_cast<uint16_t>(DEFAULT_RH_PERCENT * 65535.0f / 100.0f);
}
uint16_t compensationT() {
  return static_cast<uint16_t>((DEFAULT_TEMP_C + 45.0f) * 65535.0f / 175.0f);
}

void printSensirionError(const char* stage, uint16_t error) {
  char errorMessage[128];
  errorToString(error, errorMessage, sizeof errorMessage);
  Serial.print("Error during ");
  Serial.print(stage);
  Serial.print(": ");
  Serial.println(errorMessage);
}

void pushHistory(float voc, float nox) {
  vocHistory[historyIndex] = voc;
  noxHistory[historyIndex] = nox;
  historyIndex = (historyIndex + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) historyCount++;
}

// ---------- WiFi ----------
void connectWifi() {
  Serial.print("Joining WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // avoids power-save interfering with the handshake
  WiFi.disconnect(true, true);
  delay(300);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
    if (MDNS.begin(MDNS_HOSTNAME)) {
      Serial.print("Reachable at: http://");
      Serial.print(MDNS_HOSTNAME);
      Serial.println(".local");
    }
  } else {
    Serial.print("WiFi connect failed, status code ");
    Serial.println(WiFi.status());
  }
}

// ---------- Web handlers ----------
String qualityWord(int32_t index) {
  if (index < 0) return "warming up";
  if (index <= 100) return "good";
  if (index <= 200) return "elevated";
  return "high";
}

void handleRoot() {
  String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Air Quality Monitor</title>
<style>
  :root {
    --bg:#0a0a0c; --card:#131316; --border:#232327; --text:#eaeaea; --muted:#7a7a82;
  }
  * { box-sizing:border-box; }
  body {
    margin:0; padding:28px 16px 48px; background:var(--bg); color:var(--text);
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
  }
  h1 { font-weight:600; font-size:19px; letter-spacing:0.5px; margin:0 0 24px; color:#d4d4d8; text-align:center; }
  .stack { display:flex; flex-direction:column; gap:16px; max-width:380px; margin:0 auto; }
  .card {
    background:var(--card); border:1px solid var(--border); border-radius:26px;
    padding:20px 22px; transition:box-shadow 0.4s ease;
  }
  .card-top { display:flex; align-items:baseline; justify-content:space-between; margin-bottom:2px; }
  .card-label { font-size:13px; text-transform:uppercase; letter-spacing:1px; color:var(--muted); }
  .card-status { font-size:14px; font-weight:600; transition:color 0.4s ease; }
  .card-value { font-size:48px; font-weight:700; line-height:1.1; margin-bottom:14px; transition:color 0.4s ease; }
  svg { width:100%; height:80px; display:block; }
  .footnote { color:var(--muted); font-size:12px; margin-top:26px; max-width:380px; margin-left:auto; margin-right:auto; text-align:center; }
</style>
</head>
<body>
  <h1>AIR QUALITY MONITOR</h1>
  <div class="stack">
    <div class="card">
      <div class="card-top">
        <div class="card-label">VOC Index</div>
        <div class="card-status" id="vocStatus">--</div>
      </div>
      <div class="card-value" id="vocVal">--</div>
      <svg id="vocChart" viewBox="0 0 600 120" preserveAspectRatio="none"></svg>
    </div>
    <div class="card">
      <div class="card-top">
        <div class="card-label">NOx Index</div>
        <div class="card-status" id="noxStatus">--</div>
      </div>
      <div class="card-value" id="noxVal">--</div>
      <svg id="noxChart" viewBox="0 0 600 120" preserveAspectRatio="none"></svg>
    </div>
  </div>
  <div class="footnote">~100 = baseline air. This board measures VOC and NOx only - no CO2 or particulate sensor is fitted.</div>
<script>
// Color bands: green -> yellow -> orange -> red as the index rises
function colorForValue(v) {
  if (v < 0) return '#7a7a82';
  if (v <= 100) return '#34d399';   // green - good
  if (v <= 150) return '#facc15';   // yellow - mildly elevated
  if (v <= 250) return '#fb923c';   // orange - elevated
  return '#f87171';                 // red - high
}
function wordForValue(v) {
  if (v < 0) return 'warming up';
  if (v <= 100) return 'good';
  if (v <= 150) return 'elevated';
  if (v <= 250) return 'high';
  return 'very high';
}
function refreshCurrent() {
  fetch('/data').then(r => r.json()).then(d => {
    const vocEl = document.getElementById('vocVal');
    const noxEl = document.getElementById('noxVal');
    vocEl.textContent = d.voc >= 0 ? d.voc : '--';
    noxEl.textContent = d.nox >= 0 ? d.nox : '--';
    vocEl.style.color = colorForValue(d.voc);
    noxEl.style.color = colorForValue(d.nox);
    const vs = document.getElementById('vocStatus');
    const ns = document.getElementById('noxStatus');
    vs.textContent = wordForValue(d.voc); vs.style.color = colorForValue(d.voc);
    ns.textContent = wordForValue(d.nox); ns.style.color = colorForValue(d.nox);
  }).catch(() => {});
}
function drawChart(svgId, values) {
  const svg = document.getElementById(svgId);
  svg.innerHTML = '';
  const valid = values.filter(v => v >= 0);
  if (valid.length < 2) return;
  const w = 600, h = 80, pad = 6;
  let min = Math.min(...valid), max = Math.max(...valid);
  if (max - min < 10) { const mid = (max + min) / 2; min = mid - 5; max = mid + 5; }
  const pts = values.map((v, i) => ({
    x: (i / (values.length - 1)) * (w - pad * 2) + pad,
    y: h - pad - ((v - min) / (max - min)) * (h - pad * 2),
    v: v
  }));
  const ns = 'http://www.w3.org/2000/svg';
  for (let i = 0; i < pts.length - 1; i++) {
    const a = pts[i], b = pts[i + 1];
    const avgV = (a.v + b.v) / 2;
    const seg = document.createElementNS(ns, 'line');
    seg.setAttribute('x1', a.x.toFixed(1)); seg.setAttribute('y1', a.y.toFixed(1));
    seg.setAttribute('x2', b.x.toFixed(1)); seg.setAttribute('y2', b.y.toFixed(1));
    seg.setAttribute('stroke', colorForValue(avgV));
    seg.setAttribute('stroke-width', '3');
    seg.setAttribute('stroke-linecap', 'round');
    svg.appendChild(seg);
  }
}
function refreshHistory() {
  fetch('/history').then(r => r.json()).then(d => {
    drawChart('vocChart', d.voc);
    drawChart('noxChart', d.nox);
  }).catch(() => {});
}
setInterval(refreshCurrent, 2000);
setInterval(refreshHistory, 5000);
refreshCurrent();
refreshHistory();
</script>
</body>
</html>
)HTML";
  server.send(200, "text/html", html);
}

void handleData() {
  String voc = haveReading ? String(latestVocIndex) : "-1";
  String nox = haveReading ? String(latestNoxIndex) : "-1";
  String json = "{\"voc\":" + voc + ",\"nox\":" + nox +
                ",\"vocWord\":\"" + qualityWord(haveReading ? latestVocIndex : -1) + "\"" +
                ",\"noxWord\":\"" + qualityWord(haveReading ? latestNoxIndex : -1) + "\"}";
  server.send(200, "application/json", json);
}

void handleHistory() {
  int start = (historyCount < HISTORY_SIZE) ? 0 : historyIndex;
  String json = "{\"voc\":[";
  for (int i = 0; i < historyCount; i++) {
    int idx = (start + i) % HISTORY_SIZE;
    json += String(vocHistory[idx], 0);
    if (i < historyCount - 1) json += ",";
  }
  json += "],\"nox\":[";
  for (int i = 0; i < historyCount; i++) {
    int idx = (start + i) % HISTORY_SIZE;
    json += String(noxHistory[idx], 0);
    if (i < historyCount - 1) json += ",";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== ESP32-C3 SGP41 Air Quality Monitor ===");

  Wire.begin(SDA_PIN, SCL_PIN);
  sgp41.begin(Wire);

  connectWifi();

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/history", handleHistory);
  server.begin();
  Serial.println("Web server started.");

  Serial.print("Conditioning NOx sensor for ");
  Serial.print(CONDITIONING_S);
  Serial.println(" seconds...");
}

void loop() {
  server.handleClient();

  // Non-blocking WiFi watchdog - reconnects automatically if the
  // connection drops, so the board can run unattended on wall power.
  static unsigned long lastWifiCheck = 0;
  unsigned long nowCheck = millis();
  if (nowCheck - lastWifiCheck >= WIFI_CHECK_INTERVAL_MS) {
    lastWifiCheck = nowCheck;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi dropped - reconnecting...");
      connectWifi();
    }
  }

  static unsigned long lastSampleMs = 0;
  static unsigned long lastHistoryMs = 0;
  unsigned long now = millis();
  if (now - lastSampleMs < SAMPLING_INTERVAL_MS) return;
  lastSampleMs = now;

  uint16_t error;
  uint16_t srawVoc = 0;
  uint16_t srawNox = 0;

  if (!conditioningDone) {
    error = sgp41.executeConditioning(compensationRh(), compensationT(), srawVoc);
    if (error) printSensirionError("executeConditioning", error);
    conditioningSecondsElapsed++;
    if (conditioningSecondsElapsed >= CONDITIONING_S) {
      conditioningDone = true;
      Serial.println("Conditioning complete. Starting normal measurements.");
    }
    return;
  }

  error = sgp41.measureRawSignals(compensationRh(), compensationT(), srawVoc, srawNox);
  if (error) {
    printSensirionError("measureRawSignals", error);
    return;
  }

  latestVocIndex = vocAlgorithm.process(srawVoc);
  latestNoxIndex = noxAlgorithm.process(srawNox);
  haveReading = true;

  Serial.print("VOC Index: ");
  Serial.print(latestVocIndex);
  Serial.print("   NOx Index: ");
  Serial.println(latestNoxIndex);

  if (now - lastHistoryMs >= HISTORY_INTERVAL_MS) {
    lastHistoryMs = now;
    pushHistory((float)latestVocIndex, (float)latestNoxIndex);
  }
}
