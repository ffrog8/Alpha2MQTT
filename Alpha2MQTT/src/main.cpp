/*
Name:		Alpha2MQTT.ino
Created:	24/Aug/2022
Author:		Daniel Young

This file is part of Alpha2MQTT (A2M) which is released under GNU GENERAL PUBLIC LICENSE.
See file LICENSE or go to https://choosealicense.com/licenses/gpl-3.0/ for full license details.

Notes

First, go and customise options at the top of Definitions.h!
*/

#include <bit>
#include <bitset>
#include <cctype>
#include <cstdarg>
#include <cstdint>
// Supporting files
#include "../RegisterHandler.h"
#include "../RS485Handler.h"
#include "../Definitions.h"
#include "../include/BootModes.h"
#include "../include/BootEvent.h"
#include "../include/WifiGuard.h"
#include "../include/BucketScheduler.h"
#include "../include/MqttEntities.h"
#include "../include/PortalConfig.h"
#include "../include/ConfigCodec.h"
#include "../include/MemoryHealth.h"
#include "../include/PollingConfig.h"
#include "../include/RebootRequest.h"
#include "../include/StatusReporting.h"
#include "../include/DiscoveryModel.h"
#include "../include/Rs485ProbeLogic.h"
#include "../include/Scheduler.h"
#include "../include/TimeProvider.h"
#include "../include/diag.h"
#include <Arduino.h>
#if defined MP_ESP8266
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <coredecls.h>
#elif defined MP_ESP32
#include <WiFi.h>
#include <WebServer.h>
#if defined(MP_ESPUNO_ESP32C6)
#ifdef LED_BUILTIN
#undef LED_BUILTIN
#endif // LED_BUILTIN
#define LED_BUILTIN 8
#elif !defined(MP_XIAO_ESP32C6)
#define LED_BUILTIN 2
#endif // MP_ESPUNO_ESP32C6, !MP_XIAO_ESP32C6
#endif
#include <DNSServer.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <Wire.h>
#ifndef DISABLE_DISPLAY
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#endif
#ifdef MP_ESPUNO_ESP32C6
#include <Adafruit_NeoPixel.h>
#endif // MP_ESPUNO_ESP32C6

#if defined MP_ESP8266
using HttpServer = ESP8266WebServer;
#else
using HttpServer = WebServer;
#endif

#define popcount __builtin_popcount

#ifndef BUILD_TS_MS
#define BUILD_TS_MS 0ULL
#endif

static inline void
diagDelay(uint32_t ms)
{
	diag_note_yield(millis());
	delay(ms);
}

static inline void
diagYield(void)
{
	diag_note_yield(millis());
	yield();
}

// Device parameters
char _version[20] = "";
char deviceSerialNumber[17]; // 8 registers = max 16 chars (usually 15)
char deviceBatteryType[32];
char haUniqueId[32] = "A2M-UNKNOWN";
char controllerIdentifier[40] = "";
char statusTopic[128];
char deviceName[32];
char configSetTopic[64];
char rs485StubControlTopic[96];
char lastResetReason[64] = "";
HttpServer httpServer(80);
bool httpControlPlaneEnabled = false;
static bool inMqttCallback = false;
static bool pendingPollingConfigSet = false;
static bool pendingRs485StubControlSet = false;
static bool pendingEntityCommandSet = false;
static const mqttState *pendingEntityCommand = nullptr;
// Shared deferred-control payload buffer for small MQTT commands. MQTT pumping is
// blocked while any deferred command is pending, so stub/entity commands never
// overlap in this storage.
static char pendingDeferredControlPayload[512] = "";
static uint32_t manualRegisterReadSeq = 0;

enum PortalStatus : uint8_t {
	portalStatusIdle = 0,
	portalStatusConnecting,
	portalStatusSuccess,
	portalStatusFailed
};
PortalStatus portalStatus = portalStatusIdle;
char portalStatusReason[64] = "";
char portalStatusSsid[33] = "";
char portalStatusIp[20] = "";
int portalLastDisconnectReason = -1;
char portalLastDisconnectLabel[32] = "";
unsigned long portalConnectStart = 0;
bool portalNeedsMqttConfig = false;
bool portalMqttSaved = false;
bool portalRebootScheduled = false;
unsigned long portalRebootAt = 0;
void *portalRoutesBoundServer = nullptr;
const char kPreferenceBootIntent[] = "Boot_Intent";
const char kPreferenceBootMode[] = "Boot_Mode";
const char kPreferenceDeviceSerial[] = "Device_Serial";
const char kPreferenceBucketMap[] = "Bucket_Map";
const char kPreferencePollInterval[] = "poll_interval_s";
const char kPreferenceBucketMapMigrated[] = "Bucket_Map_Migrated";
// Persisted "last polling-config change" timestamp published as polling-config last_change.
const char kPreferencePollingLastChange[] = "polling_last_change";
const char kControllerInverterSerialEntity[] = "inverter_serial";
const char kControllerModel[] = "Alpha2MQTT Bridge";
const char kInverterModelFallback[] = "Alpha ESS";
constexpr size_t kPrefBootIntentMaxLen = 24;
constexpr size_t kPrefBootModeMaxLen = 24;
constexpr size_t kPrefDeviceSerialMaxLen = 32;
constexpr size_t kPrefWifiSsidMaxLen = 64;
constexpr size_t kPrefWifiPasswordMaxLen = 64;
constexpr size_t kPrefMqttServerMaxLen = 64;
constexpr size_t kPrefMqttUsernameMaxLen = 64;
constexpr size_t kPrefMqttPasswordMaxLen = 64;
constexpr size_t kPrefBucketMapMaxLen = 2048;
constexpr size_t kPollingConfigChunkMapMaxLen = 1024;
constexpr size_t kPrefPollingLastChangeMaxLen = 32;
// Portal handlers run on a constrained callback stack on ESP8266.
// Keep large polling map buffers in static storage to avoid stack corruption/panics.
// `g_portalBucketMapScratch` is also the deferred `config/set` payload buffer, so
// long-lived discovery/status code must not reuse it while MQTT callbacks can fire.
static BucketId g_portalBucketsScratch[kMqttEntityDescriptorCount];
static char g_portalBucketMapScratch[kPrefBucketMapMaxLen];
static size_t g_lastPublishedPollingConfigChunkCount = 0;
static char g_portalRowRenderBuf[768];
BootIntent currentBootIntent = BootIntent::Normal;
BootIntent bootIntentForPublish = BootIntent::Normal;
BootMode currentBootMode = BootMode::Normal;
BootMode bootModeForDiagnostics = BootMode::Normal;
bool bootEventPublished = false;
bool inverterReady = false;
bool inverterSubscriptionsSet = false;
SubsystemPlan bootPlan = { true, true, true };
BootMemWorst bootMemWorst = { MemLevel::Ok, BootMemStage::Boot0, { 0, 0, 0 } };
bool bootMemWarningEmitted = false;
#if defined(MP_ESP8266)
const int kSafeModePin = 0; // D3 (GPIO0) strap for safe mode.
#endif
const uint32_t kEventRateLimitMs = 30000;
const uint32_t kPollOverrunMs = 5000;
const uint32_t kMqttCommandWarmupMs = 3000;
uint32_t wifiReconnectCount = 0;
uint32_t mqttReconnectCount = 0;
uint32_t lastMqttConnectMs = 0;
uint32_t pollOkCount = 0;
uint32_t pollErrCount = 0;
uint32_t lastPollMs = 0;
uint32_t lastOkTsMs = 0;
uint32_t lastErrTsMs = 0;
int lastErrCode = 0;
uint32_t essSnapshotAttemptCount = 0;
bool essSnapshotLastOk = false;
uint32_t dispatchLastRunMs = 0;
char dispatchLastSkipReason[48] = "";
uint32_t schedTenSecLastRunMs = 0;
uint32_t schedOneMinLastRunMs = 0;
uint32_t schedFiveMinLastRunMs = 0;
uint32_t schedOneHourLastRunMs = 0;
uint32_t schedOneDayLastRunMs = 0;
bool lastWifiConnected = false;
bool lastMqttConnected = false;
bool pendingWifiDisconnectEvent = false;
bool pendingMqttDisconnectEvent = false;
uint32_t eventCounts[static_cast<uint8_t>(MqttEventCode::MaxValue)] = {};
EventLimiter eventLimiter;

// WiFi parameters
WiFiClient _wifi;
#if defined MP_ESP8266
#define WIFI_POWER_MAX 20.5
#define WIFI_POWER_MIN 12
#define WIFI_POWER_DECREMENT .25
float wifiPower = WIFI_POWER_MAX + WIFI_POWER_DECREMENT;  // Will decrement once before setting
#else // MP_ESP8266
wifi_power_t wifiPower = WIFI_POWER_11dBm; // Will bump to max before setting
#endif // MP_ESP8266

// MQTT parameters
PubSubClient _mqtt(_wifi);

// Buffer Size (and therefore payload size calc)
int _maxPayloadSize;

// I want to declare this once at a modular level, keep the heap somewhere in check.
char* _mqttPayload;

bool resendHaData = false;
bool resendHaPreludePending = false;
size_t resendHaNextEntityIndex = 0;
bool resendHaClearStaleInverterPending = false;
static constexpr size_t kStaleInverterDiscoveryQueueMax = 2;
size_t resendHaClearStaleInverterIndex = 0;
size_t resendHaClearStaleInverterQueueIndex = 0;
size_t resendHaClearStaleInverterQueueCount = 0;
char resendHaClearStaleInverterDeviceIds[kStaleInverterDiscoveryQueueMax][64] = {{0}};
bool resendAllData = false;
static constexpr size_t kHaDiscoveryBatchSize = 1;
// Human-readable timestamp of the most recent polling-config mutation.
char _pollingConfigLastChange[32] = "";
// Bucket ids accepted via /config/set mapping (legacy freq* aliases are still accepted).
const char *_pollingAllowedIntervals[] = {
	"ten_sec",
	"one_min",
	"five_min",
	"one_hour",
	"one_day",
	"user",
	"disabled"
};
const size_t _pollingAllowedIntervalCount = sizeof(_pollingAllowedIntervals) / sizeof(_pollingAllowedIntervals[0]);

uint32_t pollIntervalSeconds = kPollIntervalDefaultSeconds;
uint32_t persistLoadOk = 0;
uint32_t persistLoadErr = 0;
uint32_t persistUnknownEntityCount = 0;
uint32_t persistInvalidBucketCount = 0;
uint32_t persistDuplicateEntityCount = 0;

uint32_t schedUserLastRunMs = 0;
uint16_t schedTenSecCount = 0;
uint16_t schedOneMinCount = 0;
uint16_t schedFiveMinCount = 0;
uint16_t schedOneHourCount = 0;
uint16_t schedOneDayCount = 0;
uint16_t schedUserCount = 0;
static constexpr BucketId kRuntimeBuckets[] = {
	BucketId::TenSec,
	BucketId::OneMin,
	BucketId::FiveMin,
	BucketId::OneHour,
	BucketId::OneDay,
	BucketId::User
};
size_t schedNextCursor[sizeof(kRuntimeBuckets) / sizeof(kRuntimeBuckets[0])] = {};
BucketRuntimeBudgetState schedBudgetState[sizeof(kRuntimeBuckets) / sizeof(kRuntimeBuckets[0])] = {};
uint32_t pollingBudgetOverrunCount = 0;

// OLED variables
char _oledOperatingIndicator = '*';
char _oledLine2[OLED_CHARACTER_WIDTH] = "";
char _oledLine3[OLED_CHARACTER_WIDTH] = "";
char _oledLine4[OLED_CHARACTER_WIDTH] = "";

// Config handling
struct AppConfig {
	String wifiSSID;
	String wifiPass;
	String mqttSrvr;
	int mqttPort;
	String mqttUser;
	String mqttPass;
#if defined(MP_XIAO_ESP32C6) || defined(MP_ESPUNO_ESP32C6)
	bool extAntenna;
#endif // MP_XIAO_ESP32C6 || MP_ESPUNO_ESP32C6
};

AppConfig appConfig;

// RS485 and AlphaESS functionality are packed up into classes
// to keep separate from the main program logic.
RS485Handler* _modBus;
RegisterHandler* _registerHandler;

#if defined(DEBUG_OVER_SERIAL) || defined(DEBUG_LEVEL2) || defined(DEBUG_OUTPUT_TX_RX)
// Fixed char array for messages to the serial port
char _debugOutput[128];
#endif // DEBUG_OVER_SERIAL || DEBUG_LEVEL2 || DEBUG_OUTPUT_TX_RX

int32_t regNumberToRead = -1;
#ifdef A2M_DEBUG_WIFI
uint32_t wifiReconnects = 0;
#endif // A2M_DEBUG_WIFI
#ifdef DEBUG_CALLBACKS
uint32_t receivedCallbacks = 0;
uint32_t unknownCallbacks = 0;
uint32_t badCallbacks = 0;
#endif // DEBUG_CALLBACKS
uint32_t rs485Errors = 0;
#ifdef DEBUG_OPS
uint32_t opCounter = 0;
#endif // DEBUG_OPS

enum class PollingBudgetMetric : uint8_t {
	UsedMs = 0,
	LimitMs,
	BacklogCount,
	BacklogOldestAgeMs,
	LastFullCycleAgeMs
};

struct PollingBudgetEntitySpec {
	mqttEntityId entityId;
	BucketId bucketId;
	PollingBudgetMetric metric;
};

static const PollingBudgetEntitySpec kPollingBudgetEntitySpecs[] = {
	{ mqttEntityId::entityPollingBudgetUsedMs10s, BucketId::TenSec, PollingBudgetMetric::UsedMs },
	{ mqttEntityId::entityPollingBudgetLimitMs10s, BucketId::TenSec, PollingBudgetMetric::LimitMs },
	{ mqttEntityId::entityPollingBacklogCount10s, BucketId::TenSec, PollingBudgetMetric::BacklogCount },
	{ mqttEntityId::entityPollingBacklogOldestAgeMs10s, BucketId::TenSec, PollingBudgetMetric::BacklogOldestAgeMs },
	{ mqttEntityId::entityPollingLastFullCycleAgeMs10s, BucketId::TenSec, PollingBudgetMetric::LastFullCycleAgeMs },
	{ mqttEntityId::entityPollingBudgetUsedMs1m, BucketId::OneMin, PollingBudgetMetric::UsedMs },
	{ mqttEntityId::entityPollingBudgetLimitMs1m, BucketId::OneMin, PollingBudgetMetric::LimitMs },
	{ mqttEntityId::entityPollingBacklogCount1m, BucketId::OneMin, PollingBudgetMetric::BacklogCount },
	{ mqttEntityId::entityPollingBacklogOldestAgeMs1m, BucketId::OneMin, PollingBudgetMetric::BacklogOldestAgeMs },
	{ mqttEntityId::entityPollingLastFullCycleAgeMs1m, BucketId::OneMin, PollingBudgetMetric::LastFullCycleAgeMs },
	{ mqttEntityId::entityPollingBudgetUsedMs5m, BucketId::FiveMin, PollingBudgetMetric::UsedMs },
	{ mqttEntityId::entityPollingBudgetLimitMs5m, BucketId::FiveMin, PollingBudgetMetric::LimitMs },
	{ mqttEntityId::entityPollingBacklogCount5m, BucketId::FiveMin, PollingBudgetMetric::BacklogCount },
	{ mqttEntityId::entityPollingBacklogOldestAgeMs5m, BucketId::FiveMin, PollingBudgetMetric::BacklogOldestAgeMs },
	{ mqttEntityId::entityPollingLastFullCycleAgeMs5m, BucketId::FiveMin, PollingBudgetMetric::LastFullCycleAgeMs },
	{ mqttEntityId::entityPollingBudgetUsedMs1h, BucketId::OneHour, PollingBudgetMetric::UsedMs },
	{ mqttEntityId::entityPollingBudgetLimitMs1h, BucketId::OneHour, PollingBudgetMetric::LimitMs },
	{ mqttEntityId::entityPollingBacklogCount1h, BucketId::OneHour, PollingBudgetMetric::BacklogCount },
	{ mqttEntityId::entityPollingBacklogOldestAgeMs1h, BucketId::OneHour, PollingBudgetMetric::BacklogOldestAgeMs },
	{ mqttEntityId::entityPollingLastFullCycleAgeMs1h, BucketId::OneHour, PollingBudgetMetric::LastFullCycleAgeMs },
	{ mqttEntityId::entityPollingBudgetUsedMs1d, BucketId::OneDay, PollingBudgetMetric::UsedMs },
	{ mqttEntityId::entityPollingBudgetLimitMs1d, BucketId::OneDay, PollingBudgetMetric::LimitMs },
	{ mqttEntityId::entityPollingBacklogCount1d, BucketId::OneDay, PollingBudgetMetric::BacklogCount },
	{ mqttEntityId::entityPollingBacklogOldestAgeMs1d, BucketId::OneDay, PollingBudgetMetric::BacklogOldestAgeMs },
	{ mqttEntityId::entityPollingLastFullCycleAgeMs1d, BucketId::OneDay, PollingBudgetMetric::LastFullCycleAgeMs },
	{ mqttEntityId::entityPollingBudgetUsedMsUser, BucketId::User, PollingBudgetMetric::UsedMs },
	{ mqttEntityId::entityPollingBudgetLimitMsUser, BucketId::User, PollingBudgetMetric::LimitMs },
	{ mqttEntityId::entityPollingBacklogCountUser, BucketId::User, PollingBudgetMetric::BacklogCount },
	{ mqttEntityId::entityPollingBacklogOldestAgeMsUser, BucketId::User, PollingBudgetMetric::BacklogOldestAgeMs },
	{ mqttEntityId::entityPollingLastFullCycleAgeMsUser, BucketId::User, PollingBudgetMetric::LastFullCycleAgeMs }
};

static BucketRuntimeBudgetState *bucketBudgetStateFor(BucketId bucket);
static void resetBucketBudgetStates(void);
static bool pollingBudgetExceeded(void);
static bool formatPollingBudgetEntityValue(mqttEntityId entityId, char *out, size_t outSize);

#ifdef MP_ESPUNO_ESP32C6
Adafruit_NeoPixel _statusPixel(1, LED_BUILTIN, NEO_GRB + NEO_KHZ800);
uint32_t _statusLedColor = 0;
#endif // MP_ESPUNO_ESP32C6

//#define OP_DATA_AVG_CNT 4
#define PUSH_FUDGE_FACTOR 200 // Watts
struct {
	opMode   a2mOpMode = opMode::opModeLoadFollow;
	bool     a2mReadyToUseOpMode = false;
	uint16_t a2mSocTarget = SOC_TARGET_MAX;   // Stored as percent (0-100)
	bool     a2mReadyToUseSocTarget = false;
	int32_t  a2mPwrCharge = INVERTER_POWER_MAX;
	bool     a2mReadyToUsePwrCharge = false;
	int32_t  a2mPwrDischarge = INVERTER_POWER_MAX;
	bool     a2mReadyToUsePwrDischarge = false;
	int32_t  a2mPwrPush = 0;
	bool     a2mReadyToUsePwrPush = false;

	uint16_t essDispatchStart = DISPATCH_START_STOP;
	uint16_t essDispatchMode = 0;
	int32_t  essDispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET;
	uint16_t essDispatchSoc = 0;      // Stored as ESS register value. (percent / 0.4)
	uint16_t essBatterySoc = 0;       // Stored as ESS register value. (percent / 0.1)
	int16_t  essBatteryPower = 0;	// positive->discharge : negative->charge
	int32_t  essGridPower = 0;	// positive->fromGrid : negative->toGrid
	int32_t  essPvPower = 0;	// Positive
	uint16_t essInverterMode = UINT16_MAX;
	bool     essRs485Connected = false;
 } opData;

// Updated only by refreshEssSnapshot(). When false, ESS-derived publishes and dispatch must not run.
static bool essSnapshotValid = false;

static const unsigned long kKnownBaudRates[] = { 9600, 115200, 19200, 57600, 38400, 14400, 4800 };
static constexpr uint32_t kRs485ProbeAttemptDelayMs = 1000;
static constexpr uint32_t kRs485ProbeMaxBackoffMs = 15000;

enum class Rs485ConnectState : uint8_t {
	NotStarted = 0,
	ProbingBaud,
	ReadingIdentity,
	Connected
};

static Rs485ConnectState rs485ConnectState = Rs485ConnectState::NotStarted;
static int rs485BaudIndex = -1;
static uint8_t rs485AttemptsInCycle = 0;
static uint32_t rs485CycleBackoffMs = kRs485ProbeAttemptDelayMs;
static unsigned long rs485NextAttemptAtMs = 0;
static unsigned long rs485LockedBaud = 0;
static const char *rs485UartInfo = nullptr;
static uint32_t rs485ProbeLastAttemptMs = 0;

static bool rs485TryReadIdentityOnce(void);
static void rs485ProbeTick(void);
#if RS485_STUB
static void rs485ApplyStubConnectivityMode(Rs485StubMode mode);
#endif

/*
 * Home Assistant auto-discovered values
 */
// Entity descriptors live in flash (rodata). Per-boot mutable state is allocated only when MQTT is enabled.




// These timers are used in the main loop.
#define RUNSTATE_INTERVAL 5000
#define STATUS_INTERVAL_TEN_SECONDS 10000
#define STATUS_INTERVAL_ONE_MINUTE 60000
#define STATUS_INTERVAL_FIVE_MINUTE 300000
#define STATUS_INTERVAL_ONE_HOUR 3600000
#define STATUS_INTERVAL_ONE_DAY 86400000
#define UPDATE_STATUS_BAR_INTERVAL 500

#ifndef DISABLE_DISPLAY
#ifdef LARGE_DISPLAY
// Pins GPIO22 and GPIO21 (SCL/SDA) if ESP32
// Pins GPIO23 and GPIO22 (SCL/SDA) if XIAO ESP32C6
Adafruit_SSD1306 _display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, 0);
#else // LARGE_DISPLAY
// Wemos OLED Shield set up. 64x48
// Pins D1 D2 if ESP8266
// Pins GPIO22 and GPIO21 (SCL/SDA) with optional reset on GPIO13 if ESP32
Adafruit_SSD1306 _display(OLED_RST_PIN);
#endif // LARGE_DISPLAY
#endif // DISABLE_DISPLAY

// Forward declarations (required for PlatformIO)
void updateOLED(bool justStatus, const char* line2, const char* line3, const char* line4);
void configLoop(void);
void configHandler(void);
void setupWifi(bool initialConnect);
void mqttReconnect(void);
void mqttCallback(char* topic, byte* message, unsigned int length);
void sendHaData(void);
void requestHaDataResend(void);
void getA2mOpDataFromEss(void);
bool refreshEssSnapshot(void);
void sendData(void);
void sendStatus(bool includeEssSnapshot);
void updateRunstate(void);
uint32_t getUptimeSeconds(void);
bool checkTimer(unsigned long *lastRun, unsigned long interval);
void emptyPayload(void);
bool sendMqtt(const char*, bool);
bool sendDataFromMqttState(const mqttState*, bool, const modbusRequestAndResponse *preparedResponse = nullptr);
void loadPollingConfig(void);
void recomputeBucketCounts(void);
void publishPollingConfig(void);
bool publishConfigDiscovery(void);
static bool publishHaEntityDiscovery(const mqttState*);
bool clearHaEntityDiscovery(const mqttState*, const char *deviceId);
bool publishControllerInverterSerialDiscovery(void);
void publishControllerInverterSerialState(void);
bool handlePollingConfigSet(const char*);
const char* mqttUpdateFreqToString(mqttUpdateFreq);
bool mqttUpdateFreqFromString(const char*, mqttUpdateFreq*);
void updatePollingLastChange(void);
void getPollingTimestamp(char*, size_t);
void buildPollingKey(const mqttState*, char*, size_t);
static bool buildPersistedPollingConfigMap(const BucketId *buckets, char *map, size_t mapLen);
static bool readLegacyPollingPref(size_t index,
                                  const mqttState *entity,
                                  int defaultValue,
                                  int &storedValue,
                                  void *context);
void checkAndSetDispatchMode(void);
void printWifiBars(int rssi);
void getOpModeDesc(char *dest, size_t size, enum opMode mode);
void getInverterModeDesc(char *dest, size_t size, uint16_t inverterMode);
modbusRequestAndResponseStatusValues addToPayload(const char*);
enum gridStatus isGridOnline();
enum opMode lookupOpMode(const char*);
modbusRequestAndResponseStatusValues getSerialNumber();
void setStatusLed(bool on);
void setStatusLedColor(uint8_t red, uint8_t green, uint8_t blue);
void updateStatusLed(void);
void buildDeviceName(void);
void publishBootEventOncePerBoot(void);
void setMqttIdentifiersFromSerial(const char *serial);
void queueStaleInverterDiscoveryClear(const char *deviceId);
bool inverterSerialKnown(void);
const char *discoveryDeviceIdForScope(DiscoveryDeviceScope scope);
void publishStatusNow(void);
void publishEvent(MqttEventCode code, const char *detail);
MqttEventCode eventCodeFromResult(modbusRequestAndResponseStatusValues result);
void noteRs485Error(modbusRequestAndResponseStatusValues result, const char *detail);
static void processPendingEntityCommand(void);
static void applyRs485StubControlPayload(const char *payload);
static void publishManualRegisterReadState(int32_t requestedReg);
void setBootIntentAndReboot(BootIntent intent, bool persistIntent = true);
static void persistUserBootIntent(BootIntent intent);
static void persistUserBootMode(BootMode mode);
static void persistUserMqttConfig(const char *server, int port, const char *user, const char *pass);
static void persistUserWifiCredentials(const char *ssid, const char *pass);
static void persistUserExtAntenna(bool enabled);
static bool persistUserPollingConfig(uint32_t intervalSeconds, const char *bucketMap);
static void persistUserPollingLastChange(const char *lastChange);
static void persistUserDeviceSerial(const char *serial);
static void persistDefaultsIfMissing(void);
void setupHttpControlPlane(void);
void handleHttpRoot(void);
void handleHttpRestartAlias(void);
void handleRebootNormal(void);
void handleRebootAp(void);
void handleRebootWifi(void);
void triggerRestart(void);
void subscribeInverterTopics(void);
void serviceRs485Hooks(void);
const char* portalStatusLabel(PortalStatus status);
const char* wifiStatusReason(wl_status_t status);
const char* wifiStatusLabel(wl_status_t status);
const char* wifiModeLabel(WiFiMode_t mode);
void handlePortalStatusRequest(WiFiManager& wifiManager);
void handlePortalRebootNormalRequest(WiFiManager& wifiManager);
bool portalHasPersistedWifiCredentials(void);
void configHandlerSta(void);
const char *portalCustomHeadScript(void);
static inline bool isMqttPumpBlocked(void);
static bool pumpMqttOnce(void);

void
buildDeviceName(void)
{
	uint8_t mac[6] = { 0 };
	WiFi.macAddress(mac);
	snprintf(deviceName, sizeof(deviceName), "%s-%02X%02X%02X", DEVICE_NAME, mac[3], mac[4], mac[5]);
	buildControllerIdentifier(mac, controllerIdentifier, sizeof(controllerIdentifier));
	snprintf(configSetTopic, sizeof(configSetTopic), "%s/config/set", deviceName);
	snprintf(statusTopic, sizeof(statusTopic), "%s/status", deviceName);
	snprintf(rs485StubControlTopic, sizeof(rs485StubControlTopic), "%s/debug/rs485_stub/set", deviceName);
}

bool
inverterSerialKnown(void)
{
	return inverterSerialIsValid(deviceSerialNumber);
}

const char *
discoveryDeviceIdForScope(DiscoveryDeviceScope scope)
{
	if (scope == DiscoveryDeviceScope::Controller) {
		return controllerIdentifier;
	}
	if (!inverterSerialKnown()) {
		return "";
	}
	static char inverterIdentifier[64];
	buildInverterIdentifier(deviceSerialNumber, inverterIdentifier, sizeof(inverterIdentifier));
	return inverterIdentifier;
}

void
setMqttIdentifiersFromSerial(const char *serial)
{
	if (serial == nullptr || *serial == '\0') {
		return;
	}

	buildInverterHaUniqueId(serial, haUniqueId, sizeof(haUniqueId));
	// Subscriptions are bound to the HA unique id; if identity changes from unknown/persisted, resubscribe.
	inverterSubscriptionsSet = false;
}

void
queueStaleInverterDiscoveryClear(const char *deviceId)
{
	if (deviceId == nullptr || deviceId[0] == '\0') {
		return;
	}
	for (size_t i = 0; i < resendHaClearStaleInverterQueueCount; ++i) {
		if (strcmp(resendHaClearStaleInverterDeviceIds[i], deviceId) == 0) {
			resendHaClearStaleInverterPending = true;
			return;
		}
	}
	if (resendHaClearStaleInverterQueueCount >= kStaleInverterDiscoveryQueueMax) {
		return;
	}
	strlcpy(resendHaClearStaleInverterDeviceIds[resendHaClearStaleInverterQueueCount],
	        deviceId,
	        sizeof(resendHaClearStaleInverterDeviceIds[0]));
	resendHaClearStaleInverterQueueCount++;
	resendHaClearStaleInverterPending = true;
}

void
publishBootEventOncePerBoot(void)
{
	if (!shouldPublishBootEvent(bootEventPublished, _mqtt.connected())) {
		return;
	}

	char bootTopic[128];
	char payload[256];
	String resetReason = ESP.getResetReason();

	snprintf(bootTopic, sizeof(bootTopic), "%s/boot", deviceName);
	snprintf(payload, sizeof(payload),
		 "{ \"boot_intent\": \"%s\", \"reset_reason\": \"%s\", \"ts_ms\": %lu, \"fw_build_ts_ms\": %llu }",
		 bootIntentToString(bootIntentForPublish), resetReason.c_str(), millis(),
		 static_cast<unsigned long long>(BUILD_TS_MS));

	const bool published = _mqtt.publish(bootTopic, payload, true);
	bootEventPublished = bootEventPublishedAfterAttempt(bootEventPublished, published);
}

#if defined(DEBUG_OVER_SERIAL)
void
logHeap(const char *label)
{
	Serial.print("Heap ");
	Serial.print(label);
	Serial.print(": free=");
	Serial.print(ESP.getFreeHeap());
#if defined(MP_ESP8266)
	Serial.print(" max=");
	Serial.print(ESP.getMaxFreeBlockSize());
	Serial.print(" frag=");
	Serial.print(ESP.getHeapFragmentation());
#endif
	Serial.println();
}
#endif

static MemSample
readMemSample()
{
	MemSample sample{};
	sample.freeB = ESP.getFreeHeap();
#if defined(MP_ESP8266)
	sample.maxBlockB = ESP.getMaxFreeBlockSize();
	sample.fragPct = static_cast<uint8_t>(ESP.getHeapFragmentation());
#else
	sample.maxBlockB = 0;
	sample.fragPct = 0;
#endif
	return sample;
}

static void
recordBootMemStage(BootMemStage stage)
{
	const MemSample sample = readMemSample();
	const MemLevel level = evaluateBootMem(stage, sample);
	updateBootMemWorst(bootMemWorst, stage, sample, level);
#ifdef DEBUG_OVER_SERIAL
	if (!bootMemWarningEmitted && level != MemLevel::Ok) {
		Serial.print(F("BOOT MEM "));
		Serial.print(static_cast<uint8_t>(level));
		Serial.print(F(" S"));
		Serial.print(static_cast<uint8_t>(stage));
		Serial.print(F(" free="));
		Serial.print(sample.freeB);
		Serial.print(F(" max="));
		Serial.print(sample.maxBlockB);
		Serial.print(F(" frag="));
		Serial.print(sample.fragPct);
		if (stage == BootMemStage::Boot0) {
			Serial.print(F(" thr b0 w14000/14000 c12000/12000"));
		} else {
			Serial.print(F(" thr bN w6000/4096/25 c4000/2048/35"));
		}
		Serial.println();
		bootMemWarningEmitted = true;
	}
#endif
}

void
pumpMqttDuringSetup(uint32_t durationMs)
{
	uint32_t start = millis();

	while (millis() - start < durationMs) {
		pumpMqttOnce();
		diagDelay(5);
	}
}

static inline bool
isMqttPumpBlocked(void)
{
	if (inMqttCallback) {
		return true;
	}
	// PubSubClient dispatches at most one inbound packet per loop() call.
	// When a deferred config or entity command is already queued for loop(),
	// stop pumping MQTT so later packets stay queued on the socket instead of
	// overwriting the single pending slot from callback context.
	if (pendingPollingConfigSet || pendingRs485StubControlSet || pendingEntityCommandSet) {
		return true;
	}
	if (_modBus != nullptr && _modBus->inTransaction()) {
		return true;
	}
	return false;
}

static bool
pumpMqttOnce(void)
{
	if (!_mqtt.connected()) {
		return false;
	}
	if (isMqttPumpBlocked()) {
		return true;
	}
	return _mqtt.loop();
}

static inline bool
mqttCommandWarmupActive(void)
{
	return _mqtt.connected() && (millis() - lastMqttConnectMs) < kMqttCommandWarmupMs;
}

void
serviceRs485Hooks(void)
{
	pumpMqttOnce();
	if (httpControlPlaneEnabled) {
		httpServer.handleClient();
	}
}

static bool
rs485TryReadIdentityOnce(void)
{
	if (_registerHandler == NULL) {
		return false;
	}

	modbusRequestAndResponseStatusValues result;
	modbusRequestAndResponse response;

	result = _registerHandler->readHandledRegister(REG_SYSTEM_INFO_R_EMS_SN_BYTE_1_2, &response);
	if (result != modbusRequestAndResponseStatusValues::readDataRegisterSuccess ||
	    response.dataValueFormatted[0] == '\0' ||
	    strlen(response.dataValueFormatted) < 15) {
		return false;
	}

	char previousSerial[sizeof(deviceSerialNumber)];
	strlcpy(previousSerial, deviceSerialNumber, sizeof(previousSerial));
	char staleInverterIdentifier[64];
	const bool staleInverterNamespace = buildStaleInverterIdentifier(previousSerial,
	                                                                 response.dataValueFormatted,
	                                                                 staleInverterIdentifier,
	                                                                 sizeof(staleInverterIdentifier));
	char currentInverterIdentifier[64];
	buildInverterIdentifier(response.dataValueFormatted,
	                       currentInverterIdentifier,
	                       sizeof(currentInverterIdentifier));

	strlcpy(deviceSerialNumber, response.dataValueFormatted, sizeof(deviceSerialNumber));
	persistUserDeviceSerial(deviceSerialNumber);
	_registerHandler->setSerialNumberPrefix(deviceSerialNumber[0], deviceSerialNumber[1]);
	inverterReady = true;

	// Battery type is helpful for diagnostics, but it is not required to establish inverter identity.
	result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_TYPE, &response);
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess &&
	    response.dataValueFormatted[0] != '\0') {
		strlcpy(deviceBatteryType, response.dataValueFormatted, sizeof(deviceBatteryType));
	}

	if (!inverterHaUniqueIdMatchesSerial(haUniqueId, deviceSerialNumber)) {
		// Persisted serial is only a hint. Refresh topic/discovery identity whenever the live
		// inverter reports a different serial so reconnects cannot stay pinned to stale namespaces.
		if (staleInverterNamespace) {
			queueStaleInverterDiscoveryClear(staleInverterIdentifier);
		}
		setMqttIdentifiersFromSerial(deviceSerialNumber);
	}
	if (haUniqueId[0] != '\0' &&
	    currentInverterIdentifier[0] != '\0' &&
	    strcmp(haUniqueId, currentInverterIdentifier) != 0) {
		// Upgrades from the legacy HA namespace used haUniqueId as the discovery topic path.
		// Clear that retained namespace as well, even when the inverter serial itself did not change.
		queueStaleInverterDiscoveryClear(haUniqueId);
	}

#ifdef DEBUG_OVER_SERIAL
	snprintf(_debugOutput, sizeof(_debugOutput), "Inverter identified: %s", deviceSerialNumber);
	Serial.println(_debugOutput);
#endif

	return true;
}

#if RS485_STUB
static void
rs485ApplyStubConnectivityMode(Rs485StubMode mode)
{
	if (_modBus == NULL || _registerHandler == NULL) {
		return;
	}

	rs485AttemptsInCycle = 0;
	rs485CycleBackoffMs = kRs485ProbeAttemptDelayMs;
	rs485NextAttemptAtMs = millis();
	rs485ProbeLastAttemptMs = 0;

	if (rs485StubModeUsesProbeLifecycle(mode)) {
		rs485LockedBaud = 0;
		rs485ConnectState = Rs485ConnectState::ProbingBaud;
		essSnapshotValid = false;
		essSnapshotLastOk = false;
		resendAllData = true;
		return;
	}

	// Online-like stub modes are intended to exercise snapshot and publish behavior directly.
	// Do not make them depend on the separate baud-probe state machine or issue Modbus reads
	// from the MQTT callback path.
	if (!inverterSerialKnown()) {
		strlcpy(deviceSerialNumber, "STUBSN000000000", sizeof(deviceSerialNumber));
		setMqttIdentifiersFromSerial(deviceSerialNumber);
	}
	rs485ConnectState = Rs485ConnectState::Connected;
	rs485LockedBaud = DEFAULT_BAUD_RATE;
	essSnapshotValid = false;
	essSnapshotLastOk = false;
	resendAllData = true;
}
#endif

static void
rs485ProbeTick(void)
{
	if (rs485ConnectState == Rs485ConnectState::Connected) {
		return;
	}
	if (_modBus == NULL || _registerHandler == NULL) {
		return;
	}

	const unsigned long now = millis();
	if (static_cast<long>(now - rs485NextAttemptAtMs) < 0) {
		return;
	}

	if (rs485ConnectState == Rs485ConnectState::ReadingIdentity) {
		rs485ProbeLastAttemptMs = now;
		if (rs485TryReadIdentityOnce()) {
			rs485ConnectState = Rs485ConnectState::Connected;
			rs485AttemptsInCycle = 0;
			rs485CycleBackoffMs = kRs485ProbeAttemptDelayMs;

			// Now that inverter identity is known, discovery/config can be published under the real HA unique id.
			// If a deferred config/set payload is already queued, let loop() apply that first instead of
			// reusing the shared bucket-map scratch and clobbering the pending MQTT command.
			if (!pendingPollingConfigSet) {
				loadPollingConfig();
			}
			requestHaDataResend();
			resendAllData = true;
			return;
		}

		rs485NextAttemptAtMs = now + rs485CycleBackoffMs;
		rs485CycleBackoffMs = rs485NextBackoffMs(rs485CycleBackoffMs, kRs485ProbeMaxBackoffMs);
		return;
	}

	// ProbingBaud: try one baud per tick, and back off between full cycles.
	rs485ProbeLastAttemptMs = now;
	rs485BaudIndex = rs485NextIndex(rs485BaudIndex, static_cast<int>(sizeof(kKnownBaudRates) / sizeof(kKnownBaudRates[0])));
	const unsigned long baud = kKnownBaudRates[rs485BaudIndex];
	char baudRateString[10] = "";
	snprintf(baudRateString, sizeof(baudRateString), "%lu", baud);

	updateOLED(false, "Test Baud", baudRateString, rs485UartInfo ? rs485UartInfo : "");

#ifdef DEBUG_OVER_SERIAL
	snprintf(_debugOutput, sizeof(_debugOutput), "About To Try: %lu", baud);
	Serial.println(_debugOutput);
	logHeap("before RS485 probe");
#endif
	recordBootMemStage(BootMemStage::Boot4);

	_modBus->setBaudRate(baud);

	modbusRequestAndResponseStatusValues result = modbusRequestAndResponseStatusValues::preProcessing;
	modbusRequestAndResponse response;
#ifdef DEBUG_NO_RS485
	result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else
	result = _registerHandler->readHandledRegister(REG_SAFETY_TEST_RW_GRID_REGULATION, &response);
#endif

#ifdef DEBUG_OVER_SERIAL
	logHeap("after RS485 probe");
#endif

	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		rs485LockedBaud = baud;
		// Always re-read the live serial after a successful probe. Persisted identity is only a hint,
		// and reconnects must be able to detect an inverter replacement without manual NVS cleanup.
		rs485ConnectState = Rs485ConnectState::ReadingIdentity;
		rs485AttemptsInCycle = 0;
		rs485CycleBackoffMs = kRs485ProbeAttemptDelayMs;
		rs485NextAttemptAtMs = now;
#ifdef DEBUG_OVER_SERIAL
		snprintf(_debugOutput, sizeof(_debugOutput), "RS485 baud established: %lu", baud);
		Serial.println(_debugOutput);
#endif
		return;
	}

#ifdef DEBUG_OVER_SERIAL
	snprintf(_debugOutput, sizeof(_debugOutput), "Baud Rate Checker Problem: %s", response.statusMqttMessage);
	Serial.println(_debugOutput);
#endif
	rs485Errors++;
	updateOLED(false, "Test Baud", baudRateString, response.displayMessage);

	rs485AttemptsInCycle++;
	if (rs485AttemptsInCycle >= (sizeof(kKnownBaudRates) / sizeof(kKnownBaudRates[0]))) {
		rs485AttemptsInCycle = 0;
		rs485NextAttemptAtMs = now + rs485CycleBackoffMs;
		rs485CycleBackoffMs = rs485NextBackoffMs(rs485CycleBackoffMs, kRs485ProbeMaxBackoffMs);
	} else {
		rs485NextAttemptAtMs = now + kRs485ProbeAttemptDelayMs;
	}
}

class PreferencesBootStore : public RebootRequestStore {
public:
	void writeBootIntent(BootIntent intent) override
	{
		persistUserBootIntent(intent);
	}

	void writeBootMode(BootMode mode) override
	{
		persistUserBootMode(mode);
	}
};

PreferencesBootStore rebootStore;

void
triggerRestart(void)
{
	ESP.restart();
}

void
setupHttpControlPlane(void)
{
	if (currentBootMode != BootMode::Normal) {
		httpControlPlaneEnabled = false;
#ifdef DEBUG_OVER_SERIAL
		Serial.println("HTTP control plane disabled (boot_mode != normal).");
#endif
		return;
	}

	httpServer.on("/", HTTP_GET, handleHttpRoot);
	httpServer.on("/restart", HTTP_GET, handleHttpRestartAlias);
	httpServer.on("/restart/", HTTP_GET, handleHttpRestartAlias);
	httpServer.on("/reboot/normal", HTTP_POST, handleRebootNormal);
	httpServer.on("/reboot/ap", HTTP_POST, handleRebootAp);
	httpServer.on("/reboot/wifi", HTTP_POST, handleRebootWifi);
	httpServer.begin();
	httpControlPlaneEnabled = true;
#ifdef DEBUG_OVER_SERIAL
	Serial.println("HTTP control plane started on port 80.");
#endif
}

void
handleHttpRoot(void)
{
#ifdef DEBUG_OVER_SERIAL
	Serial.println("HTTP GET /");
#endif
	httpServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
	httpServer.sendHeader("Connection", "close");
	httpServer.send(200, "text/html", "");

	const IPAddress ip = WiFi.localIP();
	const wl_status_t wifiStatus = WiFi.status();
	const unsigned long rs485ErrorCount = static_cast<unsigned long>(rs485Errors);
#if RS485_STUB
	const char *rs485Backend = "stub";
#else
	const char *rs485Backend = "real";
#endif
	char buf[256];

	httpServer.sendContent("<!doctype html><html><body>");
	httpServer.sendContent("<h3>Alpha2MQTT Control</h3>");
	snprintf(buf, sizeof(buf), "<p>Boot mode: %s<br>Boot intent: %s<br>Reset reason: %s</p>",
	         bootModeToString(currentBootMode),
	         bootIntentToString(currentBootIntent),
	         lastResetReason);
	httpServer.sendContent(buf);

	httpServer.sendContent("<form method='POST' action='/reboot/normal'><button>Reboot Normal</button></form>");
	httpServer.sendContent("<form method='POST' action='/reboot/ap'><button>Reboot AP Config</button></form>");
	httpServer.sendContent("<form method='POST' action='/reboot/wifi'><button>Reboot WiFi Config</button></form>");

	httpServer.sendContent("<h4>Status</h4><p>");
	snprintf(buf, sizeof(buf),
	         "Firmware version: %s<br>RS485 backend: %s<br>"
	         "Uptime (ms): %lu<br>WiFi status: %d<br>RSSI (dBm): %d<br>IP: %u.%u.%u.%u",
	         _version,
	         rs485Backend,
	         static_cast<unsigned long>(millis()),
	         static_cast<int>(wifiStatus),
	         WiFi.RSSI(),
	         ip[0], ip[1], ip[2], ip[3]);
	httpServer.sendContent(buf);
	snprintf(buf, sizeof(buf),
	         "<br>MQTT connected: %u<br>MQTT reconnects: %lu"
	         "<br>Inverter ready: %u<br>RS485 state: %u<br>RS485 errors: %lu",
	         _mqtt.connected() ? 1U : 0U,
	         static_cast<unsigned long>(mqttReconnectCount),
	         inverterReady ? 1U : 0U,
	         static_cast<unsigned>(rs485ConnectState),
	         rs485ErrorCount);
	httpServer.sendContent(buf);
	snprintf(buf, sizeof(buf),
	         "<br>Poll ok: %lu<br>Poll err: %lu<br>Last poll ms: %lu"
	         "<br>ESS snapshot ok: %u<br>ESS snapshot attempts: %lu<br>poll_interval_s: %lu",
	         static_cast<unsigned long>(pollOkCount),
	         static_cast<unsigned long>(pollErrCount),
	         static_cast<unsigned long>(lastPollMs),
	         essSnapshotLastOk ? 1U : 0U,
	         static_cast<unsigned long>(essSnapshotAttemptCount),
	         static_cast<unsigned long>(pollIntervalSeconds));
	httpServer.sendContent(buf);
#if defined(MP_ESP8266)
	snprintf(buf, sizeof(buf),
	         "<br>Heap free/max/frag: %u/%u/%u",
	         ESP.getFreeHeap(),
	         ESP.getMaxFreeBlockSize(),
	         ESP.getHeapFragmentation());
#else
	snprintf(buf, sizeof(buf), "<br>Heap free: %u", ESP.getFreeHeap());
#endif
	httpServer.sendContent(buf);
	httpServer.sendContent("</p></body></html>");
	httpServer.sendContent("");
}

void
handleHttpRestartAlias(void)
{
#ifdef DEBUG_OVER_SERIAL
	Serial.println("HTTP GET /restart -> /");
#endif
	httpServer.sendHeader("Cache-Control", "no-store");
	httpServer.sendHeader("Location", "/", true);
	httpServer.send(302, "text/plain", "");
}

void
handleRebootNormal(void)
{
#ifdef DEBUG_OVER_SERIAL
	Serial.println("HTTP POST /reboot/normal");
#endif
	httpServer.send(200, "text/plain", "Rebooting into MODE_NORMAL...");
	requestReboot(rebootStore, BootMode::Normal, BootIntent::Normal, triggerRestart);
}

void
handleRebootAp(void)
{
#ifdef DEBUG_OVER_SERIAL
	Serial.println("HTTP POST /reboot/ap");
#endif
	httpServer.send(200, "text/plain", "Rebooting into MODE_AP_CONFIG...");
	requestReboot(rebootStore, BootMode::ApConfig, BootIntent::ApConfig, triggerRestart);
}

void
handleRebootWifi(void)
{
#ifdef DEBUG_OVER_SERIAL
	Serial.println("HTTP POST /reboot/wifi");
#endif

	// Serve a small "rebooting" page that polls the same host until the portal comes up.
	// In STA-only portal mode there is no captive-portal DNS redirect, so this provides
	// a reasonable browser UX when the mode switch is initiated from the control plane.
	httpServer.send(200, "text/html",
		"<!doctype html><html><head>"
		"<meta charset='utf-8'>"
		"<meta name='viewport' content='width=device-width,initial-scale=1'>"
		"<title>Rebooting…</title>"
		"</head><body>"
		"<h3>Rebooting into Wi-Fi config…</h3>"
		"<p>This page will auto-redirect when the portal is available.</p>"
		"<pre id='s'>waiting…</pre>"
		"<script>"
		"(function(){"
		"var start=Date.now();"
		"function tick(){"
		"document.getElementById('s').textContent='waiting '+Math.floor((Date.now()-start)/1000)+'s';"
		"}"
		"async function probe(){"
		"try{"
		"var r=await fetch('/',{cache:'no-store'});"
		"if(r && r.ok){window.location.href='/';return;}"
		"}catch(e){}"
		"tick();"
		"setTimeout(probe,1000);"
		"}"
		"setTimeout(probe,1000);"
		"})();"
		"</script>"
		"</body></html>");

	// Give the HTTP response a moment to flush before restarting.
	diagDelay(100);
	requestReboot(rebootStore, BootMode::WifiConfig, BootIntent::WifiConfig, triggerRestart);
}

void
subscribeInverterTopics(void)
{
	static uint32_t lastSubscribeAttemptMs = 0;

	if (!inverterReady || !inverterSerialKnown() || !_mqtt.connected()) {
		return;
	}
	if (!mqttEntitiesRtAvailable()) {
		return;
	}
	const uint32_t nowMs = millis();
	if (inverterSubscriptionsSet && ((nowMs - lastSubscribeAttemptMs) < 5000U)) {
		return;
	}
	lastSubscribeAttemptMs = nowMs;

	char subscriptionDef[100];
	// Reassert a single wildcard subscription instead of dozens of per-entity subscriptions.
	// This is both cheaper and more robust across identity refreshes and reconnects.
	snprintf(subscriptionDef, sizeof(subscriptionDef), "%s/+/+/command", deviceName);
	const bool subscribed = _mqtt.subscribe(subscriptionDef, MQTT_SUBSCRIBE_QOS);
#ifdef DEBUG_OVER_SERIAL
	snprintf(_debugOutput, sizeof(_debugOutput), "Subscribed to \"%s\" : %d", subscriptionDef, subscribed);
	Serial.println(_debugOutput);
#endif

	inverterSubscriptionsSet = subscribed;
}

void
publishStatusNow(void)
{
	if (!_mqtt.connected()) {
		return;
	}
	// Status-on-connect must not bypass ESS snapshot prerequisite. Publish liveness/net/poll only.
	sendStatus(false);
}

void
publishEvent(MqttEventCode code, const char *detail)
{
	if (code == MqttEventCode::None) {
		return;
	}
	uint8_t index = static_cast<uint8_t>(code);
	if (index >= static_cast<uint8_t>(MqttEventCode::MaxValue)) {
		return;
	}
	eventCounts[index]++;

	if (!_mqtt.connected()) {
		return;
	}
	if (!eventLimiter.shouldPublish(code, millis(), kEventRateLimitMs)) {
		return;
	}

	char topic[128];
	char payload[192];
	snprintf(topic, sizeof(topic), "%s/event", deviceName);
	if (detail && detail[0] != '\0') {
		snprintf(payload, sizeof(payload),
			 "{ \"code\": %d, \"name\": \"%s\", \"count\": %lu, \"ts_ms\": %lu, \"detail\": \"%s\" }",
			 static_cast<int>(code), eventCodeName(code),
			 static_cast<unsigned long>(eventCounts[index]),
			 static_cast<unsigned long>(millis()), detail);
	} else {
		snprintf(payload, sizeof(payload),
			 "{ \"code\": %d, \"name\": \"%s\", \"count\": %lu, \"ts_ms\": %lu }",
			 static_cast<int>(code), eventCodeName(code),
			 static_cast<unsigned long>(eventCounts[index]),
			 static_cast<unsigned long>(millis()));
	}
	_mqtt.publish(topic, payload, false);
}

MqttEventCode
eventCodeFromResult(modbusRequestAndResponseStatusValues result)
{
	switch (result) {
	case modbusRequestAndResponseStatusValues::noResponse:
		return MqttEventCode::Rs485Timeout;
	case modbusRequestAndResponseStatusValues::invalidFrame:
	case modbusRequestAndResponseStatusValues::responseTooShort:
	case modbusRequestAndResponseStatusValues::slaveError:
		return MqttEventCode::ModbusFrame;
	default:
		return MqttEventCode::None;
	}
}

void
noteRs485Error(modbusRequestAndResponseStatusValues result, const char *detail)
{
	MqttEventCode code = eventCodeFromResult(result);
	if (code == MqttEventCode::None) {
		return;
	}
	lastErrCode = static_cast<int>(code);
	publishEvent(code, detail);
}

static void
persistUserBootIntent(BootIntent intent)
{
	Preferences preferences;
	preferences.begin(DEVICE_NAME, false);
	preferences.putString(kPreferenceBootIntent, bootIntentToString(intent));
	preferences.end();
}

static void
persistUserBootMode(BootMode mode)
{
	Preferences preferences;
	preferences.begin(DEVICE_NAME, false);
	preferences.putString(kPreferenceBootMode, bootModeToString(mode));
	preferences.end();
}

static void
persistUserMqttConfig(const char *server, int port, const char *user, const char *pass)
{
	Preferences preferences;
	preferences.begin(DEVICE_NAME, false);
	preferences.putString("MQTT_Server", server);
	preferences.putInt("MQTT_Port", port);
	preferences.putString("MQTT_Username", user);
	preferences.putString("MQTT_Password", pass);
	preferences.end();
}

static void
persistUserWifiCredentials(const char *ssid, const char *pass)
{
	Preferences preferences;
	preferences.begin(DEVICE_NAME, false);
	preferences.putString("WiFi_SSID", ssid);
	preferences.putString("WiFi_Password", pass);
	preferences.end();
}

static void
persistUserExtAntenna(bool enabled)
{
	Preferences preferences;
	preferences.begin(DEVICE_NAME, false);
	preferences.putBool("Ext_Antenna", enabled);
	preferences.end();
}

static bool
persistUserPollingConfig(uint32_t intervalSeconds, const char *bucketMap)
{
	Preferences preferences;
	if (!preferences.begin(DEVICE_NAME, false)) {
		return false;
	}

	const char *safeBucketMap = (bucketMap != nullptr) ? bucketMap : "";
	const size_t bucketMapLen = strlen(safeBucketMap);
	bool ok = preferences.putUInt(kPreferencePollInterval, intervalSeconds) == sizeof(uint32_t);
	if (ok) {
		// Polling config updates run from constrained HTTP/MQTT callback stacks on ESP8266.
		// Verify writes by return value rather than allocating another full bucket-map buffer here.
		if (bucketMapLen == 0) {
			ok = !preferences.isKey(kPreferenceBucketMap) || preferences.remove(kPreferenceBucketMap) || !preferences.isKey(kPreferenceBucketMap);
		} else {
			ok = preferences.putString(kPreferenceBucketMap, safeBucketMap) == bucketMapLen;
		}
	}
	// Writing the stable Bucket_Map makes it the authoritative config source.
	// Mark migration complete so stale legacy Freq_* keys no longer reapply on boot.
	if (ok) {
		ok = preferences.putBool(kPreferenceBucketMapMigrated, true) == sizeof(uint8_t);
	}
	preferences.end();
	return ok;
}

static void
persistUserPollingLastChange(const char *lastChange)
{
	if (lastChange == nullptr || *lastChange == '\0') {
		return;
	}

	Preferences preferences;
	char stored[kPrefPollingLastChangeMaxLen] = "";
	preferences.begin(DEVICE_NAME, false);
	preferences.getString(kPreferencePollingLastChange, stored, sizeof(stored));
	if (strcmp(stored, lastChange) != 0) {
		preferences.putString(kPreferencePollingLastChange, lastChange);
	}
	preferences.end();
}

static void
persistUserDeviceSerial(const char *serial)
{
	if (serial == nullptr || *serial == '\0') {
		return;
	}

	Preferences preferences;
	char stored[kPrefDeviceSerialMaxLen] = "";
	preferences.begin(DEVICE_NAME, false);
	preferences.getString(kPreferenceDeviceSerial, stored, sizeof(stored));
	if (strcmp(stored, serial) != 0) {
		preferences.putString(kPreferenceDeviceSerial, serial);
	}
	preferences.end();
}

static void
persistDefaultsIfMissing(void)
{
	Preferences preferences;
	preferences.begin(DEVICE_NAME, false);
	if (!preferences.isKey(kPreferencePollInterval)) {
		preferences.putUInt(kPreferencePollInterval, kPollIntervalDefaultSeconds);
	} else {
		const uint32_t stored = preferences.getUInt(kPreferencePollInterval, kPollIntervalDefaultSeconds);
		const uint32_t clamped = clampPollInterval(stored);
		if (clamped != stored) {
			preferences.putUInt(kPreferencePollInterval, clamped);
		}
	}
	preferences.end();
}

void
setBootIntentAndReboot(BootIntent intent, bool persistIntent)
{
	if (persistIntent) {
		// ESP8266 doesn't provide reset intent; persist requested reboot reason for next boot.
		persistUserBootIntent(intent);
	}
	ESP.restart();
}

#if defined(MP_ESP8266)
using PortalServer = ESP8266WebServer;
#endif

#if defined(MP_ESP8266)
static const char *
httpMethodToString(HTTPMethod method)
{
	switch (method) {
	case HTTP_GET:
		return "GET";
	case HTTP_POST:
		return "POST";
	case HTTP_PUT:
		return "PUT";
	case HTTP_DELETE:
		return "DELETE";
	case HTTP_PATCH:
		return "PATCH";
	case HTTP_OPTIONS:
		return "OPTIONS";
	default:
		return "OTHER";
	}
}
#endif

#ifdef DEBUG_OVER_SERIAL
static void
portalLog(const char *format, ...)
{
	(void)format;
}
#endif

#if defined(MP_ESP8266)
class PortalRequestLogger : public RequestHandler {
public:
	bool canHandle(HTTPMethod method, const String& uri) override
	{
#ifdef DEBUG_OVER_SERIAL
		portalLog("HTTP %s %s free=%u max=%u frag=%u",
			httpMethodToString(method),
			uri.c_str(),
			ESP.getFreeHeap(),
			ESP.getMaxFreeBlockSize(),
			ESP.getHeapFragmentation());
#endif
		return false;
	}

	bool canUpload(const String& uri) override
	{
		(void)uri;
		return false;
	}

	bool handle(PortalServer &server, HTTPMethod requestMethod, const String& requestUri) override
	{
		(void)server;
		(void)requestMethod;
		(void)requestUri;
		return false;
	}
};
#endif

const char*
portalStatusLabel(PortalStatus status)
{
	switch (status) {
	case portalStatusConnecting:
		return "Connecting";
	case portalStatusSuccess:
		return "Connected";
	case portalStatusFailed:
		return "Failed";
	case portalStatusIdle:
	default:
		return "Idle";
	}
}

const char*
wifiStatusReason(wl_status_t status)
{
	switch (status) {
	case WL_NO_SSID_AVAIL:
		return "SSID not found";
	case WL_CONNECT_FAILED:
		return "Connection failed";
	case WL_CONNECTION_LOST:
		return "Connection lost";
	case WL_DISCONNECTED:
		return "Disconnected";
	case WL_IDLE_STATUS:
		return "Idle";
	default:
		return "Unknown";
	}
}

const char*
wifiStatusLabel(wl_status_t status)
{
	switch (status) {
	case WL_CONNECTED:
		return "Connected";
	case WL_NO_SSID_AVAIL:
		return "No SSID";
	case WL_CONNECT_FAILED:
		return "Connect failed";
	case WL_CONNECTION_LOST:
		return "Connection lost";
	case WL_DISCONNECTED:
		return "Disconnected";
	case WL_IDLE_STATUS:
		return "Idle";
	default:
		return "Unknown";
	}
}

const char*
wifiModeLabel(WiFiMode_t mode)
{
	switch (mode) {
	case WIFI_STA:
		return "STA";
	case WIFI_AP:
		return "AP";
	case WIFI_AP_STA:
		return "AP+STA";
	case WIFI_OFF:
		return "OFF";
	default:
		return "Unknown";
	}
}

void
handlePortalStatusRequest(WiFiManager& wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	// Keep heap usage low in the portal: stream HTML in small chunks rather than building a large String.
	// This avoids crashes when heap is fragmented after WiFiManager activity.
	wifiManager.server->setContentLength(CONTENT_LENGTH_UNKNOWN);
	wifiManager.server->send(200, "text/html", "");

	char buf[256];
	wifiManager.server->sendContent("<!DOCTYPE html><html><head>");
	wifiManager.server->sendContent("<meta charset=\"utf-8\">");
	wifiManager.server->sendContent("<meta http-equiv=\"refresh\" content=\"1\">");
	wifiManager.server->sendContent("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
	wifiManager.server->sendContent("<title>Alpha2MQTT WiFi Status</title>");
	wifiManager.server->sendContent("</head><body>");

	snprintf(buf, sizeof(buf), "<h2>WiFi Status: %s</h2>", portalStatusLabel(portalStatus));
	wifiManager.server->sendContent(buf);

	if (portalStatus == portalStatusSuccess) {
		snprintf(buf, sizeof(buf), "<p>SSID: %s<br>IP: %s</p>", portalStatusSsid, portalStatusIp);
		wifiManager.server->sendContent(buf);
		if (portalNeedsMqttConfig) {
			wifiManager.server->sendContent("<p><strong>MQTT settings not set.</strong> Redirecting to MQTT settings...</p>");
			wifiManager.server->sendContent("<p><a href=\"/param\">Open MQTT settings</a></p>");
			wifiManager.server->sendContent("<script>setTimeout(function(){window.location.href='/param';},500);</script>");
		}
	} else if (portalStatus == portalStatusFailed) {
		snprintf(buf, sizeof(buf), "<p>Reason: %s</p>", portalStatusReason);
		wifiManager.server->sendContent(buf);
	} else {
		wifiManager.server->sendContent("<p>Attempting to connect...</p>");
	}

	wifiManager.server->sendContent("<h3>Diagnostics</h3><p>");
	snprintf(buf, sizeof(buf), "Mode: %s", wifiModeLabel(WiFi.getMode()));
	wifiManager.server->sendContent(buf);

	// Only show SoftAP info when it is actually enabled; MODE_WIFI_CONFIG uses STA-only portal.
	WiFiMode_t mode = WiFi.getMode();
	if (mode == WIFI_AP || mode == WIFI_AP_STA) {
		IPAddress apIp = WiFi.softAPIP();
		snprintf(buf, sizeof(buf), "<br>SoftAP SSID: %s<br>SoftAP IP: %u.%u.%u.%u",
			 deviceName, apIp[0], apIp[1], apIp[2], apIp[3]);
		wifiManager.server->sendContent(buf);
	} else {
		wifiManager.server->sendContent("<br>SoftAP: disabled");
	}

	wl_status_t staStatus = WiFi.status();
	snprintf(buf, sizeof(buf), "<br>STA status: %s (%d)", wifiStatusLabel(staStatus), static_cast<int>(staStatus));
	wifiManager.server->sendContent(buf);

	snprintf(buf, sizeof(buf), "<br>Target SSID: %s", portalStatusSsid);
	wifiManager.server->sendContent(buf);

	snprintf(buf, sizeof(buf), "<br>Boot intent: %s<br>Boot mode: %s",
		 bootIntentToString(currentBootIntent),
		 bootModeToString(bootModeForDiagnostics));
	wifiManager.server->sendContent(buf);

	snprintf(buf, sizeof(buf), "<br>Reset reason: %s", lastResetReason);
	wifiManager.server->sendContent(buf);

	snprintf(buf, sizeof(buf), "<br>Last disconnect: %s (%d)", portalLastDisconnectLabel, portalLastDisconnectReason);
	wifiManager.server->sendContent(buf);

	if (staStatus == WL_CONNECTED) {
		snprintf(buf, sizeof(buf), "<br>RSSI: %d dBm<br>Channel: %d", WiFi.RSSI(), WiFi.channel());
		wifiManager.server->sendContent(buf);
	}

#if defined(MP_ESP8266)
	snprintf(buf, sizeof(buf), "<br>Heap: free=%u max=%u frag=%u",
		 ESP.getFreeHeap(), ESP.getMaxFreeBlockSize(), ESP.getHeapFragmentation());
#else
	snprintf(buf, sizeof(buf), "<br>Heap free=%u", ESP.getFreeHeap());
#endif
	wifiManager.server->sendContent(buf);

	snprintf(buf, sizeof(buf), "<br>Uptime (ms): %lu</p>", static_cast<unsigned long>(millis()));
	wifiManager.server->sendContent(buf);
	wifiManager.server->sendContent("<form method=\"POST\" action=\"/reboot/normal\"><button type=\"submit\">Reboot Normal</button></form>");
	wifiManager.server->sendContent("<p>Page refreshes every second.</p></body></html>");
	wifiManager.server->sendContent("");
}

void
handlePortalRebootNormalRequest(WiFiManager& wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	static const char kPortalRebootNormalPage[] PROGMEM =
		"<!doctype html><html><head>"
		"<meta charset='utf-8'>"
		"<meta name='viewport' content='width=device-width,initial-scale=1'>"
		"<title>Rebooting…</title>"
		"</head><body>"
		"<h3>Rebooting into normal runtime…</h3>"
		"<p>This page will auto-redirect when runtime mode is available.</p>"
		"<pre id='s'>waiting…</pre>"
		"<script>"
		"(function(){"
		"var start=Date.now();"
		"function tick(){"
		"document.getElementById('s').textContent='waiting '+Math.floor((Date.now()-start)/1000)+'s';"
		"}"
		"async function probe(){"
		"try{"
		"var r=await fetch('/',{cache:'no-store'});"
		"if(r&&r.ok){"
		"var t=await r.text();"
		"if(t&&t.indexOf('Alpha2MQTT Control')>=0){window.location.href='/';return;}"
		"}"
		"}catch(e){}"
		"tick();"
		"setTimeout(probe,1000);"
		"}"
		"setTimeout(probe,1000);"
		"})();"
		"</script>"
		"</body></html>";

	wifiManager.server->setContentLength(CONTENT_LENGTH_UNKNOWN);
	wifiManager.server->sendHeader("Connection", "close");
	wifiManager.server->send(200, "text/html", "");
	wifiManager.server->sendContent_P(kPortalRebootNormalPage, strlen_P(kPortalRebootNormalPage));
#if defined(MP_ESP8266)
	ESP.wdtFeed();
#endif
	wifiManager.server->sendContent("");
	portalNeedsMqttConfig = false;
	portalRebootScheduled = true;
	portalRebootAt = millis() + 1500;
}

static constexpr uint8_t kPollingPortalPageSize = 8;
static constexpr BucketId kPortalEstimateBuckets[] = {
	BucketId::TenSec,
	BucketId::OneMin,
	BucketId::FiveMin,
	BucketId::OneHour,
	BucketId::OneDay,
	BucketId::User,
};

static void
portalLogHeap(const char *label)
{
	(void)label;
}

static bool
loadPollingBucketsForPortal(const mqttState *entities,
                            size_t entityCount,
                            BucketId *outBuckets,
                            uint32_t &outPollIntervalSeconds)
{
	if (!entities || !outBuckets || entityCount == 0 || entityCount > kMqttEntityDescriptorCount) {
		return false;
	}

	outPollIntervalSeconds = clampPollInterval(pollIntervalSeconds);
	for (size_t i = 0; i < entityCount; ++i) {
		outBuckets[i] = bucketIdFromFreq(entities[i].updateFreq);
	}

	if (!mqttEntitiesRtAvailable()) {
		return true;
	}

	return mqttEntityCopyBuckets(outBuckets, entityCount);
}

static uint16_t
portalArgToU16(const String &arg, uint16_t defaultValue)
{
	if (arg.length() == 0) {
		return defaultValue;
	}
	char *endPtr = nullptr;
	errno = 0;
	unsigned long parsed = strtoul(arg.c_str(), &endPtr, 10);
	if (errno != 0 || endPtr == arg.c_str()) {
		return defaultValue;
	}
	return static_cast<uint16_t>(parsed);
}

const char *
portalCustomHeadScript(void)
{
	return
		"<script>"
		"(function(){"
		"var p=(window.location&&window.location.pathname)||'';"
		"if (p === '/wifisave') {"
		"window.location.href='/status';"
		"return;"
		"}"
		"if (p === '/restart' || p === '/restart/') {"
		"function probe(){"
		"fetch('/',{cache:'no-store'}).then(function(r){"
		"if (r && r.ok) { window.location.href='/'; return; }"
		"setTimeout(probe,1000);"
		"}).catch(function(){setTimeout(probe,1000);});"
		"}"
		"setTimeout(probe,300);"
		"}"
		"if (p === '/0wifi') {"
		"window.addEventListener('DOMContentLoaded', function(){"
		"var nodes=document.querySelectorAll(\"form[action^='/wifi?refresh=1']\");"
		"for (var i=0;i<nodes.length;i++){nodes[i].remove();}"
		"});"
		"}"
		"})();"
		"</script>";
}

static inline void
portalSendContentAndFeed(WiFiManager &wifiManager, const char *content)
{
	if (!wifiManager.server || content == nullptr) {
		return;
	}
	wifiManager.server->sendContent(content);
#if defined(MP_ESP8266)
	ESP.wdtFeed();
#endif
}

static inline void
portalSendContentPAndFeed(WiFiManager &wifiManager, PGM_P content)
{
	if (!wifiManager.server || content == nullptr) {
		return;
	}
	wifiManager.server->sendContent_P(content, strlen_P(content));
#if defined(MP_ESP8266)
	ESP.wdtFeed();
#endif
}

static void
handlePortalPollingPage(WiFiManager &wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	const mqttState *entities = mqttEntitiesDesc();
	const size_t entityCount = mqttEntitiesCount();
	BucketId *buckets = g_portalBucketsScratch;
	uint32_t storedIntervalSeconds = kPollIntervalDefaultSeconds;
	if (!loadPollingBucketsForPortal(entities, entityCount, buckets, storedIntervalSeconds)) {
		wifiManager.server->send(500, "text/plain", "polling config unavailable");
		return;
	}

	const String familyArg = wifiManager.server->arg("family");
	const MqttEntityFamily family = portalNormalizePollingFamily(entities, entityCount, familyArg.c_str());
	const char *familyKey = portalPollingFamilyKey(family);
	const char *familyLabel = portalPollingFamilyLabel(family);
	const uint16_t requestedPage = portalArgToU16(wifiManager.server->arg("page"), 0);
	const PortalFamilyPage familyPage = portalBuildFamilyPage(entities, entityCount, family, requestedPage, kPollingPortalPageSize);
	uint16_t visibleIndices[kPollingPortalPageSize] = {};
	const size_t visibleCount = portalCollectFamilyPageEntityIndices(entities,
	                                                                entityCount,
	                                                                familyPage,
	                                                                visibleIndices,
	                                                                kPollingPortalPageSize);

	const bool saved = wifiManager.server->hasArg("saved") && wifiManager.server->arg("saved") == "1";
	const bool err = wifiManager.server->hasArg("err") && wifiManager.server->arg("err") == "1";

	static const char kPageHeadA[] PROGMEM =
		"<!DOCTYPE html><html><head>"
		"<meta charset=\"utf-8\">"
		"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
		"<title>Polling Schedule</title>"
		"<style>"
		".c,body{text-align:center;font-family:verdana}"
		"div,input,select{padding:5px;font-size:1em;margin:5px 0;box-sizing:border-box}"
		"input,button,select,.msg{border-radius:.3rem;width:100%}"
		"button{cursor:pointer;border:0;background-color:#1fa3ec;color:#fff;line-height:2.2rem;font-size:1rem}"
		".wrap{text-align:left;display:inline-block;min-width:260px;max-width:500px}"
		".msg{padding:10px;margin:10px 0;border:1px solid #eee;border-left-width:5px;border-left-color:#777}"
		".msg.S{border-left-color:#5cb85c}"
		".msg.D{border-left-color:#dc3630}"
		"table{width:100%;border-collapse:collapse}"
		"th,td{padding:4px;border:1px solid #ddd;vertical-align:top}"
		".row{display:flex;gap:8px;flex-wrap:wrap}"
		".row form{flex:1;min-width:130px}"
		".hint{font-size:.95em;color:#444}"
		"</style>";
	static const char kPageHeadB[] PROGMEM =
		"<script>"
		"window._d=0;"
		"window._pk='a2m_polling_draft_v1';"
		"function pRead(){"
		"try{var raw=sessionStorage.getItem(window._pk);return raw?JSON.parse(raw):{entities:{}};}"
		"catch(e){sessionStorage.removeItem(window._pk);return {entities:{}};}"
		"}"
		"function pWrite(v){"
		"if(!v||!v.entities){sessionStorage.removeItem(window._pk);return;}"
		"var keys=Object.keys(v.entities);"
		"if(!keys.length&&(!v.poll_interval_s||v.poll_interval_s==='')){sessionStorage.removeItem(window._pk);return;}"
		"sessionStorage.setItem(window._pk,JSON.stringify(v));"
		"}"
		"function pCapture(){"
		"var form=document.getElementById('polling-form');if(!form){return;}"
		"var draft=pRead();draft.entities=draft.entities||{};"
		"var pi=form.elements['poll_interval_s'];if(pi){draft.poll_interval_s=pi.value;}"
		"var rows=document.querySelectorAll('tr[data-entity]');"
		"for(var i=0;i<rows.length;i++){"
		"var row=rows[i],entity=row.getAttribute('data-entity');"
		"var sel=row.querySelector('select[name^=\"b\"]');"
		"if(entity&&sel){draft.entities[entity]=sel.value;}"
		"}"
		"pWrite(draft);"
		"}"
		"function pRestore(){"
		"if(window.location.search.indexOf('saved=1')>=0){sessionStorage.removeItem(window._pk);return;}"
		"var form=document.getElementById('polling-form');if(!form){return;}"
		"var draft=pRead();var hasDraft=false;"
		"var pi=form.elements['poll_interval_s'];"
		"if(pi&&draft.poll_interval_s!==undefined){pi.value=draft.poll_interval_s;hasDraft=true;}"
		"var rows=document.querySelectorAll('tr[data-entity]');"
		"for(var i=0;i<rows.length;i++){"
		"var row=rows[i],entity=row.getAttribute('data-entity');"
		"var sel=row.querySelector('select[name^=\"b\"]');"
		"if(entity&&sel&&draft.entities&&draft.entities[entity]!==undefined){sel.value=draft.entities[entity];hasDraft=true;}"
		"}"
		"if(hasDraft){window._d=1;}"
		"}"
		"function pPrepareSave(){"
		"var form=document.getElementById('polling-form');if(!form){return true;}"
		"var draft=pRead();draft.entities=draft.entities||{};"
		"var pi=form.elements['poll_interval_s'];if(pi){draft.poll_interval_s=pi.value;}"
		"var rows=document.querySelectorAll('tr[data-entity]');"
		"for(var i=0;i<rows.length;i++){"
		"var row=rows[i],entity=row.getAttribute('data-entity');"
		"var sel=row.querySelector('select[name^=\"b\"]');"
		"if(entity&&sel){draft.entities[entity]=sel.value;}"
		"}"
		"var keys=Object.keys(draft.entities).sort();"
		"var parts=[];"
		"for(var i=0;i<keys.length;i++){parts.push(keys[i]+'='+draft.entities[keys[i]]);}"
		"var full=form.elements['bucket_map_full'];"
		"if(full){full.value=parts.length?parts.join(';')+';':'';}"
		"sessionStorage.removeItem(window._pk);"
		"window._d=0;"
		"return true;"
		"}"
		"function pDirty(){window._d=1;}"
		"function pNav(){"
		"if(!window._d){return true;}"
		"if(!window.confirm('Unsaved polling changes will be lost. Continue?')){return false;}"
		"pCapture();"
		"return true;"
		"}"
		"window.addEventListener('DOMContentLoaded',pRestore);"
		"</script>";
	static const char kPageHeadC[] PROGMEM =
		"</head><body class=\"c\"><div class=\"wrap\">"
		"<h1>Setup</h1><h3>Polling schedule</h3>"
		"<div class=\"row\">"
		"<form data-nav-away=\"1\" action=\"/\" method=\"get\" onsubmit=\"return pNav()\"><button type=\"submit\">Menu</button></form>"
		"<form data-nav-away=\"1\" action=\"/param\" method=\"get\" onsubmit=\"return pNav()\"><button type=\"submit\">MQTT Setup</button></form>"
		"<form data-nav-away=\"1\" action=\"/reboot/normal\" method=\"post\" onsubmit=\"return pNav()\"><button type=\"submit\">Reboot Normal</button></form>"
		"</div>";
	static const char kSavedMsg[] PROGMEM = "<div class=\"msg S\"><strong>Saved.</strong></div>";
	static const char kErrMsg[] PROGMEM = "<div class=\"msg D\"><strong>Some values were invalid and were ignored.</strong></div>";
	static const char kFormOpen[] PROGMEM = "<form id=\"polling-form\" method=\"POST\" action=\"/config/polling/save\" oninput=\"pDirty()\" onchange=\"pDirty()\" onsubmit=\"return pPrepareSave();\">";
	static const char kTableOpen[] PROGMEM = "<table><tr><th>Entity</th><th>Bucket</th></tr>";
	static const char kPlainTableClose[] PROGMEM = "</table>";
	static const char kTableClose[] PROGMEM = "</table><br><button type=\"submit\">Save</button></form>";
	static const char kNavOpen[] PROGMEM = "<div class=\"row\">";
	static const char kNavClose[] PROGMEM = "</div></div></body></html>";
	static const char kFamilyNavOpen[] PROGMEM = "<div class=\"row\">";
	static const char kFamilyNavClose[] PROGMEM = "</div>";
	static const char kFamilyNavFmt[] PROGMEM =
		"<form data-nav-away=\"1\" action=\"/config/polling\" method=\"get\" onsubmit=\"return pNav()\">"
		"<input type=\"hidden\" name=\"family\" value=\"%s\">"
		"<input type=\"hidden\" name=\"page\" value=\"0\">"
		"<button type=\"submit\"%s>%s</button>"
		"</form>";
	static const char kPageHintFmt[] PROGMEM = "<p class=\"hint\">Family %s · Page %u of %u · %u entities</p>";
	static const char kEstimateIntro[] PROGMEM =
		"<p class=\"hint\">Advisory estimates only. They are based on grouped polling transactions, not strict wire-time guarantees.</p>";
	static const char kEstimateTableOpenFmt[] PROGMEM =
		"<h4>%s</h4><table><tr><th>Bucket</th><th>Entities</th><th>Tx</th><th>~Used ms</th><th>Budget ms</th><th>~Headroom ms</th><th>Risk</th></tr>";
	static const char kEstimateRowFmt[] PROGMEM =
		"<tr><td>%s</td><td>%u</td><td>%u</td><td>%lu</td><td>%lu</td><td>%lu</td><td>%s</td></tr>";
	static const char kFormMetaFmt[] PROGMEM =
		"<input type=\"hidden\" name=\"family\" value=\"%s\">"
		"<input type=\"hidden\" name=\"page\" value=\"%u\">"
		"<input type=\"hidden\" name=\"bucket_map_full\" value=\"\">"
		"<label>poll_interval_s <input name=\"poll_interval_s\" type=\"number\" min=\"1\" max=\"86400\" value=\"%lu\"></label><br><br>";
	static const char kPrevFmt[] PROGMEM =
		"<form data-nav-away=\"1\" action=\"/config/polling\" method=\"get\" onsubmit=\"return pNav()\">"
		"<input type=\"hidden\" name=\"family\" value=\"%s\">"
		"<input type=\"hidden\" name=\"page\" value=\"%u\">"
		"<button type=\"submit\">Prev</button>"
		"</form>";
	static const char kNextFmt[] PROGMEM =
		"<form data-nav-away=\"1\" action=\"/config/polling\" method=\"get\" onsubmit=\"return pNav()\">"
		"<input type=\"hidden\" name=\"family\" value=\"%s\">"
		"<input type=\"hidden\" name=\"page\" value=\"%u\">"
		"<button type=\"submit\">Next</button>"
		"</form>";
	static const char kRowFmt[] PROGMEM =
		"<tr data-entity=\"%s\"><td>%s</td><td><select name=\"b%u\">"
		"<option value=\"%s\"%s>%s</option>"
		"<option value=\"%s\"%s>%s</option>"
		"<option value=\"%s\"%s>%s</option>"
		"<option value=\"%s\"%s>%s</option>"
		"<option value=\"%s\"%s>%s</option>"
		"<option value=\"%s\"%s>%s</option>"
		"<option value=\"%s\"%s>%s</option>"
		"</select></td></tr>";
	wifiManager.server->setContentLength(CONTENT_LENGTH_UNKNOWN);
	wifiManager.server->sendHeader("Connection", "close");
	wifiManager.server->send(200, "text/html", "");

	char buf[256];
	portalSendContentPAndFeed(wifiManager, kPageHeadA);
	portalSendContentPAndFeed(wifiManager, kPageHeadB);
	portalSendContentPAndFeed(wifiManager, kPageHeadC);

	if (saved) {
		portalSendContentPAndFeed(wifiManager, kSavedMsg);
	}
	if (err) {
		portalSendContentPAndFeed(wifiManager, kErrMsg);
	}

	portalSendContentPAndFeed(wifiManager, kFamilyNavOpen);
	for (uint8_t i = 0; i < portalPollingFamilyCount(); ++i) {
		const MqttEntityFamily navFamily = portalPollingFamilyAt(i);
		const PortalFamilyPage navPage = portalBuildFamilyPage(entities, entityCount, navFamily, 0, kPollingPortalPageSize);
		if (navPage.totalEntityCount == 0) {
			continue;
		}
		snprintf_P(buf,
		           sizeof(buf),
		           kFamilyNavFmt,
		           portalPollingFamilyKey(navFamily),
		           (navFamily == family) ? " disabled" : "",
		           portalPollingFamilyLabel(navFamily));
		portalSendContentAndFeed(wifiManager, buf);
	}
	portalSendContentPAndFeed(wifiManager, kFamilyNavClose);

	snprintf_P(buf,
	           sizeof(buf),
	           kPageHintFmt,
	           familyLabel,
	           static_cast<unsigned>(familyPage.safePage + 1),
	           static_cast<unsigned>(familyPage.maxPage + 1),
	           static_cast<unsigned>(familyPage.totalEntityCount));
	portalSendContentAndFeed(wifiManager, buf);
	portalSendContentPAndFeed(wifiManager, kEstimateIntro);

	snprintf_P(buf, sizeof(buf), kEstimateTableOpenFmt, "Current family estimate");
	portalSendContentAndFeed(wifiManager, buf);
	for (BucketId estimateBucket : kPortalEstimateBuckets) {
		const PortalPollingEstimate estimate = portalBuildFamilyPollingEstimate(
			entities, entityCount, buckets, family, estimateBucket, pollIntervalSeconds * 1000UL, kPollOverrunMs);
		if (estimate.entityCount == 0) {
			continue;
		}
		const uint32_t headroomMs = (estimate.budgetMs > estimate.estimatedUsedMs)
			? (estimate.budgetMs - estimate.estimatedUsedMs)
			: 0;
		snprintf_P(buf,
		           sizeof(buf),
		           kEstimateRowFmt,
		           bucketIdToString(estimate.bucketId),
		           static_cast<unsigned>(estimate.entityCount),
		           static_cast<unsigned>(estimate.transactionCount),
		           static_cast<unsigned long>(estimate.estimatedUsedMs),
		           static_cast<unsigned long>(estimate.budgetMs),
		           static_cast<unsigned long>(headroomMs),
		           portalEstimateLevelLabel(estimate.level));
		portalSendContentAndFeed(wifiManager, buf);
	}
	portalSendContentPAndFeed(wifiManager, kPlainTableClose);

	snprintf_P(buf, sizeof(buf), kEstimateTableOpenFmt, "All active buckets");
	portalSendContentAndFeed(wifiManager, buf);
	for (BucketId estimateBucket : kPortalEstimateBuckets) {
		const PortalPollingEstimate estimate = portalBuildPollingEstimate(
			entities, entityCount, buckets, estimateBucket, pollIntervalSeconds * 1000UL, kPollOverrunMs);
		if (estimate.entityCount == 0) {
			continue;
		}
		const uint32_t headroomMs = (estimate.budgetMs > estimate.estimatedUsedMs)
			? (estimate.budgetMs - estimate.estimatedUsedMs)
			: 0;
		snprintf_P(buf,
		           sizeof(buf),
		           kEstimateRowFmt,
		           bucketIdToString(estimate.bucketId),
		           static_cast<unsigned>(estimate.entityCount),
		           static_cast<unsigned>(estimate.transactionCount),
		           static_cast<unsigned long>(estimate.estimatedUsedMs),
		           static_cast<unsigned long>(estimate.budgetMs),
		           static_cast<unsigned long>(headroomMs),
		           portalEstimateLevelLabel(estimate.level));
		portalSendContentAndFeed(wifiManager, buf);
	}
	portalSendContentPAndFeed(wifiManager, kPlainTableClose);

	portalSendContentPAndFeed(wifiManager, kFormOpen);
	snprintf_P(buf, sizeof(buf),
	           kFormMetaFmt,
	           familyKey,
	           static_cast<unsigned>(familyPage.safePage),
	           static_cast<unsigned long>(storedIntervalSeconds));
	portalSendContentAndFeed(wifiManager, buf);

	portalSendContentPAndFeed(wifiManager, kTableOpen);
	for (size_t row = 0; row < visibleCount; ++row) {
		const size_t idx = visibleIndices[row];
		const BucketId cur = buckets[idx];
		char entityName[64];
		mqttEntityNameCopy(&entities[idx], entityName, sizeof(entityName));
		const int rowLen = snprintf_P(
			g_portalRowRenderBuf,
			sizeof(g_portalRowRenderBuf),
			kRowFmt,
			entityName,
			entityName,
			static_cast<unsigned>(row),
			bucketIdToString(BucketId::TenSec), (cur == BucketId::TenSec) ? " selected" : "", "10s",
			bucketIdToString(BucketId::OneMin), (cur == BucketId::OneMin) ? " selected" : "", "1m",
			bucketIdToString(BucketId::FiveMin), (cur == BucketId::FiveMin) ? " selected" : "", "5m",
			bucketIdToString(BucketId::OneHour), (cur == BucketId::OneHour) ? " selected" : "", "1h",
			bucketIdToString(BucketId::OneDay), (cur == BucketId::OneDay) ? " selected" : "", "1d",
			bucketIdToString(BucketId::User), (cur == BucketId::User) ? " selected" : "", "usr",
			bucketIdToString(BucketId::Disabled), (cur == BucketId::Disabled) ? " selected" : "", "off");
		if (rowLen > 0 && static_cast<size_t>(rowLen) < sizeof(g_portalRowRenderBuf)) {
			portalSendContentAndFeed(wifiManager, g_portalRowRenderBuf);
		}
	}
	portalSendContentPAndFeed(wifiManager, kTableClose);

	portalSendContentPAndFeed(wifiManager, kNavOpen);
	if (familyPage.safePage > 0) {
		snprintf_P(buf,
		           sizeof(buf),
		           kPrevFmt,
		           familyKey,
		           static_cast<unsigned>(familyPage.safePage - 1));
		portalSendContentAndFeed(wifiManager, buf);
	}
	if (familyPage.safePage < familyPage.maxPage) {
		snprintf_P(buf,
		           sizeof(buf),
		           kNextFmt,
		           familyKey,
		           static_cast<unsigned>(familyPage.safePage + 1));
		portalSendContentAndFeed(wifiManager, buf);
	}
	portalSendContentPAndFeed(wifiManager, kNavClose);
	portalSendContentAndFeed(wifiManager, "");

}

static void
handlePortalPollingSave(WiFiManager &wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	const mqttState *entities = mqttEntitiesDesc();
	const size_t entityCount = mqttEntitiesCount();
	BucketId *buckets = g_portalBucketsScratch;
	BucketId originalBuckets[kMqttEntityDescriptorCount]{};
	uint32_t storedIntervalSeconds = kPollIntervalDefaultSeconds;
	(void)loadPollingBucketsForPortal(entities, entityCount, buckets, storedIntervalSeconds);
	if (mqttEntitiesRtAvailable()) {
		memcpy(originalBuckets, buckets, sizeof(originalBuckets));
	}

	const String familyArg = wifiManager.server->arg("family");
	const MqttEntityFamily family = portalNormalizePollingFamily(entities, entityCount, familyArg.c_str());
	const char *familyKey = portalPollingFamilyKey(family);
	const uint16_t requestedPage = portalArgToU16(wifiManager.server->arg("page"), 0);
	const PortalFamilyPage familyPage = portalBuildFamilyPage(entities, entityCount, family, requestedPage, kPollingPortalPageSize);
	uint16_t visibleIndices[kPollingPortalPageSize] = {};
	const size_t visibleCount = portalCollectFamilyPageEntityIndices(entities,
	                                                                entityCount,
	                                                                familyPage,
	                                                                visibleIndices,
	                                                                kPollingPortalPageSize);

	bool hadError = false;

	// poll_interval_s
	if (wifiManager.server->hasArg("poll_interval_s")) {
		char *endPtr = nullptr;
		errno = 0;
		unsigned long parsed = strtoul(wifiManager.server->arg("poll_interval_s").c_str(), &endPtr, 10);
		if (errno == 0 && endPtr != nullptr && *endPtr == '\0') {
			storedIntervalSeconds = clampPollInterval(static_cast<uint32_t>(parsed));
		} else {
			hadError = true;
		}
	}

	if (wifiManager.server->hasArg("bucket_map_full")) {
		const String fullMap = wifiManager.server->arg("bucket_map_full");
		if (fullMap.length() >= kPrefBucketMapMaxLen) {
			hadError = true;
		} else {
			strlcpy(g_portalBucketMapScratch, fullMap.c_str(), kPrefBucketMapMaxLen);
			if (g_portalBucketMapScratch[0] != '\0') {
				uint32_t unknownCount = 0;
				uint32_t invalidCount = 0;
				uint32_t duplicateCount = 0;
				if (!applyBucketMapString(g_portalBucketMapScratch,
				                          entities,
				                          entityCount,
				                          buckets,
				                          unknownCount,
				                          invalidCount,
				                          duplicateCount)) {
					hadError = true;
				}
			}
		}
	} else {
		for (size_t row = 0; row < visibleCount; ++row) {
			const size_t idx = visibleIndices[row];
			char argName[8];
			snprintf(argName, sizeof(argName), "b%u", static_cast<unsigned>(row));
			if (!wifiManager.server->hasArg(argName)) {
				continue;
			}
			const String argVal = wifiManager.server->arg(argName);
			BucketId bucket = bucketIdFromString(argVal.c_str());
			if (bucket == BucketId::Unknown) {
				hadError = true;
				continue;
			}
			buckets[idx] = bucket;
		}
	}

	char *outMap = g_portalBucketMapScratch;
	const bool mapBuilt = buildPersistedPollingConfigMap(buckets, outMap, kPrefBucketMapMaxLen);
	if (!mapBuilt) {
		hadError = true;
	}

	const bool bucketsCanApply = !mqttEntitiesRtAvailable() || mqttEntityCanApplyBuckets(buckets, entityCount);
	if (mapBuilt && !bucketsCanApply) {
		hadError = true;
	}

	bool bucketsApplied = false;
	if (mapBuilt && bucketsCanApply) {
		bucketsApplied = !mqttEntitiesRtAvailable() || mqttEntityApplyBuckets(buckets, entityCount);
		if (bucketsApplied && persistUserPollingConfig(storedIntervalSeconds, outMap)) {
			pollIntervalSeconds = storedIntervalSeconds;
			recomputeBucketCounts();
		} else {
			if (bucketsApplied && mqttEntitiesRtAvailable()) {
				mqttEntityApplyBuckets(originalBuckets, entityCount);
			}
			hadError = true;
		}
	} else if (mapBuilt) {
		hadError = true;
	}
	// Polling save is user-driven config edit only. Never auto-reboot from this path.
	portalRebootScheduled = false;
	portalMqttSaved = false;

	// Optional hidden reboot for E2E; not shown in UI.
	if (wifiManager.server->hasArg("reboot") && wifiManager.server->arg("reboot") == "1") {
		setBootIntentAndReboot(BootIntent::Normal);
		return;
	}

	char location[96];
	snprintf(location, sizeof(location), "/config/polling?family=%s&page=%u&saved=1%s",
	         familyKey,
	         static_cast<unsigned>(familyPage.safePage),
	         hadError ? "&err=1" : "");
	wifiManager.server->sendHeader("Location", location);
	wifiManager.server->send(302, "text/plain", "");
}

/*
 * setup
 *
 * The setup function runs once when you press reset or power the board
 */
void setup()
{
#if defined(DEBUG_OVER_SERIAL) || defined(DEBUG_LEVEL2) || defined(DEBUG_OUTPUT_TX_RX)
	// Set up serial for debugging using an appropriate baud rate
	// This is for communication with the development environment, NOT the Alpha system
	// See Definitions.h for this.
	Serial.begin(9600);
#ifdef DEBUG_OVER_SERIAL
	logHeap("very-early");
	diagDelay(100);
	logHeap("boot");
#endif
#endif // DEBUG_OVER_SERIAL || DEBUG_LEVEL2 || DEBUG_OUTPUT_TX_RX

	recordBootMemStage(BootMemStage::Boot0);

		// RS485/inverter probing runs in loop() (background) so NORMAL-mode services (MQTT/http/scheduler)
		// continue to operate even when the inverter is offline.
		Preferences preferences;

#ifdef MP_ESPUNO_ESP32C6
	_statusPixel.begin();
	_statusPixel.clear();
	_statusPixel.show();
	setStatusLedColor(0, 0, 255);
	setStatusLed(false);
#else // MP_ESPUNO_ESP32C6
	// Configure LED for output
	pinMode(LED_BUILTIN, OUTPUT);
#endif // MP_ESPUNO_ESP32C6

	
#ifdef BUTTON_PIN
	// Configure the user push button
	pinMode(BUTTON_PIN, INPUT);
//	pinMode(BUTTON_PIN, INPUT_PULLUP);
#endif // BUTTON_PIN

	// Wire.setClock(10000);
#ifdef MP_ESPUNO_ESP32C6
	diagDelay(1000);          // give USB boot time to settle
	Wire.begin(6, 7);
#endif // MP_ESPUNO_ESP32C6

	snprintf(_version, sizeof(_version), "%llu", static_cast<unsigned long long>(BUILD_TS_MS));
	diag_init();

	// Display time
#ifndef DISABLE_DISPLAY
	_display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);  // initialize OLED
	_display.clearDisplay();
	_display.display();
	updateOLED(false, "", "", _version);
#endif

	// Bit of a delay to give things time to kick in
	diagDelay(500);

#if defined(MP_ESP8266)
	pinMode(kSafeModePin, INPUT_PULLUP);
#endif

#ifdef DEBUG_OVER_SERIAL
	sprintf(_debugOutput, "Starting.");
	Serial.println(_debugOutput);
#endif
	Serial.printf("Firmware build ts: %llu\r\n", static_cast<unsigned long long>(BUILD_TS_MS));
	Serial.printf("Firmware version: %s\r\n", _version);

	buildDeviceName();
	{
		String resetReason = ESP.getResetReason();
		strlcpy(lastResetReason, resetReason.c_str(), sizeof(lastResetReason));
	}

	char storedIntent[kPrefBootIntentMaxLen] = "";
	char storedMode[kPrefBootModeMaxLen] = "";
	char storedSerial[kPrefDeviceSerialMaxLen] = "";
	char wifiSsid[kPrefWifiSsidMaxLen] = "";
	char wifiPass[kPrefWifiPasswordMaxLen] = "";
	char mqttServer[kPrefMqttServerMaxLen] = "";
	char mqttUser[kPrefMqttUsernameMaxLen] = "";
	char mqttPass[kPrefMqttPasswordMaxLen] = "";

	preferences.begin(DEVICE_NAME, true); // RO
	preferences.getString(kPreferenceBootIntent, storedIntent, sizeof(storedIntent));
	preferences.getString(kPreferenceBootMode, storedMode, sizeof(storedMode));
	preferences.getString(kPreferenceDeviceSerial, storedSerial, sizeof(storedSerial));
	preferences.getString("WiFi_SSID", wifiSsid, sizeof(wifiSsid));
	preferences.getString("WiFi_Password", wifiPass, sizeof(wifiPass));
	preferences.getString("MQTT_Server", mqttServer, sizeof(mqttServer));
	appConfig.mqttPort = preferences.getInt("MQTT_Port", 0);
	preferences.getString("MQTT_Username", mqttUser, sizeof(mqttUser));
	preferences.getString("MQTT_Password", mqttPass, sizeof(mqttPass));
#if defined(MP_XIAO_ESP32C6)
	appConfig.extAntenna = preferences.getBool("Ext_Antenna", false);
#elif defined(MP_ESPUNO_ESP32C6)
	appConfig.extAntenna = preferences.getBool("Ext_Antenna", false);
#endif // MP_XIAO_ESP32C6
	preferences.end();
	persistDefaultsIfMissing();

	currentBootIntent = bootIntentFromString(storedIntent);
	bootIntentForPublish = currentBootIntent;
	if (currentBootIntent != BootIntent::Normal) {
		// Boot intent is a one-boot diagnostic hint. Consume it now so later
		// unrelated resets do not keep reporting the previous requested mode.
		persistUserBootIntent(BootIntent::Normal);
	}
	currentBootMode = bootModeFromString(storedMode);
	bootModeForDiagnostics = currentBootMode;

	appConfig.wifiSSID = wifiSsid;
	appConfig.wifiPass = wifiPass;
	appConfig.mqttSrvr = mqttServer;
	appConfig.mqttUser = mqttUser;
	appConfig.mqttPass = mqttPass;

	if (storedSerial[0] != '\0') {
		strlcpy(deviceSerialNumber, storedSerial, sizeof(deviceSerialNumber));
		setMqttIdentifiersFromSerial(storedSerial);
	}
	bootPlan = planForBootMode(currentBootMode);
	BootMode startupMode = currentBootMode;

#ifdef DEBUG_OVER_SERIAL
	Serial.print("boot_intent=");
	Serial.println(bootIntentToString(currentBootIntent));
	Serial.print("boot_mode=");
	Serial.println(bootModeToString(currentBootMode));
#endif

	if (appConfig.wifiSSID == "" && String(WIFI_SSID).length() > 0) {
		appConfig.wifiSSID = WIFI_SSID;
	}
	if (appConfig.wifiPass == "" && String(WIFI_PASSWORD).length() > 0) {
		appConfig.wifiPass = WIFI_PASSWORD;
	}
	if (appConfig.mqttSrvr == "" && String(MQTT_SERVER).length() > 0) {
		appConfig.mqttSrvr = MQTT_SERVER;
	}
	if (appConfig.mqttPort == 0 && MQTT_PORT > 0) {
		appConfig.mqttPort = MQTT_PORT;
	}
	if (appConfig.mqttUser == "" && String(MQTT_USERNAME).length() > 0) {
		appConfig.mqttUser = MQTT_USERNAME;
	}
	if (appConfig.mqttPass == "" && String(MQTT_PASSWORD).length() > 0) {
		appConfig.mqttPass = MQTT_PASSWORD;
	}

#if defined(MP_ESP8266)
	if (digitalRead(kSafeModePin) == LOW) {
#ifdef DEBUG_OVER_SERIAL
		portalLog("Safe mode strap detected (GPIO0/D3 LOW); starting config portal.");
#endif
		updateOLED(false, "Safe", "mode", "portal");
		configHandler();
		return;
	}
#endif

	if (startupMode == BootMode::ApConfig) {
#ifdef DEBUG_OVER_SERIAL
		portalLog("Config mode boot (ap_config); starting AP captive portal.");
#endif
		// Explicit AP config mode forces the AP captive portal once; success will return to normal.
		updateOLED(false, "Config", "mode", "portal");
		configHandler();
		return;
	}
	if (startupMode == BootMode::WifiConfig) {
#ifdef DEBUG_OVER_SERIAL
		portalLog("Config mode boot (wifi_config); starting STA-only portal.");
#endif
		updateOLED(false, "WiFi", "config", "portal");
		configHandlerSta();
		return;
	}

	if (bootPlan.mqtt) {
		// If config is not setup, then enter config mode
		if ((appConfig.wifiSSID == "") ||
		    (appConfig.wifiPass == "") ||
		    (appConfig.mqttSrvr == "") ||
		    (appConfig.mqttPort == 0) ||
		    (appConfig.mqttUser == "") ||
		    (appConfig.mqttPass == "")) {
			configLoop();
			setBootIntentAndReboot(BootIntent::WifiConfig);
		} else {
			updateOLED(false, "Found", "config", _version);
			diagDelay(250);
		}
	}

	if (bootPlan.wifiSta) {
		// Configure WIFI
#ifdef DEBUG_OVER_SERIAL
		logHeap("pre-wifi");
#endif
		setupWifi(true);
		lastWifiConnected = true;
#ifdef DEBUG_OVER_SERIAL
		logHeap("after WiFi");
#endif
		recordBootMemStage(BootMemStage::Boot1);
		setupHttpControlPlane();
	}

	if (bootPlan.mqtt) {
		// Configure MQTT to the address and port specified above
		_mqtt.setServer(appConfig.mqttSrvr.c_str(), appConfig.mqttPort);
		_mqtt.setKeepAlive(60);
		// PubSubClient connect() waits for CONNACK in a tight loop without yielding. Keep this well
		// below the ESP8266 WDT window to avoid soft WDT resets if the broker is slow/unreachable.
		_mqtt.setSocketTimeout(1);
#ifdef DEBUG_OVER_SERIAL
		sprintf(_debugOutput, "About to request buffer");
		Serial.println(_debugOutput);
#endif
		for (int _bufferSize = (MAX_MQTT_PAYLOAD_SIZE + MQTT_HEADER_SIZE); _bufferSize >= MIN_MQTT_PAYLOAD_SIZE + MQTT_HEADER_SIZE; _bufferSize = _bufferSize - 1024) {
#ifdef DEBUG_OVER_SERIAL
		sprintf(_debugOutput, "Requesting a buffer of : %d bytes", _bufferSize);
		Serial.println(_debugOutput);
#endif

		if (_mqtt.setBufferSize(_bufferSize)) {
			
			_maxPayloadSize = _bufferSize - MQTT_HEADER_SIZE;
#ifdef DEBUG_OVER_SERIAL
			sprintf(_debugOutput, "_bufferSize: %d,\r\n\r\n_maxPayload (Including null terminator): %d", _bufferSize, _maxPayloadSize);
			Serial.println(_debugOutput);
#endif
			_mqttPayload = new char[_maxPayloadSize];
			if (_mqttPayload != NULL) {
				emptyPayload();
#ifdef DEBUG_OVER_SERIAL
					logHeap("after MQTT payload");
#endif
					recordBootMemStage(BootMemStage::Boot2);
					break;
			} else {
#ifdef DEBUG_OVER_SERIAL
				sprintf(_debugOutput, "Couldn't allocate payload of %d bytes", _maxPayloadSize);
				Serial.println(_debugOutput);
#endif
			}
		} else {
#ifdef DEBUG_OVER_SERIAL
			sprintf(_debugOutput, "Couldn't allocate buffer of %d bytes", _bufferSize);
			Serial.println(_debugOutput);
#endif
		}
		}

		// And any messages we are subscribed to will be pushed to the mqttCallback function for processing
			_mqtt.setCallback(mqttCallback);

			// Connect to MQTT before any RS485 probing so boot intent is observable even if RS485 stalls.
			mqttReconnect();
			publishBootEventOncePerBoot();
		}

		if (bootPlan.inverter) {
		// Set up the serial for communicating with the MAX
#if defined(DEBUG_OVER_SERIAL)
		logHeap("before RS485 init");
#endif
		_modBus = new RS485Handler;
#if defined(DEBUG_OVER_SERIAL)
#if RS485_STUB
			Serial.println("RS485 backend: stub");
#else
			Serial.println("RS485 backend: real");
#endif
#endif
#if defined(DEBUG_OVER_SERIAL) || defined(DEBUG_LEVEL2) || defined(DEBUG_OUTPUT_TX_RX)
		_modBus->setDebugOutput(_debugOutput);
#endif // DEBUG_OVER_SERIAL || DEBUG_LEVEL2 || DEBUG_OUTPUT_TX_RX
		_modBus->setServiceHook(serviceRs485Hooks);

			// Set up the helper class for reading with reading registers
			_registerHandler = new RegisterHandler(_modBus);
			if (deviceSerialNumber[0] != '\0' && deviceSerialNumber[1] != '\0') {
				_registerHandler->setSerialNumberPrefix(deviceSerialNumber[0], deviceSerialNumber[1]);
			}
#if RS485_STUB
				// Stub backend is used to validate scheduler + ESS snapshot behavior without inverter hardware.
				// Mark inverterReady so the scheduler attempts refreshEssSnapshot() and dispatch gating can be exercised.
				inverterReady = true;
#endif
#if defined(DEBUG_OVER_SERIAL)
			logHeap("after RS485 init");
#endif
			recordBootMemStage(BootMemStage::Boot3);

			rs485UartInfo = _modBus->uartInfo();

			// Start background probing; loop() will keep trying indefinitely with backoff capped at 15s.
			rs485ConnectState = Rs485ConnectState::ProbingBaud;
			rs485BaudIndex = -1;
			rs485AttemptsInCycle = 0;
			rs485CycleBackoffMs = kRs485ProbeAttemptDelayMs;
			rs485NextAttemptAtMs = millis();
			rs485LockedBaud = 0;

			// The scheduler owns ESS snapshot refresh and publishing cadence. Do not block setup() waiting
			// for inverter connectivity; the inverter may be offline and MQTT must still operate.
			essSnapshotValid = false;
			updateOLED(false, "RS485", "probing", _version);
		}
	}

void
configHandlerSta(void)
{
	WiFiManager wifiManager;
	// Portal polling UI reads runtime bucket state; load it once on portal entry.
	initMqttEntitiesRtIfNeeded(true);
	loadPollingConfig();

	// MODE_WIFI_CONFIG is STA-only (no SoftAP, no DNS). This avoids interfering with the LAN and
	// keeps heap usage lower, but requires the user to reach the device on its STA IP.
	WiFi.mode(WIFI_STA);

	// Clear one-shot portal boot mode immediately so the next reboot returns to normal runtime.
	if (currentBootMode != BootMode::Normal) {
		persistUserBootMode(BootMode::Normal);
	}
	currentBootMode = BootMode::Normal;

#ifdef DEBUG_OVER_SERIAL
	portalLog("STA portal: connecting to saved WiFi (ssid=%s)", appConfig.wifiSSID.c_str());
#endif

	WiFi.hostname(deviceName);
	WiFi.begin(appConfig.wifiSSID.c_str(), appConfig.wifiPass.c_str());

	const unsigned long start = millis();
	while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
		diagDelay(50);
	}

	if (WiFi.status() != WL_CONNECTED) {
#ifdef DEBUG_OVER_SERIAL
		portalLog("STA portal: connect failed; rebooting into normal. Hold D3/GPIO0 LOW at boot for AP portal.");
#endif
		setBootIntentAndReboot(BootIntent::Normal);
		return;
	}

#ifdef DEBUG_OVER_SERIAL
	portalLog("STA portal: connected IP=%s", WiFi.localIP().toString().c_str());
#endif

	// Reuse the existing portal implementation, but without starting SoftAP/DNS.
	wifiManager.setBreakAfterConfig(false);
	wifiManager.setTitle(deviceName);
	wifiManager.setShowInfoUpdate(false);
	{
		PortalMenu menu = portalMenuDefault();
		wifiManager.setMenu(menu.items, menu.count);
	}
	wifiManager.setCustomMenuHTML(portalMenuPollingHtml());
	wifiManager.setConnectTimeout(20);
	wifiManager.setConfigPortalTimeout(0);
	wifiManager.setDisableConfigPortal(false);
	wifiManager.setCustomHeadElement(portalCustomHeadScript());

	String mqttPortDefault = String(appConfig.mqttPort);
	WiFiManagerParameter p_lineBreak_text("<p>MQTT settings:</p>");
	WiFiManagerParameter custom_mqtt_server("server", "MQTT server", appConfig.mqttSrvr.c_str(), 40);
	WiFiManagerParameter custom_mqtt_port("port", "MQTT port", mqttPortDefault.c_str(), 6);
	WiFiManagerParameter custom_mqtt_user("user", "MQTT user", appConfig.mqttUser.c_str(), 32);
	WiFiManagerParameter custom_mqtt_pass("mpass", "MQTT password", appConfig.mqttPass.c_str(), 32);
	WiFiManagerParameter p_polling_link("<p><a href=\"/config/polling\">Polling schedule</a></p>");

	wifiManager.addParameter(&p_lineBreak_text);
	wifiManager.addParameter(&custom_mqtt_server);
	wifiManager.addParameter(&custom_mqtt_port);
	wifiManager.addParameter(&custom_mqtt_user);
	wifiManager.addParameter(&custom_mqtt_pass);
	wifiManager.addParameter(&p_polling_link);

	portalStatus = portalStatusIdle;
	portalStatusReason[0] = '\0';
	portalStatusSsid[0] = '\0';
	portalStatusIp[0] = '\0';
	portalLastDisconnectReason = -1;
	portalLastDisconnectLabel[0] = '\0';
	portalConnectStart = 0;
	portalNeedsMqttConfig = false;
	portalMqttSaved = false;
	portalRebootScheduled = false;
	portalRebootAt = 0;
	portalRoutesBoundServer = nullptr;

	wifiManager.setWebServerCallback([&]() {
		if (wifiManager.server) {
			if (portalRoutesBoundServer == static_cast<void *>(wifiManager.server.get())) {
				return;
			}
			portalRoutesBoundServer = static_cast<void *>(wifiManager.server.get());
			wifiManager.server->on("/status", [&]() {
				handlePortalStatusRequest(wifiManager);
			});
			wifiManager.server->on("/config/polling", HTTP_GET, [&]() {
				handlePortalPollingPage(wifiManager);
			});
			wifiManager.server->on("/config/polling/save", HTTP_POST, [&]() {
				handlePortalPollingSave(wifiManager);
			});
			wifiManager.server->on("/reboot/normal", HTTP_POST, [&]() {
				handlePortalRebootNormalRequest(wifiManager);
			});
		}
	});

	// Called before WiFiManager begins the connect-on-save attempt.
	wifiManager.setPreSaveConfigCallback([&]() {
		portalStatus = portalStatusConnecting;
		portalConnectStart = millis();
		strlcpy(portalStatusSsid, wifiManager.getWiFiSSID().c_str(), sizeof(portalStatusSsid));
		portalStatusReason[0] = '\0';
#ifdef DEBUG_OVER_SERIAL
		portalLog("WiFi submit: SSID=%s", portalStatusSsid);
		if (WiFi.status() == WL_CONNECTED) {
			portalLog("Status URL (STA): http://%s/status", WiFi.localIP().toString().c_str());
		}
#endif
	});
	// Called only after a successful connect-on-save in WiFiManager.
	wifiManager.setSaveConfigCallback([&]() {
#ifdef DEBUG_OVER_SERIAL
		portalLog("WiFi save callback (connected): SSID=%s", wifiManager.getWiFiSSID().c_str());
#endif
	});

	wifiManager.setSaveParamsCallback([&]() {
		int port = strtol(custom_mqtt_port.getValue(), NULL, 10);
		if (port < 0 || port > SHRT_MAX) {
			port = 0;
		}
		persistUserMqttConfig(custom_mqtt_server.getValue(), port, custom_mqtt_user.getValue(), custom_mqtt_pass.getValue());

		portalMqttSaved = true;
		portalNeedsMqttConfig = mqttServerIsBlank(custom_mqtt_server.getValue());
#ifdef DEBUG_OVER_SERIAL
		portalLog("MQTT params saved (server=%s)", custom_mqtt_server.getValue());
#endif
	});

	wifiManager.setConfigPortalBlocking(false);
	wifiManager.startWebPortal();

#ifdef DEBUG_OVER_SERIAL
	portalLog("STA portal URL: http://%s/", WiFi.localIP().toString().c_str());
	portalLog("Portal loop start free=%u max=%u frag=%u",
		ESP.getFreeHeap(),
		ESP.getMaxFreeBlockSize(),
		ESP.getHeapFragmentation());
#endif

	unsigned long portalStatsLast = 0;
	for (;;) {
		unsigned long processStart = millis();
		wifiManager.process();
		unsigned long processElapsed = millis() - processStart;
#ifdef DEBUG_OVER_SERIAL
		if (processElapsed > 100) {
			portalLog("process() took %lu ms free=%u max=%u frag=%u",
				processElapsed,
				ESP.getFreeHeap(),
				ESP.getMaxFreeBlockSize(),
				ESP.getHeapFragmentation());
		}
		if (checkTimer(&portalStatsLast, 5000)) {
			unsigned long connectAge = 0;
			if (portalConnectStart > 0) {
				connectAge = millis() - portalConnectStart;
			}
			portalLog("Portal stats: status=%s free=%u max=%u frag=%u connect_age=%lu",
				portalStatusLabel(portalStatus),
				ESP.getFreeHeap(),
				ESP.getMaxFreeBlockSize(),
				ESP.getHeapFragmentation(),
				connectAge);
		}
#endif

		if (portalStatus == portalStatusConnecting) {
			if (WiFi.status() == WL_CONNECTED) {
				portalStatus = portalStatusSuccess;
				strlcpy(portalStatusIp, WiFi.localIP().toString().c_str(), sizeof(portalStatusIp));
#ifdef DEBUG_OVER_SERIAL
				portalLog("WiFi connected: SSID=%s IP=%s RSSI=%d channel=%d free=%u max=%u frag=%u",
					portalStatusSsid,
					portalStatusIp,
					WiFi.RSSI(),
					WiFi.channel(),
					ESP.getFreeHeap(),
					ESP.getMaxFreeBlockSize(),
					ESP.getHeapFragmentation());
				portalLog("Status URL (STA): http://%s/status", portalStatusIp);
#endif
				updateOLED(false, "Web", "config", "succeeded");

				persistUserWifiCredentials(wifiManager.getWiFiSSID().c_str(), wifiManager.getWiFiPass().c_str());
				{
					Preferences prefsRo;
					prefsRo.begin(DEVICE_NAME, true);
				char storedMqttServer[kPrefMqttServerMaxLen] = "";
					prefsRo.getString("MQTT_Server", storedMqttServer, sizeof(storedMqttServer));
					prefsRo.end();
				PortalPostWifiAction postWifiAction = portalPostWifiActionAfterWifiSave(storedMqttServer);
				portalNeedsMqttConfig = (postWifiAction == PortalPostWifiAction::RedirectToMqttParams);
				if (postWifiAction == PortalPostWifiAction::Reboot) {
					unsigned long statusStart = millis();
					while (millis() - statusStart < 3000) {
						wifiManager.process();
						diagDelay(50);
					}
					setBootIntentAndReboot(BootIntent::Normal);
				}
				}
			}

			if (portalConnectStart > 0 && millis() - portalConnectStart >= 20000) {
				portalStatus = portalStatusFailed;
				const char *reason = wifiStatusReason(WiFi.status());
				if (strcmp(reason, "Unknown") == 0) {
					strlcpy(portalStatusReason, "failed to connect and hit timeout", sizeof(portalStatusReason));
				} else {
					strlcpy(portalStatusReason, reason, sizeof(portalStatusReason));
				}
#ifdef DEBUG_OVER_SERIAL
				portalLog("WiFi connect failed: %s status=%d heap=%u",
					portalStatusReason,
					static_cast<int>(WiFi.status()),
					ESP.getFreeHeap());
#endif
				updateOLED(false, "Web", "config", "failed");
				portalConnectStart = 0;
			}
		}

		// Option B behavior: if MQTT params saved and WiFi credentials exist, reboot into normal.
		if (portalMqttSaved && !portalNeedsMqttConfig && portalHasPersistedWifiCredentials()) {
			if (!portalRebootScheduled) {
				portalRebootScheduled = true;
				portalRebootAt = millis() + 1500;
#ifdef DEBUG_OVER_SERIAL
				portalLog("MQTT configured and WiFi credentials present; reboot scheduled.");
#endif
			}
		}
		if (portalRebootScheduled && static_cast<long>(millis() - portalRebootAt) >= 0) {
			setBootIntentAndReboot(BootIntent::Normal);
		}

		diagDelay(50);
	}
}

void
configLoop(void)
{
	bool flip = false;
	bool ledOn = false;

#ifdef DEBUG_OVER_SERIAL
	portalLog("Configuration is not set.");
#endif

	// If we have a BUTTON_PIN then only start web config when it has been pressed.
#ifdef BUTTON_PIN
	for (int i = 0; ; i++) {
		char line4[OLED_CHARACTER_WIDTH];

		snprintf(line4, sizeof(line4), "%d", i);
		if (i % 10 == 0) flip = !flip;
		if (flip) {
			updateOLED(false, "Config", "not set.", line4);
		} else {
			updateOLED(false, "Push", "button.", line4);
		}
		ledOn = !ledOn;
		setStatusLed(ledOn);

		// Read button state
		if (digitalRead(BUTTON_PIN) == LOW) {
			break;
		}

		diagDelay(300);
	}
#endif // BUTTON_PIN
	configHandler();
}

void
configHandler(void)
{
	WiFiManager wifiManager;
	// Portal polling UI reads runtime bucket state; load it once on portal entry.
	initMqttEntitiesRtIfNeeded(true);
	loadPollingConfig();

	// Config portal is intended to be a temporary recovery/config state, not a persistent "mode".
	// Clear one-shot portal boot mode immediately so the next reboot returns to normal runtime.
	if (currentBootMode != BootMode::Normal) {
		persistUserBootMode(BootMode::Normal);
		currentBootMode = BootMode::Normal;
#ifdef DEBUG_OVER_SERIAL
		Serial.println("Config portal entry: boot_mode reset to normal.");
#endif
	}

	// Keep AP alive while attempting STA connection so the portal stays reachable.
	WiFi.mode(WIFI_AP_STA);
	wifiManager.setBreakAfterConfig(false);
	wifiManager.setTitle(deviceName);
	wifiManager.setShowInfoUpdate(false);
	// Prefer the no-scan WiFi page by default to reduce heap churn; user can still scan via Refresh.
	{
		// Avoid WiFiManager's "exit" action: it shuts down the portal without rebooting, which can
		// leave the user stranded in a dead-end state. "restart" remains the exit path.
		PortalMenu menu = portalMenuDefault();
		wifiManager.setMenu(menu.items, menu.count);
	}
	wifiManager.setCustomMenuHTML(portalMenuPollingHtml());
	wifiManager.setConnectTimeout(20);
	wifiManager.setConfigPortalTimeout(0);
	// Keep the config portal (SoftAP + web server) running after a successful WiFi save.
	// WiFiManager defaults to shutting the portal down after connect, which breaks the
	// intended flow where the user proceeds to configure MQTT settings.
	wifiManager.setDisableConfigPortal(false);
	String mqttPortDefault = String(appConfig.mqttPort);
	WiFiManagerParameter p_lineBreak_text("<p>MQTT settings:</p>");
	WiFiManagerParameter custom_mqtt_server("server", "MQTT server", appConfig.mqttSrvr.c_str(), 40);
	WiFiManagerParameter custom_mqtt_port("port", "MQTT port", mqttPortDefault.c_str(), 6);
	WiFiManagerParameter custom_mqtt_user("user", "MQTT user", appConfig.mqttUser.c_str(), 32);
	WiFiManagerParameter custom_mqtt_pass("mpass", "MQTT password", appConfig.mqttPass.c_str(), 32);
	WiFiManagerParameter p_polling_link("<p><a href=\"/config/polling\">Polling schedule</a></p>");
#ifdef MP_XIAO_ESP32C6
	const char _customHtml_checkbox[] = "type=\"checkbox\"";
	WiFiManagerParameter custom_ext_ant("ext_antenna", "Use external WiFi antenna\n", "T", 2, _customHtml_checkbox, WFM_LABEL_AFTER);
#endif // MP_XIAO_ESP32C6
#ifdef MP_ESPUNO_ESP32C6
	const char _customHtml_checkbox[] = "type=\"checkbox\"";
	WiFiManagerParameter custom_ext_ant("ext_antenna", "Use external WiFi antenna\n", "T", 2, _customHtml_checkbox, WFM_LABEL_AFTER);
#endif // MP_ESPUNO_ESP32C6

	// Do not erase saved WiFi credentials when entering the portal; the user may be
	// here only to adjust MQTT settings.
#if defined(MP_ESP8266)
	WiFi.disconnect();
#else
	WiFi.disconnect(true);
#endif
	updateOLED(false, "Web", "config", "active");

#ifdef MP_XIAO_ESP32C6
	wifiManager.addParameter(&custom_ext_ant);
#endif // MP_XIAO_ESP32C6
#ifdef MP_ESPUNO_ESP32C6
	wifiManager.addParameter(&custom_ext_ant);
#endif // MP_ESPUNO_ESP32C6
	wifiManager.addParameter(&p_lineBreak_text);
	wifiManager.addParameter(&custom_mqtt_server);
	wifiManager.addParameter(&custom_mqtt_port);
	wifiManager.addParameter(&custom_mqtt_user);
	wifiManager.addParameter(&custom_mqtt_pass);
	wifiManager.addParameter(&p_polling_link);

	portalStatus = portalStatusIdle;
	portalStatusReason[0] = '\0';
	portalStatusSsid[0] = '\0';
	portalStatusIp[0] = '\0';
	portalLastDisconnectReason = -1;
	portalLastDisconnectLabel[0] = '\0';
	portalConnectStart = 0;
	portalNeedsMqttConfig = false;
	portalMqttSaved = false;
	portalRebootScheduled = false;
	portalRebootAt = 0;

#if defined MP_ESP8266
	static WiFiEventHandler disconnectHandler;
	disconnectHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& event) {
		portalLastDisconnectReason = static_cast<int>(event.reason);
		switch (event.reason) {
		case REASON_AUTH_FAIL:
			strlcpy(portalLastDisconnectLabel, "Auth failed", sizeof(portalLastDisconnectLabel));
			break;
		case REASON_NO_AP_FOUND:
			strlcpy(portalLastDisconnectLabel, "AP not found", sizeof(portalLastDisconnectLabel));
			break;
		case REASON_ASSOC_FAIL:
			strlcpy(portalLastDisconnectLabel, "Association failed", sizeof(portalLastDisconnectLabel));
			break;
		case REASON_HANDSHAKE_TIMEOUT:
			strlcpy(portalLastDisconnectLabel, "Handshake timeout", sizeof(portalLastDisconnectLabel));
			break;
		default:
			strlcpy(portalLastDisconnectLabel, "Disconnect", sizeof(portalLastDisconnectLabel));
			break;
		}
#ifdef DEBUG_OVER_SERIAL
		portalLog("WiFi disconnect: SSID=%s reason=%d (%s)",
			portalStatusSsid,
			portalLastDisconnectReason,
			portalLastDisconnectLabel);
#endif
	});
#endif

	wifiManager.setCustomHeadElement(portalCustomHeadScript());
	portalRoutesBoundServer = nullptr;
	wifiManager.setWebServerCallback([&]() {
		if (wifiManager.server) {
			if (portalRoutesBoundServer == static_cast<void *>(wifiManager.server.get())) {
				return;
			}
			portalRoutesBoundServer = static_cast<void *>(wifiManager.server.get());
			wifiManager.server->on("/status", [&]() {
				handlePortalStatusRequest(wifiManager);
			});
			wifiManager.server->on("/config/polling", HTTP_GET, [&]() {
				handlePortalPollingPage(wifiManager);
			});
			wifiManager.server->on("/config/polling/save", HTTP_POST, [&]() {
				handlePortalPollingSave(wifiManager);
			});
			wifiManager.server->on("/reboot/normal", HTTP_POST, [&]() {
				handlePortalRebootNormalRequest(wifiManager);
			});
		}
	});
	// Called before WiFiManager begins the connect-on-save attempt.
	// Use this to mark "connecting" so timeouts and status reflect reality even if connect fails.
	wifiManager.setPreSaveConfigCallback([&]() {
		portalStatus = portalStatusConnecting;
		portalConnectStart = millis();
		strlcpy(portalStatusSsid, wifiManager.getWiFiSSID().c_str(), sizeof(portalStatusSsid));
		portalStatusReason[0] = '\0';
#ifdef DEBUG_OVER_SERIAL
		IPAddress ip = WiFi.softAPIP();
		portalLog("WiFi submit: SSID=%s", portalStatusSsid);
		portalLog("Status URL (AP): http://%u.%u.%u.%u/status", ip[0], ip[1], ip[2], ip[3]);
#endif
	});
	// Called only after a successful connect-on-save in WiFiManager.
	wifiManager.setSaveConfigCallback([&]() {
#ifdef DEBUG_OVER_SERIAL
		portalLog("WiFi save callback (connected): SSID=%s", wifiManager.getWiFiSSID().c_str());
#endif
	});

	// Persist MQTT parameters when /paramsave is used, independent of WiFi success/failure.
	// Keeping this separate avoids WiFi saves clobbering MQTT values.
	wifiManager.setSaveParamsCallback([&]() {
		int port = strtol(custom_mqtt_port.getValue(), NULL, 10);
		if (port < 0 || port > SHRT_MAX) {
			port = 0;
		}
		persistUserMqttConfig(custom_mqtt_server.getValue(), port, custom_mqtt_user.getValue(), custom_mqtt_pass.getValue());

		portalMqttSaved = true;
		portalNeedsMqttConfig = mqttServerIsBlank(custom_mqtt_server.getValue());
#ifdef DEBUG_OVER_SERIAL
		portalLog("MQTT params saved (server=%s)", custom_mqtt_server.getValue());
#endif
	});
	wifiManager.setConfigPortalBlocking(false);
	wifiManager.startConfigPortal(deviceName);

#ifdef DEBUG_OVER_SERIAL
	IPAddress ip = WiFi.softAPIP();
	portalLog("Config portal SSID: %s", deviceName);
	portalLog("Config portal IP: %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
	portalLog("Portal loop start free=%u max=%u frag=%u",
		ESP.getFreeHeap(),
		ESP.getMaxFreeBlockSize(),
		ESP.getHeapFragmentation());
#endif

	unsigned long portalStatsLast = 0;
	for (;;) {
		unsigned long processStart = millis();
		wifiManager.process();
		unsigned long processElapsed = millis() - processStart;
#ifdef DEBUG_OVER_SERIAL
		if (processElapsed > 100) {
			portalLog("process() took %lu ms free=%u max=%u frag=%u",
				processElapsed,
				ESP.getFreeHeap(),
				ESP.getMaxFreeBlockSize(),
				ESP.getHeapFragmentation());
		}
		if (checkTimer(&portalStatsLast, 5000)) {
			unsigned long connectAge = 0;
			if (portalConnectStart > 0) {
				connectAge = millis() - portalConnectStart;
			}
			portalLog("Portal stats: status=%s free=%u max=%u frag=%u connect_age=%lu",
				portalStatusLabel(portalStatus),
				ESP.getFreeHeap(),
				ESP.getMaxFreeBlockSize(),
				ESP.getHeapFragmentation(),
				connectAge);
		}
#endif

		if (portalStatus == portalStatusConnecting) {
			if (WiFi.status() == WL_CONNECTED) {
				portalStatus = portalStatusSuccess;
				strlcpy(portalStatusIp, WiFi.localIP().toString().c_str(), sizeof(portalStatusIp));
#ifdef DEBUG_OVER_SERIAL
				portalLog("WiFi connected: SSID=%s IP=%s RSSI=%d channel=%d free=%u max=%u frag=%u",
					portalStatusSsid,
					portalStatusIp,
					WiFi.RSSI(),
					WiFi.channel(),
					ESP.getFreeHeap(),
					ESP.getMaxFreeBlockSize(),
					ESP.getHeapFragmentation());
				IPAddress apIp = WiFi.softAPIP();
				portalLog("Status URL (STA): http://%s/status", portalStatusIp);
				portalLog("Status URL (AP): http://%u.%u.%u.%u/status", apIp[0], apIp[1], apIp[2], apIp[3]);
#endif
				updateOLED(false, "Web", "config", "succeeded");

				// Save WiFi settings only here. MQTT settings are saved via setSaveParamsCallback (/paramsave).
				persistUserWifiCredentials(wifiManager.getWiFiSSID().c_str(), wifiManager.getWiFiPass().c_str());
				char storedMqttServer[kPrefMqttServerMaxLen] = "";
				{
					Preferences prefsRo;
					prefsRo.begin(DEVICE_NAME, true);
					prefsRo.getString("MQTT_Server", storedMqttServer, sizeof(storedMqttServer));
					prefsRo.end();
				}
				PortalPostWifiAction postWifiAction = portalPostWifiActionAfterWifiSave(storedMqttServer);
				portalNeedsMqttConfig = (postWifiAction == PortalPostWifiAction::RedirectToMqttParams);
#ifdef MP_XIAO_ESP32C6
				{
					const char *extAnt = custom_ext_ant.getValue();
					persistUserExtAntenna(extAnt[0] == 'T');
				}
#endif // MP_XIAO_ESP32C6
#ifdef MP_ESPUNO_ESP32C6
				{
					const char *extAnt = custom_ext_ant.getValue();
					persistUserExtAntenna(extAnt[0] == 'T');
				}
#endif // MP_ESPUNO_ESP32C6

				// If WiFi saved/connected but MQTT is blank, keep the portal alive and redirect to /param.
				// Otherwise keep the legacy behavior: short status display then reboot into normal.
					if (postWifiAction == PortalPostWifiAction::Reboot) {
						unsigned long statusStart = millis();
						while (millis() - statusStart < 3000) {
							wifiManager.process();
							diagDelay(50);
						}
						setBootIntentAndReboot(BootIntent::Normal);
					}
				}

			if (portalConnectStart > 0 && millis() - portalConnectStart >= 20000) {
				portalStatus = portalStatusFailed;
				const char *reason = wifiStatusReason(WiFi.status());
				if (strcmp(reason, "Unknown") == 0) {
					strlcpy(portalStatusReason, "failed to connect and hit timeout", sizeof(portalStatusReason));
				} else {
					strlcpy(portalStatusReason, reason, sizeof(portalStatusReason));
				}
#ifdef DEBUG_OVER_SERIAL
				portalLog("WiFi connect failed: %s status=%d heap=%u",
					portalStatusReason,
					static_cast<int>(WiFi.status()),
					ESP.getFreeHeap());
#endif
				updateOLED(false, "Web", "config", "failed");
				// Stay in the portal on failure so the device remains reachable for retries.
				portalConnectStart = 0;
			}
		}

			// After MQTT params are saved:
			// Option B: if WiFi credentials exist, reboot into normal immediately even if the STA is
			// not currently connected. This keeps the portal workflow intuitive when WiFi was
			// configured previously and the user only updated MQTT settings.
			// Do not block inside nested loops here; it can run in a non-yieldable context depending on
			// the WiFiManager call path and cause a core panic in __yield().
			if (portalMqttSaved && !portalNeedsMqttConfig && portalHasPersistedWifiCredentials()) {
				if (!portalRebootScheduled) {
					portalRebootScheduled = true;
					portalRebootAt = millis() + 1500;
#ifdef DEBUG_OVER_SERIAL
					portalLog("MQTT configured and WiFi credentials present; reboot scheduled.");
#endif
				}
			}
			if (portalRebootScheduled && static_cast<long>(millis() - portalRebootAt) >= 0) {
				setBootIntentAndReboot(BootIntent::Normal);
			}
			diagDelay(50);
		}
	}

/*
 * loop
 *
 * The loop function runs over and over again until power down or reset
 */
void
loop()
{
#ifdef FORCE_RESTART_HOURS
	static unsigned long autoReboot = 0;
#endif

#ifdef BUTTON_PIN
	// Read button state
	if (digitalRead(BUTTON_PIN) == LOW) {
		configHandler();
	}
#endif // BUTTON_PIN

	const uint32_t loopNowMs = millis();
	diag_loop_tick(loopNowMs);
	diag_wifi_status(static_cast<int16_t>(WiFi.status()), loopNowMs);

	// Refresh LED Screen, will cause the status asterisk to flicker
	updateOLED(true, "", "", "");

	if (bootPlan.wifiSta) {
		// Make sure WiFi is good
		if (WiFi.status() != WL_CONNECTED) {
			if (lastWifiConnected) {
				pendingWifiDisconnectEvent = true;
			}
			lastWifiConnected = false;
			setupWifi(false);
			if (bootPlan.mqtt) {
				mqttReconnect();
				requestHaDataResend();
			}
		} else {
			lastWifiConnected = true;
		}
	}

	if (bootPlan.mqtt) {
		// make sure mqtt is still connected
		const bool mqttOk = pumpMqttOnce();
		if (!mqttOk) {
			if (lastMqttConnected) {
				pendingMqttDisconnectEvent = true;
			}
			lastMqttConnected = false;
			mqttReconnect();
			requestHaDataResend();
		} else {
			lastMqttConnected = true;
		}
	}

	if (bootPlan.mqtt && pendingPollingConfigSet) {
		pendingPollingConfigSet = false;
		handlePollingConfigSet(g_portalBucketMapScratch);
		g_portalBucketMapScratch[0] = '\0';
	}
	if (bootPlan.mqtt && pendingRs485StubControlSet) {
		pendingRs485StubControlSet = false;
		applyRs485StubControlPayload(pendingDeferredControlPayload);
		pendingDeferredControlPayload[0] = '\0';
	}
	if (bootPlan.mqtt && pendingEntityCommandSet) {
		processPendingEntityCommand();
	}

	if (httpControlPlaneEnabled) {
		httpServer.handleClient();
	}
	if (bootPlan.mqtt) {
		subscribeInverterTopics();
	}

	// Keep attempting RS485/inverter connection in the background. This must not block loop() so MQTT, HTTP,
	// and the scheduler remain responsive even when RS485 is disconnected.
	if (bootPlan.inverter) {
		rs485ProbeTick();
	}

	updateStatusLed();

	// Check and display the runstate on the display
	updateRunstate();

	// Send HA auto-discovery info
	if (bootPlan.mqtt && resendHaData == true && _mqtt.connected()) {
		sendHaData();
	}

	if (bootPlan.inverter) {
		static bool longEnough = false;
		if (!longEnough && getUptimeSeconds() > 60) {  // After a minute, set these even if we didn't get a callback
			longEnough = true;
			opData.a2mReadyToUseOpMode = true;
			opData.a2mReadyToUseSocTarget = true;
			opData.a2mReadyToUsePwrCharge = true;
			opData.a2mReadyToUsePwrDischarge = true;
			opData.a2mReadyToUsePwrPush = true;
		}
	}

	// Scheduler runs continuously; per-bucket prerequisites are resolved inside sendData().
	if (bootPlan.mqtt) {
		sendData();
	}

	// Force Restart?
#ifdef FORCE_RESTART_HOURS
	if (checkTimer(&autoReboot, FORCE_RESTART_HOURS * 60 * 60 * 1000)) {
		setBootIntentAndReboot(BootIntent::Normal);
	}
#endif
}


uint32_t
getUptimeSeconds(void)
{
	static uint32_t uptimeSeconds = 0, uptimeSecondsSaved = 0;
	uint32_t nowSeconds = nowMillis() / 1000;

	if (nowSeconds < uptimeSeconds) {
		// We wrapped
		uptimeSecondsSaved += (UINT32_MAX / 1000);;
	}
	uptimeSeconds = nowSeconds;
	return uptimeSecondsSaved + uptimeSeconds;
}

const char*
mqttUpdateFreqToString(mqttUpdateFreq value)
{
	switch (value) {
	case mqttUpdateFreq::freqTenSec:
		return "freqTenSec";
	case mqttUpdateFreq::freqOneMin:
		return "freqOneMin";
	case mqttUpdateFreq::freqFiveMin:
		return "freqFiveMin";
	case mqttUpdateFreq::freqOneHour:
		return "freqOneHour";
	case mqttUpdateFreq::freqOneDay:
		return "freqOneDay";
	case mqttUpdateFreq::freqUser:
		return "freqUser";
	case mqttUpdateFreq::freqNever:
		return "freqNever";
	case mqttUpdateFreq::freqDisabled:
		return "freqDisabled";
	default:
		return "freqNever";
	}
}

bool
portalHasPersistedWifiCredentials(void)
{
	Preferences preferences;
	char ssid[kPrefWifiSsidMaxLen] = "";

	preferences.begin(DEVICE_NAME, true); // RO
	preferences.getString("WiFi_SSID", ssid, sizeof(ssid));
	preferences.end();

	return ssid[0] != '\0';
}

bool
mqttUpdateFreqFromString(const char *value, mqttUpdateFreq *result)
{
	if (value == NULL || result == NULL) {
		return false;
	}
	if (strcmp(value, "freqTenSec") == 0) {
		*result = mqttUpdateFreq::freqTenSec;
		return true;
	}
	if (strcmp(value, "freqOneMin") == 0) {
		*result = mqttUpdateFreq::freqOneMin;
		return true;
	}
	if (strcmp(value, "freqFiveMin") == 0) {
		*result = mqttUpdateFreq::freqFiveMin;
		return true;
	}
	if (strcmp(value, "freqOneHour") == 0) {
		*result = mqttUpdateFreq::freqOneHour;
		return true;
	}
	if (strcmp(value, "freqOneDay") == 0) {
		*result = mqttUpdateFreq::freqOneDay;
		return true;
	}
	if (strcmp(value, "freqUser") == 0) {
		*result = mqttUpdateFreq::freqUser;
		return true;
	}
	if (strcmp(value, "freqDisabled") == 0) {
		*result = mqttUpdateFreq::freqDisabled;
		return true;
	}
	return false;
}

void
buildPollingKey(const mqttState *entity, char *target, size_t size)
{
	snprintf(target, size, "Freq_%u", static_cast<unsigned int>(entity->entityId));
}

static bool
readLegacyPollingPref(size_t /* index */,
                      const mqttState *entity,
                      int defaultValue,
                      int &storedValue,
                      void *context)
{
	if (entity == nullptr || context == nullptr) {
		return false;
	}

	auto *preferences = static_cast<Preferences *>(context);
	char key[16];
	buildPollingKey(entity, key, sizeof(key));
	storedValue = preferences->getInt(key, defaultValue);
	return true;
}

static inline void
maybeYield()
{
	if (inMqttCallback) {
		return;
	}
#if defined(MP_ESP8266)
	if (!can_yield()) {
		return;
	}
#endif
	diagYield();
}

#if defined(MP_ESP8266)
static void
wifiScanCompleteNoop(int)
{
}
#endif


void
getPollingTimestamp(char *target, size_t size)
{
	modbusRequestAndResponse response;
	modbusRequestAndResponseStatusValues result;
	const char *fallback = "01/Jan/1970 00:00:00";

	if (_registerHandler != NULL) {
		result = _registerHandler->readHandledRegister(REG_CUSTOM_SYSTEM_DATE_TIME, &response);
		if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess &&
		    response.dataValueFormatted[0] != 0) {
			bool printable = true;
			for (size_t i = 0; response.dataValueFormatted[i] != '\0'; i++) {
				const unsigned char ch = static_cast<unsigned char>(response.dataValueFormatted[i]);
				if (ch < 0x20 || ch > 0x7e) {
					printable = false;
					break;
				}
			}
			if (printable) {
				strlcpy(target, response.dataValueFormatted, size);
				return;
			}
		}
	}

	strlcpy(target, fallback, size);
}

void
updatePollingLastChange(void)
{
	getPollingTimestamp(_pollingConfigLastChange, sizeof(_pollingConfigLastChange));
	persistUserPollingLastChange(_pollingConfigLastChange);
}

void
recomputeBucketCounts(void)
{
	if (!mqttEntitiesRtAvailable()) {
		return;
	}

	schedTenSecCount = 0;
	schedOneMinCount = 0;
	schedFiveMinCount = 0;
	schedOneHourCount = 0;
	schedOneDayCount = 0;
	schedUserCount = 0;

	const size_t entityCount = mqttEntitiesCount();
	for (size_t idx = 0; idx < entityCount; ++idx) {
		switch (mqttEntityBucketByIndex(idx)) {
		case BucketId::TenSec:
			schedTenSecCount++;
			break;
		case BucketId::OneMin:
			schedOneMinCount++;
			break;
		case BucketId::FiveMin:
			schedFiveMinCount++;
			break;
		case BucketId::OneHour:
			schedOneHourCount++;
			break;
		case BucketId::OneDay:
			schedOneDayCount++;
			break;
		case BucketId::User:
			schedUserCount++;
			break;
		case BucketId::Disabled:
		case BucketId::Unknown:
		default:
			break;
		}
	}
}

void
loadPollingConfig(void)
{
	if (!mqttEntitiesRtAvailable()) {
		return;
	}
	Preferences preferences;
	const mqttState *entities = mqttEntitiesDesc();
	const size_t entityCount = mqttEntitiesCount();
	BucketId *buckets = g_portalBucketsScratch;
	char *bucketMap = g_portalBucketMapScratch;
	bool appliedBucketMap = false;

	persistLoadOk = 0;
	persistLoadErr = 0;
	persistUnknownEntityCount = 0;
	persistInvalidBucketCount = 0;
	persistDuplicateEntityCount = 0;

	preferences.begin(DEVICE_NAME, true);

	char lastChange[kPrefPollingLastChangeMaxLen] = "";
	const size_t lastChangeLen = preferences.getString(kPreferencePollingLastChange,
	                                                  lastChange,
	                                                  sizeof(lastChange));
	if (lastChangeLen == 0) {
		getPollingTimestamp(_pollingConfigLastChange, sizeof(_pollingConfigLastChange));
	} else {
		strlcpy(_pollingConfigLastChange, lastChange, sizeof(_pollingConfigLastChange));
	}

	const uint32_t storedPollInterval = preferences.getUInt(kPreferencePollInterval, kPollIntervalDefaultSeconds);
	pollIntervalSeconds = clampPollInterval(storedPollInterval);

	for (size_t i = 0; i < entityCount; i++) {
		buckets[i] = bucketIdFromFreq(entities[i].updateFreq);
	}

	bucketMap[0] = '\0';
	preferences.getString(kPreferenceBucketMap, bucketMap, kPrefBucketMapMaxLen);
	const bool legacyMigrated = preferences.getBool(kPreferenceBucketMapMigrated, false);
	if (bucketMap[0] != '\0') {
		if (bucketMapUsesDescriptorIndices(bucketMap)) {
			// Older compact "#<index>=bucket" maps are not stable once the catalog grows.
			// Ignore them on boot and fall back to defaults until a fresh name-based map is saved.
			persistLoadOk = 1;
		} else {
			appliedBucketMap = applyBucketMapString(bucketMap,
			                                        entities,
			                                        entityCount,
			                                        buckets,
			                                        persistUnknownEntityCount,
			                                        persistInvalidBucketCount,
			                                        persistDuplicateEntityCount);
			if (appliedBucketMap) {
				persistLoadOk = 1;
			} else {
				persistLoadErr = 1;
			}
		}
	} else if (!legacyMigrated) {
		// Reuse the shared portal scratch buffer so upgrade-time migration stays off the ESP8266 task stack.
		size_t appliedCount = 0;
		if (buildBucketMapFromLegacyReader(entities,
		                                   entityCount,
		                                   readLegacyPollingPref,
		                                   &preferences,
		                                   bucketMap,
		                                   kPrefBucketMapMaxLen,
		                                   appliedCount)) {
			appliedBucketMap = applyBucketMapString(bucketMap,
			                                        entities,
			                                        entityCount,
			                                        buckets,
			                                        persistUnknownEntityCount,
			                                        persistInvalidBucketCount,
			                                        persistDuplicateEntityCount);
			if (appliedBucketMap) {
				persistLoadOk = 1;
			} else {
				persistLoadErr = 1;
			}
		} else {
			persistLoadOk = 1;
		}
	} else {
		persistLoadOk = 1;
	}

	preferences.end();
	if (!mqttEntityApplyBuckets(buckets, entityCount)) {
		persistLoadOk = 0;
		persistLoadErr = 1;
	}
	recomputeBucketCounts();
}

static bool
buildPersistedPollingConfigMap(const BucketId *buckets, char *map, size_t mapLen)
{
	const mqttState *entities = mqttEntitiesDesc();
	const size_t entityCount = mqttEntitiesCount();

	if (!mqttEntitiesRtAvailable() || buckets == nullptr || map == nullptr || mapLen == 0) {
		return false;
	}
	map[0] = '\0';
	size_t appliedCount = 0;
	return buildBucketMapFromAssignments(entities, entityCount, buckets, map, mapLen, appliedCount);
}

static bool
beginPollingConfigPayload(void)
{
	char addition[256];
	bool first = true;
	modbusRequestAndResponseStatusValues resultAddedToPayload = addToPayload("{");
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return false;
	}

	snprintf(addition, sizeof(addition), "\"last_change\": \"%s\", \"poll_interval_s\": %lu, \"allowed_intervals\": [",
	         _pollingConfigLastChange, static_cast<unsigned long>(pollIntervalSeconds));
	resultAddedToPayload = addToPayload(addition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return false;
	}

	for (size_t i = 0; i < _pollingAllowedIntervalCount; i++) {
		snprintf(addition, sizeof(addition), "%s\"%s\"", first ? "" : ", ", _pollingAllowedIntervals[i]);
		first = false;
		resultAddedToPayload = addToPayload(addition);
		if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
			return false;
		}
	}
	return true;
}

static bool
publishPollingConfigChunkClear(size_t startIndex, size_t endIndex)
{
	char topic[128];
	for (size_t i = startIndex; i < endIndex; ++i) {
		snprintf(topic, sizeof(topic), "%s/config/entity_intervals/%lu",
		         deviceName,
		         static_cast<unsigned long>(i));
		emptyPayload();
		if (!sendMqtt(topic, MQTT_RETAIN)) {
			return false;
		}
		maybeYield();
	}
	return true;
}

static bool
publishPollingConfigChunked(const mqttState *entities, size_t entityCount, const BucketId *buckets)
{
	char *chunkMap = g_portalBucketMapScratch;
	char addition[256];
	char topic[128];
	size_t chunkCount = 0;
	size_t activeCount = 0;
	size_t startIndex = 0;

	while (startIndex < entityCount) {
		size_t nextIndex = entityCount;
		size_t appliedCount = 0;
		if (!buildActiveBucketMapChunkFromAssignments(entities,
		                                              entityCount,
		                                              buckets,
		                                              startIndex,
		                                              chunkMap,
		                                              kPollingConfigChunkMapMaxLen,
		                                              nextIndex,
		                                              appliedCount)) {
			return false;
		}
		if (appliedCount == 0) {
			break;
		}
		chunkCount++;
		activeCount += appliedCount;
		startIndex = nextIndex;
	}

	startIndex = 0;
	for (size_t chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex) {
		size_t nextIndex = entityCount;
		size_t appliedCount = 0;
		if (!buildActiveBucketMapChunkFromAssignments(entities,
		                                              entityCount,
		                                              buckets,
		                                              startIndex,
		                                              chunkMap,
		                                              kPollingConfigChunkMapMaxLen,
		                                              nextIndex,
		                                              appliedCount) ||
		    appliedCount == 0) {
			return false;
		}

		emptyPayload();
		if (addToPayload("{") == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
			return false;
		}
		snprintf(addition,
		         sizeof(addition),
		         "\"chunk_index\": %lu, \"chunk_count\": %lu, \"active_bucket_map\": \"",
		         static_cast<unsigned long>(chunkIndex),
		         static_cast<unsigned long>(chunkCount));
		if (addToPayload(addition) == modbusRequestAndResponseStatusValues::payloadExceededCapacity ||
		    addToPayload(chunkMap) == modbusRequestAndResponseStatusValues::payloadExceededCapacity ||
		    addToPayload("\"}") == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
			return false;
		}
		snprintf(topic,
		         sizeof(topic),
		         "%s/config/entity_intervals/%lu",
		         deviceName,
		         static_cast<unsigned long>(chunkIndex));
		if (!sendMqtt(topic, MQTT_RETAIN)) {
			return false;
		}
		startIndex = nextIndex;
		maybeYield();
	}

	if (g_lastPublishedPollingConfigChunkCount > chunkCount &&
	    !publishPollingConfigChunkClear(chunkCount, g_lastPublishedPollingConfigChunkCount)) {
		return false;
	}

	emptyPayload();
	if (!beginPollingConfigPayload()) {
		return false;
	}
	snprintf(addition,
	         sizeof(addition),
	         "], \"entity_intervals_encoding\": \"bucket_map_chunks\", \"entity_intervals_chunks\": %lu, \"entity_intervals_count\": %lu}",
	         static_cast<unsigned long>(chunkCount),
	         static_cast<unsigned long>(activeCount));
	if (addToPayload(addition) == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return false;
	}
	snprintf(topic, sizeof(topic), "%s/config", deviceName);
	if (!sendMqtt(topic, MQTT_RETAIN)) {
		return false;
	}
	g_lastPublishedPollingConfigChunkCount = chunkCount;
	return true;
}

void
publishPollingConfig(void)
{
	if (!mqttEntitiesRtAvailable()) {
		return;
	}
	char addition[256];
	char configTopic[64];
	bool first = true;
	const mqttState *entities = mqttEntitiesDesc();
	const size_t entityCount = mqttEntitiesCount();
	BucketId *buckets = g_portalBucketsScratch;
	if (!mqttEntityCopyBuckets(buckets, entityCount)) {
		return;
	}

	emptyPayload();
	if (beginPollingConfigPayload() &&
	    addToPayload("], \"entity_intervals\": {") != modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		for (size_t i = 0; i < entityCount; i++) {
			if (buckets[i] == BucketId::Disabled) {
				continue;
			}
			char entityName[64];
			mqttEntityNameCopy(&entities[i], entityName, sizeof(entityName));
			snprintf(addition, sizeof(addition), "%s\"%s\": \"%s\"",
			         first ? "" : ", ",
			         entityName,
			         bucketIdToString(buckets[i]));
			first = false;
			if (addToPayload(addition) == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
				emptyPayload();
				break;
			}
			if ((i % 8) == 0) {
				diagYield();
			}
		}

		if (_mqttPayload[0] != '\0' &&
		    addToPayload("}}") != modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
			snprintf(configTopic, sizeof(configTopic), "%s/config", deviceName);
			if (!sendMqtt(configTopic, MQTT_RETAIN)) {
				return;
			}
			if (g_lastPublishedPollingConfigChunkCount > 0 &&
			    !publishPollingConfigChunkClear(0, g_lastPublishedPollingConfigChunkCount)) {
				return;
			}
			g_lastPublishedPollingConfigChunkCount = 0;
			return;
		}
	}

	publishPollingConfigChunked(entities, entityCount, buckets);
}

bool
publishConfigDiscovery(void)
{
	char addition[256];
	modbusRequestAndResponseStatusValues resultAddedToPayload;
	const char *sensorName = "MQTT_Config";
	const char *prettyName = "MQTT Config";
	char topic[128];

	emptyPayload();

	resultAddedToPayload = addToPayload("{");
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return true;
	}

	snprintf(addition, sizeof(addition), "\"component\": \"sensor\"");
	resultAddedToPayload = addToPayload(addition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return true;
	}

	snprintf(addition, sizeof(addition),
		 ", \"device\": {"
		 " \"name\": \"%s\", \"model\": \"%s\", \"manufacturer\": \"AlphaESS\","
		 " \"identifiers\": [\"%s\"]}",
		 deviceName, kControllerModel,
		 controllerIdentifier);
	resultAddedToPayload = addToPayload(addition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return true;
	}

	snprintf(addition, sizeof(addition), ", \"name\": \"%s\"", prettyName);
	resultAddedToPayload = addToPayload(addition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return true;
	}

	snprintf(addition, sizeof(addition), ", \"unique_id\": \"%s_%s\"", controllerIdentifier, sensorName);
	resultAddedToPayload = addToPayload(addition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return true;
	}

	snprintf(addition, sizeof(addition),
		", \"state_topic\": \"%s/config\""
		", \"value_template\": \"{{ value_json.last_change | default(\\\"\\\") }}\""
		", \"json_attributes_topic\": \"%s/config\""
		", \"entity_category\": \"diagnostic\"",
		deviceName, deviceName);
	resultAddedToPayload = addToPayload(addition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return true;
	}

	resultAddedToPayload = addToPayload("}");
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return true;
	}

	snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", controllerIdentifier, sensorName);
	return sendMqtt(topic, MQTT_RETAIN);
}

bool
publishControllerInverterSerialDiscovery(void)
{
	char addition[256];
	modbusRequestAndResponseStatusValues resultAddedToPayload;
	char topic[160];

	emptyPayload();

	resultAddedToPayload = addToPayload("{");
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return true;
	}
	resultAddedToPayload = addToPayload("\"component\": \"sensor\"");
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return true;
	}
	snprintf(addition, sizeof(addition),
	         ", \"device\": { \"name\": \"%s\", \"model\": \"%s\", \"manufacturer\": \"AlphaESS\", \"identifiers\": [\"%s\"]}",
	         deviceName, kControllerModel, controllerIdentifier);
	resultAddedToPayload = addToPayload(addition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return true;
	}
	resultAddedToPayload = addToPayload(", \"name\": \"Inverter Serial\"");
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return true;
	}
	snprintf(addition, sizeof(addition), ", \"unique_id\": \"%s_%s\"", controllerIdentifier, kControllerInverterSerialEntity);
	resultAddedToPayload = addToPayload(addition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return true;
	}
	snprintf(addition, sizeof(addition),
	         ", \"state_topic\": \"%s/%s/%s/state\", \"entity_category\": \"diagnostic\"",
	         deviceName, controllerIdentifier, kControllerInverterSerialEntity);
	resultAddedToPayload = addToPayload(addition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return true;
	}
	resultAddedToPayload = addToPayload("}");
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return true;
	}

	snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", controllerIdentifier, kControllerInverterSerialEntity);
	return sendMqtt(topic, MQTT_RETAIN);
}

void
publishControllerInverterSerialState(void)
{
	char topic[160];
	char payload[40];
	snprintf(topic, sizeof(topic), "%s/%s/%s/state", deviceName, controllerIdentifier, kControllerInverterSerialEntity);
	if (inverterSerialKnown()) {
		strlcpy(payload, deviceSerialNumber, sizeof(payload));
	} else {
		strlcpy(payload, "unknown", sizeof(payload));
	}
	_mqtt.publish(topic, payload, true);
}

void
requestHaDataResend(void)
{
	resendHaData = true;
	resendHaPreludePending = true;
	resendHaNextEntityIndex = 0;
}

static const char *
homeAssistantEntityType(const mqttState *entity)
{
	if (entity == nullptr) {
		return "sensor";
	}
	switch (entity->haClass) {
	case homeAssistantClass::haClassBox:
	case homeAssistantClass::haClassNumber:
		return "number";
	case homeAssistantClass::haClassSelect:
		return "select";
	case homeAssistantClass::haClassBinaryProblem:
		return "binary_sensor";
	default:
		return "sensor";
	}
}

bool
clearHaEntityDiscovery(const mqttState *entity, const char *deviceId)
{
	char topic[128];

	if (entity == nullptr || deviceId == nullptr || deviceId[0] == '\0') {
		return false;
	}

	char entityName[64];
	mqttEntityNameCopy(entity, entityName, sizeof(entityName));
	snprintf(topic,
	         sizeof(topic),
	         "homeassistant/%s/%s/%s/config",
	         homeAssistantEntityType(entity),
	         deviceId,
	         entityName);
	emptyPayload();
	return sendMqtt(topic, MQTT_RETAIN);
}

static bool
publishPendingStaleInverterDiscoveryClears(void)
{
	if (!resendHaClearStaleInverterPending ||
	    resendHaClearStaleInverterQueueIndex >= resendHaClearStaleInverterQueueCount ||
	    resendHaClearStaleInverterDeviceIds[resendHaClearStaleInverterQueueIndex][0] == '\0') {
		return false;
	}
	if (!mqttEntitiesRtAvailable()) {
		return false;
	}

	const mqttState *entities = mqttEntitiesDesc();
	const size_t entityCount = mqttEntitiesCount();
	size_t batchCount = 0;
	while (resendHaClearStaleInverterIndex < entityCount && batchCount < kHaDiscoveryBatchSize) {
		const size_t idx = resendHaClearStaleInverterIndex;
		const mqttState *entity = &entities[idx];
		if (mqttEntityScope(entity->entityId) != DiscoveryDeviceScope::Inverter) {
			resendHaClearStaleInverterIndex++;
			continue;
		}
		if (!clearHaEntityDiscovery(entity,
		                            resendHaClearStaleInverterDeviceIds[resendHaClearStaleInverterQueueIndex])) {
			return true;
		}
		resendHaClearStaleInverterIndex++;
		batchCount++;
		maybeYield();
	}
	if (resendHaClearStaleInverterIndex < entityCount) {
		return true;
	}

	resendHaClearStaleInverterIndex = 0;
	resendHaClearStaleInverterDeviceIds[resendHaClearStaleInverterQueueIndex][0] = '\0';
	resendHaClearStaleInverterQueueIndex++;
	if (resendHaClearStaleInverterQueueIndex < resendHaClearStaleInverterQueueCount) {
		return true;
	}

	resendHaClearStaleInverterPending = false;
	resendHaClearStaleInverterQueueIndex = 0;
	resendHaClearStaleInverterQueueCount = 0;
	return false;
}

static bool
publishHaEntityDiscovery(const mqttState *entity)
{
	if (entity == NULL) {
		return true;
	}
	if (!mqttEntitiesRtAvailable()) {
		return true;
	}
	size_t idx = static_cast<size_t>(entity - mqttEntitiesDesc());
	if (idx >= mqttEntitiesCount()) {
		return true;
	}
	mqttUpdateFreq effectiveFreq = mqttEntityEffectiveFreqByIndex(idx);
	const DiscoveryDeviceScope scope = mqttEntityScope(entity->entityId);
	const char *deviceId = discoveryDeviceIdForScope(scope);
	if (deviceId[0] == '\0') {
		return true;
	}

	if (effectiveFreq == mqttUpdateFreq::freqNever) {
		return sendDataFromMqttState(entity, true);
	}

	if (effectiveFreq == mqttUpdateFreq::freqDisabled) {
		return clearHaEntityDiscovery(entity, deviceId);
	}

	sendDataFromMqttState(entity, true);
	return true;
}

bool
handlePollingConfigSet(const char *payload)
{
	struct PollingConfigSetContext {
		bool anyChange = false;
		bool bucketAssignmentsChanged = false;
		bool bucketsApplied = false;
		bool pollIntervalChanged = false;
		const mqttState *entities = nullptr;
		size_t entityCount = 0;
		BucketId *buckets = nullptr;
		BucketId *originalBuckets = nullptr;
		bool bucketsLoaded = false;
		uint32_t stagedPollInterval = kPollIntervalDefaultSeconds;
	};

	const mqttState *entities = mqttEntitiesDesc();
	const size_t entityCount = mqttEntitiesCount();
	BucketId *buckets = g_portalBucketsScratch;
	const bool bucketsLoaded = mqttEntitiesRtAvailable() && mqttEntityCopyBuckets(buckets, entityCount);
	BucketId originalBuckets[kMqttEntityDescriptorCount]{};
	if (bucketsLoaded) {
		memcpy(originalBuckets, buckets, sizeof(originalBuckets));
	}
	PollingConfigSetContext ctx{};
	ctx.entities = entities;
	ctx.entityCount = entityCount;
	ctx.buckets = buckets;
	ctx.originalBuckets = originalBuckets;
	ctx.bucketsLoaded = bucketsLoaded;
	ctx.stagedPollInterval = pollIntervalSeconds;

	char *valueScratch = _mqttPayload;
	size_t valueScratchSize = (_mqttPayload != nullptr && _maxPayloadSize > 0)
		? static_cast<size_t>(_maxPayloadSize)
		: sizeof(g_portalRowRenderBuf);
	if (valueScratch == nullptr || valueScratchSize < 2) {
		valueScratch = g_portalRowRenderBuf;
		valueScratchSize = sizeof(g_portalRowRenderBuf);
	}

	if (!validatePollingConfigEntries(payload,
	                                  valueScratch,
	                                  valueScratchSize)) {
		return false;
	}

	const bool parsed = visitPollingConfigEntries(
		payload,
		valueScratch,
		valueScratchSize,
		[](const char *key, const char *value, void *opaque) -> bool {
			PollingConfigSetContext &ctx = *static_cast<PollingConfigSetContext *>(opaque);
			bool handled = false;

				if (!strcmp(key, kPreferencePollInterval)) {
					char *endPtr = nullptr;
					errno = 0;
					unsigned long parsed = strtoul(value, &endPtr, 10);
					if (errno == 0 && endPtr != value && *endPtr == '\0') {
						uint32_t clamped = clampPollInterval(static_cast<uint32_t>(parsed));
						if (clamped != pollIntervalSeconds) {
							ctx.stagedPollInterval = clamped;
							ctx.pollIntervalChanged = true;
						}
					}
					handled = true;
				}

			if (!strcmp(key, "bucket_map")) {
				if (ctx.bucketsLoaded) {
					BucketId beforeBuckets[kMqttEntityDescriptorCount]{};
					memcpy(beforeBuckets, ctx.buckets, ctx.entityCount * sizeof(BucketId));
					persistUnknownEntityCount = 0;
					persistInvalidBucketCount = 0;
					persistDuplicateEntityCount = 0;
					const bool applied = applyBucketMapString(value,
					                                          ctx.entities,
					                                          ctx.entityCount,
					                                          ctx.buckets,
					                                          persistUnknownEntityCount,
					                                          persistInvalidBucketCount,
					                                          persistDuplicateEntityCount);
					persistLoadOk = applied ? 1 : 0;
					persistLoadErr = applied ? 0 : 1;
					if (!applied) {
						return false;
					}
					if (memcmp(beforeBuckets, ctx.buckets, ctx.entityCount * sizeof(BucketId)) != 0) {
						ctx.bucketAssignmentsChanged = true;
					}
				}
				handled = true;
			}

			if (!handled) {
				const mqttState *entity = lookupEntityByName(key, ctx.entities, ctx.entityCount);
				if (entity != nullptr && ctx.bucketsLoaded) {
					BucketId bucket = bucketIdFromString(value);
					if (bucket != BucketId::Unknown) {
						size_t idx = static_cast<size_t>(entity - ctx.entities);
						if (idx < ctx.entityCount && ctx.buckets[idx] != bucket) {
							ctx.buckets[idx] = bucket;
							ctx.bucketAssignmentsChanged = true;
						}
					}
				}
			}

			maybeYield();
			return true;
		},
	&ctx);
	if (!parsed) {
		return false;
	}
	if ((ctx.pollIntervalChanged || ctx.bucketAssignmentsChanged) &&
	    !buildPersistedPollingConfigMap(ctx.buckets, g_portalBucketMapScratch, kPrefBucketMapMaxLen)) {
		persistLoadOk = 0;
		persistLoadErr = 1;
		return false;
	}

	if (ctx.bucketAssignmentsChanged) {
		if (!mqttEntityApplyBuckets(buckets, entityCount)) {
			persistLoadOk = 0;
			persistLoadErr = 1;
			return false;
		}
		ctx.bucketsApplied = true;
	}
	if ((ctx.pollIntervalChanged || ctx.bucketAssignmentsChanged) &&
	    !persistUserPollingConfig(ctx.stagedPollInterval, g_portalBucketMapScratch)) {
		if (ctx.bucketsApplied) {
			mqttEntityApplyBuckets(ctx.originalBuckets, entityCount);
		}
		persistLoadOk = 0;
		persistLoadErr = 1;
		return false;
	}

	if (ctx.bucketAssignmentsChanged) {
		ctx.anyChange = true;
		requestHaDataResend();
		resendAllData = true;
	}
	if (ctx.pollIntervalChanged) {
		pollIntervalSeconds = ctx.stagedPollInterval;
		ctx.anyChange = true;
		resendAllData = true;
	}

	if (ctx.anyChange) {
		recomputeBucketCounts();
		updatePollingLastChange();
		publishPollingConfig();
		resendAllData = true;
	}

	return true;
}

/*
 * setupWifi
 *
 * Connect to WiFi
 */
void
setupWifi(bool initialConnect)
{
	char line3[OLED_CHARACTER_WIDTH];
	char line4[OLED_CHARACTER_WIDTH];

	// We start by connecting to a WiFi network
#ifdef DEBUG_OVER_SERIAL
	if (initialConnect) {
		sprintf(_debugOutput, "Connecting to %s", WIFI_SSID);
	} else {
		sprintf(_debugOutput, "Reconnect to %s", WIFI_SSID);
	}
	Serial.println(_debugOutput);
#endif
	if (initialConnect) {
		WiFi.disconnect(); // If it auto-started, restart it our way.
		diagDelay(100);
#if defined(MP_XIAO_ESP32C6) || defined(MP_ESPUNO_ESP32C6)
		bool useExtAntenna = false;
#ifdef MP_XIAO_ESP32C6
		useExtAntenna = appConfig.extAntenna;
#else // MP_XIAO_ESP32C6
		useExtAntenna = appConfig.extAntenna;
#endif // MP_XIAO_ESP32C6
#ifdef WIFI_ENABLE
		pinMode(WIFI_ENABLE, OUTPUT);
		digitalWrite(WIFI_ENABLE, LOW);
		diagDelay(100);
#endif // WIFI_ENABLE
#ifdef WIFI_ANT_CONFIG
		pinMode(WIFI_ANT_CONFIG, OUTPUT);
		digitalWrite(WIFI_ANT_CONFIG, useExtAntenna ? HIGH : LOW);
#endif // WIFI_ANT_CONFIG
#endif // MP_XIAO_ESP32C6 || MP_ESPUNO_ESP32C6
#ifdef A2M_DEBUG_WIFI
	} else {
		wifiReconnects++;
#endif // A2M_DEBUG_WIFI
	}
	if (!initialConnect) {
		wifiReconnectCount++;
	}

	// And continually try to connect to WiFi.
	// If it doesn't, the device will just wait here before continuing
	for (int tries = 0; WiFi.status() != WL_CONNECTED; tries++) {
		snprintf(line3, sizeof(line3), "WiFi %d ...", tries);

		if (tries == 5000) {
			setBootIntentAndReboot(BootIntent::Normal);
		}
#ifdef BUTTON_PIN
		// Read button state
		if (digitalRead(BUTTON_PIN) == LOW) {
			configHandler();
		}
#endif // BUTTON_PIN

		if (tries % 50 == 0) {
			WiFi.disconnect();

			// Set up in Station Mode - Will be connecting to an access point
			WiFi.mode(WIFI_STA);
			// Helps when multiple APs for our SSID
#if defined MP_ESP32
			WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
			WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
#endif // MP_ESP32

			// Set the hostname for this Arduino
			WiFi.hostname(deviceName);

			// And connect to the details defined at the top
			WiFi.begin(appConfig.wifiSSID.c_str(), appConfig.wifiPass.c_str());

#if defined MP_ESP8266
			wifiPower -= WIFI_POWER_DECREMENT;
			if (wifiPower < WIFI_POWER_MIN) {
				wifiPower = WIFI_POWER_MAX;
			}
			WiFi.setOutputPower(wifiPower);
			snprintf(line4, sizeof(line4), "TX: %0.2f", wifiPower);
#else
			switch (wifiPower) {
			case WIFI_POWER_19_5dBm:
				wifiPower = WIFI_POWER_19dBm;
				break;
			case WIFI_POWER_19dBm:
				wifiPower = WIFI_POWER_18_5dBm;
				break;
			case WIFI_POWER_18_5dBm:
				wifiPower = WIFI_POWER_17dBm;
				break;
			case WIFI_POWER_17dBm:
				wifiPower = WIFI_POWER_15dBm;
				break;
			case WIFI_POWER_15dBm:
				wifiPower = WIFI_POWER_13dBm;
				break;
			case WIFI_POWER_13dBm:
				wifiPower = WIFI_POWER_11dBm;
				break;
			case WIFI_POWER_11dBm:
			default:
				wifiPower = WIFI_POWER_19_5dBm;
				break;
			}
			WiFi.setTxPower(wifiPower);
			snprintf(line4, sizeof(line4), "TX: %0.01fdBm", (int)wifiPower / 4.0f);
#endif
		}

		if (initialConnect) {
			updateOLED(false, "Connecting", line3, line4);
		} else {
			updateOLED(false, "Reconnect", line3, line4);
		}
		diagDelay(500);
	}

	// Output some debug information
#ifdef DEBUG_OVER_SERIAL
	Serial.print("WiFi connected, IP is ");
	Serial.println(WiFi.localIP());
	byte *bssid = WiFi.BSSID();
	sprintf(_debugOutput, "WiFi BSSID is %02X:%02X:%02X:%02X:%02X:%02X", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
	Serial.println(_debugOutput);
	Serial.print("WiFi RSSI: ");
	Serial.println(WiFi.RSSI());
#endif

#if defined(MP_ESP8266)
	// Free any retained scan results to reduce heap pressure in NORMAL mode.
	WiFi.scanDelete();
#ifdef DEBUG_OVER_SERIAL
	Serial.println(F("WiFi scan results cleared."));
#endif
#endif

	// Connected, so ditch out with blank screen
	snprintf(line3, sizeof(line3), "%s", WiFi.localIP().toString().c_str());
	updateOLED(false, line3, "", _version);
}



/*
 * checkTimer
 *
 * Check to see if the elapsed interval has passed since the passed in millis() value. If it has, return true and update the lastRun.
 * Uses wraparound-safe arithmetic so the timer behaves across millis() overflow.
 */
bool
checkTimer(unsigned long *lastRun, unsigned long interval)
{
	unsigned long now = nowMillis();

	if (shouldRun(now, *lastRun, interval)) {
		*lastRun = now;
		return true;
	}

	return false;
}

#define CURSOR_LINE_1 0
#define CURSOR_LINE_2 ((SCREEN_HEIGHT / 4) * 1)
#define CURSOR_LINE_3 ((SCREEN_HEIGHT / 4) * 2)
#define CURSOR_LINE_4 ((SCREEN_HEIGHT / 4) * 3)

/*
 * updateOLED
 *
 * Update the OLED. Use "NULL" for no change to a line or "" for an empty line.
 * Three parameters representing each of the three lines available for status indication - Top line functionality fixed
 */
void
updateOLED(bool justStatus, const char* line2, const char* line3, const char* line4)
{
#ifdef DISABLE_DISPLAY
	(void)justStatus;
	(void)line2;
	(void)line3;
	(void)line4;
	return;
#else
	static unsigned long updateStatusBar = 0;

	_display.clearDisplay();
	_display.setTextSize(1);
	_display.setTextColor(WHITE);
	_display.setCursor(0, CURSOR_LINE_1);

	char line1Contents[OLED_CHARACTER_WIDTH];
	char line2Contents[OLED_CHARACTER_WIDTH];
	char line3Contents[OLED_CHARACTER_WIDTH];
	char line4Contents[OLED_CHARACTER_WIDTH];

	strlcpy(line2Contents, line2, sizeof(line2Contents));
	strlcpy(line3Contents, line3, sizeof(line3Contents));
	strlcpy(line4Contents, line4, sizeof(line4Contents));

	// Only update the operating indicator once per half second.
	if (checkTimer(&updateStatusBar, UPDATE_STATUS_BAR_INTERVAL)) {
		// Simply swap between space and asterisk every time we come here to give some indication of activity
		_oledOperatingIndicator = (_oledOperatingIndicator == '*') ? ' ' : '*';
	}

#ifdef LARGE_DISPLAY
	{
		int8_t rssi = WiFi.RSSI();
		bool mqttOk = false;
		if (bootPlan.mqtt) {
			mqttOk = _mqtt.connected();
		}
		// There's 20 characters we can play with, width wise.
		snprintf(line1Contents, sizeof(line1Contents), "A2M  %c%c%c         %3hhd",
			 _oledOperatingIndicator, (WiFi.status() == WL_CONNECTED ? 'W' : ' '), (mqttOk ? 'M' : ' '), rssi );
		_display.println(line1Contents);
		printWifiBars(rssi);
	}
#else // LARGE_DISPLAY
	bool mqttOk = false;
	if (bootPlan.mqtt) {
		mqttOk = _mqtt.connected();
	}
	// There's ten characters we can play with, width wise.
	snprintf(line1Contents, sizeof(line1Contents), "%s%c%c%c", "A2M    ",
		 _oledOperatingIndicator, (WiFi.status() == WL_CONNECTED ? 'W' : ' '), (mqttOk ? 'M' : ' ') );
	_display.println(line1Contents);
#endif // LARGE_DISPLAY

	// Next line
	_display.setCursor(0, CURSOR_LINE_2);
	if (!justStatus) {
		_display.println(line2Contents);
		strcpy(_oledLine2, line2Contents);
	} else {
		_display.println(_oledLine2);
	}

	_display.setCursor(0, CURSOR_LINE_3);
	if (!justStatus) {
		_display.println(line3Contents);
		strcpy(_oledLine3, line3Contents);
	} else {
		_display.println(_oledLine3);
	}

	_display.setCursor(0, CURSOR_LINE_4);
	if (!justStatus) {
		_display.println(line4Contents);
		strcpy(_oledLine4, line4Contents);
	} else {
		_display.println(_oledLine4);
	}
	// Refresh the display
	_display.display();
#endif
}

#define WIFI_X_POS 75 //102
void
printWifiBars(int rssi)
{
#ifdef DISABLE_DISPLAY
	(void)rssi;
	return;
#else
	if (rssi >= -55) { 
		_display.fillRect((WIFI_X_POS + 0),7,4,1, WHITE);
		_display.fillRect((WIFI_X_POS + 5),6,4,2, WHITE);
		_display.fillRect((WIFI_X_POS + 10),4,4,4, WHITE);
		_display.fillRect((WIFI_X_POS + 15),2,4,6, WHITE);
		_display.fillRect((WIFI_X_POS + 20),0,4,8, WHITE);
	} else if (rssi < -55 && rssi > -65) {
		_display.fillRect((WIFI_X_POS + 0),7,4,1, WHITE);
		_display.fillRect((WIFI_X_POS + 5),6,4,2, WHITE);
		_display.fillRect((WIFI_X_POS + 10),4,4,4, WHITE);
		_display.fillRect((WIFI_X_POS + 15),2,4,6, WHITE);
		_display.drawRect((WIFI_X_POS + 20),0,4,8, WHITE);
	} else if (rssi < -65 && rssi > -75) {
		_display.fillRect((WIFI_X_POS + 0),8,4,1, WHITE);
		_display.fillRect((WIFI_X_POS + 5),6,4,2, WHITE);
		_display.fillRect((WIFI_X_POS + 10),4,4,4, WHITE);
		_display.drawRect((WIFI_X_POS + 15),2,2,6, WHITE);
		_display.drawRect((WIFI_X_POS + 20),0,4,8, WHITE);
	} else if (rssi < -75 && rssi > -85) {
		_display.fillRect((WIFI_X_POS + 0),8,4,1, WHITE);
		_display.fillRect((WIFI_X_POS + 5),6,4,2, WHITE);
		_display.drawRect((WIFI_X_POS + 10),4,4,4, WHITE);
		_display.drawRect((WIFI_X_POS + 15),2,4,6, WHITE);
		_display.drawRect((WIFI_X_POS + 20),0,4,8, WHITE);
	} else if (rssi < -85 && rssi > -96) {
		_display.fillRect((WIFI_X_POS + 0),8,4,1, WHITE);
		_display.drawRect((WIFI_X_POS + 5),6,4,2, WHITE);
		_display.drawRect((WIFI_X_POS + 10),4,4,4, WHITE);
		_display.drawRect((WIFI_X_POS + 15),2,4,6, WHITE);
		_display.drawRect((WIFI_X_POS + 20),0,4,8, WHITE);
	} else {
		_display.drawRect((WIFI_X_POS + 0),8,4,1, WHITE);
		_display.drawRect((WIFI_X_POS + 5),6,4,2, WHITE);
		_display.drawRect((WIFI_X_POS + 10),4,4,4, WHITE);
		_display.drawRect((WIFI_X_POS + 15),2,4,6, WHITE);
		_display.drawRect((WIFI_X_POS + 20),0,4,8, WHITE);
	}
#endif
}



/*
 * getSerialNumber
 *
 * Display on load to demonstrate connectivty and send the prefix into RegisterHandler as
 * some system fault descriptions depend on knowing whether an AL based or AE based inverter.
 */
modbusRequestAndResponseStatusValues
getSerialNumber()
{
	modbusRequestAndResponseStatusValues result = modbusRequestAndResponseStatusValues::preProcessing;
	modbusRequestAndResponse response;
#ifndef DEBUG_NO_RS485
	uint32_t tries = 0;
	const uint8_t kMaxIdentityReadAttempts = 4;
#endif
	char oledLine3[OLED_CHARACTER_WIDTH];
	char oledLine4[OLED_CHARACTER_WIDTH];

#ifdef DEBUG_NO_RS485
	result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
	strcpy(response.dataValueFormatted, "AL9876543210987");
#else // DEBUG_NO_RS485
	// Get the serial number
	result = _registerHandler->readHandledRegister(REG_SYSTEM_INFO_R_EMS_SN_BYTE_1_2, &response);

	// Keep retries bounded so startup cannot stall indefinitely when RS485 is unavailable.
	uint8_t serialAttempts = 0;
	while (((result != modbusRequestAndResponseStatusValues::readDataRegisterSuccess) ||
	       (strlen(response.dataValueFormatted) < 15)) &&
	       (serialAttempts++ < kMaxIdentityReadAttempts)) {
		tries++;
		rs485Errors++;
		snprintf(oledLine4, sizeof(oledLine4), "%ld", tries);
		updateOLED(false, "Alpha sys", "not known", oledLine4);
		pumpMqttDuringSetup(250);
		result = _registerHandler->readHandledRegister(REG_SYSTEM_INFO_R_EMS_SN_BYTE_1_2, &response);
	}
#endif // DEBUG_NO_RS485
	const bool liveSerialReadOk =
		(result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) &&
		(strlen(response.dataValueFormatted) >= 15);
	if (liveSerialReadOk) {
		strlcpy(deviceSerialNumber, response.dataValueFormatted, sizeof(deviceSerialNumber));
		persistUserDeviceSerial(deviceSerialNumber);
	} else {
		Preferences preferences;
		char storedSerial[kPrefDeviceSerialMaxLen] = "";
		preferences.begin(DEVICE_NAME, true);
		preferences.getString(kPreferenceDeviceSerial, storedSerial, sizeof(storedSerial));
		preferences.end();
		if (storedSerial[0] != '\0') {
			strlcpy(deviceSerialNumber, storedSerial, sizeof(deviceSerialNumber));
		} else {
			strlcpy(deviceSerialNumber, "UNKNOWN", sizeof(deviceSerialNumber));
		}
	}
	inverterReady = liveSerialReadOk;

#ifdef DEBUG_NO_RS485
result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
	strcpy(response.dataValueFormatted, "FAKE-BAT");
#else // DEBUG_NO_RS485
	// Get the Battery Type
	result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_TYPE, &response);
	// Keep retries bounded so startup cannot stall indefinitely when RS485 is unavailable.
	uint8_t batteryAttempts = 0;
	while ((result != modbusRequestAndResponseStatusValues::readDataRegisterSuccess) &&
	       (batteryAttempts++ < kMaxIdentityReadAttempts)) {
		tries++;
		rs485Errors++;
		snprintf(oledLine4, sizeof(oledLine4), "%ld", tries);
		updateOLED(false, "Bat type", "not known", oledLine4);
		pumpMqttDuringSetup(250);
		result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_TYPE, &response);
	}
#endif // DEBUG_NO_RS485
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		strlcpy(deviceBatteryType, response.dataValueFormatted, sizeof(deviceBatteryType));
	} else {
		strlcpy(deviceBatteryType, "UNKNOWN", sizeof(deviceBatteryType));
	}

#ifndef DISABLE_DISPLAY
#ifdef LARGE_DISPLAY
	strlcpy(oledLine3, deviceSerialNumber, sizeof(oledLine3));
	strlcpy(oledLine4, deviceBatteryType, sizeof(oledLine4));
#else // LARGE_DISPLAY
	strlcpy(oledLine3, &response.dataValueFormatted[0], 11);
	strlcpy(oledLine4, &response.dataValueFormatted[10], 6);
#endif // LARGE_DISPLAY
	updateOLED(false, "Hello", oledLine3, oledLine4);
#endif // DISABLE_DISPLAY

#ifdef DEBUG_OVER_SERIAL
	sprintf(_debugOutput, "Alpha Serial Number: %s", deviceSerialNumber);
	Serial.println(_debugOutput);
#endif

	_registerHandler->setSerialNumberPrefix(deviceSerialNumber[0], deviceSerialNumber[1]);
	if (haUniqueId[0] == '\0') {
		setMqttIdentifiersFromSerial(deviceSerialNumber);
	}

	pumpMqttDuringSetup(4000);

	//Flash the LED
	setStatusLed(true);
	diagDelay(4);
	setStatusLed(false);

	return result;
}


/*
 * updateRunstate
 *
 * Determines a few things about the sytem and updates the display
 * Things updated - Dispatch state discharge/charge, battery power, battery percent
 */
#ifdef DISABLE_DISPLAY
void
updateRunstate()
{
	return;
}
#elif defined(LARGE_DISPLAY)
void
updateRunstate()
{
	static unsigned long lastRun = 0;
#ifndef DEBUG_NO_RS485
	const char *dMode = NULL, *dAction = NULL;
#endif
	char line2[OLED_CHARACTER_WIDTH] = "";
	char line3[OLED_CHARACTER_WIDTH] = "";
	char line4[OLED_CHARACTER_WIDTH] = "";


	if (checkTimer(&lastRun, RUNSTATE_INTERVAL)) {
		//Flash the LED
		setStatusLed(true);
		diagDelay(4);
		setStatusLed(false);

		if (!opData.essRs485Connected) {
			strcpy(line2, "RS485");
			strcpy(line3, "disconnected");
		} else {
#ifdef DEBUG_NO_RS485
			strcpy(line2, "NO RS485");
#else // DEBUG_NO_RS485
			// Line 2: Get Dispatch Start - Is Alpha2MQTT controlling the inverter?
			if (opData.essDispatchStart != DISPATCH_START_START) {
				strcpy(line2, "Stopped");
			} else {
				switch (opData.essDispatchMode) {
				case DISPATCH_MODE_BATTERY_ONLY_CHARGED_VIA_PV:
					dMode = "PV Only";
					break;
				case DISPATCH_MODE_STATE_OF_CHARGE_CONTROL:
					dMode = "SOC Ctl";
					break;
				case DISPATCH_MODE_LOAD_FOLLOWING:
					dMode = "LoadFollow";
					break;
				case DISPATCH_MODE_MAXIMISE_OUTPUT:
					dMode = "MaxOut";
					break;
				case DISPATCH_MODE_NORMAL_MODE:
					dMode = "Normal";
					break;
				case DISPATCH_MODE_OPTIMISE_CONSUMPTION:
					dMode = "OptConsmpt";
					break;
				case DISPATCH_MODE_MAXIMISE_CONSUMPTION:
					dMode = "MaxConsmpt";
					break;
				case DISPATCH_MODE_ECO_MODE:
					dMode = "ECO";
					break;
				case DISPATCH_MODE_FCAS_MODE:
					dMode = "FCAS";
					break;
				case DISPATCH_MODE_PV_POWER_SETTING:
					dMode = "PV Pwr";
					break;
				case DISPATCH_MODE_NO_BATTERY_CHARGE:
					dMode = "No Bat Chg";
					break;
				case DISPATCH_MODE_BURNIN_MODE:
					dMode = "Burnin";
					break;
				default:
					dMode = "BadMode";
					break;
				}

				// Determine if charging or discharging by looking at power
				if (opData.essDispatchActivePower < DISPATCH_ACTIVE_POWER_OFFSET) {
					dAction = "Charge";
				} else if (opData.essDispatchActivePower > DISPATCH_ACTIVE_POWER_OFFSET) {
					dAction = "Dischrg";
				} else {
					dAction = "Hold";
				}
				snprintf(line2, sizeof(line2), "%s : %s", dMode, dAction);
			}
#endif // DEBUG_NO_RS485

			// Get battery info for line 3
			snprintf(line3, sizeof(line3), "Bat: %4dW  %0.02f%%", opData.essBatteryPower, opData.essBatterySoc * BATTERY_SOC_MULTIPLIER);
		}
		{   // Line 4 - Rotating diags
			static int debugIdx = 0;

			if (debugIdx < 1) {
				snprintf(line4, sizeof(line4), "Uptime: %lu", getUptimeSeconds());
				debugIdx = 1;
#ifdef DEBUG_FREEMEM
			} else if (debugIdx < 2) {
				snprintf(line4, sizeof(line4), "Mem: %u", freeMemory());
				debugIdx = 2;
#endif // DEBUG_FREEMEM
#ifdef A2M_DEBUG_WIFI
			} else if (debugIdx < 3) {
				snprintf(line4, sizeof(line4), "WiFi recon: %lu", wifiReconnects);
				debugIdx = 3;
			} else if (debugIdx < 4) {
#if defined(MP_ESP32)
				snprintf(line4, sizeof(line4), "WiFi TX: %0.01fdBm", (int)WiFi.getTxPower() / 4.0f);
#else
				snprintf(line4, sizeof(line4), "WiFi TX: %0.01fdBm", wifiPower);
#endif
				debugIdx = 4;
			} else if (debugIdx < 5) {
				snprintf(line4, sizeof(line4), "WiFi RSSI: %d", WiFi.RSSI());
				debugIdx = 5;
#endif // A2M_DEBUG_WIFI
#ifdef DEBUG_CALLBACKS
			} else if (debugIdx < 6) {
				snprintf(line4, sizeof(line4), "Callbacks: %lu", receivedCallbacks);
				debugIdx = 6;
			} else if (debugIdx < 7) {
				snprintf(line4, sizeof(line4), "Unk CBs: %lu", unknownCallbacks);
				debugIdx = 7;
			} else if (debugIdx < 8) {
				snprintf(line4, sizeof(line4), "Bad CBs: %lu", badCallbacks);
				debugIdx = 8;
#endif // DEBUG_CALLBACKS
			} else if (debugIdx < 9) {
				snprintf(line4, sizeof(line4), "RS485 Err: %lu", rs485Errors);
				debugIdx = 9;
			} else if (debugIdx < 11) {
				char tmpOpMode[12];
				getOpModeDesc(tmpOpMode, sizeof(tmpOpMode), opData.a2mOpMode);
				snprintf(line4, sizeof(line4), "OpMode: %s", tmpOpMode);
				debugIdx = 11;
#ifndef DEBUG_NO_RS485
			} else if (debugIdx < 12) {
				snprintf(line4, sizeof(line4), "Pwr: %ldW", DISPATCH_ACTIVE_POWER_OFFSET - opData.essDispatchActivePower);
				debugIdx = 12;
			} else if (debugIdx < 13) {
				snprintf(line4, sizeof(line4), "SOC TGT: %hu%% %0.02f%%", opData.a2mSocTarget, opData.essDispatchSoc * DISPATCH_SOC_MULTIPLIER);
				debugIdx = 13;
#endif // ! DEBUG_NO_RS485
#ifdef DEBUG_OPS
			} else if (debugIdx < 15) {
				snprintf(line4, sizeof(line4), "opCnt: %lu", opCounter);
				debugIdx = 15;
#endif // DEBUG_OPS
			} else { // Must be last
				snprintf(line4, sizeof(line4), "Version: %s", _version);
				debugIdx = 0;
			}
		}

		updateOLED(false, line2, line3, line4);
	}
}
#else // LARGE_DISPLAY
void updateRunstate()
{
	
	static unsigned long lastRun = 0;
	static int lastLine2 = 0;

	char line2[OLED_CHARACTER_WIDTH] = "";
	char line3[OLED_CHARACTER_WIDTH] = "";
	char line4[OLED_CHARACTER_WIDTH] = "";


	if (checkTimer(&lastRun, RUNSTATE_INTERVAL)) {
		//Flash the LED
		setStatusLed(true);
		diagDelay(4);
		setStatusLed(false);

		if (!opData.essRs485Connected) {
			strcpy(line2, "RS485");
			strcpy(line3, "disconn");
			strcpy(line4, "  ected");
		} else {
			if (opData.essDispatchStart != DISPATCH_START_START) {
				strcpy(line2, "Stopped");
			} else {
				if (lastLine2 == 0) {
					lastLine2 = 1;
					// Get the mode.
					switch (opData.essDispatchMode) {
					case DISPATCH_MODE_BATTERY_ONLY_CHARGED_VIA_PV:
						strcpy(line2, "PV Only");
						break;
					case DISPATCH_MODE_STATE_OF_CHARGE_CONTROL:
						strcpy(line2, "SOC Ctl");
						break;
					case DISPATCH_MODE_LOAD_FOLLOWING:
						strcpy(line2, "LoadFollow");
						break;
					case DISPATCH_MODE_MAXIMISE_OUTPUT:
						strcpy(line2, "MaxOut");
						break;
					case DISPATCH_MODE_NORMAL_MODE:
						strcpy(line2, "Normal");
						break;
					case DISPATCH_MODE_OPTIMISE_CONSUMPTION:
						strcpy(line2, "OptConsmpt");
						break;
					case DISPATCH_MODE_MAXIMISE_CONSUMPTION:
						strcpy(line2, "MaxConsmpt");
						break;
					case DISPATCH_MODE_ECO_MODE:
						strcpy(line2, "ECO");
						break;
					case DISPATCH_MODE_FCAS_MODE:
						strcpy(line2, "FCAS");
						break;
					case DISPATCH_MODE_PV_POWER_SETTING:
						strcpy(line2, "PV Pwr");
						break;
					case DISPATCH_MODE_NO_BATTERY_CHARGE:
						strcpy(line2, "No Bat Chg");
						break;
					case DISPATCH_MODE_BURNIN_MODE:
						strcpy(line2, "Burnin");
						break;
					default:
						strcpy(line2, "BadMode");
						break;
					}
				} else {
					lastLine2 = 0;
					// Determine if charging or discharging by looking at power
					if (opData.essDispatchActivePower < DISPATCH_ACTIVE_POWER_OFFSET) {
						strcpy(line2, "Charge");
					} else if (opData.essDispatchActivePower > DISPATCH_ACTIVE_POWER_OFFSET) {
						strcpy(line2, "Discharge");
					} else {
						strcpy(line2, "Hold");
					}
				}
			}

			if (lastLine2 == 1) {
				// Get battery power for line 3
				snprintf(line3, sizeof(line3), "Bat:%dW", opData.essBatteryPower);

				// And percent for line 4
				snprintf(line4, sizeof(line4), "%0.02f%%", opData.essBatterySoc * BATTERY_SOC_MULTIPLIER);
			} else {
				snprintf(line3, sizeof(line3), "Mem: %u", freeMemory());
#if defined MP_ESP8266
				snprintf(line4, sizeof(line4), "TX: %0.2f", wifiPower);
#else
				strcpy(line4, "");
#endif
			}
		}

		updateOLED(false, line2, line3, line4);
	}
}
#endif // DISABLE_DISPLAY



/*
 * mqttReconnect
 *
 * This function reconnects to the MQTT broker
 */
void
mqttReconnect(void)
{
	static unsigned long lastAttemptMs = 0;
	static int tries = 0;
	static bool mqttTargetLogged = false;

	bool subscribed = false;
	char subscriptionDef[192];
	char line3[OLED_CHARACTER_WIDTH];
	bool inverterSubscriptionsAdded = false;

#if defined(MP_ESP8266)
	// Defensive: avoid rare soft-WDT crashes in ESP8266WiFiScanClass::_scanDone calling a stale std::function
	// callback when an async scan is in progress. We do not rely on async scans in NORMAL mode; if one is
	// running, replace the completion callback with a safe no-op so the core can finish the scan without
	// calling into a dangling target.
	auto guardAsyncWifiScanCallback = []() {
		const int8_t scanState = WiFi.scanComplete();
#ifdef DEBUG_OVER_SERIAL
		Serial.printf("WiFi guard scan state %d\r\n", scanState);
#endif
		const WifiScanGuardAction action = classifyWifiScanGuard(scanState);
		if (action == WifiScanGuardAction::RebindNoopCallback) {
			// Guard any in-flight scan, even in NORMAL mode. The protection is about
			// neutralizing a stale callback target, not authorizing new scans.
			WiFi.scanNetworksAsync(wifiScanCompleteNoop, false);
			return;
		}
		if (action == WifiScanGuardAction::DeleteResults) {
			WiFi.scanDelete();
		}
	};
#endif

	// Throttle reconnect attempts; do not block the main loop.
	const unsigned long nowMs = millis();
	if ((nowMs - lastAttemptMs) < 5000) {
		return;
	}
	lastAttemptMs = nowMs;

	initMqttEntitiesRtIfNeeded(true);
	if (!pendingPollingConfigSet) {
		loadPollingConfig();
	}

	unsigned long attemptStart = nowMs;
	// Keep the ESP8266 watchdog happy even if the broker is unreachable.
	diagDelay(0);
	tries++;
#ifdef DEBUG_OVER_SERIAL
	Serial.printf("mqttReconnect attempt %d start @ %lu ms\r\n", tries, attemptStart);
	if (!mqttTargetLogged) {
		Serial.printf("MQTT target: user=%s host=%s:%u\r\n",
			      appConfig.mqttUser.c_str(),
			      appConfig.mqttSrvr.c_str(),
			      static_cast<unsigned>(appConfig.mqttPort));
		mqttTargetLogged = true;
	}
#endif

		_mqtt.disconnect();		// Just in case.
		diagDelay(200);

#ifdef BUTTON_PIN
		// Read button state
		if (digitalRead(BUTTON_PIN) == LOW) {
			configHandler();
		}
#endif // BUTTON_PIN
		if (WiFi.status() != WL_CONNECTED) {
			setupWifi(false);
		}
		diag_wifi_status(static_cast<int16_t>(WiFi.status()), millis());

#if defined(MP_ESP8266)
#ifdef DEBUG_OVER_SERIAL
		logHeap("before WiFi guard");
#endif
		guardAsyncWifiScanCallback();
		if (!shouldStartWifiScan(currentBootMode)) {
#ifdef DEBUG_OVER_SERIAL
			Serial.println(F("WiFi guard: scans disabled in NORMAL."));
#endif
		}
#ifdef DEBUG_OVER_SERIAL
		logHeap("after WiFi guard");
#endif
#endif

#ifdef DEBUG_OVER_SERIAL
		Serial.print("Attempting MQTT connection...");
#endif

		snprintf(line3, sizeof(line3), "MQTT %d ...", tries);
		updateOLED(false, "Connecting", line3, _version);
		diagDelay(100);

#ifdef DEBUG_OVER_SERIAL
		{
			const char *mqttHost = appConfig.mqttSrvr.c_str();
			const uint16_t mqttPort = static_cast<uint16_t>(appConfig.mqttPort);
			const size_t mqttHostLen = strlen(mqttHost);
			const size_t mqttUserLen = appConfig.mqttUser.length();
			const size_t mqttPassLen = appConfig.mqttPass.length();

			Serial.printf("MQTT diag: host_len=%u user_len=%u pass_len=%u port=%u wifi=%d ip=%s rssi=%d\r\n",
			              static_cast<unsigned>(mqttHostLen),
			              static_cast<unsigned>(mqttUserLen),
			              static_cast<unsigned>(mqttPassLen),
			              static_cast<unsigned>(mqttPort),
			              static_cast<int>(WiFi.status()),
			              WiFi.localIP().toString().c_str(),
			              WiFi.RSSI());

			Serial.print("MQTT host bytes:");
			for (size_t i = 0; i < mqttHostLen; i++) {
				Serial.printf(" %02X", static_cast<unsigned>(static_cast<uint8_t>(mqttHost[i])));
			}
			Serial.println();

			WiFiClient mqttProbe;
			mqttProbe.setTimeout(2000);
			const unsigned long probeStartMs = millis();
			const bool probeOk = mqttProbe.connect(mqttHost, mqttPort);
			const unsigned long probeElapsedMs = millis() - probeStartMs;
			Serial.printf("MQTT TCP probe: ok=%d elapsed_ms=%lu\r\n", probeOk ? 1 : 0, probeElapsedMs);
			if (probeOk) {
				mqttProbe.stop();
			}
			}
	#endif

		// Attempt to connect.
		diag_mqtt_attempt(millis());
#if defined(MP_ESP8266)
			ESP.wdtDisable();
#endif
		const bool mqttConnected = _mqtt.connect(
			deviceName,
			appConfig.mqttUser.c_str(),
			appConfig.mqttPass.c_str(),
			statusTopic,
			0,
			true,
				"{ \"presence\": \"offline\", \"a2mStatus\": \"offline\", \"rs485Status\": \"unavailable\", \"gridStatus\": \"unavailable\" }");
#if defined(MP_ESP8266)
			ESP.wdtEnable(0);
#endif
			diag_mqtt_result(mqttConnected, static_cast<int16_t>(_mqtt.state()), millis());
			if (mqttConnected) {
			const mqttState *entities = mqttEntitiesDesc();
			int numberOfEntities = static_cast<int>(mqttEntitiesCount());
#ifdef DEBUG_OVER_SERIAL
			Serial.println("Connected MQTT");
#endif
			// Publish boot intent early; RS485 init can stall before periodic status messages.
			publishBootEventOncePerBoot();
			publishStatusNow();
			mqttReconnectCount++;
			lastMqttConnectMs = millis();
			lastMqttConnected = true;
			if (pendingWifiDisconnectEvent) {
				publishEvent(MqttEventCode::WifiDisconnect, "");
				pendingWifiDisconnectEvent = false;
			}
			if (pendingMqttDisconnectEvent) {
				publishEvent(MqttEventCode::MqttDisconnect, "");
				pendingMqttDisconnectEvent = false;
			}

			// Special case for Home Assistant
			sprintf(subscriptionDef, "%s", MQTT_SUB_HOMEASSISTANT);
			subscribed = _mqtt.subscribe(subscriptionDef, MQTT_SUBSCRIBE_QOS);
#ifdef DEBUG_OVER_SERIAL
			snprintf(_debugOutput, sizeof(_debugOutput), "Subscribed to \"%s\" : %d", subscriptionDef, subscribed);
			Serial.println(_debugOutput);
#endif
				sprintf(subscriptionDef, "%s/config/set", deviceName);
				subscribed = subscribed && _mqtt.subscribe(subscriptionDef, MQTT_SUBSCRIBE_QOS);
#ifdef DEBUG_OVER_SERIAL
				snprintf(_debugOutput, sizeof(_debugOutput), "Subscribed to \"%s\" : %d", subscriptionDef, subscribed);
				Serial.println(_debugOutput);
#endif

#if RS485_STUB
				subscribed = subscribed && _mqtt.subscribe(rs485StubControlTopic, MQTT_SUBSCRIBE_QOS);
#ifdef DEBUG_OVER_SERIAL
				snprintf(_debugOutput, sizeof(_debugOutput), "Subscribed to \"%s\" : %d", rs485StubControlTopic, subscribed);
				Serial.println(_debugOutput);
#endif
#endif

				if (inverterReady && inverterSerialKnown()) {
					snprintf(subscriptionDef, sizeof(subscriptionDef), "%s/+/+/command", deviceName);
					subscribed = subscribed && _mqtt.subscribe(subscriptionDef, MQTT_SUBSCRIBE_QOS);
#ifdef DEBUG_OVER_SERIAL
					snprintf(_debugOutput, sizeof(_debugOutput), "Subscribed to \"%s\" : %d", subscriptionDef, subscribed);
					Serial.println(_debugOutput);
#endif
					inverterSubscriptionsAdded = true;
				}

			// Subscribe or resubscribe to topics.
			if (subscribed) {
#ifdef DEBUG_OVER_SERIAL
				Serial.printf("mqttReconnect attempt %d succeeded after %lu ms\r\n", tries, millis() - attemptStart);
#endif
				setStatusLedColor(0, 255, 0);
				updateRunstate();
				publishPollingConfig();
				if (inverterSubscriptionsAdded) {
					inverterSubscriptionsSet = true;
				}
				return;
			}
		}

#ifdef DEBUG_OVER_SERIAL
		sprintf(_debugOutput, "MQTT Failed: RC is %d\r\nTrying again in five seconds...", _mqtt.state());
		Serial.println(_debugOutput);
		Serial.printf("mqttReconnect attempt %d failed after %lu ms\r\n", tries, millis() - attemptStart);
#endif
		// Ensure we don't hold onto a half-open TCP session between attempts.
		_wifi.stop();
		return;
}

const mqttState *
lookupSubscription(char *entityName)
{
	const mqttState *entities = mqttEntitiesDesc();
	int numberOfEntities = static_cast<int>(mqttEntitiesCount());
	for (int i = 0; i < numberOfEntities; i++) {
		if (entities[i].subscribe &&
		    mqttEntityNameEquals(&entities[i], entityName)) {
			return &entities[i];
		}
	}
	return NULL;
}

const mqttState *
lookupEntity(mqttEntityId entityId)
{
	const mqttState *entities = mqttEntitiesDesc();
	int numberOfEntities = static_cast<int>(mqttEntitiesCount());
	for (int i = 0; i < numberOfEntities; i++) {
		if (entities[i].entityId == entityId) {
			return &entities[i];
		}
	}
	return NULL;
}

modbusRequestAndResponseStatusValues
readEntity(const mqttState *singleEntity, modbusRequestAndResponse* rs)
{
	modbusRequestAndResponseStatusValues result = modbusRequestAndResponseStatusValues::preProcessing;

	rs->dataValueFormatted[0] = 0;

	switch (singleEntity->entityId) {
	case mqttEntityId::entityRegValue:
#ifdef DEBUG_NO_RS485
		sprintf(rs->dataValueFormatted, "%ld", regNumberToRead);  // Just return the register #
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
		if (regNumberToRead < 0) {
			sprintf(rs->dataValueFormatted, "%s", "Nothing read");
			result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		} else {
			result = _registerHandler->readHandledRegister(regNumberToRead, rs);
			switch (result) {
			case modbusRequestAndResponseStatusValues::readDataRegisterSuccess:
				if (!strcmp(rs->dataValueFormatted, "Unknown")) {
					switch (rs->returnDataType) {
					case modbusReturnDataType::character:
						sprintf(rs->dataValueFormatted, "Unknown (%s)", rs->characterValue);
						break;
					case modbusReturnDataType::signedInt:
						sprintf(rs->dataValueFormatted, "Unknown (%ld)", rs->signedIntValue);
						break;
					case modbusReturnDataType::signedShort:
						sprintf(rs->dataValueFormatted, "Unknown (%d)", rs->signedShortValue);
						break;
					case modbusReturnDataType::unsignedInt:
						sprintf(rs->dataValueFormatted, "Unknown (%lu)", rs->unsignedIntValue);
						break;
					case modbusReturnDataType::unsignedShort:
						sprintf(rs->dataValueFormatted, "Unknown (%u)", rs->unsignedShortValue);
						break;
					case modbusReturnDataType::notDefined:
						sprintf(rs->dataValueFormatted, "Unknown (XX)");
						break;
					}
				}
				break;
			case modbusRequestAndResponseStatusValues::notHandledRegister:
				strcpy(rs->dataValueFormatted, "Invalid register");
				break;
			case modbusRequestAndResponseStatusValues::noResponse:
				strcpy(rs->dataValueFormatted, "No response");
				break;
			case modbusRequestAndResponseStatusValues::responseTooShort:
				strcpy(rs->dataValueFormatted, "Resp too short");
				break;
			case modbusRequestAndResponseStatusValues::slaveError:
				strcpy(rs->dataValueFormatted, "Slave Error");
				break;
			case modbusRequestAndResponseStatusValues::invalidFrame:
				strcpy(rs->dataValueFormatted, "Invalid Frame");
				break;
			default:
				sprintf(rs->dataValueFormatted, "Unexpected result: %d", result);
				break;
			}
			result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		}
#endif // DEBUG_NO_RS485
		break;
	case mqttEntityId::entityRegNum:
		sprintf(rs->dataValueFormatted, "%ld", regNumberToRead);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entityGridReg:
#ifdef DEBUG_NO_RS485
		sprintf(rs->dataValueFormatted, "%s", GRID_REGULATION_AL_17_DESC);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
		result = _registerHandler->readHandledRegister(REG_SAFETY_TEST_RW_GRID_REGULATION, rs);
#endif // DEBUG_NO_RS485
		break;
	case mqttEntityId::entityBatCap:
#ifdef DEBUG_NO_RS485
		sprintf(rs->dataValueFormatted, "%0.02f", 41 * BATTERY_KWH_MULTIPLIER);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
		result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_CAPACITY, rs);
#endif // DEBUG_NO_RS485
		break;
	case mqttEntityId::entityInverterTemp:
#ifdef DEBUG_NO_RS485
		sprintf(rs->dataValueFormatted, "%0.02f", 2750 * INVERTER_TEMP_MULTIPLIER);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
		result = _registerHandler->readHandledRegister(REG_INVERTER_HOME_R_INVERTER_TEMP, rs);
#endif // DEBUG_NO_RS485
		break;
	case mqttEntityId::entityBatTemp:
#ifdef DEBUG_NO_RS485
		sprintf(rs->dataValueFormatted, "%0.02f", 2750 * BATTERY_TEMP_MULTIPLIER);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
		result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_MAX_CELL_TEMPERATURE, rs);
#endif // DEBUG_NO_RS485
		break;
	case mqttEntityId::entityBatFaults:
		{
			unsigned int count = 0, bf, bf1, bf2, bf3, bf4, bf5, bf6;
#ifdef DEBUG_NO_RS485
			static int flipFlop = 0;
			if (flipFlop == 0) {
				bf = bf1 = bf2 = bf3 = bf4 = bf5 = bf6 = 0;
				flipFlop = 1;
			} else {
				bf = 0x0; bf1 = 0x1; bf2 = 0x2; bf3 = 0x3; bf4 = 0x4; bf5 = 0x5; bf6 = 0x6;
				flipFlop = 0;
			}
			result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
			result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_FAULT_1, rs);
			bf = rs->unsignedIntValue;
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_FAULT_1_1, rs);
				bf1 = rs->unsignedIntValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_FAULT_2_1, rs);
				bf2 = rs->unsignedIntValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_FAULT_3_1, rs);
				bf3 = rs->unsignedIntValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_FAULT_4_1, rs);
				bf4 = rs->unsignedIntValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_FAULT_5_1, rs);
				bf5 = rs->unsignedIntValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_FAULT_6_1, rs);
				bf6 = rs->unsignedIntValue;
			}
#endif // DEBUG_NO_RS485
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				count = popcount(bf) + popcount(bf1) + popcount(bf2) + popcount(bf3) +
					popcount(bf4) + popcount(bf5) + popcount(bf6);
				sprintf(rs->dataValueFormatted, "{ \"numEvents\": %u, "
								  "\"Battery Faults (0x%04X)\": \"0x%08X\","
								  "\"Battery Faults 1 (0x%04X)\": \"0x%08X\","
								  "\"Battery Faults 2 (0x%04X)\": \"0x%08X\","
								  "\"Battery Faults 3 (0x%04X)\": \"0x%08X\","
								  "\"Battery Faults 4 (0x%04X)\": \"0x%08X\","
								  "\"Battery Faults 5 (0x%04X)\": \"0x%08X\","
								  "\"Battery Faults 6 (0x%04X)\": \"0x%08X\" }",
					count, REG_BATTERY_HOME_R_BATTERY_FAULT_1, bf, REG_BATTERY_HOME_R_BATTERY_FAULT_1_1, bf1,
					REG_BATTERY_HOME_R_BATTERY_FAULT_2_1, bf2, REG_BATTERY_HOME_R_BATTERY_FAULT_3_1, bf3,
					REG_BATTERY_HOME_R_BATTERY_FAULT_4_1, bf4, REG_BATTERY_HOME_R_BATTERY_FAULT_5_1, bf5,
					REG_BATTERY_HOME_R_BATTERY_FAULT_6_1, bf6);
			}
		}
		break;
	case mqttEntityId::entityBatWarnings:
		{
			unsigned int count = 0, bw, bw1, bw2, bw3, bw4, bw5, bw6;
#ifdef DEBUG_NO_RS485
			bw = 0x10; bw1 = 0x11; bw2 = 0x12; bw3 = 0x13; bw4 = 0x14; bw5 = 0x15; bw6 = 0x16;
			result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
			result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_WARNING_1, rs);
			bw = rs->unsignedIntValue;
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_WARNING_1_1, rs);
				bw1 = rs->unsignedIntValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_WARNING_2_1, rs);
				bw2 = rs->unsignedIntValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_WARNING_3_1, rs);
				bw3 = rs->unsignedIntValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_WARNING_4_1, rs);
				bw4 = rs->unsignedIntValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_WARNING_5_1, rs);
				bw5 = rs->unsignedIntValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_WARNING_6_1, rs);
				bw6 = rs->unsignedIntValue;
			}
#endif // DEBUG_NO_RS485
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				count = popcount(bw) + popcount(bw1) + popcount(bw2) + popcount(bw3) +
					popcount(bw4) + popcount(bw5) + popcount(bw6);
				sprintf(rs->dataValueFormatted, "{ \"numEvents\": %u, "
								  "\"Battery Warnings (0x%04X)\": \"0x%08X\","
								  "\"Battery Warnings 1 (0x%04X)\": \"0x%08X\","
								  "\"Battery Warnings 2 (0x%04X)\": \"0x%08X\","
								  "\"Battery Warnings 3 (0x%04X)\": \"0x%08X\","
								  "\"Battery Warnings 4 (0x%04X)\": \"0x%08X\","
								  "\"Battery Warnings 5 (0x%04X)\": \"0x%08X\","
								  "\"Battery Warnings 6 (0x%04X)\": \"0x%08X\" }",
					count, REG_BATTERY_HOME_R_BATTERY_WARNING_1, bw, REG_BATTERY_HOME_R_BATTERY_WARNING_1_1, bw1,
					REG_BATTERY_HOME_R_BATTERY_WARNING_2_1, bw2, REG_BATTERY_HOME_R_BATTERY_WARNING_3_1, bw3,
					REG_BATTERY_HOME_R_BATTERY_WARNING_4_1, bw4, REG_BATTERY_HOME_R_BATTERY_WARNING_5_1, bw5,
					REG_BATTERY_HOME_R_BATTERY_WARNING_6_1, bw6);
			}
		}
		break;
	case mqttEntityId::entityInverterFaults:
		{
			unsigned int count = 0, if1, if2;
#ifdef EMS_35_36
			unsigned int ife1, ife2, ife3, ife4;
#endif // EMS_35_36
#ifdef DEBUG_NO_RS485
			if1 = 0x21; if2 = 0x22;
#ifdef EMS_35_36
			ife1 = 0x23; ife2 = 0x24; ife3 = 0x25; ife4 = 0x26;
#endif // EMS_35_36
			result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
			result = _registerHandler->readHandledRegister(REG_INVERTER_HOME_R_INVERTER_FAULT_1_1, rs);
			if1 = rs->unsignedIntValue;
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_INVERTER_HOME_R_INVERTER_FAULT_2_1, rs);
				if2 = rs->unsignedIntValue;
			}
#ifdef EMS_35_36
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_INVERTER_HOME_R_INVERTER_FAULT_EXTEND_1_1, rs);
				ife1 = rs->unsignedIntValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_INVERTER_HOME_R_INVERTER_FAULT_EXTEND_2_1, rs);
				ife2 = rs->unsignedIntValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_INVERTER_HOME_R_INVERTER_FAULT_EXTEND_3_1, rs);
				ife3 = rs->unsignedIntValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_INVERTER_HOME_R_INVERTER_FAULT_EXTEND_4_1, rs);
				ife4 = rs->unsignedIntValue;
			}
#endif // EMS_35_36
#endif // DEBUG_NO_RS485
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				count = popcount(if1) + popcount(if2);
#ifdef EMS_35_36
				count += popcount(ife1) + popcount(ife2) + popcount(ife3) + popcount(ife4);
#endif // EMS_35_36
				sprintf(rs->dataValueFormatted, "{ \"numEvents\": %u, "
								  "\"Inverter Faults 1 (0x%04X)\": \"0x%08X\","
								  "\"Inverter Faults 2 (0x%04X)\": \"0x%08X\""
#ifdef EMS_35_36
								  ", \"Inverter Faults Extended 1 (0x%04X)\": \"0x%08X\""
								  ", \"Inverter Faults Extended 2 (0x%04X)\": \"0x%08X\""
								  ", \"Inverter Faults Extended 3 (0x%04X)\": \"0x%08X\""
								  ", \"Inverter Faults Extended 4 (0x%04X)\": \"0x%08X\""
#endif // EMS_35_36
								  " }",
					count, REG_INVERTER_HOME_R_INVERTER_FAULT_1_1, if1, REG_INVERTER_HOME_R_INVERTER_FAULT_2_1, if2
#ifdef EMS_35_36
					, REG_INVERTER_HOME_R_INVERTER_FAULT_EXTEND_1_1, ife1, REG_INVERTER_HOME_R_INVERTER_FAULT_EXTEND_2_1, ife2,
					REG_INVERTER_HOME_R_INVERTER_FAULT_EXTEND_3_1, ife3, REG_INVERTER_HOME_R_INVERTER_FAULT_EXTEND_4_1, ife4
#endif // EMS_35_36
					);
			}
		}
		break;
	case mqttEntityId::entityInverterWarnings:
		{
			unsigned int count = 0, iw1, iw2;
#ifdef DEBUG_NO_RS485
			iw1 = 0x41; iw2 = 0x42;
			result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
			result = _registerHandler->readHandledRegister(REG_INVERTER_HOME_R_INVERTER_WARNING_1_1, rs);
			iw1 = rs->unsignedIntValue;
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_INVERTER_HOME_R_INVERTER_WARNING_2_1, rs);
				iw2 = rs->unsignedIntValue;
			}
#endif // DEBUG_NO_RS485
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				count = popcount(iw1) + popcount(iw2);
				sprintf(rs->dataValueFormatted, "{ \"numEvents\": %u, "
								  "\"Inverter Warnings 1 (0x%04X)\": \"0x%08X\","
								  "\"Inverter Warnings 2 (0x%04X)\": \"0x%08X\" }",
					count, REG_INVERTER_HOME_R_INVERTER_WARNING_1_1, iw1, REG_INVERTER_HOME_R_INVERTER_WARNING_2_1, iw2);
			}
		}
		break;
	case mqttEntityId::entitySystemFaults:
		{
			unsigned int count = 0, sf, sf1;
#ifdef DEBUG_NO_RS485
			sf = 0x50; sf1 = 0x51;
			result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
			result = _registerHandler->readHandledRegister(REG_SYSTEM_INFO_R_SYSTEM_FAULT, rs);
			sf = rs->unsignedIntValue;
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_SYSTEM_OP_R_SYSTEM_FAULT_1, rs);
				sf1 = rs->unsignedIntValue;
			}
#endif // DEBUG_NO_RS485
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				count = popcount(sf) + popcount(sf1);
				sprintf(rs->dataValueFormatted, "{ \"numEvents\": %u, "
								  "\"System Faults (0x%04X)\": \"0x%08X\","
								  "\"System Faults 1 (0x%04X)\": \"0x%08X\" }",
					count, REG_SYSTEM_INFO_R_SYSTEM_FAULT, sf, REG_SYSTEM_OP_R_SYSTEM_FAULT_1, sf1);
			}
		}
		break;
	case mqttEntityId::entityPushPwr:
		sprintf(rs->dataValueFormatted, "%ld", opData.a2mPwrPush);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entityDischargePwr:
		sprintf(rs->dataValueFormatted, "%ld", opData.a2mPwrDischarge);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entityChargePwr:
		sprintf(rs->dataValueFormatted, "%ld", opData.a2mPwrCharge);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entitySocTarget:
		sprintf(rs->dataValueFormatted, "%u", opData.a2mSocTarget);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entityOpMode:
		getOpModeDesc(rs->dataValueFormatted, sizeof(rs->dataValueFormatted), opData.a2mOpMode);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entityPvEnergy:
#ifdef DEBUG_NO_RS485
		sprintf(rs->dataValueFormatted, "%0.02f", 3399 * TOTAL_ENERGY_MULTIPLIER);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
		result = _registerHandler->readHandledRegister(REG_SYSTEM_OP_R_SYSTEM_TOTAL_PV_ENERGY_1, rs);
#endif // DEBUG_NO_RS485
		break;
	case mqttEntityId::entityFrequency:
		{
			uint16_t gf, pf, if1, if2, ibf, uf;
#ifdef DEBUG_NO_RS485
			gf = 6110; pf = 6120; if1 = 6130; if2 = 6140; ibf = 6150;
			result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
			result = _registerHandler->readHandledRegister(REG_GRID_METER_R_FREQUENCY, rs);
			gf = rs->unsignedShortValue;
			pf = 0; if1 = 0; if2 = 0; ibf = 0;
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_PV_METER_R_FREQUENCY, rs);
				pf = rs->unsignedShortValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_INVERTER_HOME_R_FREQUENCY, rs);
				if1 = rs->unsignedShortValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_INVERTER_HOME_R_INVERTER_FREQUENCY, rs);
				if2 = rs->unsignedShortValue;
			}
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				result = _registerHandler->readHandledRegister(REG_INVERTER_HOME_R_INVERTER_BACKUP_FREQUENCY, rs);
				ibf = rs->unsignedShortValue;
			}
#endif // DEBUG_NO_RS485
			uf = pf;
			if (uf == 0) uf = gf;
			if (uf == 0) uf = if1;
			if (uf == 0) uf = if2;
			if (uf == 0) uf = ibf;
			sprintf(rs->dataValueFormatted, "{ \"Use Frequency\": %0.02f, "
				"\"Grid Frequency\": %0.02f, "
				"\"PV Frequency\": %0.02f, "
				"\"Inverter Frequency 1\": %0.02f, "
				"\"Inverter Frequency 2\": %0.02f, "
				"\"Inverter Backup Frequency\": %0.02f }",
				uf * FREQUENCY_MULTIPLIER, gf * FREQUENCY_MULTIPLIER, pf * FREQUENCY_MULTIPLIER,
				if1 * FREQUENCY_MULTIPLIER, if2 * FREQUENCY_MULTIPLIER, ibf * FREQUENCY_MULTIPLIER);
		}
		break;
	case mqttEntityId::entityInverterMode:
		getInverterModeDesc(rs->dataValueFormatted, sizeof(rs->dataValueFormatted), opData.essInverterMode);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entityPvPwr:
		if (opData.essPvPower == INT32_MAX) {
			result = modbusRequestAndResponseStatusValues::readDataInvalidValue;
		} else {
			sprintf(rs->dataValueFormatted, "%ld", opData.essPvPower);
			result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		}
		break;
	case mqttEntityId::entityGridEnergyTo:
#ifdef DEBUG_NO_RS485
		if (isGridOnline() == gridStatus::gridOnline) {
			rs->unsignedIntValue = 4477;
		} else {
			rs->unsignedIntValue = 0;
		}
		sprintf(rs->dataValueFormatted, "%lu", rs->unsignedIntValue);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
		result = _registerHandler->readHandledRegister(REG_GRID_METER_R_TOTAL_ENERGY_FEED_TO_GRID_1, rs);
#endif // DEBUG_NO_RS485
		if ((result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) &&
		    (rs->unsignedIntValue == 0)) {
			// When grid is off, this reports 0 instead of a 0 delta.  0 is invalid.
			result = modbusRequestAndResponseStatusValues::readDataInvalidValue;
		}
		break;
	case mqttEntityId::entityGridEnergyFrom:
#ifdef DEBUG_NO_RS485
		if (isGridOnline() == gridStatus::gridOnline) {
			rs->unsignedIntValue = 4488;
		} else {
			rs->unsignedIntValue = 0;
		}
		sprintf(rs->dataValueFormatted, "%lu", rs->unsignedIntValue);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
		result = _registerHandler->readHandledRegister(REG_GRID_METER_R_TOTAL_ENERGY_CONSUMED_FROM_GRID_1, rs);
#endif // DEBUG_NO_RS485
		if ((result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) &&
		    (rs->unsignedIntValue == 0)) {
			// When grid is off, this reports 0 instead of a 0 delta.  0 is invalid.
			result = modbusRequestAndResponseStatusValues::readDataInvalidValue;
		}
		break;
	case mqttEntityId::entityGridPwr:
		if (opData.essGridPower == INT32_MAX) {
			result = modbusRequestAndResponseStatusValues::readDataInvalidValue;
		} else {
			sprintf(rs->dataValueFormatted, "%ld", opData.essGridPower);
			result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		}
		break;
	case mqttEntityId::entityGridAvail:
		// Nothing to write.  Included already in statusTopic
		result = modbusRequestAndResponseStatusValues::preProcessing;
		break;
	case mqttEntityId::entityBatEnergyCharge:
#ifdef DEBUG_NO_RS485
		sprintf(rs->dataValueFormatted, "%u", 5599);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
		result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_CHARGE_ENERGY_1, rs);
#endif // DEBUG_NO_RS485
		break;
	case mqttEntityId::entityBatEnergyDischarge:
#ifdef DEBUG_NO_RS485
		sprintf(rs->dataValueFormatted, "%u", 5588);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
		result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_DISCHARGE_ENERGY_1, rs);
#endif // DEBUG_NO_RS485
		break;
	case mqttEntityId::entityBatPwr:
		if (opData.essBatteryPower == INT16_MAX) {
			result = modbusRequestAndResponseStatusValues::readDataInvalidValue;
		} else {
			sprintf(rs->dataValueFormatted, "%d", opData.essBatteryPower);
			result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		}
		break;
	case mqttEntityId::entityBatSoc:
		if (opData.essBatterySoc == UINT16_MAX) {
			result = modbusRequestAndResponseStatusValues::readDataInvalidValue;
		} else {
			sprintf(rs->dataValueFormatted, "%0.02f", opData.essBatterySoc * BATTERY_SOC_MULTIPLIER);
			result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		}
		break;
#ifdef DEBUG_CALLBACKS
	case mqttEntityId::entityCallbacks:
		sprintf(rs->dataValueFormatted, "%lu", receivedCallbacks);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
#endif // DEBUG_CALLBACKS
	case mqttEntityId::entityRs485Errors:
		sprintf(rs->dataValueFormatted, "%lu", rs485Errors);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
#ifdef DEBUG_FREEMEM
	case mqttEntityId::entityFreemem:
		sprintf(rs->dataValueFormatted, "%lu", freeMemory());
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
#endif // DEBUG_FREEMEM
	case mqttEntityId::entityRs485Avail:
		// Nothing to write.  Included already in statusTopic
		result = modbusRequestAndResponseStatusValues::preProcessing;
		break;
	case mqttEntityId::entityA2MUptime:
		sprintf(rs->dataValueFormatted, "%lu", getUptimeSeconds());
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entityA2MVersion:
		sprintf(rs->dataValueFormatted, "%s", _version);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entityPollingBudgetExceeded:
	case mqttEntityId::entityPollingBudgetOverrunCount:
		if (formatPollingBudgetEntityValue(singleEntity->entityId, rs->dataValueFormatted, sizeof(rs->dataValueFormatted))) {
			result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		}
		break;
	case mqttEntityId::entityInverterSn:
#ifdef DEBUG_NO_RS485
		sprintf(rs->dataValueFormatted, "%s", "fake-inv-sn");
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
		result = _registerHandler->readHandledRegister(REG_INVERTER_INFO_R_SERIAL_NUMBER_1, rs);
#endif // DEBUG_NO_RS485
		break;
	case mqttEntityId::entityInverterVersion:
#ifdef DEBUG_NO_RS485
		sprintf(rs->dataValueFormatted, "%s", "fake.inv.ver");
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
		result = _registerHandler->readHandledRegister(REG_INVERTER_INFO_R_MASTER_SOFTWARE_VERSION_1, rs);
		if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
			char master[64], slave[64];
			strlcpy(master, rs->dataValueFormatted, sizeof(master));
			result = _registerHandler->readHandledRegister(REG_INVERTER_INFO_R_SLAVE_SOFTWARE_VERSION_1, rs);
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				strlcpy(slave, rs->dataValueFormatted, sizeof(slave));
				snprintf(rs->dataValueFormatted, sizeof(rs->dataValueFormatted), "%s-%s", master, slave);
			}
		}
#endif // DEBUG_NO_RS485
		break;
	case mqttEntityId::entityEmsSn:
		sprintf(rs->dataValueFormatted, "%s", deviceSerialNumber);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entityEmsVersion:
#ifdef DEBUG_NO_RS485
		sprintf(rs->dataValueFormatted, "%s", "fake.ems.ver");
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
		result = _registerHandler->readHandledRegister(REG_SYSTEM_INFO_R_EMS_VERSION_HIGH, rs);
		if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
			char high[8], middle[8], low[8];
			strlcpy(high, rs->dataValueFormatted, sizeof(high));
			result = _registerHandler->readHandledRegister(REG_SYSTEM_INFO_R_EMS_VERSION_MIDDLE, rs);
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				strlcpy(middle, rs->dataValueFormatted, sizeof(middle));
				result = _registerHandler->readHandledRegister(REG_SYSTEM_INFO_R_EMS_VERSION_LOW, rs);
				if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
					strlcpy(low, rs->dataValueFormatted, sizeof(low));
					snprintf(rs->dataValueFormatted, sizeof(rs->dataValueFormatted), "%s.%s.%s", high, middle, low);
				}
			}
		}
#endif // DEBUG_NO_RS485
		break;
#ifdef A2M_DEBUG_WIFI
	case mqttEntityId::entityRSSI:
		sprintf(rs->dataValueFormatted, "%ld", WiFi.RSSI());
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entityBSSID:
		{
			byte *bssid = WiFi.BSSID();
			sprintf(rs->dataValueFormatted, "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
		}
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entityTxPower:
#if defined(MP_ESP32)
		sprintf(rs->dataValueFormatted, "%0.1f", WiFi.getTxPower() / 4.0f);
#else
		sprintf(rs->dataValueFormatted, "%0.1f", wifiPower);
#endif
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entityWifiRecon:
		sprintf(rs->dataValueFormatted, "%lu", wifiReconnects);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
#endif // A2M_DEBUG_WIFI
	default:
		if (formatPollingBudgetEntityValue(singleEntity->entityId, rs->dataValueFormatted, sizeof(rs->dataValueFormatted))) {
			result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
			break;
		}
		if (singleEntity->readKind == MqttEntityReadKind::Register ||
		    singleEntity->readKind == MqttEntityReadKind::Identity) {
#ifdef DEBUG_NO_RS485
			snprintf(rs->dataValueFormatted, sizeof(rs->dataValueFormatted), "%u",
			         static_cast<unsigned>(singleEntity->readKey));
			result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else // DEBUG_NO_RS485
			if (_registerHandler != nullptr) {
				result = _registerHandler->readHandledRegister(singleEntity->readKey, rs);
			}
#endif // DEBUG_NO_RS485
		}
		break;
	}

	if ((result != modbusRequestAndResponseStatusValues::readDataInvalidValue) &&
	    (result != modbusRequestAndResponseStatusValues::readDataRegisterSuccess)) {
		rs485Errors++;
#ifdef DEBUG_OVER_SERIAL
		char entityName[64];
		mqttEntityNameCopy(singleEntity, entityName, sizeof(entityName));
		snprintf(_debugOutput, sizeof(_debugOutput), "Failed to read register: %s, Result = %d", entityName, result);
		Serial.println(_debugOutput);
#endif
	}

	return result;
}

/*
 * addState
 *
 * Query the handled entity in the usual way, and add the cleansed output to the buffer
 */
modbusRequestAndResponseStatusValues
addState(const mqttState *singleEntity, modbusRequestAndResponseStatusValues *resultAddedToPayload)
{
	modbusRequestAndResponse response;
	modbusRequestAndResponseStatusValues result;

	// Read the register(s)/data
	result = readEntity(singleEntity, &response);

	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		// Let the onward process also know if the buffer failed.
		*resultAddedToPayload = addToPayload(response.dataValueFormatted);
	} else {
		*resultAddedToPayload = modbusRequestAndResponseStatusValues::preProcessing;
	}
	return result;
}

	void
		sendStatus(bool includeEssSnapshot)
		{
			// Keep large buffers out of the stack to avoid soft WDT resets on ESP8266.
			static char stateAddition[256];
			static char netAddition[256];
			static char pollAddition[1024];
			static char stubAddition[512];
		StatusCoreSnapshot core{};
		StatusNetSnapshot net{};
		StatusPollSnapshot poll{};
#if RS485_STUB
		StatusStubSnapshot stub{};
#endif
	const char *gridStatusStr;
	modbusRequestAndResponseStatusValues resultAddedToPayload;
	char ssidBuf[33];
	char ipBuf[16];
	strlcpy(ssidBuf, appConfig.wifiSSID.c_str(), sizeof(ssidBuf));
	const IPAddress localIp = WiFi.localIP();
	snprintf(ipBuf, sizeof(ipBuf), "%u.%u.%u.%u", localIp[0], localIp[1], localIp[2], localIp[3]);
	const bool essSnapshotOkNow = includeEssSnapshot && essSnapshotValid;

	if (essSnapshotOkNow) {
		switch (isGridOnline()) {
		case gridStatus::gridOnline:
			gridStatusStr = "OK";
			break;
		case gridStatus::gridOffline:
			gridStatusStr = "Problem";
			break;
		case gridStatus::gridUnknown:
		default:
			gridStatusStr = "unknown";
			break;
		}
	} else {
		gridStatusStr = "unknown";
	}

	emptyPayload();

	core.presence = "online";
	core.a2mStatus = "online";
	if (essSnapshotOkNow) {
		core.rs485Status = opData.essRs485Connected ? "OK" : "Problem";
	} else {
		core.rs485Status = "unknown";
	}
	core.gridStatus = gridStatusStr;
	core.bootMode = bootModeToString(currentBootMode);
	core.bootIntent = bootIntentToString(currentBootIntent);
	core.httpControlPlaneEnabled = httpControlPlaneEnabled;
	core.haUniqueId = haUniqueId;

	net.uptimeS = getUptimeSeconds();
	net.freeHeap = ESP.getFreeHeap();
	net.rssiDbm = WiFi.RSSI();
	net.ip = ipBuf;
	net.ssid = ssidBuf;
	net.mqttConnected = _mqtt.connected();
	net.mqttReconnects = mqttReconnectCount;
	net.wifiStatus = wifiStatusLabel(WiFi.status());
	net.wifiStatusCode = static_cast<int>(WiFi.status());
	net.wifiReconnects = wifiReconnectCount;

	poll.inverterReady = inverterReady;
	poll.essSnapshotOk = essSnapshotOkNow;
	poll.pollOkCount = pollOkCount;
	{
		const MemSample sample = readMemSample();
		const MemLevel runtimeLevel = evaluateRuntimeMem(sample);
		poll.heapFreeB = sample.freeB;
		poll.heapMaxBlockB = sample.maxBlockB;
		poll.heapFragPct = sample.fragPct;
		poll.memLevel = static_cast<uint8_t>(runtimeLevel);
		poll.bootHeapLevel = static_cast<uint8_t>(bootMemWorst.level);
		poll.bootHeapStage = static_cast<uint8_t>(bootMemWorst.stage);
		poll.bootHeapFreeB = bootMemWorst.sample.freeB;
		poll.bootHeapMaxBlockB = bootMemWorst.sample.maxBlockB;
		poll.bootHeapFragPct = bootMemWorst.sample.fragPct;
	}
		poll.pollErrCount = pollErrCount;
		poll.lastPollMs = lastPollMs;
		poll.lastOkTsMs = lastOkTsMs;
		poll.lastErrTsMs = lastErrTsMs;
		poll.lastErrCode = lastErrCode;
		poll.rs485ProbeLastAttemptMs = rs485ProbeLastAttemptMs;
		poll.rs485ProbeBackoffMs = (rs485ConnectState == Rs485ConnectState::Connected) ? 0 : rs485CycleBackoffMs;
		poll.rs485Backend =
#if RS485_STUB
				"stub";
#else
				"real";
#endif
		poll.essSnapshotLastOk = essSnapshotLastOk;
		poll.essSnapshotAttempts = essSnapshotAttemptCount;
#if RS485_STUB
			poll.rs485StubMode = _modBus ? _modBus->stubModeLabel() : "uninit";
			poll.rs485StubFailRemaining = _modBus ? _modBus->stubFailRemaining() : 0;
			poll.rs485StubWriteCount = _modBus ? _modBus->stubWriteCount() : 0;
			poll.rs485StubLastWriteStartReg = _modBus ? _modBus->stubLastWriteStartReg() : 0;
			poll.rs485StubLastWriteMs = _modBus ? _modBus->stubLastWriteMs() : 0;
#else
			poll.rs485StubMode = "";
			poll.rs485StubFailRemaining = 0;
			poll.rs485StubWriteCount = 0;
			poll.rs485StubLastWriteStartReg = 0;
			poll.rs485StubLastWriteMs = 0;
#endif
			poll.dispatchLastRunMs = dispatchLastRunMs;
			poll.dispatchLastSkipReason = dispatchLastSkipReason;
			poll.schedTenSecLastRunMs = schedTenSecLastRunMs;
			poll.schedOneMinLastRunMs = schedOneMinLastRunMs;
			poll.schedFiveMinLastRunMs = schedFiveMinLastRunMs;
			poll.schedOneHourLastRunMs = schedOneHourLastRunMs;
			poll.schedOneDayLastRunMs = schedOneDayLastRunMs;
			poll.pollIntervalSeconds = pollIntervalSeconds;
			poll.schedUserLastRunMs = schedUserLastRunMs;
			poll.schedTenSecCount = schedTenSecCount;
			poll.schedOneMinCount = schedOneMinCount;
			poll.schedFiveMinCount = schedFiveMinCount;
			poll.schedOneHourCount = schedOneHourCount;
			poll.schedOneDayCount = schedOneDayCount;
			poll.schedUserCount = schedUserCount;
			poll.persistLoadOk = persistLoadOk;
			poll.persistLoadErr = persistLoadErr;
			poll.persistUnknownEntityCount = persistUnknownEntityCount;
			poll.persistInvalidBucketCount = persistInvalidBucketCount;
			poll.persistDuplicateEntityCount = persistDuplicateEntityCount;
			poll.pollingBudgetExceeded = pollingBudgetExceeded();
			poll.pollingBudgetOverrunCount = pollingBudgetOverrunCount;
			const uint32_t nowMs = millis();
			for (size_t bucketIdx = 0; bucketIdx < (sizeof(kRuntimeBuckets) / sizeof(kRuntimeBuckets[0])); ++bucketIdx) {
				const BucketRuntimeBudgetState &budgetState = schedBudgetState[bucketIdx];
				poll.pollingBudgetUsedMs[bucketIdx] = budgetState.usedMsLast;
				poll.pollingBudgetLimitMs[bucketIdx] = budgetState.limitMsLast;
				poll.pollingBacklogCount[bucketIdx] = budgetState.backlogCount;
				poll.pollingBacklogOldestAgeMs[bucketIdx] = bucketBacklogOldestAgeMs(budgetState, nowMs);
				poll.pollingLastFullCycleAgeMs[bucketIdx] = bucketLastFullCycleAgeMs(budgetState, nowMs);
			}

			if (!buildStatusCoreJson(core, stateAddition, sizeof(stateAddition))) {
				return;
			}
	resultAddedToPayload = addToPayload(stateAddition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return;
	}

	sendMqtt(statusTopic, MQTT_RETAIN);
	maybeYield();
	publishControllerInverterSerialState();
	maybeYield();

	char netTopic[160];
	snprintf(netTopic, sizeof(netTopic), "%s/net", statusTopic);
	if (buildStatusNetJson(net, netAddition, sizeof(netAddition))) {
		_mqtt.publish(netTopic, netAddition, true);
		maybeYield();
	}

	char pollTopic[160];
	snprintf(pollTopic, sizeof(pollTopic), "%s/poll", statusTopic);
	bool pollBuilt = buildStatusPollJson(poll, pollAddition, sizeof(pollAddition));
	bool usedCompactPoll = false;
	if (!pollBuilt) {
		pollBuilt = buildStatusPollJsonCompact(poll, pollAddition, sizeof(pollAddition));
		usedCompactPoll = pollBuilt;
	}
	if (pollBuilt) {
		bool published = _mqtt.publish(pollTopic, pollAddition, true);
		if (!published && !usedCompactPoll && buildStatusPollJsonCompact(poll, pollAddition, sizeof(pollAddition))) {
			published = _mqtt.publish(pollTopic, pollAddition, true);
			usedCompactPoll = true;
		}
#ifdef DEBUG_OVER_SERIAL
		if (!published) {
			Serial.println("status/poll publish failed (full+compact)");
		}
#endif
		maybeYield();
	}

#if RS485_STUB
	if (_modBus != nullptr) {
		stub.stubReads = _modBus->stubReadCount();
		stub.stubWrites = _modBus->stubWriteCount();
		stub.stubUnknownReads = _modBus->stubUnknownRegisterReads();
		stub.socX10 = _modBus->stubBatterySocX10();
		stub.lastReadStartReg = _modBus->stubLastReadStartReg();
		stub.lastFn = _modBus->stubLastFn();
		stub.lastFailStartReg = _modBus->stubLastFailStartReg();
		stub.lastFailFn = _modBus->stubLastFailFn();
		stub.lastFailType = _modBus->stubLastFailTypeLabel();
		stub.latencyMs = _modBus->stubLatencyMs();
		stub.strictUnknown = _modBus->stubStrictUnknown();
		stub.failEveryN = _modBus->stubFailEveryN();
		stub.failForMs = _modBus->stubFailForMs();
		stub.flapOnlineMs = _modBus->stubFlapOnlineMs();
		stub.flapOfflineMs = _modBus->stubFlapOfflineMs();
		stub.probeAttempts = _modBus->stubProbeAttempts();
		stub.probeSuccessAfterN = _modBus->stubProbeSuccessAfterN();
		stub.socStepX10PerSnapshot = _modBus->stubSocStepX10PerSnapshot();

		char stubTopic[160];
		snprintf(stubTopic, sizeof(stubTopic), "%s/stub", statusTopic);
		if (buildStatusStubJson(stub, stubAddition, sizeof(stubAddition))) {
			_mqtt.publish(stubTopic, stubAddition, true);
			maybeYield();
		}
	}
#endif
}

modbusRequestAndResponseStatusValues
addConfig(const mqttState *singleEntity,
          DiscoveryDeviceScope scope,
          const char *topicBase,
          modbusRequestAndResponseStatusValues& resultAddedToPayload)
{
	// HA discovery generation runs on a constrained ESP8266 loop/callback stack.
	// Use the smaller row-render scratch here so the 2 KB bucket-map scratch remains
	// reserved for deferred `config/set` payloads while MQTT callbacks are active.
	char (&stateAddition)[sizeof(g_portalRowRenderBuf)] = g_portalRowRenderBuf;
	char prettyName[64];
	char uniqueId[128];
	const char *deviceId = discoveryDeviceIdForScope(scope);
	const bool inverterScope = (scope == DiscoveryDeviceScope::Inverter);
	char entityKey[64];
	stateAddition[0] = '\0';
	if (deviceId[0] == '\0' || topicBase == nullptr || topicBase[0] == '\0') {
		return modbusRequestAndResponseStatusValues::preProcessing;
	}
	mqttEntityNameCopy(singleEntity, entityKey, sizeof(entityKey));
	buildEntityUniqueId(scope,
	                    controllerIdentifier,
	                    deviceSerialNumber,
	                    entityKey,
	                    uniqueId,
	                    sizeof(uniqueId));

	sprintf(stateAddition, "{");
	resultAddedToPayload = addToPayload(stateAddition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return resultAddedToPayload;
	}

	switch (singleEntity->haClass) {
	case homeAssistantClass::haClassBox:
	case homeAssistantClass::haClassNumber:
		sprintf(stateAddition, "\"component\": \"number\"");
		break;
	case homeAssistantClass::haClassSelect:
		sprintf(stateAddition, "\"component\": \"select\"");
		break;
	case homeAssistantClass::haClassBinaryProblem:
		sprintf(stateAddition, "\"component\": \"binary_sensor\"");
		break;
	default:
		sprintf(stateAddition, "\"component\": \"sensor\"");
		break;
	}
	resultAddedToPayload = addToPayload(stateAddition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return resultAddedToPayload;
	}

	if (inverterScope) {
		snprintf(stateAddition, sizeof(stateAddition),
		         ", \"device\": {"
		         " \"name\": \"%s\", \"model\": \"%s\", \"manufacturer\": \"AlphaESS\","
		         " \"identifiers\": [\"%s\"], \"via_device\": \"%s\"}",
		         haUniqueId,
		         (deviceBatteryType[0] != '\0' ? deviceBatteryType : kInverterModelFallback),
		         deviceId,
		         controllerIdentifier);
	} else {
		snprintf(stateAddition, sizeof(stateAddition),
		         ", \"device\": {"
		         " \"name\": \"%s\", \"model\": \"%s\", \"manufacturer\": \"AlphaESS\","
		         " \"identifiers\": [\"%s\"]}",
		         deviceName,
		         kControllerModel,
		         deviceId);
	}
	resultAddedToPayload = addToPayload(stateAddition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return resultAddedToPayload;
	}

	strlcpy(prettyName, entityKey, sizeof(prettyName));
	while(char *ch = strchr(prettyName, '_')) {
		*ch = ' ';
	}
	snprintf(stateAddition, sizeof(stateAddition), ", \"name\": \"%s\"", prettyName);
	resultAddedToPayload = addToPayload(stateAddition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return resultAddedToPayload;
	}

	snprintf(stateAddition, sizeof(stateAddition), ", \"unique_id\": \"%s\"", uniqueId);
	resultAddedToPayload = addToPayload(stateAddition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return resultAddedToPayload;
	}

	switch (singleEntity->haClass) {
	case homeAssistantClass::haClassEnergy:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"device_class\": \"energy\""
			 ", \"state_class\": \"total_increasing\""
			 ", \"unit_of_measurement\": \"kWh\""
#ifdef MQTT_FORCE_UPDATE
			 ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			);
		break;
	case homeAssistantClass::haClassPower:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"device_class\": \"power\""
			 ", \"state_class\": \"measurement\""
			 ", \"unit_of_measurement\": \"W\""
#ifdef MQTT_FORCE_UPDATE
			 ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			);
		break;
	case homeAssistantClass::haClassFrequency:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"device_class\": \"frequency\""
			 ", \"state_class\": \"measurement\""
			 ", \"unit_of_measurement\": \"Hz\""
#ifdef MQTT_FORCE_UPDATE
			 ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			 ", \"entity_category\": \"diagnostic\"");
		break;
	case homeAssistantClass::haClassReactivePower:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"state_class\": \"measurement\""
			 ", \"unit_of_measurement\": \"var\""
#ifdef MQTT_FORCE_UPDATE
			 ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			 ", \"entity_category\": \"diagnostic\"");
		break;
	case homeAssistantClass::haClassApparentPower:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"state_class\": \"measurement\""
			 ", \"unit_of_measurement\": \"VA\""
#ifdef MQTT_FORCE_UPDATE
			 ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			 ", \"entity_category\": \"diagnostic\"");
		break;
	case homeAssistantClass::haClassPowerFactor:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"state_class\": \"measurement\""
#ifdef MQTT_FORCE_UPDATE
			 ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			 ", \"entity_category\": \"diagnostic\"");
		break;
	case homeAssistantClass::haClassBinaryProblem:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"device_class\": \"problem\""
			 ", \"payload_on\": \"Problem\""
			 ", \"payload_off\": \"OK\""
			 ", \"entity_category\": \"diagnostic\"");
		break;
	case homeAssistantClass::haClassBattery:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"device_class\": \"battery\""
			 ", \"state_class\": \"measurement\""
			 ", \"unit_of_measurement\": \"%%\""
#ifdef MQTT_FORCE_UPDATE
			 ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			);
		break;
	case homeAssistantClass::haClassVoltage:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"device_class\": \"voltage\""
			 ", \"state_class\": \"measurement\""
			 ", \"unit_of_measurement\": \"V\""
#ifdef MQTT_FORCE_UPDATE
			 ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			);
		break;
	case homeAssistantClass::haClassCurrent:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"device_class\": \"current\""
			 ", \"state_class\": \"measurement\""
			 ", \"unit_of_measurement\": \"A\""
#ifdef MQTT_FORCE_UPDATE
			 ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			);
		break;
	case homeAssistantClass::haClassTemp:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"device_class\": \"temperature\""
			 ", \"state_class\": \"measurement\""
			 ", \"unit_of_measurement\": \"°C\""
#ifdef MQTT_FORCE_UPDATE
//			 ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			 ", \"entity_category\": \"diagnostic\"");
		break;
	case homeAssistantClass::haClassDuration:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"device_class\": \"duration\""
			 ", \"state_class\": \"measurement\""
			 ", \"unit_of_measurement\": \"s\""
			 ", \"entity_category\": \"diagnostic\"");
		break;
	case homeAssistantClass::haClassBox:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"mode\": \"box\"");
		break;
	case homeAssistantClass::haClassInfo:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"entity_category\": \"diagnostic\"");
		break;
	case homeAssistantClass::haClassSelect:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"device_class\": \"enum\""
			);
		break;
	case homeAssistantClass::haClassNumber:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"entity_category\": \"diagnostic\""
			 ", \"entity_type\": \"number\"");
		break;
	default:
		strcpy(stateAddition, "");
		break;
	}
	if (strlen(stateAddition) != 0) {
		resultAddedToPayload = addToPayload(stateAddition);
		if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
			return resultAddedToPayload;
		}
	}

	stateAddition[0] = '\0';
	switch (singleEntity->entityId) {
	case mqttEntityId::entityRegNum:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"entity_category\": \"diagnostic\""
			 ", \"icon\": \"mdi:pound\""
			 ", \"min\": -1, \"max\": 41000");
		break;
	case mqttEntityId::entityRegValue:
		sprintf(stateAddition, ", \"icon\": \"mdi:folder-pound-outline\"");
		break;
	case mqttEntityId::entityGridReg:
		sprintf(stateAddition, ", \"icon\": \"mdi:security\"");
		break;
	case mqttEntityId::entityInverterMode:
		sprintf(stateAddition, ", \"icon\": \"mdi:format-list-numbered\"");
		break;
	case mqttEntityId::entityPvPwr:
		sprintf(stateAddition, ", \"icon\": \"mdi:solar-power\"");
		break;
	case mqttEntityId::entityPvEnergy:
		sprintf(stateAddition, ", \"icon\": \"mdi:solar-power-variant-outline\"");
		break;
	case mqttEntityId::entityFrequency:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"icon\": \"mdi:sine-wave\""
			 ", \"suggested_display_precision\": 2");
		break;
	case mqttEntityId::entityGridPwr:
		sprintf(stateAddition, ", \"icon\": \"mdi:transmission-tower\"");
		break;
	case mqttEntityId::entityGridEnergyTo:
		sprintf(stateAddition, ", \"icon\": \"mdi:transmission-tower-export\"");
		break;
	case mqttEntityId::entityGridEnergyFrom:
		sprintf(stateAddition, ", \"icon\": \"mdi:transmission-tower-import\"");
		break;
	case mqttEntityId::entityBatPwr:
		sprintf(stateAddition, ", \"icon\": \"mdi:battery-charging-100\"");
		break;
	case mqttEntityId::entityBatEnergyCharge:
		sprintf(stateAddition, ", \"icon\": \"mdi:battery-plus\"");
		break;
	case mqttEntityId::entityBatEnergyDischarge:
		sprintf(stateAddition, ", \"icon\": \"mdi:battery-minus\"");
		break;
	case mqttEntityId::entityBatCap:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"device_class\": \"energy\""
			 ", \"state_class\": \"total_increasing\""
			 ", \"unit_of_measurement\": \"kWh\""
			 ", \"icon\": \"mdi:home-battery\"");
		break;
	case mqttEntityId::entityOpMode:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"options\": [ \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\" ]",
			 OP_MODE_DESC_LOAD_FOLLOW, OP_MODE_DESC_TARGET, OP_MODE_DESC_PUSH,
			 OP_MODE_DESC_PV_CHARGE, OP_MODE_DESC_MAX_CHARGE, OP_MODE_DESC_NO_CHARGE);
		break;
	case mqttEntityId::entitySocTarget:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"device_class\": \"battery\""
			 ", \"state_class\": \"measurement\""
			 ", \"unit_of_measurement\": \"%%\""
			 ", \"icon\": \"mdi:battery\""
			 ", \"min\": %d, \"max\": %d",
			 SOC_TARGET_MIN, SOC_TARGET_MAX);
		break;
	case mqttEntityId::entityChargePwr:
	case mqttEntityId::entityDischargePwr:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"device_class\": \"power\""
			 ", \"state_class\": \"measurement\""
			 ", \"unit_of_measurement\": \"W\""
			 ", \"icon\": \"mdi:lightning-bolt-circle\""
			 ", \"min\": %d, \"max\": %d",
			 0, INVERTER_POWER_MAX);
		break;
	case mqttEntityId::entityPushPwr:
		snprintf(stateAddition, sizeof(stateAddition),
			 ", \"device_class\": \"power\""
			 ", \"state_class\": \"measurement\""
			 ", \"unit_of_measurement\": \"W\""
			 ", \"icon\": \"mdi:lightning-bolt-circle\""
			 ", \"min\": %d, \"max\": %d",
			 0, INVERTER_POWER_MAX);
		break;
#ifdef A2M_DEBUG_WIFI
	case mqttEntityId::entityRSSI:
	case mqttEntityId::entityBSSID:
	case mqttEntityId::entityTxPower:
	case mqttEntityId::entityWifiRecon:
		sprintf(stateAddition, ", \"icon\": \"mdi:wifi\"");
		break;
#endif // A2M_DEBUG_WIFI
	case mqttEntityId::entityA2MVersion:
	case mqttEntityId::entityInverterVersion:
	case mqttEntityId::entityEmsVersion:
		sprintf(stateAddition, ", \"icon\": \"mdi:numeric\"");
		break;
	case mqttEntityId::entityInverterSn:
	case mqttEntityId::entityEmsSn:
		sprintf(stateAddition, ", \"icon\": \"mdi:identifier\"");
		break;
	case mqttEntityId::entityRs485Errors:
	case mqttEntityId::entityBatFaults:
	case mqttEntityId::entityBatWarnings:
	case mqttEntityId::entityInverterFaults:
	case mqttEntityId::entityInverterWarnings:
	case mqttEntityId::entitySystemFaults:
		sprintf(stateAddition, ", \"icon\": \"mdi:alert-decagram-outline\"");
		break;
	case mqttEntityId::entityPollingBudgetExceeded:
	case mqttEntityId::entityPollingBudgetOverrunCount:
	case mqttEntityId::entityPollingBudgetUsedMs10s:
	case mqttEntityId::entityPollingBudgetLimitMs10s:
	case mqttEntityId::entityPollingBacklogCount10s:
	case mqttEntityId::entityPollingBacklogOldestAgeMs10s:
	case mqttEntityId::entityPollingLastFullCycleAgeMs10s:
	case mqttEntityId::entityPollingBudgetUsedMs1m:
	case mqttEntityId::entityPollingBudgetLimitMs1m:
	case mqttEntityId::entityPollingBacklogCount1m:
	case mqttEntityId::entityPollingBacklogOldestAgeMs1m:
	case mqttEntityId::entityPollingLastFullCycleAgeMs1m:
	case mqttEntityId::entityPollingBudgetUsedMs5m:
	case mqttEntityId::entityPollingBudgetLimitMs5m:
	case mqttEntityId::entityPollingBacklogCount5m:
	case mqttEntityId::entityPollingBacklogOldestAgeMs5m:
	case mqttEntityId::entityPollingLastFullCycleAgeMs5m:
	case mqttEntityId::entityPollingBudgetUsedMs1h:
	case mqttEntityId::entityPollingBudgetLimitMs1h:
	case mqttEntityId::entityPollingBacklogCount1h:
	case mqttEntityId::entityPollingBacklogOldestAgeMs1h:
	case mqttEntityId::entityPollingLastFullCycleAgeMs1h:
	case mqttEntityId::entityPollingBudgetUsedMs1d:
	case mqttEntityId::entityPollingBudgetLimitMs1d:
	case mqttEntityId::entityPollingBacklogCount1d:
	case mqttEntityId::entityPollingBacklogOldestAgeMs1d:
	case mqttEntityId::entityPollingLastFullCycleAgeMs1d:
	case mqttEntityId::entityPollingBudgetUsedMsUser:
	case mqttEntityId::entityPollingBudgetLimitMsUser:
	case mqttEntityId::entityPollingBacklogCountUser:
	case mqttEntityId::entityPollingBacklogOldestAgeMsUser:
	case mqttEntityId::entityPollingLastFullCycleAgeMsUser:
		sprintf(stateAddition, ", \"icon\": \"mdi:clock-alert-outline\"");
		break;
#ifdef DEBUG_FREEMEM
	case mqttEntityId::entityFreemem:
		sprintf(stateAddition, ", \"icon\": \"mdi:memory\"");
		break;
#endif // DEBUG_FREEMEM
#ifdef DEBUG_CALLBACKS
	case mqttEntityId::entityCallbacks:
#endif // DEBUG_CALLBACKS
	case mqttEntityId::entityRs485Avail:
	case mqttEntityId::entityA2MUptime:
	case mqttEntityId::entityBatSoc:
	case mqttEntityId::entityBatTemp:
	case mqttEntityId::entityInverterTemp:
	case mqttEntityId::entityGridAvail:
		strcpy(stateAddition, "");
		break;
	default:
		switch (singleEntity->family) {
		case MqttEntityFamily::Battery:
			snprintf(stateAddition, sizeof(stateAddition), ", \"icon\": \"mdi:battery-outline\"");
			break;
		case MqttEntityFamily::Pv:
			snprintf(stateAddition, sizeof(stateAddition), ", \"icon\": \"mdi:solar-power\"");
			break;
		case MqttEntityFamily::Grid:
			snprintf(stateAddition, sizeof(stateAddition), ", \"icon\": \"mdi:transmission-tower\"");
			break;
		case MqttEntityFamily::Backup:
			snprintf(stateAddition, sizeof(stateAddition), ", \"icon\": \"mdi:power-plug-battery\"");
			break;
		case MqttEntityFamily::Inverter:
			snprintf(stateAddition, sizeof(stateAddition), ", \"icon\": \"mdi:flash\"");
			break;
		case MqttEntityFamily::System:
		case MqttEntityFamily::Controller:
		default:
			stateAddition[0] = '\0';
			break;
		}
		break;
	}
	if (strlen(stateAddition) != 0) {
		resultAddedToPayload = addToPayload(stateAddition);
		if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
			return resultAddedToPayload;
		}
	}

	if (singleEntity->subscribe) {
#ifdef HA_IS_OP_MODE_AUTHORITY
		if (singleEntity->retain) {
			sprintf(stateAddition, ", \"retain\": \"true\"");
			resultAddedToPayload = addToPayload(stateAddition);
			if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
				return resultAddedToPayload;
			}
		}
#endif // HA_IS_OP_MODE_AUTHORITY
		sprintf(stateAddition, ", \"qos\": %d", MQTT_SUBSCRIBE_QOS);
		resultAddedToPayload = addToPayload(stateAddition);
		if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
			return resultAddedToPayload;
		}
	}

	switch (singleEntity->entityId) {
	case mqttEntityId::entityBatFaults:
	case mqttEntityId::entityBatWarnings:
	case mqttEntityId::entityInverterFaults:
	case mqttEntityId::entityInverterWarnings:
	case mqttEntityId::entitySystemFaults:
		snprintf(stateAddition, sizeof(stateAddition),
			", \"state_topic\": \"%s/state\""
			", \"value_template\": \"{{ \\\"OK\\\" if value_json.numEvents == 0 else \\\"Problem\\\" }}\""
			", \"json_attributes_topic\": \"%s/state\"",
			topicBase,
			topicBase);
		break;
	case mqttEntityId::entityFrequency:
		snprintf(stateAddition, sizeof(stateAddition),
			", \"state_topic\": \"%s/state\""
			", \"value_template\": \"{{ value_json[\\\"Use Frequency\\\"] | default(\\\"\\\") }}\""
			", \"json_attributes_topic\": \"%s/state\"",
			topicBase,
			topicBase);
		break;
	case mqttEntityId::entityRs485Avail:
		snprintf(stateAddition, sizeof(stateAddition),
			", \"state_topic\": \"%s\""
			", \"value_template\": \"{{ value_json.rs485Status | default(\\\"\\\") }}\""
			", \"json_attributes_topic\": \"%s\"",
			statusTopic, statusTopic);
		break;
	case mqttEntityId::entityGridAvail:
		snprintf(stateAddition, sizeof(stateAddition),
			", \"state_topic\": \"%s\""
			", \"value_template\": \"{{ value_json.gridStatus | default(\\\"\\\") }}\""
			", \"json_attributes_topic\": \"%s\"",
			statusTopic, statusTopic);
		break;
	default:
		snprintf(stateAddition, sizeof(stateAddition),
			", \"state_topic\": \"%s/state\"",
			topicBase);
		break;
	}
	resultAddedToPayload = addToPayload(stateAddition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return resultAddedToPayload;
	}

	if (singleEntity->subscribe) {
		snprintf(stateAddition, sizeof(stateAddition), ", \"command_topic\": \"%s/command\"", topicBase);
		resultAddedToPayload = addToPayload(stateAddition);
		if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
			return resultAddedToPayload;
		}
	}

	if (singleEntity->entityId == entityGridAvail) {
		snprintf(stateAddition, sizeof(stateAddition),
			", \"availability_template\": \"{{ \\\"online\\\" if value_json.a2mStatus == \\\"online\\\" and value_json.rs485Status == \\\"OK\\\" and value_json.gridStatus in ( \\\"OK\\\", \\\"Problem\\\" ) else \\\"offline\\\" }}\""
			", \"availability_topic\": \"%s\"", statusTopic);
	} else if (singleEntity->scope == MqttEntityScope::Controller ||
	           singleEntity->readKind == MqttEntityReadKind::Control ||
	           singleEntity->readKind == MqttEntityReadKind::Identity ||
	           singleEntity->entityId == entityBatCap ||
	           singleEntity->entityId == entityGridReg) {
		snprintf(stateAddition, sizeof(stateAddition),
			", \"availability_template\": \"{{ value_json.a2mStatus | default(\\\"\\\") }}\""
			", \"availability_topic\": \"%s\"", statusTopic);
	} else if (singleEntity->family == MqttEntityFamily::Grid) {
		snprintf(stateAddition, sizeof(stateAddition),
			", \"availability_template\": \"{{ \\\"online\\\" if value_json.a2mStatus == \\\"online\\\" and value_json.rs485Status == \\\"OK\\\" and value_json.gridStatus == \\\"OK\\\" else \\\"offline\\\" }}\""
			", \"availability_topic\": \"%s\"", statusTopic);
	} else {
		snprintf(stateAddition, sizeof(stateAddition),
			", \"availability_template\": \"{{ \\\"online\\\" if value_json.a2mStatus == \\\"online\\\" and value_json.rs485Status == \\\"OK\\\" else \\\"offline\\\" }}\""
			", \"availability_topic\": \"%s\"", statusTopic);
	}
	resultAddedToPayload = addToPayload(stateAddition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return resultAddedToPayload;
	}

	strcpy(stateAddition, "}");
	resultAddedToPayload = addToPayload(stateAddition);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return resultAddedToPayload;
	}

	return modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
}


modbusRequestAndResponseStatusValues
addToPayload(const char* addition)
{
	int targetRequestedSize = strlen(_mqttPayload) + strlen(addition);

	// If max payload size is 2048 it is stored as (0-2047), however character 2048  (position 2047) is null terminator so 2047 chars usable usable
	if (targetRequestedSize > _maxPayloadSize - 1) {
		// Safely print using snprintf
		snprintf(_mqttPayload, _maxPayloadSize, "{\r\n    \"mqttError\": \"Length of payload exceeds %d bytes.  Length would be %d bytes.\"\r\n}",
			 _maxPayloadSize - 1, targetRequestedSize);
		return modbusRequestAndResponseStatusValues::payloadExceededCapacity;
	} else {
		strlcat(_mqttPayload, addition, _maxPayloadSize);
		return modbusRequestAndResponseStatusValues::addedToPayload;
	}
}


void
sendHaData()
{
	if (!mqttEntitiesRtAvailable() || !_mqtt.connected()) {
		return;
	}
	const mqttState *entities = mqttEntitiesDesc();
	const size_t numberOfEntities = mqttEntitiesCount();

	// Spread retained HA discovery publishes across multiple loop() turns so a config change
	// cannot monopolize the ESP8266 network stack long enough to trigger the watchdog.
	if (publishPendingStaleInverterDiscoveryClears()) {
		return;
	}

	if (resendHaPreludePending) {
		if (!publishConfigDiscovery()) {
			return;
		}
		maybeYield();
		if (!publishControllerInverterSerialDiscovery()) {
			return;
		}
		maybeYield();
		resendHaPreludePending = false;
	}

	size_t batchCount = 0;
	while (resendHaNextEntityIndex < numberOfEntities && batchCount < kHaDiscoveryBatchSize) {
		if (!publishHaEntityDiscovery(&entities[resendHaNextEntityIndex])) {
			return;
		}
		++resendHaNextEntityIndex;
		++batchCount;
		maybeYield();
	}
	if (resendHaNextEntityIndex < numberOfEntities) {
		return;
	}
	resendHaData = false;
	resendHaPreludePending = false;
	resendHaNextEntityIndex = 0;
}

/*
 * refreshEssSnapshot
 *
 * Refresh opData from the inverter (ESS snapshot). This is a prerequisite operation called by the scheduler,
 * not a time-driven tick. Return true only when all snapshot reads succeeded.
 */
bool
refreshEssSnapshot(void)
{
	int gotError = 0;
	uint32_t pollStartMs = millis();
	bool rs485TimedOut = false;
	diag_rs485_poll_begin(pollStartMs);
	essSnapshotAttemptCount++;

	// Do not issue snapshot register reads until RS485 link is actually established.
	// Inverter identity may be known from persisted metadata, but that is not proof of a live bus.
	if (rs485ConnectState != Rs485ConnectState::Connected || _registerHandler == NULL) {
		essSnapshotValid = false;
		essSnapshotLastOk = false;
		lastErrCode = static_cast<int>(MqttEventCode::Rs485Timeout);
		diag_rs485_poll_end(millis(), false);
		return false;
	}

#if RS485_STUB
		if (_modBus != nullptr) {
			_modBus->beginSnapshotAttempt();
		}
#endif

#ifdef DEBUG_NO_RS485
	static unsigned long lastRs485 = 0, lastGrid = 0;
	static uint16_t essInverterMode = INVERTER_OPERATION_MODE_UPS_MODE;

	if (checkTimer(&lastRs485, STATUS_INTERVAL_TEN_SECONDS)) {
		opData.essRs485Connected = !opData.essRs485Connected;
	}

	if (checkTimer(&lastGrid, STATUS_INTERVAL_TEN_SECONDS * 2)) {
		if (essInverterMode == INVERTER_OPERATION_MODE_UPS_MODE) {
			essInverterMode = INVERTER_OPERATION_MODE_ONLINE_MODE;
		} else if (essInverterMode == INVERTER_OPERATION_MODE_ONLINE_MODE) {
			essInverterMode = INVERTER_OPERATION_MODE_CHECK_MODE;
		} else if (essInverterMode == INVERTER_OPERATION_MODE_CHECK_MODE) {
			essInverterMode = UINT16_MAX;
		} else {
			essInverterMode = INVERTER_OPERATION_MODE_UPS_MODE;
		}
	}

	if (opData.essRs485Connected) {
		opData.essDispatchStart = DISPATCH_START_START;
		opData.essDispatchMode = DISPATCH_MODE_NORMAL_MODE;
		opData.essDispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET;
		opData.essDispatchSoc = 50 / DISPATCH_SOC_MULTIPLIER;
		opData.essBatterySoc = 65 / BATTERY_SOC_MULTIPLIER;
		opData.essBatteryPower = -1357;
		opData.essGridPower = -1368;
		opData.essPvPower = -1379;
		opData.essInverterMode = essInverterMode;
	} else {
		opData.essDispatchStart = UINT16_MAX;
		opData.essDispatchMode = UINT16_MAX;
		opData.essDispatchActivePower = INT32_MAX;
		opData.essDispatchSoc = UINT16_MAX;
		opData.essBatterySoc = UINT16_MAX;
		opData.essBatteryPower = INT16_MAX;
		opData.essGridPower = INT32_MAX;
		opData.essPvPower = INT32_MAX;
		opData.essInverterMode = UINT16_MAX;
		lastErrCode = static_cast<int>(MqttEventCode::Rs485Timeout);
		gotError = 9;
	}
#else // DEBUG_NO_RS485
	if (_registerHandler == NULL) {
		opData.essDispatchStart = UINT16_MAX;
		opData.essDispatchMode = UINT16_MAX;
		opData.essDispatchActivePower = INT32_MAX;
		opData.essDispatchSoc = UINT16_MAX;
		opData.essBatterySoc = UINT16_MAX;
		opData.essBatteryPower = INT16_MAX;
		opData.essGridPower = INT32_MAX;
		opData.essPvPower = INT32_MAX;
		opData.essInverterMode = UINT16_MAX;
		opData.essRs485Connected = false;
		lastErrCode = static_cast<int>(MqttEventCode::Rs485Timeout);
		gotError = 1;
	} else {
		modbusRequestAndResponseStatusValues result = modbusRequestAndResponseStatusValues::preProcessing;
		modbusRequestAndResponse response;

	result = _registerHandler->readHandledRegister(REG_DISPATCH_RW_DISPATCH_START, &response);
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		opData.essDispatchStart = response.unsignedShortValue;
	} else {
		opData.essDispatchStart = UINT16_MAX;
		rs485TimedOut = rs485TimedOut || (result == modbusRequestAndResponseStatusValues::noResponse);
		noteRs485Error(result, response.statusMqttMessage);
		gotError++;
	}
	result = _registerHandler->readHandledRegister(REG_DISPATCH_RW_DISPATCH_MODE, &response);
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		opData.essDispatchMode = response.unsignedShortValue;
	} else {
		opData.essDispatchMode = UINT16_MAX;
		rs485TimedOut = rs485TimedOut || (result == modbusRequestAndResponseStatusValues::noResponse);
		noteRs485Error(result, response.statusMqttMessage);
		gotError++;
	}
	result = _registerHandler->readHandledRegister(REG_DISPATCH_RW_ACTIVE_POWER_1, &response);
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		opData.essDispatchActivePower = response.signedIntValue;
	} else {
		opData.essDispatchActivePower = INT32_MAX;
		rs485TimedOut = rs485TimedOut || (result == modbusRequestAndResponseStatusValues::noResponse);
		noteRs485Error(result, response.statusMqttMessage);
		gotError++;
	}
	result = _registerHandler->readHandledRegister(REG_DISPATCH_RW_DISPATCH_SOC, &response);
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		opData.essDispatchSoc = response.unsignedShortValue;
	} else {
		opData.essDispatchSoc = UINT16_MAX;
		rs485TimedOut = rs485TimedOut || (result == modbusRequestAndResponseStatusValues::noResponse);
		noteRs485Error(result, response.statusMqttMessage);
		gotError++;
	}
	result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_SOC, &response);
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		opData.essBatterySoc = response.unsignedShortValue;
	} else {
		opData.essBatterySoc = UINT16_MAX;
		rs485TimedOut = rs485TimedOut || (result == modbusRequestAndResponseStatusValues::noResponse);
		noteRs485Error(result, response.statusMqttMessage);
		gotError++;
	}
	result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_POWER, &response);
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		opData.essBatteryPower = response.signedShortValue;
	} else {
		opData.essBatteryPower = INT16_MAX;
		rs485TimedOut = rs485TimedOut || (result == modbusRequestAndResponseStatusValues::noResponse);
		noteRs485Error(result, response.statusMqttMessage);
		gotError++;
	}
	result = _registerHandler->readHandledRegister(REG_GRID_METER_R_TOTAL_ACTIVE_POWER_1, &response);
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		opData.essGridPower = response.signedIntValue;
	} else {
		opData.essGridPower = INT32_MAX;
		rs485TimedOut = rs485TimedOut || (result == modbusRequestAndResponseStatusValues::noResponse);
		noteRs485Error(result, response.statusMqttMessage);
		gotError++;
	}
	result = _registerHandler->readHandledRegister(REG_CUSTOM_TOTAL_SOLAR_POWER, &response);
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		opData.essPvPower = response.signedIntValue;
	} else {
		opData.essPvPower = INT32_MAX;
		rs485TimedOut = rs485TimedOut || (result == modbusRequestAndResponseStatusValues::noResponse);
		noteRs485Error(result, response.statusMqttMessage);
		gotError++;
	}
	result = _registerHandler->readHandledRegister(REG_INVERTER_HOME_R_WORKING_MODE, &response);
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		opData.essInverterMode = response.unsignedShortValue;
	} else {
		opData.essInverterMode = UINT16_MAX;
		rs485TimedOut = rs485TimedOut || (result == modbusRequestAndResponseStatusValues::noResponse);
		noteRs485Error(result, response.statusMqttMessage);
		gotError++;
	}
		{
			bool essRs485WasConnected = opData.essRs485Connected;
			opData.essRs485Connected = _modBus->isRs485Online();
			if (!essRs485WasConnected && opData.essRs485Connected) {
				resendAllData = true;
			}
		}
	}
#endif // DEBUG_NO_RS485

	lastPollMs = millis() - pollStartMs;
	if (gotError != 0) {
		pollErrCount++;
		lastErrTsMs = millis();
		rs485Errors += gotError;
		essSnapshotValid = false;
		strlcpy(dispatchLastSkipReason, "ess_snapshot_failed", sizeof(dispatchLastSkipReason));
	} else {
		pollOkCount++;
		lastOkTsMs = millis();
		essSnapshotValid = true;
		dispatchLastSkipReason[0] = '\0';
	}
	essSnapshotLastOk = essSnapshotValid;
	if (lastPollMs > kPollOverrunMs) {
		publishEvent(MqttEventCode::PollOverrun, "");
	}

#if RS485_STUB
		if (_modBus != nullptr) {
			_modBus->endSnapshotAttempt();
		}
#endif
	diag_rs485_poll_end(millis(), rs485TimedOut);

	return essSnapshotValid;
}

static BucketRuntimeBudgetState *
bucketBudgetStateFor(BucketId bucket)
{
	const int ordinal = bucketOrdinal(bucket);
	if (ordinal < 0 || ordinal >= static_cast<int>(sizeof(schedBudgetState) / sizeof(schedBudgetState[0]))) {
		return nullptr;
	}
	return &schedBudgetState[ordinal];
}

static size_t *
bucketCursorFor(BucketId bucket)
{
	const int ordinal = bucketOrdinal(bucket);
	if (ordinal < 0 || ordinal >= static_cast<int>(sizeof(schedNextCursor) / sizeof(schedNextCursor[0]))) {
		return nullptr;
	}
	return &schedNextCursor[ordinal];
}

static void
resetBucketCursors(void)
{
	for (size_t i = 0; i < sizeof(schedNextCursor) / sizeof(schedNextCursor[0]); ++i) {
		schedNextCursor[i] = 0;
	}
}

static void
resetBucketBudgetStates(void)
{
	memset(schedBudgetState, 0, sizeof(schedBudgetState));
}

static bool
pollingBudgetExceeded(void)
{
	for (size_t i = 0; i < sizeof(kRuntimeBuckets) / sizeof(kRuntimeBuckets[0]); ++i) {
		if (bucketRuntimeBudgetExceeded(schedBudgetState[i])) {
			return true;
		}
	}
	return false;
}

static const PollingBudgetEntitySpec *
pollingBudgetEntitySpecFor(mqttEntityId entityId)
{
	for (size_t i = 0; i < sizeof(kPollingBudgetEntitySpecs) / sizeof(kPollingBudgetEntitySpecs[0]); ++i) {
		if (kPollingBudgetEntitySpecs[i].entityId == entityId) {
			return &kPollingBudgetEntitySpecs[i];
		}
	}
	return nullptr;
}

static uint32_t
pollingBudgetMetricValue(const BucketRuntimeBudgetState &state, PollingBudgetMetric metric, uint32_t nowMs)
{
	switch (metric) {
	case PollingBudgetMetric::UsedMs:
		return state.usedMsLast;
	case PollingBudgetMetric::LimitMs:
		return state.limitMsLast;
	case PollingBudgetMetric::BacklogCount:
		return state.backlogCount;
	case PollingBudgetMetric::BacklogOldestAgeMs:
		return bucketBacklogOldestAgeMs(state, nowMs);
	case PollingBudgetMetric::LastFullCycleAgeMs:
	default:
		return bucketLastFullCycleAgeMs(state, nowMs);
	}
}

static bool
formatPollingBudgetEntityValue(mqttEntityId entityId, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}

	if (entityId == mqttEntityId::entityPollingBudgetExceeded) {
		strlcpy(out, pollingBudgetExceeded() ? "Problem" : "OK", outSize);
		return true;
	}
	if (entityId == mqttEntityId::entityPollingBudgetOverrunCount) {
		snprintf(out, outSize, "%lu", static_cast<unsigned long>(pollingBudgetOverrunCount));
		return true;
	}

	const PollingBudgetEntitySpec *spec = pollingBudgetEntitySpecFor(entityId);
	if (spec == nullptr) {
		return false;
	}

	const BucketRuntimeBudgetState *state = bucketBudgetStateFor(spec->bucketId);
	if (state == nullptr) {
		return false;
	}

	snprintf(out,
	         outSize,
	         "%lu",
	         static_cast<unsigned long>(pollingBudgetMetricValue(*state, spec->metric, millis())));
	return true;
}

static void
executePollTransaction(const MqttEntityActiveBucket &bucketPlan,
                       const MqttPollTransaction &transaction,
                       const mqttState *entities,
                       bool snapshotOkThisBucket)
{
	if (transaction.entityCount == 0 || entities == nullptr || bucketPlan.members == nullptr) {
		return;
	}

	const size_t leaderOffset = transaction.firstMemberOffset;
	if (leaderOffset >= bucketPlan.count) {
		return;
	}

	switch (transaction.kind) {
	case MqttPollTransactionKind::SnapshotFanout:
		if (!snapshotOkThisBucket) {
			return;
		}
		for (size_t member = 0; member < transaction.entityCount; ++member) {
			const size_t offset = static_cast<size_t>(transaction.firstMemberOffset) + member;
			if (offset >= bucketPlan.count) {
				break;
			}
			sendDataFromMqttState(&entities[bucketPlan.members[offset]], false, nullptr);
		}
		return;
	case MqttPollTransactionKind::RegisterFanout:
	case MqttPollTransactionKind::SingleEntity:
	default:
		break;
	}

	const size_t leaderIdx = bucketPlan.members[leaderOffset];
	modbusRequestAndResponse response;
	const modbusRequestAndResponseStatusValues result = readEntity(&entities[leaderIdx], &response);
	if (result != modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		return;
	}

	for (size_t member = 0; member < transaction.entityCount; ++member) {
		const size_t offset = static_cast<size_t>(transaction.firstMemberOffset) + member;
		if (offset >= bucketPlan.count) {
			break;
		}
		sendDataFromMqttState(&entities[bucketPlan.members[offset]], false, &response);
	}
}

/*
 * sendData
 *
 * Runs once every loop, checks to see if time periods have elapsed to allow the schedules to run.
 * Each time, the appropriate arrays are iterated, processed and added to the payload.
 */
void
sendData()
{
	static unsigned long lastRunTenSeconds = 0;
	static unsigned long lastRunOneMinute = 0;
	static unsigned long lastRunFiveMinutes = 0;
	static unsigned long lastRunOneHour = 0;
	static unsigned long lastRunOneDay = 0;
	static unsigned long lastRunUser = 0;

	if (resendAllData) {
		resendAllData = false;
		lastRunTenSeconds = lastRunOneMinute = lastRunFiveMinutes = lastRunOneHour = lastRunOneDay = 0;
		lastRunUser = 0;
		resetBucketCursors();
		resetBucketBudgetStates();
	}

	const bool dueTenSeconds = checkTimer(&lastRunTenSeconds, STATUS_INTERVAL_TEN_SECONDS);
	const bool dueOneMinute = checkTimer(&lastRunOneMinute, STATUS_INTERVAL_ONE_MINUTE);
	const bool dueFiveMinutes = checkTimer(&lastRunFiveMinutes, STATUS_INTERVAL_FIVE_MINUTE);
	const bool dueOneHour = checkTimer(&lastRunOneHour, STATUS_INTERVAL_ONE_HOUR);
	const bool dueOneDay = checkTimer(&lastRunOneDay, STATUS_INTERVAL_ONE_DAY);
	const bool dueUser = checkTimer(&lastRunUser, pollIntervalSeconds * 1000UL);
	const bool anyDue = (dueTenSeconds || dueOneMinute || dueFiveMinutes || dueOneHour || dueOneDay || dueUser);

	if (!anyDue) {
		return;
	}

	if (dueTenSeconds) {
		schedTenSecLastRunMs = lastRunTenSeconds;
	}
	if (dueOneMinute) {
		schedOneMinLastRunMs = lastRunOneMinute;
	}
	if (dueFiveMinutes) {
		schedFiveMinLastRunMs = lastRunFiveMinutes;
	}
	if (dueOneHour) {
		schedOneHourLastRunMs = lastRunOneHour;
	}
	if (dueOneDay) {
		schedOneDayLastRunMs = lastRunOneDay;
	}
	if (dueUser) {
		schedUserLastRunMs = lastRunUser;
	}

	if (dueTenSeconds && !mqttEntitiesRtAvailable()) {
		sendStatus(false);
		return;
	}

	if (!mqttEntitiesRtAvailable()) {
		return;
	}

	const mqttState *entities = mqttEntitiesDesc();
	const MqttEntityActivePlan *plan = mqttActivePlan();
	if (plan == nullptr) {
		return;
	}

	// Bucket processing is runtime-driven: due buckets iterate their pre-built membership list.
	// ESS snapshot is a bucket-scoped prerequisite and is refreshed once per scheduler pass
	// (even if multiple buckets are due at the same time).
	bool snapshotAttemptedThisPass = false;
	bool snapshotOkThisPass = essSnapshotValid;
	bool dispatchRanThisPass = false;

	auto ensureSnapshotForBucket = [&](bool bucketNeedsSnapshot) -> bool {
		if (shouldAttemptEssSnapshotRefreshForBucket(bucketNeedsSnapshot,
		                                             bootPlan.inverter,
		                                             inverterReady,
		                                             snapshotAttemptedThisPass)) {
			snapshotAttemptedThisPass = true;
			snapshotOkThisPass = refreshEssSnapshot();
		}
		const bool snapshotOkThisBucket = snapshotPrereqSatisfiedForBucket(bucketNeedsSnapshot,
		                                                                  bootPlan.inverter,
		                                                                  inverterReady,
		                                                                  snapshotOkThisPass);
		if (!snapshotOkThisBucket) {
			essSnapshotValid = false;
		}
		return snapshotOkThisBucket;
	};

	auto runBucketTransactions = [&](BucketId bucketId,
	                                 const MqttEntityActiveBucket &bucketPlan,
	                                 bool snapshotOkThisBucket) -> bool {
		BucketRuntimeBudgetState *budgetState = bucketBudgetStateFor(bucketId);
		const uint32_t budgetMs = bucketBudgetMs(bucketId, pollIntervalSeconds * 1000UL, kPollOverrunMs);
		const uint32_t bucketStartMs = millis();

		if (bucketPlan.transactionCount == 0) {
			if (budgetState != nullptr) {
				updateBucketRuntimeBudgetState(*budgetState, millis(), 0, budgetMs, 0, 0, false);
			}
			return false;
		}
		size_t *cursorPtr = bucketCursorFor(bucketId);
		if (cursorPtr == nullptr) {
			return false;
		}
		const size_t startCursor = normalizeDeferredCursor(*cursorPtr, bucketPlan.transactionCount);
		size_t processed = 0;
		bool truncated = false;

		while (processed < bucketPlan.transactionCount) {
			const size_t txnIndex = (startCursor + processed) % bucketPlan.transactionCount;
			executePollTransaction(bucketPlan, bucketPlan.transactions[txnIndex], entities, snapshotOkThisBucket);
			processed++;
			if (processed < bucketPlan.transactionCount && timedOut(bucketStartMs, millis(), budgetMs)) {
				truncated = true;
				break;
			}
		}

		const uint32_t bucketEndMs = millis();
		if (budgetState != nullptr) {
			updateBucketRuntimeBudgetState(*budgetState,
			                               bucketEndMs,
			                               static_cast<uint32_t>(bucketEndMs - bucketStartMs),
			                               budgetMs,
			                               bucketPlan.transactionCount,
			                               processed,
			                               truncated);
		}
		if (truncated) {
			pollingBudgetOverrunCount++;
		}
		*cursorPtr = nextDeferredCursor(startCursor, processed, bucketPlan.transactionCount, truncated);
		return truncated;
	};

	if (dueTenSeconds) {
		// 10s cadence also gates dispatch, which depends on the ESS snapshot.
		const bool snapshotOkThisBucket = ensureSnapshotForBucket(tenSecBucketRequiresSnapshot());
		sendStatus(snapshotOkThisBucket);
		const bool tenSecTruncated = runBucketTransactions(BucketId::TenSec, plan->tenSec, snapshotOkThisBucket);

		if (!tenSecTruncated &&
		    shouldRunDispatchForTenSecPass(dueTenSeconds, snapshotOkThisBucket, dispatchRanThisPass)) {
			checkAndSetDispatchMode();
			dispatchRanThisPass = true;
			dispatchLastSkipReason[0] = '\0';
		} else if (tenSecTruncated) {
			strlcpy(dispatchLastSkipReason, "poll_budget_exhausted", sizeof(dispatchLastSkipReason));
		} else {
			strlcpy(dispatchLastSkipReason, "ess_snapshot_failed", sizeof(dispatchLastSkipReason));
		}
	}

	if (dueOneMinute) {
		const bool snapshotOkThisBucket = ensureSnapshotForBucket(plan->oneMin.hasEssSnapshot);
		runBucketTransactions(BucketId::OneMin, plan->oneMin, snapshotOkThisBucket);
	}

	if (dueFiveMinutes) {
		const bool snapshotOkThisBucket = ensureSnapshotForBucket(plan->fiveMin.hasEssSnapshot);
		runBucketTransactions(BucketId::FiveMin, plan->fiveMin, snapshotOkThisBucket);
	}

	if (dueOneHour) {
		const bool snapshotOkThisBucket = ensureSnapshotForBucket(plan->oneHour.hasEssSnapshot);
		runBucketTransactions(BucketId::OneHour, plan->oneHour, snapshotOkThisBucket);
	}

	if (dueOneDay) {
		const bool snapshotOkThisBucket = ensureSnapshotForBucket(plan->oneDay.hasEssSnapshot);
		runBucketTransactions(BucketId::OneDay, plan->oneDay, snapshotOkThisBucket);
	}

	if (dueUser) {
		const bool snapshotOkThisBucket = ensureSnapshotForBucket(plan->user.hasEssSnapshot);
		runBucketTransactions(BucketId::User, plan->user, snapshotOkThisBucket);
	}
}

bool
sendDataFromMqttState(const mqttState *singleEntity, bool doHomeAssistant, const modbusRequestAndResponse *preparedResponse)
{
	char topic[256];
	char topicBase[200];
	modbusRequestAndResponseStatusValues result;
	modbusRequestAndResponseStatusValues resultAddedToPayload;
	DiscoveryDeviceScope scope = DiscoveryDeviceScope::Controller;
	const char *deviceId = "";

	if (singleEntity == NULL)
		return true;
	scope = mqttEntityScope(singleEntity->entityId);
	deviceId = discoveryDeviceIdForScope(scope);
	if (!mqttEntitiesRtAvailable()) {
		return true;
	}
	size_t idx = static_cast<size_t>(singleEntity - mqttEntitiesDesc());
	if (idx >= mqttEntitiesCount()) {
		return true;
	}
	mqttUpdateFreq effectiveFreq = mqttEntityEffectiveFreqByIndex(idx);
	if (!doHomeAssistant &&
		    (effectiveFreq == mqttUpdateFreq::freqNever ||
		     effectiveFreq == mqttUpdateFreq::freqDisabled)) {
		return true;
	}
	if (deviceId[0] == '\0') {
		return true;
	}
	char entityKey[64];
	mqttEntityNameCopy(singleEntity, entityKey, sizeof(entityKey));
	if (!buildEntityTopicBase(deviceName,
	                          scope,
	                          controllerIdentifier,
	                          deviceSerialNumber,
		                          entityKey,
		                          topicBase,
		                          sizeof(topicBase))) {
		return true;
	}
	if (!doHomeAssistant && mqttEntityNeedsEssSnapshotByIndex(idx) && !essSnapshotValid) {
		return true;
	}

	emptyPayload();

	if (doHomeAssistant) {
		const char *entityType;
		switch (singleEntity->haClass) {
		case homeAssistantClass::haClassBox:
		case homeAssistantClass::haClassNumber:
			entityType = "number";
			break;
		case homeAssistantClass::haClassSelect:
			entityType = "select";
			break;
		case homeAssistantClass::haClassBinaryProblem:
			entityType = "binary_sensor";
			break;
		default:
			entityType = "sensor";
			break;
		}

		snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config", entityType, deviceId, entityKey);
		result = addConfig(singleEntity, scope, topicBase, resultAddedToPayload);
	} else {
		bool skip = false;
		if (!opData.a2mReadyToUseOpMode && (singleEntity->entityId == mqttEntityId::entityOpMode)) {
			skip = true;
		}
		if (!opData.a2mReadyToUseSocTarget && (singleEntity->entityId == mqttEntityId::entitySocTarget)) {
			skip = true;
		}
		if (!opData.a2mReadyToUsePwrCharge && (singleEntity->entityId == mqttEntityId::entityChargePwr)) {
			skip = true;
		}
		if (!opData.a2mReadyToUsePwrDischarge && (singleEntity->entityId == mqttEntityId::entityDischargePwr)) {
			skip = true;
		}
		if (!opData.a2mReadyToUsePwrPush && (singleEntity->entityId == mqttEntityId::entityPushPwr)) {
			skip = true;
		}
		if (!skip) {
			snprintf(topic, sizeof(topic), "%s/state", topicBase);
			if (preparedResponse != nullptr) {
				result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
				resultAddedToPayload = addToPayload(preparedResponse->dataValueFormatted);
			} else {
				result = addState(singleEntity, &resultAddedToPayload);
			}
		} else {
			result = modbusRequestAndResponseStatusValues::preProcessing;
		}
	}

	if ((resultAddedToPayload != modbusRequestAndResponseStatusValues::payloadExceededCapacity) &&
	    (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess)) {
		// And send
		return sendMqtt(topic, singleEntity->retain ? MQTT_RETAIN : false);
	}
	return true;
}

static bool
publishManualRegisterValueState(const mqttState *valueEntity, const modbusRequestAndResponse &response)
{
	if (valueEntity == nullptr || !mqttEntitiesRtAvailable()) {
		return false;
	}
	const size_t idx = static_cast<size_t>(valueEntity - mqttEntitiesDesc());
	if (idx >= mqttEntitiesCount()) {
		return false;
	}
	mqttUpdateFreq effectiveFreq = mqttEntityEffectiveFreqByIndex(idx);
	if (effectiveFreq == mqttUpdateFreq::freqNever ||
	    effectiveFreq == mqttUpdateFreq::freqDisabled) {
		return false;
	}
	char topicBase[200];
	char topic[256];
	char entityKey[64];
	mqttEntityNameCopy(valueEntity, entityKey, sizeof(entityKey));
	if (!buildEntityTopicBase(deviceName,
	                          mqttEntityScope(valueEntity->entityId),
	                          controllerIdentifier,
	                          deviceSerialNumber,
	                          entityKey,
	                          topicBase,
	                          sizeof(topicBase))) {
		return false;
	}
	emptyPayload();
	modbusRequestAndResponseStatusValues resultAddedToPayload = addToPayload(response.dataValueFormatted);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return false;
	}
	snprintf(topic, sizeof(topic), "%s/state", topicBase);
	sendMqtt(topic, valueEntity->retain ? MQTT_RETAIN : false);
	return true;
}

static void
publishManualRegisterReadStatus(int32_t requestedReg, const modbusRequestAndResponse &response)
{
	static char manualReadAddition[256];
	char manualReadTopic[160];
	StatusManualReadSnapshot snapshot{};
	snapshot.seq = ++manualRegisterReadSeq;
	snapshot.tsMs = millis();
	snapshot.requestedReg = requestedReg;
#if RS485_STUB
	snapshot.observedReg = (_modBus != nullptr) ? _modBus->stubLastReadStartReg() : 0;
#else
	snapshot.observedReg = 0;
#endif
	snapshot.value = response.dataValueFormatted;
	snprintf(manualReadTopic, sizeof(manualReadTopic), "%s/manual_read", statusTopic);
	if (buildStatusManualReadJson(snapshot, manualReadAddition, sizeof(manualReadAddition))) {
		_mqtt.publish(manualReadTopic, manualReadAddition, true);
		maybeYield();
	}
}

static void
publishManualRegisterReadState(int32_t requestedReg)
{
	const mqttState *valueEntity = lookupEntity(mqttEntityId::entityRegValue);
	if (valueEntity == nullptr) {
		return;
	}
	modbusRequestAndResponse response;
	const modbusRequestAndResponseStatusValues result = readEntity(valueEntity, &response);
	if (result != modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		return;
	}
	publishManualRegisterValueState(valueEntity, response);
	publishManualRegisterReadStatus(requestedReg, response);
}

static void
processPendingEntityCommand(void)
{
	if (!pendingEntityCommandSet || pendingEntityCommand == NULL) {
		return;
	}

	struct PendingEntityCommandGuard {
		~PendingEntityCommandGuard()
		{
			pendingEntityCommandSet = false;
			pendingEntityCommand = NULL;
			pendingDeferredControlPayload[0] = '\0';
		}
	} guard;

	const mqttState *mqttEntity = pendingEntityCommand;
	const char *mqttIncomingPayload = pendingDeferredControlPayload;

	int32_t singleInt32 = -1;
	const char *singleString = NULL;
	char *endPtr = NULL;
	bool valueProcessingError = false;

	// First, process value.
	switch (mqttEntity->entityId) {
	case mqttEntityId::entitySocTarget:
	case mqttEntityId::entityChargePwr:
	case mqttEntityId::entityDischargePwr:
	case mqttEntityId::entityPushPwr:
	case mqttEntityId::entityRegNum:
		singleInt32 = strtol(mqttIncomingPayload, &endPtr, 10);
		if ((endPtr == mqttIncomingPayload) || ((singleInt32 == 0) && (errno != 0))) {
			valueProcessingError = true;
		}
		break;
	case mqttEntityId::entityOpMode:
		singleString = mqttIncomingPayload;
		break;
	default:
#ifdef DEBUG_OVER_SERIAL
		sprintf(_debugOutput, "Trying to update an unhandled entity! %d", mqttEntity->entityId);
		Serial.println(_debugOutput);
#endif
#ifdef DEBUG_CALLBACKS
		unknownCallbacks++;
#endif // DEBUG_CALLBACKS
		return;
	}

	if (valueProcessingError) {
#ifdef DEBUG_OVER_SERIAL
		char entityName[64];
		mqttEntityNameCopy(mqttEntity, entityName, sizeof(entityName));
		snprintf(_debugOutput, sizeof(_debugOutput), "Callback for %s with bad value: ", entityName);
		Serial.print(_debugOutput);
		Serial.println(mqttIncomingPayload);
#endif
#ifdef DEBUG_CALLBACKS
		badCallbacks++;
#endif // DEBUG_CALLBACKS
		return;
	}

	// Now set the value and take appropriate action(s)
	switch (mqttEntity->entityId) {
	case mqttEntityId::entitySocTarget:
		if ((singleInt32 < SOC_TARGET_MIN) || (singleInt32 > SOC_TARGET_MAX)) {
#ifdef DEBUG_OVER_SERIAL
			sprintf(_debugOutput, "HA sent invalid SocTarget! %ld", singleInt32);
			Serial.println(_debugOutput);
#endif
#ifdef DEBUG_CALLBACKS
			badCallbacks++;
#endif // DEBUG_CALLBACKS
		} else {
			opData.a2mSocTarget = singleInt32;
			opData.a2mReadyToUseSocTarget = true;
		}
		break;
	case mqttEntityId::entityChargePwr:
		if ((singleInt32 < 0) || (singleInt32 > INVERTER_POWER_MAX)) {
#ifdef DEBUG_OVER_SERIAL
			sprintf(_debugOutput, "HA sent invalid Charge Power! %ld", singleInt32);
			Serial.println(_debugOutput);
#endif
#ifdef DEBUG_CALLBACKS
			badCallbacks++;
#endif // DEBUG_CALLBACKS
		} else {
			opData.a2mPwrCharge = singleInt32;
			opData.a2mReadyToUsePwrCharge = true;
		}
		break;
	case mqttEntityId::entityDischargePwr:
		if ((singleInt32 < 0) || (singleInt32 > INVERTER_POWER_MAX)) {
#ifdef DEBUG_OVER_SERIAL
			sprintf(_debugOutput, "HA sent invalid Discharge Power! %ld", singleInt32);
			Serial.println(_debugOutput);
#endif
#ifdef DEBUG_CALLBACKS
			badCallbacks++;
#endif // DEBUG_CALLBACKS
		} else {
			opData.a2mPwrDischarge = singleInt32;
			opData.a2mReadyToUsePwrDischarge = true;
		}
		break;
	case mqttEntityId::entityPushPwr:
		if ((singleInt32 < 0) || (singleInt32 > INVERTER_POWER_MAX)) {
#ifdef DEBUG_OVER_SERIAL
			sprintf(_debugOutput, "HA sent invalid Push Power! %ld", singleInt32);
			Serial.println(_debugOutput);
#endif
#ifdef DEBUG_CALLBACKS
			badCallbacks++;
#endif // DEBUG_CALLBACKS
		} else {
			opData.a2mPwrPush = singleInt32;
			opData.a2mReadyToUsePwrPush = true;
		}
		break;
	case mqttEntityId::entityRegNum:
		regNumberToRead = singleInt32; // Set local variable
		publishManualRegisterReadState(singleInt32);
		break;
	case mqttEntityId::entityOpMode:
		{
			enum opMode tempOpMode = lookupOpMode(singleString);
			if (tempOpMode != (enum opMode)-1) {
				opData.a2mOpMode = tempOpMode;
				opData.a2mReadyToUseOpMode = true;
			} else {
#ifdef DEBUG_OVER_SERIAL
				snprintf(_debugOutput, sizeof(_debugOutput), "Callback: Bad opMode: %s", singleString);
				Serial.println(_debugOutput);
#endif
#ifdef DEBUG_CALLBACKS
				badCallbacks++;
#endif // DEBUG_CALLBACKS
			}
		}
		break;
	default:
#ifdef DEBUG_OVER_SERIAL
		sprintf(_debugOutput, "Trying to write an unhandled entity! %d", mqttEntity->entityId);
		Serial.println(_debugOutput);
#endif
		break;
	}

	// Send (hopefully) updated state. If we failed to update, sender should notice value not changing.
	sendDataFromMqttState(mqttEntity, false);
}

#if RS485_STUB
struct Rs485StubControlRequest {
	Rs485StubMode mode = Rs485StubMode::OfflineForever;
	uint32_t failN = 0;
	uint16_t failReg = 0;
	Rs485StubFailType failType = Rs485StubFailType::NoResponse;
	uint16_t latencyMs = 0;
	bool strictUnknown = false;
	uint32_t failEveryN = 0;
	bool failReads = true;
	bool failWrites = true;
	uint32_t failForMs = 0;
	uint32_t flapOnlineMs = 0;
	uint32_t flapOfflineMs = 0;
	uint32_t probeSuccessAfterN = 0;
	int16_t socStepX10PerSnapshot = 0;

	bool hasVirtualEss = false;
	uint16_t virtualSocPct = 65;
	int16_t virtualBatteryPowerW = 0;
	int32_t virtualGridPowerW = 0;
	int32_t virtualPvCtPowerW = 0;
	uint16_t virtualInverterMode = INVERTER_OPERATION_MODE_UPS_MODE;

	bool hasVirtualDispatch = false;
	uint16_t virtualDispatchStart = 0;
	uint16_t virtualDispatchMode = DISPATCH_MODE_NORMAL_MODE;
	int32_t virtualDispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET;
	uint16_t virtualDispatchSoc = 0;
};

static bool
payloadHasToken(const char *payload, const char *lower, const char *upper)
{
	if (payload == nullptr) {
		return false;
	}
	return strstr(payload, lower) != nullptr || strstr(payload, upper) != nullptr;
}

static bool
parseStubControlInt(const char *payload, const char *key, int32_t &out)
{
	return rs485StubParseIntField(payload, key, out);
}

static bool
parseStubControlMode(const char *payload, Rs485StubMode &mode)
{
	return rs485StubParseModeField(payload, mode);
}

static void
parseRs485StubControlPayload(const char *payload, Rs485StubControlRequest &request)
{
	int32_t value = 0;

	parseStubControlMode(payload, request.mode);

	if (parseStubControlInt(payload, "fail_n", value) ||
	    parseStubControlInt(payload, "failFirstN", value)) {
		if (value > 0) {
			request.failN = static_cast<uint32_t>(value);
		}
	}

	if (parseStubControlInt(payload, "reg", value) ||
	    parseStubControlInt(payload, "register", value)) {
		if (value > 0) {
			request.failReg = static_cast<uint16_t>(value);
		}
	}

	if (payloadHasToken(payload, "slave_error", "SLAVE_ERROR")) {
		request.failType = Rs485StubFailType::SlaveError;
	}
	if (payloadHasToken(payload, "no_response", "NO_RESPONSE")) {
		request.failType = Rs485StubFailType::NoResponse;
	}
	if (parseStubControlInt(payload, "fail_type", value) && value == 1) {
		request.failType = Rs485StubFailType::SlaveError;
	}

	if (parseStubControlInt(payload, "latency_ms", value) && value >= 0 && value <= 60000) {
		request.latencyMs = static_cast<uint16_t>(value);
	}
	if ((parseStubControlInt(payload, "strict_unknown", value) ||
	     parseStubControlInt(payload, "strict", value)) && value >= 0) {
		request.strictUnknown = (value != 0);
	}
	if (parseStubControlInt(payload, "fail_every_n", value) && value >= 1) {
		request.failEveryN = static_cast<uint32_t>(value);
	}
	if (parseStubControlInt(payload, "fail_reads", value) && value >= 0) {
		request.failReads = (value != 0);
	}
	if (parseStubControlInt(payload, "fail_writes", value) && value >= 0) {
		request.failWrites = (value != 0);
	}
	if (parseStubControlInt(payload, "fail_for_ms", value) && value >= 0) {
		request.failForMs = static_cast<uint32_t>(value);
	}
	if (parseStubControlInt(payload, "flap_online_ms", value) && value >= 0) {
		request.flapOnlineMs = static_cast<uint32_t>(value);
	}
	if (parseStubControlInt(payload, "flap_offline_ms", value) && value >= 0) {
		request.flapOfflineMs = static_cast<uint32_t>(value);
	}
	if (parseStubControlInt(payload, "probe_success_after_n", value) && value >= 0) {
		request.probeSuccessAfterN = static_cast<uint32_t>(value);
	}
	if (parseStubControlInt(payload, "soc_step_x10_per_snapshot", value) &&
	    value >= -32768 && value <= 32767) {
		request.socStepX10PerSnapshot = static_cast<int16_t>(value);
	}

	if ((parseStubControlInt(payload, "soc_pct", value) ||
	     parseStubControlInt(payload, "soc", value)) &&
	    value >= 0 && value <= 100) {
		request.virtualSocPct = static_cast<uint16_t>(value);
		request.hasVirtualEss = true;
	}
	if (parseStubControlInt(payload, "battery_power_w", value) &&
	    value >= -20000 && value <= 20000) {
		request.virtualBatteryPowerW = static_cast<int16_t>(value);
		request.hasVirtualEss = true;
	}
	if (parseStubControlInt(payload, "grid_power_w", value)) {
		request.virtualGridPowerW = value;
		request.hasVirtualEss = true;
	}
	if (parseStubControlInt(payload, "pv_ct_power_w", value)) {
		request.virtualPvCtPowerW = value;
		request.hasVirtualEss = true;
	}
	if (parseStubControlInt(payload, "inverter_mode", value) && value > 0) {
		request.virtualInverterMode = static_cast<uint16_t>(value);
		request.hasVirtualEss = true;
	}

	if (parseStubControlInt(payload, "dispatch_start", value) && value >= 0) {
		request.virtualDispatchStart = static_cast<uint16_t>(value);
		request.hasVirtualDispatch = true;
	}
	if (parseStubControlInt(payload, "dispatch_mode", value) && value >= 0) {
		request.virtualDispatchMode = static_cast<uint16_t>(value);
		request.hasVirtualDispatch = true;
	}
	if (parseStubControlInt(payload, "dispatch_active_power", value) && value >= 0) {
		request.virtualDispatchActivePower = value;
		request.hasVirtualDispatch = true;
	}
	if (parseStubControlInt(payload, "dispatch_soc", value) && value >= 0) {
		request.virtualDispatchSoc = static_cast<uint16_t>(value);
		request.hasVirtualDispatch = true;
	}
}

static void
applyRs485StubControlPayload(const char *payload)
{
	if (payload == nullptr || _modBus == nullptr) {
		return;
	}

	// Chose a helper-based parser here because ESP8266 loop stack is tight and the
	// earlier monolithic local-heavy parser triggered watchdog resets on control publish.
	Rs485StubControlRequest request{};
	const Rs485StubConfig &currentCfg = _modBus->stubConfig();
	request.mode = currentCfg.mode;
	parseRs485StubControlPayload(payload, request);
	maybeYield();

	_modBus->applyStubControl(request.mode, request.failN, request.failReg, request.failType, request.latencyMs);
	maybeYield();
	_modBus->applyAdvancedControl(
		request.strictUnknown,
		request.failEveryN,
		request.failReads,
		request.failWrites,
		request.failForMs,
		request.flapOnlineMs,
		request.flapOfflineMs,
		request.probeSuccessAfterN,
		request.socStepX10PerSnapshot);
	maybeYield();
	if (request.hasVirtualEss) {
		_modBus->applyVirtualInverterState(
			request.virtualSocPct,
			request.virtualBatteryPowerW,
			request.virtualGridPowerW,
			request.virtualPvCtPowerW,
			request.virtualInverterMode);
		maybeYield();
	}
	if (request.hasVirtualDispatch) {
		_modBus->applyVirtualDispatchState(
			request.virtualDispatchStart,
			request.virtualDispatchMode,
			request.virtualDispatchActivePower,
			request.virtualDispatchSoc);
		maybeYield();
	}
	rs485ApplyStubConnectivityMode(request.mode);
#ifdef DEBUG_OVER_SERIAL
	snprintf(_debugOutput, sizeof(_debugOutput), "RS485 stub control applied: mode=%s fail_n=%lu fail_reg=%u",
		 _modBus->stubModeLabel(), static_cast<unsigned long>(request.failN), static_cast<unsigned>(request.failReg));
	Serial.println(_debugOutput);
#endif
	// Force the next schedule pass to run ASAP so E2E tests can observe outcomes quickly.
	resendAllData = true;
}
#else
static void
applyRs485StubControlPayload(const char *payload)
{
	(void)payload;
}
#endif


/*
 * mqttCallback()
 *
 * This function is executed when an MQTT message arrives on a topic that we are subscribed to.
 */
void mqttCallback(char* topic, byte* message, unsigned int length)
{
	struct MqttCallbackGuard {
		bool &flag;
		explicit MqttCallbackGuard(bool &f) : flag(f) { flag = true; }
		~MqttCallbackGuard() { flag = false; }
	} guard(inMqttCallback);

	char mqttIncomingPayload[512] = ""; // Should be enough to cover command requests
	const mqttState *mqttEntity = NULL;

#ifdef DEBUG_OVER_SERIAL
	sprintf(_debugOutput, "Topic: %s", topic);
	Serial.println(_debugOutput);
#endif

#ifdef DEBUG_CALLBACKS
	receivedCallbacks++;
#endif // DEBUG_CALLBACKS

	if (strcmp(topic, configSetTopic) == 0) {
		// Defer config/set out of callback context, but do not reuse `_mqttPayload`:
		// sendData()/sendMqtt() may still be publishing with that shared buffer when
		// callbacks arrive from RS485 service hooks. Reuse the polling scratch instead
		// and copy it out in loop() before the config parser repurposes the scratch.
		if (!copyLengthDelimitedString(reinterpret_cast<const char *>(message),
		                               length,
		                               g_portalBucketMapScratch,
		                               sizeof(g_portalBucketMapScratch))) {
#ifdef DEBUG_OVER_SERIAL
			sprintf(_debugOutput, "mqttCallback: bad config length: %d", length);
			Serial.println(_debugOutput);
#endif
#ifdef DEBUG_CALLBACKS
			badCallbacks++;
#endif // DEBUG_CALLBACKS
			return;
		}
		pendingPollingConfigSet = true;
		return;
	}

	if (!copyLengthDelimitedString(reinterpret_cast<const char *>(message),
	                               length,
	                               mqttIncomingPayload,
	                               sizeof(mqttIncomingPayload))) {
#ifdef DEBUG_OVER_SERIAL
		sprintf(_debugOutput, "mqttCallback: bad length: %d", length);
		Serial.println(_debugOutput);
#endif
#ifdef DEBUG_CALLBACKS
		badCallbacks++;
#endif // DEBUG_CALLBACKS
		return; // We won't be doing anything
	}
#ifdef DEBUG_OVER_SERIAL
	sprintf(_debugOutput, "Payload: %d", length);
	Serial.println(_debugOutput);
	Serial.println(mqttIncomingPayload);
#endif

	// Special case for Home Assistant itself
	if (strcmp(topic, MQTT_SUB_HOMEASSISTANT) == 0) {
		if (strcmp(mqttIncomingPayload, "online") == 0) {
			requestHaDataResend();
			resendAllData = true;
		} else {
#ifdef DEBUG_OVER_SERIAL
			Serial.println("Unknown homeassistant/status: ");
			Serial.println(mqttIncomingPayload);
#endif
		}
		return; // No further processing needed.
	} else if (strcmp(topic, configSetTopic) == 0) {
		return; // handled above
#if RS485_STUB
		} else if (strcmp(topic, rs485StubControlTopic) == 0) {
			// Runtime RS485 stub control is intentionally deferred out of callback context.
			// The stub JSON parser + state application can be large enough to trigger ESP8266
			// watchdog resets if it runs while PubSubClient is inside loop().
			if (!copyLengthDelimitedString(reinterpret_cast<const char *>(message),
			                               length,
			                               pendingDeferredControlPayload,
			                               sizeof(pendingDeferredControlPayload))) {
#ifdef DEBUG_CALLBACKS
				badCallbacks++;
#endif // DEBUG_CALLBACKS
				return;
			}
			pendingRs485StubControlSet = true;
		return;
	#endif
	} else {
		const char *inverterDeviceId = discoveryDeviceIdForScope(DiscoveryDeviceScope::Inverter);
		char matchPrefix[64];

		snprintf(matchPrefix, sizeof(matchPrefix), "%s/", deviceName);
		if (inverterReady &&
		    inverterDeviceId[0] != '\0' &&
		    !strncmp(topic, matchPrefix, strlen(matchPrefix)) &&
		    !strcmp(&topic[strlen(topic) - strlen("/command")], "/command")) {
			if (mqttCommandWarmupActive()) {
				return;
			}
			const char *topicAfterDevice = &topic[strlen(matchPrefix)];
			const char *entitySep = strchr(topicAfterDevice, '/');
			if (entitySep != nullptr) {
				char topicDeviceId[64];
				char topicEntityName[64];
				size_t topicDeviceIdLen = static_cast<size_t>(entitySep - topicAfterDevice);
				int topicEntityLen = strlen(topic) - strlen(matchPrefix) - static_cast<int>(topicDeviceIdLen) - 1 - strlen("/command");
				if (topicDeviceIdLen < sizeof(topicDeviceId) && topicEntityLen > 0 && topicEntityLen < static_cast<int>(sizeof(topicEntityName))) {
					strlcpy(topicDeviceId, topicAfterDevice, topicDeviceIdLen + 1);
					strlcpy(topicEntityName, entitySep + 1, topicEntityLen + 1);
					if (!strcmp(topicDeviceId, inverterDeviceId)) {
						mqttEntity = lookupSubscription(topicEntityName);
					}
				}
			}
		}
		if (mqttEntity == NULL) {
	#ifdef DEBUG_CALLBACKS
			unknownCallbacks++;
	#endif // DEBUG_CALLBACKS
			return; // No further processing possible.
		}

		// Defer command application out of callback context to avoid deep call chains while PubSubClient
		// is executing loop() and to keep RS485 writes on the main loop path.
		if (strlen(mqttIncomingPayload) >= sizeof(pendingDeferredControlPayload)) {
	#ifdef DEBUG_CALLBACKS
			badCallbacks++;
	#endif // DEBUG_CALLBACKS
			return;
		}
		pendingEntityCommand = mqttEntity;
		strlcpy(pendingDeferredControlPayload, mqttIncomingPayload, sizeof(pendingDeferredControlPayload));
		pendingEntityCommandSet = true;
		return;
	}
}


/*
 * sendMqtt
 *
 * Sends whatever is in the modular level payload to the specified topic.
 */
bool sendMqtt(const char *topic, bool retain)
{
	static unsigned long lastFailureLogMs = 0;
	const unsigned long nowMs = millis();
	const size_t payloadLen = _mqttPayload ? strlen(_mqttPayload) : 0;

	// Avoid expensive publish attempts and large serial writes while disconnected.
	if (!_mqtt.connected()) {
#ifdef DEBUG_OVER_SERIAL
		if ((nowMs - lastFailureLogMs) >= 3000) {
			lastFailureLogMs = nowMs;
			Serial.printf("MQTT publish skipped (disconnected): topic=%s bytes=%u\r\n",
				      topic,
				      static_cast<unsigned>(payloadLen));
		}
#endif
		maybeYield();
		emptyPayload();
		return false;
	}

	// Attempt a send
	if (!_mqtt.publish(topic, _mqttPayload, retain)) {
#ifdef DEBUG_OVER_SERIAL
		if ((nowMs - lastFailureLogMs) >= 3000) {
			const size_t previewLen = (payloadLen < 96) ? payloadLen : 96;
			lastFailureLogMs = nowMs;
			Serial.printf("MQTT publish failed: topic=%s bytes=%u preview=%.*s\r\n",
				      topic,
				      static_cast<unsigned>(payloadLen),
				      static_cast<int>(previewLen),
				      _mqttPayload ? _mqttPayload : "");
		}
#endif
		maybeYield();
		emptyPayload();
		return false;
	} else {
#ifdef DEBUG_OVER_SERIAL
		//sprintf(_debugOutput, "MQTT publish success");
		//Serial.println(_debugOutput);
#endif
	}

	// Empty payload for next use.
	emptyPayload();
	return true;
}

/*
 * emptyPayload
 *
 * Clears so we start at beginning.
 */
void emptyPayload()
{
	_mqttPayload[0] = '\0';
}

void
getInverterModeDesc(char *dest, size_t size, uint16_t inverterMode)
{
	const char *inverterModeDesc = NULL;

	switch (inverterMode) {
	case INVERTER_OPERATION_MODE_WAIT_MODE:
		inverterModeDesc = INVERTER_OPERATION_MODE_WAIT_MODE_DESC;
		break;
	case INVERTER_OPERATION_MODE_ONLINE_MODE:
		inverterModeDesc = INVERTER_OPERATION_MODE_ONLINE_MODE_DESC;
		break;
	case INVERTER_OPERATION_MODE_UPS_MODE:
		inverterModeDesc = INVERTER_OPERATION_MODE_UPS_MODE_DESC;
		break;
	case INVERTER_OPERATION_MODE_BYPASS_MODE:
		inverterModeDesc = INVERTER_OPERATION_MODE_BYPASS_MODE_DESC;
		break;
	case INVERTER_OPERATION_MODE_ERROR_MODE:
		inverterModeDesc = INVERTER_OPERATION_MODE_ERROR_MODE_DESC;
		break;
	case INVERTER_OPERATION_MODE_DC_MODE:
		inverterModeDesc = INVERTER_OPERATION_MODE_DC_MODE_DESC;
		break;
	case INVERTER_OPERATION_MODE_SELF_TEST_MODE:
		inverterModeDesc = INVERTER_OPERATION_MODE_SELF_TEST_MODE_DESC;
		break;
	case INVERTER_OPERATION_MODE_CHECK_MODE:
		inverterModeDesc = INVERTER_OPERATION_MODE_CHECK_MODE_DESC;
		break;
	case INVERTER_OPERATION_MODE_UPDATE_MASTER_MODE:
		inverterModeDesc = INVERTER_OPERATION_MODE_UPDATE_MASTER_MODE_DESC;
		break;
	case INVERTER_OPERATION_MODE_UPDATE_SLAVE_MODE:
		inverterModeDesc = INVERTER_OPERATION_MODE_UPDATE_SLAVE_MODE_DESC;
		break;
	case INVERTER_OPERATION_MODE_UPDATE_ARM_MODE:
		inverterModeDesc = INVERTER_OPERATION_MODE_UPDATE_ARM_MODE_DESC;
		break;
	case UINT16_MAX:
		inverterModeDesc = "unavailable";
		break;
	default:
		snprintf(dest, size, "unknown %u", inverterMode);
		inverterModeDesc = NULL;
		break;
	}
	if (inverterModeDesc != NULL) {
		strlcpy(dest, inverterModeDesc, size);
	}
}

enum gridStatus
isGridOnline(void)
{
	enum gridStatus ret;

	if (!essSnapshotValid) {
		return gridStatus::gridUnknown;
	}
	switch (opData.essInverterMode) {
	case INVERTER_OPERATION_MODE_ONLINE_MODE:
	case INVERTER_OPERATION_MODE_CHECK_MODE:
		ret = gridStatus::gridOnline;
		break;
	case INVERTER_OPERATION_MODE_UPS_MODE:
		ret = gridStatus::gridOffline;
		break;
	case UINT16_MAX:
	default:
		ret = gridStatus::gridUnknown;
		break;
	}
	return ret;
}

void
getOpModeDesc(char *dest, size_t size, enum opMode mode)
{
	snprintf(dest, size, "Unknown %u", mode);
	switch (mode) {
	case opMode::opModePvCharge:
		strlcpy(dest, OP_MODE_DESC_PV_CHARGE, size);
		break;
	case opMode::opModeTarget:
		strlcpy(dest, OP_MODE_DESC_TARGET, size);
		break;
	case opMode::opModePush:
		strlcpy(dest, OP_MODE_DESC_PUSH, size);
		break;
	case opMode::opModeLoadFollow:
		strlcpy(dest, OP_MODE_DESC_LOAD_FOLLOW, size);
		break;
	case opMode::opModeMaxCharge:
		strlcpy(dest, OP_MODE_DESC_MAX_CHARGE, size);
		break;
	case opMode::opModeNoCharge:
		strlcpy(dest, OP_MODE_DESC_NO_CHARGE, size);
		break;
	}
}

enum opMode
lookupOpMode(const char *opModeDesc)
{
	if (!strcmp(opModeDesc, OP_MODE_DESC_PV_CHARGE))
		return opMode::opModePvCharge;
	if (!strcmp(opModeDesc, OP_MODE_DESC_TARGET))
		return opMode::opModeTarget;
	if (!strcmp(opModeDesc, OP_MODE_DESC_PUSH))
		return opMode::opModePush;
	if (!strcmp(opModeDesc, OP_MODE_DESC_LOAD_FOLLOW))
		return opMode::opModeLoadFollow;
	if (!strcmp(opModeDesc, OP_MODE_DESC_MAX_CHARGE))
		return opMode::opModeMaxCharge;
	if (!strcmp(opModeDesc, OP_MODE_DESC_NO_CHARGE))
		return opMode::opModeNoCharge;
	return (enum opMode)-1;  // Shouldn't happen
}

void
checkAndSetDispatchMode(void)
{
#ifndef DEBUG_NO_RS485
	modbusRequestAndResponseStatusValues result = modbusRequestAndResponseStatusValues::preProcessing;
	modbusRequestAndResponse response;
	uint16_t essDispatchMode, essBatterySocPct, essDispatchSoc;
	int32_t essDispatchActivePower;
	bool checkActivePower = true;

	if (!essSnapshotValid) {
		return;
	}

	if (!opData.a2mReadyToUseOpMode || !opData.a2mReadyToUseSocTarget || !opData.a2mReadyToUsePwrCharge ||
	    !opData.a2mReadyToUsePwrDischarge || !opData.a2mReadyToUsePwrPush) {
		return;  // Don't set anything if opData isn't ready.
	}

	dispatchLastRunMs = millis();

	essBatterySocPct = opData.essBatterySoc * BATTERY_SOC_MULTIPLIER;
	if (opData.a2mSocTarget == 100) {
		essDispatchSoc = 252;  // (100/DISPATCH_SOC_MULTIPLIER) = 250 but we want it a smidge higher
		// and leave power charging.  Let Alpha stop it when ready.
		essDispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET - opData.a2mPwrCharge;
	} else {
		essDispatchSoc = opData.a2mSocTarget / DISPATCH_SOC_MULTIPLIER;
		if (essBatterySocPct == opData.a2mSocTarget) {
			essDispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET;
		} else if (essBatterySocPct > opData.a2mSocTarget) {
			essDispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET + opData.a2mPwrDischarge;
		} else {
			essDispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET - opData.a2mPwrCharge;
		}
	}

	switch (opData.a2mOpMode) {
	case opMode::opModePvCharge:		// Honors Power and SOC
		essDispatchMode = DISPATCH_MODE_BATTERY_ONLY_CHARGED_VIA_PV;	// Honors Power but not SOC
		// use essDispatchActivePower from above
		break;
	case opMode::opModeTarget:		// Honors Power and SOC
		essDispatchMode = DISPATCH_MODE_STATE_OF_CHARGE_CONTROL;	// Honors Power and SOC
		// use essDispatchActivePower from above
		break;
	case opMode::opModePush:		// Honors PushPwr and SOC
		if (essBatterySocPct > opData.a2mSocTarget) {
			int32_t newBatteryPower = opData.essBatteryPower + opData.essGridPower + opData.a2mPwrPush;
			if (newBatteryPower < opData.a2mPwrPush) {
				newBatteryPower = opData.a2mPwrPush;
			}
			if (newBatteryPower > INVERTER_POWER_MAX) {
				newBatteryPower = INVERTER_POWER_MAX; // Should never happen, but just to be safe...
			}
			essDispatchMode = DISPATCH_MODE_STATE_OF_CHARGE_CONTROL;	// Honors Power and SOC
			essDispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET + newBatteryPower;
			// Smoothing - If power doesn't change much, pretend it isn't changing.
			if ((essDispatchActivePower < (opData.essDispatchActivePower + PUSH_FUDGE_FACTOR)) &&
			    (essDispatchActivePower > (opData.essDispatchActivePower - PUSH_FUDGE_FACTOR))) {
				checkActivePower = false;
			}
		} else {
			essDispatchMode = DISPATCH_MODE_NO_BATTERY_CHARGE;		// Doesn't honor Power or SOC
			essDispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET;
		}
		break;
	case opMode::opModeLoadFollow:		// Honors Power and SOC
		essDispatchMode = DISPATCH_MODE_LOAD_FOLLOWING;			// Honors Power but not SOC
		// use essDispatchActivePower from above
		break;
	case opMode::opModeMaxCharge:		// Doesn't honors Power or SOC
		essDispatchMode = DISPATCH_MODE_OPTIMISE_CONSUMPTION;		// Doesn't honor Power or SOC
		essDispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET - opData.a2mPwrCharge;
		break;
	case opMode::opModeNoCharge:		// Doesn't honors Power or SOC
		essDispatchMode = DISPATCH_MODE_NO_BATTERY_CHARGE;		// Doesn't honor Power or SOC
		essDispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET;
		break;
	default:
		return; // Shouldn't happen!  opMode is corrupt.
	}

	if ((opData.essDispatchStart != DISPATCH_START_START) ||
	    (opData.essDispatchMode != essDispatchMode) ||
	    (checkActivePower && (opData.essDispatchActivePower != essDispatchActivePower)) ||
	    (opData.essDispatchSoc != essDispatchSoc)) {
#ifdef DEBUG_OPS
		opCounter++;
#endif
		result = _registerHandler->writeDispatchRegisters(essDispatchActivePower, essDispatchMode, essDispatchSoc, &response);
		if (result != modbusRequestAndResponseStatusValues::writeDataRegisterSuccess) {
			rs485Errors++;
		}
	}
#endif // ! DEBUG_NO_RS485
}

void
getA2mOpDataFromEss(void)
{
#ifdef DEBUG_NO_RS485
	opData.a2mOpMode = opMode::opModeNoCharge;
	opData.a2mSocTarget = SOC_TARGET_MAX;
	opData.a2mPwrCharge = INVERTER_POWER_MAX;
	opData.a2mPwrDischarge = INVERTER_POWER_MAX;
#else // DEBUG_NO_RS485
	modbusRequestAndResponseStatusValues result;
	modbusRequestAndResponse response;
	// Defaults keep control logic in a safe, bounded state if ESS reads fail repeatedly.
	opData.a2mOpMode = opMode::opModeNoCharge;
	opData.a2mSocTarget = SOC_TARGET_MAX;
	opData.a2mPwrCharge = INVERTER_POWER_MAX;
	opData.a2mPwrDischarge = INVERTER_POWER_MAX;

	const uint8_t kMaxReadAttempts = 4;
	auto readWithRetries = [&](uint16_t reg, const char *name) -> bool {
		for (uint8_t attempt = 0; attempt < kMaxReadAttempts; attempt++) {
			result = _registerHandler->readHandledRegister(reg, &response);
			if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
				return true;
			}
			rs485Errors++;
#ifdef DEBUG_OVER_SERIAL
			if (attempt == 0 || (attempt + 1) == kMaxReadAttempts) {
				snprintf(_debugOutput, sizeof(_debugOutput),
					 "getA2mOpDataFromEss: %s read failed (%u/%u)",
					 name,
					 static_cast<unsigned>(attempt + 1),
					 static_cast<unsigned>(kMaxReadAttempts));
				Serial.println(_debugOutput);
			}
#endif // DEBUG_OVER_SERIAL
			diagDelay(10);
		}
		return false;
	};

	if (readWithRetries(REG_DISPATCH_RW_DISPATCH_MODE, "dispatch_mode")) {
		switch (response.unsignedShortValue) {
		case DISPATCH_MODE_BATTERY_ONLY_CHARGED_VIA_PV:
			opData.a2mOpMode = opMode::opModePvCharge;
			break;
		case DISPATCH_MODE_STATE_OF_CHARGE_CONTROL:
			opData.a2mOpMode = opMode::opModeTarget;
			break;
		case DISPATCH_MODE_LOAD_FOLLOWING:
		case DISPATCH_MODE_NORMAL_MODE:
			opData.a2mOpMode = opMode::opModeLoadFollow;
			break;
		case DISPATCH_MODE_OPTIMISE_CONSUMPTION:
		case DISPATCH_MODE_MAXIMISE_OUTPUT:
		case DISPATCH_MODE_MAXIMISE_CONSUMPTION:
			opData.a2mOpMode = opMode::opModeMaxCharge;
			break;
		case DISPATCH_MODE_NO_BATTERY_CHARGE:
			opData.a2mOpMode = opMode::opModeNoCharge;
			break;
		default:
#ifdef DEBUG_OVER_SERIAL
			snprintf(_debugOutput, sizeof(_debugOutput), "getA2mOpDataFromEss: Unhandled Dispatch Mode: %u/", response.unsignedShortValue);
			Serial.print(_debugOutput);
			Serial.println(response.dataValueFormatted);
#endif
			opData.a2mOpMode = opMode::opModeLoadFollow;
			break;
		}
	}

	if (readWithRetries(REG_DISPATCH_RW_DISPATCH_SOC, "dispatch_soc")) {
		opData.a2mSocTarget = response.unsignedShortValue * DISPATCH_SOC_MULTIPLIER;
	}

	if (readWithRetries(REG_DISPATCH_RW_ACTIVE_POWER_1, "active_power")) {
		if (response.signedIntValue > DISPATCH_ACTIVE_POWER_OFFSET) {
			opData.a2mPwrCharge = INVERTER_POWER_MAX;
			opData.a2mPwrDischarge = response.signedIntValue - DISPATCH_ACTIVE_POWER_OFFSET;
		} else if (response.signedIntValue < DISPATCH_ACTIVE_POWER_OFFSET) {
			opData.a2mPwrCharge = DISPATCH_ACTIVE_POWER_OFFSET - response.signedIntValue;
			opData.a2mPwrDischarge = INVERTER_POWER_MAX;
		} else {
			opData.a2mPwrCharge = INVERTER_POWER_MAX;
			opData.a2mPwrDischarge = INVERTER_POWER_MAX;
		}
	}
#endif // DEBUG_NO_RS485
}

#ifdef DEBUG_FREEMEM
uint32_t freeMemory()
{
	return ESP.getFreeHeap();
}
#endif // DEBUG_FREEMEM

void
setStatusLed(bool on)
{
#ifdef MP_ESPUNO_ESP32C6
	uint32_t color = on ? _statusLedColor : 0;
	_statusPixel.setPixelColor(0, color);
	_statusPixel.show();
#else // MP_ESPUNO_ESP32C6
	digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
#endif // MP_ESPUNO_ESP32C6
}

void
setStatusLedColor(uint8_t red, uint8_t green, uint8_t blue)
{
#ifdef MP_ESPUNO_ESP32C6
	_statusLedColor = _statusPixel.Color(red, green, blue);
	_statusPixel.setPixelColor(0, _statusLedColor);
	_statusPixel.show();
#else // MP_ESPUNO_ESP32C6
	bool on = (red != 0) || (green != 0) || (blue != 0);
	digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
#endif // MP_ESPUNO_ESP32C6
}

void
updateStatusLed(void)
{
	if (WiFi.status() != WL_CONNECTED) {
		setStatusLedColor(255, 0, 0);
	} else if (!_mqtt.connected()) {
		setStatusLedColor(255, 255, 0);
	} else if (!opData.essRs485Connected) {
		setStatusLedColor(128, 0, 128);
	} else {
		setStatusLedColor(0, 255, 0);
	}
}
