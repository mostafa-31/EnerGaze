/*
  Advanced Energy Meter – ESP32 + 2x ACS712-30A + ZMPT101B + 2-Channel Relay + 0.96" OLED
  Auto relay off if >3A per load + Blynk control + CSV logging with NTP time
*/

#define BLYNK_TEMPLATE_ID "TMPL6IgJwdRCy"
#define BLYNK_TEMPLATE_NAME "IOT energy meter"
#define BLYNK_AUTH_TOKEN "WltDueeMLqHDOwh3FKz0hR8EhujUKpen"

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>
#include <FS.h>
#include <SPIFFS.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#include <WebServer.h>
#include <DHT.h>
#include <esp_system.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

WiFiUDP ntpUDP;
// Set your timezone offset in seconds.
// Examples:
//  - UTC       : 0
//  - IST (UTC+5:30): 19800
//  - UTC+6     : 21600
// Your device was showing ~6 hours behind, so we use UTC+6 here.
const long TIMEZONE_OFFSET_SECONDS = 6 * 3600;
NTPClient timeClient(ntpUDP, "pool.ntp.org", TIMEZONE_OFFSET_SECONDS, 60000);

// ==================== PINS ====================
#define CURRENT_PIN1   34     // Load 1 current
#define CURRENT_PIN2   32     // Load 2 current
#define CURRENT_PIN3   33     // Load 3 current (ACS712 output) (ADC1 pin; works with WiFi)
#define VOLTAGE_PIN    35     // ZMPT101B
#define RELAY1_PIN     26     // Relay for load 1 (LOW = ON) (avoid strap pins like GPIO15)
#define RELAY2_PIN     27     // Relay for load 2 (LOW = ON)
#define RELAY3_PIN     18    // Relay for load 3 (LOW = ON)
// Load 4 relay (GPIO5 is a strapping pin; keep relay input from pulling it LOW during boot)
#define RELAY4_PIN     23      // Relay for load 4 (LOW = ON)
#define RESET_BTN      4      // Reset kWh/cost

// Ultrasonic distance sensor (e.g., HC-SR04)
// NOTE: HC-SR04 ECHO is 5V -> use a voltage divider/level shifter to ESP32 3.3V.
#define US_TRIG_PIN    19
#define US_ECHO_PIN    13

// Humidity/Temperature sensor (DHT series)
// Wiring (typical): VCC=3.3V, GND=GND, DATA=GPIO25 with ~10K pull-up to 3.3V
// Note: This sketch does NOT require GPIO36.
#define DHT_PIN        25
// Sensor type
#define DHTTYPE        DHT22

// ==================== CONFIG ====================
const char ssid[] = "iQOO Neo9";
const char pass[] = "12345679";
const float RATE_PER_KWH = 6.5;
const float MAX_CURRENT = 20.0;  // Auto-off threshold per load
const float HUMIDITY_AUTO_ON_THRESHOLD_PCT = 90.0;
const float TEMP_AUTO_ON_THRESHOLD_C = 32.0;
const unsigned long RELAY1_RETRY_AFTER_TRIP_MS = 10000;  // avoid rapid on/off if overcurrent trips
const unsigned long RELAY2_RETRY_AFTER_TRIP_MS = 10000;  // avoid rapid on/off if overcurrent trips
const unsigned long RELAY3_RETRY_AFTER_TRIP_MS = 10000;  // avoid rapid on/off if overcurrent trips

const float ULTRASONIC_ON_DISTANCE_CM = 6.0;   // if distance > 8cm => Relay3 ON
const float ULTRASONIC_OFF_DISTANCE_CM = 3.0;  // keep ON until distance <= 4cm

// ACS712-30A (for both)
#define SAMPLES     1000
const float ADC_MAX = 4095.0f;
const float VREF = 3.3f;
// 30A -> 0.066 V/A (adjust if your module is 5A/20A)
const float ACS712_SENSITIVITY = 0.1f;

float currentOffset1 = 0.0f;
float currentOffset2 = 0.0f;
float currentOffset3 = 0.0f;

// ZMPT101B calibration
float V_CAL = 113.5;  // Tune as before

// ==================== VARIABLES ====================
float Vrms = 0, I1 = 0, I2 = 0, I3 = 0, totalI = 0, realPower = 0, kWh = 0, cost = 0;
unsigned long lastMillis = 0;
BlynkTimer timer;
WebServer server(80);

// ==================== WEB LOGIN (NGROK SAFE) ====================
// Change these before exposing to the internet.
const char* WEB_USERNAME = "admin";
const char* WEB_PASSWORD = "admin123";

static String webSessionToken = ""; // set after successful login

static String makeSessionToken() {
  // 128-bit token (32 hex chars)
  char buf[33];
  for (int i = 0; i < 4; i++) {
    uint32_t r = esp_random();
    snprintf(buf + (i * 8), 9, "%08lx", (unsigned long)r);
  }
  buf[32] = 0;
  return String(buf);
}

static bool isWebAuthed() {
  if (webSessionToken.length() == 0) return false;
  String cookie = server.header("Cookie");
  if (cookie.length() == 0) return false;
  String needle = "EGSESSION=" + webSessionToken;
  return cookie.indexOf(needle) >= 0;
}

static void sendRedirect(const char* where) {
  server.sendHeader("Location", where);
  server.send(302, "text/plain", "");
}

static bool requireAuthOrRedirect() {
  if (isWebAuthed()) return true;
  sendRedirect("/login");
  return false;
}

static bool requireAuthJson() {
  if (isWebAuthed()) return true;
  server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
  return false;
}

static bool requireAuthPlain() {
  if (isWebAuthed()) return true;
  server.send(401, "text/plain", "Unauthorized");
  return false;
}

// Reset button behavior
// Short press: reset kWh/cost counters
// Long press: reset counters + delete /data.csv (start new database)
const unsigned long RESET_LONG_PRESS_MS = 3000;
bool resetBtnWasDown = false;
unsigned long resetBtnDownSinceMs = 0;
bool longPressActionDone = false;

DHT dht(DHT_PIN, DHTTYPE);
float humidityPct = NAN;
float temperatureC = NAN;
unsigned long lastDhtReadMs = 0;
const unsigned long DHT_READ_INTERVAL_MS = 2000;

const bool PRINT_EXCEL_CSV_TO_SERIAL = true;
const bool PRINT_HUMAN_READABLE_TO_SERIAL = true;

// Relay states (true = ON, false = OFF)
bool relay1On = false;
bool relay2On = false;
bool relay3On = false;
bool relay4On = false;
unsigned long relay1OvercurrentTripMs = 0;
unsigned long relay2OvercurrentTripMs = 0;
unsigned long relay3OvercurrentTripMs = 0;
bool relay1AutoByTemp = false;
bool relay2AutoByHumidity = false;
bool relay3ManualOverride = false;  // when true, ultrasonic automation won't change Relay 3

float distanceCm = NAN;

void updateEverything();
void logToCSV();
void updateDhtCached();
void handleRoot();
void handleLoginPage();
void handleLoginPost();
void handleLogout();
void handleLiveApi();
void handleSummaryApi();
void handleRelayControl();
void handleSchedulesGet();
void handleSchedulesAdd();
void handleSchedulesDelete();

// ==================== SCHEDULER ====================
struct ScheduleItem {
  int id;
  int relay; // 1..3
  int year;
  int month;
  int day;
  int startMin; // minutes from midnight
  int endMin;   // minutes from midnight
  bool enabled;
};

const int MAX_SCHEDULES = 20;
ScheduleItem schedules[MAX_SCHEDULES];
int scheduleCount = 0;
int nextScheduleId = 1;

static bool parseDateYmd(const String& s, int* y, int* m, int* d) {
  if (!y || !m || !d) return false;
  if (s.length() < 10) return false;
  *y = s.substring(0, 4).toInt();
  *m = s.substring(5, 7).toInt();
  *d = s.substring(8, 10).toInt();
  if (*y < 2000 || *y > 2100) return false;
  if (*m < 1 || *m > 12) return false;
  if (*d < 1 || *d > 31) return false;
  return true;
}

static bool parseTimeHm(const String& s, int* outMin) {
  if (!outMin) return false;
  if (s.length() < 4) return false;
  int hh = s.substring(0, 2).toInt();
  int mm = s.substring(3, 5).toInt();
  if (hh < 0 || hh > 23) return false;
  if (mm < 0 || mm > 59) return false;
  *outMin = hh * 60 + mm;
  return true;
}

static String twoDigits(int n) {
  if (n < 0) n = 0;
  if (n > 99) n = 99;
  return (n < 10) ? ("0" + String(n)) : String(n);
}

static String fmtDate(int y, int m, int d) {
  return String(y) + "-" + twoDigits(m) + "-" + twoDigits(d);
}

static String fmtTime(int mins) {
  if (mins < 0) mins = 0;
  if (mins > 24 * 60) mins = 24 * 60;
  int hh = mins / 60;
  int mm = mins % 60;
  return twoDigits(hh) + ":" + twoDigits(mm);
}

static void loadSchedulesFromFs() {
  scheduleCount = 0;
  nextScheduleId = 1;
  if (!SPIFFS.exists("/schedules.csv")) return;
  File f = SPIFFS.open("/schedules.csv", FILE_READ);
  if (!f) return;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() < 5) continue;
    if (line.startsWith("id,")) continue;

    // CSV: id,relay,YYYY-MM-DD,HH:MM,HH:MM,enabled
    int parts[3] = {0, 0, 0};
    (void)parts;

    int c1 = line.indexOf(',');
    int c2 = line.indexOf(',', c1 + 1);
    int c3 = line.indexOf(',', c2 + 1);
    int c4 = line.indexOf(',', c3 + 1);
    int c5 = line.indexOf(',', c4 + 1);
    if (c1 < 0 || c2 < 0 || c3 < 0 || c4 < 0 || c5 < 0) continue;

    int id = line.substring(0, c1).toInt();
    int relay = line.substring(c1 + 1, c2).toInt();
    String date = line.substring(c2 + 1, c3);
    String startT = line.substring(c3 + 1, c4);
    String endT = line.substring(c4 + 1, c5);
    bool en = (line.substring(c5 + 1).toInt() == 1);

    int y, m, d;
    int sMin, eMin;
    if (!parseDateYmd(date, &y, &m, &d)) continue;
    if (!parseTimeHm(startT, &sMin)) continue;
    if (!parseTimeHm(endT, &eMin)) continue;
    if (relay < 1 || relay > 4) continue;
    if (eMin <= sMin) continue;

    if (scheduleCount >= MAX_SCHEDULES) break;
    schedules[scheduleCount++] = {id, relay, y, m, d, sMin, eMin, en};
    if (id >= nextScheduleId) nextScheduleId = id + 1;
  }
  f.close();
}

static void saveSchedulesToFs() {
  File f = SPIFFS.open("/schedules.csv", FILE_WRITE);
  if (!f) return;
  f.println("id,relay,date,start,end,enabled");
  for (int i = 0; i < scheduleCount; i++) {
    const ScheduleItem& it = schedules[i];
    f.printf("%d,%d,%s,%s,%s,%d\n",
             it.id,
             it.relay,
             fmtDate(it.year, it.month, it.day).c_str(),
             fmtTime(it.startMin).c_str(),
             fmtTime(it.endMin).c_str(),
             it.enabled ? 1 : 0);
  }
  f.close();
}

