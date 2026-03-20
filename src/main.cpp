#include <Arduino.h>
#include <LittleFS.h>
#include <SensirionI2CScd4x.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <time.h>

#include "AppConfig.h"
#include "LogRing.h"

namespace {

constexpr uint32_t kStateMagic = 0x434F324C;
constexpr uint16_t kStateVersion = 1;

constexpr uint8_t kFlagExternalPower = 1 << 0;
constexpr uint8_t kFlagTimeSynced = 1 << 1;
constexpr uint8_t kFlagTimeEstimated = 1 << 2;
constexpr uint8_t kFlagValidReading = 1 << 3;

RTC_DATA_ATTR bool gRtcTimeValid = false;
RTC_DATA_ATTR uint32_t gRtcUnixBase = 0;
RTC_DATA_ATTR uint32_t gRtcMillisBase = 0;
RTC_DATA_ATTR uint32_t gBootCounter = 0;

enum class OperatingMode : uint8_t {
  Battery = 0,
  External = 1,
};

enum class PowerModeOverride : uint8_t {
  Auto = 0,
  Battery = 1,
  External = 2,
};

enum class SensorMeasurementMode : uint8_t {
  Uninitialized = 0,
  Periodic = 1,
  LowPowerPeriodic = 2,
};

enum class NetworkMode : uint8_t {
  StaConnected = 0,
  StaDisconnectedRecovering = 1,
  ApActive = 2,
};

enum class TimeSource : uint8_t {
  Unknown = 0,
  Ntp = 1,
  Phone = 2,
  Stored = 3,
  Build = 4,
};

struct LogEntry {
  uint32_t timestamp;
  uint16_t co2Ppm;
  int16_t temperatureX100;
  uint16_t humidityX100;
  uint8_t flags;
  uint8_t reserved;
};

static_assert(sizeof(LogEntry) == 12, "LogEntry must stay compact");

struct StorageState {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  uint32_t head;
  uint32_t count;
  uint32_t lastKnownTimestamp;
  uint8_t lastTimestampWasSynced;
  uint8_t reserved2[3];
};

struct Measurement {
  bool valid = false;
  uint16_t co2Ppm = 0;
  float temperatureC = 0.0f;
  float humidityPercent = 0.0f;
};

struct RuntimeState {
  OperatingMode mode = OperatingMode::Battery;
  OperatingMode behaviorMode = OperatingMode::Battery;
  PowerModeOverride powerOverride = PowerModeOverride::Auto;
  SensorMeasurementMode sensorMode = SensorMeasurementMode::Uninitialized;
  NetworkMode networkMode = NetworkMode::StaDisconnectedRecovering;
  TimeSource timeSource = TimeSource::Unknown;
  bool wifiConnected = false;
  bool apMode = false;
  bool ntpSynced = false;
  bool apStartedByFailover = false;
  wifi_err_reason_t wifiDisconnectReason = WIFI_REASON_UNSPECIFIED;
  uint8_t discardReadingsAfterModeSwitch = 0;
  uint32_t lastTimestamp = 0;
  uint32_t lastWiFiConnectedMs = 0;
  uint32_t lastWiFiDisconnectMs = 0;
  uint32_t lastReconnectAttemptMs = 0;
  uint32_t lastApRecoveryScanMs = 0;
  uint32_t apStartedAtMs = 0;
  uint32_t lastPhoneTimeSyncMs = 0;
  int32_t timezoneOffsetMinutes = 0;
  Measurement latestMeasurement;
  String statusMessage;
};

StorageState gStorageState{};
RuntimeState gRuntimeState{};
SensirionI2CScd4x gScd4x;
WebServer gServer(80);

bool readLogEntry(size_t logicalIndex, LogEntry& entry);
void runConnectedMode();

struct WifiTarget {
  int channel = 0;
  bool hasBssid = false;
  uint8_t bssid[6] = {0};
};

String modeToString(OperatingMode mode) {
  return mode == OperatingMode::External ? "external" : "battery";
}

OperatingMode behaviorModeFor(OperatingMode reportedMode) {
  if (AppConfig::kForceExternalBehaviorForBatteryDiagnostics) {
    return OperatingMode::External;
  }
  return reportedMode;
}

String powerOverrideToString(PowerModeOverride mode) {
  switch (mode) {
    case PowerModeOverride::Battery:
      return "battery";
    case PowerModeOverride::External:
      return "external";
    case PowerModeOverride::Auto:
    default:
      return "auto";
  }
}

String networkModeToString(NetworkMode mode) {
  switch (mode) {
    case NetworkMode::StaConnected:
      return "sta";
    case NetworkMode::StaDisconnectedRecovering:
      return "recovering";
    case NetworkMode::ApActive:
    default:
      return "ap";
  }
}

String timeSourceToString(TimeSource source) {
  switch (source) {
    case TimeSource::Ntp:
      return "ntp";
    case TimeSource::Phone:
      return "phone";
    case TimeSource::Stored:
      return "stored";
    case TimeSource::Build:
      return "build";
    case TimeSource::Unknown:
    default:
      return "unknown";
  }
}

String sensorModeToString(SensorMeasurementMode mode) {
  switch (mode) {
    case SensorMeasurementMode::LowPowerPeriodic:
      return "low_power_periodic";
    case SensorMeasurementMode::Periodic:
      return "periodic";
    case SensorMeasurementMode::Uninitialized:
    default:
      return "uninitialized";
  }
}

uint32_t toUnixTime() {
  if (gRtcTimeValid) {
    const uint32_t elapsedSeconds = (millis() - gRtcMillisBase) / 1000;
    return gRtcUnixBase + elapsedSeconds;
  }

  const time_t now = time(nullptr);
  if (now > 1700000000) {
    gRtcTimeValid = true;
    gRtcUnixBase = static_cast<uint32_t>(now);
    gRtcMillisBase = millis();
    return gRtcUnixBase;
  }

  return 0;
}

void setUnixTimeBase(time_t now) {
  if (now <= 0) {
    return;
  }

  gRtcTimeValid = true;
  gRtcUnixBase = static_cast<uint32_t>(now);
  gRtcMillisBase = millis();
}

void restoreEstimatedTimeFromStorage() {
  if (gRtcTimeValid) {
    return;
  }
  if (gStorageState.lastKnownTimestamp == 0) {
    return;
  }

  gRtcTimeValid = true;
  gRtcUnixBase = gStorageState.lastKnownTimestamp;
  gRtcMillisBase = millis();
  gRuntimeState.ntpSynced = gStorageState.lastTimestampWasSynced != 0;
  gRuntimeState.timeSource = gRuntimeState.ntpSynced ? TimeSource::Ntp : TimeSource::Stored;
  Serial.printf("Restored stored time base: %lu (%s)\n",
                static_cast<unsigned long>(gRtcUnixBase),
                gRuntimeState.ntpSynced ? "synced" : "estimated");
}

bool parseBuildTime(tm& outTime) {
  memset(&outTime, 0, sizeof(outTime));

  char monthStr[4] = {};
  int day = 0;
  int year = 0;
  if (sscanf(__DATE__, "%3s %d %d", monthStr, &day, &year) != 3) {
    return false;
  }

  static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int month = -1;
  for (int i = 0; i < 12; ++i) {
    if (strncmp(monthStr, kMonths[i], 3) == 0) {
      month = i;
      break;
    }
  }
  if (month < 0) {
    return false;
  }

  int hour = 0;
  int minute = 0;
  int second = 0;
  if (sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) {
    return false;
  }

  outTime.tm_year = year - 1900;
  outTime.tm_mon = month;
  outTime.tm_mday = day;
  outTime.tm_hour = hour;
  outTime.tm_min = minute;
  outTime.tm_sec = second;
  return true;
}

void restoreEstimatedTimeFromBuild() {
  if (gRtcTimeValid) {
    return;
  }

  tm buildTime{};
  if (!parseBuildTime(buildTime)) {
    return;
  }

  const time_t compiledAt = mktime(&buildTime);
  if (compiledAt <= 0) {
    return;
  }

  gRtcTimeValid = true;
  gRtcUnixBase = static_cast<uint32_t>(compiledAt);
  gRtcMillisBase = millis();
  gRuntimeState.ntpSynced = false;
  gRuntimeState.timeSource = TimeSource::Build;
  Serial.printf("Restored build time base: %lu\n", static_cast<unsigned long>(gRtcUnixBase));
}

bool hasWiFiCredentials() {
  return strlen(AppConfig::kWifiSsid) > 0;
}

bool initFilesystem() {
  if (LittleFS.begin(true)) {
    Serial.println("LittleFS mounted");
    return true;
  }

  Serial.println("LittleFS mount failed");
  return false;
}

bool saveStorageState() {
  File file = LittleFS.open(AppConfig::kStateFile, FILE_WRITE);
  if (!file) {
    return false;
  }

  const size_t written = file.write(reinterpret_cast<const uint8_t*>(&gStorageState), sizeof(gStorageState));
  file.close();
  return written == sizeof(gStorageState);
}

bool loadStorageState() {
  if (!LittleFS.exists(AppConfig::kStateFile)) {
    gStorageState.magic = kStateMagic;
    gStorageState.version = kStateVersion;
    gStorageState.head = 0;
    gStorageState.count = 0;
    gStorageState.lastKnownTimestamp = 0;
    gStorageState.lastTimestampWasSynced = 0;
    const bool ok = saveStorageState();
    Serial.printf("Storage state initialized: %s\n", ok ? "ok" : "failed");
    return ok;
  }

  File file = LittleFS.open(AppConfig::kStateFile, FILE_READ);
  if (!file) {
    return false;
  }

  const size_t read = file.read(reinterpret_cast<uint8_t*>(&gStorageState), sizeof(gStorageState));
  file.close();
  if (read != sizeof(gStorageState) || gStorageState.magic != kStateMagic || gStorageState.version != kStateVersion ||
      gStorageState.head >= AppConfig::kLogCapacity || gStorageState.count > AppConfig::kLogCapacity) {
    gStorageState.magic = kStateMagic;
    gStorageState.version = kStateVersion;
    gStorageState.head = 0;
    gStorageState.count = 0;
    gStorageState.lastKnownTimestamp = 0;
    gStorageState.lastTimestampWasSynced = 0;
    const bool ok = saveStorageState();
    Serial.printf("Storage state reset: %s\n", ok ? "ok" : "failed");
    return ok;
  }

  Serial.printf("Storage state loaded: head=%lu count=%lu last_ts=%lu\n",
                static_cast<unsigned long>(gStorageState.head),
                static_cast<unsigned long>(gStorageState.count),
                static_cast<unsigned long>(gStorageState.lastKnownTimestamp));
  return true;
}

size_t logOffset(size_t index) {
  return index * sizeof(LogEntry);
}

bool appendLogEntry(const LogEntry& entry) {
  File file = LittleFS.open(AppConfig::kLogsFile, "r+");
  if (!file) {
    file = LittleFS.open(AppConfig::kLogsFile, "w+");
  }
  if (!file) {
    return false;
  }

  if (!file.seek(logOffset(gStorageState.head), SeekSet)) {
    file.close();
    return false;
  }

  const size_t written = file.write(reinterpret_cast<const uint8_t*>(&entry), sizeof(entry));
  file.flush();
  file.close();
  if (written != sizeof(entry)) {
    return false;
  }

  LogRing::State ringState{AppConfig::kLogCapacity, gStorageState.head, gStorageState.count};
  LogRing::advanceAfterAppend(ringState);
  gStorageState.head = static_cast<uint32_t>(ringState.head);
  gStorageState.count = static_cast<uint32_t>(ringState.count);

  const bool saved = saveStorageState();
  if (saved) {
    Serial.printf("Log appended: ts=%lu co2=%u flags=0x%02x count=%lu\n",
                  static_cast<unsigned long>(entry.timestamp), entry.co2Ppm, entry.flags,
                  static_cast<unsigned long>(gStorageState.count));
  }
  return saved;
}

bool verifyLatestLogEntry(const LogEntry& expected) {
  if (gStorageState.count == 0) {
    return false;
  }

  LogEntry actual{};
  if (!readLogEntry(gStorageState.count - 1, actual)) {
    Serial.println("Latest log verification failed: read back failed");
    return false;
  }

  const bool matches = memcmp(&expected, &actual, sizeof(LogEntry)) == 0;
  Serial.printf("Latest log verification: %s\n", matches ? "ok" : "mismatch");
  return matches;
}

bool readLogEntry(size_t logicalIndex, LogEntry& entry) {
  const LogRing::State ringState{AppConfig::kLogCapacity, gStorageState.head, gStorageState.count};
  if (!LogRing::isValidLogicalIndex(ringState, logicalIndex)) {
    return false;
  }

  const size_t physicalIndex = LogRing::physicalIndexForLogical(ringState, logicalIndex);

  File file = LittleFS.open(AppConfig::kLogsFile, FILE_READ);
  if (!file) {
    return false;
  }

  if (!file.seek(logOffset(physicalIndex), SeekSet)) {
    file.close();
    return false;
  }

  const size_t read = file.read(reinterpret_cast<uint8_t*>(&entry), sizeof(entry));
  file.close();
  return read == sizeof(entry);
}

String jsonEscape(const String& value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value.charAt(i);
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      default:
        out += c;
        break;
    }
  }
  return out;
}

