#pragma once

#include <Arduino.h>

#if __has_include("AppSecrets.h")
#include "AppSecrets.h"
#endif

namespace AppConfig {

constexpr uint8_t kPowerDetectPin = 2;
constexpr uint8_t kI2cSdaPin = SDA;
constexpr uint8_t kI2cSclPin = SCL;

constexpr uint32_t kBatteryLogIntervalSeconds = 120;
constexpr uint32_t kExternalLogIntervalSeconds = 10;
constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kWifiReconnectIntervalMs = 5000;
constexpr uint32_t kWifiFailoverToApTimeoutMs = 30000;
constexpr uint32_t kWifiReconnectScanIntervalMs = 15000;
constexpr uint32_t kApRecoveryGracePeriodMs = 120000;
constexpr uint32_t kNtpSyncTimeoutMs = 10000;
constexpr uint32_t kSensorReadyTimeoutMs = 7000;
constexpr uint32_t kSensorPollDelayMs = 250;

// Debug recovery mode: keep the board awake while bring-up and USB recovery are in progress.
constexpr bool kDisableDeepSleep = true;
// Temporary diagnostics mode: keep battery operation behaving like external mode while still reporting the true D0 state.
constexpr bool kForceExternalBehaviorForBatteryDiagnostics = true;

constexpr size_t kLogCapacity = 279620;

constexpr char kApSsid[] = "CO2-Logger-Setup";
constexpr char kApPassword[] = "co2logger";
constexpr char kHostname[] = "co2-logger";
constexpr char kNtpServer1[] = "pool.ntp.org";
constexpr char kNtpServer2[] = "time.nist.gov";
constexpr long kGmtOffsetSeconds = 0;
constexpr int kDstOffsetSeconds = 0;

#ifdef APP_WIFI_SSID
constexpr char kWifiSsid[] = APP_WIFI_SSID;
#else
constexpr char kWifiSsid[] = "";
#endif

#ifdef APP_WIFI_PASSWORD
constexpr char kWifiPassword[] = APP_WIFI_PASSWORD;
#else
constexpr char kWifiPassword[] = "";
#endif

constexpr char kLogsFile[] = "/logs.bin";
constexpr char kStateFile[] = "/state.bin";

}  // namespace AppConfig