static void applyRelayStateFromSchedule(int relayNum, bool shouldOn) {
  bool* state = nullptr;
  int pin = -1;
  int blynkPin = -1;
  if (relayNum == 1) {
    state = &relay1On;
    pin = RELAY1_PIN;
    blynkPin = V5;
    relay1AutoByTemp = false;
  } else if (relayNum == 2) {
    state = &relay2On;
    pin = RELAY2_PIN;
    blynkPin = V6;
    relay2AutoByHumidity = false;
  } else if (relayNum == 3) {
    state = &relay3On;
    pin = RELAY3_PIN;
    blynkPin = V10;
    relay3ManualOverride = false;
  } else if (relayNum == 4) {
    state = &relay4On;
    pin = RELAY4_PIN;
    blynkPin = V12;
  } else {
    return;
  }

  if (*state == shouldOn) return;
  *state = shouldOn;
  digitalWrite(pin, shouldOn ? LOW : HIGH);
  if (Blynk.connected()) Blynk.virtualWrite(blynkPin, shouldOn ? 1 : 0);
}

static void applySchedulesNow() {
  if (scheduleCount <= 0) return;

  unsigned long epoch = timeClient.getEpochTime();
  time_t rawTime = (time_t)epoch;
  struct tm timeInfo;
  gmtime_r(&rawTime, &timeInfo);

  int y = timeInfo.tm_year + 1900;
  int m = timeInfo.tm_mon + 1;
  int d = timeInfo.tm_mday;
  int nowMin = timeInfo.tm_hour * 60 + timeInfo.tm_min;

  bool hasToday[5] = {false, false, false, false, false};
  bool shouldOn[5] = {false, false, false, false, false};

  for (int i = 0; i < scheduleCount; i++) {
    const ScheduleItem& it = schedules[i];
    if (!it.enabled) continue;
    if (it.year != y || it.month != m || it.day != d) continue;
    if (it.relay < 1 || it.relay > 4) continue;
    hasToday[it.relay] = true;
    if (nowMin >= it.startMin && nowMin < it.endMin) {
      shouldOn[it.relay] = true;
    }
  }

  // If there is any schedule for today for a relay, force ON during window and OFF outside.
  if (hasToday[1]) applyRelayStateFromSchedule(1, shouldOn[1]);
  if (hasToday[2]) applyRelayStateFromSchedule(2, shouldOn[2]);
  if (hasToday[3]) applyRelayStateFromSchedule(3, shouldOn[3]);
  if (hasToday[4]) applyRelayStateFromSchedule(4, shouldOn[4]);
}

static uint32_t parseDayKey(const String& ts) {
  // ts format: YYYY-MM-DD ...
  if (ts.length() < 10) return 0;
  int y = ts.substring(0, 4).toInt();
  int m = ts.substring(5, 7).toInt();
  int d = ts.substring(8, 10).toInt();
  if (y <= 0 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
  return (uint32_t)(y * 10000 + m * 100 + d);
}

static int daysInMonth(int year, int month) {
  static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (month < 1 || month > 12) return 30;
  int days = dim[month - 1];
  bool leap = ((year % 4) == 0) && (((year % 100) != 0) || ((year % 400) == 0));
  if (month == 2 && leap) days = 29;
  return days;
}

static bool computeAvgDailyKwhFromCsv(int lookbackDays, float* outAvgKwh) {
  if (!outAvgKwh) return false;
  if (lookbackDays < 1) lookbackDays = 1;
  if (lookbackDays > 60) lookbackDays = 60;
  if (!SPIFFS.exists("/data.csv")) return false;

  File file = SPIFFS.open("/data.csv", FILE_READ);
  if (!file) return false;

  float buf[60];
  int bufLen = 0;

  uint32_t curDay = 0;
  float dayStart = NAN;
  float dayEnd = NAN;

  auto pushUsed = [&](float used) {
    if (used < 0) used = 0;
    if (bufLen < 60) {
      buf[bufLen++] = used;
    } else {
      for (int i = 1; i < 60; i++) buf[i - 1] = buf[i];
      buf[59] = used;
    }
  };

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() < 10) continue;
    if (line.startsWith("Timestamp")) continue;

    String ts;
    float k = NAN;
    bool parsed = false;

    // Backward compatible parsing:
    // Old:  Timestamp,Epoch,Vrms_V,I1_A,I2_A,TotalI_A,Power_W,kWh,...
    // New:  Timestamp,Epoch,Vrms_V,I1_A,I2_A,I3_A,TotalI_A,Power_W,kWh,...
    for (int pass = 0; pass < 2 && !parsed; pass++) {
      const int skipCount = (pass == 0) ? 7 : 6; // after timestamp: skip epoch..power

      char tmp[200];
      size_t n = line.length();
      if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
      memcpy(tmp, line.c_str(), n);
      tmp[n] = 0;

      char* saveptr = nullptr;
      char* tok = strtok_r(tmp, ",", &saveptr); // timestamp
      if (!tok) continue;
      ts = String(tok);

      for (int i = 0; i < skipCount; i++) {
        tok = strtok_r(nullptr, ",", &saveptr);
        if (!tok) break;
      }
      tok = strtok_r(nullptr, ",", &saveptr); // kWh
      if (!tok) continue;
      char* endptr = nullptr;
      float candidate = strtof(tok, &endptr);
      if (endptr != tok) {
        k = candidate;
        parsed = true;
      }
    }
    if (!parsed) continue;

    uint32_t dayKey = parseDayKey(ts);
    if (dayKey == 0) continue;

    if (curDay == 0) {
      curDay = dayKey;
      dayStart = k;
      dayEnd = k;
      continue;
    }

    if (dayKey != curDay) {
      if (!isnan(dayStart) && !isnan(dayEnd)) {
        pushUsed(dayEnd - dayStart);
      }
      curDay = dayKey;
      dayStart = k;
      dayEnd = k;
    } else {
      if (isnan(dayStart)) dayStart = k;
      dayEnd = k;
    }
  }
  file.close();

  if (curDay != 0 && !isnan(dayStart) && !isnan(dayEnd)) {
    pushUsed(dayEnd - dayStart);
  }

  if (bufLen <= 0) return false;

  int useN = (bufLen < lookbackDays) ? bufLen : lookbackDays;
  float sum = 0;
  for (int i = bufLen - useN; i < bufLen; i++) sum += buf[i];
  *outAvgKwh = sum / (float)useN;
  return true;
}

static String currentTimestampLocal() {
  unsigned long epoch = timeClient.getEpochTime();
  time_t rawTime = (time_t)epoch;
  struct tm timeInfo;
  gmtime_r(&rawTime, &timeInfo);
  char timestamp[32];
  snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
           timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
           timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);
  return String(timestamp);
}