String measurementToJson(const Measurement& measurement) {
  String json = "{";
  json += "\"valid\":";
  json += measurement.valid ? "true" : "false";
  json += ",\"co2\":";
  json += String(measurement.co2Ppm);
  json += ",\"temperature_c\":";
  json += String(measurement.temperatureC, 2);
  json += ",\"humidity_percent\":";
  json += String(measurement.humidityPercent, 2);
  json += "}";
  return json;
}

String recordToJson(const LogEntry& entry) {
  String json = "{";
  json += "\"timestamp\":";
  json += String(entry.timestamp);
  json += ",\"co2\":";
  json += String(entry.co2Ppm);
  json += ",\"temperature_c\":";
  json += String(static_cast<float>(entry.temperatureX100) / 100.0f, 2);
  json += ",\"humidity_percent\":";
  json += String(static_cast<float>(entry.humidityX100) / 100.0f, 2);
  json += ",\"flags\":";
  json += String(entry.flags);
  json += "}";
  return json;
}

String buildLiveJson() {
  String json = "{";
  json += "\"mode\":\"";
  json += modeToString(gRuntimeState.mode);
  json += "\",\"behavior_mode\":\"";
  json += modeToString(gRuntimeState.behaviorMode);
  json += "\",\"network_mode\":\"";
  json += networkModeToString(gRuntimeState.networkMode);
  json += "\",\"power_override\":\"";
  json += powerOverrideToString(gRuntimeState.powerOverride);
  json += "\",\"sensor_mode\":\"";
  json += sensorModeToString(gRuntimeState.sensorMode);
  json += "\",\"time_source\":\"";
  json += timeSourceToString(gRuntimeState.timeSource);
  json += "\",\"wifi_connected\":";
  json += gRuntimeState.wifiConnected ? "true" : "false";
  json += ",\"ap_mode\":";
  json += gRuntimeState.apMode ? "true" : "false";
  json += ",\"ntp_synced\":";
  json += gRuntimeState.ntpSynced ? "true" : "false";
  json += ",\"timezone_offset_minutes\":";
  json += String(gRuntimeState.timezoneOffsetMinutes);
  json += ",\"last_timestamp\":";
  json += String(gRuntimeState.lastTimestamp);
  json += ",\"status\":\"";
  json += jsonEscape(gRuntimeState.statusMessage);
  json += "\",\"measurement\":";
  json += measurementToJson(gRuntimeState.latestMeasurement);
  json += "}";
  return json;
}

