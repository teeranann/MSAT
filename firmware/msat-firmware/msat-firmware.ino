/*
 * ============================================================================
 * MSAT Controller - Multi-Sensor Automatic Titrator   (firmware V.Y2026.88.17)
 * ----------------------------------------------------------------------------
 * ESP32 firmware for a low-cost automatic acid-base titrator that logs pH,
 * electrical conductivity (EC), colour (RGB -> dE), temperature and titrant
 * mass in real time, serves a WiFi dashboard, and saves runs to an SD card.
 *
 * Copyright (c) 2026 Burapha University.
 * Inventor / developer: Teeranan Nongnual <teeranan.no@buu.ac.th>
 *   Department of Chemistry, Faculty of Science, Burapha University, Thailand
 *
 * Petty Patent pending: Application No. 2603001145 (filed 2026-05-12).
 *
 * License: Apache License 2.0  (see /LICENSE). Free for any use, including
 *   commercial, with attribution. The petty patent above is licensed under
 *   Apache-2.0 section 3.
 * SPDX-License-Identifier: Apache-2.0
 * Required Notice: Copyright (c) 2026 Burapha University.
 *
 * SETUP: set your WiFi name/password in the two lines marked "EDIT ME" below.
 * ============================================================================
 */

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <SPI.h>
#include <SD.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>
#include <sys/time.h> 
#include <Wire.h>
#include "HX711.h"
#include <Adafruit_ADS1X15.h>
#include <Adafruit_TCS34725.h>
#include <ModbusMaster.h>
#include <RTClib.h> 
#include <LiquidCrystal_I2C.h> 
#include <vector>
#include <algorithm>
#include <deque>
#include <esp_task_wdt.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#ifndef ON
#define ON true
#endif
#ifndef OFF
#define OFF false
#endif

#define EC_PROFILE_CONSERVATIVE 0
#define EC_PROFILE_BALANCED 1
#define EC_PROFILE_AGGRESSIVE 2

// ================= USER PIN & WIFI CONFIGURATION =================
const char* ssid     = "YOUR_WIFI_SSID";       // <-- EDIT ME (2.4 GHz network)
const char* password = "YOUR_WIFI_PASSWORD";   // <-- EDIT ME
IPAddress local_IP(192, 168, 1, 200);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

// ================= USER SENSOR CONFIGURATION =================
const bool ECadaptivefilter = ON;
const uint8_t ECadaptiveProfile = EC_PROFILE_BALANCED;

// ================= USER WEB DASHBOARD CONFIGURATION =================
const unsigned long WS_REFRESH_IDLE_MS = 1000;
const unsigned long WS_REFRESH_RUN_REC_MS = 500;
const unsigned long WS_REFRESH_PENDING_RINSE_MS = 1000;

#define RELAY_PIN 13
#define MAX485_DE_RE 2
#define MAX485_RX 16
#define MAX485_TX 17
#define BUTTON1_PIN 27
#define BUTTON2_PIN 14
#define BUTTON3_PIN 15
#define I2C_SDA 32
#define I2C_SCL 33
#define LOADCELL_DOUT_PIN 25
#define LOADCELL_SCK_PIN 26
#define SD_CS_PIN 5
#define SD_SPI_FREQ 4000000
#define ONE_WIRE_BUS 4

// ================= CALIBRATION DATA =================
float ph_voltages[] = { 4.3066, 3.9333, 3.5657 }; 
float ph_values[]   = { 4.01,   6.86,   9.18 };

float cal_ec_raw[] = { 80.08, 1400.51, 12387.90 }; 
float cal_ec_std[] = { 84.43, 1452.93, 13258.08 };

float ec_slope = 1.0;
float ec_intercept = 0.0;

float loadcell_raw[] = { 
  2611734, 2619282, 2628194, 2636986, 2654585, 
  2698371, 2785731, 2872980, 3047255, 3307728, 
  3483624, 3918092, 4356299 
};
float loadcell_weight[] = { 
  0.0, 1.0, 2.0, 3.0, 5.0, 
  10.0, 20.0, 30.0, 50.0, 80.0, 
  100.0, 150.0, 200.0 
};

// ================= CONSTANTS =================
const unsigned long PENDING_DURATION_SEC = 10;
const unsigned long STRICT_LOG_FILE_RETRY_MS = 5000;
const unsigned long REC_QUEUE_WAIT_MS = 100;
const uint32_t SD_LOCK_TIMEOUT_MS = 1000;
const unsigned long SD_RETRY_MS = 5000;
const char* SYSTEM_LOG_PATH = "/autotttoutput/msatlog.txt";
const size_t SYSTEM_LOG_MAX_BYTES = 256000;
const bool ENABLE_METRIC_LOG = true;
const char* METRIC_LOG_PATH = "/autotttoutput/msatmetric.txt";
const unsigned long METRIC_LOG_COOLDOWN_MS = 60000;
const size_t SD_WRITE_QUEUE_MAX = 500;
const size_t SD_QUEUE_LINE_MAX = 192;
const size_t METRIC_QUEUE_WARN = 50;
const size_t METRIC_QUEUE_ERR = 200;
const unsigned long STOP_FLUSH_TIMEOUT_MS = 3000;
const unsigned long METRIC_SD_WRITE_WARN_MS = 250;
const unsigned long METRIC_SD_WRITE_ERR_MS = 500;
const unsigned long METRIC_SENSOR_LAG_MS = 200;
const unsigned long WEB_STALL_WARN_MS = 1000;
const unsigned long SYNC_TIMEOUT_MS = 10000;
const unsigned long BUTTON_COOLDOWN_MS = 500;
#define BOOT_CHECK_DELAY_MS 2000 
#define TCS_GAIN_SETTING TCS34725_GAIN_4X 
#define TCS_INTEGRATION_TIME TCS34725_INTEGRATIONTIME_24MS
const unsigned long STATUS_CHECK_INTERVAL = 5000;
const int AUTO_STOP_AVG_SAMPLES = 10;
const int AUTO_STOP_HOLD_COUNT = 3;
const float AUTO_STOP_MAX_WEIGHT_STEP_G_PER_SEC = 20.0f;
const float AUTO_STOP_MIN_WEIGHT_STEP_G = 1.0f;
const float AUTO_STOP_GUIDE_MAX_DEVIATION_G = 2.0f;
const float AUTO_STOP_GUIDE_SLOPE_ALPHA = 0.25f;
const float AUTO_STOP_GUIDE_MAX_SLOPE_G_PER_SEC = 20.0f;
const float SENSOR_EC_EMA_ALPHA = 0.22f;
const float SENSOR_PH_EMA_ALPHA = 0.20f;
const float SENSOR_PH_RUN_MAX_STEP_PER_SEC = 6.00f;
const float SENSOR_PH_RUN_SPIKE_REJECT_DELTA = 2.50f;
const float SENSOR_PH_RUN_BURST_TRIGGER_DELTA = 0.60f;
const int SENSOR_PH_RUN_BURST_HOLD_CYCLES = 30;
const float SENSOR_PH_RUN_BURST_MAX_STEP_PER_SEC = 18.00f;
const float SENSOR_PH_RUN_BURST_ALPHA = 0.45f;
const float SENSOR_PH_RUN_EMI_OFFSET_ALPHA = 0.22f;
const float SENSOR_PH_RUN_EMI_RELEASE_ALPHA = 0.35f;
const float SENSOR_PH_RUN_EMI_OFFSET_MAX = 0.65f;
const float SENSOR_PH_BUMP_THRESHOLD_DELTA = 0.50f;
const int SENSOR_PH_BUMP_THRESHOLD_COUNT = 3;
const float SENSOR_TEMP_EMA_ALPHA = 0.25f;
const float SENSOR_RGB_EMA_ALPHA = 0.30f;
const float SENSOR_EC_MAX_STEP_US_PER_SEC = 260.0f;
const float SENSOR_EC_MIN_STEP_US = 8.0f;
const float SENSOR_EC_SPIKE_REJECT_US = 180.0f;
const float SENSOR_EC_ADAPT_TRIGGER_DELTA_US = 52.0f;
const float SENSOR_EC_ADAPT_RELEASE_DELTA_US = 34.0f;
const int SENSOR_EC_ADAPT_HOLD_CYCLES = 3;
const float SENSOR_EC_ADAPT_EMA_ALPHA = 0.33f;
const float SENSOR_EC_ADAPT_MAX_STEP_US_PER_SEC = 420.0f;
const float SENSOR_EC_ADAPT_SPIKE_REJECT_US = 250.0f;
const float SENSOR_EC_BUMP_THRESHOLD_DELTA_US = 90.0f;
const int SENSOR_EC_BUMP_THRESHOLD_COUNT = 3;
// EC start-of-RUN settling: a real physical/electrical transient happens
// in the first few seconds after dosing begins (probe boundary layer,
// pump turn-on coupling, initial salt formation). It can be wider than
// median7 can reject. For this window, clamp per-sample EC change to a
// small fraction of the current value so a multi-sample spike cannot
// inflate ecFiltered. After the window, no clamp -> EP stays sharp.
const int SENSOR_EC_RUN_SETTLE_CYCLES = 20;        // ~5 s @250 ms cadence
const float SENSOR_EC_RUN_SETTLE_MAX_FRAC = 0.05f; // <=5%/sample during settle
const float SENSOR_EC_RUN_SETTLE_MIN_STEP_US = 10.0f;
// Anomaly-skip: protects against EC transients that are too wide for
// median9 (e.g. Succinic ~5 raw samples). If the median-filtered value
// suddenly deviates from the filter state by > 20% (or > 300 uS), freeze
// ecFiltered for up to N samples; if the deviation persists past N (real
// fast change), accept it. The 20% threshold is far above any per-sample
// delta at the titration inflection (typically <2% of mid-titration EC),
// so the EP is never frozen. Works any time during RUN, not just startup.
const float SENSOR_EC_ANOMALY_FRAC = 0.20f;        // 20% deviation triggers
const float SENSOR_EC_ANOMALY_MIN_DELTA_US = 300.0f; // absolute floor
const int SENSOR_EC_ANOMALY_SKIP_MAX = 12;         // ~3 s max freeze
const float SENSOR_TEMP_VALID_MIN_C = -55.0f;
const float SENSOR_TEMP_VALID_MAX_C = 125.0f;
const float SENSOR_TEMP_POR_INVALID_C = 85.0f;
const float SENSOR_TEMP_POR_EPS_C = 0.01f;
const float DISPLAY_WEIGHT_GUIDE_ALPHA = 0.25f;
const float DISPLAY_WEIGHT_MAX_DEVIATION_G = 1.5f;
const float DISPLAY_WEIGHT_MAX_SLOPE_G_PER_SEC = 25.0f;
const float DISPLAY_WEIGHT_RUN_MAX_DEVIATION_G = 4.0f;
const float DISPLAY_WEIGHT_RUN_MAX_SLOPE_G_PER_SEC = 80.0f;

const unsigned long SENSOR_IDLE_MS = 250;
const unsigned long SENSOR_PENDING_MS = 200;
const unsigned long SENSOR_RUNNING_MS = 50;
const unsigned long SENSOR_RINSE_MS = 300;
const unsigned long SENSOR_REC_MS = 150;

const unsigned long EC_IDLE_MS = 1000;
const unsigned long EC_PENDING_MS = 500;
const unsigned long EC_RUNNING_MS = 250;
const unsigned long EC_RINSE_MS = 1000;
const unsigned long EC_REC_MS = 250;

const unsigned long LCD_IDLE_MS = 500;
const unsigned long LCD_PENDING_MS = 100;
const unsigned long LCD_RUNNING_MS = 200;
const unsigned long LCD_RINSE_MS = 500;
const unsigned long LCD_REC_MS = 200;

const unsigned long LOG_IDLE_MS = 10000;
const unsigned long LOG_PENDING_MS = 2000;
const unsigned long LOG_RUNNING_MS = 500;
const unsigned long LOG_RINSE_MS = 5000;
const unsigned long LOG_REC_MS = 500;

// File list and rebuild constants (removed from implementation, but kept for compatibility)
const unsigned long FILE_LIST_CACHE_MAX = 2000;
const unsigned long FILE_LIST_REFRESH_MS = 10000;
const unsigned long AUTO_REBUILD_MIN_UPTIME_MS = 10000;
const unsigned long REBUILD_RETRY_BACKOFF_MS = 30000;

const unsigned long TIME_SYNC_RETRY_MS = 300000;
const char* INDEX_FILE_PATH = "/autotttoutput/_index.txt";
const char* FW_VERSION = "V.Y2026.88.17";
const char* FW_BUILD_ID = "MSAT-V.Y2026.88.17";

// ================= GLOBAL OBJECTS =================
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
HX711 scale;
Adafruit_ADS1115 ads;
Adafruit_TCS34725 tcs = Adafruit_TCS34725(TCS_INTEGRATION_TIME, TCS_GAIN_SETTING); 
ModbusMaster node; 
RTC_DS3231 rtc; 
LiquidCrystal_I2C lcd(0x27, 16, 2);

TaskHandle_t TaskSensors;
TaskHandle_t TaskSDWriter;

struct SDQueueItem {
  char data[SD_QUEUE_LINE_MAX];
};

std::deque<SDQueueItem> sdWriteQueue;
SemaphoreHandle_t sdQueueMutex = NULL;
SemaphoreHandle_t sdCardMutex = NULL;
unsigned long lastSdRetryMs = 0;
SemaphoreHandle_t fileListMutex = NULL;
std::vector<String> cachedFileList;
bool cachedFileListTruncated = false;
unsigned long lastFileListUpdateMs = 0;
volatile bool fileListRefreshRequested = false;
volatile bool fileListRefreshInProgress = false;
volatile uint32_t fileListGeneration = 0;
volatile bool rebuildIndexRequested = false;
volatile bool rebuildIndexInProgress = false;
volatile uint32_t rebuildProgress = 0;
volatile unsigned long rebuildStartedMs = 0;
volatile unsigned long lastRebuildFailMs = 0;
String rebuildOwnerIP = "";
String rebuildStatusText = "IDLE";
volatile bool manifestRebuildRequested = false;
volatile bool manifestRebuildInProgress = false;

// ================= CLIENT CONNECTION TRACKING =================
#define MAX_CLIENTS 2
#define CLIENT_TIMEOUT_SEC 300  
#define ADMIN_PASSWORD "CHANGE_ME"   // <-- EDIT ME (admin action password)

struct ClientInfo {
  String ipAddress;
  String macAddress;
  unsigned long connectedTime;
  unsigned long lastActivityTime;
  String userAgent;
  String deviceName;
};
std::vector<ClientInfo> connectedClients;
SemaphoreHandle_t clientListMutex = NULL;

// ================= SHARED VARIABLES =================
volatile int currentState = 0;
volatile bool isLogging = false;
volatile bool stopFlushInProgress = false;
volatile bool logFileCreatePending = false;
volatile bool syncMode = false;
unsigned long lastSyncHeartbeatMs = 0;

// ================= WIFI MONITORING =================
unsigned long lastWifiCheck = 0;
unsigned long lastWifiReconnectAttempt = 0;
const unsigned long WIFI_CHECK_INTERVAL_MS = 10000;      // Check every 10 seconds (detect disconnection faster)
const unsigned long WIFI_RECONNECT_COOLDOWN_MS = 60000;  // Wait 60s between reconnect attempts (slow routers need longer)
const unsigned long WIFI_RECONNECT_TIMEOUT_MS = 60000;   // Allow up to 60s for one reconnection attempt
volatile bool wifiConnected = false;
volatile bool wifiReconnecting = false;
volatile bool wifiEverConnected = false;
volatile bool webServerStarted = false;
volatile bool twdtInitialized = false;  // Track if TWDT init succeeded to prevent crashes
unsigned long syncUntilMs = 0;
unsigned long syncLastActivityMs = 0;
String currentLogFileName = "";
String lastCompletedRecFileName = "";
String lastCompletedStopReason = "NONE";
volatile int lcdPage = 0; 
String controlClientIP = "";
String adminClientIP = "";
String lastWebCommandIP = "";
unsigned long startLimiterUntilMs = 0;
bool useExternalWebServer = false;
const char* PC_WEB_SERVER_IP = "192.168.1.199";
const int PC_WEB_SERVER_PORT = 80;

volatile int autoStopMode = 1;
volatile float autoStopWeightTarget = 85.0;
volatile unsigned long autoStopTimeTarget = 300;
volatile int runAutoStopMode = 0;
volatile float runAutoStopWeightTarget = 0.0;
volatile unsigned long runAutoStopTimeTarget = 0;
String customFilename = "";
unsigned long recordingStartTime = 0;
unsigned long lastLogCreateAttemptMs = 0;
uint64_t runLogEpochBaseMs = 0;
unsigned long runLogSampleIndex = 0;
unsigned long runLogStepMs = LOG_RUNNING_MS;
bool runLogClockArmed = false;
String lastStopReason = "NONE";
volatile int lastCommandSource = 2;
// ===== Web Command Flags =====
volatile bool webCmdToggle = false;
volatile bool webCmdRinse = false;
volatile bool webCmdRecord = false;
volatile bool webCmdTare = false;

float weightLossAvg = 0.0;
float weightLossSamples[AUTO_STOP_AVG_SAMPLES];
int weightLossSampleCount = 0;
int weightLossSampleIndex = 0;
int autoStopHitCount = 0;
float weightLossGuide = 0.0f;
float weightLossGuideSlope = 0.0f;
bool weightLossGuideInit = false;

bool timeSynced = false;
unsigned long lastTimeSyncMs = 0;
bool st_ads = false, st_rgb = false, st_temp = false, st_scale = false, st_sd = false, st_ec = false, st_rtc = false;
unsigned long lastScaleSuccess = 0;

volatile float currentPH = 0.0, currentVolt = 0.0, currentEC = 0.0, currentTemp = 0.0;
volatile float currentGrossWeight = 0.0, tareOffset = 0.0, currentNetWeight = 0.0, currentNetWeightRaw = 0.0;
volatile int currentR = 0, currentG = 0, currentB = 0, currentC = 0;
bool phFilterInit = false, ecFilterInit = false, tempFilterInit = false, rgbFilterInit = false;
float phRawHist[3] = {0.0f}, ecRawHist[9] = {0.0f}, tempRawHist[3] = {0.0f};
int phRawHistCount = 0, phRawHistIndex = 0, ecRawHistCount = 0, ecRawHistIndex = 0, ecSpikeGuardCount = 0, tempRawHistCount = 0, tempRawHistIndex = 0;
int rgbRawHistR[3] = {0}, rgbRawHistG[3] = {0}, rgbRawHistB[3] = {0};
int rgbRawHistCount = 0, rgbRawHistIndex = 0;
float phFiltered = 0.0f, ecFiltered = 0.0f, tempFiltered = 0.0f, rgbFilteredR = 0.0f, rgbFilteredG = 0.0f, rgbFilteredB = 0.0f;
int phBumpConfirmCounter = 0, ecBumpConfirmCounter = 0;
float phLastNonRunning = 0.0f; bool phLastNonRunningValid = false;
float phLastNonRunningVolt = 0.0f; bool phLastNonRunningVoltValid = false;
float phRunVoltOffset = 0.0f; bool phRunVoltOffsetInit = false;
int phRunBurstCycles = 0;
bool displayWeightFilterInit = false; float displayWeightGuide = 0.0f; float displayWeightGuideSlope = 0.0f;
float w0_sum = 0.0, w0_sum_sq = 0.0, ph0_sum = 0.0, temp0_sum = 0.0, ec0_sum = 0.0;
long r0_sum = 0, g0_sum = 0, b0_sum = 0; int w0_count = 0, countdownTime = 0;
unsigned long pendingStartTime = 0;
float W0 = 0.0, W0_SD = 0.0, weightLoss = 0.0, PH0 = 0.0, Temp0 = 0.0, EC0 = 0.0;
int Red0 = 0, Green0 = 0, Blue0 = 0; float recStartWeight = 0.0f;
unsigned long lastBtn1Action = 0, lastBtn2Action = 0, lastBtn3Action = 0, btn1PressStart = 0, btn2PressStart = 0;
bool btn1HoldPending = false, btn2HoldPending = false;
const unsigned long STOP_HOLD_MS = 1500; const unsigned long RINSE_HOLD_MS = 300;
int lastRelayPinState = HIGH;

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 25200;
const int daylightOffset_sec = 0;
const char* TZ_INFO = "UTC-7";

bool ensureSDReady();
bool tryLockSD(uint32_t timeoutMs);
void unlockSD();
String getSyncStatusJSON();

uint64_t getFileModifiedTimeSafe(File &f) {
  time_t ts = f.getLastWrite();
  if (ts <= 0) return 0;
  return (uint64_t)ts;
}


