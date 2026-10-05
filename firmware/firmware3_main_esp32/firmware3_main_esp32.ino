// FIRMWARE 3 (reconstructed) - Main ESP32 hub: E-Nose, swab colorimeter, sonar, dashboard, arbitration
// Libraries: Adafruit TCS34725, WebServer (built-in). Board: ESP32 Dev Module.

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_TCS34725.h>

const char* WIFI_SSID = "YOUR_SSID";
const char* WIFI_PASS = "YOUR_PASS";
const char* CAM_IP = "192.168.1.60"; // ESP32-CAM (use DHCP reservation)

#define PIN_MQ2 34
#define PIN_MQ3 32
#define PIN_MQ135 33
#define PIN_TRIG 5
#define PIN_ECHO 18
#define PIN_BUZZ 25
#define PIN_FAN 14
#define PIN_VBAT 35 // 3S LiPo via divider (e.g. 47k top / 10k bottom)

#define SerialMove Serial2 // TX2 = GPIO17 -> Pico GP1

Adafruit_TCS34725 tcs(TCS34725_INTEGRATIONTIME_154MS, TCS34725_GAIN_4X);
WebServer server(80);

// ---------- E-nose ----------
const uint32_t WARMUP_MS = 180000; // MQ heaters need minutes to stabilise
float base2, base3, base135; // clean-air baselines (ADC counts)
float th2 = 150, th3 = 150, th135 = 150; // adjustable via /set
bool baselineDone = false;
int d2, d3, d135;
uint8_t hitCount = 0;
String enoseClass = "WARMING UP";
bool narcotics_detected = false, explosive_detected = false;

// ---------- swab ----------
float swabRatio = 0, blankRatio = 0;
const float ABS_RATIO_LIMIT = 0.65, REL_RATIO_LIMIT = 0.80;

// ---------- AI intercept ----------
char ai_nav_vector = 'S';
String face_status = "RELAXED", tracked_subject_id = "Scanning...";
bool ai_armed_remote = false, ai_enabled_local = false; // two-key rule: python 'arm' AND dashboard enable
uint32_t aiLastMs = 0;

long dist = 999;
uint32_t turnUntil = 0, lastTx = 0, lastSonar = 0;
float vbat = 0;

float avgADC(int pin, int n = 16) { long s = 0; for (int i = 0; i < n; i++) s += analogRead(pin); return s / (float)n; }

void calibrateBaseline() {
  base2 = avgADC(PIN_MQ2, 64); base3 = avgADC(PIN_MQ3, 64); base135 = avgADC(PIN_MQ135, 64);
  baselineDone = true; narcotics_detected = false; enoseClass = "CLEAN";
}

void classifyEnose() {
  if (!baselineDone) return;
  d2 = avgADC(PIN_MQ2) - base2; d3 = avgADC(PIN_MQ3) - base3; d135 = avgADC(PIN_MQ135) - base135;
  String cls = "CLEAN"; bool hit = false;
  if (d3 > 1.2 * d135 && d3 > th3) cls = "SOLVENT/SANITIZER (ignored)";
  else if (d135 > 1.6 * d3 && d135 > th135) { cls = "ACETIC-ACID-LIKE (heroin marker)"; hit = true; }
  else if ((d135 + d2) > 2.2 * d3 && d2 > th2) { cls = "METHYL-BENZOATE-LIKE (cocaine marker)"; hit = true; }
  hitCount = hit ? min(hitCount + 1, 10) : 0; // persistence filter: 3 consecutive hits
  if (hitCount >= 3) narcotics_detected = true; // latched until /ack
  enoseClass = cls;
}

void readSwab(bool asBlank) {
  uint16_t r, g, b, c;
  tcs.setInterrupt(false); delay(60); // LED on
  tcs.getRawData(&r, &g, &b, &c);
  tcs.setInterrupt(true); // LED off
  float ratio = r ? (float)g / r : 1.0;
  if (asBlank) { blankRatio = ratio; return; }
  swabRatio = ratio;
  bool pos = (blankRatio > 0) ? (ratio / blankRatio < REL_RATIO_LIMIT) : (ratio < ABS_RATIO_LIMIT);
  if (pos) explosive_detected = true;
}

long getDistance() {
  digitalWrite(PIN_TRIG, LOW); delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10); digitalWrite(PIN_TRIG, LOW);
  long d = pulseIn(PIN_ECHO, HIGH, 25000);
  return d ? d * 0.0343 / 2 : 999;
}

void notifyCamera(const char* type) {
  HTTPClient h; h.setTimeout(800);
  h.begin(String("http://") + CAM_IP + "/alert?type=" + type); h.GET(); h.end();
}