String buildRecentLogsJson(size_t maxRecords) {
  String json = "[";
  const size_t available = gStorageState.count;
  const size_t start = available > maxRecords ? available - maxRecords : 0;

  bool first = true;
  for (size_t i = start; i < available; ++i) {
    LogEntry entry{};
    if (!readLogEntry(i, entry)) {
      continue;
    }
    if (!first) {
      json += ",";
    }
    first = false;
    json += recordToJson(entry);
  }

  json += "]";
  return json;
}

void handleRoot() {
  static const char kPage[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>CO2 Logger</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    :root {
      --bg: #f3efe6;
      --card: rgba(255, 252, 245, 0.92);
      --text: #1e2a24;
      --muted: #5d6b63;
      --accent: #1f6f5f;
      --accent-2: #f2a65a;
      --line: rgba(31, 111, 95, 0.18);
    }
    body {
      margin: 0;
      font-family: Georgia, "Times New Roman", serif;
      background:
        radial-gradient(circle at top left, rgba(242, 166, 90, 0.22), transparent 30%),
        radial-gradient(circle at top right, rgba(31, 111, 95, 0.18), transparent 28%),
        linear-gradient(180deg, #f6f1e8 0%, #e8ede7 100%);
      color: var(--text);
    }
    main {
      max-width: 980px;
      margin: 0 auto;
      padding: 24px;
    }
    .hero, .panel {
      background: var(--card);
      border: 1px solid var(--line);
      border-radius: 18px;
      box-shadow: 0 14px 40px rgba(37, 54, 47, 0.08);
      padding: 20px;
      margin-bottom: 18px;
      backdrop-filter: blur(8px);
    }
    .hero h1 {
      margin: 0 0 6px;
      font-size: 2.2rem;
    }
    .subtle {
      color: var(--muted);
      margin: 0;
    }
    .stats {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(170px, 1fr));
      gap: 14px;
      margin-top: 16px;
    }
    .stat {
      background: rgba(255,255,255,0.72);
      border-radius: 14px;
      padding: 14px;
    }
    .stat .label {
      font-size: 0.9rem;
      color: var(--muted);
      margin-bottom: 6px;
    }
    .stat .value {
      font-size: 1.8rem;
      font-weight: 700;
    }
    .toolbar {
      display: flex;
      gap: 12px;
      flex-wrap: wrap;
      align-items: center;
      margin-bottom: 12px;
    }
    a.button {
      background: var(--accent);
      color: white;
      text-decoration: none;
      padding: 10px 14px;
      border-radius: 999px;
    }
    .chart-wrap {
      position: relative;
      width: 100%;
      height: 320px;
    }
    .chart-grid {
      display: grid;
      gap: 18px;
    }
    .chart-card {
      background: rgba(255,255,255,0.72);
      border-radius: 14px;
      padding: 14px;
    }
    .chart-card h3 {
      margin: 0 0 10px;
      font-size: 1rem;
      color: var(--muted);
    }
    canvas {
      display: block;
      width: 100% !important;
      height: 100% !important;
    }
  </style>
</head>
<body>
  <main>
    <section class="hero">
      <p class="subtle">CO2 Logger ESP32-C3</p>
      <h1>Live Air Quality Monitor</h1>
      <p id="status" class="subtle">Waiting for device data...</p>
      <div class="stats">
        <div class="stat"><div class="label">CO2</div><div id="co2" class="value">--</div></div>
        <div class="stat"><div class="label">Temperature</div><div id="temp" class="value">--</div></div>
        <div class="stat"><div class="label">Humidity</div><div id="humidity" class="value">--</div></div>
        <div class="stat"><div class="label">Time Quality</div><div id="time" class="value">--</div></div>
      </div>
    </section>
    <section class="panel">
      <div class="toolbar">
        <strong>History</strong>
        <a class="button" href="/api/export.csv">Download CSV</a>
      </div>
      <div class="chart-grid">
        <div class="chart-card">
          <h3>CO2 ppm</h3>
          <div class="chart-wrap">
            <canvas id="co2-chart"></canvas>
          </div>
        </div>
        <div class="chart-card">
          <h3>Temperature C</h3>
          <div class="chart-wrap">
            <canvas id="temp-chart"></canvas>
          </div>
        </div>
        <div class="chart-card">
          <h3>Humidity %</h3>
          <div class="chart-wrap">
            <canvas id="humidity-chart"></canvas>
          </div>
        </div>
      </div>
    </section>
  </main>
  <script>
    let co2Chart;
    let tempChart;
    let humidityChart;
    let lastPhoneTimePostMs = 0;
    let phoneTimeInFlight = false;

    function describeTimeSource(live) {
      if (live.ntp_synced || live.time_source === 'ntp') return 'synced (NTP)';
      if (live.time_source === 'phone') return 'estimated (phone)';
      if (live.time_source === 'stored') return 'estimated (stored)';
      if (live.time_source === 'build') return 'estimated (build)';
      return 'estimated';
    }

    async function maybePostPhoneTime(live) {
      if (!live.ap_mode || live.ntp_synced || phoneTimeInFlight) return;
      const nowMs = Date.now();
      if (lastPhoneTimePostMs !== 0 && nowMs - lastPhoneTimePostMs < 60000) return;

      phoneTimeInFlight = true;
      try {
        const response = await fetch('/api/time', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            unix_time: Math.floor(nowMs / 1000),
            timezone_offset_minutes: -new Date().getTimezoneOffset()
          })
        });
        if (response.ok) {
          const payload = await response.json();
          if (payload.accepted) {
            lastPhoneTimePostMs = nowMs;
          }
        }
      } catch (error) {
      } finally {
        phoneTimeInFlight = false;
      }
    }

    function buildChart(canvasId, label, color, fill, data) {
      return new Chart(document.getElementById(canvasId), {
        type: 'line',
        data: {
          datasets: [{
            label,
            data,
            borderColor: color,
            backgroundColor: fill,
            fill: true,
            tension: 0.28,
            parsing: false
          }]
        },
        options: {
          responsive: true,
          maintainAspectRatio: false,
          scales: {
            x: {
              type: 'linear',
              ticks: {
                callback: (value) => {
                  const date = new Date(value * 1000);
                  return date.toLocaleTimeString([], {
                    hour: '2-digit',
                    minute: '2-digit',
                    second: '2-digit'
                  });
                }
              }
            }
          }
        }
      });
    }

    async function loadLive() {
      const live = await fetch('/api/live').then(r => r.json());
      document.getElementById('status').textContent =
        `${live.mode} mode | net=${live.network_mode} | time=${describeTimeSource(live)} | wifi=${live.wifi_connected} | ap=${live.ap_mode} | ${live.status}`;
      document.getElementById('co2').textContent = live.measurement.valid ? `${live.measurement.co2} ppm` : 'n/a';
      document.getElementById('temp').textContent = live.measurement.valid ? `${live.measurement.temperature_c.toFixed(2)} C` : 'n/a';
      document.getElementById('humidity').textContent = live.measurement.valid ? `${live.measurement.humidity_percent.toFixed(2)} %` : 'n/a';
      document.getElementById('time').textContent = describeTimeSource(live);
      await maybePostPhoneTime(live);
    }

    async function loadLogs() {
      const logs = await fetch('/api/logs').then(r => r.json());
      const co2 = logs.map(x => ({ x: x.timestamp, y: x.co2 }));
      const temperatures = logs.map(x => ({ x: x.timestamp, y: x.temperature_c }));
      const humidities = logs.map(x => ({ x: x.timestamp, y: x.humidity_percent }));
      if (!co2Chart) {
        co2Chart = buildChart('co2-chart', 'CO2 ppm', '#1f6f5f', 'rgba(31, 111, 95, 0.18)', co2);
        tempChart = buildChart('temp-chart', 'Temperature C', '#c96f32', 'rgba(201, 111, 50, 0.16)', temperatures);
        humidityChart = buildChart('humidity-chart', 'Humidity %', '#3e7cb1', 'rgba(62, 124, 177, 0.16)', humidities);
      } else {
        co2Chart.data.datasets[0].data = co2;
        tempChart.data.datasets[0].data = temperatures;
        humidityChart.data.datasets[0].data = humidities;
        co2Chart.update();
        tempChart.update();
        humidityChart.update();
      }
    }

    async function refresh() {
      await Promise.all([loadLive(), loadLogs()]);
    }

    refresh();
    setInterval(refresh, 5000);
  </script>