// ================= FUNCTIONS DECLARATION =================
void preTransmission(); void postTransmission(); bool checkI2C(uint8_t addr);
bool checkPCWebServer(); void printSerialData(); bool logDataToSD(bool strictQueue = false);
void addSystemLog(const String &message); bool appendLogLine(const String &line);
size_t readLogLines(size_t maxLines, std::vector<String> &linesOut);
bool metricLog(const char *tag, const char *details, unsigned long &lastLogMs);
size_t getSDQueueSizeSnapshot(); bool flushSDQueueBeforeStop(unsigned long timeoutMs);
bool ensureSDReady(); void logSDInfo(); void setTimeZone(); bool syncTimeFromNtp();
void resetAutoStopFilter(); bool tryLockSD(uint32_t timeoutMs); void unlockSD();
void updateFileListCache(); bool tryCreateLogFile(const char *context, bool updateFileList);
bool ensureSDReady(); void logSDInfo(); void setTimeZone(); bool syncTimeFromNtp();
void resetAutoStopFilter(); bool tryLockSD(uint32_t timeoutMs); void unlockSD();
void updateFileListCache(); bool tryCreateLogFile(const char *context, bool updateFileList);
void disconnectClientByIP(const String &ip, bool logEvent);
bool isAdminClient(const String &ip); bool isControlClient(const String &ip); bool isAuthorizedClient(const String &ip);
bool isValidCustomFilename(const String &name); uint64_t getEpochMsNow(); String formatEpochMs(uint64_t epochMs);
void armRunLogClock(unsigned long stepMs); void disarmRunLogClock(); String nextRunLogTimestamp();
String getFormattedTimeMS(); String getFormattedTimeSec(); String createFileName(); String getDataCSV(const String &timestampText, int sampleIndex = -1);
bool enqueueSDLine(const String &line, size_t &queueSizeAfterPush);
bool isTxtLikeDataFile(const String &baseName);
bool buildFileListSlice(uint32_t cursor, uint32_t limit, String &out, uint32_t &totalCount, int32_t &nextCursor, uint32_t &sentCount);
bool readIndexFileSlice(uint32_t cursor, uint32_t limit, String &out, uint32_t &totalCount, int32_t &nextCursor, uint32_t &sentCount);
uint32_t countIndexFileLines();
void TaskSensorsCode(void * pvParameters); void printCentered(String text, int row);
float calibrateEC(float raw); void logCheck(String name, bool success);
float getSmoothedPH(); float phFromVoltage(float volt); float updateDisplayWeightFilter(float rawWeight, float dtSec);
String getRecTimeStr(); void setStopReason(const String &reason);
void stopToIdle(const String &reason, bool setRelayHigh = true);
String getCommandSourceTag(); void applyRelayState();
void toggleSystem(); void toggleRinse(); void toggleRecord();
void TaskSDWriterCode(void * pvParameters);

// ===== ISR & FLAGS THAT WERE MISSING =====
volatile bool flagBtn1 = false;
volatile bool flagBtn2 = false; 
volatile bool flagBtn3 = false; 
unsigned long lastInt1Time = 0; 
unsigned long lastInt2Time = 0; 
unsigned long lastInt3Time = 0;

void IRAM_ATTR isrButton1() { unsigned long t = millis(); if (t - lastInt1Time > 200) { flagBtn1 = true; lastInt1Time = t; } }
void IRAM_ATTR isrButton2() { unsigned long t = millis(); if (t - lastInt2Time > 200) { flagBtn2 = true; lastInt2Time = t; } }
void IRAM_ATTR isrButton3() { unsigned long t = millis(); if (t - lastInt3Time > 200) { flagBtn3 = true; lastInt3Time = t; } }

void preTransmission() { digitalWrite(MAX485_DE_RE, HIGH); }
void postTransmission() { digitalWrite(MAX485_DE_RE, LOW); }

// ===== ALGORITHMS THAT WERE MISSING =====
float clampFloat(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

float interpolate(float x, float in_val[], float out_val[], int size) {
  bool ascending = in_val[1] > in_val[0];
  if ((ascending && x <= in_val[0]) || (!ascending && x >= in_val[0])) {
    float slope = (out_val[1] - out_val[0]) / (in_val[1] - in_val[0]);
    return out_val[0] + (x - in_val[0]) * slope;
  }
  if ((ascending && x >= in_val[size - 1]) || (!ascending && x <= in_val[size - 1])) {
    float slope = (out_val[size - 1] - out_val[size - 2]) / (in_val[size - 1] - in_val[size - 2]);
    return out_val[size - 1] + (x - in_val[size - 1]) * slope;
  }
  for (int i = 0; i < size - 1; i++) {
    if (ascending) {
      if (x >= in_val[i] && x <= in_val[i+1]) {
         float slope = (out_val[i+1] - out_val[i]) / (in_val[i+1] - in_val[i]);
         return out_val[i] + (x - in_val[i]) * slope;
      }
    } else { 
      if (x <= in_val[i] && x >= in_val[i+1]) {
         float slope = (out_val[i+1] - out_val[i]) / (in_val[i+1] - in_val[i]);
         return out_val[i] + (x - in_val[i]) * slope;
      }
    }
  }
  return 0.0;
}

float median3f(float a, float b, float c) {
  if (a > b) { float t = a; a = b; b = t; }
  if (b > c) { float t = b; b = c; c = t; }
  if (a > b) { float t = a; a = b; b = t; }
  return b;
}

int median3i(int a, int b, int c) {
  if (a > b) { int t = a; a = b; b = t; }
  if (b > c) { int t = b; b = c; c = t; }
  if (a > b) { int t = a; a = b; b = t; }
  return b;
}

// median5 rejects up to 2 consecutive outliers, median7 up to 3. EC uses
// the largest available (median7) because observed early-RUN raw spikes can
// be 2-3 samples wide (e.g. Succinic-12: 1566 -> 3656 -> 4393 -> 3777 ->
// back to baseline), and even one spike sample that slips through is
// stretched into a visible bump by the downstream EMA. median is
// edge-preserving (ramps/steps pass unchanged; only isolated short-duration
// spikes are removed) and adds no lag on the real signal.
float median5f(float a, float b, float c, float d, float e) {
  float v[5] = {a, b, c, d, e};
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4 - i; j++) {
      if (v[j] > v[j+1]) { float t = v[j]; v[j] = v[j+1]; v[j+1] = t; }
    }
  }
  return v[2];
}

float median7f(float a, float b, float c, float d, float e, float f, float g) {
  float v[7] = {a, b, c, d, e, f, g};
  for (int i = 0; i < 6; i++) {
    for (int j = 0; j < 6 - i; j++) {
      if (v[j] > v[j+1]) { float t = v[j]; v[j] = v[j+1]; v[j+1] = t; }
    }
  }
  return v[3];
}

float median9f(float a, float b, float c, float d, float e, float f, float g, float h, float i) {
  float v[9] = {a, b, c, d, e, f, g, h, i};
  for (int k = 0; k < 8; k++) {
    for (int j = 0; j < 8 - k; j++) {
      if (v[j] > v[j+1]) { float t = v[j]; v[j] = v[j+1]; v[j+1] = t; }
    }
  }
  return v[4];
}