void handleRoot() {
  if (!requireAuthOrRedirect()) return;
  // Professional dark-theme dashboard with relay controls
  const char* page = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <meta name="color-scheme" content="dark" />
  <title>Energy Meter Dashboard</title>
  <style>
    :root{
      --bg:#0d1117;
      --card:#161b22;
      --card-hover:#1c2128;
      --border:#30363d;
      --fg:#e6edf3;
      --muted:#8b949e;
      --accent:#58a6ff;
      --accent-glow:rgba(88,166,255,0.15);
      --green:#3fb950;
      --green-glow:rgba(63,185,80,0.2);
      --red:#f85149;
      --red-glow:rgba(248,81,73,0.2);
      --yellow:#d29922;
      --purple:#a371f7;
      --gradient:linear-gradient(135deg,#58a6ff 0%,#a371f7 100%);
    }
    *{box-sizing:border-box;margin:0;padding:0}
    body{background:var(--bg);color:var(--fg);font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;min-height:100vh}
    .wrap{max-width:1100px;margin:0 auto;padding:20px}
    .header{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:16px;margin-bottom:20px;padding-bottom:16px;border-bottom:1px solid var(--border)}
    .logo{display:flex;align-items:center;gap:12px}
    .logo-icon{width:42px;height:42px;border-radius:12px;background:var(--gradient);display:flex;align-items:center;justify-content:center;font-size:20px}
    .logo h1{font-size:20px;font-weight:700;background:var(--gradient);-webkit-background-clip:text;-webkit-text-fill-color:transparent;background-clip:text}
    .logo .sub{color:var(--muted);font-size:12px;margin-top:2px}
    .status-pill{display:flex;align-items:center;gap:8px;background:var(--card);border:1px solid var(--border);border-radius:999px;padding:8px 14px;font-size:13px;color:var(--muted);transition:all .2s}
    .status-pill:hover{border-color:var(--accent)}
    .dot{width:8px;height:8px;border-radius:50%;background:var(--border);transition:all .3s}
    .dot.ok{background:var(--green);box-shadow:0 0 8px var(--green)}
    .grid{display:grid;grid-template-columns:1fr;gap:16px}
    /* Desktop layout: fixed right column so chart/summary match exact width */
    @media(min-width:900px){
      .wrap{max-width:1160px}
      .grid{grid-template-columns:380px 733.6px}
    }
    .right-col{display:flex;flex-direction:column;gap:20px}
    .card{background:var(--card);border:1px solid var(--border);border-radius:16px;padding:16px;transition:all .25s ease}
    .card:hover{border-color:var(--accent);box-shadow:0 0 20px var(--accent-glow)}
    .card-title{display:flex;align-items:center;gap:8px;font-size:13px;font-weight:600;text-transform:uppercase;letter-spacing:.06em;color:var(--accent);margin-bottom:14px}
    .card-title svg{width:16px;height:16px}
    .metrics{display:grid;gap:10px}
    .metric{background:var(--bg);border:1px solid var(--border);border-radius:12px;padding:12px;transition:all .2s}
    .metric:hover{border-color:var(--purple);transform:translateY(-2px);box-shadow:0 4px 12px rgba(0,0,0,.3)}
    .metric .label{font-size:11px;text-transform:uppercase;letter-spacing:.05em;color:#ffffff;margin-bottom:4px;font-weight: 500;}
    .metric .value{font-size:20px;font-weight:700;color:var(--fg)}
    .metric .value.accent{color:var(--accent)}
    .metric .value.green{color:var(--green)}
    .metric .value.yellow{color:var(--yellow)}
    .metric .hint{font-size:11px;color:var(--muted);margin-top:4px}

    /* Auto-start alert bars (Humidity/Temp) */
    .auto-alert{margin-top:10px;border-top:1px dashed var(--border);padding-top:10px;display:none}
    .auto-alert.show{display:block}
    .auto-text{margin-top:8px;font-size:12px;font-weight:900;letter-spacing:.02em;color:var(--fg)}
    .auto-text .muted{color:var(--muted);font-weight:800}
    .auto-line{height:10px;border-radius:999px;background:var(--card);border:1px solid var(--border);overflow:hidden;position:relative}
    .auto-line::after{content:'';position:absolute;inset:-2px;border-radius:999px;pointer-events:none;opacity:.35;filter:blur(10px)}
    .auto-line.hum::after{background:var(--green-glow);animation:glow 1.2s ease-in-out infinite}
    .auto-line.temp::after{background:var(--accent-glow);animation:glow 1.2s ease-in-out infinite}
    .auto-line .flow{position:absolute;top:0;left:-60%;height:100%;width:60%;border-radius:999px}
    .auto-line .flow.hum{background:linear-gradient(90deg,var(--green),var(--accent),var(--purple));box-shadow:0 0 18px var(--green-glow);animation:flow 1.05s ease-in-out infinite}
    .auto-line .flow.temp{background:linear-gradient(90deg,var(--yellow),var(--red),var(--purple));box-shadow:0 0 18px var(--accent-glow);animation:flow 1.05s ease-in-out infinite}
    @keyframes flow{0%{transform:translateX(0);opacity:.65}50%{opacity:1}100%{transform:translateX(220%);opacity:.75}}
    @keyframes glow{0%,100%{opacity:.28}50%{opacity:.55}}

    .relay-section{margin-top:16px}
    .relay-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
    .relay-card{background:var(--bg);border:1px solid var(--border);border-radius:12px;padding:14px;text-align:center;transition:all .25s}
    .relay-card:hover{border-color:var(--accent)}
    .relay-card.on{border-color:var(--green);box-shadow:0 0 16px var(--green-glow);padding-bottom: 16px}
    .relay-card.off{border-color:var(--red);box-shadow:0 0 16px var(--red-glow);padding-bottom: 16px}
    .relay-label{font-size:15px;text-transform:uppercase;font-weight:400px;letter-spacing:.04em;color:#ffff;margin-bottom:8px}
    .relay-status{font-size:15px;font-weight:700;margin-bottom:10px}
    .relay-metric{font-size:14px;color:#fffff;font-weight:300px;margin:-4px 0 10px}
    .relay-status.on{color:var(--green)}
    .relay-status.off{color:var(--red)}
    .toggle{position:relative;width:56px;height:28px;background:var(--border);border-radius:999px;cursor:pointer;transition:all .3s;margin:0 auto}
    .toggle.on{background:var(--green)}
    .toggle::after{content:'';position:absolute;top:3px;left:3px;width:22px;height:22px;background:#fff;border-radius:50%;transition:all .3s;box-shadow:0 2px 4px rgba(0,0,0,.3)}
    .toggle.on::after{left:31px}
    .toggle:hover{opacity:.85}
    .actions{display:flex;gap:10px;flex-wrap:wrap;margin-top:16px}
    .btn{display:inline-flex;align-items:center;justify-content:center;gap:8px;padding:12px 16px;background:var(--accent);color:#fff;border:none;border-radius:12px;font-size:13px;font-weight:600;text-decoration:none;cursor:pointer;transition:all .2s}
    .btn:hover{opacity:.9;transform:translateY(-1px);box-shadow:0 4px 12px var(--accent-glow)}
    .btn.secondary{background:transparent;border:1px solid var(--border);color:var(--fg)}
    .btn.secondary:hover{border-color:var(--accent);color:var(--accent)}
    .note{font-size:11px;color:var(--muted);margin-top:12px}
    .note code{background:var(--bg);padding:2px 6px;border-radius:4px;font-family:ui-monospace,monospace}
    .chart-card{display:flex;flex-direction:column;width:100%}
    @media(min-width:900px){.chart-card{height:383.6px; width: 722.6px;}}
    .chart-header{display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:10px;margin-bottom:12px}
    .chart-value{font-size:28px;font-weight:700;color:var(--accent)}
    .chart-range{font-size:12px;color:var(--muted)}
    .canvas-wrap{flex:1;background:var(--bg);border:1px solid var(--border);border-radius:12px;padding:10px;min-height:260px}
    canvas{display:block;width:100%;height:100%}
    /* Summary card styles (layout handled by .right-col) */
    .summary-controls{display:flex;flex-wrap:wrap;gap:10px;align-items:center;background:var(--bg);border:1px solid var(--border);border-radius:12px;padding:10px}
    .seg{display:flex;gap:8px;flex-wrap:wrap;}
    .btn-small{display:inline-flex;align-items:center;justify-content:center;gap:8px;padding:10px 12px;border-radius:12px;border:1px solid var(--border);background:var(--card);color:var(--fg);text-decoration:none;font-weight:700;cursor:pointer;transition:all .2s}
    .btn-small:hover{background:var(--card-hover)}
    .btn-small.active{border-color:rgba(88,166,255,0.8);box-shadow:0 0 0 3px var(--accent-glow)}
    .days{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
    .input{width:120px;max-width:40vw;padding:10px 12px;border-radius:12px;border:1px solid var(--border);background:var(--card);color:var(--fg);outline:none}
    .input:focus{border-color:rgba(88,166,255,0.8);box-shadow:0 0 0 3px var(--accent-glow)}
    .summary-out{margin-top:12px;display:grid;grid-template-columns:1fr 1fr;gap:10px}
    .summary-box{background:var(--bg);border:1px solid var(--border);border-radius:12px;padding:12px;transition:all .2s}
    .summary-box:hover{border-color:var(--purple);transform:translateY(-2px);box-shadow:0 4px 12px rgba(0,0,0,.3)}
    .summary-k{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.05em}
    .summary-v{margin-top:6px;font-size:18px;font-weight:800}
    #sumKwh{color:var(--accent)}
    #sumCost{color:var(--green)}
    @media (max-width:720px){.summary-out{grid-template-columns:1fr}}
    .sched-list{margin-top:12px;display:grid;grid-template-columns:1fr;gap:10px}
    .sched-row{display:flex;align-items:center;justify-content:space-between;gap:10px;flex-wrap:wrap}
    .footer{margin-top:20px;padding-top:16px;border-top:1px solid var(--border);display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:10px;font-size:12px;color:var(--muted)}
    .footer a{color:var(--accent);text-decoration:none}
    .footer a:hover{text-decoration:underline}
    @keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}
    .loading{animation:pulse 1.5s infinite}
  </style>
</head>
<body>
  <div class="wrap">
    <header class="header">
      <div class="logo">
        <div class="logo-icon">⚡</div>
        <div>
          <h1>EnerGaze</h1>
          <div class="sub">Smart Energy Monitor Dashboard</div>
        </div>
      </div>
      <div class="status-pill">
        <span class="dot" id="dot"></span>
        <span id="status">Connecting…</span>
      </div>
    </header>

    <div class="grid">
      <div class="card">
        <div class="card-title">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M13 2L3 14h9l-1 8 10-12h-9l1-8z"/></svg>
          Live Metrics
        </div>
        <div class="metrics">
          <div class="metric">
            <div class="label">🕐 Timestamp (Local)</div>
            <div class="value" id="ts">-</div>
          </div>
          <div class="metric">
            <div class="label">⚡ Voltage</div>
            <div class="value accent" id="v">-</div>
          </div>
          <div class="metric">
            <div class="label">🔌 Total Current</div>
            <div class="value" id="it">-</div>
          </div>
          <div class="metric">
            <div class="label">💡 Power</div>
            <div class="value yellow" id="p">-</div>
          </div>
          <div class="metric">
            <div class="label">💰 Energy / Cost</div>
            <div class="value green" id="e">-</div>
          </div>
          <div class="metric">
            <div class="label">💧 Humidity</div>
            <div class="value accent" id="hum">-</div>
            <div class="auto-alert" id="humAuto">
              <div class="auto-line hum"><span class="flow hum"></span></div>
              <div class="auto-text">⚡ Humidity High <span class="muted">— Exhaust fan started automatically</span></div>
            </div>
          </div>
          <div class="metric">
            <div class="label">🌡️ Temperature</div>
            <div class="value yellow" id="temp">-</div>
            <div class="auto-alert" id="tempAuto">
              <div class="auto-line temp"><span class="flow temp"></span></div>
              <div class="auto-text">⚡ Temperature High <span class="muted">— AC/FAN started automatically</span></div>
            </div>
          </div>
        </div>

        <div class="relay-section">
          <div class="card-title">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="11" width="18" height="10" rx="2"/><circle cx="12" cy="5" r="2"/><path d="M12 7v4"/></svg>
            Load Control
          </div>
          <div class="relay-grid">
            <div class="relay-card" id="r1card">
              <div class="relay-label">AC/FAN</div>
              <div class="relay-metric" id="r1i">I1: - A</div>
              <div class="relay-status" id="r1status">-</div>
              <div class="toggle" id="r1toggle" onclick="toggleRelay(1)"></div>
            </div>
            <div class="relay-card" id="r2card">
              <div class="relay-label">Exhaust Fan</div>
              <div class="relay-metric" id="r2i">I2: - A</div>
              <div class="relay-status" id="r2status">-</div>
              <div class="toggle" id="r2toggle" onclick="toggleRelay(2)"></div>
            </div>
            <div class="relay-card" id="r3card">
              <div class="relay-label">Water Pump</div>
              <div class="relay-metric" id="r3i">I3: - A</div>
              <div class="relay-status" id="r3status">-</div>
              <div class="toggle" id="r3toggle" onclick="toggleRelay(3)"></div>
            </div>
            <div class="relay-card" id="r4card">
              <div class="relay-label">Light</div>
              <div class="relay-metric" id="r4i">I4: -</div>
              <div class="relay-status" id="r4status">-</div>
              <div class="toggle" id="r4toggle" onclick="toggleRelay(4)"></div>
            </div>
          </div>
        </div>

        <div class="actions">
          <a class="btn" href="/data.csv">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 15v4a2 2 0 01-2 2H5a2 2 0 01-2-2v-4M7 10l5 5 5-5M12 15V3"/></svg>
            Download CSV
          </a>
        </div>
        <div class="note">API: <code>/api</code> · Control: <code>POST /relay</code></div>
      </div>

      <div class="right-col">
        <div class="card chart-card">
          <div class="card-title">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/></svg>
            Power Trend
          </div>
          <div class="chart-header">
            <div class="chart-value" id="pwNow">- W</div>
            <div class="chart-range" id="range">last ~60 seconds</div>
          </div>
          <div class="canvas-wrap">
            <canvas id="chart" width="900" height="320"></canvas>
          </div>
        </div>
        <div class="card summary-card" style="width:722.6px;">
          <div class="card-title">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M3 3v18h18"/><path d="M7 14l3-3 3 2 5-6"/></svg>
            Consumption &amp; Cost
          </div>
          <div class="summary-controls">
            <div class="days">
              <span style="color:var(--muted);font-size:12px;font-weight:700">Day</span>
              <input class="input" id="pickDay" type="date" />
              <button class="btn-small" onclick="calcDay()">Calculate</button>
            </div>
            <div class="days">
              <span style="color:var(--muted);font-size:12px;font-weight:700">Month</span>
              <input class="input" id="pickMonth" type="month" />
              <button class="btn-small" onclick="calcMonth()">Calculate</button>
            </div>
            <div class="days">
              <span style="color:var(--muted);font-size:12px;font-weight:700">Year</span>
              <input class="input" id="pickYear" type="number" min="2000" max="2100" />
              <button class="btn-small" onclick="calcYear()">Calculate</button>
            </div>
            <div class="days">
              <span style="color:var(--muted);font-size:12px;font-weight:700">Range</span>
              <input class="input" id="rangeStart" type="date" />
              <input class="input" id="rangeEnd" type="date" />
              <button class="btn-small" onclick="calcRange()">Calculate</button>
            </div>
          </div>
          <div class="summary-out">
            <div class="summary-box">
              <div class="summary-k" id="sumRange">Range: -</div>
              <div class="summary-v" id="sumKwh">- kWh</div>
            </div>
            <div class="summary-box">
              <div class="summary-k">Cost</div>
              <div class="summary-v" id="sumCost">-</div>
            </div>
          </div>
        </div>

        <div class="card" style="width:722.6px;">
          <div class="card-title">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M8 7V3m8 4V3M4 11h16M6 5h12a2 2 0 012 2v14a2 2 0 01-2 2H6a2 2 0 01-2-2V7a2 2 0 012-2z"/></svg>
            Schedules
          </div>
          <div class="summary-controls">
            <div class="days">
              <span style="color:var(--muted);font-size:12px;font-weight:700">Relay</span>
              <select class="input" id="schedRelay" style="width:120px">
                <option value="1">AC/FAN</option>
                <option value="2">Exhaust Fan</option>
                <option value="3">Submersible Pump</option>
                <option value="4">Light</option>
              </select>
            </div>
            <div class="days">
              <span style="color:var(--muted);font-size:12px;font-weight:700">Date</span>
              <input class="input" id="schedDate" type="date" />
            </div>
            <div class="days">
              <span style="color:var(--muted);font-size:12px;font-weight:700">Start</span>
              <input class="input" id="schedStart" type="time" />
            </div>
            <div class="days">
              <span style="color:var(--muted);font-size:12px;font-weight:700">End</span>
              <input class="input" id="schedEnd" type="time" />
            </div>
            <button class="btn-small" onclick="addSchedule()">Add</button>
          </div>
          <div class="sched-list" id="schedList">
            <div class="summary-box"><div class="summary-k">No schedules yet</div></div>
          </div>
        </div>
      </div>
    </div>

    <footer class="footer">
      <div>ESP32 Energy Meter · <a href="/api">JSON API</a></div>
      <div id="wifi">WiFi: Connected</div>
    </footer>
  </div>

  <script>
    const el = (id) => document.getElementById(id);
    const ctx = el('chart').getContext('2d');
    const history = [];
    const MAX = 60;

    function setStatus(isOk, text){
      el('status').textContent = text;
      el('dot').className = isOk ? 'dot ok' : 'dot';
    }

    function updateRelayUI(r1, r2, r3, r4){
      el('r1card').className = 'relay-card ' + (r1 ? 'on' : 'off');
      el('r2card').className = 'relay-card ' + (r2 ? 'on' : 'off');
      el('r3card').className = 'relay-card ' + (r3 ? 'on' : 'off');
      el('r4card').className = 'relay-card ' + (r4 ? 'on' : 'off');
      el('r1status').textContent = r1 ? 'ON' : 'OFF';
      el('r2status').textContent = r2 ? 'ON' : 'OFF';
      el('r3status').textContent = r3 ? 'ON' : 'OFF';
      el('r4status').textContent = r4 ? 'ON' : 'OFF';
      el('r1status').className = 'relay-status ' + (r1 ? 'on' : 'off');
      el('r2status').className = 'relay-status ' + (r2 ? 'on' : 'off');
      el('r3status').className = 'relay-status ' + (r3 ? 'on' : 'off');
      el('r4status').className = 'relay-status ' + (r4 ? 'on' : 'off');
      el('r1toggle').className = 'toggle ' + (r1 ? 'on' : '');
      el('r2toggle').className = 'toggle ' + (r2 ? 'on' : '');
      el('r3toggle').className = 'toggle ' + (r3 ? 'on' : '');
      el('r4toggle').className = 'toggle ' + (r4 ? 'on' : '');
    }

    async function toggleRelay(num){
      try{
        const r = await fetch('/relay', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          body: 'relay=' + num + '&action=toggle'
        });
        const d = await r.json();
        updateRelayUI(d.relay1, d.relay2, d.relay3, d.relay4);
      }catch(e){ console.error(e); }
    }

    async function fetchSummary(query){
      try{
        const url = (typeof query === 'string')
          ? ('/summary?days=' + encodeURIComponent(query))
          : ('/summary?' + new URLSearchParams(query).toString());
        const r = await fetch(url, {cache:'no-store'});
        const s = await r.json();
        if(s && s.status === 'future'){
          const lbl = s.label ? String(s.label) : '';
          el('sumRange').textContent = lbl ? (`Range: ${lbl} · Not reached yet`) : 'Range: Not reached yet';
          el('sumKwh').textContent = '- kWh';
          el('sumCost').textContent = '-';
          return;
        }
        if(s && s.status === 'predicted'){
          const lbl = s.label ? String(s.label) : '';
          el('sumRange').textContent = lbl ? (`Range: ${lbl} · Predicted`) : 'Range: Predicted';
          el('sumKwh').textContent = `${Number(s.kwh_used).toFixed(4)} kWh`;
          el('sumCost').textContent = `৳${Number(s.cost).toFixed(2)}`;
          return;
        }
        el('sumRange').textContent = s.label ? (`Range: ${s.label}`) : (s.days ? `Range: last ${s.days} day(s)` : 'Range: -');
        el('sumKwh').textContent = `${Number(s.kwh_used).toFixed(4)} kWh`;
        el('sumCost').textContent = `৳${Number(s.cost).toFixed(2)}`;
      }catch(e){
        el('sumRange').textContent = 'Range: -';
        el('sumKwh').textContent = '- kWh';
        el('sumCost').textContent = '-';
      }
    }

    function pad2(n){ return String(n).padStart(2,'0'); }
    function todayStr(){
      const d = new Date();
      return `${d.getFullYear()}-${pad2(d.getMonth()+1)}-${pad2(d.getDate())}`;
    }
    function monthStr(){
      const d = new Date();
      return `${d.getFullYear()}-${pad2(d.getMonth()+1)}`;
    }

    function initPickers(){
      const t = todayStr();
      const m = monthStr();
      el('pickDay').value = t;
      el('pickMonth').value = m;
      el('pickYear').value = String(new Date().getFullYear());
      el('rangeStart').value = t;
      el('rangeEnd').value = t;

      // scheduler defaults
      el('schedDate').value = t;
      el('schedStart').value = '08:00';
      el('schedEnd').value = '09:00';
    }

    async function loadSchedules(){
      try{
        const r = await fetch('/schedules', {cache:'no-store'});
        const d = await r.json();
        renderSchedules(d.items || []);
      }catch(e){
        renderSchedules([]);
      }
    }

    function renderSchedules(items){
      const box = el('schedList');
      if(!items || items.length === 0){
        box.innerHTML = '<div class="summary-box"><div class="summary-k">No schedules yet</div></div>';
        return;
      }
      box.innerHTML = '';
      items.forEach(it => {
        const row = document.createElement('div');
        row.className = 'summary-box';
        const relayLabel = (it.relay === 1) ? 'Load 1'
                          : (it.relay === 2) ? 'Load 2'
                          : (it.relay === 3) ? 'Load 3'
                          : 'Load 4';
        row.innerHTML = `
          <div class="sched-row">
            <div>
              <div class="summary-k">${relayLabel} · ${it.date} · ${it.start} - ${it.end}</div>
              <div class="summary-v">${it.enabled ? 'Enabled' : 'Disabled'}</div>
            </div>
            <button class="btn-small" onclick="deleteSchedule(${Number(it.id)})">Delete</button>
          </div>
        `;
        box.appendChild(row);
      });
    }

    async function addSchedule(){
      const relay = el('schedRelay').value;
      const date = el('schedDate').value;
      const start = el('schedStart').value;
      const end = el('schedEnd').value;
      if(!relay || !date || !start || !end){
        return;
      }
      try{
        await fetch('/schedules/add', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          body: `relay=${encodeURIComponent(relay)}&date=${encodeURIComponent(date)}&start=${encodeURIComponent(start)}&end=${encodeURIComponent(end)}`
        });
      }catch(e){}
      loadSchedules();
    }

    async function deleteSchedule(id){
      if(!id) return;
      try{
        await fetch('/schedules/delete', {
          method: 'POST',
          headers: {'Content-Type': 'application/x-www-form-urlencoded'},
          body: `id=${encodeURIComponent(String(id))}`
        });
      }catch(e){}
      loadSchedules();
    }

    function calcDay(){
      const d = el('pickDay').value || todayStr();
      fetchSummary({mode:'day', date:d});
    }
    function calcMonth(){
      const m = el('pickMonth').value || monthStr();
      fetchSummary({mode:'month', month:m});
    }
    function calcYear(){
      const y = String(parseInt(el('pickYear').value || String(new Date().getFullYear()), 10));
      fetchSummary({mode:'year', year:y});
    }
    function calcRange(){
      const s = el('rangeStart').value;
      const e = el('rangeEnd').value;
      if(!s || !e){
        el('sumRange').textContent = 'Range: select start & end';
        return;
      }
      fetchSummary({start:s, end:e});
    }

    function draw(){
      const w = ctx.canvas.width;
      const h = ctx.canvas.height;
      ctx.clearRect(0,0,w,h);
      ctx.fillStyle = '#0d1117';
      ctx.fillRect(0,0,w,h);

      if(history.length < 2) return;

      const ys = history.map(p => p.p);
      const maxY = Math.max(...ys) || 1;
      const minY = Math.min(...ys) || 0;
      const span = Math.max(1, maxY - minY);

      const padL = 45, padR = 12, padT = 14, padB = 28;
      const plotW = w - padL - padR;
      const plotH = h - padT - padB;

      // grid
      ctx.strokeStyle = '#30363d';
      ctx.lineWidth = 1;
      for(let i=0;i<=4;i++){
        const y = padT + (i/4)*plotH;
        ctx.beginPath();
        ctx.moveTo(padL, y);
        ctx.lineTo(padL + plotW, y);
        ctx.stroke();
      }

      // y labels
      ctx.fillStyle = '#8b949e';
      ctx.font = '11px system-ui';
      ctx.textAlign = 'right';
      ctx.textBaseline = 'middle';
      for(let i=0;i<=4;i++){
        const y = padT + (i/4)*plotH;
        const value = maxY - (i/4)*span;
        ctx.fillText(value.toFixed(0), padL - 8, y);
      }

      // gradient fill under line
      const grad = ctx.createLinearGradient(0, padT, 0, padT + plotH);
      grad.addColorStop(0, 'rgba(88,166,255,0.3)');
      grad.addColorStop(1, 'rgba(88,166,255,0)');
      ctx.fillStyle = grad;
      ctx.beginPath();
      ctx.moveTo(padL, padT + plotH);
      history.forEach((pt, idx) => {
        const x = padL + (idx/(MAX-1))*plotW;
        const y = padT + (1 - (pt.p - minY)/span)*plotH;
        ctx.lineTo(x, y);
      });
      ctx.lineTo(padL + ((history.length-1)/(MAX-1))*plotW, padT + plotH);
      ctx.closePath();
      ctx.fill();

      // line
      ctx.strokeStyle = '#58a6ff';
      ctx.lineWidth = 2.5;
      ctx.lineJoin = 'round';
      ctx.beginPath();
      history.forEach((pt, idx) => {
        const x = padL + (idx/(MAX-1))*plotW;
        const y = padT + (1 - (pt.p - minY)/span)*plotH;
        if(idx === 0) ctx.moveTo(x,y); else ctx.lineTo(x,y);
      });
      ctx.stroke();

      // glow dot
      const last = history[history.length - 1];
      if(last){
        const x = padL + ((history.length-1)/(MAX-1))*plotW;
        const y = padT + (1 - (last.p - minY)/span)*plotH;
        ctx.fillStyle = '#58a6ff';
        ctx.shadowColor = '#58a6ff';
        ctx.shadowBlur = 10;
        ctx.beginPath();
        ctx.arc(x, y, 5, 0, Math.PI*2);
        ctx.fill();
        ctx.shadowBlur = 0;
      }

      el('range').textContent = `min ${minY.toFixed(0)} W / max ${maxY.toFixed(0)} W`;
    }

    function fmt(v, d){
      if(v === null || v === undefined) return '-';
      const n = Number(v);
      return Number.isNaN(n) ? '-' : n.toFixed(d);
    }

    function setAlertVisible(id, show){
      const node = el(id);
      if(!node) return;
      node.className = show ? 'auto-alert show' : 'auto-alert';
    }

    async function tick(){
      try{
        const r = await fetch('/api', {cache:'no-store'});
        const d = await r.json();
        setStatus(true, 'Live');
        el('ts').textContent = d.timestamp ?? '-';
        el('v').textContent = `${fmt(d.vrms_v, 1)} V`;
          el('it').textContent = `${fmt(d.total_i_a, 2)} A`;
          el('r1i').textContent = `I1: ${fmt(d.i1_a, 2)} A`;
          el('r2i').textContent = `I2: ${fmt(d.i2_a, 2)} A`;
          el('r3i').textContent = `I3: ${fmt(d.i3_a, 2)} A`;
          el('r4i').textContent = `I4: -`;

        el('p').textContent = `${fmt(d.power_w, 1)} W`;
        el('pwNow').textContent = `${fmt(d.power_w, 1)} W`;
        el('e').innerHTML = `${fmt(d.kwh, 4)} kWh&nbsp;&nbsp;&nbsp·&nbsp;&nbsp;&nbsp৳${fmt(d.cost, 2)}`;
        el('hum').textContent = `${fmt(d.humidity_pct, 1)} %`;
        el('temp').textContent = `${fmt(d.temp_c, 1)} °C`;
        updateRelayUI(d.relay1, d.relay2, d.relay3, d.relay4);

        const hum = Number(d.humidity_pct);
        const tmp = Number(d.temp_c);
        const humThr = (d.hum_thr === null || d.hum_thr === undefined) ? 90 : Number(d.hum_thr);
        const tmpThr = (d.temp_thr === null || d.temp_thr === undefined) ? 28 : Number(d.temp_thr);

        const humAuto = Boolean(d.auto_humidity) && Boolean(d.relay2) && !Number.isNaN(hum) && hum >= humThr;
        const tmpAuto = Boolean(d.auto_temp) && Boolean(d.relay1) && !Number.isNaN(tmp) && tmp >= tmpThr;
        setAlertVisible('humAuto', humAuto);
        setAlertVisible('tempAuto', tmpAuto);

        const pw = Number(d.power_w);
        if(!Number.isNaN(pw)){
          history.push({t:Date.now(), p:pw});
          while(history.length > MAX) history.shift();
          draw();
        }
      }catch(e){
        setStatus(false, 'Reconnecting…');
      }
    }

    tick();
    setInterval(tick, 1000);

    initPickers();
    calcDay();
    loadSchedules();
  </script>
</body>
</html>
  )HTML";
  server.send(200, "text/html", page);
}

void handleLoginPage() {
  if (isWebAuthed()) {
    sendRedirect("/");
    return;
  }

  bool showErr = server.hasArg("err");
  String page =
    "<!doctype html><html lang='en'><head>"
    "<meta charset='utf-8'/><meta name='viewport' content='width=device-width,initial-scale=1'/>"
    "<meta name='color-scheme' content='dark'/><title>Login</title>"
    "<style>"
    ":root{--bg:#0d1117;--card:#161b22;--border:#30363d;--fg:#e6edf3;--muted:#8b949e;--accent:#58a6ff;--accent-glow:rgba(88,166,255,.15);--red:#f85149;}"
    "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}"
    ".wrap{width:min(420px,100%);display:flex;flex-direction:column;align-items:center;gap:10px}"
    ".card{width:100%;background:var(--card);border:1px solid var(--border);border-radius:16px;padding:18px;box-shadow:0 0 20px var(--accent-glow)}"
    ".brand{text-align:center;font-weight:950;font-size:35px;letter-spacing:.08em;color:var(--accent);margin:0}"
    ".brand-sub{text-align:center;color:var(--muted);font-size:17px;font-weight:800;margin:-6px 0 4px;margin-bottom: 50px}"
    ".title{font-size:18px;font-weight:800;margin:0 0 6px}"
    ".sub{color:var(--muted);font-size:12px;margin:0 0 14px}"
    ".row{display:grid;gap:10px}"
    ".label{font-size:11px;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);font-weight:800}"
    ".input{width:100%;padding:12px 12px;border-radius:12px;border:1px solid var(--border);background:var(--bg);color:var(--fg);outline:none}"
    ".input:focus{border-color:rgba(88,166,255,.8);box-shadow:0 0 0 3px var(--accent-glow)}"
    ".pw{position:relative}"
    ".pw .input{padding-right:52px}"
    ".pw-btn{position:absolute;right:0px;top:50%;transform:translateY(-50%);width:40px;height:40px;border-radius:12px;background:#161b2200;color:var(--fg);cursor:pointer;display:flex;align-items:center;justify-content:center;font-size:16px}"
    ".btn{margin-top:8px;width:100%;padding:12px 14px;border-radius:12px;border:none;background:var(--accent);color:#fff;font-weight:900;cursor:pointer}"
    ".err{margin-top:10px;color:var(--red);font-weight:800;font-size:12px;display:";

  page += (showErr ? "block" : "none");
  page += ";}"
          "</style></head><body>"
        "<div class='wrap'>"
        "<div class='brand'>EnerGaze</div>"
        "<div class='brand-sub'>Smart Energy Monitor</div>"
        "<div class='card'>"
          "<h1 class='title'>🔐 Dashboard Login</h1>"
          "<p class='sub'>Enter username &amp; password to open the dashboard.</p>"
          "<form method='POST' action='/login' class='row'>"
          "<div><div class='label'>Username</div><input class='input' name='user' autocomplete='username' required></div>"
      "<div><div class='label'>Password</div><div class='pw'><input class='input' id='pass' name='pass' type='password' autocomplete='current-password' required><button class='pw-btn' id='pwBtn' type='button' onclick='togglePw()' aria-label='Show password'>👁️</button></div></div>"
          "<button class='btn' type='submit'>Login</button>"
          "<div class='err'>❌ Wrong username or password</div>"
          "</form>"
      "<script>function togglePw(){var i=document.getElementById('pass');var b=document.getElementById('pwBtn');if(!i||!b)return;if(i.type==='password'){i.type='text';b.textContent='🙈';b.setAttribute('aria-label','Hide password');}else{i.type='password';b.textContent='👁️';b.setAttribute('aria-label','Show password');}}</script>"
        "</div></div></body></html>";

  server.send(200, "text/html", page);
}

void handleLoginPost() {
  if (!server.hasArg("user") || !server.hasArg("pass")) {
    sendRedirect("/login?err=1");
    return;
  }
  String u = server.arg("user");
  String p = server.arg("pass");

  if (u == WEB_USERNAME && p == WEB_PASSWORD) {
    webSessionToken = makeSessionToken();
    server.sendHeader("Set-Cookie", "EGSESSION=" + webSessionToken + "; Path=/; HttpOnly; SameSite=Lax");
    sendRedirect("/");
    return;
  }
  sendRedirect("/login?err=1");
}

void handleLogout() {
  webSessionToken = "";
  server.sendHeader("Set-Cookie", "EGSESSION=deleted; Path=/; Max-Age=0");
  sendRedirect("/login");
}

void handleSchedulesGet() {
  if (!requireAuthJson()) return;
  String json = "{\"items\":[";
  for (int i = 0; i < scheduleCount; i++) {
    const ScheduleItem& it = schedules[i];
    if (i) json += ",";
    json += "{";
    json += "\"id\":" + String(it.id) + ",";
    json += "\"relay\":" + String(it.relay) + ",";
    json += "\"date\":\"" + fmtDate(it.year, it.month, it.day) + "\",";
    json += "\"start\":\"" + fmtTime(it.startMin) + "\",";
    json += "\"end\":\"" + fmtTime(it.endMin) + "\",";
    json += "\"enabled\":" + String(it.enabled ? "true" : "false");
    json += "}";
  }
  json += "]}";
  server.send(200, "application/json", json);
}

void handleSchedulesAdd() {
  if (!requireAuthJson()) return;
  if (!server.hasArg("relay") || !server.hasArg("date") || !server.hasArg("start") || !server.hasArg("end")) {
    server.send(400, "application/json", "{\"error\":\"missing params\"}");
    return;
  }
  int relay = server.arg("relay").toInt();
  String date = server.arg("date");
  String startT = server.arg("start");
  String endT = server.arg("end");

  int y, m, d;
  int sMin, eMin;
  if (relay < 1 || relay > 4) {
    server.send(400, "application/json", "{\"error\":\"invalid relay\"}");
    return;
  }
  if (!parseDateYmd(date, &y, &m, &d)) {
    server.send(400, "application/json", "{\"error\":\"invalid date\"}");
    return;
  }
  if (!parseTimeHm(startT, &sMin) || !parseTimeHm(endT, &eMin) || eMin <= sMin) {
    server.send(400, "application/json", "{\"error\":\"invalid time range\"}");
    return;
  }
  if (scheduleCount >= MAX_SCHEDULES) {
    server.send(400, "application/json", "{\"error\":\"schedule full\"}");
    return;
  }

  schedules[scheduleCount++] = {nextScheduleId++, relay, y, m, d, sMin, eMin, true};
  saveSchedulesToFs();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleSchedulesDelete() {
  if (!requireAuthJson()) return;
  if (!server.hasArg("id")) {
    server.send(400, "application/json", "{\"error\":\"missing id\"}");
    return;
  }
  int id = server.arg("id").toInt();
  bool removed = false;
  for (int i = 0; i < scheduleCount; i++) {
    if (schedules[i].id == id) {
      for (int j = i + 1; j < scheduleCount; j++) schedules[j - 1] = schedules[j];
      scheduleCount--;
      removed = true;
      break;
    }
  }
  if (removed) saveSchedulesToFs();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleLiveApi() {
  if (!requireAuthJson()) return;
  // JSON response with sensor data + relay states
  String ts = currentTimestampLocal();
  unsigned long epoch = timeClient.getEpochTime();

  auto fmt = [](float v, int decimals) -> String {
    if (isnan(v)) return "null";
    return String(v, decimals);
  };

  String json = "{";
  json += "\"timestamp\":\"" + ts + "\",";
  json += "\"epoch\":" + String(epoch) + ",";
  json += "\"vrms_v\":" + fmt(Vrms, 1) + ",";
  json += "\"i1_a\":" + fmt(I1, 3) + ",";
  json += "\"i2_a\":" + fmt(I2, 3) + ",";
  json += "\"i3_a\":" + fmt(I3, 3) + ",";
  json += "\"total_i_a\":" + fmt(totalI, 3) + ",";
  json += "\"power_w\":" + fmt(realPower, 1) + ",";
  json += "\"kwh\":" + fmt(kWh, 4) + ",";
  json += "\"cost\":" + fmt(cost, 2) + ",";
  json += "\"humidity_pct\":" + fmt(humidityPct, 1) + ",";
  json += "\"temp_c\":" + fmt(temperatureC, 1) + ",";
  json += "\"hum_thr\":" + String(HUMIDITY_AUTO_ON_THRESHOLD_PCT, 1) + ",";
  json += "\"temp_thr\":" + String(TEMP_AUTO_ON_THRESHOLD_C, 1) + ",";
  json += "\"auto_humidity\":" + String(relay2AutoByHumidity ? "true" : "false") + ",";
  json += "\"auto_temp\":" + String(relay1AutoByTemp ? "true" : "false") + ",";
  json += "\"relay1\":" + String(relay1On ? "true" : "false") + ",";
  json += "\"relay2\":" + String(relay2On ? "true" : "false") + ",";
  json += "\"relay3\":" + String(relay3On ? "true" : "false") + ",";
  json += "\"relay4\":" + String(relay4On ? "true" : "false");
  json += "}";

  server.send(200, "application/json", json);
}

void handleSummaryApi() {
  if (!requireAuthJson()) return;
  // Supported:
  //  - Calendar pickers:
  //      /summary?mode=day&date=YYYY-MM-DD
  //      /summary?mode=month&month=YYYY-MM
  //      /summary?mode=year&year=YYYY
  //  - Date range:
  //      /summary?start=YYYY-MM-DD&end=YYYY-MM-DD
  //  - Rolling range:
  //      /summary?days=N

  // Range (start/end) has highest priority
  bool hasRange = server.hasArg("start") && server.hasArg("end");
  String startDate = hasRange ? server.arg("start") : "";
  String endDate = hasRange ? server.arg("end") : "";

  String today = currentTimestampLocal().substring(0, 10);
  String currentMonth = currentTimestampLocal().substring(0, 7);
  String currentYear = currentTimestampLocal().substring(0, 4);

  String mode = server.hasArg("mode") ? server.arg("mode") : "";
  mode.toLowerCase();
  bool isCalendar = (mode == "day" || mode == "month" || mode == "year");

  String prefix = "";
  String label = "";
  if (isCalendar) {
    if (mode == "day") {
      String date = server.hasArg("date") ? server.arg("date") : currentTimestampLocal().substring(0, 10);
      prefix = date; // YYYY-MM-DD
      label = date;
    } else if (mode == "month") {
      String month = server.hasArg("month") ? server.arg("month") : currentTimestampLocal().substring(0, 7);
      prefix = month; // YYYY-MM
      label = month;
    } else if (mode == "year") {
      String year = server.hasArg("year") ? server.arg("year") : currentTimestampLocal().substring(0, 4);
      prefix = year; // YYYY
      label = year;
    }
  }

  // Future selection guard: if user selects a future date/month/year, return "not reached yet"
  if (isCalendar) {
    bool isFuture = false;
    if (mode == "day" && prefix > today) isFuture = true;
    if (mode == "month" && prefix > currentMonth) isFuture = true;
    if (mode == "year" && prefix > currentYear) isFuture = true;
    if (isFuture) {
      float avgDaily = NAN;
      bool ok = computeAvgDailyKwhFromCsv(14, &avgDaily);

      if (ok && !isnan(avgDaily)) {
        float predKwh = avgDaily;
        if (mode == "month") {
          int y = prefix.substring(0, 4).toInt();
          int m = prefix.substring(5, 7).toInt();
          predKwh = avgDaily * (float)daysInMonth(y, m);
        } else if (mode == "year") {
          int y = prefix.substring(0, 4).toInt();
          bool leap = ((y % 4) == 0) && (((y % 100) != 0) || ((y % 400) == 0));
          predKwh = avgDaily * (float)(leap ? 366 : 365);
        }
        float predCost = predKwh * RATE_PER_KWH;

        String json = "{";
        json += "\"status\":\"predicted\",";
        json += "\"message\":\"prediction\",";
        json += "\"model\":\"avg14days\",";
        json += "\"mode\":\"" + mode + "\",";
        json += "\"label\":\"" + label + "\",";
        json += "\"kwh_used\":" + String(predKwh, 4) + ",";
        json += "\"cost\":" + String(predCost, 2);
        json += "}";
        server.send(200, "application/json", json);
        return;
      }

      // Fallback if not enough history
      String json = "{";
      json += "\"status\":\"future\",";
      json += "\"message\":\"not reached yet\",";
      json += "\"mode\":\"" + mode + "\",";
      json += "\"label\":\"" + label + "\"";
      json += "}";
      server.send(200, "application/json", json);
      return;
    }
  }

  int days = server.hasArg("days") ? server.arg("days").toInt() : 1;
  if (days < 1) days = 1;
  if (days > 3650) days = 3650;

  unsigned long nowEpoch = timeClient.getEpochTime();
  unsigned long windowSeconds = (unsigned long)days * 86400UL;
  unsigned long cutoff = (nowEpoch > windowSeconds) ? (nowEpoch - windowSeconds) : 0;

  float startKwh = NAN;
  float endKwh = NAN;
  float oldestKwh = NAN;
  bool haveAny = false;

  String rangeStartTs = hasRange ? (startDate + " 00:00:00") : "";
  String rangeEndTs = hasRange ? (endDate + " 23:59:59") : "";
  if (hasRange) {
    // If range is in the future, show "not reached yet".
    if (startDate > today) {
      String json = "{";
      json += "\"status\":\"future\",";
      json += "\"message\":\"not reached yet\",";
      json += "\"mode\":\"range\",";
      json += "\"label\":\"" + startDate + " to " + endDate + "\"";
      json += "}";
      server.send(200, "application/json", json);
      return;
    }

    // If end date is in the future, clamp it to today (range includes current/past).
    if (endDate > today) {
      endDate = today;
      rangeEndTs = endDate + " 23:59:59";
    }

    if (endDate < startDate) {
      server.send(400, "application/json", "{\"error\":\"invalid range\"}");
      return;
    }

    label = startDate + " to " + endDate;
  }

  if (SPIFFS.exists("/data.csv")) {
    File file = SPIFFS.open("/data.csv", FILE_READ);
    if (file) {
      while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() < 10) continue;
        if (line.startsWith("Timestamp")) continue;

        // CSV columns: Timestamp,Epoch,Vrms_V,I1_A,I2_A,I3_A,TotalI_A,Power_W,kWh,Cost,...
        String ts;
        unsigned long ep = 0;
        float k = NAN;
        bool parsed = false;

        // Backward compatible parsing:
        // Old: Timestamp,Epoch,Vrms_V,I1_A,I2_A,TotalI_A,Power_W,kWh,Cost,...
        // New: Timestamp,Epoch,Vrms_V,I1_A,I2_A,I3_A,TotalI_A,Power_W,kWh,Cost,...
        for (int pass = 0; pass < 2 && !parsed; pass++) {
          const int skipCount = (pass == 0) ? 6 : 5; // after epoch: skip vrms..power

          char buf[200];
          size_t n = line.length();
          if (n >= sizeof(buf)) n = sizeof(buf) - 1;
          memcpy(buf, line.c_str(), n);
          buf[n] = 0;

          char* saveptr = nullptr;
          char* tok = strtok_r(buf, ",", &saveptr); // timestamp
          if (!tok) continue;
          ts = String(tok);
          tok = strtok_r(nullptr, ",", &saveptr);   // epoch
          if (!tok) continue;
          ep = strtoul(tok, nullptr, 10);

          for (int i = 0; i < skipCount; i++) {
            tok = strtok_r(nullptr, ",", &saveptr);
            if (!tok) break;
          }
          tok = strtok_r(nullptr, ",", &saveptr); // kWh
          if (!tok) continue;
          char* endptr = nullptr;
          float candidate = strtof(tok, &endptr);
          if (endptr != tok) {
            k = candidate;
            parsed = true;
          }
        }
        if (!parsed) continue;

        if (!haveAny) {
          oldestKwh = k;
          haveAny = true;
        }

        if (hasRange) {
          if (ts >= rangeStartTs && ts <= rangeEndTs) {
            if (isnan(startKwh)) startKwh = k;
            endKwh = k;
          }
        } else if (isCalendar) {
          if (ts.startsWith(prefix)) {
            if (isnan(startKwh)) startKwh = k;
            endKwh = k;
          }
        } else {
          if (isnan(startKwh) && ep >= cutoff) {
            startKwh = k;
          }
        }
      }
      file.close();
    }
  }

  if (isnan(startKwh)) {
    // If no matching rows, fall back to earliest available.
    startKwh = haveAny ? oldestKwh : kWh;
  }

  float endVal = (!isnan(endKwh) && (hasRange || isCalendar)) ? endKwh : kWh;
  float used = endVal - startKwh;
  if (used < 0) used = 0; // handles counter reset
  float usedCost = used * RATE_PER_KWH;

  String json = "{";
  if (hasRange) {
    json += "\"mode\":\"range\",";
    json += "\"label\":\"" + label + "\",";
  } else if (isCalendar) {
    json += "\"mode\":\"" + mode + "\",";
    json += "\"label\":\"" + label + "\",";
  } else {
    json += "\"days\":" + String(days) + ",";
  }
  json += "\"kwh_used\":" + String(used, 4) + ",";
  json += "\"cost\":" + String(usedCost, 2);
  json += "}";
  server.send(200, "application/json", json);
}

void handleRelayControl() {
  if (!requireAuthJson()) return;
  // POST /relay?relay=1&action=toggle  (or action=on / action=off)
  if (!server.hasArg("relay")) {
    server.send(400, "application/json", "{\"error\":\"missing relay param\"}");
    return;
  }

  int relayNum = server.arg("relay").toInt();
  String action = server.hasArg("action") ? server.arg("action") : "toggle";

  bool* state = nullptr;
  int pin = -1;
  int blynkPin = -1;
  if (relayNum == 1) {
    state = &relay1On;
    pin = RELAY1_PIN;
    blynkPin = V5;
  } else if (relayNum == 2) {
    state = &relay2On;
    pin = RELAY2_PIN;
    blynkPin = V6;
  } else if (relayNum == 3) {
    state = &relay3On;
    pin = RELAY3_PIN;
    blynkPin = V10;
  } else if (relayNum == 4) {
    state = &relay4On;
    pin = RELAY4_PIN;
    blynkPin = V12;
  } else {
    server.send(400, "application/json", "{\"error\":\"invalid relay\"}");
    return;
  }

  if (action == "on") {
    *state = true;
  } else if (action == "off") {
    *state = false;
  } else {
    *state = !(*state);  // toggle
  }

  // Any manual control from the web should override automation
  if (relayNum == 1) {
    relay1AutoByTemp = false;
  }
  if (relayNum == 2) {
    relay2AutoByHumidity = false;
  }
  if (relayNum == 3) {
    // Requirement: if Relay 3 is turned ON manually (dashboard), ignore distance logic.
    // When turned OFF manually, resume normal distance automation.
    relay3ManualOverride = *state;
  }

  digitalWrite(pin, *state ? LOW : HIGH);  // LOW = relay ON
  if (Blynk.connected()) Blynk.virtualWrite(blynkPin, *state ? 1 : 0);

  Serial.printf("Relay %d: %s (web)\n", relayNum, *state ? "ON" : "OFF");

  String json = "{";
  json += "\"relay1\":" + String(relay1On ? "true" : "false") + ",";
  json += "\"relay2\":" + String(relay2On ? "true" : "false") + ",";
  json += "\"relay3\":" + String(relay3On ? "true" : "false") + ",";
  json += "\"relay4\":" + String(relay4On ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleCsvDownload() {
  if (!requireAuthPlain()) return;
  if (!SPIFFS.exists("/data.csv")) {
    server.send(404, "text/plain", "data.csv not found");
    return;
  }

  File file = SPIFFS.open("/data.csv", FILE_READ);
  if (!file) {
    server.send(500, "text/plain", "Failed to open data.csv");
    return;
  }

  server.sendHeader("Content-Disposition", "attachment; filename=\"data.csv\"");
  server.streamFile(file, "text/csv");
  file.close();
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  pinMode(RESET_BTN, INPUT_PULLUP);
  pinMode(RELAY1_PIN, OUTPUT); digitalWrite(RELAY1_PIN, HIGH);  // OFF
  pinMode(RELAY2_PIN, OUTPUT); digitalWrite(RELAY2_PIN, HIGH);  // OFF
  pinMode(RELAY3_PIN, OUTPUT); digitalWrite(RELAY3_PIN, HIGH);  // OFF
  pinMode(RELAY4_PIN, OUTPUT); digitalWrite(RELAY4_PIN, HIGH);  // OFF

  pinMode(US_TRIG_PIN, OUTPUT);
  pinMode(US_ECHO_PIN, INPUT);
  digitalWrite(US_TRIG_PIN, LOW);

  analogReadResolution(12);

  // ACS712 offset calibration (friend's method)
  // IMPORTANT: keep loads OFF during calibration (relays are OFF above).
  delay(300);
  currentOffset1 = calibrateOffset(CURRENT_PIN1);
  currentOffset2 = calibrateOffset(CURRENT_PIN2);
  currentOffset3 = calibrateOffset(CURRENT_PIN3);

  // ADC attenuation (your friend sets it after calibration; keeping same order)
  analogSetPinAttenuation(CURRENT_PIN1, ADC_11db);
  analogSetPinAttenuation(CURRENT_PIN2, ADC_11db);
  analogSetPinAttenuation(CURRENT_PIN3, ADC_11db);
  analogSetPinAttenuation(VOLTAGE_PIN, ADC_11db);

  Serial.printf("ACS712 offsets: I1=%.3fV I2=%.3fV I3=%.3fV\n", currentOffset1, currentOffset2, currentOffset3);

  dht.begin();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED failed")); for(;;);
  }
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0); display.println("Energy Meter V2");
  display.display(); delay(2000);

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS failed"); for(;;);
  }

  loadSchedulesFromFs();

  EEPROM.begin(64);
  float tempKwh, tempCost;
  EEPROM.get(0, tempKwh); EEPROM.get(8, tempCost);
  kWh = (isnan(tempKwh) || tempKwh < 0) ? 0.0 : tempKwh;
  cost = (isnan(tempCost) || tempCost < 0) ? 0.0 : tempCost;

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  int timeout = 0;
  while (WiFi.status() != WL_CONNECTED && timeout < 40) {
    delay(300);
    Serial.print(".");
    timeout++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Blynk.config(BLYNK_AUTH_TOKEN);
    Blynk.connect(3000);

    const char* hdrs[] = {"Cookie"};
    server.collectHeaders(hdrs, 1);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/login", HTTP_GET, handleLoginPage);
    server.on("/login", HTTP_POST, handleLoginPost);
    server.on("/logout", HTTP_GET, handleLogout);
    server.on("/api", HTTP_GET, handleLiveApi);
    server.on("/summary", HTTP_GET, handleSummaryApi);
    server.on("/relay", HTTP_POST, handleRelayControl);
    server.on("/schedules", HTTP_GET, handleSchedulesGet);
    server.on("/schedules/add", HTTP_POST, handleSchedulesAdd);
    server.on("/schedules/delete", HTTP_POST, handleSchedulesDelete);
    server.on("/data.csv", HTTP_GET, handleCsvDownload);
    server.begin();
    Serial.print("CSV download: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/data.csv");
    Serial.print("Live dashboard: http://");
    Serial.print(WiFi.localIP());
    Serial.println("/");
  }

  lastMillis = millis();

  timeClient.begin();

  timer.setInterval(1000L, updateEverything);
  timer.setInterval(60000L, logToCSV);  // Log every 60s

  display.clearDisplay(); display.display();

  if (PRINT_EXCEL_CSV_TO_SERIAL) {
    Serial.println("Timestamp,Epoch,Vrms_V,I1_A,I2_A,I3_A,TotalI_A,Power_W,kWh,Cost,Humidity_pct,Temp_C,Relay1,Relay2,Relay3,Relay4");
  }
}

// ==================== LOOP ====================
void loop() {
  if (WiFi.isConnected()) {
    if (!Blynk.connected()) {
      Blynk.connect(1000);
    }
    Blynk.run();
    server.handleClient();
  }
  timer.run();
  if (WiFi.isConnected()) timeClient.update();

  // Reset button: short-press vs long-press
  bool resetBtnDown = (digitalRead(RESET_BTN) == LOW);
  unsigned long now = millis();

  if (resetBtnDown && !resetBtnWasDown) {
    resetBtnDownSinceMs = now;
    longPressActionDone = false;
  }

  if (resetBtnDown && !longPressActionDone) {
    if (now - resetBtnDownSinceMs >= RESET_LONG_PRESS_MS) {
      // Long press action: clear counters + delete CSV database
      kWh = cost = 0;
      EEPROM.put(0, 0.0);
      EEPROM.put(8, 0.0);
      EEPROM.commit();
      SPIFFS.remove("/data.csv");
      Serial.println("FACTORY RESET: counters cleared + /data.csv deleted");

      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(0, 18);
      display.print("FACTORY RESET");
      display.setCursor(0, 36);
      display.print("CSV deleted");
      display.display();
      delay(1500);

      longPressActionDone = true;
    }
  }

  if (!resetBtnDown && resetBtnWasDown) {
    // Button released
    unsigned long heldMs = now - resetBtnDownSinceMs;
    if (heldMs >= 50 && heldMs < RESET_LONG_PRESS_MS && !longPressActionDone) {
      // Short press action: clear counters only
      kWh = cost = 0;
      EEPROM.put(0, 0.0);
      EEPROM.put(8, 0.0);
      EEPROM.commit();
      Serial.println("RESET: counters cleared (CSV kept)");

      display.clearDisplay();
      display.setCursor(20, 20);
      display.setTextSize(2);
      display.print("RESET");
      display.display();
      delay(1200);
    }
  }

  resetBtnWasDown = resetBtnDown;
}

// ==================== READ HUMIDITY/TEMP (DHT) ====================
void updateDhtCached() {
  unsigned long now = millis();
  if (now - lastDhtReadMs < DHT_READ_INTERVAL_MS) return;
  lastDhtReadMs = now;

  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h)) humidityPct = h;
  if (!isnan(t)) temperatureC = t;
}