</body>
</html>
)rawliteral";

  gServer.send_P(200, "text/html", kPage);
}

void handleLive() {
  gServer.send(200, "application/json", buildLiveJson());
}

void handleLogs() {
  gServer.send(200, "application/json", buildRecentLogsJson(256));
}

void handleDevMode() {
  if (!gServer.hasArg("value")) {
    gServer.send(400, "application/json", "{\"error\":\"missing value\"}");
    return;
  }

  const String value = gServer.arg("value");
  if (value == "auto") {
    gRuntimeState.powerOverride = PowerModeOverride::Auto;
  } else if (value == "battery") {
    gRuntimeState.powerOverride = PowerModeOverride::Battery;
  } else if (value == "external") {
    gRuntimeState.powerOverride = PowerModeOverride::External;
  } else {
    gServer.send(400, "application/json", "{\"error\":\"invalid value\"}");
    return;
  }

  Serial.printf("Power override changed to %s\n", powerOverrideToString(gRuntimeState.powerOverride).c_str());
  gServer.send(200, "application/json", buildLiveJson());
}

bool extractJsonLong(const String& body, const char* key, long& value) {
  const String token = String("\"") + key + "\"";
  const int keyPos = body.indexOf(token);
  if (keyPos < 0) {
    return false;
  }
  const int colonPos = body.indexOf(':', keyPos + token.length());
  if (colonPos < 0) {
    return false;
  }

  int start = colonPos + 1;
  while (start < body.length() && isspace(static_cast<unsigned char>(body[start]))) {
    ++start;
  }
  if (start >= body.length()) {
    return false;
  }

  int end = start;
  if (body[end] == '-' || body[end] == '+') {
    ++end;
  }
  while (end < body.length() && isdigit(static_cast<unsigned char>(body[end]))) {
    ++end;
  }
  if (end == start || (end == start + 1 && (body[start] == '-' || body[start] == '+'))) {
    return false;
  }

  value = body.substring(start, end).toInt();
  return true;
}