const char PAGE[] PROGMEM = R"HTML(<!doctype html><meta name="viewport" content="width=device-width, initial-scale=1">
<title>RPF-SHIELD</title><style>body{font-family:sans-serif; background:#0b1220;color:#e6edf7;margin:8px}
.c{background:#162036; border-radius:8px; padding:10px; margin:8px 0} .r{color:#ff5a5a; font-weight:700}
button{padding:8px 12px; margin:2px}</style>
<h2>RPF-SHIELD Command Centre</h2><div class=c><img id=cam style="width:100%; max-width:480px"></div>
<div class=c id=s>...</div><div class=c>
<button onclick="f('/calibrate')">Re-baseline</button><button onclick="f('/ack')">Acknowledge alerts</button>
<button onclick="f('/swab_blank')">Swab: blank</button><button onclick="f('/swab')">Swab: test</button>
<button onclick="f('/ai_enable?on=1')">AI intercept ON</button><button onclick="f('/ai_enable?on=0')">AI OFF</button><br>
Sensitivity <input type=range min=50 max=600 value=150 onchange="f('/set?t='+this.value)"></div>
<script>cam.src='http://CAM_IP_HERE:81/';
function f(u){fetch(u)} setInterval(async()=>{let j=await(await fetch('/status')).json();
s.innerHTML='E-nose: '+j.enose+'<br>dMQ2/3/135: '+j.d2+'/'+j.d3+'/'+j.d135+'<br>Swab G/R: '+j.swab.toFixed(2)+
'<br>Sonar: '+j.dist+' cm | Battery: '+j.vbat.toFixed(1)+' V<br>Subject: '+j.subject+' ('+j.face+') AI: '+j.ai+
(j.narc?'<p class=r>NARCOTIC MARKER ALERT</p>':'')+
(j.expl?'<p class=r>NITRO EXPLOSIVE TRACE ALERT</p>':'')},1000)</script>)HTML";

void setupRoutes() {
  server.on("/", []() { String p = FPSTR(PAGE); p.replace("CAM_IP_HERE", CAM_IP); server.send(200, "text/html", p); });
  server.on("/status", []() {
    String j = "{\"enose\":\"" + enoseClass + "\",\"d2\":" + d2 + ",\"d3\":" + d3 + ",\"d135\":" + d135 +
    ",\"swab\":" + swabRatio + ",\"dist\":" + dist + ",\"vbat\":" + vbat + ",\"narc\":" + narcotics_detected +
    ",\"expl\":" + explosive_detected + ",\"subject\":\"" + tracked_subject_id + "\",\"face\":\"" + face_status +
    "\",\"ai\":\"" + ((ai_enabled_local && ai_armed_remote)? "ARMED" : "ADVISORY") + "\"}";
    server.send(200, "application/json", j);
  });
  server.on("/calibrate", []() { calibrateBaseline(); server.send(200, "text/plain", "OK"); });
  server.on("/ack", []() {
    narcotics_detected = false; explosive_detected = false; hitCount = 0;
    server.send(200, "text/plain", "OK");
  });
  server.on("/swab_blank", []() { readSwab(true); server.send(200, "text/plain", "OK"); });
  server.on("/swab", []() { readSwab(false); server.send(200, "text/plain", "OK"); });
  server.on("/set", []() {
    if (server.hasArg("t")) { th2 = th3 = th135 = server.arg("t").toFloat(); }
    server.send(200, "text/plain", "OK");
  });
  server.on("/ai_nav", []() {
    if (server.hasArg("cmd")) ai_nav_vector = server.arg("cmd").charAt(0);
    if (server.hasArg("state")) face_status = server.arg("state");
    if (server.hasArg("id")) tracked_subject_id = server.arg("id");
    if (server.hasArg("arm")) ai_armed_remote = (server.arg("arm") == "1");
    aiLastMs = millis(); // heartbeat for stale-command timeout
    server.send(200, "text/plain", "OK");
  });
  server.on("/ai_enable", []() {
    if (server.hasArg("on")) ai_enabled_local = (server.arg("on") == "1");
    server.send(200, "text/plain", "OK");
  });
}

void setup() {
  pinMode(PIN_TRIG, OUTPUT); pinMode(PIN_ECHO, INPUT); pinMode(PIN_BUZZ, OUTPUT); pinMode(PIN_FAN, OUTPUT);
  digitalWrite(PIN_FAN, HIGH);
  analogReadResolution(12); analogSetAttenuation(ADC_11db);
  SerialMove.begin(115200, SERIAL_8N1, 16, 17);
  Wire.begin(21, 22);
  tcs.begin(); tcs.setInterrupt(true);
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(300);
  setupRoutes(); server.begin();
}

void loop() {
  server.handleClient();
  uint32_t now = millis();
  if (now - lastSonar > 100) {
    lastSonar = now; dist = getDistance();
    vbat = (analogRead(PIN_VBAT) / 4095.0) * 3.3 * 5.7;
  }
  if (!baselineDone && now > WARMUP_MS) calibrateBaseline();
  static uint32_t lastEn = 0;
  if (now - lastEn > 500) { lastEn = now; classifyEnose(); }

  bool threat = narcotics_detected || explosive_detected;
  static bool alerted = false;
  if (threat && !alerted) { alerted = true; notifyCamera(explosive_detected ? "NITRO" : "NARCOTIC"); }
  if (!threat) alerted = false;
  digitalWrite(PIN_BUZZ, threat);

  // Subsumption arbitration (non-blocking)
  bool aiFresh = (now - aiLastMs) < 1500;
  bool aiGo = ai_enabled_local && ai_armed_remote && aiFresh && (face_status == "STRESSED");
  char out = 'S';
  if (threat) out = 'S'; // P1 halt and hold
  else if (vbat > 5 && vbat < 10.2) out = 'S'; // P1b low-battery stop (<3.4 V/cell)
  else if (dist < 25 || now < turnUntil) { // P2 obstacle avoidance
    if (dist < 25 && now >= turnUntil) turnUntil = now + 400;
    out = 'L';
  }
  else if (aiGo) out = ai_nav_vector; // P3 operator-armed intercept

  if (now - lastTx > 100) { lastTx = now; SerialMove.write(out); }
}