// ==================== READ CURRENT (ACS712) ====================
static float calibrateOffset(int pin) {
  long sum = 0;
  const int samples = 2000;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
    delayMicroseconds(200);
  }
  return (sum / (float)samples) * (VREF / ADC_MAX);
}

static float offsetForPin(int pin) {
  if (pin == CURRENT_PIN1) return currentOffset1;
  if (pin == CURRENT_PIN2) return currentOffset2;
  if (pin == CURRENT_PIN3) return currentOffset3;
  return 0.0f;
}

float readIrms(int pin) {
  float sum = 0.0f;
  const float currentOffset = offsetForPin(pin);

  for (int i = 0; i < SAMPLES; i++) {
    float voltage = analogRead(pin) * (VREF / ADC_MAX);
    float diff = voltage - currentOffset;
    sum += diff * diff;
    if ((i % 200) == 0) delay(0);
  }

  float rmsVoltage = sqrt(sum / (float)SAMPLES);
  float current = rmsVoltage / ACS712_SENSITIVITY;

  // Noise floor elimination
  if (current < 0.05f) current = 0.0f;
  return current;
}

// ==================== READ ULTRASONIC DISTANCE (HC-SR04) ====================
static float readUltrasonicDistanceCm() {
  // Trigger pulse: LOW 2us -> HIGH 10us -> LOW
  digitalWrite(US_TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(US_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(US_TRIG_PIN, LOW);

  // Timeout ~30ms (~5m max range). We only care about < 20cm here.
  unsigned long duration = pulseIn(US_ECHO_PIN, HIGH, 30000UL);
  if (duration == 0) return NAN;

  // Speed of sound: 343 m/s -> 0.0343 cm/us. Divide by 2 for round-trip.
  return (float)duration * 0.0343f / 2.0f;
}
// // ====== Read Current from ACS712 ====== nafiz
// float readCurrent() {
//   const int samples = 1000;
//   float sum = 0;

//   for (int i = 0; i < samples; i++) {
//     float voltage = analogRead(ACS712_PIN) * (VREF / ADC_MAX);
//     float diff = voltage - currentOffset;
//     sum += diff * diff;
//   }

//   float rmsVoltage = sqrt(sum / samples);
//   float current = rmsVoltage / ACS712_SENSITIVITY;

//   // Noise floor elimination
//   if (current < 0.05) current = 0;

//   return current;
// }
// ==================== READ VOLTAGE (ZMPT101B) ====================
float readVrms() {
  float sum = 0;
  for (int i = 0; i < SAMPLES; i++) {
    int raw = analogRead(VOLTAGE_PIN);
    float voltage = raw * 3.3 / 4095.0;
    sum += voltage * voltage;
    if ((i % 200) == 0) delay(0);
  }
  float rms = sqrt(sum / SAMPLES) * V_CAL;
  return (rms < 10) ? 0 : rms;
}

// ==================== BLYNK VIRTUAL PINS ====================
BLYNK_WRITE(V5) {  // Relay 1 control (from Blynk app)
  int state = param.asInt();
  relay1On = (state == 1);
  relay1AutoByTemp = false; // manual override
  digitalWrite(RELAY1_PIN, relay1On ? LOW : HIGH);
  Serial.printf("Relay 1: %s (Blynk)\n", relay1On ? "ON" : "OFF");
}

BLYNK_WRITE(V6) {  // Relay 2 control (from Blynk app)
  int state = param.asInt();
  relay2On = (state == 1);
  relay2AutoByHumidity = false; // manual override
  digitalWrite(RELAY2_PIN, relay2On ? LOW : HIGH);
  Serial.printf("Relay 2: %s (Blynk)\n", relay2On ? "ON" : "OFF");
}

BLYNK_WRITE(V10) {  // Relay 3 control (from Blynk app)
  int state = param.asInt();
  relay3On = (state == 1);
  relay3ManualOverride = relay3On;
  digitalWrite(RELAY3_PIN, relay3On ? LOW : HIGH);
  Serial.printf("Relay 3: %s (Blynk)\n", relay3On ? "ON" : "OFF");
}

BLYNK_WRITE(V12) {  // Relay 4 control (from Blynk app)
  int state = param.asInt();
  relay4On = (state == 1);
  digitalWrite(RELAY4_PIN, relay4On ? LOW : HIGH);
  Serial.printf("Relay 4: %s (Blynk)\n", relay4On ? "ON" : "OFF");
}

// ==================== MAIN UPDATE ====================
void updateEverything() {
  updateDhtCached();

  unsigned long now = millis();

  // Apply schedules (if any) for today's date.
  // Behavior: if a relay has any schedule today, it is forced ON during the window and OFF outside.
  applySchedulesNow();

  // Ultrasonic automation for Load 3:
  // - If distance > 8cm => turn ON
  // - Stay ON until distance <= 4cm => turn OFF
  // (respects a short cooldown after an overcurrent auto-off)
  distanceCm = readUltrasonicDistanceCm();
  if (!relay3ManualOverride && !isnan(distanceCm)) {
    if (distanceCm > ULTRASONIC_ON_DISTANCE_CM && !relay3On) {
      if (relay3OvercurrentTripMs == 0 || (now - relay3OvercurrentTripMs) >= RELAY3_RETRY_AFTER_TRIP_MS) {
        relay3On = true;
        digitalWrite(RELAY3_PIN, LOW);  // ON (active LOW)
        if (Blynk.connected()) Blynk.virtualWrite(V10, 1);
        Serial.println("Load 3 auto-on: ultrasonic distance > 8cm");
      }
    } else if (distanceCm <= ULTRASONIC_OFF_DISTANCE_CM && relay3On) {
      relay3On = false;
      digitalWrite(RELAY3_PIN, HIGH);  // OFF (active LOW)
      if (Blynk.connected()) Blynk.virtualWrite(V10, 0);
      Serial.println("Load 3 auto-off: ultrasonic distance <= 4cm");
    }
  }

  // Temperature automation for Load 1:
  // - Auto-ON when temp >= threshold and Load 1 is OFF
  // - Auto-OFF when temp drops below threshold, but only if Load 1 was auto-enabled
  // (respects a short cooldown after an overcurrent auto-off)
  if (!isnan(temperatureC)) {
    if (temperatureC >= TEMP_AUTO_ON_THRESHOLD_C && !relay1On) {
      if (relay1OvercurrentTripMs == 0 || (now - relay1OvercurrentTripMs) >= RELAY1_RETRY_AFTER_TRIP_MS) {
        relay1On = true;
        relay1AutoByTemp = true;
        digitalWrite(RELAY1_PIN, LOW);  // ON (active LOW)
        if (Blynk.connected()) Blynk.virtualWrite(V5, 1);
        Serial.println("Load 1 auto-on: temp >= threshold");
      }
    } else if (temperatureC < TEMP_AUTO_ON_THRESHOLD_C && relay1On && relay1AutoByTemp) {
      relay1On = false;
      relay1AutoByTemp = false;
      digitalWrite(RELAY1_PIN, HIGH);  // OFF (active LOW)
      if (Blynk.connected()) Blynk.virtualWrite(V5, 0);
      Serial.println("Load 1 auto-off: temp dropped below threshold");
    }
  }

  // Humidity automation for Load 2:
  // - Auto-ON when humidity >= threshold and Load 2 is OFF
  // - Auto-OFF when humidity drops below threshold, but only if Load 2 was auto-enabled
  // (respects a short cooldown after an overcurrent auto-off)
  if (!isnan(humidityPct)) {
    if (humidityPct >= HUMIDITY_AUTO_ON_THRESHOLD_PCT && !relay2On) {
      if (relay2OvercurrentTripMs == 0 || (now - relay2OvercurrentTripMs) >= RELAY2_RETRY_AFTER_TRIP_MS) {
        relay2On = true;
        relay2AutoByHumidity = true;
        digitalWrite(RELAY2_PIN, LOW);  // ON (active LOW)
        if (Blynk.connected()) Blynk.virtualWrite(V6, 1);
        Serial.println("Load 2 auto-on: humidity >= threshold");
      }
    } else if (humidityPct < HUMIDITY_AUTO_ON_THRESHOLD_PCT && relay2On && relay2AutoByHumidity) {
      relay2On = false;
      relay2AutoByHumidity = false;
      digitalWrite(RELAY2_PIN, HIGH);  // OFF (active LOW)
      if (Blynk.connected()) Blynk.virtualWrite(V6, 0);
      Serial.println("Load 2 auto-off: humidity dropped below threshold");
    }
  }

  I1 = readIrms(CURRENT_PIN1) * 0.1 + I1 * 0.9;  // smooth
  I2 = readIrms(CURRENT_PIN2) * 0.1 + I2 * 0.9;  // smooth
  I3 = readIrms(CURRENT_PIN3) * 0.1 + I3 * 0.9;  // smooth
  totalI = I1 + I2 + I3;
  Vrms = readVrms();
  realPower = Vrms * totalI;

  // Auto-off if >3A (overcurrent protection)
  if (I1 > MAX_CURRENT && relay1On) {
    relay1On = false;
    digitalWrite(RELAY1_PIN, HIGH);  // OFF
    relay1OvercurrentTripMs = now;
    relay1AutoByTemp = false;
    if (Blynk.connected()) Blynk.virtualWrite(V5, 0);
    Serial.println("Load 1 auto-off: overcurrent");
  }
  if (I2 > MAX_CURRENT && relay2On) {
    relay2On = false;
    digitalWrite(RELAY2_PIN, HIGH);  // OFF
    relay2OvercurrentTripMs = now;
    relay2AutoByHumidity = false;
    if (Blynk.connected()) Blynk.virtualWrite(V6, 0);
    Serial.println("Load 2 auto-off: overcurrent");
  }
  if (I3 > MAX_CURRENT && relay3On) {
    relay3On = false;
    relay3ManualOverride = false;
    digitalWrite(RELAY3_PIN, HIGH);  // OFF
    relay3OvercurrentTripMs = now;
    if (Blynk.connected()) Blynk.virtualWrite(V10, 0);
    Serial.println("Load 3 auto-off: overcurrent");
  }

  kWh += realPower * (now - lastMillis) / 3600000000.0;
  lastMillis = now;
  cost = kWh * RATE_PER_KWH;

  EEPROM.put(0,kWh); EEPROM.put(8,cost); EEPROM.commit();

  // Blynk
  if (Blynk.connected()) {
    Blynk.virtualWrite(V0, Vrms);
    Blynk.virtualWrite(V1, I1);
    Blynk.virtualWrite(V7, I2);
    Blynk.virtualWrite(V11, I3);
    Blynk.virtualWrite(V2, realPower);
    Blynk.virtualWrite(V3, kWh);
    Blynk.virtualWrite(V4, cost);
    Blynk.virtualWrite(V8, humidityPct);
    Blynk.virtualWrite(V9, temperatureC);
  }

  // OLED
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0,0);  display.printf("V:%.0f I:%.2fA", Vrms, totalI);
  display.setCursor(0,18); display.printf("I1:%.2f I2:%.2f", I1, I2);
  if (isnan(humidityPct)) {
    display.setCursor(0,36); display.printf("I3:%.2f H:--", I3);
  } else {
    display.setCursor(0,36); display.printf("I3:%.2f H:%.0f%% T:%.fC", I3, humidityPct,temperatureC);
  }
  display.setCursor(0,54); display.printf("kWh:%.3f C:%.1f", kWh, cost);
  display.display();

  // Serial
  if (PRINT_EXCEL_CSV_TO_SERIAL) {
    unsigned long epoch = timeClient.getEpochTime();
    time_t rawTime = (time_t)epoch;
    struct tm timeInfo;
    gmtime_r(&rawTime, &timeInfo);
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
             timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
             timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);

    // True CSV row matching the header printed in setup()
    Serial.printf("%s,%lu,%.1f,%.3f,%.3f,%.3f,%.3f,%.1f,%.4f,%.2f,%.1f,%.1f,%d,%d,%d,%d\n",
                  timestamp, (unsigned long)epoch, Vrms, I1, I2, I3, totalI, realPower, kWh, cost,
                  isnan(humidityPct) ? -1.0 : humidityPct,
                  isnan(temperatureC) ? -127.0 : temperatureC,
                  relay1On ? 1 : 0,
                  relay2On ? 1 : 0,
                  relay3On ? 1 : 0,
                  relay4On ? 1 : 0);
  }
}