void handlePhoneTime() {
  if (gRuntimeState.ntpSynced) {
    gServer.send(409, "application/json",
                 "{\"accepted\":false,\"reason\":\"ntp_synced\",\"time_source\":\"ntp\"}");
    return;
  }

  const String body = gServer.arg("plain");
  long unixTime = 0;
  long timezoneOffsetMinutes = 0;
  if (!extractJsonLong(body, "unix_time", unixTime) ||
      !extractJsonLong(body, "timezone_offset_minutes", timezoneOffsetMinutes) ||
      unixTime < 1700000000L) {
    gServer.send(400, "application/json",
                 "{\"accepted\":false,\"reason\":\"invalid_payload\"}");
    return;
  }

  setUnixTimeBase(static_cast<time_t>(unixTime));
  gRuntimeState.ntpSynced = false;
  gRuntimeState.timeSource = TimeSource::Phone;
  gRuntimeState.timezoneOffsetMinutes = static_cast<int32_t>(timezoneOffsetMinutes);
  gRuntimeState.lastTimestamp = static_cast<uint32_t>(unixTime);
  gRuntimeState.lastPhoneTimeSyncMs = millis();
  gStorageState.lastKnownTimestamp = gRuntimeState.lastTimestamp;
  gStorageState.lastTimestampWasSynced = 0;
  saveStorageState();
  gRuntimeState.statusMessage = "Time estimated from phone via AP";

  String response = "{";
  response += "\"accepted\":true";
  response += ",\"time_source\":\"";
  response += timeSourceToString(gRuntimeState.timeSource);
  response += "\",\"timestamp\":";
  response += String(gRuntimeState.lastTimestamp);
  response += ",\"timezone_offset_minutes\":";
  response += String(gRuntimeState.timezoneOffsetMinutes);
  response += "}";
  gServer.send(200, "application/json", response);
}

void handleExportCsv() {
  gServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  gServer.send(200, "text/csv", "");
  gServer.sendContent("timestamp,co2_ppm,temperature_c,humidity_percent,flags\n");

  for (size_t i = 0; i < gStorageState.count; ++i) {
    LogEntry entry{};
    if (!readLogEntry(i, entry)) {
      continue;
    }

    String line;
    line.reserve(96);
    line += String(entry.timestamp);
    line += ",";
    line += String(entry.co2Ppm);
    line += ",";
    line += String(static_cast<float>(entry.temperatureX100) / 100.0f, 2);
    line += ",";
    line += String(static_cast<float>(entry.humidityX100) / 100.0f, 2);
    line += ",";
    line += String(entry.flags);
    line += "\n";
    gServer.sendContent(line);
  }

  gServer.sendContent("");
}

void configureRoutes() {
  gServer.on("/", HTTP_GET, handleRoot);
  gServer.on("/api/live", HTTP_GET, handleLive);
  gServer.on("/api/logs", HTTP_GET, handleLogs);
  gServer.on("/api/export.csv", HTTP_GET, handleExportCsv);
  gServer.on("/api/dev/mode", HTTP_ANY, handleDevMode);
  gServer.on("/api/time", HTTP_POST, handlePhoneTime);
}

bool initSensor() {
  Serial.println("Initializing SCD41");
  Wire.begin(AppConfig::kI2cSdaPin, AppConfig::kI2cSclPin);
  gScd4x.begin(Wire);

  uint16_t error = gScd4x.wakeUp();
  (void)error;
  gScd4x.stopPeriodicMeasurement();
  delay(500);
  Serial.println("SCD41 idle and ready");
  return true;
}

bool setSensorMode(SensorMeasurementMode targetMode) {
  if (gRuntimeState.sensorMode == targetMode && gRuntimeState.discardReadingsAfterModeSwitch == 0) {
    return true;
  }

  gScd4x.wakeUp();
  uint16_t error = gScd4x.stopPeriodicMeasurement();
  if (error != 0) {
    Serial.printf("SCD4x stop failed: %u\n", error);
  }
  delay(500);

  if (targetMode == SensorMeasurementMode::LowPowerPeriodic) {
    error = gScd4x.startLowPowerPeriodicMeasurement();
  } else {
    error = gScd4x.startPeriodicMeasurement();
  }

  if (error != 0) {
    Serial.printf("SCD4x mode start failed: %u\n", error);
    return false;
  }

  gRuntimeState.sensorMode = targetMode;
  gRuntimeState.discardReadingsAfterModeSwitch = 1;
  Serial.printf("SCD41 mode switched to %s\n", sensorModeToString(targetMode).c_str());
  delay(5000);
  return true;
}