// ================= HTML PAGE (V.Y2026.88.17) =================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head><title>MSAT V.Y2026.88.17 (T)</title><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body{font-family:'Segoe UI',Roboto,Helvetica,Arial,sans-serif; text-align:center; margin-top:20px; background:#f0f2f5; color:#333;} 
  h2 { font-size: 26px; color: #1a237e; margin-bottom: 20px; font-weight: 700; }
  .card { background:white; padding:20px; margin:15px auto; max-width:480px; border-radius:12px; box-shadow:0 4px 10px rgba(0,0,0,0.05); border: 1px solid #e0e0e0; }
  .card-init { background:#e3f2fd; border: 1px solid #90caf9; } 
  .loss-box { background:#fff3e0; border: 1px solid #ffe0b2; padding: 12px; border-radius: 8px; margin: 15px 0; display:flex; justify-content:space-between; align-items:center; }
  .txt-loss-label { color:#e65100; font-weight:600; font-size:16px; }
  .txt-loss-val { color:#d84315; font-size: 22px; font-weight: 800; }
  .auto-stop-box { margin: 10px 0; background:#f1f8e9; padding:8px 12px; border-radius:8px; border:1px solid #c5e1a5; display:flex; flex-direction:column; align-items:flex-start; gap:6px; }
  .auto-title { font-size:14px; color:#33691e; font-weight:700; margin:0; }
  .auto-option { display:flex; align-items:center; gap:6px; white-space:nowrap; }
  .auto-radio { transform:scale(1.1); cursor:pointer; margin:0; }
  .auto-txt { font-size:13px; color:#33691e; font-weight:500; margin:0; }
  .auto-input { width:45px; margin:0 2px; padding:2px 4px; border:1px solid #aaa; border-radius:4px; text-align:center; font-weight:bold; font-size:13px; }
  .filename-box { margin: 10px 0; background:#fffde7; padding:8px 12px; border-radius:8px; border:1px solid #8B7500; display:flex; align-items:center; justify-content:center; gap:6px; flex-wrap:wrap; }
  .filename-label { font-size:14px; color:#8B7500; font-weight:600; margin:0; }
  .filename-format { font-size:12px; color:#8B7500; font-family:monospace; font-weight:500; }
  .filename-input { width:120px; padding:4px 8px; border:1px solid #8B7500; border-radius:4px; text-align:center; font-weight:bold; font-size:13px; background:#fff; }
  .data-grid { display:grid; grid-template-columns: 1fr 1fr; gap:10px; text-align:left; margin: 10px 0; }
  .span-2 { grid-column: span 2; }
  .data-item { background:#f8f9fa; padding:8px 12px; border-radius:8px; display:flex; justify-content:space-between; align-items:center; }
  .data-label { font-size:15px; color:#555; font-weight:500; display:flex; align-items:center; gap:5px; }
  .data-val { font-size:17px; font-weight:700; color:#0d47a1; transition: color 0.1s; }
  .unit { font-size:12px; color:#757575; margin-left:2px; font-weight:normal; }
  .btn { padding:14px 0; font-size:16px; cursor:pointer; background:#4CAF50; color:white; border:none; border-radius:8px; width:100%; transition: all 0.2s; font-weight: 600; box-shadow: 0 2px 5px rgba(0,0,0,0.15); letter-spacing: 0.5px; } 
  .btn:active { transform: scale(0.98); box-shadow: 0 1px 3px rgba(0,0,0,0.1); } 
  .btn:disabled { background: #bdbdbd !important; cursor: not-allowed; opacity: 0.7; box-shadow: none; transform: none; }
  .off { background:#f44336; } .pending { background:#ff9800; } .rinse { background:#039be5; } .rec { background:#9c27b0; }
  .tare-btn { background:#607d8b; font-size: 16px; margin-top: 0; padding: 12px 0; }
  .status-row { display: flex; justify-content: center; gap: 25px; margin-bottom: 15px; background: #fafafa; padding: 12px; border-radius: 8px; border: 1px solid #f0f0f0; }
  .status-item { display: flex; flex-direction: column; align-items: center; font-size: 11px; color: #757575; font-weight: 500; }
  .dot { height: 9px; width: 9px; border-radius: 50%; display: inline-block; margin-top: 5px; background-color: #e0e0e0; }
  .ok { background-color: #4CAF50; box-shadow: 0 0 5px #4CAF50; } .err { background-color: #f44336; }
  hr { border: 0; border-top: 1px solid #eeeeee; margin: 15px 0; }
  .init-item { background:rgba(255,255,255,0.6); } .init-val { color:#455a64; }
  .perf-indicator { display: inline-block; width: 8px; height: 8px; border-radius: 50%; margin: 0 2px; transition: all 0.3s; }
  .perf-ok { background: #4CAF50; box-shadow: 0 0 6px #4CAF50; }
  .perf-warn { background: #FFC107; box-shadow: 0 0 6px #FFC107; }
  .perf-error { background: #f44336; box-shadow: 0 0 6px #f44336; }
  .perf-offline { background: #9e9e9e; box-shadow: none; }
</style>
</head>
<body>
<h3>MSAT Controller <span id="wsStatus" style="color:#f44336;">(Disconnected)</span></h3>
<h4>(Multi-Sensor Automatic Titrator V.Y2026.88.17)</h4>

<div class="card">
  <div class="status-row">
    <div class="status-item">pH<span id="s_ads" class="dot"></span></div>
    <div class="status-item">EC<span id="s_ec" class="dot"></span></div>
    <div class="status-item">Temp<span id="s_temp" class="dot"></span></div>
    <div class="status-item">RGB<span id="s_rgb" class="dot"></span></div>
    <div class="status-item">Scale<span id="s_scale" class="dot"></span></div>
    <div class="status-item">SD<span id="s_sd" class="dot"></span></div>
    <div class="status-item">RTC<span id="s_rtc" class="dot"></span></div>
  </div>
  <div style="display:flex; justify-content:space-between; align-items:flex-start; margin-bottom:10px;">
      <div style="max-width:60%;">
        <div id="status" style="font-size:22px; font-weight:800; color:#333; line-height:1.3; word-break:break-word;">IDLE</div>
        <div id="syncBadge" style="font-size:12px; font-weight:700; color:#616161; margin-top:4px; display:none;">SYNC by PC</div>
      </div>
      <div style="text-align:right;">
          <div id="time" style="font-size:14px; color:#1a237e; font-weight:600;">Waiting...</div>
        <div id="yourIp" style="font-size:13px; color:#d32f2f; font-weight:600;">Your IP: -</div>
          <div id="recTime" style="font-size:14px; color:#d32f2f; font-weight:700; display:none;">00:00:00</div>
      </div>
  </div>
    <div id="autoStopInfo" style="font-size:14px; color:#2e7d32; margin-top:-6px; margin-bottom:4px; text-align:left; font-weight:700; display:none;">Auto-stop at: -</div>
    <div id="stopReason" style="font-size:12px; color:#666; margin-top:-6px; margin-bottom:8px; text-align:left; white-space:nowrap; overflow:hidden; text-overflow:ellipsis;">Last REC: -</div>
    <div id="perfInfo" style="font-size:11px; color:#999; margin-bottom:8px; text-align:left; font-family:monospace;"><span id="perfText">Connecting via WebSocket...</span></div>
  <hr>
  <div class="data-grid">
      <div class="data-item"><span class="data-label">🧪pH</span> <span class="data-val"><span id="ph">-</span></span></div>
      <div class="data-item"><span class="data-label">&#9889; Voltage</span> <span class="data-val"><span id="volt">-</span><span class="unit">V</span></span></div>
      <div class="data-item"><span class="data-label">🔌EC</span> <span class="data-val"><span id="ec">-</span><span class="unit">uS/cm</span></span></div>
      <div class="data-item"><span class="data-label">🌡️Temp</span> <span class="data-val"><span id="temp">-</span><span class="unit">&deg;C</span></span></div>
      <div class="data-item span-2"><span class="data-label">🎨RGB</span> <span class="data-val" style="font-size:14px;"><span id="rgb">-</span></span></div>
      <div class="data-item span-2"><span class="data-label">&#x2696; Weight</span> <span class="data-val"><span id="weight">-</span><span class="unit">g</span></span></div>
  </div>
  <div class="loss-box">
      <span class="txt-loss-label">📉 Weight Loss</span>
      <span><span id="loss" class="txt-loss-val">-</span> <span style="font-size:14px; color:#d84315;">g</span></span>
  </div>
  <div class="filename-box">
      <span class="filename-label">Filename:</span><span class="filename-format">YYYYMMDD_HHMMSS_</span><input type="text" id="txtFilename" maxlength="30" class="filename-input" 
       onkeypress="return isFilenameChar(event)" 
       onblur="validateFilenamePopup()"><span class="filename-format">.txt</span>
      <div style="font-size:12px; color:#000; margin-top:3px; text-align:center;">*Name includes "A-Z a-z 0-9 - _ ." only.</div>
  </div>
    <div class="auto-stop-box">
      <div class="auto-title">Auto-stop at:</div>
      <div class="auto-option"><input type="radio" name="autoMode" value="1" id="rdWeightLoss" class="auto-radio" checked><label for="rdWeightLoss" class="auto-txt">Weight loss =</label><input type="number" id="txtWeightLoss" value="85" step="1" min="1" class="auto-input" onblur="validateWeightInput(this)" oninput="validateWeightInput(this)"><span class="auto-txt">g</span></div>
      <div class="auto-option"><input type="radio" name="autoMode" value="2" id="rdTime" class="auto-radio"><label for="rdTime" class="auto-txt">Time =</label><input type="number" id="txtTime" value="5" step="1" min="1" class="auto-input"><span class="auto-txt">min</span></div>
      <div class="auto-option"><input type="radio" name="autoMode" value="0" id="rdNoStop" class="auto-radio"><label for="rdNoStop" class="auto-txt">No</label></div>
    </div>
  <div style="display:flex; flex-direction:column; gap:10px;">
      <button id="toggleBtn" class="btn" onclick="userToggle()">MSAT TITRATION START</button>
      <button id="recBtn" class="btn rec" onclick="userRec()">RECORD ONLY</button>
      <div style="display:grid; grid-template-columns: 1fr 1fr; gap:10px;">
          <button id="rinseBtn" class="btn rinse" onclick="userRinse()">RINSE</button>
          <button id="tareBtn" class="btn tare-btn" onclick="tareScale()">Tare Scale</button>
      </div>
  </div>
</div>

<div class="card card-init">
  <div class="data-grid">
      <div class="data-item init-item"><span class="data-label">🧪pH<sub>0</sub></span> <span class="data-val init-val"><span id="ph0">-</span></span></div>
      <div class="data-item init-item"><span class="data-label">&#9889; Voltage<sub>0</sub></span> <span class="data-val init-val" style="font-size:14px;"><span id="volt0">-</span><span class="unit">V</span></span></div>
      <div class="data-item init-item"><span class="data-label">🔌EC<sub>0</sub></span> <span class="data-val init-val"><span id="ec0">-</span><span class="unit">uS/cm</span></span></div>
      <div class="data-item init-item"><span class="data-label">🌡️Temp<sub>0</sub></span> <span class="data-val init-val"><span id="t0">-</span><span class="unit">&deg;C</span></span></div>
      <div class="data-item init-item span-2"><span class="data-label">🎨RGB<sub>0</sub></span> <span class="data-val init-val" style="font-size:14px;"><span id="rgb0">-</span></span></div>
      <div class="data-item init-item span-2"><span class="data-label">&#x2696; Weight<sub>0</sub></span> <span class="data-val init-val"><span id="w0">-</span><span class="unit">g</span></span></div>
  </div>
</div>

<div class="card" style="padding:15px;">
  <button id="rebootBtn" class="btn" onclick="rebootESP32()" style="background:#ef6c00; border:1px solid #e65100;">Reboot ESP32</button>
</div>

<script>
var ws;
var isCommandProcessing = false;
var lastState = 0;
var fallbackPollTimer = null;
var isSyncShown = false;
var myIpCache = "";

function updateDot(id, status) { document.getElementById(id).className = "dot " + (status ? "ok" : "err"); }
function perfDot(level) { return "<span class='perf-indicator perf-" + level + "'></span>"; }
function sanitizeFilenameInputValue(inputValue) { return inputValue.replace(/[^A-Za-z0-9._-]/g, ""); }
function validateAndPrepareFilename() {
  var input = document.getElementById("txtFilename"); var raw = input.value || ""; var cleaned = sanitizeFilenameInputValue(raw);
  if (cleaned.length > 30) cleaned = cleaned.substring(0, 30);
  if (cleaned !== raw) { input.value = cleaned; return null; }
  return cleaned;
}

function startFallbackPoll() {
  if (fallbackPollTimer) return;
  fallbackPollTimer = setInterval(function() {
    fetch('/data').then(function(r){ return r.json(); }).then(function(d){
      if (d.sync === 1) {
        if (!isSyncShown) { showSyncUI(); isSyncShown = true; }
      } else {
        if (isSyncShown) { isSyncShown = false; }
      }
    }).catch(function(){});
  }, 3000);
}

function stopFallbackPoll() {
  if (fallbackPollTimer) { clearInterval(fallbackPollTimer); fallbackPollTimer = null; }
  isSyncShown = false;
}

function showSyncUI() {
  var stat = document.getElementById('status');
  stat.innerHTML = 'SYNC'; stat.style.color = '#616161';
  var wsEl = document.getElementById('wsStatus');
  wsEl.innerText = '(SYNC)'; wsEl.style.color = '#FF9800';
  var perfEl = document.getElementById('perfText');
  if (perfEl) perfEl.innerHTML = '<span style=\"color:#9e9e9e\">&#x1f504; SYNC in progress &mdash; dashboard paused</span>';
  var ids = ['ph','volt','ec','weight','temp','rgb'];
  for (var i=0;i<ids.length;i++){ var el=document.getElementById(ids[i]); if(el) el.innerText='--'; }
  document.getElementById('loss').innerText = '--';
  document.getElementById('w0').innerText = '--'; document.getElementById('ph0').innerText = '--';
  document.getElementById('volt0').innerText = '--'; document.getElementById('ec0').innerText = '--';
  document.getElementById('t0').innerText = '--'; document.getElementById('rgb0').innerText = '--';
  var startBtn=document.getElementById('toggleBtn'), rinseBtn=document.getElementById('rinseBtn'), recBtn=document.getElementById('recBtn'), tareBtn=document.getElementById('tareBtn');
  startBtn.disabled=true; rinseBtn.disabled=true; recBtn.disabled=true; tareBtn.disabled=true;
  startBtn.className='btn'; startBtn.style.background='#cfd8dc'; startBtn.innerText='SYNC';
  rinseBtn.className='btn'; rinseBtn.style.background='#cfd8dc'; rinseBtn.innerText='RINSE';
  recBtn.className='btn'; recBtn.style.background='#cfd8dc'; recBtn.innerText='RECORD ONLY';
  tareBtn.style.background='#cfd8dc'; tareBtn.style.cursor='not-allowed'; tareBtn.innerText='Tare Scale';
  document.querySelector('.auto-stop-box').style.opacity='0.6'; document.querySelector('.filename-box').style.opacity='0.6';
  var syncBadge=document.getElementById('syncBadge'); if(syncBadge) syncBadge.style.display='block';
}

function initWebSocket() {
  var gateway = 'ws://' + window.location.hostname + '/ws';
  console.log("Connecting to WebSocket:", gateway);
  ws = new WebSocket(gateway);
  ws.onopen = function(evt) { 
      console.log("WS connected"); 
      stopFallbackPoll();
      document.getElementById("wsStatus").innerText = "(Live)"; 
      document.getElementById("wsStatus").style.color = "#4CAF50"; 
  };
  ws.onclose = function(evt) { 
      console.log("WS closed, retrying in 2s"); 
      document.getElementById("wsStatus").innerText = "(Offline)"; 
      document.getElementById("wsStatus").style.color = "#f44336";
      startFallbackPoll();
      setTimeout(initWebSocket, 2000); 
  };
  ws.onerror = function(evt) {
      console.error("WS error:", evt);
      document.getElementById("wsStatus").innerText = "(Error)";
      document.getElementById("wsStatus").style.color = "#f44336";
  };
  ws.onmessage = function(evt) {
      if(isCommandProcessing) return; // Prevent flicker during button presses
      try {
        var obj = JSON.parse(evt.data);
        updateDashboard(obj);
      } catch(e) {
        console.error("JSON parse error:", e, evt.data);
      }
  };
}

function updateDashboard(obj) {
  // Clear the user-entered filename suffix once the operation returns to
  // IDLE so the next run does not silently reuse the previous name.
  if ((lastState == 2 || lastState == 4) && obj.state == 0) {
    var fnEl = document.getElementById("txtFilename");
    if (fnEl) fnEl.value = "";
  }
  lastState = obj.state;
  let isSync = (obj.syncMode === true || obj.syncMode === "true");
  if (!isSync) {
    document.getElementById("ph").innerText = obj.ph.toFixed(2); document.getElementById("volt").innerText = obj.volt.toFixed(3);
    document.getElementById("ec").innerText = obj.ec.toFixed(0); document.getElementById("weight").innerText = obj.weight.toFixed(2);
    document.getElementById("temp").innerText = obj.temp.toFixed(1); document.getElementById("rgb").innerText = obj.r + "," + obj.g + "," + obj.b;
    if (obj.ip && obj.ip !== "Broadcast" && obj.ip !== "-") { myIpCache = obj.ip; }
    document.getElementById("yourIp").innerText = "Your IP: " + (myIpCache || "-");
  }
  document.getElementById("time").innerText = obj.time;
  if (!isSync) {
    let autoStopEl = document.getElementById("autoStopInfo");
    if (obj.state == 2 || obj.state == 4) {
      let mode = obj.runAutoMode; let autoText = "None";
      if (mode == 1) { autoText = "Weight loss = " + obj.runAutoWeight.toFixed(1) + " g"; } else if (mode == 2) { let minutes = Math.round(obj.runAutoTime / 60); autoText = "Time = " + minutes + " min"; }
      autoStopEl.innerText = "Auto-stop at: " + autoText; autoStopEl.style.display = "block";
    } else { autoStopEl.style.display = "none"; }
    let stopText = "Last REC: ";
    if (obj.lastRecFile && obj.lastRecFile !== "-") {
      let filename = obj.lastRecFile; let lastSlash = Math.max(filename.lastIndexOf('/'), filename.lastIndexOf('\\'));
      if (lastSlash !== -1) { filename = filename.substring(lastSlash + 1); }
      stopText += filename; if (obj.lastRecReason && obj.lastRecReason !== "-" && obj.lastRecReason !== "NONE") { stopText += " (" + obj.lastRecReason + ")"; }
    } else { stopText += "-"; }
    document.getElementById("stopReason").innerText = stopText;
    
    var qSize = (typeof obj.queueSize === "number") ? obj.queueSize : 0;
    var wifiRssi = (typeof obj.wifiRssi === "number") ? obj.wifiRssi : -99;
    var queueLevel = qSize >= 200 ? "error" : (qSize >= 50 ? "warn" : "ok");
    var wifiLevel = wifiRssi < -80 ? "error" : (wifiRssi < -67 ? "warn" : "ok");
    var line1 = perfDot(queueLevel) + "SD Queue: " + qSize + " | " + perfDot(wifiLevel) + "WiFi: " + wifiRssi + "dBm";
    document.getElementById("perfText").innerHTML = line1 + "<br>⚡ <b>WebSocket Stream Active</b>";
  }
  let recTimeEl = document.getElementById("recTime");
  if (obj.state == 2 || obj.state == 4) { recTimeEl.innerText = obj.recTime; recTimeEl.style.display = "block"; } else { recTimeEl.style.display = "none"; }
  let syncBadge = document.getElementById("syncBadge"); if (syncBadge) { syncBadge.style.display = isSync ? "block" : "none"; }
  let isRunning = (obj.state != 0 || isSync);
  document.getElementById("rdNoStop").disabled = isRunning; document.getElementById("rdWeightLoss").disabled = isRunning; document.getElementById("rdTime").disabled = isRunning;
  document.getElementById("txtWeightLoss").disabled = isRunning; document.getElementById("txtTime").disabled = isRunning; document.getElementById("txtFilename").disabled = isRunning;
  document.querySelector(".auto-stop-box").style.opacity = isRunning ? "0.6" : "1.0"; document.querySelector(".filename-box").style.opacity = isRunning ? "0.6" : "1.0";
  if (!isSync) {
    if (obj.state == 2) { 
        document.getElementById("w0").innerText = obj.w0.toFixed(2); document.getElementById("ph0").innerText = obj.ph0.toFixed(2);
        document.getElementById("volt0").innerText = "-"; document.getElementById("ec0").innerText = obj.ec0.toFixed(0); 
        document.getElementById("t0").innerText = obj.temp0.toFixed(1); document.getElementById("rgb0").innerText = obj.r0 + "," + obj.g0 + "," + obj.b0;
        document.getElementById("loss").innerText = obj.loss.toFixed(2);
    } else {
        document.getElementById("loss").innerText = (obj.state == 4) ? obj.loss.toFixed(2) : "-"; 
        document.getElementById("w0").innerText = "-"; document.getElementById("ph0").innerText = "-"; document.getElementById("volt0").innerText = "-";
        document.getElementById("ec0").innerText = "-"; document.getElementById("t0").innerText = "-"; document.getElementById("rgb0").innerText = "-";
    }
    updateDot("s_ads", obj.st_ads); updateDot("s_rgb", obj.st_rgb); updateDot("s_temp", obj.st_temp); updateDot("s_scale", obj.st_scale); updateDot("s_ec", obj.st_ec); updateDot("s_sd", obj.st_sd); updateDot("s_rtc", obj.st_rtc);
  }
  let startBtn = document.getElementById("toggleBtn"); let rinseBtn = document.getElementById("rinseBtn"); let recBtn = document.getElementById("recBtn");
  let tareBtn = document.getElementById("tareBtn"); let stat = document.getElementById("status");
  startBtn.disabled = false; rinseBtn.disabled = false; recBtn.disabled = false;
  if (obj.state == 0) { tareBtn.disabled = false; tareBtn.style.background = "#607d8b"; tareBtn.style.cursor = "pointer"; tareBtn.innerText = "Tare Scale"; } else { tareBtn.disabled = true; tareBtn.style.background = "#cfd8dc"; tareBtn.style.cursor = "not-allowed"; }
  if (isSync) {
    startBtn.disabled = true; rinseBtn.disabled = true; recBtn.disabled = true; tareBtn.disabled = true;
    startBtn.className = "btn"; startBtn.style.background = "#cfd8dc"; startBtn.innerText = "SYNC";
    rinseBtn.className = "btn"; rinseBtn.style.background = "#cfd8dc"; rinseBtn.innerText = "RINSE";
    recBtn.className = "btn"; recBtn.style.background = "#cfd8dc"; recBtn.innerText = "RECORD ONLY";
    tareBtn.style.background = "#cfd8dc"; tareBtn.style.cursor = "not-allowed"; tareBtn.innerText = "Tare Scale";
    stat.innerHTML = "SYNC"; stat.style.color = "#616161";
  } else if(obj.state == 0) { 
    startBtn.innerText = "MSAT TITRATION START"; startBtn.className = "btn"; startBtn.style.background = ""; 
    rinseBtn.innerText = "RINSE"; rinseBtn.className = "btn rinse"; rinseBtn.style.background = ""; 
    recBtn.innerText = "RECORD ONLY"; recBtn.className = "btn rec"; recBtn.style.background = "";
    stat.innerHTML = "IDLE"; stat.style.color = "#333";
  } else if (obj.state == 1) { 
    startBtn.innerText = "CANCEL"; startBtn.className = "btn pending"; startBtn.style.background = ""; 
    rinseBtn.disabled = true; recBtn.disabled = true;
    stat.innerHTML = "PENDING (" + obj.countdown + "s)"; stat.style.color = "#ef6c00";
  } else if (obj.state == 2) { 
    startBtn.innerText = "STOP"; startBtn.className = "btn off"; startBtn.style.background = ""; 
    rinseBtn.disabled = true; recBtn.disabled = true;
    var runFile = (obj.logfile && obj.logfile !== "-") ? obj.logfile : "-";
    stat.innerHTML = "REC<div style='font-size:14px; font-weight:700; margin-top:2px;'>" + (runFile.length > 14 ? runFile.substring(14) : runFile) + "</div>"; stat.style.color = "#d32f2f";
  } else if (obj.state == 3) { 
    startBtn.disabled = true; recBtn.disabled = true;
    rinseBtn.innerText = "STOP RINSE"; rinseBtn.className = "btn off"; rinseBtn.style.background = ""; 
    stat.innerHTML = "RINSING..."; stat.style.color = "#0277bd";
  } else if (obj.state == 4) { 
    startBtn.disabled = true; rinseBtn.disabled = true;
    recBtn.innerText = "STOP"; recBtn.className = "btn off"; recBtn.style.background = "";
    var recFile = (obj.logfile && obj.logfile !== "-") ? obj.logfile : "-";
    stat.innerHTML = "REC ONLY<div style='font-size:14px; font-weight:700; margin-top:2px;'>" + (recFile.length > 14 ? recFile.substring(14) : recFile) + "</div>"; stat.style.color = "#7b1fa2";
  }
}

// Button HTTP Actions
function validateWeightInput(el) {
  var raw = el.value;
  if (raw === '' || raw === null) return true;
  if (raw.indexOf('.') !== -1) { alert('Weight target must be a whole number.\nDecimals are not allowed (e.g. use 85, not 85.5).'); el.value = Math.round(parseFloat(raw)) || 85; return false; }
  if (!/^\d+$/.test(raw) || parseInt(raw, 10) < 1) { alert('Invalid input. Please enter a positive whole number (e.g. 85).'); el.value = 85; return false; }
  return true;
}
function userToggle() { 
  if(isCommandProcessing) return;
  if (lastState == 1) { if (!confirm("Cancel pending process?")) return; } else if (lastState == 2) { if (!confirm("Stop current operation?")) return; }
  var wEl = document.getElementById("txtWeightLoss"); if (!validateWeightInput(wEl)) return;
  isCommandProcessing = true; let btn = document.getElementById("toggleBtn"); btn.disabled = true; btn.innerText = "SENDING...";
  let mode = document.querySelector('input[name="autoMode"]:checked').value; let weightTarget = wEl.value; let timeTarget = document.getElementById("txtTime").value;
  let preparedName = validateAndPrepareFilename();
  fetch("/toggle?mode=" + mode + "&weight=" + weightTarget + "&time=" + timeTarget + "&fname=" + encodeURIComponent(preparedName||"")).then(r => {
    setTimeout(() => { isCommandProcessing = false; }, 300);
  });
}
function userRinse() { 
  if(isCommandProcessing) return; isCommandProcessing = true; let btn = document.getElementById("rinseBtn"); btn.disabled = true; btn.innerText = "..."; 
  fetch("/rinse").then(r => { setTimeout(() => { isCommandProcessing = false; }, 300); });
}
function userRec() {
  if(isCommandProcessing) return; if (lastState == 4) { if (!confirm("Stop REC mode?")) return; }
  isCommandProcessing = true; let btn = document.getElementById("recBtn"); btn.disabled = true; btn.innerText = "...";
  let preparedName = validateAndPrepareFilename();
  let mode = document.querySelector('input[name="autoMode"]:checked').value; let timeTarget = document.getElementById("txtTime").value;
  fetch("/record?mode=" + mode + "&time=" + timeTarget + "&fname=" + encodeURIComponent(preparedName||"")).then(r => { setTimeout(() => { isCommandProcessing = false; }, 300); });
}
function tareScale() { 
  if(isCommandProcessing) return; isCommandProcessing = true; let btn = document.getElementById("tareBtn"); btn.innerText = "Taring...";
  fetch("/tare").then(r => { setTimeout(() => { isCommandProcessing = false; }, 300); }); 
}
function rebootESP32() {
  if (isCommandProcessing) return;
  if (!confirm("Reboot ESP32 now?")) return;
  isCommandProcessing = true;
  let btn = document.getElementById("rebootBtn");
  if (btn) {
    btn.disabled = true;
    btn.innerText = "Rebooting...";
  }
  fetch("/restart").then(r => {
    setTimeout(() => { isCommandProcessing = false; }, 300);
  }).catch(e => {
    isCommandProcessing = false;
  });
}
//===== Input Validation: Character Filter & Alert Functions =====

function isFilenameChar(evt) {
  var charCode = (evt.which) ? evt.which : evt.keyCode;
  var charStr = String.fromCharCode(charCode);
  // Allow only A-Z, a-z, 0-9, -, _, .
  if (/[a-zA-Z0-9._-]/.test(charStr)) return true;
  return false;
}

function validateFilenamePopup() {
  var input = document.getElementById("txtFilename");
  // If invalid characters are detected (e.g. via paste), strip them and alert
  if (input.value && /[^a-zA-Z0-9._-]/.test(input.value)) {
    alert("Invalid characters detected! Only A-Z, a-z, 0-9, . , - , _ are allowed.");
    input.value = input.value.replace(/[^a-zA-Z0-9._-]/g, "");
  }
}

//=====================================================
window.addEventListener('load', initWebSocket);
</script></body></html>
)rawliteral";

// ================= WS & HTTP HANDLERS (ASYNC) =================

String getTelemetryJSON(String clientIP) {
  String json; json.reserve(900); json = "{";
  json += "\"state\":" + String(currentState) + ",\"countdown\":" + String(countdownTime) + ",";
  json += "\"w0\":" + String(W0) + ",\"ph0\":" + String(PH0) + ",\"temp0\":" + String(Temp0) + ",\"ec0\":" + String(EC0) + ",";
  json += "\"r0\":" + String(Red0) + ",\"g0\":" + String(Green0) + ",\"b0\":" + String(Blue0) + ",";
  json += "\"ph\":" + String(currentPH) + ",\"volt\":" + String(currentVolt) + ",\"ec\":" + String(currentEC) + ",";
  json += "\"weight\":" + String(currentNetWeight) + ",\"temp\":" + String(currentTemp) + ",";
  json += "\"r\":" + String(currentR) + ",\"g\":" + String(currentG) + ",\"b\":" + String(currentB) + ",";
  json += "\"loss\":" + String(weightLoss) + ",";
  json += "\"st_ads\":" + String(st_ads) + ",\"st_rgb\":" + String(st_rgb) + ",\"st_temp\":" + String(st_temp) + ",";
  json += "\"st_scale\":" + String(st_scale) + ",\"st_sd\":" + String(st_sd) + ",\"st_ec\":" + String(st_ec) + ",\"st_rtc\":" + String(st_rtc) + ",";
  json += "\"recTime\":\"" + getRecTimeStr() + "\",\"logfile\":\"" + (currentState == 2 || currentState == 4 ? currentLogFileName : "-") + "\",";
  json += "\"lastRecFile\":\"" + (lastCompletedRecFileName.length() > 0 ? lastCompletedRecFileName : "-") + "\",\"lastRecReason\":\"" + lastCompletedStopReason + "\",";
  json += "\"runAutoMode\":" + String(runAutoStopMode) + ",\"runAutoWeight\":" + String(runAutoStopWeightTarget, 2) + ",\"runAutoTime\":" + String(runAutoStopTimeTarget) + ",";

  size_t queueSizeSnapshot = 0;
  if (sdQueueMutex) {
    if (!xSemaphoreTake(sdQueueMutex, pdMS_TO_TICKS(10))) {
      queueSizeSnapshot = 999;
    } else {
      queueSizeSnapshot = sdWriteQueue.size();
      xSemaphoreGive(sdQueueMutex);
    }
  }

  String ipForUi = (clientIP == "Broadcast") ? "-" : clientIP;
  json += "\"queueSize\":" + String((unsigned int)queueSizeSnapshot) + ",\"wifiRssi\":" + String(WiFi.RSSI()) + ",";
  json += "\"syncMode\":" + String(syncMode ? "true" : "false") + ",";
  json += "\"ip\":\"" + ipForUi + "\",\"time\":\"" + getFormattedTimeSec() + "\"}";
  return json;
}

String getSyncStatusJSON() {
  unsigned long now = millis();
  unsigned long hbAge = (lastSyncHeartbeatMs > 0 && now >= lastSyncHeartbeatMs) ? (now - lastSyncHeartbeatMs) : 0;
  unsigned long rebuildAge = (rebuildStartedMs > 0 && now >= rebuildStartedMs) ? (now - rebuildStartedMs) : 0;
  String json = "{";
  json += "\"syncMode\":" + String(syncMode ? "true" : "false") + ",";
  json += "\"syncTimeoutMs\":" + String((unsigned long)SYNC_TIMEOUT_MS) + ",";
  json += "\"lastHeartbeatAgeMs\":" + String(hbAge) + ",";
  json += "\"rebuildRequested\":" + String(rebuildIndexRequested ? "true" : "false") + ",";
  json += "\"rebuildRunning\":" + String(rebuildIndexInProgress ? "true" : "false") + ",";
  json += "\"rebuildProgress\":" + String(rebuildProgress) + ",";
  json += "\"rebuildOwner\":\"" + rebuildOwnerIP + "\",";
  json += "\"rebuildStatus\":\"" + rebuildStatusText + "\",";
  json += "\"rebuildAgeMs\":" + String(rebuildAge);
  json += "}";
  return json;
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if(type == WS_EVT_CONNECT){
    // Verbose logging disabled to prevent Serial race condition
    // Serial.printf("WS Client %u connected IP: %s (Total: %d)\n", client->id(), client->remoteIP().toString().c_str(), server->count());
    client->text(getTelemetryJSON(client->remoteIP().toString()));
  } else if(type == WS_EVT_DISCONNECT){
    // Serial.printf("WS Client %u disconnected (Remaining: %d)\n", client->id(), server->count());
  } else if(type == WS_EVT_DATA){
    // Process incoming WebSocket data from dashboard
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    if(info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT){
      String msg = "";
      for(size_t i = 0; i < len; i++) {
        msg += (char) data[i];
      }
      // Serial.printf("WS Data from Client %u: %s\n", client->id(), msg.c_str());  // Verbose logging disabled
      
      // Send acknowledgment back to client
      client->text("ACK:" + msg);
    }
  }
}

// Setup HTTP Endpoints
// WiFi Event Handler - Auto Reconnect on Disconnect
void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch(event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      // Only log during setup; runtime reconnects logged elsewhere
      if (!wifiEverConnected) {
        Serial.println("[WiFi] [OK] Connected! IP: " + WiFi.localIP().toString());
      } else {
        Serial.println("[WiFi] [OK] Reconnected! IP: " + WiFi.localIP().toString());
      }
      if (webServerStarted) {
        Serial.printf("[WiFi] Server is ONLINE - access: http://%s\n", WiFi.localIP().toString().c_str());
      }
      wifiConnected = true;
      wifiEverConnected = true;
      wifiReconnecting = false;
      lastWifiReconnectAttempt = 0;
      break;
    
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      {
        uint8_t reason = info.wifi_sta_disconnected.reason;
        // Log only first disconnect to avoid spam
        static unsigned long lastDisconnectLog = 0;
        unsigned long now = millis();
        if (now - lastDisconnectLog > 5000) {  // Only log every 5 seconds
          if (wifiEverConnected) {
            Serial.printf("[WiFi] [ERR] Link lost (reason=%u). Auto-recovery active...\n", reason);
          } else {
            Serial.printf("[WiFi] [ERR] Connect attempt failed (reason=%u). Retrying...\n", reason);
          }
          lastDisconnectLog = now;
        }
        wifiConnected = false;
        // Don't call WiFi.begin() here - let the main loop handle it
      }
      break;
    
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      Serial.println("[WiFi] [WARN] Lost IP address");
      wifiConnected = false;
      break;
      
    default:
      break;
  }
}

// WiFi Reconnection Logic
void checkAndReconnectWiFi() {
  wl_status_t st = WiFi.status();
  unsigned long now = millis();

  // Skip if already connected
  if (st == WL_CONNECTED) {
    if (!wifiConnected) {
      wifiConnected = true;
      Serial.println("[WiFi] [OK] Stable connection confirmed");
    }
    wifiReconnecting = false;
    lastWifiReconnectAttempt = 0;
    return;
  }

  // If an attempt is already running, wait for timeout before declaring failure.
  if (wifiReconnecting) {
    unsigned long elapsed = millis() - lastWifiReconnectAttempt;
    if (elapsed >= WIFI_RECONNECT_TIMEOUT_MS) {
      wifiReconnecting = false;
      Serial.println("[WiFi] [ERR] Reconnection timeout, will retry in 60s");
      lastWifiReconnectAttempt = millis();
    }
    return;
  }

  // Skip in cooldown period
  if ((now - lastWifiReconnectAttempt < WIFI_RECONNECT_COOLDOWN_MS)) {
    return;
  }
  
  // Start reconnection attempt
  wifiReconnecting = true;
  lastWifiReconnectAttempt = now;
  wifiConnected = false;
  
  Serial.println("[WiFi] [RETRY] Attempting reconnection...");
  Serial.printf("[WiFi] status=%d, RSSI=%d dBm\n", (int)st, WiFi.RSSI());
  
  // Reset STA state completely to clear cached state and ghost connections
  WiFi.disconnect(true);  // true = turn off modem, clear NVRAM cache
  delay(1000);  // Give modem 1 second to settle
  WiFi.mode(WIFI_STA);

  // Re-apply static IP config before reconnecting
  WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS);
  
  // Use begin() with known credentials and let event handler report success.
  WiFi.begin(ssid, password);
}

// Setup HTTP Endpoints
// Setup HTTP Endpoints
void setupServerRoutes() {
  Serial.println("[Server] Setting up routes...");
  
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ 
    // Serial.println("[Server] GET /");  // Verbose logging disabled
    request->redirect("/dashboard"); 
  });
  
  server.on("/dashboard", HTTP_GET, [](AsyncWebServerRequest *request){
    if (syncMode) {
      request->send(503, "text/html",
        "<html><head><meta charset='UTF-8'><meta http-equiv='refresh' content='5'>"
        "<title>MSAT - SYNC</title>"
        "<style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{background:#111;color:#eee;font-family:'Segoe UI',monospace;min-height:100vh;display:flex;flex-direction:column;align-items:center}"
        ".top-bar{width:100%;background:#1a1a2e;padding:12px 20px;display:flex;justify-content:space-between;align-items:center;border-bottom:2px solid #0f0}"
        ".top-bar h2{color:#0f0;font-size:1.1em;letter-spacing:1px}"
        ".top-bar .ver{color:#555;font-size:0.8em}"
        ".sync-box{margin-top:60px;text-align:center;padding:40px}"
        ".sync-box h1{font-size:3.5em;color:#0f0;letter-spacing:8px;text-shadow:0 0 20px #0f03;animation:pulse 2s ease-in-out infinite}"
        "@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.5}}"
        ".sync-box p{color:#888;font-size:1.1em;margin-top:16px;line-height:1.8}"
        ".sync-box .spinner{display:inline-block;width:24px;height:24px;border:3px solid #333;border-top-color:#0f0;border-radius:50%;animation:spin 1s linear infinite;vertical-align:middle;margin-right:8px}"
        "@keyframes spin{to{transform:rotate(360deg)}}"
        ".btn-row{display:flex;gap:14px;justify-content:center;margin-top:50px;flex-wrap:wrap}"
        ".btn{padding:14px 32px;border:2px solid #333;border-radius:8px;background:#1a1a1a;color:#555;font-size:1em;cursor:not-allowed;opacity:0.4;user-select:none;min-width:140px;text-align:center}"
        ".status-row{display:flex;gap:20px;justify-content:center;margin-top:40px;flex-wrap:wrap}"
        ".status-card{background:#1a1a1a;border:1px solid #333;border-radius:8px;padding:16px 24px;min-width:120px;text-align:center}"
        ".status-card .label{color:#555;font-size:0.8em;text-transform:uppercase;letter-spacing:1px}"
        ".status-card .val{color:#444;font-size:1.6em;margin-top:4px}"
        "</style></head>"
        "<body>"
        "<div class='top-bar'><h2>MSAT AUTOTITRATION</h2><span class='ver'>V.Y2026.88.17 | SYNC MODE</span></div>"
        "<div class='sync-box'>"
        "<h1>- SYNC -</h1>"
        "<p><span class='spinner'></span>File synchronization in progress</p>"
        "<p style='color:#555;font-size:0.9em'>Dashboard will resume automatically after sync completes</p>"
        "</div>"
        "<div class='btn-row'>"
        "<div class='btn'>START</div>"
        "<div class='btn'>RINSE</div>"
        "<div class='btn'>REC ONLY</div>"
        "<div class='btn'>TARE</div>"
        "</div>"
        "<div class='status-row'>"
        "<div class='status-card'><div class='label'>pH</div><div class='val'>--</div></div>"
        "<div class='status-card'><div class='label'>EC</div><div class='val'>--</div></div>"
        "<div class='status-card'><div class='label'>Temp</div><div class='val'>--</div></div>"
        "<div class='status-card'><div class='label'>Weight</div><div class='val'>--</div></div>"
        "</div>"
        "</body></html>");
      return;
    }
    request->send_P(200, "text/html", index_html);
  });

  server.on("/toggle", HTTP_GET, [](AsyncWebServerRequest *request){
    if (syncMode) { request->send(503, "text/plain", "SYNC_ACTIVE"); return; }
    lastCommandSource = 0;
    if (request->hasParam("mode")) autoStopMode = request->getParam("mode")->value().toInt();
    if (request->hasParam("weight")) autoStopWeightTarget = request->getParam("weight")->value().toFloat();
    if (request->hasParam("time")) autoStopTimeTarget = request->getParam("time")->value().toInt() * 60;
    if (request->hasParam("fname")) {
      String candidate = request->getParam("fname")->value();
      if (isValidCustomFilename(candidate)) customFilename = candidate;
    }
    webCmdToggle = true; // Deferred to loop() for processing
    request->send(200, "text/plain", "OK");
  });

  server.on("/rinse", HTTP_GET, [](AsyncWebServerRequest *request){
    if (syncMode) { request->send(503, "text/plain", "SYNC_ACTIVE"); return; }
    lastCommandSource = 0; webCmdRinse = true; request->send(200, "text/plain", "OK");
  });

  server.on("/record", HTTP_GET, [](AsyncWebServerRequest *request){
    if (syncMode) { request->send(503, "text/plain", "SYNC_ACTIVE"); return; }
    lastCommandSource = 0;
    if (request->hasParam("mode")) autoStopMode = request->getParam("mode")->value().toInt();
    if (request->hasParam("time")) autoStopTimeTarget = request->getParam("time")->value().toInt() * 60;
    if (request->hasParam("fname")) {
      String candidate = request->getParam("fname")->value();
      if (isValidCustomFilename(candidate)) customFilename = candidate;
    }
    webCmdRecord = true; // Deferred to loop() for processing
    request->send(200, "text/plain", "OK");
  });

  server.on("/tare", HTTP_GET, [](AsyncWebServerRequest *request){
    webCmdTare = true; request->send(200, "text/plain", "OK");
  });

  server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", "RESTARTING");
    delay(150);
    ESP.restart();
  });

  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request){
    // Return JSON with system state and SD status
    String stateStr = "UNKNOWN";
    if (currentState == 0) stateStr = "IDLE";
    else if (currentState == 1) stateStr = "PENDING";
    else if (currentState == 2) stateStr = "RUNNING";
    else if (currentState == 3) stateStr = "RINSING";
    else if (currentState == 4) stateStr = "REC";
    
    String jsonResponse = "{\"state\":" + String(currentState > 0 ? 1 : 0) + 
                          ",\"is_logging\":" + String(isLogging ? 1 : 0) +
                          ",\"state_name\":\"" + stateStr + "\"" +
                          ",\"st_sd\":" + String(st_sd ? 1 : 0) +
                          ",\"sync\":" + String(syncMode ? 1 : 0) +
                          "}";
    request->send(200, "application/json", jsonResponse);
  });

  server.on("/sync", HTTP_GET, [](AsyncWebServerRequest *request){
    String cmd = "";
    if (request->hasParam("cmd")) {
      cmd = request->getParam("cmd")->value();
    } else if (request->hasParam("on")) {
      String on = request->getParam("on")->value();
      cmd = (on == "1") ? "start" : "stop";
    }

    cmd.toLowerCase();
    if (cmd == "start") {
      syncMode = true;
      lastSyncHeartbeatMs = millis();
      ws.closeAll();
      ws.enable(false);
      Serial.println("[SYNC] Mode started (WebSocket disabled, dashboard locked)");
      request->send(200, "text/plain", "SYNC_STARTED");
      return;
    }
    if (cmd == "heartbeat") {
      lastSyncHeartbeatMs = millis();
      request->send(200, "text/plain", "ACK");
      return;
    }
    if (cmd == "stop") {
      syncMode = false;
      ws.enable(true);
      Serial.println("[SYNC] Mode stopped (WebSocket enabled, dashboard unlocked)");
      request->send(200, "text/plain", "SYNC_STOPPED");
      return;
    }

    request->send(400, "text/plain", "INVALID_CMD");
  });

  server.on("/sync_status", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "application/json", getSyncStatusJSON());
  });

  server.on("/fwid", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", FW_BUILD_ID);
  });

  server.on("/sync_manifest", HTTP_GET, [](AsyncWebServerRequest *request){
    bool rebuild = false;
    if (request->hasParam("rebuild")) {
      String v = request->getParam("rebuild")->value();
      rebuild = (v == "1" || v == "true" || v == "TRUE");
    }

    if (rebuild) {
      if (manifestRebuildInProgress) {
        request->send(202, "text/plain", "REBUILDING");
        return;
      }
      manifestRebuildRequested = true;
      request->send(202, "text/plain", "REBUILD_QUEUED");
      return;
    }

    if (manifestRebuildInProgress) {
      request->send(202, "text/plain", "REBUILDING");
      return;
    }

    String indexContent;
    bool readSuccess = false;
    
    if (sdCardMutex && xSemaphoreTake(sdCardMutex, pdMS_TO_TICKS(1000))) {
      File idx = SD.open(INDEX_FILE_PATH, FILE_READ);
      if (idx) {
        size_t filesizeBytes = idx.size();
        if (filesizeBytes > 0 && filesizeBytes < 65536) {
          indexContent.reserve(filesizeBytes + 512);
          byte buf[512];
          size_t totalRead = 0;
          while (idx.available()) {
            size_t toRead = (filesizeBytes - totalRead > sizeof(buf)) ? sizeof(buf) : (filesizeBytes - totalRead);
            size_t n = idx.readBytes((char*)buf, toRead);
            if (n > 0) {
              for (size_t i = 0; i < n; i++) {
                indexContent += (char)buf[i];
              }
              totalRead += n;
            } else {
              break;
            }
          }
          readSuccess = (totalRead > 0);
        } else if (filesizeBytes == 0) {
          // File is empty - write failed; re-queue rebuild (done above), treat as not ready
          readSuccess = false;
        }
        idx.close();
        Serial.printf("[SYNC] /sync_manifest: read index (%u bytes, success=%d)\n", (unsigned int)filesizeBytes, readSuccess ? 1 : 0);
        if (filesizeBytes == 0 && !manifestRebuildInProgress) {
          Serial.println("[SYNC] /sync_manifest: index 0 bytes, re-queuing rebuild");
          manifestRebuildRequested = true;
        }
      } else {
        Serial.println("[SYNC] /sync_manifest: index file exists but cannot open");
      }
      xSemaphoreGive(sdCardMutex);
    } else {
      Serial.println("[SYNC] /sync_manifest: SD lock timeout");
    }

    if (!readSuccess) {
      if (!manifestRebuildInProgress && !manifestRebuildRequested) {
        manifestRebuildRequested = true;
      }
      request->send(503, "text/plain", "INDEX_NOT_READY");
      return;
    }
    request->send(200, "text/plain", indexContent);
  });

  server.on("/listfiles_raw", HTTP_GET, [](AsyncWebServerRequest *request){
    if (manifestRebuildInProgress) {
      request->send(503, "text/plain", "REBUILDING");
      return;
    }
    uint32_t limit = 0;
    if (request->hasParam("limit")) {
      String p = request->getParam("limit")->value();
      if (!(p == "all" || p == "ALL")) {
        int v = p.toInt();
        if (v > 0) limit = (uint32_t)v;
      }
    }

    String body;
    uint32_t totalCount = 0;
    uint32_t sentCount = 0;
    int32_t nextCursor = -1;
    bool ok = readIndexFileSlice(0, limit, body, totalCount, nextCursor, sentCount);
    if (!ok) {
      request->send(503, "text/plain", "SD Fail");
      return;
    }
    request->send(200, "text/plain", body);
  });

  server.on("/listfiles", HTTP_GET, [](AsyncWebServerRequest *request){
    if (manifestRebuildInProgress) {
      request->send(503, "text/plain", "REBUILDING");
      return;
    }
    uint32_t limit = 0;
    if (request->hasParam("limit")) {
      String p = request->getParam("limit")->value();
      if (!(p == "all" || p == "ALL")) {
        int v = p.toInt();
        if (v > 0) limit = (uint32_t)v;
      }
    }

    String body;
    uint32_t totalCount = 0;
    uint32_t sentCount = 0;
    int32_t nextCursor = -1;
    bool ok = readIndexFileSlice(0, limit, body, totalCount, nextCursor, sentCount);
    if (!ok) {
      request->send(503, "text/plain", "SD Fail");
      return;
    }
    request->send(200, "text/plain", body);
  });

  server.on("/listfiles_cursor", HTTP_GET, [](AsyncWebServerRequest *request){
    if (manifestRebuildInProgress) {
      request->send(503, "text/plain", "REBUILDING");
      return;
    }
    uint32_t cursor = 0;
    uint32_t limit = 120;

    if (request->hasParam("cursor")) {
      int v = request->getParam("cursor")->value().toInt();
      if (v > 0) cursor = (uint32_t)v;
    }
    if (request->hasParam("limit")) {
      int v = request->getParam("limit")->value().toInt();
      if (v > 0) {
        limit = (uint32_t)v;
        if (limit > 500) limit = 500;
      }
    }

    String body;
    uint32_t totalCount = 0;
    uint32_t sentCount = 0;
    int32_t nextCursor = -1;
    bool ok = readIndexFileSlice(cursor, limit, body, totalCount, nextCursor, sentCount);
    if (!ok) {
      request->send(503, "text/plain", "SD Fail");
      return;
    }

    fileListGeneration++;
    lastFileListUpdateMs = millis();

    AsyncWebServerResponse *resp = request->beginResponse(200, "text/plain", body);
    resp->addHeader("X-Next-Cursor", String(nextCursor));
    resp->addHeader("X-List-Generation", String(fileListGeneration));
    request->send(resp);
  });

  server.on("/listcount", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasParam("refresh")) {
      manifestRebuildRequested = true;
    }

    uint32_t cnt = countIndexFileLines();

    if (cnt > 0) {
      fileListGeneration++;
      lastFileListUpdateMs = millis();
    }

    String json = "{";
    json += "\"count\":" + String(cnt) + ",";
    json += "\"refreshing\":" + String((manifestRebuildInProgress || manifestRebuildRequested) ? 1 : 0) + ",";
    json += "\"generation\":" + String(fileListGeneration) + ",";
    json += "\"updated\":" + String(lastFileListUpdateMs);
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/rebuild_status", HTTP_GET, [](AsyncWebServerRequest *request){
    String json = "{";
    json += "\"rebuilding\":" + String(manifestRebuildInProgress ? 1 : 0) + ",";
    json += "\"rebuild_requested\":" + String(manifestRebuildRequested ? 1 : 0) + ",";
    json += "\"generation\":" + String(fileListGeneration) + ",";
    json += "\"uptime_ms\":" + String(millis());
    json += "}";
    request->send(200, "application/json", json);
  });

  // ===== Batch delete from /autotttoutput (used by MSAT-SYNC cleanup) =====
  // Body: newline-separated filenames (basename only, no path). Each name
  // is hard-validated by isValidDeleteFileName(). Only runs when state==IDLE
  // and no index rebuild is in progress. After the batch, a single full
  // index rebuild is requested so _index.txt reflects the deletes.
  server.on("/delete_batch", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      String body;
      if (request->_tempObject) {
        body = *((String*)request->_tempObject);
        delete (String*)request->_tempObject;
        request->_tempObject = nullptr;
      }
      if (currentState != 0) {
        request->send(409, "application/json", "{\"error\":\"not_idle\",\"deleted\":0,\"failed\":[]}");
        return;
      }
      if (manifestRebuildInProgress) {
        request->send(409, "application/json", "{\"error\":\"rebuilding\",\"deleted\":0,\"failed\":[]}");
        return;
      }
      if (body.length() == 0) {
        request->send(400, "application/json", "{\"error\":\"empty_body\",\"deleted\":0,\"failed\":[]}");
        return;
      }

      std::vector<String> names;
      int s = 0;
      while (s <= (int)body.length()) {
        int nl = body.indexOf('\n', s);
        if (nl < 0) nl = body.length();
        String line = body.substring(s, nl);
        line.replace("\r", "");
        line.trim();
        if (line.length() > 0) names.push_back(line);
        if (nl >= (int)body.length()) break;
        s = nl + 1;
      }
      if (names.empty()) {
        request->send(400, "application/json", "{\"error\":\"no_files\",\"deleted\":0,\"failed\":[]}");
        return;
      }

      if (!tryLockSD(SD_LOCK_TIMEOUT_MS)) {
        request->send(503, "application/json", "{\"error\":\"sd_busy\",\"deleted\":0,\"failed\":[]}");
        return;
      }

      uint32_t deleted = 0;
      String failedJson = "";
      uint32_t failCount = 0;
      const uint32_t failListCap = 50; // cap the echoed names; counter is exact
      for (auto &n : names) {
        bool ok = false;
        if (isValidDeleteFileName(n)) {
          String full = "/autotttoutput/" + n;
          if (SD.exists(full)) ok = SD.remove(full);
        }
        if (ok) {
          deleted++;
        } else {
          if (failCount < failListCap) {
            if (failedJson.length() > 0) failedJson += ",";
            failedJson += "\"" + n + "\"";
          }
          failCount++;
        }
      }
      unlockSD();

      // One full rebuild at the end so _index.txt mirrors the SD again.
      manifestRebuildRequested = true;

      String resp = "{\"deleted\":" + String(deleted) +
                    ",\"failed_count\":" + String(failCount) +
                    ",\"failed\":[" + failedJson + "]}";
      request->send(200, "application/json", resp);
    },
    NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (index == 0) {
        String *b = new String();
        if (total > 0 && total < 131072) b->reserve(total + 1);
        request->_tempObject = (void*)b;
      }
      if (request->_tempObject) {
        String *b = (String*)request->_tempObject;
        b->concat((const char*)data, len);
      }
    }
  );

  // ===== Download File from SD Card (stable direct stream) =====
  server.on("/down", HTTP_GET, [](AsyncWebServerRequest *request){
    if(!request->hasParam("f")){
      request->send(400, "text/plain", "Missing file parameter");
      return;
    }
    
    String path = request->getParam("f")->value();
    
    // Check if the file exists
    bool fileExists = false;
    if(sdCardMutex && xSemaphoreTake(sdCardMutex, pdMS_TO_TICKS(1000))){
      if(SD.exists(path)){
        File f = SD.open(path, FILE_READ);
        if(f){
          fileExists = true;
          f.close();
        }
      }
      xSemaphoreGive(sdCardMutex);
    }
    
    if(!fileExists){
      Serial.printf("[DOWN] 404 not found: %s\n", path.c_str());
      request->send(404, "text/plain", "File not found on SD Card");
      return;
    }
    
    AsyncWebServerResponse *response = request->beginResponse(SD, path, "application/octet-stream", true);
    
    if(response){
      String filename = path.substring(path.lastIndexOf('/') + 1);
      response->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
      response->addHeader("Cache-Control", "no-cache");
      response->addHeader("Connection", "keep-alive");
      request->send(response);
    } else {
      request->send(500, "text/plain", "Failed to open file");
    }
  });
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200); delay(1000);
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = 15000,
    .idle_core_mask = 0,
    .trigger_panic = false,
  };
  esp_err_t twdtErr = esp_task_wdt_reconfigure(&twdt_config);
  if (twdtErr == ESP_ERR_INVALID_STATE) {
    twdtErr = esp_task_wdt_init(&twdt_config);
  }
  if (twdtErr == ESP_OK) {
    twdtInitialized = true;
    Serial.println("[WDT] TaskWDT initialized successfully");
  } else {
    Serial.printf("[WDT] Warning: init/reconfigure failed (%d) - watchdog disabled\n", (int)twdtErr);
  }
  setTimeZone(); timeSynced = false; lastTimeSyncMs = millis();
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); 
  Serial.println("\n\n========================================");
  Serial.println("  AUTOTITRATION V.Y2026.88.17 (WEBSOCKET, SYNC-MANIFEST)  "); Serial.println("========================================");
  Serial.printf("[Version] FW_VERSION: %s\n", FW_VERSION);
  Serial.printf("[Version] FW_BUILD_ID: %s\n", FW_BUILD_ID);
  
  pinMode(RELAY_PIN, OUTPUT); digitalWrite(RELAY_PIN, HIGH); lastRelayPinState = HIGH;
  pinMode(MAX485_DE_RE, OUTPUT); digitalWrite(MAX485_DE_RE, LOW); 
  pinMode(BUTTON1_PIN, INPUT_PULLUP); attachInterrupt(digitalPinToInterrupt(BUTTON1_PIN), isrButton1, FALLING);
  pinMode(BUTTON2_PIN, INPUT_PULLUP); attachInterrupt(digitalPinToInterrupt(BUTTON2_PIN), isrButton2, FALLING);
  pinMode(BUTTON3_PIN, INPUT_PULLUP); attachInterrupt(digitalPinToInterrupt(BUTTON3_PIN), isrButton3, FALLING);
  Wire.begin(I2C_SDA, I2C_SCL); 
  lcd.begin(16, 2); lcd.backlight(); 
  
  bool rtcOK = rtc.begin(); logCheck("RTC", rtcOK); if(rtcOK) { st_rtc=true; DateTime now = rtc.now(); struct timeval tv = { now.unixtime(), 0 }; settimeofday(&tv, NULL); } else { st_rtc=false; }
  bool adsOK = ads.begin(); logCheck("ADS1115", adsOK); st_ads = adsOK;
  bool rgbOK = tcs.begin(); logCheck("RGB Color", rgbOK); st_rgb = rgbOK;
  sensors.begin(); sensors.setWaitForConversion(false); bool tempOK = (sensors.getDeviceCount() > 0); logCheck("Temp Sensor", tempOK); st_temp = tempOK;
  scale.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN); logCheck("Scale", true); st_scale=true; 
  
  sdCardMutex = xSemaphoreCreateMutex(); bool sdOK = false;
  if (sdCardMutex) {
    delay(1200); xSemaphoreTake(sdCardMutex, portMAX_DELAY);
    sdOK = SD.begin(SD_CS_PIN, SPI, SD_SPI_FREQ);
    if (sdOK && !SD.exists("/autotttoutput")) SD.mkdir("/autotttoutput");
    if (sdOK) {
      bool needIndexRebuild = !SD.exists(INDEX_FILE_PATH);
      if (!needIndexRebuild) {
        File idxCheck = SD.open(INDEX_FILE_PATH, FILE_READ);
        if (!idxCheck || idxCheck.size() == 0) {
          needIndexRebuild = true;
        }
        if (idxCheck) idxCheck.close();
      }
      if (needIndexRebuild) {
        manifestRebuildRequested = true;
        Serial.println("[SYNC] Index rebuild queued at boot");
      }
    }
    if (sdOK) { logSDInfo(); }
    xSemaphoreGive(sdCardMutex);
  }
  logCheck("SD Card", sdOK); st_sd = sdOK;
  
  Serial2.begin(9600, SERIAL_8N1, MAX485_RX, MAX485_TX); Serial2.setTimeout(100); node.begin(1, Serial2); node.preTransmission(preTransmission); node.postTransmission(postTransmission); uint8_t res = node.readHoldingRegisters(0x0000, 1); bool ecOK = (res == node.ku8MBSuccess); logCheck("EC Meter", ecOK); st_ec = ecOK;

  lcd.clear(); lcd.setCursor(0,0); lcd.print("WiFi Connecting");
  
  // Register WiFi event handler for auto-reconnect
  WiFi.onEvent(onWiFiEvent);
  
  // Configure WiFi with power-saving disabled and auto-reconnect enabled
  // No explicit disconnect needed - let the WiFi library handle it
  delay(1000); 
  WiFi.setSleep(false);           // Disable power saving
  WiFi.setAutoReconnect(false);   // DISABLE during setup to prevent config race
  WiFi.persistent(false);         // Don't save to flash (prevent flash wear)
  
  // Correct order: mode → begin → config
  WiFi.mode(WIFI_STA);
  
  // Set Static IP before begin() (ESP32)
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("[WiFi] ⚠ Static IP config failed!");
  }
  
  WiFi.begin(ssid, password);
  Serial.printf("[WiFi] Connecting to SSID: %s (IP: %s)\n", ssid, local_IP.toString().c_str());
  int retries = 0;
  const int WIFI_CONNECT_TIMEOUT_RETRIES = 30;  // 30 x 2 seconds = 60 seconds timeout for slow routers
  while (WiFi.status() != WL_CONNECTED && retries < WIFI_CONNECT_TIMEOUT_RETRIES) {
    lcd.setCursor(0,1);
    // Display countdown timer on LCD showing time elapsed and total
    lcd.print("Wait " + String((retries + 1) * 2) + "s / 60s ");
    Serial.print(".");
    delay(2000);  // 2-second interval gives Router time to respond
    retries++;
  }
  Serial.println();
  lcd.clear();
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    wifiEverConnected = true;
    WiFi.setAutoReconnect(false);  // Manual reconnect only to avoid race with custom logic
    lcd.setCursor(0,0); lcd.print("WiFi Connected!"); 
    lcd.setCursor(0,1); lcd.print(WiFi.localIP());
    timeSynced = syncTimeFromNtp(); lastTimeSyncMs = millis();
    Serial.println("[WiFi] ========== CONNECTED ==========");
    Serial.printf("[WiFi] IP Address: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WiFi] Gateway: %s\n", gateway.toString().c_str());
    Serial.printf("[WiFi] Subnet: %s\n", subnet.toString().c_str());
    Serial.printf("[WiFi] SSID: %s\n", WiFi.SSID().c_str());
    Serial.printf("[WiFi] RSSI: %d dBm\n", WiFi.RSSI());
    Serial.printf("[WiFi] MAC: %s\n", WiFi.macAddress().c_str());
  } else {
    wifiConnected = false;
    // Keep manual reconnect handling only
    WiFi.setAutoReconnect(false);
    lcd.setCursor(0,0); lcd.print("WiFi Wait...");
    lcd.setCursor(0,1); lcd.print("Server Active");
    Serial.println("[WiFi] ========== CONNECTION FAILED ==========");
    Serial.printf("[WiFi] Status: %d (see below)\n", WiFi.status());
    Serial.println("[WiFi] WiFi Status Codes:");
    Serial.println("[WiFi]   0=IDLE, 1=NO_SSID_AVAIL, 2=SCAN_COMPLETE");
    Serial.println("[WiFi]   3=CONNECTED, 4=CONNECT_FAIL, 5=CONNECTION_LOST, 6=DISCONNECTED");
    Serial.printf("[WiFi] Current Status: %d\n", WiFi.status());
    Serial.printf("[WiFi] MAC Address: %s\n", WiFi.macAddress().c_str());
    Serial.println("[WiFi] Troubleshooting:");
    Serial.printf("[WiFi]   - Check SSID exists: \"%s\"\n", ssid);
    Serial.printf("[WiFi]   - Check password: \"%s\"\n", password);
    Serial.printf("[WiFi]   - Static IP attempted: %s\n", local_IP.toString().c_str());
    Serial.println("[WiFi]   - Check router nearby and WiFi enabled");
    Serial.println("[WiFi]   - Check if Static IP conflicts with another device");
    Serial.println("[WiFi]   - Try scanning networks with phone to verify SSID");
    Serial.println("[WiFi] Retrying in background...");
    Serial.println("[WiFi] Server standby: waiting for WiFi before access is possible");
  }
  delay(1500);

  sdQueueMutex = xSemaphoreCreateMutex();
  fileListMutex = xSemaphoreCreateMutex();
  clientListMutex = xSemaphoreCreateMutex();

  Serial.println("[System] Creating FreeRTOS tasks...");
  xTaskCreatePinnedToCore(TaskSensorsCode, "TaskSensors", 10000, NULL, 2, &TaskSensors, 0);
  Serial.println("[System] TaskSensors created on Core 0");
  xTaskCreatePinnedToCore(TaskSDWriterCode, "TaskSDWriter", 8000, NULL, 1, &TaskSDWriter, 1);  // SD writes at lower priority on Core 1
  Serial.println("[System] TaskSDWriter created on Core 1");
  
  Serial.println("[WebSocket] Attaching WebSocket handler...");
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  
  Serial.println("[Server] Configuring routes...");
  setupServerRoutes();
  
  Serial.println("[Server] Starting web server on port 80...");
  server.begin();
  webServerStarted = true;
  
  // ==========================================
    // Web server startup status
  // ==========================================
  lcd.clear(); 
  lcd.setCursor(0,0); lcd.print("Web Server Start");
  lcd.setCursor(0,1); lcd.print("Checking...");
    Serial.println("[System] Verifying Web Server startup state...");

    delay(500); // Allow the async listener task to finish startup bookkeeping

  lcd.clear();
  lcd.setCursor(0,0); lcd.print("Web Server:");
  lcd.setCursor(0,1);
  if (WiFi.status() != WL_CONNECTED) {
    lcd.print("[ WAIT WIFI ]");
      Serial.println("[System] Web Server start check skipped: WiFi not connected yet");
    } else if (webServerStarted) {
    lcd.print("[ READY ]");
      Serial.println("[System] Web Server started successfully");
  } else {
    lcd.print("[ FAILED ]");
      Serial.println("[System] ERROR: Web Server failed to start");
  }
  delay(2000); // Show status on LCD for 2 seconds
  // ==========================================
  
  Serial.println("================ WEB SERVER STARTED ================");
  Serial.printf("*** Dashboard: http://%s/dashboard ***\n", local_IP.toString().c_str());
  Serial.printf("*** Root: http://%s/ ***\n", local_IP.toString().c_str());
  
  // Start file list cache in background (non-blocking)
  fileListRefreshRequested = true;
  
  lcd.clear(); Serial.println("================ SYSTEM READY ================");
}

void loop() { 
  // WiFi Health Check & Auto Reconnect every 10 seconds
  if (millis() - lastWifiCheck >= WIFI_CHECK_INTERVAL_MS) {
    lastWifiCheck = millis();
    checkAndReconnectWiFi();
  }

  if (syncMode && !rebuildIndexInProgress && (millis() - lastSyncHeartbeatMs > SYNC_TIMEOUT_MS)) {
    syncMode = false;
    ws.enable(true);
    Serial.println(">> SYNC MODE TIMEOUT: Auto-exiting (WebSocket enabled)");
  }
  
  // Send WebSocket Broadcast with adaptive frequency
  // Reduces network load when multiple devices connected
  static unsigned long lastWsBroadcast = 0;
  static unsigned long lastWsCleanup = 0;
  // User-configurable WS interval by state group
  unsigned long broadcastInterval = WS_REFRESH_IDLE_MS;
  if (currentState == 2 || currentState == 4) {
    broadcastInterval = WS_REFRESH_RUN_REC_MS;
  } else if (currentState == 1 || currentState == 3) {
    broadcastInterval = WS_REFRESH_PENDING_RINSE_MS;
  }
  
  if (!syncMode && (millis() - lastWsBroadcast > broadcastInterval)) {
    lastWsBroadcast = millis();
    // Guard: skip broadcast if heap is critically low to prevent malloc panic
    // (heap can fragment after long runs with many SD queue allocations)
    if (ws.count() > 0 && ESP.getFreeHeap() > 8192) {
      // REVERTED to the library's own synchronized broadcast. The previous
      // per-client loop over ws.getClients() from loop() raced the AsyncTCP
      // task: if a client disconnected mid-iteration the client list was
      // mutated under us -> iterator invalidation -> heap corruption ->
      // ESP32 panic/reboot loop -> dashboard unreachable. ws.textAll() with
      // the availableForWriteAll() guard is the library-safe path. (WL
      // dashboard lag is the lesser evil vs. the web crashing; revisit with
      // a snapshot-copy approach if the lag must be fixed.)
      if (ws.availableForWriteAll()) {
        ws.textAll(getTelemetryJSON("-"));
      }
    }
  }
  
  // Separate cleanup from broadcast to reduce blocking
  if (millis() - lastWsCleanup > 2000) {  // 2s cleanup to release stale client frame buffers sooner
    lastWsCleanup = millis();
    ws.cleanupClients();
  }

  applyRelayState();
  if (!timeSynced && wifiConnected) {
    if (millis() - lastTimeSyncMs >= TIME_SYNC_RETRY_MS) { timeSynced = syncTimeFromNtp(); lastTimeSyncMs = millis(); }
  }
  
  // Buttons handling
  if (flagBtn1) { 
    flagBtn1 = false; 
    if (currentState == 0) { if (millis() - lastBtn1Action > BUTTON_COOLDOWN_MS) { lastBtn1Action = millis(); lastCommandSource = 1; toggleSystem(); } } 
    else { btn1PressStart = millis(); btn1HoldPending = true; }
  }
  if (btn1HoldPending) {
    if (digitalRead(BUTTON1_PIN) == LOW) {
      if (millis() - btn1PressStart >= STOP_HOLD_MS) { btn1HoldPending = false; if (millis() - lastBtn1Action > BUTTON_COOLDOWN_MS) { lastBtn1Action = millis(); lastCommandSource = 1; toggleSystem(); } }
    } else { btn1HoldPending = false; }
  }
  
  if (flagBtn2) { flagBtn2 = false; btn2PressStart = millis(); btn2HoldPending = true; }
  if (btn2HoldPending) {
    if (digitalRead(BUTTON2_PIN) == LOW) {
      if (millis() - btn2PressStart >= RINSE_HOLD_MS) { btn2HoldPending = false; if (millis() - lastBtn2Action > BUTTON_COOLDOWN_MS) { lastBtn2Action = millis(); lastCommandSource = 1; toggleRinse(); } }
    } else { btn2HoldPending = false; }
  }
  
  if (flagBtn3) { flagBtn3 = false; if (millis() - lastBtn3Action > 200) { lastBtn3Action = millis(); lcdPage++; int maxPage = (currentState == 2) ? 6 : 5; if(lcdPage > maxPage) lcdPage = 0; } }
  // Process web commands here in loop() to prevent Async Task stall causing board reset
  if (webCmdToggle) { webCmdToggle = false; toggleSystem(); }
  if (webCmdRinse)  { webCmdRinse = false; toggleRinse(); }
  if (webCmdRecord) { webCmdRecord = false; toggleRecord(); }
  if (webCmdTare)   { 
    webCmdTare = false; 
    tareOffset = currentGrossWeight; currentNetWeightRaw = currentGrossWeight - tareOffset; currentNetWeight = currentNetWeightRaw;
    displayWeightGuide = currentNetWeightRaw; displayWeightGuideSlope = 0.0f; displayWeightFilterInit = true;
  }
  delay(1); 
}

void TaskSensorsCode(void * pvParameters) {
  unsigned long lastStep = 0, lastLog = 0, lastEC = 0, lastStatus = 0, lastLCD = 0; unsigned long lastLoopMs = 0; unsigned long lastSerial = 0;
  static bool hasLastValidNetWeight = false; static float lastValidNetWeight = 0.0f; static int lastStateForLogSync = -1;
  static int weightRejectCount = 0;
  static int ecAdaptiveHoldCycles = 0;
  static int ecRunSettleCycles = 0;
  static int ecAnomalySkips = 0;
  static unsigned long lastCountdownUpdate = 0;  // Add countdown update timer
  
  bool wdtRegistered = false;
  if (twdtInitialized) {
    esp_err_t wdtAddErr = esp_task_wdt_add(NULL); // Register this task with watchdog supervision
    wdtRegistered = (wdtAddErr == ESP_OK || wdtAddErr == ESP_ERR_INVALID_STATE);
    if (!wdtRegistered) {
      Serial.printf("[WDT] Warning: TaskSensors add failed (%d)\n", (int)wdtAddErr);
    }
  }

  for(;;) {
    unsigned long now = millis();
    if (syncMode) {
      if (now - lastLCD >= 400) {
        lastLCD = now;
        printCentered("- SYNC BY PC -", 0);
        printCentered("WAIT...", 1);
      }
      if (wdtRegistered) esp_task_wdt_reset();
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    unsigned long sensorInterval = SENSOR_IDLE_MS; unsigned long ecInterval = EC_IDLE_MS; unsigned long lcdInterval = LCD_IDLE_MS; unsigned long logInterval = LOG_IDLE_MS;
    if (currentState == 1) { sensorInterval = SENSOR_PENDING_MS; ecInterval = EC_PENDING_MS; lcdInterval = LCD_PENDING_MS; logInterval = LOG_PENDING_MS; } 
    else if (currentState == 2) { sensorInterval = SENSOR_RUNNING_MS; ecInterval = EC_RUNNING_MS; lcdInterval = LCD_RUNNING_MS; logInterval = LOG_RUNNING_MS; } 
    else if (currentState == 3) { sensorInterval = SENSOR_RINSE_MS; ecInterval = EC_RINSE_MS; lcdInterval = LCD_RINSE_MS; logInterval = LOG_RINSE_MS; } 
    else if (currentState == 4) { sensorInterval = SENSOR_REC_MS; ecInterval = EC_REC_MS; lcdInterval = LCD_REC_MS; logInterval = LOG_REC_MS; }
    
    if (currentState != lastStateForLogSync) {
      // Reset lastLog when entering PENDING (state 1) or REC_ONLY (state 4) to restart counting immediately
      if (currentState == 1 || currentState == 4) { lastLog = now; }
      if (currentState == 2 || currentState == 4) {
        // Reset anti-spike baseline on state entry to avoid carrying stale value across runs.
        hasLastValidNetWeight = false;
        weightRejectCount = 0;
        ecAdaptiveHoldCycles = 0;
        ecRunSettleCycles = SENSOR_EC_RUN_SETTLE_CYCLES;
        ecAnomalySkips = 0;
        ecBumpConfirmCounter = 0;
        phBumpConfirmCounter = 0;
        displayWeightFilterInit = false;
        phRunVoltOffset = 0.0f;
        phRunVoltOffsetInit = false;
        phRunBurstCycles = 0;
      }
      if (currentState == 2 && phLastNonRunningValid) { phFiltered = phLastNonRunning; phFilterInit = true; currentPH = phFiltered; }
      lastStateForLogSync = currentState;
    }
    
    if (now - lastStep >= sensorInterval) { lastStep = now;
      if (scale.is_ready()) {
        long r = scale.read(); int sz = sizeof(loadcell_raw) / sizeof(loadcell_raw[0]); float raw_arr_f[sz];
        for(int i=0; i<sz; i++) raw_arr_f[i] = (float)loadcell_raw[i];
        currentGrossWeight = interpolate((float)r, raw_arr_f, loadcell_weight, sz);
        currentNetWeightRaw = currentGrossWeight - tareOffset;
        float dtWeightSec = (float)sensorInterval / 1000.0f;
        currentNetWeight = updateDisplayWeightFilter(currentNetWeightRaw, dtWeightSec); lastScaleSuccess = now;
      }
      float phNow = getSmoothedPH();
      if (currentState == 2 && phLastNonRunningVoltValid) {
        float targetOffset = currentVolt - phLastNonRunningVolt;
        if (targetOffset < 0.0f) targetOffset = 0.0f;
        if (!phRunVoltOffsetInit) {
          phRunVoltOffset = targetOffset;
          phRunVoltOffsetInit = true;
        } else {
          if (targetOffset > phRunVoltOffset) {
            // Grow quickly when EMI grows.
            phRunVoltOffset += SENSOR_PH_RUN_EMI_OFFSET_ALPHA * (targetOffset - phRunVoltOffset);
          } else {
            // Release faster when offset drops, so pH does not stick at the high end.
            float releaseAlpha = SENSOR_PH_RUN_EMI_RELEASE_ALPHA;
            if (targetOffset <= 0.03f) releaseAlpha *= 1.5f;
            phRunVoltOffset += releaseAlpha * (targetOffset - phRunVoltOffset);
          }
        }
        phRunVoltOffset = clampFloat(phRunVoltOffset, 0.0f, SENSOR_PH_RUN_EMI_OFFSET_MAX);
        // Apply less EMI offset once voltage falls below non-RUNNING anchor.
        float offsetScale = 1.0f;
        if (currentVolt <= phLastNonRunningVolt) offsetScale = 0.20f;
        if (currentVolt <= (phLastNonRunningVolt - 0.10f)) offsetScale = 0.0f;
        float correctedVolt = currentVolt - (phRunVoltOffset * offsetScale);
        if (correctedVolt < 0.0f) correctedVolt = 0.0f;
        phNow = phFromVoltage(correctedVolt);
      }
      phRawHist[phRawHistIndex] = phNow; phRawHistIndex = (phRawHistIndex + 1) % 3;
      if (phRawHistCount < 3) phRawHistCount++;
      float phMed = (phRawHistCount >= 3) ? median3f(phRawHist[0], phRawHist[1], phRawHist[2]) : phNow;
      if (!phFilterInit) {
        phFiltered = phMed;
        phFilterInit = true;
      } else {
        bool phWarped = false;
        float phInput = phMed;
        float phAlpha = SENSOR_PH_EMA_ALPHA;
        float runMaxStepPerSec = SENSOR_PH_RUN_MAX_STEP_PER_SEC;
        if (currentState == 2) {
          float rawDelta = phMed - phFiltered;
          float absRawDelta = fabsf(rawDelta);

          if (absRawDelta >= SENSOR_PH_BUMP_THRESHOLD_DELTA) {
            phBumpConfirmCounter++;
          } else {
            phBumpConfirmCounter = 0;
          }

          if (phBumpConfirmCounter >= SENSOR_PH_BUMP_THRESHOLD_COUNT) {
            // Confirmed true process jump: warp immediately to avoid endpoint lag.
            phFiltered = phMed;
            phBumpConfirmCounter = 0;
            phWarped = true;
          }

          if (!phWarped) {
          if (absRawDelta >= SENSOR_PH_RUN_BURST_TRIGGER_DELTA) {
            phRunBurstCycles = SENSOR_PH_RUN_BURST_HOLD_CYCLES;
          } else if (phRunBurstCycles > 0) {
            phRunBurstCycles--;
          }

          if (phRunBurstCycles > 0) {
            // Endpoint zone: allow fast pH movement for a short time window.
            phAlpha = SENSOR_PH_RUN_BURST_ALPHA;
            runMaxStepPerSec = SENSOR_PH_RUN_BURST_MAX_STEP_PER_SEC;
          }

          // Clip only absurd jumps, but do not hard-hold.
          if (fabsf(rawDelta) >= SENSOR_PH_RUN_SPIKE_REJECT_DELTA) {
            phInput = phFiltered + ((rawDelta > 0.0f) ? SENSOR_PH_RUN_SPIKE_REJECT_DELTA : -SENSOR_PH_RUN_SPIKE_REJECT_DELTA);
          }
          }
        } else {
          phBumpConfirmCounter = 0;
          phRunBurstCycles = 0;
        }
        if (!phWarped) {
          float phCandidate = phFiltered + phAlpha * (phInput - phFiltered);
          if (currentState == 2) {
            float dtPhSec = (float)sensorInterval / 1000.0f;
            if (dtPhSec < 0.01f) dtPhSec = 0.01f;
            float allowedStep = runMaxStepPerSec * dtPhSec;
            float delta = phCandidate - phFiltered;
            if (delta > allowedStep) delta = allowedStep;
            if (delta < -allowedStep) delta = -allowedStep;
            phFiltered += delta;
          } else {
            phFiltered = phCandidate;
          }
        }
      }
      currentPH = phFiltered;
      if (currentState != 2) {
        phBumpConfirmCounter = 0;
        phLastNonRunning = phFiltered; phLastNonRunningValid = true;
        phLastNonRunningVolt = currentVolt; phLastNonRunningVoltValid = st_ads;
        phRunVoltOffset = 0.0f; phRunVoltOffsetInit = false;
        phRunBurstCycles = 0;
      }
      if (st_temp) {
        float t = sensors.getTempCByIndex(0); bool validTempRange = (t >= SENSOR_TEMP_VALID_MIN_C && t <= SENSOR_TEMP_VALID_MAX_C);
        bool isPowerOnMarker = (fabsf(t - SENSOR_TEMP_POR_INVALID_C) <= SENSOR_TEMP_POR_EPS_C);
        if (validTempRange && !isPowerOnMarker) {
          tempRawHist[tempRawHistIndex] = t; tempRawHistIndex = (tempRawHistIndex + 1) % 3;
          if (tempRawHistCount < 3) tempRawHistCount++;
          float tMed = (tempRawHistCount >= 3) ? median3f(tempRawHist[0], tempRawHist[1], tempRawHist[2]) : t;
          if (!tempFilterInit) { tempFiltered = tMed; tempFilterInit = true; } else { tempFiltered += SENSOR_TEMP_EMA_ALPHA * (tMed - tempFiltered); }
          currentTemp = tempFiltered;
        }
        sensors.requestTemperatures();
      }
      if (st_rgb) {
        uint16_t r,g,b,c; tcs.getRawData(&r,&g,&b,&c);
        rgbRawHistR[rgbRawHistIndex] = (int)r; rgbRawHistG[rgbRawHistIndex] = (int)g; rgbRawHistB[rgbRawHistIndex] = (int)b;
        rgbRawHistIndex = (rgbRawHistIndex + 1) % 3; if (rgbRawHistCount < 3) rgbRawHistCount++;
        int rMed = (rgbRawHistCount >= 3) ? median3i(rgbRawHistR[0], rgbRawHistR[1], rgbRawHistR[2]) : (int)r;
        int gMed = (rgbRawHistCount >= 3) ? median3i(rgbRawHistG[0], rgbRawHistG[1], rgbRawHistG[2]) : (int)g;
        int bMed = (rgbRawHistCount >= 3) ? median3i(rgbRawHistB[0], rgbRawHistB[1], rgbRawHistB[2]) : (int)b;
        if (!rgbFilterInit) { rgbFilteredR = (float)rMed; rgbFilteredG = (float)gMed; rgbFilteredB = (float)bMed; rgbFilterInit = true; } 
        else { rgbFilteredR += SENSOR_RGB_EMA_ALPHA * ((float)rMed - rgbFilteredR); rgbFilteredG += SENSOR_RGB_EMA_ALPHA * ((float)gMed - rgbFilteredG); rgbFilteredB += SENSOR_RGB_EMA_ALPHA * ((float)bMed - rgbFilteredB); }
        currentR = (int)(rgbFilteredR + 0.5f); currentG = (int)(rgbFilteredG + 0.5f); currentB = (int)(rgbFilteredB + 0.5f); currentC = (int)c;
      }
    }
    if (now - lastEC >= ecInterval) {
      lastEC = now; uint8_t res = node.readHoldingRegisters(0x0000, 2);
      if (res == node.ku8MBSuccess) {
        float rawEC = (float)node.getResponseBuffer(0); float ecNow = calibrateEC(rawEC);
        ecRawHist[ecRawHistIndex] = ecNow; ecRawHistIndex = (ecRawHistIndex + 1) % 9; if (ecRawHistCount < 9) ecRawHistCount++;
        // Prefer median9 (rejects up to 4 consecutive outliers). Fall back
        // through 7/5/3 during startup so smaller windows still get partial
        // protection. Anomaly-skip below catches anything wider than this.
        float ecMed;
        if (ecRawHistCount >= 9) {
          ecMed = median9f(ecRawHist[0], ecRawHist[1], ecRawHist[2], ecRawHist[3],
                           ecRawHist[4], ecRawHist[5], ecRawHist[6], ecRawHist[7], ecRawHist[8]);
        } else if (ecRawHistCount >= 7) {
          ecMed = median7f(ecRawHist[0], ecRawHist[1], ecRawHist[2], ecRawHist[3],
                           ecRawHist[4], ecRawHist[5], ecRawHist[6]);
        } else if (ecRawHistCount >= 5) {
          ecMed = median5f(ecRawHist[0], ecRawHist[1], ecRawHist[2], ecRawHist[3], ecRawHist[4]);
        } else if (ecRawHistCount >= 3) {
          ecMed = median3f(ecRawHist[0], ecRawHist[1], ecRawHist[2]);
        } else {
          ecMed = ecNow;
        }
        if (!ecFilterInit) { ecFiltered = ecMed; ecFilterInit = true; ecSpikeGuardCount = 0; } 
        else {
          // Single fixed-alpha EMA on the median-filtered signal. The rate
          // limiter and adaptive regime switching were removed: they turned
          // the titration inflection into a staircase (a constant-rate
          // clamped ramp, then a catch-up, plus an abrupt alpha/step switch
          // mid-inflection) instead of two clean straight slopes meeting.
          // median9f() above rejects up to 4 consecutive raw spikes and is
          // edge-preserving, so each linear segment stays straight and the
          // corner is just gently rounded - no step, no kink.
          ecBumpConfirmCounter = 0;
          ecSpikeGuardCount = 0;
          ecAdaptiveHoldCycles = 0;
          {
            // Anomaly-skip: large sudden deviation -> freeze ecFiltered for
            // up to SENSOR_EC_ANOMALY_SKIP_MAX samples. If it persists past
            // the cap (real fast change), accept it. The threshold (20% or
            // 300 uS) is FAR above per-sample delta at the titration
            // inflection, so EP is never frozen. Catches the wide physical
            // transients that median9 cannot fully reject.
            float ecDev = fabsf(ecMed - ecFiltered);
            float anomalyThr = fabsf(ecFiltered) * SENSOR_EC_ANOMALY_FRAC;
            if (anomalyThr < SENSOR_EC_ANOMALY_MIN_DELTA_US) anomalyThr = SENSOR_EC_ANOMALY_MIN_DELTA_US;

            if (ecDev > anomalyThr) {
              ecAnomalySkips++;
              if (ecAnomalySkips >= SENSOR_EC_ANOMALY_SKIP_MAX) {
                // Persisted too long -> treat as real, snap and resume.
                ecFiltered = ecMed;
                ecAnomalySkips = 0;
              }
              // Otherwise: do not update ecFiltered (freeze).
            } else {
              ecAnomalySkips = 0;
              ecFiltered += SENSOR_EC_EMA_ALPHA * (ecMed - ecFiltered);
            }
          }
        }
        currentEC = ecFiltered; st_ec = true;
      }
    }
    if (now - lastLCD >= lcdInterval) { lastLCD = now; String statStr = syncMode ? "- SYNC -" : (currentState == 0) ? "- IDLE -" : (currentState == 1) ? "- MSAT START -" : (currentState == 2) ? "- RUNNING -" : (currentState == 3) ? "- RINSING -" : "- REC ONLY -"; printCentered(statStr, 0); 
        String line2 = ""; char buf[32]; 
        switch(lcdPage) { case 0: line2 = "pH: " + String(currentPH, 2); break; case 1: line2 = "EC: " + String((int)currentEC) + " uS/cm"; break; case 2: line2 = "Temp: " + String(currentTemp, 1) + " C"; break; case 3: sprintf(buf, "R%04dG%04dB%04d", currentR, currentG, currentB); line2 = String(buf); break; case 4: line2 = "W: " + String(currentNetWeight, 2) + " g"; break; case 5: line2 = "PT: " + String(currentVolt, 3) + " V"; break; case 6: if (currentState == 2) { if (runAutoStopMode == 1) { sprintf(buf, "WL=%.2fg/%.0fg", weightLoss, runAutoStopWeightTarget); } else { sprintf(buf, "WL=%.2fg/--", weightLoss); } line2 = String(buf); } else { lcdPage = 0; line2 = "pH: " + String(currentPH, 2); } break; }
        printCentered(line2, 1);
    }
    
    // Update countdown every 1 second in PENDING state (independent of log interval)
    if (currentState == 1) {
      if (now - lastCountdownUpdate >= 1000) {
        lastCountdownUpdate = now;
        if (pendingStartTime == 0) pendingStartTime = now;
        unsigned long el = (now >= pendingStartTime) ? (now - pendingStartTime) : 0;
        long remainingSec = (long)PENDING_DURATION_SEC - (long)(el / 1000UL);
        countdownTime = (remainingSec > 0) ? (int)remainingSec : 0;
      }
    } else {
      lastCountdownUpdate = 0;  // Reset when not in PENDING state
    }
    
    if (currentState == 4) {
      if (now - lastLog >= logInterval) { 
        lastLog = now; 
        
        float netWeightForRec = currentNetWeightRaw;
        if (!hasLastValidNetWeight) { hasLastValidNetWeight = true; lastValidNetWeight = currentNetWeightRaw; } 
        else {
          float dtSec = (float)logInterval / 1000.0f; if (dtSec < 0.05f) dtSec = 0.05f;
          float allowedStep = AUTO_STOP_MAX_WEIGHT_STEP_G_PER_SEC * dtSec; if (allowedStep < AUTO_STOP_MIN_WEIGHT_STEP_G) allowedStep = AUTO_STOP_MIN_WEIGHT_STEP_G;
          allowedStep *= 2.5f; // REC/RUN can have occasional larger transients; avoid long flat-lining
          float stepAbs = fabsf(currentNetWeightRaw - lastValidNetWeight);
          if (stepAbs > allowedStep) {
            // Soft-clamp slew-limit: step continuously instead of hard-holding
            float step = currentNetWeightRaw - lastValidNetWeight;
            if (step > allowedStep) step = allowedStep;
            if (step < -allowedStep) step = -allowedStep;
            lastValidNetWeight += step;
            netWeightForRec = lastValidNetWeight;
            weightRejectCount++;
            if (weightRejectCount >= 4) {
              // Only re-anchor fully after sustained rejection
              lastValidNetWeight = currentNetWeightRaw;
              netWeightForRec = lastValidNetWeight;
              weightRejectCount = 0;
            }
          } else {
            lastValidNetWeight = currentNetWeightRaw;
            weightRejectCount = 0;
          }
        }

        float rawWeightLossRec = recStartWeight - netWeightForRec; if (rawWeightLossRec < 0.0f) rawWeightLossRec = 0.0f;
        float dtGuideSecRec = (float)logInterval / 1000.0f; if (dtGuideSecRec < 0.05f) dtGuideSecRec = 0.05f;

        if (!weightLossGuideInit) { weightLossGuide = rawWeightLossRec; weightLossGuideSlope = 0.0f; weightLossGuideInit = true; }
        // Snap guide down immediately if raw WL dropped far below guide (weight bounce-back after spike)
        if (rawWeightLossRec < weightLossGuide - AUTO_STOP_GUIDE_MAX_DEVIATION_G) {
          weightLossGuide = rawWeightLossRec;
          weightLossGuideSlope = 0.0f;
        }
        float observedSlopeRec = (rawWeightLossRec - weightLossGuide) / dtGuideSecRec;
        if (observedSlopeRec < 0.0f) observedSlopeRec = 0.0f; if (observedSlopeRec > AUTO_STOP_GUIDE_MAX_SLOPE_G_PER_SEC) observedSlopeRec = AUTO_STOP_GUIDE_MAX_SLOPE_G_PER_SEC;
        weightLossGuideSlope = (1.0f - AUTO_STOP_GUIDE_SLOPE_ALPHA) * weightLossGuideSlope + (AUTO_STOP_GUIDE_SLOPE_ALPHA * observedSlopeRec);
        float guidedLossRec = weightLossGuide + (weightLossGuideSlope * dtGuideSecRec);
        float guideWindowRec = AUTO_STOP_GUIDE_MAX_DEVIATION_G;
        float maxGuideWindowRec = AUTO_STOP_MAX_WEIGHT_STEP_G_PER_SEC * dtGuideSecRec; if (maxGuideWindowRec < AUTO_STOP_MIN_WEIGHT_STEP_G) maxGuideWindowRec = AUTO_STOP_MIN_WEIGHT_STEP_G;
        if (guideWindowRec > maxGuideWindowRec) guideWindowRec = maxGuideWindowRec;

        float boundedLossRec = rawWeightLossRec; float lowerGuideRec = guidedLossRec - guideWindowRec; float upperGuideRec = guidedLossRec + guideWindowRec;
        if (boundedLossRec < lowerGuideRec) boundedLossRec = lowerGuideRec;
        if (boundedLossRec > upperGuideRec) boundedLossRec = upperGuideRec;
        if (boundedLossRec < 0.0f) boundedLossRec = 0.0f;
        weightLossGuide = boundedLossRec; weightLoss = boundedLossRec;

        bool shouldStop = false;
        if (runAutoStopMode == 2) {
          unsigned long elapsedMs = (lastLog > recordingStartTime) ? (lastLog - recordingStartTime) : 0;
          unsigned long elapsed = elapsedMs / 1000;
          if (elapsed >= runAutoStopTimeTarget) { shouldStop = true; Serial.println(">> AUTO STOP: REC Time Reached"); }
        }

        if (shouldStop) { stopToIdle("AUTO_REC_TIME"); } 
        else {
          if (!isLogging) { 
            unsigned long retryNow = millis();
            if (!logFileCreatePending) { logFileCreatePending = true; lastLogCreateAttemptMs = 0; }
            if (lastLogCreateAttemptMs == 0 || (retryNow - lastLogCreateAttemptMs >= STRICT_LOG_FILE_RETRY_MS)) {
              lastLogCreateAttemptMs = retryNow; tryCreateLogFile("REC", true);
            }
          } else { logDataToSD(true); }
        }
      }
    } 
    else if (now - lastLog >= logInterval) {
      lastLog = now;
      if (currentState == 1) {
        // Accumulate samples during PENDING (countdown is updated separately every 1 second)
        w0_sum += currentNetWeightRaw; w0_sum_sq += (currentNetWeightRaw * currentNetWeightRaw); ph0_sum += currentPH; temp0_sum += currentTemp; ec0_sum += currentEC; r0_sum += currentR; g0_sum += currentG; b0_sum += currentB; w0_count++;
        
        // Check if PENDING duration has elapsed
        if (pendingStartTime == 0) pendingStartTime = now;
        unsigned long el = (now >= pendingStartTime) ? (now - pendingStartTime) : 0;
        if (el >= (PENDING_DURATION_SEC * 1000UL)) {
          if (w0_count > 0) { W0 = w0_sum/w0_count; float v = (w0_sum_sq - (w0_sum*w0_sum)/w0_count)/w0_count; if(v<0) v=0; W0_SD = sqrt(v); PH0 = ph0_sum / w0_count; Temp0 = temp0_sum / w0_count; EC0 = ec0_sum / w0_count; Red0 = r0_sum / w0_count; Green0 = g0_sum / w0_count; Blue0 = b0_sum / w0_count; }
          else { W0 = currentNetWeightRaw; W0_SD = 0; PH0 = currentPH; Temp0 = currentTemp; EC0 = currentEC; }

          resetAutoStopFilter(); hasLastValidNetWeight = false; weightRejectCount = 0; currentState = 2; lcdPage = 6; recordingStartTime = millis(); armRunLogClock(LOG_RUNNING_MS); digitalWrite(RELAY_PIN, LOW);
          Serial.printf(">> PENDING COMPLETE: elapsed=%lums, samples=%d\n", el, w0_count);
          if (!tryCreateLogFile("START", false)) { logFileCreatePending = true; lastLogCreateAttemptMs = millis(); }
        }
      } else if (currentState == 2) {
          float netWeightForAutoStop = currentNetWeightRaw;
          if (!hasLastValidNetWeight) { hasLastValidNetWeight = true; lastValidNetWeight = currentNetWeightRaw; } 
          else {
            float dtSec = (float)logInterval / 1000.0f; if (dtSec < 0.05f) dtSec = 0.05f;
            float allowedStep = AUTO_STOP_MAX_WEIGHT_STEP_G_PER_SEC * dtSec; if (allowedStep < AUTO_STOP_MIN_WEIGHT_STEP_G) allowedStep = AUTO_STOP_MIN_WEIGHT_STEP_G;
            allowedStep *= 2.5f; // Allow realistic flow-induced movement; only reject extreme spikes
            float stepAbs = fabsf(currentNetWeightRaw - lastValidNetWeight);
            if (stepAbs > allowedStep) {
              // Soft-clamp slew-limit: step continuously instead of hard-holding
              float step = currentNetWeightRaw - lastValidNetWeight;
              if (step > allowedStep) step = allowedStep;
              if (step < -allowedStep) step = -allowedStep;
              lastValidNetWeight += step;
              netWeightForAutoStop = lastValidNetWeight;
              weightRejectCount++;
              if (weightRejectCount >= 4) {
                // Only re-anchor fully after sustained rejection
                lastValidNetWeight = currentNetWeightRaw;
                netWeightForAutoStop = lastValidNetWeight;
                weightRejectCount = 0;
              }
            } else {
              lastValidNetWeight = currentNetWeightRaw;
              weightRejectCount = 0;
            }
          }
          float rawWeightLoss = W0 - netWeightForAutoStop;
          float dtGuideSec = (float)logInterval / 1000.0f; if (dtGuideSec < 0.05f) dtGuideSec = 0.05f;

          if (!weightLossGuideInit) { weightLossGuide = rawWeightLoss; weightLossGuideSlope = 0.0f; weightLossGuideInit = true; }
          // Snap guide down immediately if raw WL dropped far below guide (weight bounce-back after spike)
          if (rawWeightLoss < weightLossGuide - AUTO_STOP_GUIDE_MAX_DEVIATION_G) {
            weightLossGuide = rawWeightLoss;
            weightLossGuideSlope = 0.0f;
          }
          float observedSlope = (rawWeightLoss - weightLossGuide) / dtGuideSec; if (observedSlope < 0.0f) observedSlope = 0.0f; if (observedSlope > AUTO_STOP_GUIDE_MAX_SLOPE_G_PER_SEC) observedSlope = AUTO_STOP_GUIDE_MAX_SLOPE_G_PER_SEC;
          weightLossGuideSlope = (1.0f - AUTO_STOP_GUIDE_SLOPE_ALPHA) * weightLossGuideSlope + (AUTO_STOP_GUIDE_SLOPE_ALPHA * observedSlope);

          float guidedLoss = weightLossGuide + (weightLossGuideSlope * dtGuideSec); float guideWindow = AUTO_STOP_GUIDE_MAX_DEVIATION_G;
          float maxGuideWindow = AUTO_STOP_MAX_WEIGHT_STEP_G_PER_SEC * dtGuideSec; if (maxGuideWindow < AUTO_STOP_MIN_WEIGHT_STEP_G) maxGuideWindow = AUTO_STOP_MIN_WEIGHT_STEP_G;
          if (guideWindow > maxGuideWindow) guideWindow = maxGuideWindow;

          float boundedLoss = rawWeightLoss; float lowerGuide = guidedLoss - guideWindow; float upperGuide = guidedLoss + guideWindow;
          if (boundedLoss < lowerGuide) boundedLoss = lowerGuide;
          if (boundedLoss > upperGuide) boundedLoss = upperGuide;
          if (boundedLoss < 0.0f) boundedLoss = 0.0f;
          weightLossGuide = boundedLoss; weightLoss = boundedLoss;

          weightLossSamples[weightLossSampleIndex] = weightLoss; weightLossSampleIndex = (weightLossSampleIndex + 1) % AUTO_STOP_AVG_SAMPLES;
          if (weightLossSampleCount < AUTO_STOP_AVG_SAMPLES) { weightLossSampleCount++; }

          float sumLoss = 0.0; for (int i = 0; i < weightLossSampleCount; i++) { sumLoss += weightLossSamples[i]; }
          weightLossAvg = (weightLossSampleCount > 0) ? (sumLoss / weightLossSampleCount) : 0.0;

          bool shouldStop = false;
          if (runAutoStopMode == 1) {
            if (weightLossAvg >= runAutoStopWeightTarget) { autoStopHitCount++; } else { autoStopHitCount = 0; }
            if (autoStopHitCount >= AUTO_STOP_HOLD_COUNT) { shouldStop = true; Serial.println(">> AUTO STOP: Weight Loss Reached (filtered)"); }
          } else if (runAutoStopMode == 2) {
            unsigned long elapsed = (now - recordingStartTime) / 1000;
            if (elapsed >= runAutoStopTimeTarget) { shouldStop = true; Serial.println(">> AUTO STOP: Time Reached"); }
          }

          if (shouldStop) {
            if (runAutoStopMode == 1) stopToIdle("AUTO_WEIGHT"); else stopToIdle("AUTO_TIME");
          } else {
            if (!isLogging) { 
              unsigned long retryNow = millis();
              if (!logFileCreatePending) { logFileCreatePending = true; lastLogCreateAttemptMs = 0; }
              if (lastLogCreateAttemptMs == 0 || (retryNow - lastLogCreateAttemptMs >= STRICT_LOG_FILE_RETRY_MS)) {
                lastLogCreateAttemptMs = retryNow; tryCreateLogFile("START", false);
              }
            } else { logDataToSD(true); }
          }
      } else { hasLastValidNetWeight = false; countdownTime = 0; weightLoss = 0; }
    }
    
    if (now - lastStatus >= STATUS_CHECK_INTERVAL) { 
      lastStatus = now; st_ads = checkI2C(0x48); st_rgb = checkI2C(0x29); st_rtc = checkI2C(0x68); 
      st_scale = (now - lastScaleSuccess < 2000); st_temp = (sensors.getDeviceCount() > 0);
      if (!st_sd) ensureSDReady();
    }

    // File list refresh is handled in TaskSDWriterCode to avoid blocking the sensor task
    // with a full SD directory scan during or immediately after a run.

    if (wdtRegistered) {
      esp_task_wdt_reset();
    }
    vTaskDelay(10); 
  }
}

// ===== HELPER FUNCTIONS (Restored) =====
void addSystemLog(const String &message) {
  if (!st_sd) return;
  String line = getFormattedTimeMS() + " | " + message;
  appendLogLine(line);
}

bool appendLogLine(const String &line) {
  if (!ensureSDReady()) return false;
  if (!tryLockSD(SD_LOCK_TIMEOUT_MS)) return false;
  if (SD.exists(SYSTEM_LOG_PATH)) {
    File sizeCheck = SD.open(SYSTEM_LOG_PATH, FILE_READ);
    if (sizeCheck) {
      if (sizeCheck.size() > SYSTEM_LOG_MAX_BYTES) {
        sizeCheck.close(); SD.remove(SYSTEM_LOG_PATH);
      } else { sizeCheck.close(); }
    }
  }
  File outFile = SD.open(SYSTEM_LOG_PATH, FILE_APPEND);
  if (!outFile) { st_sd = false; unlockSD(); return false; }
  outFile.println(line); outFile.close(); unlockSD();
  return true;
}

bool enqueueSDLine(const String &line, size_t &queueSizeAfterPush) {
  queueSizeAfterPush = 0;
  if (!sdQueueMutex) return false;

  xSemaphoreTake(sdQueueMutex, portMAX_DELAY);
  if (sdWriteQueue.size() >= SD_WRITE_QUEUE_MAX) {
    xSemaphoreGive(sdQueueMutex);
    return false;
  }

  SDQueueItem item = {};
  line.toCharArray(item.data, SD_QUEUE_LINE_MAX);
  sdWriteQueue.push_back(item);
  queueSizeAfterPush = sdWriteQueue.size();
  xSemaphoreGive(sdQueueMutex);
  return true;
}

bool metricLog(const char *tag, const char *details, unsigned long &lastLogMs) {
  if (!ENABLE_METRIC_LOG) return false;
  unsigned long now = millis();
  if (now - lastLogMs < METRIC_LOG_COOLDOWN_MS) return false;
  if (!st_sd) return false;
  if (!ensureSDReady()) return false;
  if (!tryLockSD(SD_LOCK_TIMEOUT_MS)) return false;
  if (!SD.exists("/autotttoutput")) { unlockSD(); return false; }
  File f = SD.open(METRIC_LOG_PATH, FILE_APPEND);
  if (!f) { st_sd = false; unlockSD(); return false; }
  f.print(getFormattedTimeMS()); f.print(" | "); f.print(tag); f.print(" "); f.println(details);
  f.close(); unlockSD(); lastLogMs = now;
  return true;
}

size_t getSDQueueSizeSnapshot() {
  size_t queueSize = 0;
  if (!sdQueueMutex) { return sdWriteQueue.size(); }
  xSemaphoreTake(sdQueueMutex, portMAX_DELAY);
  queueSize = sdWriteQueue.size();
  xSemaphoreGive(sdQueueMutex);
  return queueSize;
}

bool flushSDQueueBeforeStop(unsigned long timeoutMs) {
  unsigned long startMs = millis();
  while (millis() - startMs < timeoutMs) {
    if (getSDQueueSizeSnapshot() == 0) return true;
    // Use vTaskDelay (not delay) to explicitly yield to TaskSDWriterCode on Core 0
    // so it can drain the queue while we wait
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return (getSDQueueSizeSnapshot() == 0);
}

bool ensureSDReady() {
  if (st_sd) return true;
  unsigned long now = millis();
  if (now - lastSdRetryMs < SD_RETRY_MS) return false;
  lastSdRetryMs = now;
  
  if (!sdCardMutex) return false;
  if (!tryLockSD(SD_LOCK_TIMEOUT_MS)) return false;
  
  SD.end(); // Deinitialize SD before reinit to reset chipset state
  delay(50);
  
  // Reconnect using standard SPI mode
  bool ok = SD.begin(SD_CS_PIN, SPI, SD_SPI_FREQ);
  
  if (ok) {
    if (!SD.exists("/autotttoutput")) {
      SD.mkdir("/autotttoutput");
    }
    logSDInfo();
    st_sd = true;
    Serial.println(">> SD RECOVERED SUCCESSFULLY");
  } else {
    st_sd = false;
    Serial.println(">> SD STILL FAIL");
  }
  
  unlockSD();
  return ok;
}

void logSDInfo() {
  uint8_t cardType = SD.cardType();
  const char *typeStr = "UNKNOWN";
  if (cardType == CARD_MMC) typeStr = "MMC";
  else if (cardType == CARD_SD) typeStr = "SDSC";
  else if (cardType == CARD_SDHC) typeStr = "SDHC";
  uint64_t sizeMB = SD.cardSize() / (1024 * 1024);
  Serial.println(String(">> SD TYPE: ") + typeStr + " | SIZE: " + sizeMB + "MB");
}

void setTimeZone() {
  setenv("TZ", TZ_INFO, 1);
  tzset();
}

bool syncTimeFromNtp() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Sync Time: [SKIP] WiFi disconnected");
    return false;
  }
  IPAddress ntpIp;
  bool reachable = WiFi.hostByName(ntpServer, ntpIp);
  Serial.print("Connect to Time Sync Server: ");
  Serial.println(reachable ? "[CONNECTED]" : "[FAIL]");
  if (!reachable) { Serial.println("Sync Time: [SKIP]"); return false; }

  configTzTime(TZ_INFO, ntpServer, "time.google.com", "time.cloudflare.com");
  struct tm timeinfo;
  bool ok = false;
  for (int i = 0; i < 3; i++) {
    if (getLocalTime(&timeinfo, 2500)) {
      if ((timeinfo.tm_year + 1900) >= 2024) { ok = true; break; }
    }
    delay(150);
  }
  Serial.print("Sync Time: ");
  Serial.println(ok ? "[PASS]" : "[FAIL]");
  if (ok && st_rtc) {
    rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
  }
  return ok;
}

void resetAutoStopFilter() {
  weightLossAvg = 0.0; weightLossSampleCount = 0; weightLossSampleIndex = 0; autoStopHitCount = 0;
  weightLossGuide = 0.0f; weightLossGuideSlope = 0.0f; weightLossGuideInit = false;
  for (int i = 0; i < AUTO_STOP_AVG_SAMPLES; i++) { weightLossSamples[i] = 0.0; }
}

bool tryLockSD(uint32_t timeoutMs) {
  if (!sdCardMutex) return false;
  return xSemaphoreTake(sdCardMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void unlockSD() { if (sdCardMutex) xSemaphoreGive(sdCardMutex); }

void updateFileListCache() {
  // Stub: File list cache removed - now handled by msatsync.sh script
}

bool tryCreateLogFile(const char *context, bool updateFileList) {
  isLogging = false;
  if (!ensureSDReady()) { Serial.println(String(">> ERROR: SD not ready for ") + context + "!"); addSystemLog(String("ERROR: SD not ready for ") + context); return false; }
  if (!tryLockSD(SD_LOCK_TIMEOUT_MS)) { Serial.println(String(">> WARN: SD busy - will retry ") + context); return false; }
  if (!SD.exists("/autotttoutput")) { unlockSD(); Serial.println(String(">> ERROR: /autotttoutput missing for ") + context + "!"); addSystemLog(String("ERROR: /autotttoutput missing for ") + context); return false; }
  currentLogFileName = createFileName();
  File f = SD.open(currentLogFileName, FILE_WRITE);
  if (f) {
    // Add SampleIndex as the first column for data continuity verification
    f.println("SampleIndex,Timestamp,Temp,pH,Volt,EC,Weight,Weightloss,R,G,B,C,RelayStatus");
    size_t createdSize = f.size();
    f.close();

    int slash = currentLogFileName.lastIndexOf('/');
    String nameOnly = (slash >= 0) ? currentLogFileName.substring(slash + 1) : currentLogFileName;
    uint64_t createdMtime = 0;
    File check = SD.open(currentLogFileName, FILE_READ);
    if (check) {
      createdMtime = getFileModifiedTimeSafe(check);
      check.close();
    }
    // Index file update removed - now handled by msatsync.sh script

    unlockSD();
    isLogging = true; logFileCreatePending = false; Serial.println(String(">> ") + context + " File Created: " + currentLogFileName);
    return true;
  }
  unlockSD(); Serial.println(String(">> ERROR: Failed to create ") + context + " file!"); addSystemLog(String("ERROR: Failed to create ") + context + " file"); return false;
}

void disconnectClientByIP(const String &ip, bool logEvent) {
  if (ip.length() == 0) return;
  xSemaphoreTake(clientListMutex, portMAX_DELAY);
  connectedClients.erase( std::remove_if(connectedClients.begin(), connectedClients.end(), [ip](const ClientInfo &c) { return c.ipAddress == ip; }), connectedClients.end() );
  if (ip == controlClientIP) controlClientIP = ""; if (ip == adminClientIP) adminClientIP = "";
  xSemaphoreGive(clientListMutex);
  if (logEvent) { String logMsg = "DISCONNECT client " + ip; xSemaphoreTake(clientListMutex, portMAX_DELAY); xSemaphoreGive(clientListMutex); addSystemLog(logMsg); }
}

bool isAdminClient(const String &ip) { return (adminClientIP.length() > 0 && ip == adminClientIP); }
bool isControlClient(const String &ip) { return (controlClientIP.length() > 0 && ip == controlClientIP); }
bool isAuthorizedClient(const String &ip) { return isControlClient(ip) || isAdminClient(ip); }

String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length() + 16);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in.charAt(i);
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

String urlEncode(const String &in) {
  static const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(in.length() * 3);
  for (size_t i = 0; i < in.length(); i++) {
    uint8_t c = (uint8_t)in.charAt(i);
    bool safe = ((c >= 'A' && c <= 'Z') ||
                 (c >= 'a' && c <= 'z') ||
                 (c >= '0' && c <= '9') ||
                 c == '-' || c == '_' || c == '.' || c == '~');
    if (safe) {
      out += (char)c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

bool isTxtLikeDataFile(const String &baseName) {
  if (baseName.length() == 0) return false;
  if (baseName.equalsIgnoreCase("_index.txt")) return false;
  return baseName.endsWith(".txt") || baseName.endsWith(".TXT");
}

// Stricter check for /delete_batch: must look like a real data filename
// "YYYYMMDD_HHMMSS<rest>.txt" and contain no path separators or shell/
// filesystem metacharacters. This is a hard safety guard - the delete
// endpoint will reject any name that does not pass.
bool isValidDeleteFileName(const String &n) {
  if (n.length() < 19) return false; // 8 + '_' + 6 + ".txt" minimum
  for (int i = 0; i < 8; i++) if (!isdigit((unsigned char)n[i])) return false;
  if (n[8] != '_') return false;
  for (int i = 9; i < 15; i++) if (!isdigit((unsigned char)n[i])) return false;
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    if (c == '/' || c == '\\' || c == '<' || c == '>' || c == '"' ||
        c == ':' || c == '|' || c == '?' || c == '*' || c < 0x20) return false;
  }
  String low = n; low.toLowerCase();
  if (!low.endsWith(".txt")) return false;
  if (n.equalsIgnoreCase("_index.txt")) return false;
  return true;
}

bool buildFileListSlice(uint32_t cursor, uint32_t limit, String &out, uint32_t &totalCount, int32_t &nextCursor, uint32_t &sentCount) {
  totalCount = 0;
  sentCount = 0;
  nextCursor = -1;
  out = "";

  if (!ensureSDReady()) return false;
  if (!tryLockSD(SD_LOCK_TIMEOUT_MS)) return false;

  File dir = SD.open("/autotttoutput");
  if (!(dir && dir.isDirectory())) {
    if (dir) dir.close();
    unlockSD();
    return false;
  }

  out.reserve(8192);
  while (true) {
    File f = dir.openNextFile();
    if (!f) break;

    if (!f.isDirectory()) {
      String fullName = String(f.name());
      String baseName = fullName;
      int slashPos = baseName.lastIndexOf('/');
      if (slashPos >= 0) baseName = baseName.substring(slashPos + 1);

      if (isTxtLikeDataFile(baseName)) {
        if (totalCount >= cursor && (limit == 0 || sentCount < limit)) {
          out += "/autotttoutput/";
          out += baseName;
          out += "|";
          out += String((unsigned long)f.size());
          out += "\n";
          sentCount++;
        }
        totalCount++;
      }
    }

    f.close();
    if ((totalCount % 32) == 0) vTaskDelay(1);
  }

  dir.close();
  unlockSD();

  if (cursor + sentCount < totalCount) {
    nextCursor = (int32_t)(cursor + sentCount);
  }
  return true;
}

// ===== Read file list from pre-built _index.txt (fast, non-blocking) =====
bool readIndexFileSlice(uint32_t cursor, uint32_t limit, String &out, uint32_t &totalCount, int32_t &nextCursor, uint32_t &sentCount) {
  sentCount = 0;
  totalCount = 0;
  nextCursor = -1;
  out = "";

  if (manifestRebuildInProgress) return false;
  if (!tryLockSD(SD_LOCK_TIMEOUT_MS)) return false;

  File idx = SD.open(INDEX_FILE_PATH, FILE_READ);
  if (!idx || idx.size() == 0) {
    if (idx) idx.close();
    unlockSD();
    return false;
  }

  out.reserve(8192);
  char buf[512];
  String line;
  line.reserve(128);

  while (idx.available()) {
    int n = idx.readBytes(buf, sizeof(buf));
    for (int i = 0; i < n; i++) {
      if (buf[i] == '\n' || buf[i] == '\r') {
        if (line.length() > 0) {
          if (totalCount >= cursor && (limit == 0 || sentCount < limit)) {
            out += line;
            out += "\n";
            sentCount++;
          }
          totalCount++;
          line = "";
        }
        continue;
      }
      line += buf[i];
    }
  }
  if (line.length() > 0) {
    if (totalCount >= cursor && (limit == 0 || sentCount < limit)) {
      out += line;
      out += "\n";
      sentCount++;
    }
    totalCount++;
  }

  idx.close();
  unlockSD();

  if (cursor + sentCount < totalCount) {
    nextCursor = (int32_t)(cursor + sentCount);
  }
  return true;
}

uint32_t countIndexFileLines() {
  if (manifestRebuildInProgress) return 0;
  if (!tryLockSD(SD_LOCK_TIMEOUT_MS)) return 0;

  File idx = SD.open(INDEX_FILE_PATH, FILE_READ);
  if (!idx) { unlockSD(); return 0; }

  uint32_t count = 0;
  char buf[512];
  while (idx.available()) {
    int n = idx.readBytes(buf, sizeof(buf));
    for (int i = 0; i < n; i++) {
      if (buf[i] == '\n') count++;
    }
  }
  idx.close();
  unlockSD();
  return count;
}

// Append a single just-finalized data file to _index.txt so the file list
// stays current without a full rebuild (O(1), no SD directory walk). This is
// the freshness fix: stopping a recording now updates the index immediately
// instead of relying on a manual ?rebuild=1. Skipped while a full rebuild is
// running (that walk already includes the file). If the index does not exist
// yet, a full rebuild is requested instead of writing a partial index.
void appendFileToIndex(const String &fullPath) {
  if (fullPath.length() == 0) return;
  if (manifestRebuildInProgress) return;

  String baseName = fullPath;
  int slashPos = baseName.lastIndexOf('/');
  if (slashPos >= 0) baseName = baseName.substring(slashPos + 1);
  if (!isTxtLikeDataFile(baseName)) return;

  if (!tryLockSD(SD_LOCK_TIMEOUT_MS)) return;

  if (!SD.exists(INDEX_FILE_PATH)) {
    unlockSD();
    manifestRebuildRequested = true;
    return;
  }

  uint32_t fsize = 0;
  File df = SD.open(fullPath, FILE_READ);
  if (df) { fsize = (uint32_t)df.size(); df.close(); }

  File idx = SD.open(INDEX_FILE_PATH, FILE_APPEND);
  if (idx) {
    idx.print("/autotttoutput/");
    idx.print(baseName);
    idx.print("|");
    idx.println((unsigned long)fsize);
    idx.flush();
    idx.close();
  }
  unlockSD();
}

bool isValidCustomFilename(const String &name) {
  if (name.length() == 0) return true;
  if (name.length() > 30) return false;
  for (size_t i = 0; i < name.length(); i++) {
    char ch = name.charAt(i);
    bool ok = ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '_');
    if (!ok) return false;
  }
  return true;
}

uint64_t getEpochMsNow() {
  struct timeval tv; gettimeofday(&tv, NULL);
  return ((uint64_t)tv.tv_sec * 1000ULL) + (uint64_t)(tv.tv_usec / 1000ULL);
}

String formatEpochMs(uint64_t epochMs) {
  time_t sec = (time_t)(epochMs / 1000ULL); uint16_t ms = (uint16_t)(epochMs % 1000ULL);
  struct tm timeinfo; localtime_r(&sec, &timeinfo);
  char s[64]; strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S", &timeinfo);
  char msbuf[8]; snprintf(msbuf, sizeof(msbuf), ".%03u", (unsigned int)ms);
  return String(s) + String(msbuf);
}

void armRunLogClock(unsigned long stepMs) {
  runLogEpochBaseMs = getEpochMsNow(); runLogSampleIndex = 0; runLogStepMs = (stepMs > 0) ? stepMs : 1; runLogClockArmed = true;
}

void disarmRunLogClock() { runLogClockArmed = false; runLogSampleIndex = 0; runLogStepMs = LOG_RUNNING_MS; }

String nextRunLogTimestamp() {
  if (!runLogClockArmed) return getFormattedTimeMS();
  // Do not increment index here - let the caller handle it
  uint64_t ts = runLogEpochBaseMs + ((uint64_t)runLogSampleIndex * (uint64_t)runLogStepMs);
  return formatEpochMs(ts);
}

String getFormattedTimeMS() {
  struct timeval tv; gettimeofday(&tv, NULL); struct tm* timeinfo = localtime(&tv.tv_sec);
  char s[64]; strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S", timeinfo);
  char ms[8]; sprintf(ms, ".%03ld", tv.tv_usec / 1000); 
  return String(s) + String(ms);
}

String getFormattedTimeSec() {
  struct timeval tv; gettimeofday(&tv, NULL); struct tm* timeinfo = localtime(&tv.tv_sec);
  char s[64]; strftime(s, sizeof(s), "%Y-%m-%d %H:%M:%S", timeinfo);
  return String(s);
}

String createFileName() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){ return "/autotttoutput/log_unknown.txt"; }
  char s[64];
  if (customFilename.length() > 0) { strftime(s, 64, "/autotttoutput/%Y%m%d_%H%M%S_", &timeinfo); return String(s) + customFilename + ".txt"; } 
  else { strftime(s, 64, "/autotttoutput/%Y%m%d_%H%M%S.txt", &timeinfo); return String(s); }
}

String getDataCSV(const String &timestampText, int sampleIndex) {
  String line = "";
  // Add Sample Index as the first column (for data continuity verification)
  if (sampleIndex >= 0) {
    line += String(sampleIndex) + ",";
  }
  line += timestampText + ","; line += String(currentTemp, 1) + ","; line += String(currentPH, 2) + ","; line += String(currentVolt, 3) + ",";
  line += String(currentEC, 0) + ","; line += String(currentNetWeightRaw, 2) + ",";  // Use RAW weight (no display filter)
  if (currentState == 2) line += String(weightLoss, 2) + ","; else if (currentState == 4) line += String(weightLoss, 2) + ","; else line += "XX.XX,"; 
  line += String(currentR) + ","; line += String(currentG) + ","; line += String(currentB) + ","; line += String(currentC) + ",";
  String statStr = "OFF"; if(currentState == 2) statStr = "ON"; else if(currentState == 3) statStr = "RINSE"; else if(currentState == 4) statStr = "REC_ONLY";
  line += statStr; return line;
}

void printCentered(String text, int row) {
  int padding = (16 - text.length()) / 2; if (padding < 0) padding = 0;
  String line = ""; for(int i=0; i<padding; i++) line += " "; line += text;
  while(line.length() < 16) line += " "; lcd.setCursor(0, row); lcd.print(line);
}

float calibrateEC(float raw) {
  int sz = sizeof(cal_ec_raw) / sizeof(cal_ec_raw[0]); float val = interpolate(raw, cal_ec_raw, cal_ec_std, sz);
  return (val < 0) ? 0 : val;
}

void logCheck(String name, bool success) {
    Serial.print("Check: " + name + " -> "); Serial.println(success ? "PASS" : "FAIL");
    lcd.clear(); lcd.setCursor(0,0); lcd.print("Check: " + name); lcd.setCursor(0,1); lcd.print(success ? "[PASS]" : "[FAIL]"); delay(BOOT_CHECK_DELAY_MS);
}

float phFromVoltage(float volt) {
  if (volt < 0.0f) volt = 0.0f;
  int sz = sizeof(ph_voltages)/sizeof(ph_voltages[0]);
  float ph = interpolate(volt, ph_voltages, ph_values, sz);
  if (ph < 0.0f) ph = 0.0f;
  if (ph > 14.0f) ph = 14.0f;
  return ph;
}

float getSmoothedPH() {
    // Guard: if ADS1115 not responding, skip read to prevent I2C hang → WDT reboot loop
    if (!st_ads) return currentPH;
  // More ADC oversampling = lower pH noise with ZERO lag and no edge
  // distortion (averaging N white-noise samples cuts sigma ~sqrt(N)), so
  // the EP stays sharp - unlike heavier median/EMA. Original was only 5/9
  // (trimmed mean kept ~3/~5), too weak in the buffered <pH7 region.
  // (The earlier web outage was a WiFi router fault, NOT this change.)
  // Timeout must be >= one ADS1115 conversion (~8 ms @128 SPS) or
  // conversionComplete() can time out and return a stale read.
  const int numSamples = (currentState == 2) ? 12 : 12;
  const unsigned long sampleTimeoutMs = (currentState == 2) ? 15 : 18;
  float samples[15];
    for (int i=0; i<numSamples; i++) {
        // Use startADCReading + timeout instead of readADC_SingleEnded to avoid
        // infinite while(!conversionComplete()) if I2C bus is stuck (SDA locked LOW)
        ads.startADCReading(ADS1X15_REG_CONFIG_MUX_SINGLE_0, /*continuous=*/false);
        unsigned long t0 = millis();
        while (!ads.conversionComplete()) {
      if (millis() - t0 > sampleTimeoutMs) break;
            delay(1);
        }
        samples[i] = ads.getLastConversionResults() * 0.0001875f;
        delay(1);
    }
    for (int i=0; i<numSamples-1; i++) { for (int j=0; j<numSamples-i-1; j++) { if (samples[j] > samples[j+1]) { float temp = samples[j]; samples[j] = samples[j+1]; samples[j+1] = temp; } } }
  int clipCount = (int)(numSamples * 0.2f);
  if (clipCount < 1) clipCount = 1;
  if ((clipCount * 2) >= numSamples) clipCount = (numSamples / 2) - 1;
  if (clipCount < 0) clipCount = 0;
  float sum = 0; int count = 0;
    for (int i = clipCount; i < (numSamples - clipCount); i++) { sum += samples[i]; count++; }
    float avgVolt = (count > 0) ? (sum / count) : 0.0; if(avgVolt < 0) avgVolt = 0; currentVolt = avgVolt; 
    return phFromVoltage(avgVolt);
}

float updateDisplayWeightFilter(float rawWeight, float dtSec) {
  if (dtSec < 0.05f) dtSec = 0.05f;
  if (!displayWeightFilterInit) { displayWeightGuide = rawWeight; displayWeightGuideSlope = 0.0f; displayWeightFilterInit = true; return rawWeight; }
  float maxSlope = (currentState == 2) ? DISPLAY_WEIGHT_RUN_MAX_SLOPE_G_PER_SEC : DISPLAY_WEIGHT_MAX_SLOPE_G_PER_SEC;
  float maxDeviation = (currentState == 2) ? DISPLAY_WEIGHT_RUN_MAX_DEVIATION_G : DISPLAY_WEIGHT_MAX_DEVIATION_G;
  float observedSlope = (rawWeight - displayWeightGuide) / dtSec;
  observedSlope = clampFloat(observedSlope, -maxSlope, maxSlope);
  displayWeightGuideSlope = (1.0f - DISPLAY_WEIGHT_GUIDE_ALPHA) * displayWeightGuideSlope + (DISPLAY_WEIGHT_GUIDE_ALPHA * observedSlope);
  float guidedWeight = displayWeightGuide + (displayWeightGuideSlope * dtSec);
  float guideWindow = maxDeviation;
  float maxGuideWindow = maxSlope * dtSec;
  if (maxGuideWindow < AUTO_STOP_MIN_WEIGHT_STEP_G) maxGuideWindow = AUTO_STOP_MIN_WEIGHT_STEP_G;
  if (guideWindow > maxGuideWindow) guideWindow = maxGuideWindow;
  float boundedWeight = clampFloat(rawWeight, guidedWeight - guideWindow, guidedWeight + guideWindow);
  displayWeightGuide = boundedWeight; return boundedWeight;
}

String getRecTimeStr() {
    if (currentState != 2 && currentState != 4) return "";
    unsigned long elapsed = (millis() - recordingStartTime) / 1000;
    unsigned long hours = elapsed / 3600; unsigned long minutes = (elapsed % 3600) / 60; unsigned long seconds = elapsed % 60;
    char buf[16]; sprintf(buf, "%02lu:%02lu:%02lu", hours, minutes, seconds); return String(buf);
}

void setStopReason(const String &reason) { lastStopReason = reason; Serial.println(">> STOP REASON: " + reason); }

void stopToIdle(const String &reason, bool setRelayHigh) {
  int prevState = currentState; bool wasLogging = isLogging && (prevState == 2 || prevState == 4);
  if (wasLogging) {
    stopFlushInProgress = true; bool flushed = flushSDQueueBeforeStop(STOP_FLUSH_TIMEOUT_MS);
    if (!flushed) {
      size_t remain = 0; xSemaphoreTake(sdQueueMutex, portMAX_DELAY); remain = sdWriteQueue.size();
      while (!sdWriteQueue.empty()) sdWriteQueue.pop_front(); xSemaphoreGive(sdQueueMutex);
      addSystemLog(String("WARN: STOP flush timeout, dropped ") + remain + " queued lines");
    }
    stopFlushInProgress = false;
  }
  isLogging = false; logFileCreatePending = false; disarmRunLogClock(); resetAutoStopFilter();
  if (setRelayHigh) digitalWrite(RELAY_PIN, HIGH);
  currentState = 0; lcdPage = 0; startLimiterUntilMs = 0; pendingStartTime = 0;  // Reset pending timer
  if ((prevState == 2 || prevState == 4) && currentLogFileName.length() > 0) { lastCompletedRecFileName = currentLogFileName; lastCompletedStopReason = reason; appendFileToIndex(currentLogFileName); }
  addSystemLog("STOP -> " + reason); setStopReason(reason);
  
  // Trigger file list and file size refresh on SD immediately after operation stops
  fileListRefreshRequested = true;
  lastFileListUpdateMs = 0; 
}

String getCommandSourceTag() { if (lastCommandSource == 0) return "WEB"; if (lastCommandSource == 1) return "BUTTON"; return "UNKNOWN"; }

void applyRelayState() {
  int desired = (currentState == 2 || currentState == 3) ? LOW : HIGH; int actual = digitalRead(RELAY_PIN);
  if (actual != lastRelayPinState) { Serial.println(String(">> RELAY WARN: External change detected -> ") + (actual == LOW ? "ON" : "OFF")); lastRelayPinState = actual; }
  if (actual != desired) {
    if (currentState == 2 && desired == LOW && actual == HIGH) { Serial.println(">> RELAY LOCK: Prevented OFF during RUNNING"); digitalWrite(RELAY_PIN, desired); lastRelayPinState = desired; } 
    else { Serial.println(String(">> RELAY CORRECT: Force -> ") + (desired == LOW ? "ON" : "OFF")); digitalWrite(RELAY_PIN, desired); lastRelayPinState = desired; }
  }
}

void toggleSystem() { // MSAT START
  if (currentState == 3 || currentState == 4) { return; } 
  if (currentState == 0) {
    digitalWrite(RELAY_PIN, HIGH); pendingStartTime = millis(); startLimiterUntilMs = pendingStartTime + 5000;
    addSystemLog("START requested (pending)"); resetAutoStopFilter(); runAutoStopMode = autoStopMode; runAutoStopWeightTarget = autoStopWeightTarget; runAutoStopTimeTarget = autoStopTimeTarget;
    logFileCreatePending = false; w0_sum = 0; w0_sum_sq = 0; w0_count = 0; ph0_sum = 0; temp0_sum = 0; ec0_sum = 0; r0_sum = 0; g0_sum = 0; b0_sum = 0;
    W0 = 0; W0_SD = 0; PH0 = 0; Temp0 = 0; EC0 = 0; Red0=0; Green0=0; Blue0=0;
    currentState = 1; Serial.println(">> STARTED -> PENDING"); lastStopReason = "NONE";
  } else {
    if (currentState == 1) { stopToIdle("CANCEL_PENDING_" + getCommandSourceTag()); Serial.println(">> CANCELED -> IDLE"); } 
    else { stopToIdle("USER_STOP_" + getCommandSourceTag()); Serial.println(">> STOPPED -> IDLE"); }
  }
}

void toggleRecord() { // REC ONLY
  if (currentState == 1 || currentState == 2 || currentState == 3) { return; } 
  if (currentState == 0) {
    currentState = 4; digitalWrite(RELAY_PIN, HIGH); recordingStartTime = millis(); recStartWeight = currentNetWeightRaw; weightLoss = 0.0f;
    resetAutoStopFilter(); armRunLogClock(LOG_REC_MS); addSystemLog("REC start (no pump)");
    runAutoStopMode = (autoStopMode == 1) ? 0 : autoStopMode; runAutoStopWeightTarget = autoStopWeightTarget; runAutoStopTimeTarget = autoStopTimeTarget;
    if (!tryCreateLogFile("REC", true)) { logFileCreatePending = true; lastLogCreateAttemptMs = millis(); }
    Serial.println(">> REC START (NO PUMP)");
  } else if (currentState == 4) { stopToIdle("REC_STOP_" + getCommandSourceTag()); Serial.println(">> REC STOP"); }
}

void toggleRinse() {
  if (currentState == 1 || currentState == 2 || currentState == 4) return;
  if (currentState == 0) {
    currentState = 3; digitalWrite(RELAY_PIN, LOW); isLogging = false; Serial.println(">> RINSE START"); String src = getCommandSourceTag();
    if (src == "WEB" && lastWebCommandIP.length() > 0) { addSystemLog("RINSE start (WEB " + lastWebCommandIP + ")"); } else { addSystemLog("RINSE start (" + src + ")"); }
  } else if (currentState == 3) { stopToIdle("RINSE_STOP_" + getCommandSourceTag()); Serial.println(">> RINSE STOP"); }
}

size_t readLogLines(size_t maxLines, std::vector<String> &linesOut) {
  linesOut.clear();
  if (!ensureSDReady()) return 0;
  if (!tryLockSD(SD_LOCK_TIMEOUT_MS)) return 0;
  File file = SD.open(SYSTEM_LOG_PATH, FILE_READ);
  if (!file) { st_sd = false; unlockSD(); return 0; }
  String line;
  while (file.available()) {
    char c = file.read();
    if (c == '\n') {
      line.trim();
      if (line.length() > 0) { linesOut.push_back(line); if (linesOut.size() > maxLines) linesOut.erase(linesOut.begin()); }
      line = "";
    } else if (c != '\r') { line += c; }
  }
  if (line.length() > 0) {
    line.trim();
    if (line.length() > 0) { linesOut.push_back(line); if (linesOut.size() > maxLines) linesOut.erase(linesOut.begin()); }
  }
  file.close(); unlockSD(); return linesOut.size();
}

bool checkI2C(uint8_t addr) { Wire.beginTransmission(addr); return (Wire.endTransmission() == 0); }

bool checkPCWebServer() {
  IPAddress pcIP; if (!WiFi.hostByName(PC_WEB_SERVER_IP, pcIP)) { return false; }
  WiFiClient client; if (!client.connect(PC_WEB_SERVER_IP, PC_WEB_SERVER_PORT, 2000)) { return false; }
  client.println("HEAD / HTTP/1.1"); client.print("Host: "); client.println(PC_WEB_SERVER_IP); client.println("Connection: close"); client.println();
  unsigned long timeout = millis() + 1500; while (client.available() == 0 && millis() < timeout) { delay(10); }
  bool serverResponded = client.available() > 0; client.stop(); return serverResponded;
}

void printSerialData() { Serial.println(getDataCSV(getFormattedTimeMS(), -1)); }

bool logDataToSD(bool strictQueue) {
  if (syncMode) return false;
  if (!isLogging || stopFlushInProgress) return false;

  // Build timestamp and get current sample index (do not increment yet)
  String ts = ((currentState == 2 || currentState == 4) ? nextRunLogTimestamp() : getFormattedTimeMS());
  int sampleIdx = (currentState == 2 || currentState == 4) ? (int)runLogSampleIndex : -1;
  String csvData = getDataCSV(ts, sampleIdx);
  
  size_t queueSizeAfterPush = 0;
  bool pushSuccess = false;
  
  while (true) {
    if (enqueueSDLine(csvData, queueSizeAfterPush)) {
      pushSuccess = true;
      break;
    }
    if (!strictQueue) { 
      Serial.println(">> WARN: SD queue full - skip one sample (non-strict)"); 
      return false; 
    }
    if (!isLogging || stopFlushInProgress || (currentState != 4 && currentState != 2)) { 
      return false; 
    }
    vTaskDelay(pdMS_TO_TICKS(REC_QUEUE_WAIT_MS));
  }
  
  // Increment sample index only when push to queue succeeds
  if (pushSuccess && (currentState == 2 || currentState == 4)) {
    runLogSampleIndex++;
  }
  
  static unsigned long lastQueueLogMs = 0;
  if (queueSizeAfterPush >= METRIC_QUEUE_WARN) {
    char buf[48]; const char *level = (queueSizeAfterPush >= METRIC_QUEUE_ERR) ? "SD_QUEUE_ERR" : "SD_QUEUE_WARN";
    snprintf(buf, sizeof(buf), "size=%u", (unsigned int)queueSizeAfterPush); metricLog(level, buf, lastQueueLogMs);
  }
  return true;
}

void TaskSDWriterCode(void * pvParameters) {
  static unsigned long lastSdWriteLogMs = 0; static unsigned long lastSdErrorTime = 0;
  for(;;) {
    if (manifestRebuildRequested && !manifestRebuildInProgress) {
      manifestRebuildRequested = false;
      manifestRebuildInProgress = true;
      unsigned long tRebuild0 = millis();
      Serial.println("[SYNC] Index rebuild started");
      if (ensureSDReady() && tryLockSD(SD_LOCK_TIMEOUT_MS)) {
        File dir = SD.open("/autotttoutput");
        if (!(dir && dir.isDirectory())) {
          if (dir) dir.close();
          SD.mkdir("/autotttoutput");
          dir = SD.open("/autotttoutput");
        }
        SD.remove(INDEX_FILE_PATH);
        File idx = SD.open(INDEX_FILE_PATH, FILE_WRITE);
        if (idx) {
          uint32_t n = 0;
          if (dir && dir.isDirectory()) {
            while (true) {
              File f = dir.openNextFile();
              if (!f) break;

              if (!f.isDirectory()) {
                String fullName = String(f.name());
                String baseName = fullName;
                int slashPos = baseName.lastIndexOf('/');
                if (slashPos >= 0) baseName = baseName.substring(slashPos + 1);

                if (baseName.endsWith(".txt") || baseName.endsWith(".TXT")) {
                  idx.print("/autotttoutput/");
                  idx.print(baseName);
                  idx.print("|");
                  idx.println((unsigned long)f.size());
                  n++;
                }
              }

              f.close();
              if ((n % 8) == 0) {
                idx.flush();
              }
              // Release SD lock every 25 files so /down endpoint can serve files during rebuild
              if ((n % 25) == 0 && n > 0) {
                idx.flush();
                unlockSD();
                vTaskDelay(pdMS_TO_TICKS(10));
                if (!tryLockSD(3000)) {
                  Serial.println("[SYNC] Index rebuild: lost SD lock, aborting");
                  idx.close();
                  dir.close();
                  goto rebuild_done;
                }
              }
              vTaskDelay(1);
            }
            dir.close();
          } else {
            Serial.println("[SYNC] Index rebuild note: /autotttoutput unavailable, writing empty index");
          }

          idx.flush();
          idx.close();

          // Verify write succeeded by re-opening and checking size
          File verIdx = SD.open(INDEX_FILE_PATH, FILE_READ);
          size_t verSize = verIdx ? verIdx.size() : 0;
          if (verIdx) verIdx.close();
          Serial.printf("[SYNC] Index rebuild done: %u entries in %lu ms (verified=%u bytes)\n",
                        (unsigned int)n, (unsigned long)(millis() - tRebuild0), (unsigned int)verSize);
          if (verSize == 0 && n > 0) {
            Serial.println("[SYNC] Index rebuild WARN: file 0 bytes after write - endpoint will re-queue");
          }
        } else {
          Serial.println("[SYNC] Index rebuild failed: cannot open _index.txt");
        }
        unlockSD();
      } else {
        Serial.println("[SYNC] Index rebuild deferred: SD not ready or lock timeout");
      }
      rebuild_done:
      manifestRebuildInProgress = false;
      vTaskDelay(1);
    }

    if (syncMode) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    xSemaphoreTake(sdQueueMutex, portMAX_DELAY);
    if (!sdWriteQueue.empty()) {
      SDQueueItem dataToWrite = sdWriteQueue.front(); sdWriteQueue.pop_front();
      size_t queueSizeAfterPop = sdWriteQueue.size(); xSemaphoreGive(sdQueueMutex);
      bool writeSuccess = false;
      if (isLogging) {
        if (!ensureSDReady()) {
          xSemaphoreTake(sdQueueMutex, portMAX_DELAY); sdWriteQueue.push_front(dataToWrite); xSemaphoreGive(sdQueueMutex);
          unsigned long now = millis(); if (now - lastSdErrorTime > 5000) { Serial.println(">> WARN: SD not ready - buffering data in RAM"); lastSdErrorTime = now; }
          vTaskDelay(100); continue;
        }
        unsigned long t0 = millis();
        if (!tryLockSD(SD_LOCK_TIMEOUT_MS)) { xSemaphoreTake(sdQueueMutex, portMAX_DELAY); sdWriteQueue.push_front(dataToWrite); xSemaphoreGive(sdQueueMutex); vTaskDelay(5); continue; }
        File file = SD.open(currentLogFileName, FILE_APPEND);
        if (file) { file.println(dataToWrite.data); file.close(); writeSuccess = true; } 
        else {
          st_sd = false; xSemaphoreTake(sdQueueMutex, portMAX_DELAY); sdWriteQueue.push_front(dataToWrite); xSemaphoreGive(sdQueueMutex);
          unsigned long now = millis(); if (now - lastSdErrorTime > 5000) { Serial.println(">> ERROR: SD file open failed - buffering in RAM"); lastSdErrorTime = now; }
        }
        unlockSD();
        if (writeSuccess) {
          unsigned long dt = millis() - t0;
          if (dt >= METRIC_SD_WRITE_WARN_MS) {
            char buf[64]; const char *level = (dt >= METRIC_SD_WRITE_ERR_MS) ? "SD_WRITE_ERR" : "SD_WRITE_WARN";
            snprintf(buf, sizeof(buf), "ms=%lu q=%u", dt, (unsigned int)queueSizeAfterPop); metricLog(level, buf, lastSdWriteLogMs);
          }
        }
      }
      vTaskDelay(writeSuccess ? 10 : 200); // On write failure, delay longer to allow bus to clear
    } else { 
      xSemaphoreGive(sdQueueMutex);
      vTaskDelay(50); 
    }
  }
}