// ==================== LOG TO CSV ====================
void logToCSV() {
  updateDhtCached();

  timeClient.update();
  unsigned long epoch = timeClient.getEpochTime();
  time_t rawTime = (time_t)epoch;
  struct tm timeInfo;
  gmtime_r(&rawTime, &timeInfo);
  char timestamp[32];
  snprintf(timestamp, sizeof(timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
           timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday,
           timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);

  bool needHeader = true;
  bool headerHasRelay4 = false;
  if (SPIFFS.exists("/data.csv")) {
    File existing = SPIFFS.open("/data.csv", FILE_READ);
    if (existing) {
      needHeader = (existing.size() == 0);
      if (!needHeader) {
        String header = existing.readStringUntil('\n');
        header.trim();
        headerHasRelay4 = (header.indexOf("Relay4") >= 0);
      }
      existing.close();
    }
  }

  const bool writeRelay4Column = needHeader ? true : headerHasRelay4;

  File file = SPIFFS.open("/data.csv", FILE_APPEND);
  if (file) {
    if (needHeader) {
      file.println("Timestamp,Epoch,Vrms_V,I1_A,I2_A,I3_A,TotalI_A,Power_W,kWh,Cost,Humidity_pct,Temp_C,Relay1,Relay2,Relay3,Relay4");
    }
    if (writeRelay4Column) {
      file.printf("%s,%lu,%.1f,%.3f,%.3f,%.3f,%.3f,%.1f,%.4f,%.2f,%.1f,%.1f,%d,%d,%d,%d\n",
                  timestamp, (unsigned long)epoch, Vrms, I1, I2, I3, totalI, realPower, kWh, cost,
                  isnan(humidityPct) ? -1.0 : humidityPct,
                  isnan(temperatureC) ? -127.0 : temperatureC,
                  relay1On ? 1 : 0,
                  relay2On ? 1 : 0,
                  relay3On ? 1 : 0,
                  relay4On ? 1 : 0);
    } else {
      file.printf("%s,%lu,%.1f,%.3f,%.3f,%.3f,%.3f,%.1f,%.4f,%.2f,%.1f,%.1f,%d,%d,%d\n",
                  timestamp, (unsigned long)epoch, Vrms, I1, I2, I3, totalI, realPower, kWh, cost,
                  isnan(humidityPct) ? -1.0 : humidityPct,
                  isnan(temperatureC) ? -127.0 : temperatureC,
                  relay1On ? 1 : 0,
                  relay2On ? 1 : 0,
                  relay3On ? 1 : 0);
    }
    file.close();
  } else {
    Serial.println("CSV append failed");
  }
}