Measurement readSensorMeasurement() {
  Measurement measurement;

  const uint32_t start = millis();
  while (millis() - start < AppConfig::kSensorReadyTimeoutMs) {
    bool ready = false;
    const uint16_t readyError = gScd4x.getDataReadyFlag(ready);
    if (readyError == 0 && ready) {
      uint16_t co2 = 0;
      float temp = 0.0f;
      float humidity = 0.0f;
      const uint16_t readError = gScd4x.readMeasurement(co2, temp, humidity);
      if (readError == 0 && co2 > 0) {
        measurement.valid = true;
        measurement.co2Ppm = co2;
        measurement.temperatureC = temp;
        measurement.humidityPercent = humidity;
        if (gRuntimeState.discardReadingsAfterModeSwitch > 0) {
          --gRuntimeState.discardReadingsAfterModeSwitch;
          measurement.valid = false;
          Serial.println("Discarding first reading after sensor mode switch");
        }
        Serial.printf("Measurement: co2=%u temp=%.2fC humidity=%.2f%%\n", co2, temp, humidity);
      } else {
        Serial.printf("SCD41 read failed or invalid value: err=%u co2=%u\n", readError, co2);
      }
      return measurement;
    }
    delay(AppConfig::kSensorPollDelayMs);
  }

  Serial.println("SCD41 measurement timed out");
  return measurement;
}

bool syncNtpTime() {
  Serial.println("Starting NTP sync");
  configTime(AppConfig::kGmtOffsetSeconds, AppConfig::kDstOffsetSeconds, AppConfig::kNtpServer1, AppConfig::kNtpServer2);

  const uint32_t start = millis();
  while (millis() - start < AppConfig::kNtpSyncTimeoutMs) {
    const time_t now = time(nullptr);
    if (now > 1700000000) {
      setUnixTimeBase(now);
      gRuntimeState.ntpSynced = true;
      gRuntimeState.timeSource = TimeSource::Ntp;
      Serial.printf("NTP sync successful: %lu\n", static_cast<unsigned long>(now));
      return true;
    }
    delay(250);
  }

  Serial.println("NTP sync timed out");
  return false;
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    gRuntimeState.wifiConnected = true;
    gRuntimeState.networkMode = NetworkMode::StaConnected;
    gRuntimeState.lastWiFiConnectedMs = millis();
    gRuntimeState.lastWiFiDisconnectMs = 0;
    Serial.println("Wi-Fi STA connected event");
    return;
  }

  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    gRuntimeState.wifiDisconnectReason = static_cast<wifi_err_reason_t>(info.wifi_sta_disconnected.reason);
    gRuntimeState.wifiConnected = false;
    gRuntimeState.lastWiFiDisconnectMs = millis();
    if (!gRuntimeState.apMode) {
      gRuntimeState.networkMode = NetworkMode::StaDisconnectedRecovering;
    }
    Serial.printf("Wi-Fi disconnect reason: %u\n", static_cast<unsigned>(gRuntimeState.wifiDisconnectReason));
  }
}

void updateNetworkFlags(NetworkMode mode) {
  gRuntimeState.networkMode = mode;
  gRuntimeState.wifiConnected = WiFi.status() == WL_CONNECTED;
  gRuntimeState.apMode = mode == NetworkMode::ApActive;
}

bool findConfiguredSsid(WifiTarget& target) {
  Serial.println("Scanning 2.4 GHz networks");
  const int networkCount = WiFi.scanNetworks(false, true);
  if (networkCount < 0) {
    Serial.printf("Wi-Fi scan failed: %d\n", networkCount);
    return false;
  }

  for (int i = 0; i < networkCount; ++i) {
    Serial.printf("Found SSID: %s RSSI=%d channel=%d\n",
                  WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
    if (WiFi.SSID(i) == AppConfig::kWifiSsid) {
      target.channel = WiFi.channel(i);
      const uint8_t* bssid = WiFi.BSSID(i);
      if (bssid != nullptr) {
        memcpy(target.bssid, bssid, sizeof(target.bssid));
        target.hasBssid = true;
      }
      return true;
    }
  }

  return false;
}

bool connectWifi() {
  if (!hasWiFiCredentials()) {
    gRuntimeState.statusMessage = "Wi-Fi credentials not configured";
    Serial.println("Wi-Fi skipped: no credentials configured");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.onEvent(onWiFiEvent);
  WiFi.setHostname(AppConfig::kHostname);
  WiFi.setAutoReconnect(true);
  WiFi.disconnect(false, true);

  WifiTarget target;
  if (findConfiguredSsid(target)) {
    Serial.printf("Configured SSID visible on channel %d\n", target.channel);
  } else {
    gRuntimeState.statusMessage = "Configured SSID not visible on 2.4 GHz";
    Serial.println("Configured SSID was not found in the 2.4 GHz scan");
    Serial.println("ESP32-C3 cannot connect to 5 GHz-only networks");
    return false;
  }

  if (target.hasBssid) {
    Serial.printf("Connecting using BSSID %02X:%02X:%02X:%02X:%02X:%02X\n",
                  target.bssid[0], target.bssid[1], target.bssid[2],
                  target.bssid[3], target.bssid[4], target.bssid[5]);
    WiFi.begin(AppConfig::kWifiSsid, AppConfig::kWifiPassword, target.channel, target.bssid, true);
  } else {
    WiFi.begin(AppConfig::kWifiSsid, AppConfig::kWifiPassword);
  }
  Serial.printf("Connecting to Wi-Fi SSID: %s\n", AppConfig::kWifiSsid);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < AppConfig::kWifiConnectTimeoutMs) {
    delay(250);
  }

  gRuntimeState.wifiConnected = WiFi.status() == WL_CONNECTED;
  if (gRuntimeState.wifiConnected) {
    updateNetworkFlags(NetworkMode::StaConnected);
    gRuntimeState.lastWiFiConnectedMs = millis();
    gRuntimeState.apStartedByFailover = false;
    gRuntimeState.statusMessage = "Connected to Wi-Fi";
    Serial.printf("Wi-Fi connected, IP=%s\n", WiFi.localIP().toString().c_str());
    return true;
  }

  updateNetworkFlags(NetworkMode::StaDisconnectedRecovering);
  gRuntimeState.lastWiFiDisconnectMs = millis();
  gRuntimeState.statusMessage = "Wi-Fi connect failed";
  Serial.printf("Wi-Fi connection failed, status=%d reason=%u\n",
                static_cast<int>(WiFi.status()),
                static_cast<unsigned>(gRuntimeState.wifiDisconnectReason));
  return false;
}

void startAccessPoint() {
  if (WiFi.getMode() != WIFI_AP && WiFi.getMode() != WIFI_AP_STA) {
    WiFi.mode(WIFI_AP);
  }
  WiFi.softAP(AppConfig::kApSsid, AppConfig::kApPassword);
  updateNetworkFlags(NetworkMode::ApActive);
  gRuntimeState.apStartedAtMs = millis();
  gRuntimeState.lastApRecoveryScanMs = 0;
  gRuntimeState.statusMessage = "Access point active";
  Serial.printf("AP started: ssid=%s ip=%s\n", AppConfig::kApSsid, WiFi.softAPIP().toString().c_str());
}

void stopAccessPoint() {
  if (gRuntimeState.apMode) {
    WiFi.softAPdisconnect(true);
    gRuntimeState.apMode = false;
    Serial.println("AP stopped");
  }
}

bool recoverStaFromAp() {
  if (!hasWiFiCredentials()) {
    return false;
  }
  if (gRuntimeState.apStartedAtMs != 0 &&
      millis() - gRuntimeState.apStartedAtMs < AppConfig::kApRecoveryGracePeriodMs) {
    return false;
  }
  if (millis() - gRuntimeState.lastApRecoveryScanMs < AppConfig::kWifiReconnectScanIntervalMs) {
    return false;
  }
  gRuntimeState.lastApRecoveryScanMs = millis();

  WifiTarget target;
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  if (!findConfiguredSsid(target)) {
    Serial.println("AP recovery scan: configured SSID not visible");
    return false;
  }

  Serial.println("AP recovery scan: configured SSID visible, attempting reconnect");
  if (target.hasBssid) {
    WiFi.begin(AppConfig::kWifiSsid, AppConfig::kWifiPassword, target.channel, target.bssid, true);
  } else {
    WiFi.begin(AppConfig::kWifiSsid, AppConfig::kWifiPassword);
  }

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < AppConfig::kWifiConnectTimeoutMs) {
    gServer.handleClient();
    delay(50);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("AP recovery reconnect failed");
    return false;
  }

  stopAccessPoint();
  WiFi.mode(WIFI_STA);
  updateNetworkFlags(NetworkMode::StaConnected);
  gRuntimeState.apStartedByFailover = false;
  gRuntimeState.statusMessage = "Recovered home Wi-Fi";
  gRuntimeState.lastWiFiConnectedMs = millis();
  Serial.printf("Recovered STA connection, IP=%s\n", WiFi.localIP().toString().c_str());
  syncNtpTime();
  return true;
}

void handleNetworkStateMachine() {
  if (gRuntimeState.apMode) {
    recoverStaFromAp();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    updateNetworkFlags(NetworkMode::StaConnected);
    gRuntimeState.lastWiFiConnectedMs = millis();
    return;
  }

  if (gRuntimeState.networkMode != NetworkMode::StaDisconnectedRecovering) {
    updateNetworkFlags(NetworkMode::StaDisconnectedRecovering);
    if (gRuntimeState.lastWiFiDisconnectMs == 0) {
      gRuntimeState.lastWiFiDisconnectMs = millis();
    }
  }

  if (gRuntimeState.lastWiFiDisconnectMs != 0 &&
      millis() - gRuntimeState.lastWiFiDisconnectMs >= AppConfig::kWifiFailoverToApTimeoutMs) {
    Serial.println("Wi-Fi unavailable beyond timeout, switching to AP");
    WiFi.disconnect(false, true);
    startAccessPoint();
    gRuntimeState.apStartedByFailover = true;
    return;
  }

  if (millis() - gRuntimeState.lastReconnectAttemptMs < AppConfig::kWifiReconnectIntervalMs) {
    return;
  }

  gRuntimeState.lastReconnectAttemptMs = millis();
  Serial.println("Attempting Wi-Fi reconnect");
  WiFi.reconnect();
  gRuntimeState.statusMessage = "Recovering Wi-Fi connection";
}

uint8_t buildFlags(bool validMeasurement) {
  uint8_t flags = 0;
  if (gRuntimeState.mode == OperatingMode::External) {
    flags |= kFlagExternalPower;
  }
  if (gRuntimeState.ntpSynced) {
    flags |= kFlagTimeSynced;
  } else {
    flags |= kFlagTimeEstimated;
  }
  if (validMeasurement) {
    flags |= kFlagValidReading;
  }
  return flags;
}

void recordMeasurement(const Measurement& measurement) {
  const uint32_t timestamp = toUnixTime();
  gRuntimeState.lastTimestamp = timestamp;
  gRuntimeState.latestMeasurement = measurement;

  if (!measurement.valid) {
    Serial.println("Skipping flash log append for invalid measurement");
    return;
  }

  LogEntry entry{};
  entry.timestamp = timestamp;
  entry.co2Ppm = measurement.co2Ppm;
  entry.temperatureX100 = static_cast<int16_t>(measurement.temperatureC * 100.0f);
  entry.humidityX100 = static_cast<uint16_t>(measurement.humidityPercent * 100.0f);
  entry.flags = buildFlags(measurement.valid);

  gStorageState.lastKnownTimestamp = timestamp;
  gStorageState.lastTimestampWasSynced = gRuntimeState.ntpSynced ? 1 : 0;
  if (!appendLogEntry(entry)) {
    Serial.println("Failed to append log entry");
  } else {
    verifyLatestLogEntry(entry);
  }
}

bool externalPowerPresent() {
  return digitalRead(AppConfig::kPowerDetectPin) == HIGH;
}

OperatingMode requestedMode() {
  switch (gRuntimeState.powerOverride) {
    case PowerModeOverride::Battery:
      return OperatingMode::Battery;
    case PowerModeOverride::External:
      return OperatingMode::External;
    case PowerModeOverride::Auto:
    default:
      return externalPowerPresent() ? OperatingMode::External : OperatingMode::Battery;
  }
}

uint32_t logIntervalSecondsForMode(OperatingMode mode) {
  return mode == OperatingMode::External ? AppConfig::kExternalLogIntervalSeconds
                                         : AppConfig::kBatteryLogIntervalSeconds;
}

bool applyModeConfiguration(OperatingMode mode) {
  gRuntimeState.mode = mode;
  gRuntimeState.behaviorMode = behaviorModeFor(mode);

  if (gRuntimeState.behaviorMode == OperatingMode::External) {
    if (mode == OperatingMode::Battery) {
      gRuntimeState.statusMessage = "Battery detected, external behavior test mode active";
    } else {
      gRuntimeState.statusMessage = "External power mode active";
    }
    return setSensorMode(SensorMeasurementMode::Periodic);
  }

  if (mode == OperatingMode::Battery) {
    gRuntimeState.statusMessage = externalPowerPresent() ? "Battery mode emulation active" : "Battery mode active";
    return setSensorMode(SensorMeasurementMode::LowPowerPeriodic);
  }

  gRuntimeState.statusMessage = "External power mode active";
  return setSensorMode(SensorMeasurementMode::Periodic);
}

bool externalPowerPresentRaw() {
  return digitalRead(AppConfig::kPowerDetectPin) == HIGH;
}

void configureSleep() {
  // Deep sleep is intentionally disabled during development to keep the board responsive.
  // esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(AppConfig::kBatteryLogIntervalSeconds) * 1000000ULL);
  // esp_deep_sleep_enable_gpio_wakeup(1ULL << AppConfig::kPowerDetectPin, ESP_GPIO_WAKEUP_GPIO_HIGH);
}

void runBatteryMode() {
  applyModeConfiguration(OperatingMode::Battery);
  if (gRuntimeState.behaviorMode == OperatingMode::External) {
    Serial.println("Battery detected, routing to external-style behavior test mode");
    runConnectedMode();
    return;
  }

  if (AppConfig::kDisableDeepSleep) {
    Serial.println("Running battery mode with deep sleep disabled");

    uint32_t lastMeasurementAt = 0;
    while (requestedMode() == OperatingMode::Battery) {
      if (millis() - lastMeasurementAt >= logIntervalSecondsForMode(OperatingMode::Battery) * 1000UL || lastMeasurementAt == 0) {
        Measurement measurement = readSensorMeasurement();
        recordMeasurement(measurement);
        lastMeasurementAt = millis();
      }
      delay(50);
    }

    Serial.println("Leaving battery mode debug hold");
    runConnectedMode();
    return;
  }

  gRuntimeState.statusMessage = "Battery mode logging cycle";
  Serial.println("Running battery mode cycle");

  Measurement measurement = readSensorMeasurement();
  recordMeasurement(measurement);

  configureSleep();
  Serial.println("Deep sleep disabled; staying awake in development mode");
  while (requestedMode() == OperatingMode::Battery) {
    delay(50);
  }
  Serial.println("Leaving battery mode while staying awake");
  runConnectedMode();
}

void runConnectedMode() {
  applyModeConfiguration(requestedMode());
  Serial.printf("Running connected mode with reported=%s behavior=%s\n",
                modeToString(gRuntimeState.mode).c_str(),
                modeToString(gRuntimeState.behaviorMode).c_str());
  if (!connectWifi()) {
    startAccessPoint();
  } else {
    syncNtpTime();
  }

  configureRoutes();
  gServer.begin();
  Serial.println("Web server started on port 80");

  uint32_t lastMeasurementAt = 0;
  while (externalPowerPresentRaw() || gRuntimeState.powerOverride != PowerModeOverride::Auto ||
         AppConfig::kForceExternalBehaviorForBatteryDiagnostics) {
    const OperatingMode mode = requestedMode();
    if (mode != gRuntimeState.mode) {
      Serial.printf("Applying mode transition to %s (behavior=%s)\n",
                    modeToString(mode).c_str(),
                    modeToString(behaviorModeFor(mode)).c_str());
      applyModeConfiguration(mode);
      lastMeasurementAt = 0;
    }

    if (millis() - lastMeasurementAt >= logIntervalSecondsForMode(gRuntimeState.behaviorMode) * 1000UL ||
        lastMeasurementAt == 0) {
      Measurement measurement = readSensorMeasurement();
      recordMeasurement(measurement);
      lastMeasurementAt = millis();
    }

    handleNetworkStateMachine();
    gServer.handleClient();
    delay(10);
  }

  gServer.stop();
  stopAccessPoint();
  WiFi.mode(WIFI_OFF);
  gRuntimeState.wifiConnected = false;
  gRuntimeState.apMode = false;
  gRuntimeState.networkMode = NetworkMode::StaDisconnectedRecovering;
  gRuntimeState.statusMessage = "Connected mode exited";
  if (requestedMode() == OperatingMode::Battery) {
    runBatteryMode();
  }
}

}  // namespace

void setup() {
  ++gBootCounter;
  Serial.begin(115200);
  delay(300);

  pinMode(AppConfig::kPowerDetectPin, INPUT);
#ifdef LED_BUILTIN
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
#endif

  if (!initFilesystem() || !loadStorageState()) {
    Serial.println("Storage initialization failed");
    delay(1000);
    ESP.restart();
  }

  if (!initSensor()) {
    gRuntimeState.statusMessage = "Sensor init failed";
  }

  restoreEstimatedTimeFromStorage();
  restoreEstimatedTimeFromBuild();

  gRuntimeState.mode = requestedMode();
  applyModeConfiguration(gRuntimeState.mode);

  Serial.printf("Boot #%lu in %s mode\n", static_cast<unsigned long>(gBootCounter), modeToString(gRuntimeState.mode).c_str());

  if (gRuntimeState.behaviorMode == OperatingMode::External) {
    runConnectedMode();
  } else {
    runBatteryMode();
  }
}

void loop() {
}
