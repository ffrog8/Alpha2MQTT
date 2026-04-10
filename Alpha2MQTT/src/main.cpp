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
#include <new>
// Supporting files
#include "../RegisterHandler.h"
#include "../RS485Handler.h"
#include "../Definitions.h"
#include "../include/BootModes.h"
#include "../include/BootEvent.h"
#include "../include/WifiGuard.h"
#include "../include/WifiRecoveryPolicy.h"
#include "../include/BucketScheduler.h"
#include "../include/MqttEntities.h"
#include "../include/PortalConfig.h"
#include "../include/Rs485BaudSync.h"
#include "../include/ConfigCodec.h"
#include "../include/DebugLog.h"
#include "../include/MemoryHealth.h"
#include "../include/PollingConfig.h"
#include "../include/PowerSnapshot.h"
#include "../include/RebootRequest.h"
#include "../include/StatusReporting.h"
#include "../include/StatusLedPolicy.h"
#include "../include/DiscoveryModel.h"
#include "../include/DispatchTiming.h"
#include "../include/DispatchRequest.h"
#include "../include/Rs485ProbeLogic.h"
#include "../include/Rs485RuntimeReconnect.h"
#include "../include/SchedulerReadPolicy.h"
#include "../include/Scheduler.h"
#include "../include/TimeProvider.h"
#include "../include/diag.h"
#include <Arduino.h>
#if defined(ARDUINO)
#include <pgmspace.h>
#define A2M_FMT(str) PSTR(str)
#define A2M_SNPRINTF snprintf_P
#else
#define A2M_FMT(str) str
#define A2M_SNPRINTF snprintf
#endif
#if defined(MP_ESP8266) || defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Updater.h>
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

#if defined(MP_ESP8266) || defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
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
char lastResetReason[64] = "";
static HttpServer *g_httpServer = nullptr;
bool httpControlPlaneEnabled = false;
static bool inMqttCallback = false;
static uint32_t mqttCallbackSequence = 0;
static uint32_t loopSequence = 0;
// Dispatch changes can refresh ESS state before sendData() runs later in the
// same loop. sendData() reuses that primed snapshot instead of issuing a
// second snapshot refresh for the same bucket pass.
static uint32_t essSnapshotPrimedForSendDataLoop = 0;
static bool pendingPollingConfigSet = false;
static bool pendingPollingConfigPublish = false;
static bool pollingConfigLoadedFromStorage = false;
static bool pendingRs485StubControlSet = false;
static bool pendingEntityCommandSet = false;
static mqttEntityId pendingEntityCommandId = mqttEntityId::entityRegNum;
static bool pendingDispatchRequestSet = false;
static char *pendingPollingConfigPayload = nullptr;
constexpr size_t kPendingDeferredControlPayloadSize = 512;
constexpr size_t kPendingDispatchPayloadSize = 256;
constexpr size_t kStatusJsonScratchSize = 1024;
// Shared deferred-control payload buffer for small MQTT commands. MQTT pumping is
// blocked while any deferred command is pending, so stub/entity commands never
// overlap in this storage.
static char *pendingDeferredControlPayload = nullptr;
static char *pendingDispatchPayload = nullptr;
// Status and manual-read JSON publishes are serialized through the single-threaded main loop.
// Keep one shared scratch buffer instead of reserving multiple independent publish buffers.
static char *g_statusJsonScratch = nullptr;
static uint32_t manualRegisterReadSeq = 0;
constexpr uint8_t kDeferredMqttDrainMaxIterations = 16;

enum class BootMemPublishCheckpoint : uint8_t {
	PreWifi = 0,
	PostWifi,
	PostMqtt,
	PreRs485,
	PostRs485,
};

struct BootMemPublishState {
	uint32_t heapPreWifi = 0;
	uint32_t heapPostWifi = 0;
	uint32_t heapPostMqtt = 0;
	uint32_t heapPreRs485 = 0;
	uint32_t heapPostRs485 = 0;
	uint8_t validMask = 0;
};

enum PortalStatus : uint8_t {
	portalStatusIdle = 0,
	portalStatusConnecting,
	portalStatusSuccess,
	portalStatusFailed
};
PortalStatus portalStatus = portalStatusIdle;
char portalStatusReason[64] = "";
char portalStatusSsid[33] = "";
char portalSubmittedPass[64] = "";
char portalStatusIp[20] = "";
char portalUpdateCsrfToken[33] = "";
bool portalUpdateUploadStarted = false;
constexpr size_t kPollingProfileLineMaxLen = 96;
constexpr size_t kPollingProfileUploadErrorMaxLen = 72;
struct PortalPollingProfileUploadState {
	bool started = false;
	bool finished = false;
	bool ok = false;
	bool sawHeader = false;
	bool sawPollInterval = false;
	uint16_t unknownCount = 0;
	uint16_t lineNumber = 0;
	uint32_t stagedIntervalSeconds = 0;
	uint16_t entityCount = 0;
	uint16_t lineLen = 0;
	BucketId *stagedBuckets = nullptr;
	uint8_t *seenEntities = nullptr;
	char *lineBuf = nullptr;
	char *error = nullptr;
};
static PortalPollingProfileUploadState portalPollingProfileUpload;
int portalLastDisconnectReason = -1;
char portalLastDisconnectLabel[32] = "";
unsigned long portalConnectStart = 0;
bool portalNeedsMqttConfig = false;
bool portalMqttSaved = false;
bool portalRebootScheduled = false;
BootIntent portalRebootIntent = BootIntent::Normal;
unsigned long portalRebootAt = 0;
bool portalWifiCredentialsChanged = false;
uint8_t portalRouteRebindRetriesRemaining = 0;
unsigned long portalRouteRebindRetryAt = 0;
unsigned long portalLastActivityAt = 0;
bool deferredControlPlaneRebootScheduled = false;
BootIntent deferredControlPlaneRebootIntent = BootIntent::Normal;
unsigned long deferredControlPlaneRebootAt = 0;
void *portalRoutesBoundServer = nullptr;
int wifiLastDisconnectReason = -1;
char wifiLastDisconnectLabel[32] = "";
#if defined(MP_ESP8266)
static WiFiEventHandler runtimeWifiDisconnectHandler;
#endif
const char kPreferenceBootIntent[] = "Boot_Intent";
const char kPreferenceBootMode[] = "Boot_Mode";
const char kPreferenceInverterLabel[] = "Inverter_Label";
const char kPreferenceBucketMap[] = "Bucket_Map";
const char kPreferencePollInterval[] = "poll_interval_s";
const char kPreferenceRs485Baud[] = "rs485_baud";
const char kPreferenceBucketMapMigrated[] = "Bucket_Map_Migrated";
// Persisted "last polling-config change" timestamp published as polling-config last_change.
const char kPreferencePollingLastChange[] = "polling_last_change";
const char kControllerInverterSerialEntity[] = "inverter_serial";
const char kControllerModel[] = "Alpha2MQTT Bridge";
const char kInverterModelFallback[] = "Alpha ESS";
constexpr size_t kPrefBootIntentMaxLen = 24;
constexpr size_t kPrefBootModeMaxLen = 24;
constexpr size_t kPrefInverterLabelMaxLen = 11;
constexpr size_t kPrefWifiSsidMaxLen = 64;
constexpr size_t kPrefWifiPasswordMaxLen = 64;
constexpr size_t kPrefMqttServerMaxLen = 64;
constexpr size_t kPrefMqttUsernameMaxLen = 64;
constexpr size_t kPrefMqttPasswordMaxLen = 64;
// Stable "name=bucket;" persistence is larger than the old index encoding. Size these
// buffers for the full current catalog, but keep them off steady-state globals.
constexpr size_t kPrefBucketMapMaxLen = 4608;
constexpr size_t kPollingConfigSetPayloadMaxLen = 5120;
constexpr size_t kPollingConfigChunkMapMaxLen = 1024;
constexpr size_t kPrefPollingLastChangeMaxLen = 32;
static WiFiManagerParameter gPortalMqttSection("<p>MQTT settings:</p>");
static WiFiManagerParameter gPortalMqttServer("server", "MQTT server", "", 40);
static WiFiManagerParameter gPortalMqttPort("port", "MQTT port", "", 6);
static WiFiManagerParameter gPortalMqttUser("user", "MQTT user", "", 32);
static WiFiManagerParameter gPortalMqttPass("mpass", "MQTT password", "", 32);
static WiFiManagerParameter gPortalInverterLabel("inverter_label", "Inverter label (optional)", "", kPrefInverterLabelMaxLen);
static WiFiManagerParameter gPortalPollingLink("<p><a href=\"/config/polling\">Polling schedule</a></p>");
// Portal handlers run on a constrained callback stack on ESP8266.
// Keep only the small bucket assignment array persistent. Large text buffers are
// allocated on demand so NORMAL-mode runtime does not carry portal/config scratch.
static BucketId *g_portalBucketsScratch = nullptr;
static bool g_portalPollingCacheValid = false;
static size_t g_portalPollingCacheEntityCount = 0;
static uint32_t g_portalPollingCacheIntervalSeconds = kPollIntervalDefaultSeconds;
static size_t g_lastPublishedPollingConfigChunkCount = 0;
BootIntent currentBootIntent = BootIntent::Normal;
BootIntent bootIntentForPublish = BootIntent::Normal;
BootMode currentBootMode = BootMode::Normal;
BootMode bootModeForDiagnostics = BootMode::Normal;
bool mqttConfigComplete = false;
bool mqttRuntimeEnabled = false;
bool bootEventPublished = false;
bool bootMemEventPublished = false;
bool inverterReady = false;
static char g_rediscoveryPreviousSerial[sizeof(deviceSerialNumber)] = "";
static char g_rediscoveryPreviousHaUniqueId[sizeof(haUniqueId)] = "";
bool inverterSubscriptionsSet = false;
bool inverterCommandSubscriptionsSet = false;
bool inverterDispatchSubscriptionSet = false;
size_t inverterNextCommandSubscriptionIndex = 0;
SubsystemPlan bootPlan = { true, true, true };
BootMemWorst bootMemWorst = { MemLevel::Ok, BootMemStage::Boot0, { 0, 0, 0 } };
BootMemPublishState bootMemPublishState{};
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
uint32_t rs485TransportErrors = 0;
uint32_t rs485OtherErrors = 0;
uint32_t essSnapshotAttemptCount = 0;
uint32_t essPowerSnapshotLastBuildMs = 0;
uint32_t snapshotPublishSkipCount = 0;
uint32_t dispatchWaitDueToSnapshotMs = 0;
uint32_t dispatchQueueCoalesceCount = 0;
uint32_t dispatchBlockCacheHitCount = 0;
uint32_t pvBlockCacheHitCount = 0;
uint32_t pvMeterCacheHitCount = 0;
bool essSnapshotLastOk = false;
uint32_t dispatchLastRunMs = 0;
char dispatchLastSkipReason[48] = "";
static TimedDispatchRuntimeState timedDispatchState;
static char *g_dispatchRequestStatus = nullptr;
static bool dispatchRequestStatusDirty = false;
static bool dispatchMirrorPublishPending = false;
static uint32_t dispatchRequestQueuedMs = 0;
static uint32_t dispatchRequestStatusPublishEarliestLoop = 0;
static uint32_t dispatchMirrorPublishEarliestLoop = 0;
static DispatchRegisterReadback dispatchMirrorPublishReadback{};
static bool dispatchMirrorPublishReadbackValid = false;
// Readback-confirmed acceptance is what arms timed dispatch lifecycle tracking.
// Give the inverter/RS485 path a bounded settle window instead of assuming the
// registers will reflect within a handful of 25 ms retries.
static constexpr uint32_t kDispatchReadbackTimeoutMs = 2500;
static constexpr uint32_t kDispatchReadbackRetryMs = 100;
struct AtomicDispatchRuntimeState {
	bool inFlight = false;
	DispatchRequestPayload payload{};
	DispatchRequestPlan plan{};
	uint32_t queuedMs = 0;
	uint32_t readbackStartedMs = 0;
	uint32_t nextReadbackAtMs = 0;
	uint8_t readbackAttempts = 0;
};
static AtomicDispatchRuntimeState atomicDispatchState{};

struct DispatchBlockSnapshot {
	SourceGroupReadMeta meta{};
	uint16_t dispatchStart = UINT16_MAX;
	uint16_t dispatchMode = UINT16_MAX;
	int32_t dispatchActivePower = INT32_MAX;
	uint16_t dispatchSocRaw = UINT16_MAX;
	uint32_t dispatchTimeRaw = UINT32_MAX;
};

struct PvStringBlockSnapshot {
	SourceGroupReadMeta meta{};
	uint16_t voltage[kPvStringCount] = { 0, 0, 0, 0, 0, 0 };
	uint16_t current[kPvStringCount] = { 0, 0, 0, 0, 0, 0 };
	int32_t power[kPvStringCount] = { 0, 0, 0, 0, 0, 0 };
};

struct PvMeterTotalSnapshot {
	SourceGroupReadMeta meta{};
	int32_t totalPower = 0;
};

struct SchedulerPassSourceCache {
	uint32_t passId = 0;
	bool active = false;
	bool snapshotBuildInProgress = false;
	bool dispatchQueuedDuringSnapshot = false;
	uint32_t dispatchWaitDueToSnapshotMs = 0;
	uint32_t dispatchQueueCoalesceCount = 0;
	uint32_t dispatchCacheHitCount = 0;
	uint32_t pvBlockCacheHitCount = 0;
	uint32_t pvMeterCacheHitCount = 0;
	EssSnapshotMeta essSnapshot{};
	DispatchBlockSnapshot dispatch{};
	PvStringBlockSnapshot pvBlock{};
	PvMeterTotalSnapshot pvMeter{};
};

static uint32_t schedulerPassSequence = 0;
static SchedulerPassSourceCache schedulerPassCache{};
static bool essSnapshotBuildInProgress = false;
static bool essPowerSnapshotValid = false;
static constexpr size_t kEntityTopicScratchSize = 176;
static constexpr size_t kEntityTopicBaseScratchSize = 164;
static constexpr size_t kRuntimeTopicScratchSize = 192;
static constexpr size_t kStaleInverterDiscoveryQueueMax = 2;
// MQTT publish helpers are single-threaded. Keep the runtime scratch bounded to the
// actual topic lengths instead of reserving oversized publish buffers permanently in .bss.
struct MqttPublishTopicScratch {
	char topic[kEntityTopicScratchSize];
	char topicBase[kEntityTopicBaseScratchSize];
	char entityKey[64];
};

struct RuntimeScratch {
	modbusRequestAndResponse modbusReadScratch{};
	MqttPublishTopicScratch publishTopic{};
	char inverterSubscription[kRuntimeTopicScratchSize] = "";
	char inverterSubscriptionEntityKey[64] = "";
	mqttState inverterSubscriptionEntity{};
};

static RuntimeScratch *g_runtimeScratch = nullptr;

struct ControllerDiscoveryClearScratch {
	char deviceIds[kStaleInverterDiscoveryQueueMax][64] = {{0}};
	char lastQueued[64] = "";
};

static ControllerDiscoveryClearScratch *g_controllerDiscoveryClearScratch = nullptr;

enum class RuntimeDiagPhase : uint8_t {
	None = 0,
	PollSnapshot,
	BucketPublish,
	StatusPublish,
	DispatchWrite,
	DispatchReadback,
	DispatchForcePublish,
};

struct RuntimeDiagTracker {
	bool minimaValid = false;
	uint32_t minFreeHeap = 0;
	uint32_t minMaxBlock = 0;
	uint8_t maxFragPct = 0;
	bool worstValid = false;
	RuntimeDiagPhase worstPhase = RuntimeDiagPhase::None;
	MemSample worstSample{};
	uint32_t mqttMaxPayloadSeen = 0;
	const char *mqttMaxPayloadKind = "";
};

static RuntimeDiagTracker runtimeDiag{};
static RuntimeDiagPhase currentRuntimeDiagPhase = RuntimeDiagPhase::None;
static const char *currentRuntimeDiagPayloadKind = "";

struct RuntimeDiagScope {
	RuntimeDiagPhase previousPhase;
	const char *previousPayloadKind;

	RuntimeDiagScope(RuntimeDiagPhase phase, const char *payloadKind = "")
		: previousPhase(currentRuntimeDiagPhase), previousPayloadKind(currentRuntimeDiagPayloadKind)
	{
		currentRuntimeDiagPhase = phase;
		currentRuntimeDiagPayloadKind = (payloadKind != nullptr) ? payloadKind : "";
	}

	~RuntimeDiagScope()
	{
		currentRuntimeDiagPhase = previousPhase;
		currentRuntimeDiagPayloadKind = previousPayloadKind;
	}
};

uint32_t schedTenSecLastRunMs = 0;
uint32_t schedOneMinLastRunMs = 0;
uint32_t schedFiveMinLastRunMs = 0;
uint32_t schedOneHourLastRunMs = 0;
uint32_t schedOneDayLastRunMs = 0;
bool lastWifiConnected = false;
bool lastMqttConnected = false;
bool pendingWifiDisconnectEvent = false;
bool pendingMqttDisconnectEvent = false;
uint32_t mqttActivityPulseUntilMs = 0;
static constexpr uint32_t kMqttActivityPulseMs = 60U;
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
bool resendHaClearStaleControllerPending = false;
size_t resendHaClearStaleInverterIndex = 0;
size_t resendHaClearStaleInverterQueueIndex = 0;
size_t resendHaClearStaleInverterQueueCount = 0;
char resendHaClearStaleInverterDeviceIds[kStaleInverterDiscoveryQueueMax][64] = {{0}};
size_t resendHaClearStaleControllerIndex = 0;
size_t resendHaClearStaleControllerQueueIndex = 0;
size_t resendHaClearStaleControllerQueueCount = 0;
bool resendAllData = false;
#if RS485_STUB
uint32_t rs485StubControlSchedulerCooldownUntilMs = 0;
bool rs485StubSkipNextScheduledStatusPublish = false;
bool rs485StubStatusAckPending = false;
bool rs485StubControlProcessedThisLoop = false;
uint32_t rs485StubLastOnlineControlMs = 0;
#endif
static constexpr uint8_t kBootstrapPublishPerTick = 3;
static constexpr size_t kBootstrapPublishBitsetBytes =
	(kMqttEntityDescriptorCount + 7U) / 8U;
bool bootstrapPublishPending = false;
size_t bootstrapPublishCursor = 0;
uint8_t bootstrapPublishedEntities[kBootstrapPublishBitsetBytes] = {};
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
	String inverterLabel;
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
static void resetBootstrapPublishState(bool pending);
static bool bootstrapEntityWasPublished(size_t idx);
static void markBootstrapEntityPublished(size_t idx);
static void serviceBootstrapPublishPass(void);
static bool pollingBudgetExceeded(void);
static bool formatPollingBudgetEntityValue(mqttEntityId entityId, char *out, size_t outSize);

#ifdef MP_ESPUNO_ESP32C6
Adafruit_NeoPixel _statusPixel(1, LED_BUILTIN, NEO_GRB + NEO_KHZ800);
uint32_t _statusLedColor = 0;
#endif // MP_ESPUNO_ESP32C6

//#define OP_DATA_AVG_CNT 4
#define PUSH_FUDGE_FACTOR 200 // Watts
struct {
	opMode   a2mOpMode = opMode::opModeNormal;
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
	uint32_t essDispatchTime = 0;
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
static constexpr uint32_t kRs485BaudRetryMs = 5000;

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
static Rs485RuntimeReconnectTracker rs485RuntimeReconnect{};
static Rs485BaudTracker rs485BaudTracker{};
static uint32_t rs485BaudNextActionAtMs = 0;

static bool rs485TryReadIdentityOnce(void);
static void rs485ProbeTick(void);
static void resetRs485ProbeState(unsigned long now);
static void noteRs485ConnectedEpoch(void);
static void beginRs485RuntimeRediscovery(const char *reason);
static void serviceRs485BaudReconcile(void);
static bool readLiveRs485Baud(uint32_t &baudOut,
                              modbusRequestAndResponseStatusValues *resultOut,
                              const char **detailOut);
static void seedMissingConfiguredRs485BaudAfterProbe(void);
#if RS485_STUB
static void rs485ApplyStubConnectivityMode(Rs485StubMode mode);
struct Rs485StubControlRequest;
static Rs485StubControlRequest *ensureRs485StubControlRequestScratch(void);
static bool parseRs485StubControlPayload(const char *payload, Rs485StubControlRequest &request);
static void applyParsedRs485StubControl(const Rs485StubControlRequest &request);
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
static bool refreshEssSnapshotAfterDispatch(bool primeForCurrentSendDataPass);
static void beginSchedulerPass(void);
static void endSchedulerPass(void);
static bool fetchDispatchBlockSnapshot(DispatchBlockSnapshot &out,
                                       modbusRequestAndResponseStatusValues *resultOut = nullptr);
static bool fetchPvStringBlockSnapshot(PvStringBlockSnapshot &out,
                                       modbusRequestAndResponseStatusValues *resultOut = nullptr);
static bool fetchPvMeterTotalSnapshot(PvMeterTotalSnapshot &out,
                                      modbusRequestAndResponseStatusValues *resultOut = nullptr);
static bool prepareDispatchBlockResponse(mqttEntityId entityId,
                                         const DispatchBlockSnapshot &snapshot,
                                         modbusRequestAndResponse &prepared);
static bool formatDispatchStartValue(char *dest, size_t destSize, uint16_t dispatchStart);
static bool formatDispatchModeValue(char *dest, size_t destSize, uint16_t dispatchMode);
static bool preparePvBlockResponse(const mqttState &entity,
                                   const PvStringBlockSnapshot &snapshot,
                                   modbusRequestAndResponse &prepared);
static void executeDispatchBlockTransaction(const MqttEntityActiveBucket &bucketPlan,
                                            const MqttPollTransaction &transaction);
static void executePvBlockTransaction(const MqttEntityActiveBucket &bucketPlan,
                                      const MqttPollTransaction &transaction);
static void executeGenericPollTransaction(const MqttEntityActiveBucket &bucketPlan,
                                          const MqttPollTransaction &transaction,
                                          const mqttState &leader);
void sendData(void);
void sendStatus(bool includeEssSnapshot);
static void populateStatusPollSnapshot(StatusPollSnapshot &poll, bool includeEssSnapshot);
#if RS485_STUB
static bool populateStatusStubSnapshot(StatusStubSnapshot &stub);
#endif
static bool publishStatusPollSnapshot(const StatusPollSnapshot &poll);
#if RS485_STUB
static bool publishStatusStubSnapshot(const StatusStubSnapshot &stub);
static bool publishStubControlStatusNow(bool includeEssSnapshot);
#endif
void updateRunstate(void);
uint32_t getUptimeSeconds(void);
bool checkTimer(unsigned long *lastRun, unsigned long interval);
void emptyPayload(void);
bool sendMqtt(const char*, bool);
bool sendDataFromMqttState(const mqttState*,
                           bool,
                           const modbusRequestAndResponse *preparedResponse = nullptr,
                           bool forcePublish = false);
void loadPollingConfig(void);
void recomputeBucketCounts(void);
void publishPollingConfig(void);
bool publishConfigDiscovery(void);
static bool publishHaEntityDiscovery(const mqttState*);
bool clearHaEntityDiscovery(const mqttState*, const char *deviceId);
bool publishControllerInverterSerialDiscovery(void);
void publishControllerInverterSerialState(void);
bool handlePollingConfigSet(char*);
static bool subscribeMqttReconnectTopic(bool current, const char *topic);
static bool __attribute__((noinline)) subscribeMqttReconnectTopics(bool *inverterSubscriptionsAdded);
static bool __attribute__((noinline)) subscribeInverterCommandTopicByIndex(size_t idx, bool *eligibleOut);
static bool __attribute__((noinline)) subscribeInverterDispatchTopic(void);
#if defined(MP_ESP8266)
static void guardPendingAsyncWifiScanBeforeMqttReconnect(void);
#endif
#ifdef DEBUG_OVER_SERIAL
static void debugLogMqttReconnectProbe(void);
#endif
static void __attribute__((noinline))
completeSuccessfulMqttReconnect(unsigned long attemptStart, int tries, bool inverterSubscriptionsAdded);
static bool lookupSubscription(char *entityName, mqttState *outEntity);
static bool lookupSubscriptionSpan(const char *entityName, size_t entityNameLen, mqttState *outEntity);
static bool lookupEntity(mqttEntityId entityId, mqttState *outEntity);
static bool lookupEntityIndex(mqttEntityId entityId, size_t *outIdx);
const char* mqttUpdateFreqToString(mqttUpdateFreq);
bool mqttUpdateFreqFromString(const char*, mqttUpdateFreq*);
void updatePollingLastChange(void);
void getPollingTimestamp(char*, size_t);
void buildPollingKey(const mqttState*, char*, size_t);
static bool buildPersistedPollingConfigMapForCatalog(const mqttState *entities,
                                                     size_t entityCount,
                                                     const BucketId *buckets,
                                                     char *map,
                                                     size_t mapLen);
static bool readLegacyPollingPref(size_t index,
                                  const mqttState *entity,
                                  int defaultValue,
                                  int &storedValue,
                                  void *context);
static void dispatchService(void);
static void __attribute__((noinline)) serviceDeferredMqttWork(void);
static void publishDispatchStateEntity(mqttEntityId entityId);
static void publishDispatchAuxiliaryStates(bool publishRawTime);
static bool publishDispatchAuxiliaryStatesIfReady(bool publishRawTime);
static bool computeDispatchCommand(uint16_t &essDispatchMode,
                                   int32_t &essDispatchActivePower,
                                   uint16_t &essDispatchSoc,
                                   bool &checkActivePower);
static bool isRetiredLegacyDispatchControlEntity(mqttEntityId entityId);
static bool includeEntityInPublicSurfaces(const mqttState &entity);
static bool includeEntityInPollingPortal(const mqttState &entity);
static void setDispatchRequestStatus(const char *status);
static void resetAtomicDispatchState(void);
static bool publishDispatchRequestStatus(void);
static const char *runtimeDiagPhaseName(RuntimeDiagPhase phase);
static bool runtimeDiagSampleIsWorse(const MemSample &candidate, const MemSample &currentWorst);
static void noteRuntimeMemObservation(const MemSample &sample);
static void noteRuntimePhaseObservation(RuntimeDiagPhase phase);
static void noteTrackedMqttPayload(const char *kind, size_t payloadLen);
static void noteMqttActivityPulse(void);
static bool publishTrackedTextPayload(const char *topic, const char *payload, bool retain);
static bool readDispatchRegisterReadback(const DispatchRequestPlan &plan,
                                         DispatchRegisterReadback &readback,
                                         char *error,
                                         size_t errorSize);
static void recordBootMemPublishCheckpoint(BootMemPublishCheckpoint checkpoint);
static bool bootMemPublishReady(void);
static bool forcePublishDispatchMirror(void);
static bool publishPendingDispatchMirror(void);
static void processPendingDispatchRequest(void);
static void serviceAtomicDispatchRequest(void);
static void handleMqttReconnectDispatchReset(void);
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
void publishBootMemEventOncePerBoot(void);
void setMqttIdentifiersFromSerial(const char *serial);
void queueStaleInverterDiscoveryClear(const char *deviceId);
void queueStaleControllerDiscoveryClear(const char *deviceId);
static void queueCurrentHaDiscoveryClears(void);
bool inverterSerialKnown(void);
const char *discoveryDeviceIdForScope(DiscoveryDeviceScope scope);
#if RS485_STUB
static void primeStubRuntimeInverterIdentity(const char *serial);
#endif
void publishStatusNow(void);
void publishEvent(MqttEventCode code, const char *detail);
MqttEventCode eventCodeFromResult(modbusRequestAndResponseStatusValues result);
enum class Rs485ErrorClass : uint8_t;
static Rs485ErrorClass classifyRs485Error(modbusRequestAndResponseStatusValues result);
static void recordRs485Error(modbusRequestAndResponseStatusValues result, uint32_t count = 1);
void noteRs485Error(modbusRequestAndResponseStatusValues result, const char *detail);
static void maybeYield(void);
static void processPendingEntityCommand(void);
static bool entityCommandEnabled(mqttEntityId entityId);
static bool applyMaxFeedinPercentCommand(const mqttState *entity, int32_t requestedPercent);
static void __attribute__((noinline)) applyRs485StubControlPayload(const char *payload);
static void __attribute__((noinline)) servicePendingRs485StubControl(void);
static bool __attribute__((noinline)) servicePendingRs485StubControlInLoop(void);
static void publishManualRegisterReadState(int32_t requestedReg);
void setBootIntentAndReboot(BootIntent intent, bool persistIntent = true);
static void scheduleDeferredControlPlaneReboot(BootIntent intent, unsigned long delayMs = 1500);
static void schedulePortalReboot(BootIntent intent, unsigned long delayMs = 1500);
static void persistUserBootIntent(BootIntent intent);
static void persistUserBootMode(BootMode mode);
static void persistUserMqttConfig(const char *server, int port, const char *user, const char *pass);
static void persistUserWifiCredentials(const char *ssid, const char *pass);
static void clearUserWifiCredentials(void);
static void clearSdkWifiCredentials(void);
static void beginWifiStationWithStoredCredentials(void);
static bool syncPortalWifiCredentials(WiFiManager *wifiManager, const char *ssidHint = nullptr, const char *passHint = nullptr);
static void persistUserExtAntenna(bool enabled);
static void persistUserInverterLabel(const char *label);
static bool persistUserBucketMap(const char *bucketMap);
static bool persistUserPollingConfig(uint32_t intervalSeconds, const char *bucketMap);
static void persistUserPollingLastChange(const char *lastChange);
static bool resetPollingConfigToDefaults(void);
static bool resetBucketsToCatalogDefaults(BucketId *buckets, size_t entityCount);
static bool recoverPollingConfigLoadToDefaults(const char *context,
                                               BucketId *buckets,
                                               size_t entityCount,
                                               uint32_t &outIntervalSeconds);
static void handlePortalMenuPage(WiFiManager &wifiManager);
static void handlePortalPollingPage(WiFiManager &wifiManager);
static void handlePortalPollingExport(WiFiManager &wifiManager);
static void handlePortalPollingImportPage(WiFiManager &wifiManager);
static void handlePortalPollingImport(WiFiManager &wifiManager);
static void handlePortalPollingImportUpload(WiFiManager &wifiManager);
static void handlePortalPollingReset(WiFiManager &wifiManager);
static void handlePortalPollingSave(WiFiManager &wifiManager);
static void handlePortalPollingClear(WiFiManager &wifiManager);
static void handlePortalWifiPage(WiFiManager &wifiManager);
static void handlePortalWifiSave(WiFiManager &wifiManager);
static void handlePortalRs485Page(WiFiManager &wifiManager);
static void handlePortalRs485Save(WiFiManager &wifiManager);
static void handlePortalParamPage(WiFiManager &wifiManager);
static void handlePortalParamSave(WiFiManager &wifiManager);
static void handlePortalUpdatePage(WiFiManager &wifiManager);
static void handlePortalUpdateUpload(WiFiManager &wifiManager);
static void handlePortalUpdatePost(WiFiManager &wifiManager);
static void clearPendingPollingConfigPayload(void);
static bool portalResolveEntityToken(const char *token, size_t entityCount, size_t &resolvedIndex);
static bool portalApplyBucketMapString(const char *map,
                                       size_t entityCount,
                                       BucketId *buckets,
                                       uint32_t &unknownEntityCount,
                                       uint32_t &invalidBucketCount,
                                       uint32_t &duplicateEntityCount);
static void ensurePortalPollingRuntimeReady(void);
static void preparePortalPollingRuntime(void);
static void primePortalPollingCache(void);
static bool portalEstimatePersistedBucketMap(const BucketId *buckets,
                                             size_t entityCount,
                                             size_t &used,
                                             size_t &appliedCount);
static bool portalBuildPersistedBucketMap(const BucketId *buckets,
                                          size_t entityCount,
                                          char *out,
                                          size_t outSize,
                                          size_t &appliedCount);
static void refreshPortalUpdateCsrfToken(void);
static bool portalUpdateRequestHasValidToken(WiFiManager *wifiManager);
static void resetPortalPollingProfileUploadState(void);
static void refreshPortalCustomParameters(void);
static bool isWifiConfigComplete(void);
static bool isMqttConfigComplete(void);
static bool mqttSubsystemEnabled(void);
static BootIntent portalRestartIntent(void);
static BootIntent portalNormalRebootIntent(void);
bool inverterLabelOverrideIsValid(const char *labelOverride);
bool applyLegacyBucketMapString(const char *map,
                                const mqttState *entities,
                                size_t entityCount,
                                BucketId *buckets,
                                uint32_t &unknownEntityCount,
                                uint32_t &invalidBucketCount,
                                uint32_t &duplicateEntityCount);
static void clearRuntimeInverterIdentity(void);
static bool applyLiveInverterIdentity(const char *serial);
static void persistDefaultsIfMissing(void);
void setupHttpControlPlane(void);
void handleHttpRoot(void);
void handleHttpPollingReset(void);
void handleHttpRestartAlias(void);
void handleRebootNormal(void);
void handleRebootApConfirm(void);
void handleRebootAp(void);
static bool portalRequestHasMqttFields(WiFiManager &wifiManager);
void handleRebootWifi(void);
static bool sendRebootHandoffPage(HttpServer *server, BootIntent intent);
void triggerRestart(void);
bool subscribeInverterTopics(void);
void serviceRs485Hooks(void);
const char* portalStatusLabel(PortalStatus status);
const char* wifiStatusReason(wl_status_t status);
const char* wifiStatusLabel(wl_status_t status);
const char* wifiModeLabel(WiFiMode_t mode);
static const char* wifiDisconnectReasonLabel(int reason);
static void notePortalActivity(void);
static void clearWifiFailureTracking(void);
static WifiFailureSignals currentWifiFailureSignals(void);
void handlePortalStatusRequest(WiFiManager& wifiManager);
static void handlePortalRestartRequest(WiFiManager& wifiManager);
void handlePortalRebootNormalRequest(WiFiManager& wifiManager);
bool portalHasPersistedWifiCredentials(void);
void configHandlerSta(void);
const char *portalCustomHeadElement(void);
static inline bool isMqttPumpBlocked(void);
static bool pumpMqttOnce(void);

static bool
ensureSharedRuntimeBuffer(char *&buffer, size_t size)
{
	if (buffer != nullptr) {
		return true;
	}
	buffer = new (std::nothrow) char[size];
	if (buffer == nullptr) {
		return false;
	}
	buffer[0] = '\0';
	return true;
}

static bool
ensureStatusJsonScratch(void)
{
	return ensureSharedRuntimeBuffer(g_statusJsonScratch, kStatusJsonScratchSize);
}

static bool
ensureDispatchRequestStatusBuffer(void)
{
	return ensureSharedRuntimeBuffer(g_dispatchRequestStatus, 96);
}

static bool
ensureDeferredControlPayload(void)
{
	return ensureSharedRuntimeBuffer(pendingDeferredControlPayload, kPendingDeferredControlPayloadSize);
}

static bool
ensureDispatchPayload(void)
{
	return ensureSharedRuntimeBuffer(pendingDispatchPayload, kPendingDispatchPayloadSize);
}

static bool
ensureRuntimeScratch(void)
{
	if (g_runtimeScratch != nullptr) {
		return true;
	}
	g_runtimeScratch = new (std::nothrow) RuntimeScratch();
	return g_runtimeScratch != nullptr;
}

static bool
ensureControllerDiscoveryClearScratch(void)
{
	if (g_controllerDiscoveryClearScratch != nullptr) {
		return true;
	}
	g_controllerDiscoveryClearScratch = new (std::nothrow) ControllerDiscoveryClearScratch();
	return g_controllerDiscoveryClearScratch != nullptr;
}

static modbusRequestAndResponse *
runtimeModbusReadScratch(void)
{
	if (!ensureRuntimeScratch()) {
		return nullptr;
	}
	return &g_runtimeScratch->modbusReadScratch;
}

static MqttPublishTopicScratch *
runtimePublishTopicScratch(void)
{
	if (!ensureRuntimeScratch()) {
		return nullptr;
	}
	return &g_runtimeScratch->publishTopic;
}

static bool
ensureNormalRuntimeBuffers(void)
{
	return ensureStatusJsonScratch() &&
	       ensureDeferredControlPayload() &&
	       ensureDispatchPayload();
}

static bool
topicEqualsDeviceSuffix(const char *topic, const char *suffix)
{
	if (topic == nullptr || suffix == nullptr || deviceName[0] == '\0') {
		return false;
	}
	const size_t deviceLen = strlen(deviceName);
	return strncmp(topic, deviceName, deviceLen) == 0 &&
	       strcmp(topic + deviceLen, suffix) == 0;
}

static bool
ensurePortalBucketsScratch(void)
{
	if (g_portalBucketsScratch != nullptr) {
		return true;
	}
	g_portalBucketsScratch = new (std::nothrow) BucketId[kMqttEntityDescriptorCount];
	return g_portalBucketsScratch != nullptr;
}

static bool
ensureHttpServer(void)
{
	if (g_httpServer != nullptr) {
		return true;
	}
	g_httpServer = new (std::nothrow) HttpServer(80);
	return g_httpServer != nullptr;
}

static HttpServer &
httpServerRef(void)
{
	return *g_httpServer;
}

void
buildDeviceName(void)
{
	uint8_t mac[6] = { 0 };
	WiFi.macAddress(mac);
	snprintf(deviceName, sizeof(deviceName), "%s-%02X%02X%02X", DEVICE_NAME, mac[3], mac[4], mac[5]);
	buildControllerIdentifier(mac, controllerIdentifier, sizeof(controllerIdentifier));
	snprintf(statusTopic, sizeof(statusTopic), "%s/status", deviceName);
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
	if (!inverterReady || !inverterSerialIsValid(deviceSerialNumber)) {
		return "";
	}
	static char inverterIdentifier[64];
	buildInverterIdentifier(deviceSerialNumber, inverterIdentifier, sizeof(inverterIdentifier));
	return inverterIdentifier;
}

static bool
includeEntityInPublicSurfaces(const mqttState &entity)
{
	return mqttEntityIncludedInPublicSurface(&entity);
}

static bool
includeEntityInPollingPortal(const mqttState &entity)
{
	return includeEntityInPublicSurfaces(entity);
}

static inline bool
loopSequenceReached(uint32_t earliestLoop)
{
	return static_cast<int32_t>(loopSequence - earliestLoop) >= 0;
}

static bool
legacyDispatchControlSurfaceEnabled(void)
{
	return false;
}

static void
setDispatchRequestStatus(const char *status)
{
	if (!ensureDispatchRequestStatusBuffer()) {
		A2M_DEBUG_LINE("dq:stat:oom");
		return;
	}
	strlcpy(g_dispatchRequestStatus, (status != nullptr) ? status : "", 96);
	dispatchRequestStatusDirty = true;
	dispatchRequestStatusPublishEarliestLoop = loopSequence + 1;
	A2M_DEBUG_PRINTF("dq:stat:set:%s\r\n", g_dispatchRequestStatus);
}

static void
resetAtomicDispatchState(void)
{
	pendingDispatchRequestSet = false;
	if (pendingDispatchPayload != nullptr) {
		pendingDispatchPayload[0] = '\0';
	}
	atomicDispatchState = AtomicDispatchRuntimeState{};
}

static void
handleMqttReconnectDispatchReset(void)
{
	const DispatchReconnectResetPlan resetPlan =
		dispatchReconnectResetPlan(atomicDispatchState.inFlight);
	if (resetPlan.clearStatus) {
		setDispatchRequestStatus("");
	}
	if (resetPlan.clearPendingRequest) {
		pendingDispatchRequestSet = false;
	}
	if (resetPlan.clearPendingPayload) {
		if (pendingDispatchPayload != nullptr) {
			pendingDispatchPayload[0] = '\0';
		}
	}
	// MQTT reconnects should only clear MQTT-facing request state. Preserving
	// timedDispatchState and pending mirror replay avoids re-arming
	// bootStopPending mid-run and lets disabled Dispatch_* mirrors republish
	// after a transient broker disconnect. Preserve an in-flight atomic
	// dispatch confirmation window for the same reason.
	if (resetPlan.clearInFlightState) {
		atomicDispatchState = AtomicDispatchRuntimeState{};
	}
	if (resetPlan.clearQueuedTimestamp) {
		dispatchRequestQueuedMs = 0;
	}
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
	inverterCommandSubscriptionsSet = false;
	inverterDispatchSubscriptionSet = false;
	inverterNextCommandSubscriptionIndex = 0;
}

static void
clearRuntimeInverterIdentity(void)
{
	deviceSerialNumber[0] = '\0';
	strlcpy(haUniqueId, "A2M-UNKNOWN", sizeof(haUniqueId));
	inverterReady = false;
	inverterSubscriptionsSet = false;
	inverterCommandSubscriptionsSet = false;
	inverterDispatchSubscriptionSet = false;
	inverterNextCommandSubscriptionIndex = 0;
	if (_registerHandler != NULL) {
		_registerHandler->setSerialNumberPrefix('\0', '\0');
	}
}

static void
preserveRuntimeIdentityForRediscovery(void)
{
	if (deviceSerialNumber[0] == '\0' || !inverterSerialIsValid(deviceSerialNumber)) {
		return;
	}
	strlcpy(g_rediscoveryPreviousSerial, deviceSerialNumber, sizeof(g_rediscoveryPreviousSerial));
	if (haUniqueId[0] != '\0' && strcmp(haUniqueId, "A2M-UNKNOWN") != 0) {
		strlcpy(g_rediscoveryPreviousHaUniqueId, haUniqueId, sizeof(g_rediscoveryPreviousHaUniqueId));
	}
}

#if RS485_STUB
static void
primeStubRuntimeInverterIdentity(const char *serial)
{
	if (!inverterSerialIsValid(serial)) {
		return;
	}
	const bool serialChanged = strcmp(deviceSerialNumber, serial) != 0;
	strlcpy(deviceSerialNumber, serial, sizeof(deviceSerialNumber));
	if (_registerHandler != NULL) {
		_registerHandler->setSerialNumberPrefix(deviceSerialNumber[0], deviceSerialNumber[1]);
	}
	if (serialChanged || haUniqueId[0] == '\0' || strcmp(haUniqueId, "A2M-UNKNOWN") == 0) {
		setMqttIdentifiersFromSerial(deviceSerialNumber);
	}
}

static void
applyStubRuntimeInverterIdentity(const char *serial)
{
	primeStubRuntimeInverterIdentity(serial);
	inverterReady = inverterSerialKnown();
}
#endif

static bool
applyLiveInverterIdentity(const char *serial)
{
	if (!inverterSerialIsValid(serial)) {
		return false;
	}

	char previousSerial[sizeof(deviceSerialNumber)];
	if (deviceSerialNumber[0] != '\0') {
		strlcpy(previousSerial, deviceSerialNumber, sizeof(previousSerial));
	} else {
		strlcpy(previousSerial, g_rediscoveryPreviousSerial, sizeof(previousSerial));
	}
	char previousHaUniqueId[sizeof(haUniqueId)];
	if (haUniqueId[0] != '\0' && strcmp(haUniqueId, "A2M-UNKNOWN") != 0) {
		strlcpy(previousHaUniqueId, haUniqueId, sizeof(previousHaUniqueId));
	} else {
		strlcpy(previousHaUniqueId, g_rediscoveryPreviousHaUniqueId, sizeof(previousHaUniqueId));
	}
	char staleInverterIdentifier[64];
	const bool staleInverterNamespace = buildStaleInverterIdentifier(previousSerial,
	                                                                 serial,
	                                                                 staleInverterIdentifier,
	                                                                 sizeof(staleInverterIdentifier));
	char currentLegacyHaUniqueId[sizeof(haUniqueId)];
	buildInverterHaUniqueId(serial, currentLegacyHaUniqueId, sizeof(currentLegacyHaUniqueId));
	const bool runtimeIdentityUnknown =
		(haUniqueId[0] == '\0') || (strcmp(haUniqueId, "A2M-UNKNOWN") == 0);

	strlcpy(deviceSerialNumber, serial, sizeof(deviceSerialNumber));
	if (_registerHandler != NULL) {
		_registerHandler->setSerialNumberPrefix(deviceSerialNumber[0], deviceSerialNumber[1]);
	}
	inverterReady = true;

	auto queueLegacyControllerClearIfNeeded = [&](const char *deviceId) {
		if (!ensureControllerDiscoveryClearScratch()) {
			return;
		}
		if (deviceId == nullptr || deviceId[0] == '\0' || strcmp(deviceId, "A2M-UNKNOWN") == 0 ||
		    strcmp(deviceId, controllerIdentifier) == 0) {
			return;
		}
		if (strcmp(g_controllerDiscoveryClearScratch->lastQueued, deviceId) == 0) {
			return;
		}
		queueStaleControllerDiscoveryClear(deviceId);
		strlcpy(g_controllerDiscoveryClearScratch->lastQueued,
		        deviceId,
		        sizeof(g_controllerDiscoveryClearScratch->lastQueued));
	};

	queueLegacyControllerClearIfNeeded(currentLegacyHaUniqueId);
	queueLegacyControllerClearIfNeeded(previousHaUniqueId);

	if (!inverterHaUniqueIdMatchesSerial(previousHaUniqueId, deviceSerialNumber)) {
		if (staleInverterNamespace) {
			queueStaleInverterDiscoveryClear(staleInverterIdentifier);
		}
		if (previousHaUniqueId[0] != '\0' &&
		    strcmp(previousHaUniqueId, "A2M-UNKNOWN") != 0 &&
		    strcmp(previousHaUniqueId, currentLegacyHaUniqueId) != 0) {
			queueStaleInverterDiscoveryClear(previousHaUniqueId);
		}
		setMqttIdentifiersFromSerial(deviceSerialNumber);
	} else if (runtimeIdentityUnknown) {
		setMqttIdentifiersFromSerial(deviceSerialNumber);
	}

	g_rediscoveryPreviousSerial[0] = '\0';
	g_rediscoveryPreviousHaUniqueId[0] = '\0';

	return true;
}

static void
resetRs485ProbeState(unsigned long now)
{
	rs485BaudIndex = -1;
	rs485AttemptsInCycle = 0;
	rs485CycleBackoffMs = kRs485ProbeAttemptDelayMs;
	rs485NextAttemptAtMs = now;
	rs485LockedBaud = 0;
}

static void
noteRs485ConnectedEpoch(void)
{
	rs485RuntimeReconnectOnConnected(rs485RuntimeReconnect);
	rs485BaudTrackerOnConnected(rs485BaudTracker);
	rs485BaudNextActionAtMs = millis();
}

static void
beginRs485RuntimeRediscovery(const char *reason)
{
#ifdef DEBUG_OVER_SERIAL
	snprintf(_debugOutput,
	         sizeof(_debugOutput),
	         "RS485 runtime rediscovery: %s",
	         reason != nullptr ? reason : "unknown");
	Serial.println(_debugOutput);
#else
	(void)reason;
#endif

	preserveRuntimeIdentityForRediscovery();
	clearRuntimeInverterIdentity();
	strlcpy(deviceBatteryType, "UNKNOWN", sizeof(deviceBatteryType));
	opData.essRs485Connected = false;
	essSnapshotValid = false;
	essSnapshotLastOk = false;
	essPowerSnapshotValid = false;
	essPowerSnapshotLastBuildMs = 0;
	essSnapshotPrimedForSendDataLoop = 0;
	strlcpy(dispatchLastSkipReason, "rs485_runtime_loss", sizeof(dispatchLastSkipReason));
	rs485RuntimeReconnectOnRediscoveryStart(rs485RuntimeReconnect);
	rs485BaudTracker.actualBaud = 0;
	rs485BaudTracker.syncState = Rs485BaudSyncState::Unknown;
	rs485BaudNextActionAtMs = 0;
	resetRs485ProbeState(millis());
	rs485ConnectState = Rs485ConnectState::ProbingBaud;
	resendAllData = true;
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
queueStaleControllerDiscoveryClear(const char *deviceId)
{
	if (deviceId == nullptr || deviceId[0] == '\0') {
		return;
	}
	if (!ensureControllerDiscoveryClearScratch()) {
		return;
	}
	for (size_t i = 0; i < resendHaClearStaleControllerQueueCount; ++i) {
		if (strcmp(g_controllerDiscoveryClearScratch->deviceIds[i], deviceId) == 0) {
			resendHaClearStaleControllerPending = true;
			return;
		}
	}
	if (resendHaClearStaleControllerQueueCount >= kStaleInverterDiscoveryQueueMax) {
		return;
	}
	strlcpy(g_controllerDiscoveryClearScratch->deviceIds[resendHaClearStaleControllerQueueCount],
	        deviceId,
	        sizeof(g_controllerDiscoveryClearScratch->deviceIds[0]));
	resendHaClearStaleControllerQueueCount++;
	resendHaClearStaleControllerPending = true;
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
	if (published) {
		noteMqttActivityPulse();
	}
	bootEventPublished = bootEventPublishedAfterAttempt(bootEventPublished, published);
}

void
publishBootMemEventOncePerBoot(void)
{
	if (bootMemEventPublished || !_mqtt.connected() || !bootMemPublishReady()) {
		return;
	}

	char bootMemTopic[160];
	char payload[224];
	StatusBootMemSnapshot snapshot{};
	snapshot.fwBuildTsMs = BUILD_TS_MS;
	snapshot.tsMs = millis();
	snapshot.heapPreWifi = bootMemPublishState.heapPreWifi;
	snapshot.heapPostWifi = bootMemPublishState.heapPostWifi;
	snapshot.heapPostMqtt = bootMemPublishState.heapPostMqtt;
	snapshot.heapPreRs485 = bootMemPublishState.heapPreRs485;
	snapshot.heapPostRs485 = bootMemPublishState.heapPostRs485;

	if (!buildStatusBootMemJson(snapshot, payload, sizeof(payload))) {
		return;
	}

	snprintf(bootMemTopic, sizeof(bootMemTopic), "%s/boot/mem", deviceName);
	if (publishTrackedTextPayload(bootMemTopic, payload, MQTT_RETAIN)) {
		bootMemEventPublished = true;
	}
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

void
logHeapFreeOnly(const char *label)
{
	Serial.print("Heap ");
	Serial.print(label);
	Serial.print(": free=");
	Serial.print(ESP.getFreeHeap());
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

static const char *
runtimeDiagPhaseName(RuntimeDiagPhase phase)
{
	switch (phase) {
	case RuntimeDiagPhase::PollSnapshot:
		return "poll_snapshot";
	case RuntimeDiagPhase::BucketPublish:
		return "bucket_publish";
	case RuntimeDiagPhase::StatusPublish:
		return "status_publish";
	case RuntimeDiagPhase::DispatchWrite:
		return "dispatch_write";
	case RuntimeDiagPhase::DispatchReadback:
		return "dispatch_readback";
	case RuntimeDiagPhase::DispatchForcePublish:
		return "dispatch_force_publish";
	case RuntimeDiagPhase::None:
	default:
		return "";
	}
}

static bool
runtimeDiagSampleIsWorse(const MemSample &candidate, const MemSample &currentWorst)
{
	const MemLevel candidateLevel = evaluateRuntimeMem(candidate);
	const MemLevel currentLevel = evaluateRuntimeMem(currentWorst);
	if (candidateLevel != currentLevel) {
		return static_cast<uint8_t>(candidateLevel) > static_cast<uint8_t>(currentLevel);
	}
	if (candidate.freeB != currentWorst.freeB) {
		return candidate.freeB < currentWorst.freeB;
	}
	if (candidate.maxBlockB != currentWorst.maxBlockB) {
		return candidate.maxBlockB < currentWorst.maxBlockB;
	}
	return candidate.fragPct > currentWorst.fragPct;
}

static void
noteRuntimeMemObservation(const MemSample &sample)
{
	if (!runtimeDiag.minimaValid) {
		runtimeDiag.minimaValid = true;
		runtimeDiag.minFreeHeap = sample.freeB;
		runtimeDiag.minMaxBlock = sample.maxBlockB;
		runtimeDiag.maxFragPct = sample.fragPct;
		return;
	}
	if (sample.freeB < runtimeDiag.minFreeHeap) {
		runtimeDiag.minFreeHeap = sample.freeB;
	}
	if (sample.maxBlockB < runtimeDiag.minMaxBlock) {
		runtimeDiag.minMaxBlock = sample.maxBlockB;
	}
	if (sample.fragPct > runtimeDiag.maxFragPct) {
		runtimeDiag.maxFragPct = sample.fragPct;
	}
}

static void
noteRuntimePhaseObservation(RuntimeDiagPhase phase)
{
	if (phase == RuntimeDiagPhase::None) {
		return;
	}
	const MemSample sample = readMemSample();
	noteRuntimeMemObservation(sample);
	if (!runtimeDiag.worstValid || runtimeDiagSampleIsWorse(sample, runtimeDiag.worstSample)) {
		runtimeDiag.worstValid = true;
		runtimeDiag.worstPhase = phase;
		runtimeDiag.worstSample = sample;
	}
}

static void
noteTrackedMqttPayload(const char *kind, size_t payloadLen)
{
	if (kind == nullptr || kind[0] == '\0') {
		return;
	}
	if (payloadLen > runtimeDiag.mqttMaxPayloadSeen) {
		runtimeDiag.mqttMaxPayloadSeen = static_cast<uint32_t>(payloadLen);
		runtimeDiag.mqttMaxPayloadKind = kind;
	}
}

static uint8_t
bootMemPublishCheckpointBit(BootMemPublishCheckpoint checkpoint)
{
	return static_cast<uint8_t>(1u << static_cast<uint8_t>(checkpoint));
}

static void
recordBootMemPublishCheckpoint(BootMemPublishCheckpoint checkpoint)
{
	const MemSample sample = readMemSample();
	switch (checkpoint) {
		case BootMemPublishCheckpoint::PreWifi:
			bootMemPublishState.heapPreWifi = sample.freeB;
			break;
		case BootMemPublishCheckpoint::PostWifi:
			bootMemPublishState.heapPostWifi = sample.freeB;
			break;
		case BootMemPublishCheckpoint::PostMqtt:
			bootMemPublishState.heapPostMqtt = sample.freeB;
			break;
		case BootMemPublishCheckpoint::PreRs485:
			bootMemPublishState.heapPreRs485 = sample.freeB;
			break;
		case BootMemPublishCheckpoint::PostRs485:
			bootMemPublishState.heapPostRs485 = sample.freeB;
			break;
	}
	bootMemPublishState.validMask |= bootMemPublishCheckpointBit(checkpoint);
}

static bool
bootMemPublishReady(void)
{
	const uint8_t requiredMask = bootMemPublishCheckpointBit(BootMemPublishCheckpoint::PreWifi) |
	                             bootMemPublishCheckpointBit(BootMemPublishCheckpoint::PostWifi) |
	                             bootMemPublishCheckpointBit(BootMemPublishCheckpoint::PostMqtt) |
	                             bootMemPublishCheckpointBit(BootMemPublishCheckpoint::PreRs485) |
	                             bootMemPublishCheckpointBit(BootMemPublishCheckpoint::PostRs485);
	return (bootMemPublishState.validMask & requiredMask) == requiredMask;
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
	if (pendingPollingConfigSet || pendingRs485StubControlSet || pendingEntityCommandSet ||
	    pendingDispatchRequestSet) {
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
	// Serving the normal runtime control page from inside an RS485 transaction
	// can re-enter the ESP8266 web stack while SoftwareSerial is sitting in its
	// timing loop, which trips __yield assertions under repeated HTTP load. Keep
	// portal modes responsive here, but let normal runtime HTTP wait until the
	// main loop regains control.
	if (httpControlPlaneEnabled && currentBootMode != BootMode::Normal) {
		if (g_httpServer != nullptr) {
			httpServerRef().handleClient();
		}
	}
}

static bool
rs485TryReadIdentityOnce(void)
{
	if (_registerHandler == NULL) {
		return false;
	}

	modbusRequestAndResponseStatusValues result;
	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		return false;
	}

	*response = modbusRequestAndResponse{};
	result = _registerHandler->readHandledRegister(REG_SYSTEM_INFO_R_EMS_SN_BYTE_1_2, response);
	if (result != modbusRequestAndResponseStatusValues::readDataRegisterSuccess ||
	    response->dataValueFormatted[0] == '\0' ||
	    !inverterSerialIsValid(response->dataValueFormatted)) {
		return false;
	}
	if (!applyLiveInverterIdentity(response->dataValueFormatted)) {
		return false;
	}

	// Battery type is helpful for diagnostics, but it is not required to establish inverter identity.
	*response = modbusRequestAndResponse{};
	result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_TYPE, response);
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess &&
	    response->dataValueFormatted[0] != '\0') {
		strlcpy(deviceBatteryType, response->dataValueFormatted, sizeof(deviceBatteryType));
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

	resetRs485ProbeState(millis());
	rs485ProbeLastAttemptMs = 0;

	if (rs485StubModeUsesProbeLifecycle(mode)) {
		rs485RuntimeReconnectOnRediscoveryStart(rs485RuntimeReconnect);
		rs485ConnectState = Rs485ConnectState::ProbingBaud;
		essSnapshotValid = false;
		essSnapshotLastOk = false;
		rs485StubSkipNextScheduledStatusPublish = false;
		resendAllData = true;
		return;
	}

	// Online-like stub modes are intended to exercise snapshot and publish behavior directly.
	// Do not make them depend on the separate baud-probe state machine or issue Modbus reads
	// from the MQTT callback path. Also avoid forcing the full resend/bootstrap path here:
	// the immediate status refresh is enough for control acknowledgement, and the first normal
	// scheduler pass should avoid stacking a second full status burst onto the same transition.
	if (!inverterSerialKnown()) {
		primeStubRuntimeInverterIdentity("STUBSN000000000");
	}
	inverterReady = inverterSerialKnown();
	if (rs485ConnectState != Rs485ConnectState::Connected || !opData.essRs485Connected) {
		noteRs485ConnectedEpoch();
	}
	rs485ConnectState = Rs485ConnectState::Connected;
	rs485LockedBaud = DEFAULT_BAUD_RATE;
	opData.essRs485Connected = true;
	essSnapshotValid = false;
	essSnapshotLastOk = false;
	rs485StubSkipNextScheduledStatusPublish = true;
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
			seedMissingConfiguredRs485BaudAfterProbe();
			noteRs485ConnectedEpoch();

			// Now that inverter identity is known, discovery/config can be published under the real HA unique id.
			// If a deferred config/set payload is already queued, let loop() apply that first instead of
			// reusing the shared bucket-map scratch and clobbering the pending MQTT command.
			if (shouldReloadPollingConfigFromStorage(pendingPollingConfigSet, pollingConfigLoadedFromStorage)) {
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
	logHeapFreeOnly("before RS485 probe");
#endif
	recordBootMemStage(BootMemStage::Boot4);

	_modBus->setBaudRate(baud);

	modbusRequestAndResponseStatusValues result = modbusRequestAndResponseStatusValues::preProcessing;
	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		return;
	}
#ifdef DEBUG_NO_RS485
	result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
#else
	*response = modbusRequestAndResponse{};
	result = _registerHandler->readHandledRegister(REG_SAFETY_TEST_RW_GRID_REGULATION, response);
#endif

#ifdef DEBUG_OVER_SERIAL
	logHeapFreeOnly("after RS485 probe");
#endif

	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		rs485LockedBaud = baud;
		// Always re-read the live serial after a successful probe. Runtime identity is live-only,
		// so reconnects must detect an inverter replacement directly from hardware.
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
	snprintf(_debugOutput, sizeof(_debugOutput), "Baud Rate Checker Problem: %s", response->statusMqttMessage);
	Serial.println(_debugOutput);
	#endif
	recordRs485Error(result);
	updateOLED(false, "Test Baud", baudRateString, response->displayMessage);

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

enum class PortalUiMode : uint8_t {
	Normal,
	Wifi,
	Ap,
};

namespace {

#define A2M_UI_SHARED_STYLE \
	"<style>" \
	"body{margin:0 auto;padding:20px 16px 32px;max-width:460px;background:#eef3f6;color:#173042;font-family:Verdana,Geneva,sans-serif;line-height:1.45;}" \
	"h1,h2,h3,h4{margin:0 0 12px;color:var(--a2m-accent-dark);}p{margin:0 0 14px;}form{margin:0 0 12px;}a{color:var(--a2m-accent-dark);}" \
	"button,.btn,input[type='submit'],input[type='button']{width:100%;min-height:52px;padding:14px 16px;border:0;border-radius:14px;background:var(--a2m-accent);color:#fff;font-size:16px;font-weight:700;line-height:1.2;box-shadow:0 4px 0 var(--a2m-accent-dark);cursor:pointer;}" \
	"button:active,.btn:active,input[type='submit']:active,input[type='button']:active{transform:translateY(2px);box-shadow:0 2px 0 var(--a2m-accent-dark);}" \
	"button:focus-visible,.btn:focus-visible,input[type='submit']:focus-visible,input[type='button']:focus-visible{outline:2px solid rgba(23,48,66,.25);outline-offset:2px;}" \
	"strong{color:var(--a2m-accent-dark);}" \
	"</style>"

#define A2M_PORTAL_AUTOMATION_SCRIPT \
	"<script>" \
	"(function(){" \
	"var p=(window.location&&window.location.pathname)||'';" \
	"if(p==='/wifisave'){window.location.href='/status';return;}" \
	"if(p==='/restart'||p==='/restart/'){function probe(){fetch('/',{cache:'no-store'}).then(function(r){if(r&&r.ok){window.location.href='/';return;}setTimeout(probe,1000);}).catch(function(){setTimeout(probe,1000);});}setTimeout(probe,300);}" \
	"if(p==='/0wifi'){window.addEventListener('DOMContentLoaded',function(){var nodes=document.querySelectorAll(\"form[action^='/wifi?refresh=1']\");for(var i=0;i<nodes.length;i++){nodes[i].remove();}});}" \
	"})();" \
	"</script>"

static const char kUiPageHeadOpen[] PROGMEM =
	"<!DOCTYPE html><html><head>"
	"<meta charset=\"utf-8\">"
	"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
	"<meta name=\"a2m-ui\" content=\"buttons-v1\">"
	"<meta name=\"a2m-mode\" content=\"";
static const char kUiPageHeadAccentOpen[] PROGMEM = "\"><meta name=\"a2m-accent\" content=\"";
static const char kUiPageHeadVarsOpen[] PROGMEM =
	"\"><style>:root{--a2m-accent:";
static const char kUiPageHeadVarsMid[] PROGMEM =
	";--a2m-accent-dark:";
static const char kUiPageHeadVarsClose[] PROGMEM =
	";}</style>";
static const char kUiSharedStyle[] PROGMEM = A2M_UI_SHARED_STYLE;
static const char kUiPageTitleOpen[] PROGMEM = "<title>";
static const char kUiPageTitleClose[] PROGMEM = "</title>";
static const char kUiPageBodyOpen[] PROGMEM = "<body data-a2m-mode=\"";
static const char kUiPageHeadingOpen[] PROGMEM = "\"><h2>";
static const char kUiPageHeadingClose[] PROGMEM = "</h2>";
static const char kUiPageTail[] PROGMEM = "</body></html>";
static const char kPortalCustomHeadAp[] PROGMEM =
	"<meta name=\"a2m-ui\" content=\"buttons-v1\">"
	"<meta name=\"a2m-mode\" content=\"ap\">"
	"<meta name=\"a2m-accent\" content=\"#1c6bcf\">"
	"<style>:root{--a2m-accent:#1c6bcf;--a2m-accent-dark:#15539f;}</style>"
	A2M_UI_SHARED_STYLE
	A2M_PORTAL_AUTOMATION_SCRIPT;
static const char kPortalCustomHeadWifi[] PROGMEM =
	"<meta name=\"a2m-ui\" content=\"buttons-v1\">"
	"<meta name=\"a2m-mode\" content=\"wifi\">"
	"<meta name=\"a2m-accent\" content=\"#c57a00\">"
	"<style>:root{--a2m-accent:#c57a00;--a2m-accent-dark:#8d5700;}</style>"
	A2M_UI_SHARED_STYLE
	A2M_PORTAL_AUTOMATION_SCRIPT;

#undef A2M_UI_SHARED_STYLE
#undef A2M_PORTAL_AUTOMATION_SCRIPT

static const char *
portalUiModeToken(PortalUiMode mode)
{
	switch (mode) {
	case PortalUiMode::Ap:
		return "ap";
	case PortalUiMode::Wifi:
		return "wifi";
	case PortalUiMode::Normal:
	default:
		return "normal";
	}
}

static const char *
portalUiAccentHex(PortalUiMode mode)
{
	switch (mode) {
	case PortalUiMode::Ap:
		return "#1c6bcf";
	case PortalUiMode::Wifi:
		return "#c57a00";
	case PortalUiMode::Normal:
	default:
		return "#1d8c4b";
	}
}

static const char *
portalUiAccentDarkHex(PortalUiMode mode)
{
	switch (mode) {
	case PortalUiMode::Ap:
		return "#15539f";
	case PortalUiMode::Wifi:
		return "#8d5700";
	case PortalUiMode::Normal:
	default:
		return "#146238";
	}
}

} // namespace

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

	if (!ensureHttpServer()) {
		return;
	}
	httpServerRef().on("/", HTTP_GET, handleHttpRoot);
	httpServerRef().on("/config/polling/reset", HTTP_POST, handleHttpPollingReset);
	httpServerRef().on("/restart", HTTP_GET, handleHttpRestartAlias);
	httpServerRef().on("/restart/", HTTP_GET, handleHttpRestartAlias);
	httpServerRef().on("/reboot/normal", HTTP_POST, handleRebootNormal);
	httpServerRef().on("/reboot/ap", HTTP_GET, handleRebootApConfirm);
	httpServerRef().on("/reboot/ap", HTTP_POST, handleRebootAp);
	httpServerRef().on("/reboot/wifi", HTTP_POST, handleRebootWifi);
	httpServerRef().begin();
	httpControlPlaneEnabled = true;
#ifdef DEBUG_OVER_SERIAL
	Serial.println("HTTP control plane started on port 80.");
#endif
}

void
handleHttpPollingReset(void)
{
#ifdef DEBUG_OVER_SERIAL
	Serial.println("HTTP POST /config/polling/reset");
#endif
	if (!resetPollingConfigToDefaults()) {
		httpServerRef().sendHeader("Cache-Control", "no-store");
		httpServerRef().sendHeader("Location", "/?polling_reset=err", true);
		httpServerRef().send(303, "text/plain", "");
		return;
	}
	httpServerRef().sendHeader("Cache-Control", "no-store");
	httpServerRef().sendHeader("Location", "/?polling_reset=1", true);
	httpServerRef().send(303, "text/plain", "");
}

void
handleHttpRestartAlias(void)
{
#ifdef DEBUG_OVER_SERIAL
	Serial.println("HTTP GET /restart -> /");
#endif
	httpServerRef().sendHeader("Cache-Control", "no-store");
	httpServerRef().sendHeader("Location", "/", true);
	httpServerRef().send(302, "text/plain", "");
}

void
handleRebootNormal(void)
{
#ifdef DEBUG_OVER_SERIAL
	Serial.println("HTTP POST /reboot/normal");
	Serial.println("Scheduling deferred reboot -> normal");
#endif
	(void)sendRebootHandoffPage(&httpServerRef(), BootIntent::Normal);
	scheduleDeferredControlPlaneReboot(BootIntent::Normal);
}

void
handleRebootApConfirm(void)
{
#ifdef DEBUG_OVER_SERIAL
	Serial.println("HTTP GET /reboot/ap");
#endif
	char html[640];
	if (!buildPortalRebootToApConfirmHtml(html, sizeof(html))) {
		httpServerRef().send(500, "text/plain", "Failed to build AP reboot confirmation page");
		return;
	}
	httpServerRef().send(200, "text/html", html);
}

void
handleRebootAp(void)
{
#ifdef DEBUG_OVER_SERIAL
	Serial.println("HTTP POST /reboot/ap");
	Serial.println("Scheduling deferred reboot -> ap_config");
#endif
	(void)sendRebootHandoffPage(&httpServerRef(), BootIntent::ApConfig);
	scheduleDeferredControlPlaneReboot(BootIntent::ApConfig);
}

void
handleRebootWifi(void)
{
#ifdef DEBUG_OVER_SERIAL
	Serial.println("HTTP POST /reboot/wifi");
	Serial.println("Scheduling deferred reboot -> wifi_config");
#endif
	(void)sendRebootHandoffPage(&httpServerRef(), BootIntent::WifiConfig);
	scheduleDeferredControlPlaneReboot(BootIntent::WifiConfig, 1500);
}

static bool __attribute__((noinline))
subscribeInverterCommandTopicByIndex(size_t idx, bool *eligibleOut)
{
	if (!ensureRuntimeScratch()) {
		return false;
	}
	RuntimeScratch &scratch = *g_runtimeScratch;
	if (eligibleOut != nullptr) {
		*eligibleOut = false;
	}
	if (!mqttEntityCopyByIndex(idx, &scratch.inverterSubscriptionEntity) ||
	    !scratch.inverterSubscriptionEntity.subscribe ||
	    mqttEntityScope(scratch.inverterSubscriptionEntity.entityId) != DiscoveryDeviceScope::Inverter) {
		return false;
	}
	if (eligibleOut != nullptr) {
		*eligibleOut = true;
	}

	mqttEntityNameCopy(&scratch.inverterSubscriptionEntity,
	                   scratch.inverterSubscriptionEntityKey,
	                   sizeof(scratch.inverterSubscriptionEntityKey));
	const char *inverterDeviceId = discoveryDeviceIdForScope(DiscoveryDeviceScope::Inverter);
	if (deviceName[0] == '\0' ||
	    inverterDeviceId == nullptr ||
	    inverterDeviceId[0] == '\0' ||
	    scratch.inverterSubscriptionEntityKey[0] == '\0') {
		scratch.inverterSubscription[0] = '\0';
		return false;
	}

	const int written = snprintf(scratch.inverterSubscription,
	                             sizeof(scratch.inverterSubscription),
	                             "%s/%s/%s/command",
	                             deviceName,
	                             inverterDeviceId,
	                             scratch.inverterSubscriptionEntityKey);
	if (written <= 0 || static_cast<size_t>(written) >= sizeof(scratch.inverterSubscription)) {
		scratch.inverterSubscription[0] = '\0';
		return false;
	}
	return _mqtt.subscribe(scratch.inverterSubscription, MQTT_SUBSCRIBE_QOS);
}

static bool __attribute__((noinline))
subscribeInverterDispatchTopic(void)
{
	if (!ensureRuntimeScratch()) {
		return false;
	}
	RuntimeScratch &scratch = *g_runtimeScratch;
	const char *inverterDeviceId = discoveryDeviceIdForScope(DiscoveryDeviceScope::Inverter);
	if (inverterDeviceId == nullptr || inverterDeviceId[0] == '\0') {
		scratch.inverterSubscription[0] = '\0';
		return false;
	}
	const int written = snprintf(scratch.inverterSubscription,
	                             sizeof(scratch.inverterSubscription),
	                             "%s/dispatch/set",
	                             inverterDeviceId);
	if (written <= 0 || static_cast<size_t>(written) >= sizeof(scratch.inverterSubscription)) {
		scratch.inverterSubscription[0] = '\0';
		return false;
	}
	return _mqtt.subscribe(scratch.inverterSubscription, MQTT_SUBSCRIBE_QOS);
}

bool __attribute__((noinline))
subscribeInverterTopics(void)
{
	static uint32_t lastSubscribeAttemptMs = 0;

	if (!inverterReady || !inverterSerialKnown() || !_mqtt.connected()) {
		return false;
	}
	if (!mqttEntitiesRtAvailable()) {
		return false;
	}
	if (inverterSubscriptionsSet) {
		return false;
	}
	const uint32_t nowMs = millis();
	if ((nowMs - lastSubscribeAttemptMs) < 250U) {
		return false;
	}
	lastSubscribeAttemptMs = nowMs;

	// Stage inverter command-topic subscriptions across later loops. The wildcard
	// subscribe path started tripping ESP8266 stack-smash resets during stub-online
	// transitions; explicit command topics preserve the MQTT surface without that
	// broker/client wildcard path.
	if (!inverterCommandSubscriptionsSet) {
		const size_t entityCount = mqttEntitiesCount();
		for (size_t idx = inverterNextCommandSubscriptionIndex; idx < entityCount; ++idx) {
			bool eligible = false;
			const bool subscribed = subscribeInverterCommandTopicByIndex(idx, &eligible);
			if (!eligible) {
				continue;
			}
			if (subscribed) {
				inverterNextCommandSubscriptionIndex = idx + 1;
				if (inverterNextCommandSubscriptionIndex >= entityCount) {
					inverterCommandSubscriptionsSet = true;
				}
			}
			return subscribed;
		}
		inverterCommandSubscriptionsSet = true;
	}
	if (!inverterDispatchSubscriptionSet) {
		// The atomic dispatch request is intentionally inverter-rooted so a
		// controller replacement does not change the command path.
		const bool subscribed = subscribeInverterDispatchTopic();
			if (subscribed) {
				inverterDispatchSubscriptionSet = true;
				inverterSubscriptionsSet = true;
		}
		return subscribed;
	}
	inverterSubscriptionsSet = true;
	return false;
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

static bool
publishStatusCoreNow(void)
{
	if (!_mqtt.connected() || !ensureStatusJsonScratch()) {
		return false;
	}

	StatusCoreSnapshot core{};
	core.presence = "online";
	core.a2mStatus = "online";
	core.rs485Status = essSnapshotValid ? (opData.essRs485Connected ? "OK" : "Problem") : "unknown";
	core.gridStatus = "unknown";
	core.bootMode = bootModeToString(currentBootMode);
	core.bootIntent = bootIntentToString(currentBootIntent);
	core.httpControlPlaneEnabled = httpControlPlaneEnabled;
	core.haUniqueId = inverterReady ? haUniqueId : "A2M-UNKNOWN";

	if (!buildStatusCoreJson(core, g_statusJsonScratch, kStatusJsonScratchSize)) {
		return false;
	}

	RuntimeDiagScope diagScope(RuntimeDiagPhase::StatusPublish, "core");
	if (!publishTrackedTextPayload(statusTopic, g_statusJsonScratch, MQTT_RETAIN)) {
		return false;
	}
	maybeYield();
	publishControllerInverterSerialState();
	maybeYield();
	return true;
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
	if (_mqtt.publish(topic, payload, false)) {
		noteMqttActivityPulse();
	}
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

enum class Rs485ErrorClass : uint8_t {
	None = 0,
	Transport,
	Other,
};

// Keep the legacy total counter intact while separating bus transport failures
// from higher-level Modbus/inverter read problems for diagnostics and HA.
static Rs485ErrorClass
classifyRs485Error(modbusRequestAndResponseStatusValues result)
{
	switch (result) {
	case modbusRequestAndResponseStatusValues::invalidFrame:
	case modbusRequestAndResponseStatusValues::responseTooShort:
	case modbusRequestAndResponseStatusValues::noResponse:
		return Rs485ErrorClass::Transport;
	case modbusRequestAndResponseStatusValues::slaveError:
		return Rs485ErrorClass::Other;
	case modbusRequestAndResponseStatusValues::preProcessing:
	case modbusRequestAndResponseStatusValues::notHandledRegister:
	case modbusRequestAndResponseStatusValues::noMQTTPayload:
	case modbusRequestAndResponseStatusValues::invalidMQTTPayload:
	case modbusRequestAndResponseStatusValues::writeSingleRegisterSuccess:
	case modbusRequestAndResponseStatusValues::writeDataRegisterSuccess:
	case modbusRequestAndResponseStatusValues::readDataRegisterSuccess:
	case modbusRequestAndResponseStatusValues::setDischargeSuccess:
	case modbusRequestAndResponseStatusValues::setChargeSuccess:
	case modbusRequestAndResponseStatusValues::setNormalSuccess:
	case modbusRequestAndResponseStatusValues::payloadExceededCapacity:
	case modbusRequestAndResponseStatusValues::addedToPayload:
	case modbusRequestAndResponseStatusValues::readDataInvalidValue:
		return Rs485ErrorClass::None;
	default:
		return Rs485ErrorClass::Other;
	}
}

static void
recordRs485Error(modbusRequestAndResponseStatusValues result, uint32_t count)
{
	if (count == 0) {
		return;
	}
	switch (classifyRs485Error(result)) {
	case Rs485ErrorClass::Transport:
		rs485Errors += count;
		rs485TransportErrors += count;
		break;
	case Rs485ErrorClass::Other:
		rs485Errors += count;
		rs485OtherErrors += count;
		break;
	case Rs485ErrorClass::None:
	default:
		break;
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
noteRs485BaudSyncFailure(const char *detail)
{
	lastErrCode = static_cast<int>(MqttEventCode::ModbusFrame);
	publishEvent(MqttEventCode::ModbusFrame, detail != nullptr ? detail : "rs485_baud_sync_failed");
}

static bool
persistConfiguredRs485Baud(uint32_t baud)
{
	if (!rs485BaudValueSupported(baud)) {
		return false;
	}
	Preferences preferences;
	preferences.begin(DEVICE_NAME, false);
	const bool ok = preferences.putUInt(kPreferenceRs485Baud, baud) == sizeof(uint32_t);
	preferences.end();
	return ok;
}

static bool
persist_defaults_if_missing_rs485_baud(uint32_t baud)
{
	if (!rs485BaudValueSupported(baud)) {
		return false;
	}
	Preferences preferences;
	preferences.begin(DEVICE_NAME, false);
	const bool hasKey = preferences.isKey(kPreferenceRs485Baud);
	const uint32_t storedBaud = preferences.getUInt(kPreferenceRs485Baud, 0);
	const bool ok = rs485BaudStoredValueUsable(hasKey, storedBaud) ||
	                preferences.putUInt(kPreferenceRs485Baud, baud) == sizeof(uint32_t);
	preferences.end();
	return ok;
}

static void
seedMissingConfiguredRs485BaudAfterProbe(void)
{
	if (rs485BaudTracker.hasConfiguredBaud || _registerHandler == nullptr || _modBus == nullptr) {
		return;
	}
	uint32_t liveBaud = 0;
	modbusRequestAndResponseStatusValues result = modbusRequestAndResponseStatusValues::preProcessing;
	const char *detail = "";
	if (!readLiveRs485Baud(liveBaud, &result, &detail)) {
		if (result != modbusRequestAndResponseStatusValues::preProcessing) {
			recordRs485Error(result);
			noteRs485Error(result, detail);
		}
		return;
	}
	if (persist_defaults_if_missing_rs485_baud(liveBaud)) {
		rs485BaudTrackerMarkSeeded(rs485BaudTracker, liveBaud);
	}
}

static bool
loadConfiguredRs485Baud(uint32_t &baudOut, bool &hasConfiguredOut)
{
	Preferences preferences;
	preferences.begin(DEVICE_NAME, true);
	const bool hasKey = preferences.isKey(kPreferenceRs485Baud);
	const uint32_t storedBaud = preferences.getUInt(kPreferenceRs485Baud, 0);
	preferences.end();
	if (!rs485BaudStoredValueUsable(hasKey, storedBaud)) {
		baudOut = 0;
		hasConfiguredOut = false;
		return false;
	}
	baudOut = storedBaud;
	hasConfiguredOut = true;
	return true;
}

static bool
readLiveRs485Baud(uint32_t &baudOut, modbusRequestAndResponseStatusValues *resultOut, const char **detailOut)
{
	baudOut = 0;
	if (resultOut != nullptr) {
		*resultOut = modbusRequestAndResponseStatusValues::preProcessing;
	}
	if (detailOut != nullptr) {
		*detailOut = "";
	}
	if (_registerHandler == nullptr) {
		return false;
	}
	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		return false;
	}
	*response = modbusRequestAndResponse{};
	const modbusRequestAndResponseStatusValues result =
		_registerHandler->readHandledRegister(REG_SYSTEM_CONFIG_RW_MODBUS_BAUD_RATE, response);
	if (resultOut != nullptr) {
		*resultOut = result;
	}
	if (detailOut != nullptr) {
		*detailOut = response->statusMqttMessage;
	}
	if (result != modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		return false;
	}
	return rs485BaudRegisterToValue(response->unsignedShortValue, baudOut);
}

static bool
writeConfiguredRs485Baud(uint32_t targetBaud, modbusRequestAndResponseStatusValues *resultOut, const char **detailOut)
{
	if (resultOut != nullptr) {
		*resultOut = modbusRequestAndResponseStatusValues::preProcessing;
	}
	if (detailOut != nullptr) {
		*detailOut = "";
	}
	if (_registerHandler == nullptr) {
		return false;
	}
	uint16_t regValue = 0;
	if (!rs485BaudValueToRegister(targetBaud, regValue)) {
		return false;
	}
	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		return false;
	}
	*response = modbusRequestAndResponse{};
	response->registerCount = 1;
	const modbusRequestAndResponseStatusValues result =
		_registerHandler->writeRawDataRegister(REG_SYSTEM_CONFIG_RW_MODBUS_BAUD_RATE, regValue, response);
	if (resultOut != nullptr) {
		*resultOut = result;
	}
	if (detailOut != nullptr) {
		*detailOut = response->statusMqttMessage;
	}
	return result == modbusRequestAndResponseStatusValues::writeDataRegisterSuccess;
}

static void
serviceRs485BaudReconcile(void)
{
	if (_registerHandler == nullptr || rs485ConnectState != Rs485ConnectState::Connected || !inverterReady ||
	    !opData.essRs485Connected) {
		return;
	}
	const bool shouldObserveBaud = !rs485BaudTracker.hasConfiguredBaud || rs485BaudTracker.pendingConfirmation ||
	                               rs485BaudTracker.actualBaud == 0 ||
	                               rs485BaudTrackerNeedsWriteAttempt(rs485BaudTracker,
	                                                                rs485RuntimeReconnect.connectionEpoch);
	if (!shouldObserveBaud) {
		return;
	}
	const uint32_t now = millis();
	if (static_cast<int32_t>(now - rs485BaudNextActionAtMs) < 0) {
		return;
	}

	uint32_t liveBaud = 0;
	modbusRequestAndResponseStatusValues result = modbusRequestAndResponseStatusValues::preProcessing;
	const char *detail = "";
	if (!readLiveRs485Baud(liveBaud, &result, &detail)) {
		if (result != modbusRequestAndResponseStatusValues::preProcessing) {
			recordRs485Error(result);
			noteRs485Error(result, detail);
		}
		rs485BaudNextActionAtMs = now + kRs485BaudRetryMs;
		return;
	}

	rs485BaudTrackerMarkObserved(rs485BaudTracker, liveBaud);
	rs485BaudNextActionAtMs = now + kRs485BaudRetryMs;

	if (!rs485BaudTracker.hasConfiguredBaud) {
		if (!rs485BaudTrackerNeedsSeedAttempt(rs485BaudTracker, rs485RuntimeReconnect.connectionEpoch)) {
			return;
		}
		rs485BaudTrackerMarkSeedAttempt(rs485BaudTracker, rs485RuntimeReconnect.connectionEpoch);
		if (persist_defaults_if_missing_rs485_baud(liveBaud)) {
			rs485BaudTrackerMarkSeeded(rs485BaudTracker, liveBaud);
		}
		return;
	}

	if (rs485BaudTracker.pendingConfirmation) {
		const bool synced = liveBaud == rs485BaudTracker.configuredBaud;
		rs485BaudTrackerMarkConfirmationResult(rs485BaudTracker, rs485RuntimeReconnect.connectionEpoch, synced);
		if (!synced) {
			noteRs485BaudSyncFailure("rs485_baud_reconcile_failed");
		}
		return;
	}

	if (!rs485BaudTrackerNeedsWriteAttempt(rs485BaudTracker, rs485RuntimeReconnect.connectionEpoch)) {
		return;
	}

	if (!writeConfiguredRs485Baud(rs485BaudTracker.configuredBaud, &result, &detail)) {
		if (result != modbusRequestAndResponseStatusValues::preProcessing) {
			recordRs485Error(result);
			noteRs485Error(result, detail);
		}
		rs485BaudTracker.syncState = Rs485BaudSyncState::Failed;
		rs485BaudTracker.lastWriteAttemptEpoch = rs485RuntimeReconnect.connectionEpoch;
		rs485BaudNextActionAtMs = now + kRs485BaudRetryMs;
		return;
	}

	rs485BaudTrackerMarkWriteAttempt(rs485BaudTracker, rs485RuntimeReconnect.connectionEpoch);
	beginRs485RuntimeRediscovery("baud_reconcile");
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
clearUserWifiCredentials(void)
{
	Preferences preferences;
	if (!preferences.begin(DEVICE_NAME, false)) {
		return;
	}
	if (preferences.isKey("WiFi_SSID")) {
		(void)preferences.remove("WiFi_SSID");
	}
	if (preferences.isKey("WiFi_Password")) {
		(void)preferences.remove("WiFi_Password");
	}
	preferences.end();
}

static void
clearSdkWifiCredentials(void)
{
	// The firmware owns WiFi credentials in Preferences. Clear the SDK-side copy too
	// so portal erase semantics and later reconnects cannot drift onto stale station
	// config hidden inside the WiFi stack.
	WiFi.disconnect(true);
	diagDelay(100);
}

static void
beginWifiStationWithStoredCredentials(void)
{
	// Always connect using the firmware-owned credentials rather than any SDK-
	// persisted station config.
	WiFi.persistent(false);
	clearSdkWifiCredentials();
	WiFi.mode(WIFI_STA);
#if defined MP_ESP32
	WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
	WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
#endif // MP_ESP32
	WiFi.hostname(deviceName);
	WiFi.begin(appConfig.wifiSSID.c_str(), appConfig.wifiPass.c_str());
}

static bool
syncPortalWifiCredentials(WiFiManager *wifiManager, const char *ssidHint, const char *passHint)
{
	// AP onboarding can hand us empty SSID/password hints even though WiFiManager still
	// holds the submitted values it just used for a successful connect. Treat both empty
	// hints as "unknown" so we can fall back to WiFiManager state, but preserve an empty
	// password when the caller supplied an SSID because that is how open networks are
	// represented through the portal form.
	const bool ssidProvided = ssidHint != nullptr && ssidHint[0] != '\0';
	const bool passProvided = passHint != nullptr && (passHint[0] != '\0' || ssidProvided);
	String ssid = ssidProvided ? String(ssidHint) : String();
	String pass = passProvided ? String(passHint) : String();

	if (wifiManager != nullptr) {
		if (!ssidProvided && ssid.length() == 0) {
			const String currentSsid = wifiManager->getWiFiSSID();
			if (currentSsid.length() > 0) {
				ssid = currentSsid;
			}
		}
		if (!passProvided && pass.length() == 0) {
			const String currentPass = wifiManager->getWiFiPass();
			if (currentPass.length() > 0) {
				pass = currentPass;
			}
		}
	}

	if (ssid.length() == 0 && WiFi.status() == WL_CONNECTED) {
		ssid = WiFi.SSID();
	}
	if (!passProvided && pass.length() == 0 && appConfig.wifiPass.length() > 0) {
		pass = appConfig.wifiPass;
	}
	if (ssid.length() == 0) {
		return false;
	}

	const bool changed = appConfig.wifiSSID != ssid || appConfig.wifiPass != pass;
	appConfig.wifiSSID = ssid;
	appConfig.wifiPass = pass;
	if (changed) {
		persistUserWifiCredentials(ssid.c_str(), pass.c_str());
	}
	return changed;
}

static void
refreshPortalUpdateCsrfToken(void)
{
	const IPAddress staIp = WiFi.localIP();
	const IPAddress apIp = WiFi.softAPIP();
	const uint32_t ipMix = (static_cast<uint32_t>(staIp[0]) << 24) | (static_cast<uint32_t>(staIp[1]) << 16) |
	                       (static_cast<uint32_t>(staIp[2]) << 8) | static_cast<uint32_t>(staIp[3]);
	const uint32_t apMix = (static_cast<uint32_t>(apIp[0]) << 24) | (static_cast<uint32_t>(apIp[1]) << 16) |
	                       (static_cast<uint32_t>(apIp[2]) << 8) | static_cast<uint32_t>(apIp[3]);
	const uint32_t a = static_cast<uint32_t>(micros());
	const uint32_t b = static_cast<uint32_t>(millis()) ^ static_cast<uint32_t>(ESP.getFreeHeap());
#if defined(MP_ESP8266)
	const uint32_t c = ESP.getCycleCount();
#else
	const uint32_t c = static_cast<uint32_t>(ESP.getEfuseMac());
#endif
	const uint32_t d = portalConnectStart ^ ipMix ^ apMix ^ static_cast<uint32_t>(wifiPower);
	snprintf(portalUpdateCsrfToken,
	         sizeof(portalUpdateCsrfToken),
	         "%08lx%08lx%08lx%08lx",
	         static_cast<unsigned long>(a),
	         static_cast<unsigned long>(b),
	         static_cast<unsigned long>(c),
	         static_cast<unsigned long>(d));
}

static bool
portalUpdateRequestHasValidToken(WiFiManager *wifiManager)
{
	if (wifiManager == nullptr || !wifiManager->server || portalUpdateCsrfToken[0] == '\0') {
		return false;
	}
	if (!wifiManager->server->hasArg("csrf")) {
		return false;
	}
	return wifiManager->server->arg("csrf") == portalUpdateCsrfToken;
}

static void
capturePortalActiveStaConnection(void)
{
	if (WiFi.status() != WL_CONNECTED) {
		portalStatus = portalStatusIdle;
		portalStatusSsid[0] = '\0';
		portalStatusIp[0] = '\0';
		return;
	}

	const String currentSsid = WiFi.SSID();
	const String currentIp = WiFi.localIP().toString();
	portalStatus = portalStatusSuccess;
	strlcpy(portalStatusSsid, currentSsid.c_str(), sizeof(portalStatusSsid));
	strlcpy(portalStatusIp, currentIp.c_str(), sizeof(portalStatusIp));
}

static void
persistUserExtAntenna(bool enabled)
{
	Preferences preferences;
	preferences.begin(DEVICE_NAME, false);
	preferences.putBool("Ext_Antenna", enabled);
	preferences.end();
}

struct ScopedCharBuffer {
	char *data = nullptr;
	size_t size = 0;

	explicit ScopedCharBuffer(size_t bufferSize)
	{
		(void)reset(bufferSize);
	}

	~ScopedCharBuffer()
	{
		delete[] data;
	}

	bool ok() const
	{
		return data != nullptr;
	}

	bool reset(size_t bufferSize)
	{
		delete[] data;
		data = nullptr;
		size = 0;
		if (bufferSize == 0) {
			return true;
		}
		data = new (std::nothrow) char[bufferSize];
		if (data == nullptr) {
			return false;
		}
		size = bufferSize;
		data[0] = '\0';
		return true;
	}
};

static size_t
preferenceStringBufferLen(Preferences &preferences, const char *key, size_t maxLen)
{
	// Bucket_Map is stored with putString(). ESP32 reports blob sizes via getBytesLength(),
	// so use the string-specific length probe there and keep the legacy path for ESP8266.
#if defined(MP_ESP32)
	const size_t storedLen = preferences.getStringLength(key);
#else
	const size_t storedLen = preferences.getBytesLength(key);
#endif
	return (storedLen < maxLen) ? (storedLen + 1) : maxLen;
}

static void
persistUserInverterLabel(const char *label)
{
	Preferences preferences;
	if (!preferences.begin(DEVICE_NAME, false)) {
		return;
	}
	const char *safeLabel = (label != nullptr) ? label : "";
	if (safeLabel[0] == '\0') {
		if (preferences.isKey(kPreferenceInverterLabel)) {
			(void)preferences.remove(kPreferenceInverterLabel);
		}
		preferences.end();
		return;
	}
	char bounded[kPrefInverterLabelMaxLen];
	strlcpy(bounded, safeLabel, sizeof(bounded));
	preferences.putString(kPreferenceInverterLabel, bounded);
	preferences.end();
}

static bool
persistUserBucketMap(const char *bucketMap)
{
	Preferences preferences;
	if (!preferences.begin(DEVICE_NAME, false)) {
		return false;
	}

	const char *safeBucketMap = (bucketMap != nullptr) ? bucketMap : "";
	const size_t bucketMapLen = strlen(safeBucketMap);
	bool ok = false;
	if (bucketMapLen == 0) {
		ok = !preferences.isKey(kPreferenceBucketMap) || preferences.remove(kPreferenceBucketMap) ||
		     !preferences.isKey(kPreferenceBucketMap);
	} else {
		ok = preferences.putString(kPreferenceBucketMap, safeBucketMap) == bucketMapLen;
	}
	if (ok) {
		ok = preferences.putBool(kPreferenceBucketMapMigrated, true) == sizeof(uint8_t);
	}
	preferences.end();
	return ok;
}

static bool
persistUserPollingConfig(uint32_t intervalSeconds, const char *bucketMap)
{
	Preferences preferences;
	if (!preferences.begin(DEVICE_NAME, false)) {
		return false;
	}

	const uint32_t originalIntervalSeconds =
		preferences.getUInt(kPreferencePollInterval, kPollIntervalDefaultSeconds);
	const bool updateBucketMap = (bucketMap != nullptr);
	const bool originalBucketMapPresent = updateBucketMap && preferences.isKey(kPreferenceBucketMap);
	const size_t originalBucketMapBufferLen =
		originalBucketMapPresent ? preferenceStringBufferLen(preferences, kPreferenceBucketMap, kPrefBucketMapMaxLen) : 1;
	ScopedCharBuffer originalBucketMap(originalBucketMapPresent ? originalBucketMapBufferLen : 1);
	if (originalBucketMapPresent) {
		if (!originalBucketMap.ok()) {
			preferences.end();
			return false;
		}
		originalBucketMap.data[0] = '\0';
		preferences.getString(kPreferenceBucketMap, originalBucketMap.data, originalBucketMapBufferLen);
	}
	const bool originalBucketMapMigrated = updateBucketMap ? preferences.getBool(kPreferenceBucketMapMigrated, false) : false;
	bool ok = preferences.putUInt(kPreferencePollInterval, intervalSeconds) == sizeof(uint32_t);
	if (updateBucketMap) {
		const char *safeBucketMap = bucketMap;
		const size_t bucketMapLen = strlen(safeBucketMap);
		if (ok) {
			if (bucketMapLen == 0) {
				ok = !preferences.isKey(kPreferenceBucketMap) || preferences.remove(kPreferenceBucketMap) ||
				     !preferences.isKey(kPreferenceBucketMap);
			} else {
				ok = preferences.putString(kPreferenceBucketMap, safeBucketMap) == bucketMapLen;
			}
		}
		if (ok) {
			ok = preferences.putBool(kPreferenceBucketMapMigrated, true) == sizeof(uint8_t);
		}
	}
	if (!ok) {
		preferences.putUInt(kPreferencePollInterval, originalIntervalSeconds);
		if (updateBucketMap) {
			if (originalBucketMapPresent) {
				preferences.putString(kPreferenceBucketMap, originalBucketMap.data);
			} else if (preferences.isKey(kPreferenceBucketMap)) {
				preferences.remove(kPreferenceBucketMap);
			}
			preferences.putBool(kPreferenceBucketMapMigrated, originalBucketMapMigrated);
		}
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

static bool
resetBucketsToCatalogDefaults(BucketId *buckets, size_t entityCount)
{
	if (buckets == nullptr || entityCount == 0 || entityCount > kMqttEntityDescriptorCount) {
		return false;
	}
	for (size_t idx = 0; idx < entityCount; ++idx) {
		mqttState entity{};
		if (!mqttEntityCopyByIndex(idx, &entity)) {
			return false;
		}
		buckets[idx] = bucketIdFromFreq(entity.updateFreq);
	}
	return true;
}

static bool
recoverPollingConfigLoadToDefaults(const char *context,
                                   BucketId *buckets,
                                   size_t entityCount,
                                   uint32_t &outIntervalSeconds,
                                   PollingLoadFailureKind failureKind = PollingLoadFailureKind::Transient)
{
	outIntervalSeconds = kPollIntervalDefaultSeconds;
	if (!resetBucketsToCatalogDefaults(buckets, entityCount)) {
		return false;
	}
	const bool resetPersisted = shouldResetPersistedPollingConfig(failureKind);
	const bool persistedResetOk =
		!resetPersisted || persistUserPollingConfig(kPollIntervalDefaultSeconds, "");
#ifdef DEBUG_OVER_SERIAL
	Serial.printf("%s: recovered polling prefs to defaults reset_persisted=%u persisted=%u\r\n",
	              (context != nullptr) ? context : "polling load",
	              resetPersisted ? 1U : 0U,
	              persistedResetOk ? 1U : 0U);
#endif
	return shouldAcceptRecoveredPollingConfig(failureKind, persistedResetOk);
}

static bool
resetPollingConfigToDefaults(void)
{
	// Runtime reset contract:
	// - unlike portal save/import, this path runs against the live runtime
	// - defaults must apply immediately and trigger HA rediscovery/state resend
	if (!mqttEntitiesRtAvailable()) {
		return false;
	}
	if (!ensurePortalBucketsScratch()) {
		return false;
	}

	// A direct reset should win over any earlier deferred config/set payload.
	pendingPollingConfigSet = false;
	clearPendingPollingConfigPayload();

	if (!persistUserPollingConfig(kPollIntervalDefaultSeconds, "")) {
		return false;
	}

	g_portalPollingCacheValid = false;
	g_portalPollingCacheEntityCount = 0;
	g_portalPollingCacheIntervalSeconds = kPollIntervalDefaultSeconds;
	pollingConfigLoadedFromStorage = false;
	loadPollingConfig();
	if (!pollingConfigLoadedFromStorage || pollIntervalSeconds != kPollIntervalDefaultSeconds) {
		return false;
	}

	updatePollingLastChange();
	// A runtime reset can move entities back to freqDisabled. Clear the current
	// retained HA discovery surface first so resend only republishes entities that
	// remain enabled under the default schedule.
	queueCurrentHaDiscoveryClears();
	requestHaDataResend();
	resendAllData = true;
	publishPollingConfig();
	return true;
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
#ifdef DEBUG_OVER_SERIAL
	Serial.printf("Persisting reboot intent=%s mode=%s\r\n",
	              bootIntentToString(intent),
	              bootModeToString(bootModeForIntent(intent, currentBootMode)));
#endif
	if (persistIntent) {
		persistUserBootIntent(intent);
	}
	persistUserBootMode(bootModeForIntent(intent, currentBootMode));
	triggerRestart();
}

static void
scheduleDeferredControlPlaneReboot(BootIntent intent, unsigned long delayMs)
{
	deferredControlPlaneRebootIntent = intent;
	deferredControlPlaneRebootAt = millis() + delayMs;
	deferredControlPlaneRebootScheduled = true;
#ifdef DEBUG_OVER_SERIAL
	Serial.printf("Deferred reboot queued intent=%s delay_ms=%lu due_at=%lu\r\n",
	              bootIntentToString(intent),
	              delayMs,
	              deferredControlPlaneRebootAt);
#endif
}

static void
schedulePortalReboot(BootIntent intent, unsigned long delayMs)
{
	portalRebootIntent = intent;
	portalRebootAt = millis() + delayMs;
	portalRebootScheduled = true;
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
#define portalLog(...)                   \
	do {                                 \
		Serial.print(F("[portal] "));    \
		Serial.printf(__VA_ARGS__);      \
		Serial.print(F("\r\n"));         \
	} while (0)
#else
#define portalLog(...) do { } while (0)
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

static const char*
wifiDisconnectReasonLabel(int reason)
{
#if defined(MP_ESP8266)
	switch (reason) {
	case REASON_AUTH_FAIL:
		return "Auth failed";
	case REASON_NO_AP_FOUND:
		return "AP not found";
	case REASON_ASSOC_FAIL:
		return "Association failed";
	case REASON_HANDSHAKE_TIMEOUT:
		return "Handshake timeout";
	default:
		return "Disconnect";
	}
#else
	(void)reason;
	return "Disconnect";
#endif
}

static void
notePortalActivity(void)
{
	portalLastActivityAt = millis();
}

static void
clearWifiFailureTracking(void)
{
	wifiLastDisconnectReason = -1;
	wifiLastDisconnectLabel[0] = '\0';
}

static WifiFailureSignals
currentWifiFailureSignals(void)
{
	WifiFailureSignals signals;
	const wl_status_t status = WiFi.status();
	signals.connected = status == WL_CONNECTED;
	signals.missingSsid = status == WL_NO_SSID_AVAIL;
	signals.connectFailed = status == WL_CONNECT_FAILED;
#if defined(MP_ESP8266)
	signals.authFailed = wifiLastDisconnectReason == REASON_AUTH_FAIL;
	if (wifiLastDisconnectReason == REASON_NO_AP_FOUND) {
		signals.missingSsid = true;
	}
#endif
	return signals;
}

static void
htmlEscapeInto(const char *src, char *dest, size_t destSize)
{
	if (dest == nullptr || destSize == 0) {
		return;
	}
	dest[0] = '\0';
	if (src == nullptr) {
		return;
	}

	size_t out = 0;
	for (const char *p = src; *p != '\0' && out + 1 < destSize; ++p) {
		const char *replacement = nullptr;
		switch (*p) {
		case '&':
			replacement = "&amp;";
			break;
		case '<':
			replacement = "&lt;";
			break;
		case '>':
			replacement = "&gt;";
			break;
		case '"':
			replacement = "&quot;";
			break;
		case '\'':
			replacement = "&#39;";
			break;
		default:
			dest[out++] = *p;
			dest[out] = '\0';
			continue;
		}

		const size_t replLen = strlen(replacement);
		if (out + replLen >= destSize) {
			break;
		}
		memcpy(dest + out, replacement, replLen);
		out += replLen;
		dest[out] = '\0';
	}
}

struct PortalResponseWriter {
	HttpServer *server = nullptr;
	size_t bytes = 0;
	bool allowYield = true;
	static constexpr size_t kMaxWriteChunkBytes = 128;

	bool
	writeBytes(const uint8_t *data, size_t len)
	{
		if (data == nullptr) {
			return false;
		}
		if (server == nullptr) {
			bytes += len;
			return true;
		}
		size_t written = 0;
		while (written < len) {
			if (!server->client().connected()) {
				return false;
			}
			const size_t remaining = len - written;
			const size_t chunkLen = (remaining < kMaxWriteChunkBytes) ? remaining : kMaxWriteChunkBytes;
			server->sendContent(reinterpret_cast<const char *>(data + written), chunkLen);
			written += chunkLen;
			if (allowYield) {
				diagYield();
			}
#if defined(MP_ESP8266)
			ESP.wdtFeed();
#endif
		}
		bytes += len;
#if defined(MP_ESP8266)
		ESP.wdtFeed();
#endif
		return true;
	}

	bool
	write(const char *content)
	{
		if (content == nullptr) {
			return false;
		}
		const size_t len = strlen(content);
		return writeBytes(reinterpret_cast<const uint8_t *>(content), len);
	}

	bool
	writeP(PGM_P content)
		{
			if (content == nullptr) {
				return false;
			}
			const size_t totalLen = strlen_P(content);
			if (server == nullptr) {
				bytes += totalLen;
				return true;
			}
			size_t remaining = totalLen;
			size_t offset = 0;
			while (remaining > 0) {
				if (!server->client().connected()) {
					return false;
				}
				const size_t chunk = (remaining < kMaxWriteChunkBytes) ? remaining : kMaxWriteChunkBytes;
				server->sendContent_P(content + offset, chunk);
				offset += chunk;
				remaining -= chunk;
				if (allowYield) {
					diagYield();
				}
			}
			bytes += totalLen;
			return true;
		}
};

static bool
writePortalUiPageStart(PortalResponseWriter &writer,
                       const char *title,
                       const char *heading,
                       PortalUiMode mode,
                       PGM_P extraHead = nullptr)
{
	return writer.writeP(kUiPageHeadOpen) &&
	       writer.write(portalUiModeToken(mode)) &&
	       writer.writeP(kUiPageHeadAccentOpen) &&
	       writer.write(portalUiAccentHex(mode)) &&
	       writer.writeP(kUiPageHeadVarsOpen) &&
	       writer.write(portalUiAccentHex(mode)) &&
	       writer.writeP(kUiPageHeadVarsMid) &&
	       writer.write(portalUiAccentDarkHex(mode)) &&
	       writer.writeP(kUiPageHeadVarsClose) &&
	       (extraHead == nullptr || writer.writeP(extraHead)) &&
	       writer.writeP(kUiSharedStyle) &&
	       writer.writeP(kUiPageTitleOpen) &&
	       writer.write(title != nullptr ? title : "Alpha2MQTT") &&
	       writer.writeP(kUiPageTitleClose) &&
	       writer.writeP(kUiPageBodyOpen) &&
	       writer.write(portalUiModeToken(mode)) &&
	       writer.writeP(kUiPageHeadingOpen) &&
	       writer.write(heading != nullptr ? heading : "Alpha2MQTT") &&
	       writer.writeP(kUiPageHeadingClose);
}

static bool
writePortalUiPageStartP(PortalResponseWriter &writer,
                        PGM_P title,
                        PGM_P heading,
                        PortalUiMode mode,
                        PGM_P extraHead = nullptr)
{
	return writer.writeP(kUiPageHeadOpen) &&
	       writer.write(portalUiModeToken(mode)) &&
	       writer.writeP(kUiPageHeadAccentOpen) &&
	       writer.write(portalUiAccentHex(mode)) &&
	       writer.writeP(kUiPageHeadVarsOpen) &&
	       writer.write(portalUiAccentHex(mode)) &&
	       writer.writeP(kUiPageHeadVarsMid) &&
	       writer.write(portalUiAccentDarkHex(mode)) &&
	       writer.writeP(kUiPageHeadVarsClose) &&
	       (extraHead == nullptr || writer.writeP(extraHead)) &&
	       writer.writeP(kUiSharedStyle) &&
	       writer.writeP(kUiPageTitleOpen) &&
	       writer.writeP(title != nullptr ? title : PSTR("Alpha2MQTT")) &&
	       writer.writeP(kUiPageTitleClose) &&
	       writer.writeP(kUiPageBodyOpen) &&
	       writer.write(portalUiModeToken(mode)) &&
	       writer.writeP(kUiPageHeadingOpen) &&
	       writer.writeP(heading != nullptr ? heading : PSTR("Alpha2MQTT")) &&
	       writer.writeP(kUiPageHeadingClose);
}

template <typename EmitFn>
static bool
sendPortalHtmlResponse(HttpServer *server, EmitFn emitPage, const char *fallbackError, bool allowYield = true)
{
	if (server == nullptr) {
		return false;
	}

	if (!server->client().connected()) {
#ifdef DEBUG_OVER_SERIAL
		portalLog("portal html response: client disconnected before headers");
#endif
		return false;
	}
	server->sendHeader("Cache-Control", "no-store");
#if defined(MP_ESP8266)
	if (!server->chunkedResponseModeStart_P(200, PSTR("text/html"))) {
#else
	if (!server->chunkedResponseModeStart(200, "text/html")) {
#endif
		// AP-mode WiFiManager can occasionally refuse chunked startup even though the
		// socket is still usable. Fall back to a counted fixed-length stream so the
		// portal stays reachable instead of returning a 500 on simple pages.
		PortalResponseWriter countWriter;
		countWriter.server = nullptr;
		countWriter.allowYield = allowYield;
		if (!emitPage(countWriter) || countWriter.bytes == 0) {
#ifdef DEBUG_OVER_SERIAL
			portalLog("portal html response: chunked start and count fallback failed free=%u max=%u frag=%u",
		          ESP.getFreeHeap(),
		          ESP.getMaxFreeBlockSize(),
		          ESP.getHeapFragmentation());
#endif
			server->send(500, "text/plain", fallbackError != nullptr ? fallbackError : "portal unavailable");
			return false;
		}
#ifdef DEBUG_OVER_SERIAL
		portalLog("portal html response: chunked start failed; falling back to fixed-length stream bytes=%u free=%u max=%u frag=%u",
		          static_cast<unsigned>(countWriter.bytes),
		          ESP.getFreeHeap(),
		          ESP.getMaxFreeBlockSize(),
		          ESP.getHeapFragmentation());
#endif
		server->setContentLength(countWriter.bytes);
		server->send(200, "text/html", "");
		PortalResponseWriter bodyWriter;
		bodyWriter.server = server;
		bodyWriter.allowYield = allowYield;
		if (!emitPage(bodyWriter)) {
#ifdef DEBUG_OVER_SERIAL
			portalLog("portal html response: fixed-length body write failed free=%u max=%u frag=%u",
			          ESP.getFreeHeap(),
			          ESP.getMaxFreeBlockSize(),
			          ESP.getHeapFragmentation());
#endif
			return false;
		}
		return true;
	}

	PortalResponseWriter writer;
	writer.server = server;
	writer.allowYield = allowYield;
	if (!emitPage(writer)) {
#ifdef DEBUG_OVER_SERIAL
		portalLog("portal html response: body write failed free=%u max=%u frag=%u",
		          ESP.getFreeHeap(),
		          ESP.getMaxFreeBlockSize(),
		          ESP.getHeapFragmentation());
#endif
		server->chunkedResponseFinalize();
		return false;
	}
	server->chunkedResponseFinalize();
	return true;
}

static bool
appendHttpHtmlText(char *out, size_t outSize, size_t &used, const char *text)
{
	if (out == nullptr || outSize == 0 || text == nullptr || used >= outSize) {
		return false;
	}
	const size_t remaining = outSize - used;
	const size_t len = strlen(text);
	if (len >= remaining) {
		return false;
	}
	memcpy(out + used, text, len + 1);
	used += len;
	return true;
}

static bool
appendHttpHtmlTextP(char *out, size_t outSize, size_t &used, PGM_P text)
{
	if (out == nullptr || outSize == 0 || text == nullptr || used >= outSize) {
		return false;
	}
	const size_t remaining = outSize - used;
	const size_t len = strlen_P(text);
	if (len >= remaining) {
		return false;
	}
	memcpy_P(out + used, text, len);
	used += len;
	out[used] = '\0';
	return true;
}

static bool
appendHttpHtmlFmt(char *out, size_t outSize, size_t &used, const char *fmt, ...)
{
	if (out == nullptr || outSize == 0 || fmt == nullptr || used >= outSize) {
		return false;
	}
	va_list args;
	va_start(args, fmt);
	const int written = vsnprintf(out + used, outSize - used, fmt, args);
	va_end(args);
	if (written < 0 || static_cast<size_t>(written) >= (outSize - used)) {
		return false;
	}
	used += static_cast<size_t>(written);
	return true;
}

static bool
writePortalFmt(PortalResponseWriter &writer, const char *fmt, ...)
{
	if (fmt == nullptr) {
		return false;
	}
	char buf[256];
	va_list args;
	va_start(args, fmt);
	const int written = vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	if (written < 0 || static_cast<size_t>(written) >= sizeof(buf)) {
		return false;
	}
	return writer.write(buf);
}

void
handleHttpRoot(void)
{
#ifdef DEBUG_OVER_SERIAL
	Serial.println("HTTP GET /");
#endif
	if (deferredControlPlaneRebootScheduled) {
		httpServerRef().sendHeader("Cache-Control", "no-store");
		httpServerRef().sendHeader("Retry-After", "2");
		httpServerRef().send(503, "text/plain", "Reboot pending");
		return;
	}

	const IPAddress ip = WiFi.localIP();
	const wl_status_t wifiStatus = WiFi.status();
	const unsigned long rs485ErrorCount = static_cast<unsigned long>(rs485Errors);
	const unsigned long rs485TransportErrorCount = static_cast<unsigned long>(rs485TransportErrors);
	const unsigned long rs485OtherErrorCount = static_cast<unsigned long>(rs485OtherErrors);
	const bool pollingResetApplied = httpServerRef().hasArg("polling_reset") &&
	                                 httpServerRef().arg("polling_reset") == "1";
	const bool pollingResetFailed = httpServerRef().hasArg("polling_reset") &&
	                                httpServerRef().arg("polling_reset") == "err";
#if RS485_STUB
	const char *rs485Backend = "stub";
#else
	const char *rs485Backend = "real";
#endif
	auto emitPage = [&](PortalResponseWriter &writer) -> bool {
		if (!writePortalUiPageStartP(writer,
		                             PSTR("Alpha2MQTT Control"),
		                             PSTR("Alpha2MQTT Control"),
		                             PortalUiMode::Normal) ||
		    !writePortalFmt(writer,
		                    "<p>Boot mode: %s<br>Boot intent: %s<br>Reset reason: %s</p>",
		                    bootModeToString(currentBootMode),
		                    bootIntentToString(currentBootIntent),
		                    lastResetReason)) {
			return false;
		}
		if (pollingResetApplied && !writer.writeP(PSTR("<p>Polling reset to defaults.</p>"))) {
			return false;
		}
		if (pollingResetFailed && !writer.writeP(PSTR("<p>Polling reset failed.</p>"))) {
			return false;
		}
		if (!writer.writeP(PSTR("<form method='POST' action='/reboot/normal'><button>Reboot Normal</button></form>"
		                        "<form method='GET' action='/reboot/ap'><button>Reboot AP Config</button></form>"
		                        "<form method='POST' action='/reboot/wifi'><button>Reboot WiFi Config</button></form>"
		                        "<h4>Status</h4><p>")) ||
		    !writePortalFmt(writer,
		                    "Firmware version: %s<br>RS485 backend: %s<br>"
		                    "Uptime (ms): %lu<br>WiFi status: %d<br>RSSI (dBm): %d<br>IP: %u.%u.%u.%u",
		                    _version,
		                    rs485Backend,
		                    static_cast<unsigned long>(millis()),
		                    static_cast<int>(wifiStatus),
		                    WiFi.RSSI(),
		                    ip[0], ip[1], ip[2], ip[3]) ||
		    !writePortalFmt(writer,
		                    "<br>MQTT connected: %u<br>MQTT reconnects: %lu"
		                    "<br>Inverter ready: %u<br>RS485 state: %u"
		                    "<br>RS485 errors: %lu"
		                    "<br>RS485 transport errors: %lu"
		                    "<br>RS485 other errors: %lu",
		                    _mqtt.connected() ? 1U : 0U,
		                    static_cast<unsigned long>(mqttReconnectCount),
		                    inverterReady ? 1U : 0U,
		                    static_cast<unsigned>(rs485ConnectState),
		                    rs485ErrorCount,
		                    rs485TransportErrorCount,
		                    rs485OtherErrorCount) ||
		    !writePortalFmt(writer,
		                    "<br>Poll ok: %lu<br>Poll err: %lu<br>Last poll ms: %lu"
		                    "<br>ESS snapshot ok: %u<br>ESS snapshot attempts: %lu<br>poll_interval_s: %lu",
		                    static_cast<unsigned long>(pollOkCount),
		                    static_cast<unsigned long>(pollErrCount),
		                    static_cast<unsigned long>(lastPollMs),
		                    essSnapshotLastOk ? 1U : 0U,
		                    static_cast<unsigned long>(essSnapshotAttemptCount),
		                    static_cast<unsigned long>(pollIntervalSeconds))) {
			return false;
		}
#if defined(MP_ESP8266)
		if (!writePortalFmt(writer,
		                    "<br>Heap free/max/frag: %u/%u/%u",
		                    ESP.getFreeHeap(),
		                    ESP.getMaxFreeBlockSize(),
		                    ESP.getHeapFragmentation())) {
#else
		if (!writePortalFmt(writer, "<br>Heap free: %u", ESP.getFreeHeap())) {
#endif
			return false;
		}
		return writer.writeP(PSTR("</p>")) && writer.writeP(kUiPageTail);
	};

	sendPortalHtmlResponse(&httpServerRef(), emitPage, "control page unavailable");
}

static bool
writePortalMenuButton(PortalResponseWriter &writer, const char *action, const char *label, const char *method)
{
	return writer.write("<form action=\"") &&
	       writer.write(action != nullptr ? action : "/") &&
	       writer.write("\" method=\"") &&
	       writer.write(method != nullptr ? method : "get") &&
	       writer.write("\"><button type=\"submit\">") &&
	       writer.write(label != nullptr ? label : "Open") &&
	       writer.write("</button></form>");
}

static bool
writePortalMenuButtonP(PortalResponseWriter &writer, PGM_P action, PGM_P label, PGM_P method)
{
	return writer.writeP(PSTR("<form action=\"")) &&
	       writer.writeP(action != nullptr ? action : PSTR("/")) &&
	       writer.writeP(PSTR("\" method=\"")) &&
	       writer.writeP(method != nullptr ? method : PSTR("get")) &&
	       writer.writeP(PSTR("\"><button type=\"submit\">")) &&
	       writer.writeP(label != nullptr ? label : PSTR("Open")) &&
	       writer.writeP(PSTR("</button></form>"));
}

static bool
writePortalHtmlEscaped(PortalResponseWriter &writer, const char *src)
{
	if (src == nullptr) {
		return true;
	}

	char chunk[64];
	size_t used = 0;
	auto flush = [&]() -> bool {
		if (used == 0) {
			return true;
		}
		chunk[used] = '\0';
		const bool ok = writer.write(chunk);
		used = 0;
		return ok;
	};
	auto append = [&](const char *text) -> bool {
		if (text == nullptr) {
			return false;
		}
		const size_t textLen = strlen(text);
		if (textLen >= sizeof(chunk)) {
			return flush() && writer.write(text);
		}
		if (used + textLen >= sizeof(chunk)) {
			if (!flush()) {
				return false;
			}
		}
		memcpy(chunk + used, text, textLen);
		used += textLen;
		return true;
	};

	for (const char *p = src; *p != '\0'; ++p) {
		const char *replacement = nullptr;
		switch (*p) {
		case '&':
			replacement = "&amp;";
			break;
		case '<':
			replacement = "&lt;";
			break;
		case '>':
			replacement = "&gt;";
			break;
		case '"':
			replacement = "&quot;";
			break;
		case '\'':
			replacement = "&#39;";
			break;
		default:
			char single[2] = { *p, '\0' };
			if (!append(single)) {
				return false;
			}
			continue;
		}

		if (!append(replacement)) {
			return false;
		}
	}
	return flush();
}

enum class RebootProbeStrategy : uint8_t {
	Fetch,
	Iframe,
	Manual
};

struct RebootHandoffSpec {
	PGM_P title = nullptr;
	PGM_P heading = nullptr;
	PortalUiMode targetUiMode = PortalUiMode::Normal;
	RebootProbeStrategy probeStrategy = RebootProbeStrategy::Fetch;
	PGM_P expectedModeToken = nullptr;
	bool showExpectedUrl = false;
	char probeUrl[96] = "";
	char expectedUrl[96] = "";
	char manualHint[192] = "";
};

static const char kRebootHandoffBodyOpen[] PROGMEM =
	"<div id=\"reboot-handoff\" data-target-mode=\"";
static const char kRebootHandoffProbeUrlOpen[] PROGMEM = "\" data-probe-url=\"";
static const char kRebootHandoffProbeKindOpen[] PROGMEM = "\" data-probe-kind=\"";
static const char kRebootHandoffTimingOpen[] PROGMEM =
	"\" data-start-ms=\"10000\" data-retry-ms=\"5000\" data-timeout-ms=\"300000\">";
static const char kRebootHandoffLead[] PROGMEM =
	"<p><strong>Auto-refresh starts in 10 seconds and retries every 5 seconds for up to 5 minutes.</strong></p>";
static const char kRebootHandoffWait[] PROGMEM =
	"<pre id=\"reboot-status\">Waiting 10s before checks begin.</pre>";
static const char kRebootHandoffTryNowOpen[] PROGMEM = "<p><a href=\"";
static const char kRebootHandoffTryNowMid[] PROGMEM = "\">Open expected address now</a></p>";
static const char kRebootHandoffIframe[] PROGMEM = "<iframe id=\"reboot-probe-frame\" hidden></iframe>";
static const char kRebootProbeKindFetch[] PROGMEM = "fetch";
static const char kRebootProbeKindIframe[] PROGMEM = "iframe";
static const char kRebootProbeKindManual[] PROGMEM = "manual";
static const char kRebootTitleDefault[] PROGMEM = "Rebooting";
static const char kRebootHeadingDefault[] PROGMEM = "Rebooting";
static const char kRebootModeDefault[] PROGMEM = "normal";
static const char kRebootTitleAp[] PROGMEM = "Rebooting to AP Config";
static const char kRebootHeadingAp[] PROGMEM = "Rebooting to AP config";
static const char kRebootTitleWifi[] PROGMEM = "Rebooting to Wi-Fi Config";
static const char kRebootHeadingWifi[] PROGMEM = "Rebooting to Wi-Fi config";
static const char kRebootTitleNormal[] PROGMEM = "Rebooting to Normal";
static const char kRebootHeadingNormal[] PROGMEM = "Rebooting to normal mode";
static const char kRebootManualHintSameOrigin[] PROGMEM =
	"This page should return here automatically when the destination is ready. If it does not, refresh this page manually.";
static const char kRebootManualHintAp[] PROGMEM =
	"Join WiFi SSID %s first, then open %s if the browser does not return automatically. Auto-refresh is best effort across the network change.";
static const char kRebootManualHintSta[] PROGMEM =
	"Reconnect to your LAN, then open %s if the browser does not return automatically. Auto-refresh is best effort across the network change.";
static const char kRebootHandoffScript[] PROGMEM =
	"<script>"
	"(function(){"
	"var root=document.getElementById('reboot-handoff');"
	"if(!root){return;}"
	"var target=root.getAttribute('data-target-mode')||'';"
	"var probe=root.getAttribute('data-probe-url')||'/';"
	"var kind=root.getAttribute('data-probe-kind')||'fetch';"
	"var startMs=parseInt(root.getAttribute('data-start-ms')||'10000',10);"
	"var retryMs=parseInt(root.getAttribute('data-retry-ms')||'5000',10);"
	"var timeoutMs=parseInt(root.getAttribute('data-timeout-ms')||'300000',10);"
	"var status=document.getElementById('reboot-status');"
	"var frame=document.getElementById('reboot-probe-frame');"
	"var started=Date.now();"
	"var armAt=started+startMs;"
	"var redirected=false;"
	"function elapsed(){return Math.floor((Date.now()-started)/1000);}"
	"function setStatus(text){if(status){status.textContent=text;}}"
	"function redirect(){if(redirected){return;}redirected=true;window.location.href=probe;}"
	"function schedule(){if(redirected){return;}setTimeout(loop,retryMs);}"
	"function loop(){"
	"if(redirected){return;}"
	"if((Date.now()-started)>timeoutMs){setStatus('Timed out after '+Math.floor(timeoutMs/1000)+'s. Open the expected address manually.');return;}"
	"if(Date.now()<armAt){setStatus('Waiting to start checks: '+Math.max(0,Math.ceil((armAt-Date.now())/1000))+'s');setTimeout(loop,500);return;}"
	"setStatus('Checking destination… elapsed '+elapsed()+'s');"
	"if(kind==='manual'){"
	"setStatus('Auto-open is disabled for this network change. Join the target network and open the expected address manually. Elapsed '+elapsed()+'s');"
	"schedule();"
	"return;"
	"}"
	"if(kind==='iframe'){"
	"if(!frame){setStatus('Browser probe unavailable. Open the expected address manually.');return;}"
	"frame.onload=function(){redirect();};"
	"frame.src=probe+(probe.indexOf('?')===-1?'?':'&')+'_a2m_probe='+(Date.now());"
	"schedule();"
	"return;"
	"}"
	"fetch(probe,{cache:'no-store'})"
	".then(function(r){return r.text().then(function(t){"
	"if(r&&r.ok&&t&&t.indexOf('meta name=\"a2m-mode\" content=\"'+target+'\"')!==-1){redirect();return;}"
	"setStatus('Destination not ready yet. Elapsed '+elapsed()+'s');"
	"schedule();"
	"});})"
	".catch(function(){setStatus('Destination not reachable yet. Elapsed '+elapsed()+'s');schedule();});"
	"}"
	"loop();"
	"})();"
	"</script>";

static PortalUiMode
portalUiModeForBootMode(BootMode mode)
{
	switch (mode) {
	case BootMode::ApConfig:
		return PortalUiMode::Ap;
	case BootMode::WifiConfig:
		return PortalUiMode::Wifi;
	case BootMode::Normal:
	default:
		return PortalUiMode::Normal;
	}
}

static PGM_P
portalUiModeTokenP(PortalUiMode mode)
{
	switch (mode) {
	case PortalUiMode::Ap:
		return PSTR("ap");
	case PortalUiMode::Wifi:
		return PSTR("wifi");
	case PortalUiMode::Normal:
	default:
		return PSTR("normal");
	}
}

static bool
buildRebootHandoffSpec(BootIntent intent, RebootHandoffSpec &spec)
{
	spec.title = kRebootTitleDefault;
	spec.heading = kRebootHeadingDefault;
	spec.expectedModeToken = kRebootModeDefault;

	const BootMode targetMode = bootModeForIntent(intent, currentBootMode);
	spec.targetUiMode = portalUiModeForBootMode(targetMode);
	spec.expectedModeToken = portalUiModeTokenP(spec.targetUiMode);

	switch (spec.targetUiMode) {
	case PortalUiMode::Ap:
		spec.title = kRebootTitleAp;
		spec.heading = kRebootHeadingAp;
		break;
	case PortalUiMode::Wifi:
		spec.title = kRebootTitleWifi;
		spec.heading = kRebootHeadingWifi;
		break;
	case PortalUiMode::Normal:
	default:
		spec.title = kRebootTitleNormal;
		spec.heading = kRebootHeadingNormal;
		break;
	}

	if (targetMode == currentBootMode) {
		spec.probeStrategy = RebootProbeStrategy::Fetch;
		strlcpy(spec.probeUrl, "/", sizeof(spec.probeUrl));
		strlcpy(spec.expectedUrl, "/", sizeof(spec.expectedUrl));
		strncpy_P(spec.manualHint, kRebootManualHintSameOrigin, sizeof(spec.manualHint) - 1);
		spec.manualHint[sizeof(spec.manualHint) - 1] = '\0';
		return true;
	}

	if (targetMode == BootMode::ApConfig) {
		spec.probeStrategy = RebootProbeStrategy::Manual;
		spec.showExpectedUrl = true;
		strlcpy(spec.probeUrl, "http://192.168.4.1/", sizeof(spec.probeUrl));
		strlcpy(spec.expectedUrl, spec.probeUrl, sizeof(spec.expectedUrl));
		snprintf_P(spec.manualHint,
		           sizeof(spec.manualHint),
		           kRebootManualHintAp,
		           deviceName,
		           spec.expectedUrl);
		return true;
	}

	if (currentBootMode == BootMode::ApConfig &&
	    (targetMode == BootMode::Normal || targetMode == BootMode::WifiConfig)) {
		spec.probeStrategy = RebootProbeStrategy::Iframe;
		spec.showExpectedUrl = true;
		snprintf(spec.probeUrl, sizeof(spec.probeUrl), "http://%s/", deviceName);
		strlcpy(spec.expectedUrl, spec.probeUrl, sizeof(spec.expectedUrl));
		snprintf_P(spec.manualHint,
		           sizeof(spec.manualHint),
		           kRebootManualHintSta,
		           spec.expectedUrl);
		return true;
	}

	spec.probeStrategy = RebootProbeStrategy::Fetch;
	strlcpy(spec.probeUrl, "/", sizeof(spec.probeUrl));
	strlcpy(spec.expectedUrl, "/", sizeof(spec.expectedUrl));
	strncpy_P(spec.manualHint, kRebootManualHintSameOrigin, sizeof(spec.manualHint) - 1);
	spec.manualHint[sizeof(spec.manualHint) - 1] = '\0';
	return true;
}

static bool
sendRebootHandoffPage(HttpServer *server, BootIntent intent)
{
	if (server == nullptr) {
		return false;
	}

	RebootHandoffSpec spec{};
	if (!buildRebootHandoffSpec(intent, spec)) {
		return false;
	}

	auto emitPage = [&](PortalResponseWriter &writer) -> bool {
		PGM_P probeKind =
			(spec.probeStrategy == RebootProbeStrategy::Fetch)
				? kRebootProbeKindFetch
				: ((spec.probeStrategy == RebootProbeStrategy::Iframe)
					   ? kRebootProbeKindIframe
					   : kRebootProbeKindManual);
		if (!writePortalUiPageStartP(writer, spec.title, spec.heading, spec.targetUiMode) ||
		    !writer.writeP(kRebootHandoffBodyOpen) ||
		    !writer.writeP(spec.expectedModeToken) ||
		    !writer.writeP(kRebootHandoffProbeUrlOpen) ||
		    !writer.write(spec.probeUrl) ||
		    !writer.writeP(kRebootHandoffProbeKindOpen) ||
		    !writer.writeP(probeKind) ||
		    !writer.writeP(kRebootHandoffTimingOpen) ||
		    !writer.writeP(kRebootHandoffLead) ||
		    !writer.writeP(kRebootHandoffWait)) {
			return false;
		}
		if (spec.showExpectedUrl &&
		    (!writer.writeP(PSTR("<p>Expected address: <a href=\"")) ||
		     !writer.write(spec.expectedUrl) ||
		     !writer.writeP(PSTR("\">")) ||
		     !writer.write(spec.expectedUrl) ||
		     !writer.writeP(PSTR("</a></p>")))) {
			return false;
		}
		if (!writer.writeP(PSTR("<p>")) ||
		    !writePortalHtmlEscaped(writer, spec.manualHint) ||
		    !writer.writeP(PSTR("</p>"))) {
			return false;
		}
		if (spec.showExpectedUrl &&
		    (!writer.writeP(kRebootHandoffTryNowOpen) ||
		     !writer.write(spec.expectedUrl) ||
		     !writer.writeP(kRebootHandoffTryNowMid))) {
			return false;
		}
		if (spec.probeStrategy == RebootProbeStrategy::Iframe && !writer.writeP(kRebootHandoffIframe)) {
			return false;
		}
		return writer.writeP(PSTR("</div>")) &&
		       writer.writeP(kRebootHandoffScript) &&
		       writer.writeP(kUiPageTail);
	};

	return sendPortalHtmlResponse(server, emitPage, "reboot handoff unavailable");
}

struct PortalPollingProfileApplyResult {
	bool ok = false;
	uint32_t unknownCount = 0;
};

static bool
writePortalCurrentPollingProfile(PortalResponseWriter &writer)
{
	ensurePortalPollingRuntimeReady();
	const size_t entityCount = mqttEntitiesCount();
	if (entityCount == 0 || entityCount > kMqttEntityDescriptorCount) {
		return false;
	}
	if (!g_portalPollingCacheValid || g_portalPollingCacheEntityCount != entityCount) {
		primePortalPollingCache();
	}
	if (!g_portalPollingCacheValid || g_portalPollingCacheEntityCount != entityCount) {
		return false;
	}

	char line[96];
	const int intervalWritten = snprintf(line,
	                                     sizeof(line),
	                                     "poll_interval_s=%lu\n",
	                                     static_cast<unsigned long>(g_portalPollingCacheIntervalSeconds));
	if (!writer.write("A2M_POLLING_PROFILE 1\n") || intervalWritten <= 0 ||
	    static_cast<size_t>(intervalWritten) >= sizeof(line) || !writer.write(line)) {
		return false;
	}

	for (size_t idx = 0; idx < entityCount; ++idx) {
		const BucketId bucket = g_portalBucketsScratch[idx];
		if (bucket == BucketId::Unknown || bucket == BucketId::Disabled) {
			continue;
		}
		mqttState entity{};
		if (!mqttEntityCopyByIndex(idx, &entity)) {
			return false;
		}
		if (!includeEntityInPollingPortal(entity)) {
			continue;
		}
		char entityName[64];
		mqttEntityNameCopy(&entity, entityName, sizeof(entityName));
		const char *bucketName = bucketIdToString(bucket);
		const int written = snprintf(line, sizeof(line), "%s=%s\n", entityName, bucketName);
		if (written <= 0 || static_cast<size_t>(written) >= sizeof(line) || !writer.write(line)) {
			return false;
		}
	}
	return true;
}

static bool
initializePortalPollingProfileBuckets(BucketId *buckets, size_t entityCount)
{
	if (buckets == nullptr || !resetBucketsToCatalogDefaults(buckets, entityCount)) {
		return false;
	}
	for (size_t idx = 0; idx < entityCount; ++idx) {
		mqttState entity{};
		if (!mqttEntityCopyByIndex(idx, &entity)) {
			return false;
		}
		if (includeEntityInPollingPortal(entity)) {
			buckets[idx] = BucketId::Disabled;
		}
	}
	return true;
}

static PortalPollingProfileApplyResult
applyPortalPollingProfileState(uint32_t stagedIntervalSeconds,
                               const BucketId *stagedBuckets,
                               size_t entityCount,
                               uint32_t unknownCount)
{
	// Import contract:
	// - portal import fully replaces the visible polling schedule
	// - entities omitted from the file are disabled before apply
	// - runtime and HA observe the imported schedule after Reboot Normal
	PortalPollingProfileApplyResult result{};
	if (stagedBuckets == nullptr || entityCount == 0 || entityCount > kMqttEntityDescriptorCount) {
		return result;
	}
	if (!ensurePortalBucketsScratch()) {
		return result;
	}

	const uint32_t storedIntervalSeconds = clampPollInterval(stagedIntervalSeconds);
	size_t persistedOverrideCount = 0;
	size_t persistedMapLen = 0;
	if (!portalEstimatePersistedBucketMap(stagedBuckets,
	                                      entityCount,
	                                      persistedMapLen,
	                                      persistedOverrideCount)) {
		return result;
	}
	ScopedCharBuffer canonicalMapBuffer((persistedMapLen == 0 ? 0 : persistedMapLen) + 1);
	if (!canonicalMapBuffer.ok()) {
		return result;
	}
	if (!portalBuildPersistedBucketMap(stagedBuckets,
	                                   entityCount,
	                                   canonicalMapBuffer.data,
	                                   canonicalMapBuffer.size,
	                                   persistedOverrideCount)) {
		return result;
	}
	if (mqttEntitiesRtAvailable() && !mqttEntityCanApplyBuckets(stagedBuckets, entityCount)) {
		return result;
	}
	if (!persistUserPollingConfig(storedIntervalSeconds, canonicalMapBuffer.data)) {
		return result;
	}

	memcpy(g_portalBucketsScratch, stagedBuckets, entityCount * sizeof(BucketId));
	pollIntervalSeconds = storedIntervalSeconds;
	g_portalPollingCacheValid = true;
	g_portalPollingCacheEntityCount = entityCount;
	g_portalPollingCacheIntervalSeconds = storedIntervalSeconds;
	updatePollingLastChange();
	pollingConfigLoadedFromStorage = true;
	portalRebootScheduled = false;
	portalRebootIntent = BootIntent::Normal;
	portalMqttSaved = false;
	result.ok = true;
	result.unknownCount = unknownCount;
	return result;
}

static void
resetPortalPollingProfileUploadState(void)
{
	delete[] portalPollingProfileUpload.lineBuf;
	delete[] portalPollingProfileUpload.error;
	delete[] portalPollingProfileUpload.stagedBuckets;
	delete[] portalPollingProfileUpload.seenEntities;
	portalPollingProfileUpload = PortalPollingProfileUploadState{};
}

static void
copyPortalPollingProfileUploadErrorP(PGM_P message)
{
	if (message == nullptr || portalPollingProfileUpload.error == nullptr) {
		return;
	}
	const size_t srcLen = strlen_P(message);
	const size_t copyLen =
		(srcLen < (kPollingProfileUploadErrorMaxLen - 1)) ? srcLen : (kPollingProfileUploadErrorMaxLen - 1);
	memcpy_P(portalPollingProfileUpload.error, message, copyLen);
	portalPollingProfileUpload.error[copyLen] = '\0';
}

static void
setPortalPollingProfileUploadError(const char *message)
{
	portalPollingProfileUpload.ok = false;
	if (message != nullptr && message[0] != '\0' && portalPollingProfileUpload.error != nullptr &&
	    portalPollingProfileUpload.error[0] == '\0') {
		strlcpy(portalPollingProfileUpload.error, message, kPollingProfileUploadErrorMaxLen);
	}
}

static bool
processPortalPollingProfileUploadLine(const char *line)
{
	static const char kInvalidLineFmt[] PROGMEM = "Invalid polling profile line %u.";
	static const char kHeaderOrderErr[] PROGMEM = "Profile header must appear once at the top.";
	static const char kIntervalOrderErr[] PROGMEM = "Profile must define poll_interval_s after the header.";
	static const char kAssignmentOrderErr[] PROGMEM =
		"Assignments must follow the header and poll_interval_s.";
	static const char kEntityLookupErr[] PROGMEM = "Profile entity lookup failed.";
	static const char kDuplicateEntityFmt[] PROGMEM = "Duplicate entity '%s' in polling profile.";
	portalPollingProfileUpload.lineNumber++;
	PollingProfileLine parsed{};
	if (!parsePollingProfileLine(line, parsed)) {
		if (portalPollingProfileUpload.error != nullptr) {
			snprintf_P(portalPollingProfileUpload.error,
			           kPollingProfileUploadErrorMaxLen,
			           kInvalidLineFmt,
			           static_cast<unsigned>(portalPollingProfileUpload.lineNumber));
		}
		portalPollingProfileUpload.ok = false;
		return false;
	}

	switch (parsed.kind) {
	case PollingProfileLineKind::Ignore:
		return true;
	case PollingProfileLineKind::Header:
		if (portalPollingProfileUpload.sawHeader || portalPollingProfileUpload.sawPollInterval) {
			copyPortalPollingProfileUploadErrorP(kHeaderOrderErr);
			portalPollingProfileUpload.ok = false;
			return false;
		}
		portalPollingProfileUpload.sawHeader = true;
		return true;
	case PollingProfileLineKind::PollInterval:
		if (!portalPollingProfileUpload.sawHeader || portalPollingProfileUpload.sawPollInterval) {
			copyPortalPollingProfileUploadErrorP(kIntervalOrderErr);
			portalPollingProfileUpload.ok = false;
			return false;
		}
		portalPollingProfileUpload.sawPollInterval = true;
		portalPollingProfileUpload.stagedIntervalSeconds = parsed.pollIntervalSeconds;
		return true;
	case PollingProfileLineKind::Assignment:
		if (!portalPollingProfileUpload.sawHeader || !portalPollingProfileUpload.sawPollInterval) {
			copyPortalPollingProfileUploadErrorP(kAssignmentOrderErr);
			portalPollingProfileUpload.ok = false;
			return false;
		}
		break;
	}

	size_t idx = 0;
	if (!mqttEntityIndexByName(parsed.entityName, &idx) || idx >= portalPollingProfileUpload.entityCount) {
		portalPollingProfileUpload.unknownCount++;
		return true;
	}
	mqttState entity{};
	if (!mqttEntityCopyByIndex(idx, &entity)) {
		copyPortalPollingProfileUploadErrorP(kEntityLookupErr);
		portalPollingProfileUpload.ok = false;
		return false;
	}
	if (!includeEntityInPollingPortal(entity)) {
		portalPollingProfileUpload.unknownCount++;
		return true;
	}
	if (portalPollingProfileUpload.seenEntities[idx] != 0) {
		if (portalPollingProfileUpload.error != nullptr) {
			snprintf_P(portalPollingProfileUpload.error,
			           kPollingProfileUploadErrorMaxLen,
			           kDuplicateEntityFmt,
			           parsed.entityName);
		}
		portalPollingProfileUpload.ok = false;
		return false;
	}
	portalPollingProfileUpload.seenEntities[idx] = 1;
	portalPollingProfileUpload.stagedBuckets[idx] = parsed.bucketId;
	return true;
}

static bool
appendPortalPollingProfileUploadBytes(const uint8_t *data, size_t len)
{
	static const char kLineTooLongErr[] PROGMEM = "Polling profile line exceeds the portal limit.";
	if (data == nullptr || len == 0) {
		return true;
	}
	if (portalPollingProfileUpload.lineBuf == nullptr) {
		setPortalPollingProfileUploadError("Polling profile upload buffer is unavailable.");
		return false;
	}
	for (size_t i = 0; i < len; ++i) {
		const char ch = static_cast<char>(data[i]);
		if (ch == '\r') {
			continue;
		}
		if (ch == '\n') {
			portalPollingProfileUpload.lineBuf[portalPollingProfileUpload.lineLen] = '\0';
			if (!processPortalPollingProfileUploadLine(portalPollingProfileUpload.lineBuf)) {
				return false;
			}
			portalPollingProfileUpload.lineLen = 0;
			continue;
		}
		if (portalPollingProfileUpload.lineLen + 1 >= kPollingProfileLineMaxLen) {
			copyPortalPollingProfileUploadErrorP(kLineTooLongErr);
			portalPollingProfileUpload.ok = false;
			return false;
		}
		portalPollingProfileUpload.lineBuf[portalPollingProfileUpload.lineLen++] = ch;
	}
	return true;
}

static void
renderPortalPollingImportPage(WiFiManager &wifiManager,
                              const char *message,
                              bool isError)
{
	if (!wifiManager.server) {
		return;
	}

	static const char kTitle[] PROGMEM = "Restore Polling Profile";
	static const char kHeading[] PROGMEM = "Restore polling profile";
	static const char kNav[] PROGMEM = "<p><a href=\"/\">Menu</a> | <a href=\"/config/polling\">Polling</a></p>";
	static const char kMessageOpen[] PROGMEM = "<p>";
	static const char kErrorMessageOpen[] PROGMEM = "<p><strong>";
	static const char kMessageClose[] PROGMEM = "</p>";
	static const char kErrorMessageClose[] PROGMEM = "</strong></p>";
	static const char kIntro[] PROGMEM =
		"<p>Upload a polling profile exported from another Alpha2MQTT device. "
		"Restore replaces the current polling config, and any polling entities omitted from the file will be disabled. "
		"After importing, use Reboot Normal to apply it to runtime and Home Assistant.</p>";
	static const char kFormOpenFmt[] PROGMEM =
		"<form id=\"polling-profile-form\" method=\"post\" enctype=\"multipart/form-data\" action=\"/config/polling/import?csrf=%s\">";
	static const char kLabel[] PROGMEM = "<label for=\"profile\">Polling profile file</label><br>";
	static const char kFileInput[] PROGMEM =
		"<input id=\"profile\" name=\"profile\" type=\"file\" accept=\".txt,.cfg,.profile,text/plain\">";
	static const char kHint[] PROGMEM =
		"<p class=\"hint\">Import streams one line at a time so stable entity names can be restored without buffering the whole file in RAM.</p>";
	static const char kSubmit[] PROGMEM = "<p><button type=\"submit\">Apply Profile</button></p></form>";
	refreshPortalUpdateCsrfToken();

	auto emitPage = [&](PortalResponseWriter &writer) -> bool {
		const PortalUiMode uiMode =
			(currentBootMode == BootMode::WifiConfig) ? PortalUiMode::Wifi : PortalUiMode::Ap;
		if (!writePortalUiPageStartP(writer, kTitle, kHeading, uiMode) ||
		    !writer.writeP(kNav) ||
		    !writePortalMenuButtonP(writer,
		                            PSTR("/config/polling/export"),
		                            PSTR("Download Current Polling Profile"),
		                            PSTR("get")) ||
		    !writePortalMenuButtonP(
			    writer, PSTR("/config/reboot-normal"), PSTR("Reboot Normal"), PSTR("post"))) {
			return false;
		}
		if (message != nullptr && message[0] != '\0') {
			if (!writer.writeP(isError ? kErrorMessageOpen : kMessageOpen) ||
			    !writer.write(message) ||
			    !writer.writeP(isError ? kErrorMessageClose : kMessageClose)) {
				return false;
			}
		}
		char formOpen[192];
		const int formOpenLen =
			snprintf_P(formOpen, sizeof(formOpen), kFormOpenFmt, portalUpdateCsrfToken);
		if (formOpenLen <= 0 || static_cast<size_t>(formOpenLen) >= sizeof(formOpen) ||
		    !writer.writeP(kIntro) ||
		    !writer.write(formOpen) ||
		    !writer.writeP(kLabel) ||
		    !writer.writeP(kFileInput) ||
		    !writer.writeP(kHint) ||
		    !writer.writeP(kSubmit)) {
			return false;
		}
		return writer.writeP(kUiPageTail);
	};

	(void)sendPortalHtmlResponse(wifiManager.server.get(), emitPage, "polling profile unavailable");
}

static void
handlePortalMenuPage(WiFiManager& wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	const wl_status_t staStatus = WiFi.status();
	const IPAddress ip = (currentBootMode == BootMode::ApConfig) ? WiFi.softAPIP() : WiFi.localIP();
	const bool pollingResetApplied = wifiManager.server->hasArg("polling_reset") &&
	                                 wifiManager.server->arg("polling_reset") == "1";
	const bool pollingResetFailed = wifiManager.server->hasArg("polling_reset") &&
	                                wifiManager.server->arg("polling_reset") == "err";
	char buf[192];
	static const char kWifiPortalIntro[] PROGMEM =
		"<p>WiFi config portal is active on the current LAN. Choose a setup page below.</p>";
	static const char kApPortalIntro[] PROGMEM =
		"<p>AP config portal is active. Configure WiFi or continue with setup below.</p>";

	auto emitPage = [&](PortalResponseWriter &writer) -> bool {
		const PortalUiMode uiMode =
			(currentBootMode == BootMode::WifiConfig) ? PortalUiMode::Wifi : PortalUiMode::Ap;
		if (!writePortalUiPageStartP(writer, PSTR("Alpha2MQTT Setup"), PSTR("Alpha2MQTT Setup"), uiMode)) {
			return false;
		}
		refreshPortalUpdateCsrfToken();
		if (currentBootMode == BootMode::WifiConfig) {
			if (!writer.writeP(kWifiPortalIntro)) {
				return false;
			}
		} else {
			if (!writer.writeP(kApPortalIntro)) {
				return false;
			}
		}

		const int written = snprintf(buf,
		                             sizeof(buf),
		                             "<p>Boot mode: %s<br>WiFi status: %s (%d)<br>IP: %u.%u.%u.%u</p>",
		                             bootModeToString(currentBootMode),
		                             wifiStatusLabel(staStatus),
		                             static_cast<int>(staStatus),
		                             ip[0], ip[1], ip[2], ip[3]);
		if (written <= 0 || static_cast<size_t>(written) >= sizeof(buf) || !writer.write(buf)) {
			return false;
		}
		if (pollingResetApplied && !writer.writeP(PSTR("<p><strong>Polling reset to defaults.</strong></p>"))) {
			return false;
		}
		if (pollingResetFailed && !writer.writeP(PSTR("<p><strong>Polling reset failed.</strong></p>"))) {
			return false;
		}
		if (!writePortalMenuButtonP(writer, PSTR("/0wifi"), PSTR("WiFi Setup"), PSTR("get")) ||
		    !writePortalMenuButtonP(writer, PSTR("/config/mqtt"), PSTR("MQTT Setup"), PSTR("get")) ||
		    !writePortalMenuButtonP(writer, PSTR("/config/rs485"), PSTR("RS485"), PSTR("get")) ||
		    !writePortalMenuButtonP(writer, PSTR("/config/polling"), PSTR("Polling"), PSTR("get")) ||
		    !writePortalMenuButtonP(
			    writer, PSTR("/config/polling/export"), PSTR("Download Polling Profile"), PSTR("get")) ||
		    !writePortalMenuButtonP(
			    writer, PSTR("/config/polling/import"), PSTR("Restore Polling Profile"), PSTR("get")) ||
		    !writer.writeP(PSTR("<form method=\"post\" action=\"/config/polling/reset\">"
		                       "<input type=\"hidden\" name=\"csrf\" value=\"")) ||
		    !writer.write(portalUpdateCsrfToken) ||
		    !writer.writeP(PSTR("\"><button type=\"submit\">Reset Polling Defaults</button></form>")) ||
		    !writePortalMenuButtonP(writer, PSTR("/config/update"), PSTR("Update"), PSTR("get")) ||
		    !writePortalMenuButtonP(writer, PSTR("/status"), PSTR("Status"), PSTR("get")) ||
		    !writePortalMenuButtonP(
			    writer, PSTR("/config/reboot-normal"), PSTR("Reboot Normal"), PSTR("post"))) {
			return false;
		}
		return writer.writeP(kUiPageTail);
	};
	(void)sendPortalHtmlResponse(wifiManager.server.get(), emitPage, "portal menu unavailable");
}

static void
handlePortalWifiPage(WiFiManager& wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	const bool saved = wifiManager.server->hasArg("saved");
	const String errCode = wifiManager.server->arg("err");
	const bool errSsid = errCode == "ssid";
	const bool errPass = errCode == "pass";

	char escaped[6 * kPrefWifiPasswordMaxLen] = "";
	static const char kWifiConfigIntro[] PROGMEM =
		"<p>WiFi changes are saved now and applied on the next reboot.</p>";
	static const char kApConfigIntro[] PROGMEM =
		"<p>Saving WiFi here reboots into the next onboarding step using the saved credentials.</p>";
	static const char kSaved[] PROGMEM =
		"<p><strong>Saved.</strong> The current WiFi session stays active until reboot.</p>";
	static const char kErrSsid[] PROGMEM =
		"<p><strong>WiFi SSID is required.</strong></p>";
	static const char kErrPass[] PROGMEM =
		"<p><strong>Enter a password for a different SSID, or check Open network.</strong></p>";
	static const char kTailSta[] PROGMEM =
		"<p>Leave the password blank to keep the saved password for this SSID.</p>"
		"<p>Check Open network to clear the saved password.</p>"
		"<p><button type=\"submit\">Save WiFi</button></p>"
		"</form></body></html>";
	static const char kTailAp[] PROGMEM =
		"<p>Leave the password blank for open networks.</p>"
		"<p><button type=\"submit\">Save WiFi</button></p>"
		"</form></body></html>";
	refreshPortalUpdateCsrfToken();
		auto emitPage = [&](PortalResponseWriter &writer) -> bool {
			const PortalUiMode uiMode =
				(currentBootMode == BootMode::WifiConfig) ? PortalUiMode::Wifi : PortalUiMode::Ap;
			const char *wifiSaveAction =
				(currentBootMode == BootMode::WifiConfig) ? "/wifisave" : "/config/wifi/save";
			if (!writePortalUiPageStartP(writer, PSTR("Alpha2MQTT WiFi Setup"), PSTR("WiFi Setup"), uiMode)) {
				return false;
			}
		if (currentBootMode == BootMode::WifiConfig) {
			if (!writer.writeP(kWifiConfigIntro)) {
				return false;
			}
		} else {
			if (!writer.writeP(kApConfigIntro)) {
				return false;
			}
		}
		if (saved && !writer.writeP(kSaved)) {
			return false;
		}
		if (errSsid && !writer.writeP(kErrSsid)) {
			return false;
		}
		if (errPass && !writer.writeP(kErrPass)) {
			return false;
		}
			if (!writePortalMenuButtonP(writer, PSTR("/"), PSTR("Menu"), PSTR("get")) ||
			    !writePortalMenuButtonP(writer, PSTR("/config/mqtt"), PSTR("MQTT Setup"), PSTR("get")) ||
			    !writePortalMenuButtonP(writer, PSTR("/config/rs485"), PSTR("RS485"), PSTR("get")) ||
			    !writePortalMenuButtonP(writer, PSTR("/config/polling"), PSTR("Polling"), PSTR("get")) ||
			    !writePortalMenuButtonP(
				    writer, PSTR("/config/reboot-normal"), PSTR("Reboot Normal"), PSTR("post")) ||
			    !writer.writeP(PSTR("<form method=\"POST\" action=\"")) ||
			    !writer.write(wifiSaveAction) ||
			    !writer.writeP(PSTR("\">")) ||
			    !writer.writeP(PSTR("<input type=\"hidden\" name=\"csrf\" value=\"")) ||
			    !writer.write(portalUpdateCsrfToken) ||
			    !writer.writeP(PSTR("\">"))) {
				return false;
			}
		htmlEscapeInto(appConfig.wifiSSID.c_str(), escaped, sizeof(escaped));
		if (!writer.writeP(PSTR("<p>WiFi SSID<br><input name=\"s\" value=\"")) ||
		    !writer.write(escaped) ||
		    !writer.writeP(PSTR("\" maxlength=\"63\"></p>"))) {
			return false;
		}
		if (!writer.writeP(PSTR("<p>WiFi password<br><input name=\"p\" type=\"password\" value=\"\" maxlength=\"63\" "
		                        "autocomplete=\"new-password\"></p>"))) {
			return false;
		}
		if (currentBootMode == BootMode::WifiConfig) {
			if (!writer.writeP(
			        PSTR("<p><label><input name=\"open\" type=\"checkbox\" value=\"1\"> Open network / clear saved "
			             "password</label></p>"))) {
				return false;
			}
			return writer.writeP(kTailSta);
		}
		return writer.writeP(kTailAp);
	};
	(void)sendPortalHtmlResponse(wifiManager.server.get(), emitPage, "wifi setup unavailable");
}

static void
handlePortalWifiSave(WiFiManager& wifiManager)
{
	if (!wifiManager.server) {
		return;
	}
	if (!portalUpdateRequestHasValidToken(&wifiManager)) {
		wifiManager.server->send(403, "text/plain", "invalid csrf");
		return;
	}

	const String ssid = wifiManager.server->arg("s");
	const String passArg = wifiManager.server->arg("p");
	const bool openNetworkRequested = wifiManager.server->hasArg("open");
	if (ssid.length() == 0) {
		wifiManager.server->sendHeader("Location", "/0wifi?err=ssid");
		wifiManager.server->send(302, "text/plain", "");
		return;
	}

	const bool keepExistingPassword = portalWifiSaveKeepsExistingPassword(
		appConfig.wifiSSID.c_str(), ssid.c_str(), passArg.c_str(), openNetworkRequested);
	if (!portalWifiSaveAllowsBlankPassword(
	        appConfig.wifiSSID.c_str(), ssid.c_str(), passArg.c_str(), openNetworkRequested)) {
		wifiManager.server->sendHeader("Location", "/0wifi?err=pass");
		wifiManager.server->send(302, "text/plain", "");
		return;
	}

	String pass = passArg;
	if (keepExistingPassword) {
		pass = appConfig.wifiPass;
	}
	const bool credentialsChanged = appConfig.wifiSSID != ssid || appConfig.wifiPass != pass;
	appConfig.wifiSSID = ssid;
	appConfig.wifiPass = pass;
	persistUserWifiCredentials(appConfig.wifiSSID.c_str(), appConfig.wifiPass.c_str());
	portalWifiCredentialsChanged = portalWifiCredentialsChanged || credentialsChanged;
	capturePortalActiveStaConnection();
	if (currentBootMode != BootMode::WifiConfig) {
		const PortalPostWifiAction postWifiAction = portalPostWifiActionAfterWifiSave(
			appConfig.mqttSrvr.c_str(),
			static_cast<uint16_t>(appConfig.mqttPort),
			appConfig.mqttUser.c_str(),
			appConfig.mqttPass.c_str());
		portalNeedsMqttConfig = (postWifiAction == PortalPostWifiAction::RedirectToMqttParams);
		const BootIntent rebootIntent =
			portalNeedsMqttConfig ? BootIntent::WifiConfig : portalNormalRebootIntent();
		wifiManager.server->sendHeader("Connection", "close");
		wifiManager.server->send(
			200,
			"text/html",
			portalNeedsMqttConfig
				? "<!doctype html><html><body><h3>Wi-Fi saved.</h3><p>Rebooting into Wi-Fi config...</p></body></html>"
				: "<!doctype html><html><body><h3>Wi-Fi saved.</h3><p>Rebooting into normal runtime...</p></body></html>");
#if defined(MP_ESP8266)
		ESP.wdtFeed();
#endif
		schedulePortalReboot(rebootIntent);
		return;
	}
	wifiManager.server->sendHeader("Location", "/0wifi?saved=1");
	wifiManager.server->send(302, "text/plain", "");
}

static void
handlePortalRs485Page(WiFiManager& wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	const bool saved = wifiManager.server->hasArg("saved");
	const bool err = wifiManager.server->hasArg("err");
	const uint32_t selectedBaud =
		rs485BaudTracker.hasConfiguredBaud ? rs485BaudTracker.configuredBaud : 9600UL;
	const char *syncLabel = rs485BaudSyncStateLabel(rs485BaudTracker.syncState);
	char syncEscaped[24] = "";
	htmlEscapeInto(syncLabel, syncEscaped, sizeof(syncEscaped));
	refreshPortalUpdateCsrfToken();

	auto emitBaudOption = [&](PortalResponseWriter &writer, uint32_t baud) -> bool {
		const char *label = nullptr;
		if (!portalRs485BaudLabel(baud, &label) || label == nullptr) {
			return false;
		}
		const bool checked = selectedBaud == baud;
		char line[128];
		const int written = snprintf(line,
		                             sizeof(line),
		                             "<p><label><input type=\"radio\" name=\"baud\" value=\"%lu\"%s> %s</label></p>",
		                             static_cast<unsigned long>(baud),
		                             checked ? " checked" : "",
		                             label);
		return written > 0 && static_cast<size_t>(written) < sizeof(line) && writer.write(line);
	};

	auto emitPage = [&](PortalResponseWriter &writer) -> bool {
		const PortalUiMode uiMode =
			(currentBootMode == BootMode::WifiConfig) ? PortalUiMode::Wifi : PortalUiMode::Ap;
		if (!writePortalUiPageStartP(writer, PSTR("Alpha2MQTT RS485"), PSTR("RS485"), uiMode)) {
			return false;
		}
		if (saved && !writer.writeP(PSTR("<p><strong>Saved.</strong> Reboot Normal is not required; runtime will reconcile on the next live RS485 epoch.</p>"))) {
			return false;
		}
		if (err && !writer.writeP(PSTR("<p><strong>Invalid RS485 baud.</strong> Choose 9600, 115200, or 19200.</p>"))) {
			return false;
		}
		if (!writePortalMenuButtonP(writer, PSTR("/"), PSTR("Menu"), PSTR("get")) ||
		    !writePortalMenuButtonP(writer, PSTR("/config/mqtt"), PSTR("MQTT Setup"), PSTR("get")) ||
		    !writePortalMenuButtonP(writer, PSTR("/config/polling"), PSTR("Polling"), PSTR("get")) ||
		    !writePortalMenuButtonP(
			    writer, PSTR("/config/reboot-normal"), PSTR("Reboot Normal"), PSTR("post")) ||
		    !writer.writeP(PSTR("<p>Configured baud is persisted as a numeric target and reconciled once per live RS485 connection epoch.</p>"))) {
			return false;
		}
		char statusLine[196];
		const int statusWritten = snprintf(
			statusLine,
			sizeof(statusLine),
			"<p>Configured: %lu<br>Live: %lu<br>Sync: %s</p>",
			static_cast<unsigned long>(rs485BaudTracker.hasConfiguredBaud ? rs485BaudTracker.configuredBaud : 0),
			static_cast<unsigned long>(rs485BaudTracker.actualBaud),
			syncEscaped);
		if (statusWritten <= 0 || static_cast<size_t>(statusWritten) >= sizeof(statusLine) ||
		    !writer.write(statusLine) ||
		    !writer.writeP(PSTR("<form method=\"POST\" action=\"/config/rs485/save\">"
		                        "<input type=\"hidden\" name=\"csrf\" value=\"")) ||
		    !writer.write(portalUpdateCsrfToken) ||
		    !writer.writeP(PSTR("\">"))) {
			return false;
		}
		if (!emitBaudOption(writer, 9600UL) || !emitBaudOption(writer, 115200UL) ||
		    !emitBaudOption(writer, 19200UL) ||
		    !writer.writeP(PSTR("<p><button type=\"submit\">Save</button></p></form></body></html>"))) {
			return false;
		}
		return true;
	};
	(void)sendPortalHtmlResponse(wifiManager.server.get(), emitPage, "rs485 config unavailable");
}

static void
handlePortalRs485Save(WiFiManager& wifiManager)
{
	if (!wifiManager.server) {
		return;
	}
	if (!portalUpdateRequestHasValidToken(&wifiManager)) {
		wifiManager.server->send(403, "text/plain", "invalid csrf");
		return;
	}

	const String baudArg = wifiManager.server->arg("baud");
	char *endPtr = nullptr;
	const unsigned long parsedBaud = strtoul(baudArg.c_str(), &endPtr, 10);
	if (baudArg.length() == 0 || endPtr == nullptr || *endPtr != '\0' ||
	    !rs485BaudValueSupported(parsedBaud)) {
		wifiManager.server->sendHeader("Location", "/config/rs485?err=1");
		wifiManager.server->send(302, "text/plain", "");
		return;
	}

	if (!persistConfiguredRs485Baud(parsedBaud)) {
		wifiManager.server->sendHeader("Location", "/config/rs485?err=1");
		wifiManager.server->send(302, "text/plain", "");
		return;
	}

	rs485BaudTracker.configuredBaud = parsedBaud;
	rs485BaudTracker.hasConfiguredBaud = true;
	if (rs485BaudTracker.actualBaud == parsedBaud) {
		rs485BaudTracker.syncState = Rs485BaudSyncState::Synced;
	} else if (rs485BaudTracker.actualBaud != 0) {
		rs485BaudTracker.syncState = Rs485BaudSyncState::Mismatch;
	} else {
		rs485BaudTracker.syncState = Rs485BaudSyncState::Unknown;
	}
	rs485BaudNextActionAtMs = millis();

	wifiManager.server->sendHeader("Location", "/config/rs485?saved=1");
	wifiManager.server->send(302, "text/plain", "");
}

static void
handlePortalParamPage(WiFiManager& wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	char portValue[8] = "";
	snprintf(portValue, sizeof(portValue), "%d", appConfig.mqttPort);

	const bool saved = wifiManager.server->hasArg("saved");
	const bool err = wifiManager.server->hasArg("err");

	char escaped[6 * kPrefMqttPasswordMaxLen] = "";
	static const char kSaved[] PROGMEM = "<p><strong>Saved.</strong></p>";
	static const char kErr[] PROGMEM = "<p><strong>Invalid MQTT values.</strong></p>";
	static const char kTail[] PROGMEM =
		"<p><button type=\"submit\">Save</button></p></form></body></html>";
	auto emitPage = [&](PortalResponseWriter &writer) -> bool {
		const PortalUiMode uiMode =
			(currentBootMode == BootMode::WifiConfig) ? PortalUiMode::Wifi : PortalUiMode::Ap;
		if (!writePortalUiPageStartP(writer, PSTR("Alpha2MQTT MQTT Setup"), PSTR("MQTT Setup"), uiMode)) {
			return false;
		}
		if (saved && !writer.writeP(kSaved)) {
			return false;
		}
		if (err && !writer.writeP(kErr)) {
			return false;
		}
		if (!writePortalMenuButtonP(writer, PSTR("/"), PSTR("Menu"), PSTR("get")) ||
		    !writePortalMenuButtonP(writer, PSTR("/config/rs485"), PSTR("RS485"), PSTR("get")) ||
		    !writePortalMenuButtonP(writer, PSTR("/config/polling"), PSTR("Polling"), PSTR("get")) ||
		    !writePortalMenuButtonP(
			    writer, PSTR("/config/reboot-normal"), PSTR("Reboot Normal"), PSTR("post")) ||
		    !writer.writeP(PSTR("<form method=\"POST\" action=\"/config/mqtt/save\">"))) {
			return false;
		}
		htmlEscapeInto(appConfig.mqttSrvr.c_str(), escaped, sizeof(escaped));
		if (!writer.writeP(PSTR("<p>MQTT server<br><input name=\"server\" value=\"")) ||
		    !writer.write(escaped) ||
		    !writer.writeP(PSTR("\" maxlength=\"63\"></p>"))) {
			return false;
		}
		if (!writer.writeP(
			    PSTR("<p>MQTT port<br><input name=\"port\" type=\"number\" min=\"0\" max=\"32767\" value=\"")) ||
		    !writer.write(portValue) ||
		    !writer.writeP(PSTR("\"></p>"))) {
			return false;
		}
		htmlEscapeInto(appConfig.mqttUser.c_str(), escaped, sizeof(escaped));
		if (!writer.writeP(PSTR("<p>MQTT user<br><input name=\"user\" value=\"")) ||
		    !writer.write(escaped) ||
		    !writer.writeP(PSTR("\" maxlength=\"63\"></p>"))) {
			return false;
		}
		htmlEscapeInto(appConfig.mqttPass.c_str(), escaped, sizeof(escaped));
		if (!writer.writeP(PSTR("<p>MQTT password<br><input name=\"mpass\" type=\"password\" value=\"")) ||
		    !writer.write(escaped) ||
		    !writer.writeP(PSTR("\" maxlength=\"63\"></p>"))) {
			return false;
		}
		htmlEscapeInto(appConfig.inverterLabel.c_str(), escaped, sizeof(escaped));
		if (!writer.writeP(PSTR("<p>Inverter label<br><input name=\"inverter_label\" value=\"")) ||
		    !writer.write(escaped) ||
		    !writer.writeP(PSTR("\" maxlength=\"10\"></p>"))) {
			return false;
		}
		return writer.writeP(kTail);
	};
	(void)sendPortalHtmlResponse(wifiManager.server.get(), emitPage, "mqtt setup unavailable");
}

static void
handlePortalParamSave(WiFiManager& wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	const String server = wifiManager.server->arg("server");
	const String portArg = wifiManager.server->arg("port");
	const String user = wifiManager.server->arg("user");
	const String pass = wifiManager.server->arg("mpass");
	const String label = wifiManager.server->arg("inverter_label");
	const long parsedPort = strtol(portArg.c_str(), nullptr, 10);
	const bool portValid = portArg.length() > 0 && parsedPort >= 0 && parsedPort <= SHRT_MAX;

	if ((portArg.length() > 0 && !portValid) || !inverterLabelOverrideIsValid(label.c_str())) {
		wifiManager.server->sendHeader("Location", "/config/mqtt?err=1");
		wifiManager.server->send(302, "text/plain", "");
		return;
	}

	appConfig.mqttSrvr = server;
	appConfig.mqttPort = portValid ? static_cast<int>(parsedPort) : 0;
	appConfig.mqttUser = user;
	appConfig.mqttPass = pass;
	appConfig.inverterLabel = label;
	persistUserMqttConfig(appConfig.mqttSrvr.c_str(), appConfig.mqttPort, appConfig.mqttUser.c_str(), appConfig.mqttPass.c_str());
	persistUserInverterLabel(appConfig.inverterLabel.c_str());
	refreshPortalCustomParameters();

	mqttConfigComplete = isMqttConfigComplete();
	mqttRuntimeEnabled = bootPlan.mqtt && mqttConfigComplete;
	portalMqttSaved = true;
	portalNeedsMqttConfig = !mqttConfigComplete;
	const bool rebootScheduled =
		portalMqttSaved && !portalNeedsMqttConfig && portalHasPersistedWifiCredentials();
	if (rebootScheduled) {
		schedulePortalReboot(portalNormalRebootIntent());
	}

	if (rebootScheduled) {
		// Once MQTT save completes the STA portal configuration, the user-facing
		// next step is the same-origin reboot back to normal runtime.
		(void)sendRebootHandoffPage(wifiManager.server.get(), portalNormalRebootIntent());
#if defined(MP_ESP8266)
		ESP.wdtFeed();
#endif
		return;
	}

	wifiManager.server->sendHeader("Location", "/config/mqtt?saved=1");
	wifiManager.server->send(302, "text/plain", "");
}

void
handlePortalStatusRequest(WiFiManager& wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	struct PortalStatusSnapshot {
		PortalStatus status;
		bool needsMqttConfig;
		BootIntent bootIntent;
		BootMode bootMode;
		BootMode diagnosticsMode;
		WiFiMode_t wifiMode;
		wl_status_t staStatus;
		IPAddress apIp;
		int lastDisconnectReason;
		int rssi;
		int channel;
		unsigned int freeHeap;
		unsigned int maxFreeBlock;
		unsigned int heapFrag;
		unsigned long uptimeMs;
		char statusReason[sizeof(portalStatusReason)];
		char statusSsid[sizeof(portalStatusSsid)];
		char statusIp[sizeof(portalStatusIp)];
		char targetSsid[kPrefWifiSsidMaxLen + 1];
		char resetReason[sizeof(lastResetReason)];
		char disconnectLabel[sizeof(portalLastDisconnectLabel)];
	};

	PortalStatusSnapshot snapshot{};
	snapshot.status = portalStatus;
	snapshot.needsMqttConfig = portalNeedsMqttConfig;
	snapshot.bootIntent = currentBootIntent;
	snapshot.bootMode = currentBootMode;
	snapshot.diagnosticsMode = bootModeForDiagnostics;
	snapshot.wifiMode = WiFi.getMode();
	snapshot.staStatus = WiFi.status();
	snapshot.apIp = WiFi.softAPIP();
	snapshot.lastDisconnectReason = portalLastDisconnectReason;
	snapshot.rssi = WiFi.RSSI();
	snapshot.channel = WiFi.channel();
#if defined(MP_ESP8266)
	snapshot.freeHeap = ESP.getFreeHeap();
	snapshot.maxFreeBlock = ESP.getMaxFreeBlockSize();
	snapshot.heapFrag = ESP.getHeapFragmentation();
#else
	snapshot.freeHeap = ESP.getFreeHeap();
	snapshot.maxFreeBlock = 0;
	snapshot.heapFrag = 0;
#endif
	snapshot.uptimeMs = millis();
	strlcpy(snapshot.statusReason, portalStatusReason, sizeof(snapshot.statusReason));
	strlcpy(snapshot.statusSsid, portalStatusSsid, sizeof(snapshot.statusSsid));
	strlcpy(snapshot.statusIp, portalStatusIp, sizeof(snapshot.statusIp));
	strlcpy(snapshot.targetSsid,
	        appConfig.wifiSSID.length() > 0 ? appConfig.wifiSSID.c_str() : portalStatusSsid,
	        sizeof(snapshot.targetSsid));
	strlcpy(snapshot.resetReason, lastResetReason, sizeof(snapshot.resetReason));
	strlcpy(snapshot.disconnectLabel, portalLastDisconnectLabel, sizeof(snapshot.disconnectLabel));

	char buf[256];
	static const char kRefreshHead[] PROGMEM = "<meta http-equiv=\"refresh\" content=\"1\">";
	static const char kDiagOpen[] PROGMEM = "<h3>Diagnostics</h3><p>";
	static const char kTail[] PROGMEM =
		"<form method=\"POST\" action=\"/config/reboot-normal\"><button type=\"submit\">Reboot Normal</button></form>"
		"<p>Page refreshes every second.</p></body></html>";
	static const char kStatusSuccessFmt[] PROGMEM =
		"<p><strong>WiFi Status: %s</strong><br>SSID: %s<br>IP: %s</p>";
	static const char kSavedWifiFmt[] PROGMEM = "<p>Saved WiFi for next reboot: %s</p>";
	static const char kStatusFailedFmt[] PROGMEM =
		"<p><strong>WiFi Status: %s</strong><br>Reason: %s</p>";
	static const char kStatusOnlyFmt[] PROGMEM = "<p><strong>WiFi Status: %s</strong></p>";
	static const char kSoftApFmt[] PROGMEM = "<br>SoftAP SSID: %s<br>SoftAP IP: %u.%u.%u.%u";
	static const char kStaStatusFmt[] PROGMEM = "<br>STA status: %s (%d)";
	static const char kTargetSsidFmt[] PROGMEM = "<br>Target SSID: %s";
	static const char kBootModeFmt[] PROGMEM = "<br>Boot intent: %s<br>Boot mode: %s";
	static const char kFirmwareFmt[] PROGMEM = "<br>Firmware version: %s<br>Reset reason: %s";
	static const char kDisconnectFmt[] PROGMEM = "<br>Last disconnect: %s (%d)";
	static const char kRssiFmt[] PROGMEM = "<br>RSSI: %d dBm<br>Channel: %d";
#if defined(MP_ESP8266)
	static const char kHeapFmt[] PROGMEM = "<br>Heap: free=%u max=%u frag=%u";
#else
	static const char kHeapFmt[] PROGMEM = "<br>Heap free=%u";
#endif
	static const char kUptimeFmt[] PROGMEM = "<br>Uptime (ms): %lu</p>";
	auto emitPage = [&](PortalResponseWriter &writer) -> bool {
		const PortalUiMode uiMode =
			(currentBootMode == BootMode::WifiConfig) ? PortalUiMode::Wifi : PortalUiMode::Ap;
		if (!writePortalUiPageStartP(
			    writer, PSTR("Alpha2MQTT WiFi Status"), PSTR("WiFi Status"), uiMode, kRefreshHead)) {
			return false;
		}

		if (snapshot.status == portalStatusSuccess) {
			snprintf_P(buf,
			           sizeof(buf),
			           kStatusSuccessFmt,
			           portalStatusLabel(snapshot.status),
			           snapshot.statusSsid,
			           snapshot.statusIp);
			if (!writer.write(buf)) {
				return false;
			}
			if (snapshot.bootMode == BootMode::WifiConfig && appConfig.wifiSSID.length() > 0 &&
			    strcmp(snapshot.statusSsid, appConfig.wifiSSID.c_str()) != 0) {
				snprintf_P(buf, sizeof(buf), kSavedWifiFmt, appConfig.wifiSSID.c_str());
				if (!writer.write(buf)) {
					return false;
				}
			}
			if (snapshot.needsMqttConfig) {
				if (!writer.writeP(
				        PSTR("<p><strong>MQTT settings not set.</strong> Redirecting to MQTT settings...</p>")) ||
				    !writer.writeP(PSTR("<p><a href=\"/config/mqtt\">Open MQTT settings</a></p>")) ||
				    !writer.writeP(
					    PSTR("<script>setTimeout(function(){window.location.href='/config/mqtt';},500);</script>"))) {
					return false;
				}
			}
		} else if (snapshot.status == portalStatusFailed) {
			snprintf_P(buf,
			           sizeof(buf),
			           kStatusFailedFmt,
			           portalStatusLabel(snapshot.status),
			           snapshot.statusReason);
			if (!writer.write(buf)) {
				return false;
			}
		} else {
			snprintf_P(buf, sizeof(buf), kStatusOnlyFmt, portalStatusLabel(snapshot.status));
			if (!writer.write(buf)) {
				return false;
			}
			if (snapshot.bootMode == BootMode::WifiConfig) {
				if (!writer.writeP(PSTR("<p>Portal active. WiFi changes apply on reboot.</p>"))) {
					return false;
				}
			} else {
				if (!writer.writeP(PSTR("<p>Attempting to connect...</p>"))) {
					return false;
				}
			}
		}

		if (!writer.writeP(kDiagOpen)) {
			return false;
		}
		snprintf(buf, sizeof(buf), "Mode: %s", wifiModeLabel(snapshot.wifiMode));
		if (!writer.write(buf)) {
			return false;
		}

		// Only show SoftAP info when it is actually enabled; MODE_WIFI_CONFIG uses STA-only portal.
		if (snapshot.wifiMode == WIFI_AP || snapshot.wifiMode == WIFI_AP_STA) {
			snprintf_P(buf,
			           sizeof(buf),
			           kSoftApFmt,
			           deviceName,
			           snapshot.apIp[0],
			           snapshot.apIp[1],
			           snapshot.apIp[2],
			           snapshot.apIp[3]);
			if (!writer.write(buf)) {
				return false;
			}
		} else {
			if (!writer.writeP(PSTR("<br>SoftAP: disabled"))) {
				return false;
			}
		}

		snprintf_P(
			buf, sizeof(buf), kStaStatusFmt, wifiStatusLabel(snapshot.staStatus), static_cast<int>(snapshot.staStatus));
		if (!writer.write(buf)) {
			return false;
		}

		snprintf_P(buf, sizeof(buf), kTargetSsidFmt, snapshot.targetSsid);
		if (!writer.write(buf)) {
			return false;
		}

		snprintf_P(
			buf, sizeof(buf), kBootModeFmt, bootIntentToString(snapshot.bootIntent), bootModeToString(snapshot.diagnosticsMode));
		if (!writer.write(buf)) {
			return false;
		}

		snprintf_P(buf, sizeof(buf), kFirmwareFmt, _version, snapshot.resetReason);
		if (!writer.write(buf)) {
			return false;
		}

		snprintf_P(buf, sizeof(buf), kDisconnectFmt, snapshot.disconnectLabel, snapshot.lastDisconnectReason);
		if (!writer.write(buf)) {
			return false;
		}

		if (snapshot.staStatus == WL_CONNECTED) {
			snprintf_P(buf, sizeof(buf), kRssiFmt, snapshot.rssi, snapshot.channel);
			if (!writer.write(buf)) {
				return false;
			}
		}

#if defined(MP_ESP8266)
		snprintf_P(buf, sizeof(buf), kHeapFmt, snapshot.freeHeap, snapshot.maxFreeBlock, snapshot.heapFrag);
#else
		snprintf_P(buf, sizeof(buf), kHeapFmt, snapshot.freeHeap);
#endif
		if (!writer.write(buf)) {
			return false;
		}

		snprintf_P(buf, sizeof(buf), kUptimeFmt, snapshot.uptimeMs);
		if (!writer.write(buf)) {
			return false;
		}
		return writer.writeP(kTail);
	};
	(void)sendPortalHtmlResponse(wifiManager.server.get(), emitPage, "status unavailable");
}

static void
handlePortalUpdatePage(WiFiManager &wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	refreshPortalUpdateCsrfToken();

	static const char kTail[] PROGMEM =
		"<p><input type=\"file\" name=\"firmware\" id=\"firmware\"></p>"
		"<p><button type=\"submit\">Upload Firmware</button></p>"
		"</form>"
		"</body></html>";
	static const char kFormOpenFmt[] PROGMEM =
		"<form id=\"ota-form\" method=\"POST\" enctype=\"multipart/form-data\" action=\"/config/update?csrf=%s\">";
	auto emitPage = [&](PortalResponseWriter &writer) -> bool {
		const PortalUiMode uiMode =
			(currentBootMode == BootMode::WifiConfig) ? PortalUiMode::Wifi : PortalUiMode::Ap;
		if (!writePortalUiPageStartP(writer, PSTR("Alpha2MQTT OTA Update"), PSTR("OTA Update"), uiMode) ||
		    !writer.writeP(PSTR("<p>Upload a firmware binary. The device reboots automatically after a successful update.</p>")) ||
		    !writePortalMenuButtonP(writer, PSTR("/"), PSTR("Menu"), PSTR("get")) ||
		    !writePortalMenuButtonP(
			    writer, PSTR("/config/reboot-normal"), PSTR("Reboot Normal"), PSTR("post"))) {
			return false;
		}
		// ESP8266WebServer buffers non-form POST bodies into a String before dispatch. Keep OTA on
		// multipart upload so the parser streams chunks through the upload callback instead.
		char formOpen[200];
		const int written = snprintf_P(
			formOpen, sizeof(formOpen), kFormOpenFmt, portalUpdateCsrfToken);
		if (written <= 0 || static_cast<size_t>(written) >= sizeof(formOpen) || !writer.write(formOpen)) {
			return false;
		}
		return writer.writeP(kTail);
	};
	portalUpdateUploadStarted = false;
	(void)sendPortalHtmlResponse(wifiManager.server.get(), emitPage, "update unavailable");
}

static void
handlePortalUpdateUpload(WiFiManager &wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	if (!portalUpdateRequestHasValidToken(&wifiManager)) {
		return;
	}

	HTTPUpload &upload = wifiManager.server->upload();
	switch (upload.status) {
	case UPLOAD_FILE_START: {
#ifdef DEBUG_OVER_SERIAL
		portalLog("OTA upload start: %s total=%u free=%u max=%u frag=%u",
		          upload.filename.c_str(),
		          static_cast<unsigned>(upload.totalSize),
		          ESP.getFreeHeap(),
		          ESP.getMaxFreeBlockSize(),
		          ESP.getHeapFragmentation());
#endif
		portalUpdateUploadStarted = true;
		WiFiUDP::stopAll();
		const uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000U) & 0xFFFFF000U;
		if (!Update.begin(maxSketchSpace)) {
#ifdef DEBUG_OVER_SERIAL
			Update.printError(Serial);
#endif
		}
		break;
	}
	case UPLOAD_FILE_WRITE:
#ifdef DEBUG_OVER_SERIAL
		if ((upload.totalSize % 32768U) == 0U) {
			portalLog("OTA upload write: current=%u total=%u free=%u max=%u frag=%u",
			          static_cast<unsigned>(upload.currentSize),
			          static_cast<unsigned>(upload.totalSize),
			          ESP.getFreeHeap(),
			          ESP.getMaxFreeBlockSize(),
			          ESP.getHeapFragmentation());
		}
#endif
		if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
#ifdef DEBUG_OVER_SERIAL
			Update.printError(Serial);
#endif
		}
#if defined(MP_ESP8266)
		ESP.wdtFeed();
#endif
		break;
	case UPLOAD_FILE_END:
#ifdef DEBUG_OVER_SERIAL
		portalLog("OTA upload end: size=%u free=%u max=%u frag=%u",
		          static_cast<unsigned>(upload.totalSize),
		          ESP.getFreeHeap(),
		          ESP.getMaxFreeBlockSize(),
		          ESP.getHeapFragmentation());
#endif
		if (!Update.end(true)) {
#ifdef DEBUG_OVER_SERIAL
			Update.printError(Serial);
#endif
		}
		break;
	case UPLOAD_FILE_ABORTED:
#ifdef DEBUG_OVER_SERIAL
		portalLog("OTA upload aborted");
#endif
		portalUpdateUploadStarted = false;
		if (Update.isRunning()) {
			Update.end();
		}
		break;
	default:
		break;
	}
}

static void
handlePortalUpdatePost(WiFiManager &wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	if (!portalUpdateRequestHasValidToken(&wifiManager)) {
		wifiManager.server->sendHeader("Connection", "close");
		wifiManager.server->send(403,
		                         "text/html",
		                         "<!DOCTYPE html><html><body><h2>Forbidden.</h2><p>Reload the update page and try again.</p></body></html>");
		return;
	}

	const bool ok = portalUpdateUploadStarted && Update.isFinished() && !Update.hasError();
#ifdef DEBUG_OVER_SERIAL
	portalLog("OTA post: started=%d finished=%d running=%d error=%d free=%u max=%u frag=%u",
	          portalUpdateUploadStarted ? 1 : 0,
	          Update.isFinished() ? 1 : 0,
	          Update.isRunning() ? 1 : 0,
	          Update.hasError() ? 1 : 0,
	          ESP.getFreeHeap(),
	          ESP.getMaxFreeBlockSize(),
	          ESP.getHeapFragmentation());
#endif
	if (!ok) {
		wifiManager.server->sendHeader("Connection", "close");
		wifiManager.server->send(500,
		                         "text/html",
		                         "<!DOCTYPE html><html><body><h2>Update failed.</h2><p>Check serial output for details.</p></body></html>");
		return;
	}
#ifdef DEBUG_OVER_SERIAL
	portalLog("OTA post: rebooting to normal runtime after successful update");
#endif
	// OTA success remains on the same portal origin, so it should use the same
	// reboot handoff contract as the explicit reboot actions.
	(void)sendRebootHandoffPage(wifiManager.server.get(), portalNormalRebootIntent());
#if defined(MP_ESP8266)
	ESP.wdtFeed();
#endif
	portalNeedsMqttConfig = false;
	schedulePortalReboot(portalNormalRebootIntent());
}

void
handlePortalRestartRequest(WiFiManager& wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	(void)sendRebootHandoffPage(wifiManager.server.get(), portalRestartIntent());
#if defined(MP_ESP8266)
	ESP.wdtFeed();
#endif
	schedulePortalReboot(portalRestartIntent());
}

void
handlePortalRebootNormalRequest(WiFiManager& wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	(void)sendRebootHandoffPage(wifiManager.server.get(), portalNormalRebootIntent());
#if defined(MP_ESP8266)
	ESP.wdtFeed();
#endif
	portalNeedsMqttConfig = false;
	schedulePortalReboot(portalNormalRebootIntent());
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

struct ScopedEntityCatalogCopy {
	mqttState *entities = nullptr;
	size_t count = 0;

	~ScopedEntityCatalogCopy()
	{
		delete[] entities;
	}

	bool load()
	{
		count = mqttEntitiesCount();
		if (count == 0) {
			return false;
		}
		entities = new (std::nothrow) mqttState[count];
		if (entities == nullptr) {
			return false;
		}
		if (!mqttEntityCopyCatalog(entities, count)) {
			delete[] entities;
			entities = nullptr;
			count = 0;
			return false;
		}
		return true;
	}
};

static bool
queuePendingPollingConfigPayload(const char *src, size_t length)
{
	if (src == nullptr || length == 0 || length >= kPollingConfigSetPayloadMaxLen) {
		return false;
	}
	char *buffer = new (std::nothrow) char[length + 1];
	if (buffer == nullptr) {
		return false;
	}
	memcpy(buffer, src, length);
	buffer[length] = '\0';
	delete[] pendingPollingConfigPayload;
	pendingPollingConfigPayload = buffer;
	return true;
}

static void
clearPendingPollingConfigPayload(void)
{
	delete[] pendingPollingConfigPayload;
	pendingPollingConfigPayload = nullptr;
}

static void
processPendingPollingConfigPayload(void)
{
	if (!pendingPollingConfigSet) {
		return;
	}
	pendingPollingConfigSet = false;
	if (pendingPollingConfigPayload == nullptr) {
		return;
	}
#ifdef DEBUG_OVER_SERIAL
	Serial.printf("config/set dequeue: %s\r\n", pendingPollingConfigPayload);
#endif
	handlePollingConfigSet(pendingPollingConfigPayload);
	clearPendingPollingConfigPayload();
}

static void
invalidatePortalRouteBinding(const char *reason)
{
#ifdef DEBUG_OVER_SERIAL
	if (portalRoutesBoundServer != nullptr) {
		portalLog("Invalidating portal route binding (%s) previous_server=%p",
		          reason ? reason : "unspecified",
		          portalRoutesBoundServer);
	}
#endif
	portalRoutesBoundServer = nullptr;
}

static void
bindPortalRoutes(WiFiManager &wifiManager)
{
	if (!wifiManager.server) {
		return;
	}
	// During AP onboarding WiFiManager may swap internal web-server state while
	// handling /wifisave. Rebinding our custom routes in that window has caused
	// ESP8266 exceptions from std::function route-table churn. Keep the existing
	// AP routes stable until the connect attempt either succeeds or fails.
	if (currentBootMode != BootMode::WifiConfig && portalStatus == portalStatusConnecting &&
	    portalRoutesBoundServer != nullptr) {
		return;
	}
	if (portalRoutesBoundServer == static_cast<void *>(wifiManager.server.get())) {
		return;
	}
	portalRoutesBoundServer = static_cast<void *>(wifiManager.server.get());
#ifdef DEBUG_OVER_SERIAL
	portalLog("Binding portal routes to server=%p", portalRoutesBoundServer);
#endif
	wifiManager.server->on("/", HTTP_GET, [&]() {
		notePortalActivity();
		handlePortalMenuPage(wifiManager);
	});
	wifiManager.server->on("/restart", HTTP_GET, [&]() {
		notePortalActivity();
		handlePortalRestartRequest(wifiManager);
	});
	wifiManager.server->on("/restart/", HTTP_GET, [&]() {
		notePortalActivity();
		handlePortalRestartRequest(wifiManager);
	});
	wifiManager.server->on("/status", [&]() {
		notePortalActivity();
#ifdef DEBUG_OVER_SERIAL
		portalLog("route hit: /status");
#endif
		handlePortalStatusRequest(wifiManager);
	});
	wifiManager.server->on("/config/polling", HTTP_GET, [&]() {
		notePortalActivity();
		handlePortalPollingPage(wifiManager);
	});
	wifiManager.server->on("/config/polling/export", HTTP_GET, [&]() {
		notePortalActivity();
		handlePortalPollingExport(wifiManager);
	});
	wifiManager.server->on("/config/polling/import", HTTP_GET, [&]() {
		notePortalActivity();
		handlePortalPollingImportPage(wifiManager);
	});
	wifiManager.server->on(
		"/config/polling/import",
		HTTP_POST,
		[&]() {
			notePortalActivity();
			handlePortalPollingImport(wifiManager);
		},
		[&]() {
			notePortalActivity();
			handlePortalPollingImportUpload(wifiManager);
		});
	wifiManager.server->on("/config/polling/reset", HTTP_POST, [&]() {
		notePortalActivity();
		handlePortalPollingReset(wifiManager);
	});
	wifiManager.server->on("/0wifi", HTTP_GET, [&]() {
		notePortalActivity();
#ifdef DEBUG_OVER_SERIAL
		portalLog("route hit: /0wifi");
#endif
		handlePortalWifiPage(wifiManager);
	});
	wifiManager.server->on("/wifi", HTTP_GET, [&]() {
		notePortalActivity();
#ifdef DEBUG_OVER_SERIAL
		portalLog("route hit: /wifi");
#endif
		handlePortalWifiPage(wifiManager);
	});
	if (currentBootMode == BootMode::WifiConfig) {
		wifiManager.server->on("/wifisave", HTTP_POST, [&]() {
			notePortalActivity();
			handlePortalWifiSave(wifiManager);
		});
	}
	wifiManager.server->on("/config/wifi/save", HTTP_POST, [&]() {
		notePortalActivity();
		handlePortalWifiSave(wifiManager);
	});
	wifiManager.server->on("/config/mqtt", HTTP_GET, [&]() {
		notePortalActivity();
#ifdef DEBUG_OVER_SERIAL
		portalLog("route hit: /config/mqtt");
#endif
		handlePortalParamPage(wifiManager);
	});
	wifiManager.server->on("/config/rs485", HTTP_GET, [&]() {
		notePortalActivity();
		handlePortalRs485Page(wifiManager);
	});
	wifiManager.server->on("/param", HTTP_GET, [&]() {
		notePortalActivity();
		handlePortalParamPage(wifiManager);
	});
	wifiManager.server->on("/config/mqtt/save", HTTP_POST, [&]() {
		notePortalActivity();
		handlePortalParamSave(wifiManager);
	});
	wifiManager.server->on("/config/rs485/save", HTTP_POST, [&]() {
		notePortalActivity();
		handlePortalRs485Save(wifiManager);
	});
	wifiManager.server->on("/paramsave", HTTP_POST, [&]() {
		notePortalActivity();
		handlePortalParamSave(wifiManager);
	});
	wifiManager.server->on("/config/update", HTTP_GET, [&]() {
		notePortalActivity();
		handlePortalUpdatePage(wifiManager);
	});
	wifiManager.server->on("/update", HTTP_GET, [&]() {
		notePortalActivity();
		handlePortalUpdatePage(wifiManager);
	});
	wifiManager.server->on("/config/update", HTTP_POST, [&]() {
		notePortalActivity();
		handlePortalUpdatePost(wifiManager);
	}, [&]() {
		notePortalActivity();
		handlePortalUpdateUpload(wifiManager);
	});
	wifiManager.server->on("/u", HTTP_POST, [&]() {
		notePortalActivity();
		handlePortalUpdatePost(wifiManager);
	}, [&]() {
		notePortalActivity();
		handlePortalUpdateUpload(wifiManager);
	});
	wifiManager.server->on("/config/polling/save", HTTP_POST, [&]() {
		notePortalActivity();
		handlePortalPollingSave(wifiManager);
	});
	wifiManager.server->on("/config/polling/clear", HTTP_POST, [&]() {
		notePortalActivity();
		handlePortalPollingClear(wifiManager);
	});
	wifiManager.server->on("/config/reboot-normal", HTTP_POST, [&]() {
		notePortalActivity();
		handlePortalRebootNormalRequest(wifiManager);
	});
	wifiManager.server->on("/config/reboot-normal/", HTTP_POST, [&]() {
		notePortalActivity();
		handlePortalRebootNormalRequest(wifiManager);
	});
}

static void
schedulePortalRouteRebindRetries(void)
{
	portalRouteRebindRetriesRemaining = 6;
	portalRouteRebindRetryAt = millis() + 250;
}

static void
servicePortalRouteRebindRetries(WiFiManager &wifiManager)
{
	if (portalRouteRebindRetriesRemaining == 0) {
		return;
	}
	if (static_cast<long>(millis() - portalRouteRebindRetryAt) < 0) {
		return;
	}
	invalidatePortalRouteBinding("post-connect-retry");
	bindPortalRoutes(wifiManager);
	portalRouteRebindRetriesRemaining--;
	portalRouteRebindRetryAt = millis() + 500;
}

static void
ensurePortalPollingRuntimeReady(void)
{
	(void)ensurePortalBucketsScratch();
	initMqttEntitiesRtIfNeeded(true);
	if (currentBootMode != BootMode::ApConfig && currentBootMode != BootMode::WifiConfig) {
		loadPollingConfig();
	}
}

static void
preparePortalPollingRuntime(void)
{
	// Portal-mode contract:
	// - AP/WiFi portal handlers must not re-read polling prefs inline on ESP8266.
	// - Portal edits persist schedule changes, but normal-runtime application is
	//   a separate step that happens after Reboot Normal.
	// Load the saved schedule once during portal startup so cache/page handlers
	// can reuse initialized buckets without request-time NVS churn.
	ensurePortalPollingRuntimeReady();
	loadPollingConfig();
}

static void
refreshPortalCustomParameters(void)
{
	char mqttPortValue[8];
	snprintf(mqttPortValue, sizeof(mqttPortValue), "%d", appConfig.mqttPort);
	gPortalMqttServer.setValue(appConfig.mqttSrvr.c_str(), 40);
	gPortalMqttPort.setValue(mqttPortValue, 6);
	gPortalMqttUser.setValue(appConfig.mqttUser.c_str(), 32);
	gPortalMqttPass.setValue(appConfig.mqttPass.c_str(), 32);
	gPortalInverterLabel.setValue(appConfig.inverterLabel.c_str(), kPrefInverterLabelMaxLen);
}

static bool
isWifiConfigComplete(void)
{
	return appConfig.wifiSSID != "";
}

static bool
isMqttConfigComplete(void)
{
	return appConfig.mqttSrvr != "" && appConfig.mqttPort != 0;
}

static bool
mqttSubsystemEnabled(void)
{
	return bootPlan.mqtt && mqttRuntimeEnabled;
}

static bool
portalResolveLegacyBucketToken(const char *token, size_t &resolvedIndex)
{
	if (token == nullptr || token[0] != '#') {
		return false;
	}

	char *endPtr = nullptr;
	errno = 0;
	const unsigned long parsed = strtoul(token + 1, &endPtr, 10);
	if (errno != 0 || endPtr == token + 1 || *endPtr != '\0') {
		return false;
	}

	static constexpr mqttEntityId kLegacyBucketMapOrder[] = {
#ifdef DEBUG_FREEMEM
		mqttEntityId::entityFreemem,
#endif
#ifdef DEBUG_CALLBACKS
		mqttEntityId::entityCallbacks,
#endif
#ifdef DEBUG_RS485
		mqttEntityId::entityRs485Errors,
#endif
#ifdef A2M_DEBUG_WIFI
		mqttEntityId::entityRSSI,
		mqttEntityId::entityBSSID,
		mqttEntityId::entityTxPower,
		mqttEntityId::entityWifiRecon,
#endif
		mqttEntityId::entityRs485Avail,
		mqttEntityId::entityA2MUptime,
		mqttEntityId::entityA2MVersion,
		mqttEntityId::entityInverterVersion,
		mqttEntityId::entityInverterSn,
		mqttEntityId::entityEmsVersion,
		mqttEntityId::entityEmsSn,
		mqttEntityId::entityBatSoc,
		mqttEntityId::entityBatPwr,
		mqttEntityId::entityBatEnergyCharge,
		mqttEntityId::entityBatEnergyDischarge,
		mqttEntityId::entityGridAvail,
		mqttEntityId::entityGridPwr,
		mqttEntityId::entityGridEnergyTo,
		mqttEntityId::entityGridEnergyFrom,
		mqttEntityId::entityPvPwr,
		mqttEntityId::entityPvEnergy,
		mqttEntityId::entityFrequency,
		mqttEntityId::entityOpMode,
		mqttEntityId::entitySocTarget,
		mqttEntityId::entityChargePwr,
		mqttEntityId::entityDischargePwr,
		mqttEntityId::entityPushPwr,
		mqttEntityId::entityBatCap,
		mqttEntityId::entityBatTemp,
		mqttEntityId::entityInverterTemp,
		mqttEntityId::entityBatFaults,
		mqttEntityId::entityBatWarnings,
		mqttEntityId::entityInverterFaults,
		mqttEntityId::entityInverterWarnings,
		mqttEntityId::entitySystemFaults,
		mqttEntityId::entityInverterMode,
		mqttEntityId::entityGridReg,
		mqttEntityId::entityRegNum,
		mqttEntityId::entityRegValue,
	};

	if (parsed >= (sizeof(kLegacyBucketMapOrder) / sizeof(kLegacyBucketMapOrder[0]))) {
		return false;
	}
	return mqttEntityIndexById(kLegacyBucketMapOrder[parsed], &resolvedIndex);
}

static bool
portalApplyLegacyBucketMapString(const char *map,
                                 size_t entityCount,
                                 BucketId *buckets,
                                 uint32_t &unknownEntityCount,
                                 uint32_t &invalidBucketCount,
                                 uint32_t &duplicateEntityCount)
{
	if (map == nullptr || *map == '\0' || buckets == nullptr || entityCount == 0 ||
	    entityCount > kMqttEntityDescriptorCount) {
		return false;
	}
	if (isDisableAllBucketMap(map)) {
		for (size_t i = 0; i < entityCount; ++i) {
			buckets[i] = BucketId::Disabled;
		}
		return true;
	}

	BucketId staged[kMqttEntityDescriptorCount];
	memcpy(staged, buckets, entityCount * sizeof(BucketId));
	uint8_t seen[kMqttEntityDescriptorCount];
	memset(seen, 0, sizeof(seen));

	const char *cursor = map;
	while (*cursor != '\0') {
		while (*cursor != '\0' && (*cursor == ';' || isspace(static_cast<unsigned char>(*cursor)))) {
			cursor++;
		}
		if (*cursor == '\0') {
			break;
		}

		char token[64] = {0};
		char bucketName[32] = {0};
		size_t tokenIdx = 0;
		size_t bucketIdx = 0;
		while (*cursor != '\0' && *cursor != '=' && *cursor != ';' && tokenIdx < sizeof(token) - 1) {
			token[tokenIdx++] = *cursor++;
		}
		token[tokenIdx] = '\0';
		if (*cursor != '=') {
			return false;
		}
		cursor++;
		while (*cursor != '\0' && *cursor != ';' && bucketIdx < sizeof(bucketName) - 1) {
			bucketName[bucketIdx++] = *cursor++;
		}
		bucketName[bucketIdx] = '\0';
		if (token[0] == '\0' || bucketName[0] == '\0') {
			return false;
		}

		size_t idx = 0;
		if (token[0] == '#') {
			if (!portalResolveLegacyBucketToken(token, idx)) {
				unknownEntityCount++;
				continue;
			}
		} else if (!portalResolveEntityToken(token, entityCount, idx)) {
			unknownEntityCount++;
			continue;
		}

		const BucketId bucket = bucketIdFromString(bucketName);
		if (bucket == BucketId::Unknown) {
			invalidBucketCount++;
			continue;
		}
		if (seen[idx]) {
			duplicateEntityCount++;
		}
		staged[idx] = bucket;
		seen[idx] = 1;
	}

	memcpy(buckets, staged, entityCount * sizeof(BucketId));
	return true;
}

static bool
loadPollingBucketsForPortal(const mqttState *entities,
                            size_t entityCount,
                            BucketId *outBuckets,
                            uint32_t &outPollIntervalSeconds)
{
	if (!ensurePortalBucketsScratch()) {
		return false;
	}
	if (!outBuckets || entityCount == 0 || entityCount > kMqttEntityDescriptorCount) {
		return false;
	}

	const bool usePersistedOnly =
		currentBootMode == BootMode::ApConfig || currentBootMode == BootMode::WifiConfig;
	outPollIntervalSeconds = clampPollInterval(pollIntervalSeconds);
	if (usePersistedOnly && mqttEntitiesRtAvailable()) {
		// Portal cache contract:
		// - wifi_config already loaded the saved schedule during startup
		// - fresh AP onboarding only needs the current live defaults until the user saves
		// - request handlers must not touch Preferences again here
		// - transient startup recovery must keep the cache fail-closed so later
		//   portal save/clear/import paths cannot overwrite a still-valid
		//   persisted Bucket_Map with recovered defaults
		if (!mqttEntityCopyBuckets(outBuckets, entityCount)) {
			return false;
		}
		const bool trustRuntimeCache = shouldTrustPortalPollingRuntimeCache(pollingConfigLoadedFromStorage);
		g_portalPollingCacheValid = trustRuntimeCache;
		g_portalPollingCacheEntityCount = trustRuntimeCache ? entityCount : 0;
		g_portalPollingCacheIntervalSeconds = outPollIntervalSeconds;
		if (outBuckets != g_portalBucketsScratch) {
			memcpy(g_portalBucketsScratch, outBuckets, entityCount * sizeof(BucketId));
		}
		return true;
	}
		if (usePersistedOnly || !mqttEntitiesRtAvailable()) {
			if (g_portalPollingCacheValid && g_portalPollingCacheEntityCount == entityCount) {
				outPollIntervalSeconds = g_portalPollingCacheIntervalSeconds;
			// The portal cache lives in g_portalBucketsScratch. Do not prefill
			// defaults into that buffer before checking the cache or the saved
			// overrides get clobbered and the page falsely reverts to defaults.
			if (outBuckets != g_portalBucketsScratch) {
				memcpy(outBuckets, g_portalBucketsScratch, entityCount * sizeof(BucketId));
			}
			return true;
		}

			for (size_t i = 0; i < entityCount; ++i) {
				mqttState entity{};
				if (entities != nullptr) {
					entity = entities[i];
				} else if (!mqttEntityCopyByIndex(i, &entity)) {
					return false;
				}
				outBuckets[i] = bucketIdFromFreq(entity.updateFreq);
				if ((i & 0x7u) == 0x7u) {
					diagYield();
				}
			}

			Preferences preferences;
			preferences.begin(DEVICE_NAME, true);
			// Reuse the boot-loaded poll interval in portal mode. Re-reading the
			// raw interval key inside the ESP8266 portal request path has proven
			// unstable on real hardware, while setup() can load it safely.
			auto recoverPortalLoad = [&](const char *reason,
			                             PollingLoadFailureKind failureKind = PollingLoadFailureKind::Transient) -> bool {
			preferences.end();
			if (!recoverPollingConfigLoadToDefaults(
				    reason, outBuckets, entityCount, outPollIntervalSeconds, failureKind)) {
				return false;
			}
			// Trust contract:
			// - transient load failures may recover defaults for this request, but
			//   must leave the portal cache invalid so later save/clear/import paths
			//   fail closed instead of overwriting valid persisted data
			// - only a proven corrupt persisted Bucket_Map may become trusted
			//   recovered defaults after the reset write succeeds
			const bool trustRecovered = shouldTrustRecoveredPortalPollingConfig(failureKind);
			g_portalPollingCacheValid = trustRecovered;
			g_portalPollingCacheEntityCount = trustRecovered ? entityCount : 0;
			g_portalPollingCacheIntervalSeconds = outPollIntervalSeconds;
			if (trustRecovered && outBuckets != g_portalBucketsScratch) {
				memcpy(g_portalBucketsScratch, outBuckets, entityCount * sizeof(BucketId));
			}
			return true;
		};

			const bool legacyMigrated = preferences.getBool(kPreferenceBucketMapMigrated, false);
			const bool hasPersistedMap = preferences.isKey(kPreferenceBucketMap);
			if (hasPersistedMap) {
			const size_t persistedMapBufferLen =
				preferenceStringBufferLen(preferences, kPreferenceBucketMap, kPrefBucketMapMaxLen);
			ScopedCharBuffer persistedMap(persistedMapBufferLen);
				if (!persistedMap.ok()) {
					return recoverPortalLoad("portal polling load alloc");
				}
			persistedMap.data[0] = '\0';
			preferences.getString(kPreferenceBucketMap, persistedMap.data, persistedMapBufferLen);
			uint32_t unknownCount = 0;
			uint32_t invalidCount = 0;
			uint32_t duplicateCount = 0;
			const bool applied = bucketMapUsesDescriptorIndices(persistedMap.data)
			                         ? portalApplyLegacyBucketMapString(persistedMap.data,
			                                                           entityCount,
			                                                           outBuckets,
			                                                           unknownCount,
			                                                           invalidCount,
			                                                           duplicateCount)
			                         : portalApplyBucketMapString(persistedMap.data,
			                                                      entityCount,
			                                                      outBuckets,
			                                                      unknownCount,
			                                                      invalidCount,
			                                                      duplicateCount);
				if (!applied) {
					return recoverPortalLoad(
						"portal polling load apply", PollingLoadFailureKind::PersistedBucketMapCorrupt);
				}
		} else if (!legacyMigrated) {
			bool hadLegacyOverrides = false;
			for (size_t idx = 0; idx < entityCount; ++idx) {
				mqttState entity{};
				if (!mqttEntityCopyByIndex(idx, &entity)) {
					return recoverPortalLoad("portal polling load catalog");
				}
				const int defaultValue = static_cast<int>(entity.updateFreq);
				int storedValue = defaultValue;
				if (!readLegacyPollingPref(idx, &entity, defaultValue, storedValue, &preferences)) {
					return recoverPortalLoad("portal polling legacy read");
				}
				if (isValidMqttUpdateFreq(storedValue)) {
					const BucketId bucket = bucketIdFromLegacyFreq(storedValue);
					if (bucket != BucketId::Unknown) {
						const bool missingLegacyKey = (storedValue == defaultValue);
						if (!(bucket == bucketIdFromFreq(entity.updateFreq) ||
						      (missingLegacyKey &&
						       entity.updateFreq == mqttUpdateFreq::freqNever &&
						       bucket == BucketId::Disabled))) {
							outBuckets[idx] = bucket;
							hadLegacyOverrides = true;
						}
					}
				}
				if ((idx & 0x7u) == 0x7u) {
					diagYield();
				}
			}
			}
		preferences.end();
		g_portalPollingCacheValid = true;
		g_portalPollingCacheEntityCount = entityCount;
		g_portalPollingCacheIntervalSeconds = outPollIntervalSeconds;
		if (outBuckets != g_portalBucketsScratch) {
			memcpy(g_portalBucketsScratch, outBuckets, entityCount * sizeof(BucketId));
		}
		return true;
	}

	return mqttEntityCopyBuckets(outBuckets, entityCount);
}

static void
primePortalPollingCache(void)
{
	g_portalPollingCacheValid = false;
	g_portalPollingCacheEntityCount = 0;
	g_portalPollingCacheIntervalSeconds = kPollIntervalDefaultSeconds;

	const size_t entityCount = mqttEntitiesCount();
	if (entityCount == 0 || entityCount > kMqttEntityDescriptorCount) {
		return;
	}
	uint32_t storedIntervalSeconds = kPollIntervalDefaultSeconds;
	if (!loadPollingBucketsForPortal(nullptr, entityCount, g_portalBucketsScratch, storedIntervalSeconds)) {
		// If cache priming fails for a transient reason, leave the cache invalid.
		// Portal save/page handlers must not synthesize defaults here because that
		// can overwrite a valid persisted Bucket_Map after a read-time failure.
		return;
	}
}

static uint16_t
portalArgToU16(const String &arg, uint16_t defaultValue)
{
	return portalParseU16Strict(arg.c_str(), defaultValue);
}

struct PortalPollingPageView {
	MqttEntityFamily family = MqttEntityFamily::Battery;
	PortalFamilyPage page{};
	uint16_t visibleIndices[kPollingPortalPageSize]{};
	size_t visibleCount = 0;
	size_t familyCounts[8]{};
	size_t entityCount = 0;
};

static bool
buildPortalPollingPageView(const char *requestedKey, uint16_t requestedPage, PortalPollingPageView &view)
{
	view = PortalPollingPageView{};
	view.entityCount = mqttEntitiesCount();
	if (view.entityCount == 0) {
		return false;
	}

	const uint8_t familyCount = portalPollingFamilyCount();
	if (familyCount == 0 || familyCount > (sizeof(view.familyCounts) / sizeof(view.familyCounts[0]))) {
		return false;
	}

	MqttEntityFamily requestedFamily = portalPollingFamilyAt(0);
	bool requestedFamilyValid = portalPollingFamilyFromKey(requestedKey, &requestedFamily);
	for (size_t idx = 0; idx < view.entityCount; ++idx) {
		mqttState entity{};
		if (!mqttEntityCopyByIndex(idx, &entity)) {
			return false;
		}
		if (!includeEntityInPollingPortal(entity)) {
			continue;
		}
		for (uint8_t familyIdx = 0; familyIdx < familyCount; ++familyIdx) {
			if (entity.family == portalPollingFamilyAt(familyIdx)) {
				view.familyCounts[familyIdx]++;
				break;
			}
		}
	}

	bool selectedFamily = false;
	if (requestedFamilyValid) {
		for (uint8_t familyIdx = 0; familyIdx < familyCount; ++familyIdx) {
			if (portalPollingFamilyAt(familyIdx) == requestedFamily && view.familyCounts[familyIdx] > 0) {
				view.family = requestedFamily;
				selectedFamily = true;
				break;
			}
		}
	}
	if (!selectedFamily) {
		for (uint8_t familyIdx = 0; familyIdx < familyCount; ++familyIdx) {
			if (view.familyCounts[familyIdx] > 0) {
				view.family = portalPollingFamilyAt(familyIdx);
				selectedFamily = true;
				break;
			}
		}
	}
	if (!selectedFamily) {
		view.family = portalPollingFamilyAt(0);
	}

	size_t totalEntityCount = 0;
	for (uint8_t familyIdx = 0; familyIdx < familyCount; ++familyIdx) {
		if (portalPollingFamilyAt(familyIdx) == view.family) {
			totalEntityCount = view.familyCounts[familyIdx];
			break;
		}
	}
	view.page.family = view.family;
	view.page.totalEntityCount = totalEntityCount;
	view.page.maxPage =
		(totalEntityCount == 0) ? 0 : static_cast<uint16_t>((totalEntityCount - 1) / kPollingPortalPageSize);
	view.page.safePage = (requestedPage > view.page.maxPage) ? view.page.maxPage : requestedPage;
	view.page.pageStartOffset = static_cast<size_t>(view.page.safePage) * kPollingPortalPageSize;

	size_t familyOrdinal = 0;
	for (size_t idx = 0; idx < view.entityCount; ++idx) {
		mqttState entity{};
		if (!mqttEntityCopyByIndex(idx, &entity)) {
			return false;
		}
		if (!includeEntityInPollingPortal(entity)) {
			continue;
		}
		if (entity.family != view.family) {
			continue;
		}
		if (familyOrdinal >= view.page.pageStartOffset && view.visibleCount < kPollingPortalPageSize) {
			view.visibleIndices[view.visibleCount++] = static_cast<uint16_t>(idx);
		}
		familyOrdinal++;
	}
	view.page.pageCount = view.visibleCount;
	return true;
}

static bool
portalBucketMatchesPersistedDefault(size_t idx, BucketId bucket)
{
	if (bucket == BucketId::Unknown) {
		return false;
	}
	mqttState entity{};
	if (!mqttEntityCopyByIndex(idx, &entity)) {
		return false;
	}
	if (entity.updateFreq == mqttUpdateFreq::freqNever) {
		return false;
	}
	return bucket == bucketIdFromFreq(entity.updateFreq);
}

static bool
portalAppendBucketOverride(size_t idx, BucketId bucket, char *out, size_t outSize, size_t &used)
{
	mqttState entity{};
	if (!mqttEntityCopyByIndex(idx, &entity)) {
		return false;
	}
	const char *bucketStr = bucketIdToString(bucket);
	char entityName[64];
	mqttEntityNameCopy(&entity, entityName, sizeof(entityName));
	const int needed = snprintf(out + used, outSize - used, "%s=%s;", entityName, bucketStr);
	if (needed < 0 || static_cast<size_t>(needed) >= (outSize - used)) {
		return false;
	}
	used += static_cast<size_t>(needed);
	return true;
}

static bool
portalEstimatePersistedBucketMap(const BucketId *buckets,
                                 size_t entityCount,
                                 size_t &used,
                                 size_t &appliedCount)
{
	if (buckets == nullptr || entityCount == 0 || entityCount > kMqttEntityDescriptorCount) {
		return false;
	}

	used = 0;
	appliedCount = 0;
	for (size_t idx = 0; idx < entityCount; ++idx) {
		const BucketId bucket = buckets[idx];
		if (bucket == BucketId::Unknown || portalBucketMatchesPersistedDefault(idx, bucket)) {
			continue;
		}
		mqttState entity{};
		if (!mqttEntityCopyByIndex(idx, &entity)) {
			return false;
		}
		char entityName[64];
		mqttEntityNameCopy(&entity, entityName, sizeof(entityName));
		used += strlen(entityName) + 1 + strlen(bucketIdToString(bucket)) + 1;
		appliedCount++;
	}
	return true;
}

static bool
portalBuildPersistedBucketMap(const BucketId *buckets,
                              size_t entityCount,
                              char *out,
                              size_t outSize,
                              size_t &appliedCount)
{
	if (buckets == nullptr || out == nullptr || outSize == 0 || entityCount == 0 ||
	    entityCount > kMqttEntityDescriptorCount) {
		return false;
	}

	out[0] = '\0';
	size_t used = 0;
	appliedCount = 0;
	for (size_t idx = 0; idx < entityCount; ++idx) {
		const BucketId bucket = buckets[idx];
		if (bucket == BucketId::Unknown || portalBucketMatchesPersistedDefault(idx, bucket)) {
			continue;
		}
		if (!portalAppendBucketOverride(idx, bucket, out, outSize, used)) {
			return false;
		}
		appliedCount++;
	}
	return true;
}

static bool
portalResolveEntityToken(const char *token, size_t entityCount, size_t &resolvedIndex)
{
	if (token == nullptr || token[0] == '\0' || entityCount == 0 || entityCount > kMqttEntityDescriptorCount) {
		return false;
	}
	if (token[0] == '@') {
		char *endPtr = nullptr;
		errno = 0;
		unsigned long parsed = strtoul(token + 1, &endPtr, 10);
		if (errno != 0 || endPtr == token + 1 || *endPtr != '\0' || parsed > UINT16_MAX) {
			return false;
		}
		return mqttEntityIndexById(static_cast<mqttEntityId>(parsed), &resolvedIndex) &&
		       resolvedIndex < entityCount;
	}
	if (token[0] == '#') {
		char *endPtr = nullptr;
		errno = 0;
		unsigned long parsed = strtoul(token + 1, &endPtr, 10);
		if (errno != 0 || endPtr == token + 1 || *endPtr != '\0' || parsed >= entityCount) {
			return false;
		}
		resolvedIndex = static_cast<size_t>(parsed);
		return true;
	}
	return mqttEntityIndexByName(token, &resolvedIndex) && resolvedIndex < entityCount;
}

static bool
portalApplyBucketMapString(const char *map,
                           size_t entityCount,
                           BucketId *buckets,
                           uint32_t &unknownEntityCount,
                           uint32_t &invalidBucketCount,
                           uint32_t &duplicateEntityCount)
{
	if (map == nullptr || *map == '\0' || buckets == nullptr || entityCount == 0 ||
	    entityCount > kMqttEntityDescriptorCount) {
		return false;
	}
	if (isDisableAllBucketMap(map)) {
		for (size_t i = 0; i < entityCount; ++i) {
			buckets[i] = BucketId::Disabled;
		}
		return true;
	}

	BucketId staged[kMqttEntityDescriptorCount];
	memcpy(staged, buckets, entityCount * sizeof(BucketId));

	uint8_t seen[kMqttEntityDescriptorCount];
	memset(seen, 0, sizeof(seen));
	const char *cursor = map;
	while (*cursor != '\0') {
		while (*cursor != '\0' && (*cursor == ';' || isspace(static_cast<unsigned char>(*cursor)))) {
			cursor++;
		}
		if (*cursor == '\0') {
			break;
		}

		char token[64] = {0};
		char bucketName[32] = {0};
		size_t tokenIdx = 0;
		size_t bucketIdx = 0;
		while (*cursor != '\0' && *cursor != '=' && *cursor != ';' && tokenIdx < sizeof(token) - 1) {
			token[tokenIdx++] = *cursor++;
		}
		token[tokenIdx] = '\0';
		if (*cursor != '=') {
			return false;
		}
		cursor++;
		while (*cursor != '\0' && *cursor != ';' && bucketIdx < sizeof(bucketName) - 1) {
			bucketName[bucketIdx++] = *cursor++;
		}
		bucketName[bucketIdx] = '\0';
		if (token[0] == '\0' || bucketName[0] == '\0') {
			return false;
		}

		size_t idx = 0;
		if (!portalResolveEntityToken(token, entityCount, idx)) {
			unknownEntityCount++;
			continue;
		}
		const BucketId bucket = bucketIdFromString(bucketName);
		if (bucket == BucketId::Unknown) {
			invalidBucketCount++;
			continue;
		}
		if (seen[idx]) {
			duplicateEntityCount++;
		}
		staged[idx] = bucket;
		seen[idx] = 1;
	}

	memcpy(buckets, staged, entityCount * sizeof(BucketId));
	return true;
}

const char *
portalCustomHeadElement(void)
{
	return (currentBootMode == BootMode::WifiConfig) ? kPortalCustomHeadWifi : kPortalCustomHeadAp;
}

static const char kPollingPageExtraHead[] PROGMEM =
	"<style>"
	".polling-nav{display:flex;gap:12px;margin:18px 0 0;}"
	".polling-nav-slot{flex:1;}"
	".polling-nav-slot form{margin:0;}"
	".polling-nav-slot button{margin:0;}"
	".polling-nav-slot button[disabled]{background:#c6d1d8;box-shadow:0 4px 0 #98a8b3;color:#5d6a73;cursor:default;}"
	".polling-nav-slot button[disabled]:active{transform:none;box-shadow:0 4px 0 #98a8b3;}"
	"</style>";

// Keep pagination controls in fixed left/right slots so navigation does not jump around between pages.
static bool
writePollingNavButton(PortalResponseWriter &writer,
                      const char *familyKey,
                      uint16_t page,
                      const char *slotClass,
                      const char *buttonId,
                      const char *label,
                      bool enabled)
{
	if (!writer.write("<div class=\"polling-nav-slot ") ||
	    !writer.write(slotClass) ||
	    !writer.write("\">")) {
		return false;
	}
	if (!enabled) {
		return writer.write("<button id=\"") &&
		       writer.write(buttonId) &&
		       writer.write("\" type=\"button\" disabled>") &&
		       writer.write(label) &&
		       writer.write("</button></div>");
	}
	char pageBuf[8];
	snprintf(pageBuf, sizeof(pageBuf), "%u", static_cast<unsigned>(page));
	return writer.write("<form method=\"get\" action=\"/config/polling\">") &&
	       writer.write("<input type=\"hidden\" name=\"family\" value=\"") &&
	       writer.write(familyKey) &&
	       writer.write("\">") &&
	       writer.write("<input type=\"hidden\" name=\"page\" value=\"") &&
	       writer.write(pageBuf) &&
	       writer.write("\">") &&
	       writer.write("<button id=\"") &&
	       writer.write(buttonId) &&
	       writer.write("\" type=\"submit\">") &&
	       writer.write(label) &&
	       writer.write("</button></form></div>");
}

static void
handlePortalPollingPage(WiFiManager &wifiManager)
{
	if (!wifiManager.server) {
		return;
	}
	ensurePortalPollingRuntimeReady();
	const String familyArg = wifiManager.server->arg("family");
	const uint16_t requestedPage = portalArgToU16(wifiManager.server->arg("page"), 0);
	PortalPollingPageView view{};
	if (!buildPortalPollingPageView(familyArg.c_str(), requestedPage, view)) {
		wifiManager.server->send(500, "text/plain", "polling config unavailable: catalog");
		return;
	}
	const char *familyKey = portalPollingFamilyKey(view.family);
	const char *familyLabel = portalPollingFamilyLabel(view.family);
	BucketId *buckets = g_portalBucketsScratch;
	if (!g_portalPollingCacheValid || g_portalPollingCacheEntityCount != view.entityCount) {
		primePortalPollingCache();
	}
	if (!g_portalPollingCacheValid || g_portalPollingCacheEntityCount != view.entityCount) {
		wifiManager.server->send(500, "text/plain", "polling config unavailable: load");
		return;
	}
	const uint32_t storedIntervalSeconds = g_portalPollingCacheIntervalSeconds;
	ScopedCharBuffer rowBuffer(768);
	if (!rowBuffer.ok()) {
		wifiManager.server->send(500, "text/plain", "polling config unavailable: row");
		return;
	}

	const bool saved = wifiManager.server->hasArg("saved") && wifiManager.server->arg("saved") == "1";
	const bool imported = wifiManager.server->hasArg("imported") && wifiManager.server->arg("imported") == "1";
	const bool err = wifiManager.server->hasArg("err") && wifiManager.server->arg("err") == "1";
	const uint16_t importedUnknownCount = portalArgToU16(wifiManager.server->arg("unknown"), 0);

	static const char kSavedMsg[] PROGMEM = "<p><strong>Saved.</strong></p>";
	static const char kImportedMsg[] PROGMEM =
		"<p><strong>Polling profile imported. Reboot Normal to apply it to runtime and Home Assistant.</strong></p>";
	static const char kImportedUnknownFmt[] PROGMEM =
		"<p><strong>Ignored %u unknown entities from the imported profile.</strong></p>";
	static const char kErrMsg[] PROGMEM = "<p><strong>Some values were invalid and were ignored.</strong></p>";
	static const char kFormOpen[] PROGMEM = "<form id=\"polling-form\" method=\"POST\" action=\"/config/polling/save\">";
	static const char kTableOpen[] PROGMEM = "<table><tr><th>Entity</th><th>Bucket</th></tr>";
	static const char kTableClose[] PROGMEM = "</table><p><button type=\"submit\">Save</button></p></form>";
	static const char kFamilyNavOpen[] PROGMEM = "<p>";
	static const char kFamilyNavClose[] PROGMEM = "</p>";
	static const char kFamilyNavFmt[] PROGMEM =
		"<a href=\"/config/polling?family=%s&page=0\">%s%s</a> ";
	static const char kPageHintFmt[] PROGMEM = "<p class=\"hint\">Family %s · Page %u of %u · %u entities</p>";
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
	char buf[256];
	refreshPortalUpdateCsrfToken();
	auto emitPage = [&](PortalResponseWriter &writer) -> bool {
		const PortalUiMode uiMode =
			(currentBootMode == BootMode::WifiConfig) ? PortalUiMode::Wifi : PortalUiMode::Ap;
		if (!writePortalUiPageStartP(
			    writer, PSTR("Polling Schedule"), PSTR("Polling schedule"), uiMode, kPollingPageExtraHead) ||
		    !writer.writeP(PSTR("<p><a href=\"/\">Menu</a> | <a href=\"/config/mqtt\">MQTT Setup</a> | "
		                        "<a href=\"/config/rs485\">RS485</a> | "
		                        "<a href=\"/config/polling/import\">Restore Profile</a></p>")) ||
		    !writePortalMenuButtonP(
			    writer, PSTR("/config/polling/export"), PSTR("Download Polling Profile"), PSTR("get")) ||
		    !writePortalMenuButtonP(
			    writer, PSTR("/config/reboot-normal"), PSTR("Reboot Normal"), PSTR("post"))) {
			return false;
		}
		if (saved && !writer.writeP(kSavedMsg)) {
			return false;
		}
		if (imported && !writer.writeP(kImportedMsg)) {
			return false;
		}
		if (importedUnknownCount != 0) {
			snprintf_P(buf, sizeof(buf), kImportedUnknownFmt, static_cast<unsigned>(importedUnknownCount));
			if (!writer.write(buf)) {
				return false;
			}
		}
		if (err && !writer.writeP(kErrMsg)) {
			return false;
		}

		if (!writer.writeP(kFamilyNavOpen)) {
			return false;
		}
		for (uint8_t i = 0; i < portalPollingFamilyCount(); ++i) {
			const MqttEntityFamily navFamily = portalPollingFamilyAt(i);
			if (view.familyCounts[i] == 0) {
				continue;
			}
			snprintf_P(buf,
			           sizeof(buf),
			           kFamilyNavFmt,
			           portalPollingFamilyKey(navFamily),
			           (navFamily == view.family) ? "[" : "",
			           portalPollingFamilyLabel(navFamily));
			if (!writer.write(buf)) {
				return false;
			}
			if (navFamily == view.family && !writer.write("] ")) {
				return false;
			}
		}
		if (!writer.writeP(kFamilyNavClose)) {
			return false;
		}

		snprintf_P(buf,
		           sizeof(buf),
		           kPageHintFmt,
		           familyLabel,
		           static_cast<unsigned>(view.page.safePage + 1),
		           static_cast<unsigned>(view.page.maxPage + 1),
		           static_cast<unsigned>(view.page.totalEntityCount));
		if (!writer.write(buf) ||
		    !writer.writeP(PSTR("<p>Edit the visible rows and save.</p>")) ||
		    !writer.writeP(PSTR("<form method=\"post\" action=\"/config/polling/reset\">"
		                       "<input type=\"hidden\" name=\"csrf\" value=\"")) ||
		    !writer.write(portalUpdateCsrfToken) ||
		    !writer.writeP(PSTR("\"><button type=\"submit\">Reset Polling Defaults</button></form>")) ||
		    !writer.writeP(kFormOpen)) {
			return false;
		}
		if (!writer.writeP(PSTR("<input type=\"hidden\" name=\"family\" value=\"")) ||
		    !writer.write(familyKey) ||
		    !writer.writeP(PSTR("\">"))) {
			return false;
		}
		snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(view.page.safePage));
		if (!writer.writeP(PSTR("<input type=\"hidden\" name=\"page\" value=\"")) ||
		    !writer.write(buf) ||
		    !writer.writeP(PSTR("\">")) ||
		    !writer.writeP(PSTR("<input type=\"hidden\" name=\"csrf\" value=\"")) ||
		    !writer.write(portalUpdateCsrfToken) ||
		    !writer.writeP(PSTR("\">")) ||
		    !writer.writeP(PSTR("<input type=\"hidden\" name=\"bucket_map_full\" value=\"\">"))) {
			return false;
		}
		snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(storedIntervalSeconds));
		if (!writer.writeP(
			    PSTR("<label>poll_interval_s <input name=\"poll_interval_s\" type=\"number\" min=\"1\" max=\"120\" value=\"")) ||
		    !writer.write(buf) ||
		    !writer.writeP(PSTR("\"></label><br><br>")) ||
		    !writer.writeP(kTableOpen)) {
			return false;
		}
		for (size_t row = 0; row < view.visibleCount; ++row) {
			const size_t idx = view.visibleIndices[row];
			const BucketId cur = buckets[idx];
			mqttState entity{};
			if (!mqttEntityCopyByIndex(idx, &entity)) {
				return false;
			}
			char entityName[64];
			mqttEntityNameCopy(&entity, entityName, sizeof(entityName));
			char entityDisplayName[64];
			const DiscoveryDeviceScope scope = mqttEntityScope(entity.entityId);
			buildEntityDisplayName(&entity, scope, entityDisplayName, sizeof(entityDisplayName));
			if (entityDisplayName[0] == '\0') {
				strlcpy(entityDisplayName, entityName, sizeof(entityDisplayName));
			}
			const int rowLen = snprintf_P(
				rowBuffer.data,
				rowBuffer.size,
				kRowFmt,
				entityName,
				entityDisplayName,
				static_cast<unsigned>(row),
				bucketIdToString(BucketId::TenSec), (cur == BucketId::TenSec) ? " selected" : "", "10s",
				bucketIdToString(BucketId::OneMin), (cur == BucketId::OneMin) ? " selected" : "", "1m",
				bucketIdToString(BucketId::FiveMin), (cur == BucketId::FiveMin) ? " selected" : "", "5m",
				bucketIdToString(BucketId::OneHour), (cur == BucketId::OneHour) ? " selected" : "", "1h",
				bucketIdToString(BucketId::OneDay), (cur == BucketId::OneDay) ? " selected" : "", "1d",
				bucketIdToString(BucketId::User), (cur == BucketId::User) ? " selected" : "", "usr",
				bucketIdToString(BucketId::Disabled), (cur == BucketId::Disabled) ? " selected" : "", "off");
			if (rowLen > 0 && static_cast<size_t>(rowLen) < rowBuffer.size && !writer.write(rowBuffer.data)) {
				return false;
			}
		}
		if (!writer.writeP(kTableClose)) {
			return false;
		}
		snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(view.page.safePage));
		if (!writer.writeP(PSTR("<form action=\"/config/polling/clear\" method=\"post\">")) ||
		    !writer.writeP(PSTR("<input type=\"hidden\" name=\"family\" value=\"")) ||
		    !writer.write(familyKey) ||
		    !writer.writeP(PSTR("\">")) ||
		    !writer.writeP(PSTR("<input type=\"hidden\" name=\"page\" value=\"")) ||
		    !writer.write(buf) ||
		    !writer.writeP(PSTR("\">")) ||
		    !writer.writeP(PSTR("<input type=\"hidden\" name=\"csrf\" value=\"")) ||
		    !writer.write(portalUpdateCsrfToken) ||
		    !writer.writeP(PSTR("\">")) ||
		    !writer.writeP(PSTR("<button type=\"submit\">Disable All Entities</button></form>")) ||
		    !writer.writeP(PSTR("<div class=\"polling-nav\">")) ||
		    !writePollingNavButton(writer,
		                           familyKey,
		                           (view.page.safePage > 0) ? static_cast<uint16_t>(view.page.safePage - 1) : 0,
		                           "polling-nav-prev",
		                           "polling-nav-prev",
		                           "Prev",
		                           view.page.safePage > 0) ||
		    !writePollingNavButton(writer,
		                           familyKey,
		                           (view.page.safePage < view.page.maxPage)
		                               ? static_cast<uint16_t>(view.page.safePage + 1)
		                               : view.page.safePage,
		                           "polling-nav-next",
		                           "polling-nav-next",
		                           "Next",
		                           view.page.safePage < view.page.maxPage) ||
		    !writer.writeP(PSTR("</div>"))) {
			return false;
		}
		return writer.writeP(kUiPageTail);
	};
	if (!sendPortalHtmlResponse(wifiManager.server.get(), emitPage, "polling config unavailable: emit")) {
	}
}

static void
handlePortalPollingExport(WiFiManager &wifiManager)
{
	if (!wifiManager.server) {
		return;
	}

	PortalResponseWriter countWriter;
	if (!writePortalCurrentPollingProfile(countWriter) || countWriter.bytes == 0) {
		wifiManager.server->send(500, "text/plain", "polling profile unavailable");
		return;
	}

	wifiManager.server->sendHeader("Cache-Control", "no-store");
	wifiManager.server->sendHeader(
		"Content-Disposition", "attachment; filename=\"alpha2mqtt_polling_profile.txt\"");
	wifiManager.server->setContentLength(countWriter.bytes);
	wifiManager.server->send(200, "text/plain", "");
	PortalResponseWriter writer;
	writer.server = wifiManager.server.get();
	(void)writePortalCurrentPollingProfile(writer);
}

static void
handlePortalPollingImportPage(WiFiManager &wifiManager)
{
	resetPortalPollingProfileUploadState();
	renderPortalPollingImportPage(wifiManager, nullptr, false);
}

static void
handlePortalPollingImportUpload(WiFiManager &wifiManager)
{
	static const char kImportUnavailableErr[] PROGMEM = "Polling profile import unavailable.";
	static const char kImportOomErr[] PROGMEM = "Polling profile import ran out of memory.";
	static const char kPrepareBucketsErr[] PROGMEM = "Polling profile import could not prepare buckets.";
	static const char kMissingHeaderErr[] PROGMEM = "Polling profile header is missing.";
	static const char kMissingIntervalErr[] PROGMEM = "Polling profile must define poll_interval_s.";
	static const char kImportFailedErr[] PROGMEM = "Polling profile import failed.";
	static const char kUploadInterruptedErr[] PROGMEM = "Polling profile upload was interrupted.";
	if (!wifiManager.server) {
		return;
	}
	if (!portalUpdateRequestHasValidToken(&wifiManager)) {
		return;
	}

	HTTPUpload &upload = wifiManager.server->upload();
	switch (upload.status) {
	case UPLOAD_FILE_START: {
		resetPortalPollingProfileUploadState();
		portalPollingProfileUpload.started = true;
		portalPollingProfileUpload.ok = true;
		ensurePortalPollingRuntimeReady();
		const size_t entityCount = mqttEntitiesCount();
		if (entityCount == 0 || entityCount > kMqttEntityDescriptorCount) {
			copyPortalPollingProfileUploadErrorP(kImportUnavailableErr);
			portalPollingProfileUpload.ok = false;
			break;
		}
		portalPollingProfileUpload.entityCount = entityCount;
		portalPollingProfileUpload.stagedBuckets = new (std::nothrow) BucketId[entityCount];
		portalPollingProfileUpload.seenEntities = new (std::nothrow) uint8_t[entityCount];
		portalPollingProfileUpload.lineBuf = new (std::nothrow) char[kPollingProfileLineMaxLen];
		portalPollingProfileUpload.error = new (std::nothrow) char[kPollingProfileUploadErrorMaxLen];
		if (portalPollingProfileUpload.stagedBuckets == nullptr ||
		    portalPollingProfileUpload.seenEntities == nullptr ||
		    portalPollingProfileUpload.lineBuf == nullptr ||
		    portalPollingProfileUpload.error == nullptr) {
			copyPortalPollingProfileUploadErrorP(kImportOomErr);
			portalPollingProfileUpload.ok = false;
			break;
		}
		portalPollingProfileUpload.error[0] = '\0';
		memset(portalPollingProfileUpload.seenEntities, 0, entityCount * sizeof(uint8_t));
		if (!initializePortalPollingProfileBuckets(portalPollingProfileUpload.stagedBuckets, entityCount)) {
			copyPortalPollingProfileUploadErrorP(kPrepareBucketsErr);
			portalPollingProfileUpload.ok = false;
		}
		break;
	}
	case UPLOAD_FILE_WRITE:
		if (portalPollingProfileUpload.started && portalPollingProfileUpload.ok) {
			(void)appendPortalPollingProfileUploadBytes(upload.buf, upload.currentSize);
		}
		break;
	case UPLOAD_FILE_END:
		if (!portalPollingProfileUpload.started) {
			break;
		}
		if (portalPollingProfileUpload.ok && portalPollingProfileUpload.lineLen > 0) {
			portalPollingProfileUpload.lineBuf[portalPollingProfileUpload.lineLen] = '\0';
			(void)processPortalPollingProfileUploadLine(portalPollingProfileUpload.lineBuf);
			portalPollingProfileUpload.lineLen = 0;
		}
		if (portalPollingProfileUpload.ok && !portalPollingProfileUpload.sawHeader) {
			copyPortalPollingProfileUploadErrorP(kMissingHeaderErr);
			portalPollingProfileUpload.ok = false;
		}
		if (portalPollingProfileUpload.ok && !portalPollingProfileUpload.sawPollInterval) {
			copyPortalPollingProfileUploadErrorP(kMissingIntervalErr);
			portalPollingProfileUpload.ok = false;
		}
		if (portalPollingProfileUpload.ok) {
			const PortalPollingProfileApplyResult applyResult =
				applyPortalPollingProfileState(portalPollingProfileUpload.stagedIntervalSeconds,
				                              portalPollingProfileUpload.stagedBuckets,
				                              portalPollingProfileUpload.entityCount,
				                              portalPollingProfileUpload.unknownCount);
			if (!applyResult.ok) {
				copyPortalPollingProfileUploadErrorP(kImportFailedErr);
				portalPollingProfileUpload.ok = false;
			} else {
				portalPollingProfileUpload.unknownCount = applyResult.unknownCount;
			}
		}
		portalPollingProfileUpload.finished = true;
		break;
	case UPLOAD_FILE_ABORTED:
		if (portalPollingProfileUpload.started) {
			copyPortalPollingProfileUploadErrorP(kUploadInterruptedErr);
			portalPollingProfileUpload.ok = false;
			portalPollingProfileUpload.finished = true;
		}
		break;
	default:
		break;
	}
}

static void
handlePortalPollingImport(WiFiManager &wifiManager)
{
	if (!wifiManager.server) {
		return;
	}
	if (!portalUpdateRequestHasValidToken(&wifiManager)) {
		wifiManager.server->send(403, "text/plain", "invalid csrf");
		return;
	}
	if (!portalPollingProfileUpload.started) {
		renderPortalPollingImportPage(wifiManager, "Polling profile file is required.", true);
		return;
	}
	if (!portalPollingProfileUpload.finished || !portalPollingProfileUpload.ok) {
		const char *message =
			(portalPollingProfileUpload.error != nullptr && portalPollingProfileUpload.error[0] != '\0')
				? portalPollingProfileUpload.error
				: "Polling profile import failed.";
		renderPortalPollingImportPage(wifiManager, message, true);
		resetPortalPollingProfileUploadState();
		return;
	}

	char location[96];
	if (portalPollingProfileUpload.unknownCount != 0) {
		snprintf(location,
		         sizeof(location),
		         "/config/polling?imported=1&unknown=%lu",
		         static_cast<unsigned long>(portalPollingProfileUpload.unknownCount));
	} else {
		strlcpy(location, "/config/polling?imported=1", sizeof(location));
	}
	resetPortalPollingProfileUploadState();
	wifiManager.server->sendHeader("Location", location);
	wifiManager.server->send(302, "text/plain", "");
}

static void
handlePortalPollingReset(WiFiManager &wifiManager)
{
	if (!wifiManager.server) {
		return;
	}
	if (!portalUpdateRequestHasValidToken(&wifiManager)) {
		wifiManager.server->send(403, "text/plain", "invalid csrf");
		return;
	}
	ensurePortalPollingRuntimeReady();
	if (!resetPollingConfigToDefaults()) {
		wifiManager.server->sendHeader("Location", "/?polling_reset=err");
		wifiManager.server->send(302, "text/plain", "");
		return;
	}
	wifiManager.server->sendHeader("Location", "/?polling_reset=1");
	wifiManager.server->send(302, "text/plain", "");
}

static void
handlePortalPollingSave(WiFiManager &wifiManager)
{
	if (!wifiManager.server) {
		return;
	}
	if (!portalUpdateRequestHasValidToken(&wifiManager)) {
		wifiManager.server->send(403, "text/plain", "invalid csrf");
		return;
	}
	ensurePortalPollingRuntimeReady();

	const size_t entityCount = mqttEntitiesCount();
	if (entityCount == 0 || entityCount > kMqttEntityDescriptorCount) {
		wifiManager.server->send(500, "text/plain", "polling config unavailable");
		return;
	}
	if (!g_portalPollingCacheValid || g_portalPollingCacheEntityCount != entityCount) {
		primePortalPollingCache();
	}
	if (!g_portalPollingCacheValid || g_portalPollingCacheEntityCount != entityCount) {
		// Saving against an untrusted cache risks writing defaults over a valid
		// persisted map after a load-time failure. Fail closed instead.
		wifiManager.server->send(500, "text/plain", "polling config unavailable");
		return;
	}
	BucketId workingBuckets[kMqttEntityDescriptorCount]{};
	memcpy(workingBuckets, g_portalBucketsScratch, entityCount * sizeof(BucketId));
	BucketId *buckets = workingBuckets;
	uint32_t storedIntervalSeconds = g_portalPollingCacheIntervalSeconds;
	const String familyArg = wifiManager.server->arg("family");
	const uint16_t requestedPage = portalArgToU16(wifiManager.server->arg("page"), 0);
	PortalPollingPageView view{};
	if (!buildPortalPollingPageView(familyArg.c_str(), requestedPage, view)) {
		wifiManager.server->send(500, "text/plain", "polling config unavailable");
		return;
	}
	const char *familyKey = portalPollingFamilyKey(view.family);
	bool hadError = false;
	if (wifiManager.server->hasArg("poll_interval_s")) {
		const String pollIntervalValue = wifiManager.server->arg("poll_interval_s");
		uint32_t parsedInterval = 0;
		if (!parseStrictUint32(pollIntervalValue.c_str(), kPollIntervalMaxSeconds, parsedInterval)) {
			wifiManager.server->sendHeader(
				"Location",
				String("/config/polling?family=") + familyKey + "&page=" + String(view.page.safePage) +
					"&err=1");
			wifiManager.server->send(302, "text/plain", "");
			return;
		}
		storedIntervalSeconds = clampPollInterval(parsedInterval);
	}

	bool usedFullMap = false;
	if (wifiManager.server->hasArg("bucket_map_full")) {
		const String fullMap = wifiManager.server->arg("bucket_map_full");
		if (fullMap.length() >= kPrefBucketMapMaxLen) {
			wifiManager.server->sendHeader(
				"Location",
				String("/config/polling?family=") + familyKey + "&page=" + String(view.page.safePage) +
					"&err=1");
			wifiManager.server->send(302, "text/plain", "");
			return;
		}
		if (fullMap.length() > 0) {
			usedFullMap = true;
			uint32_t unknownCount = 0;
			uint32_t invalidCount = 0;
			uint32_t duplicateCount = 0;
			if (!portalApplyBucketMapString(fullMap.c_str(),
			                               entityCount,
			                               buckets,
			                               unknownCount,
			                               invalidCount,
			                               duplicateCount)) {
				hadError = true;
			}
		}
	}
	if (!usedFullMap) {
		for (size_t row = 0; row < view.visibleCount; ++row) {
			char argName[8];
			snprintf(argName, sizeof(argName), "b%u", static_cast<unsigned>(row));
			if (!wifiManager.server->hasArg(argName)) {
				continue;
			}
			const String argVal = wifiManager.server->arg(argName);
			const BucketId bucket = bucketIdFromString(argVal.c_str());
			if (bucket == BucketId::Unknown) {
				continue;
			}
			const size_t idx = view.visibleIndices[row];
			if (idx >= entityCount) {
				hadError = true;
				continue;
			}
			buckets[idx] = bucket;
		}
	}

	size_t persistedOverrideCount = 0;
	size_t persistedMapLen = 0;
	if (!portalEstimatePersistedBucketMap(buckets, entityCount, persistedMapLen, persistedOverrideCount)) {
		hadError = true;
	}
	ScopedCharBuffer canonicalMapBuffer((persistedMapLen == 0 ? 0 : persistedMapLen) + 1);
	if (!canonicalMapBuffer.ok()) {
		wifiManager.server->send(500, "text/plain", "polling config unavailable");
		return;
	}

	if (!portalBuildPersistedBucketMap(buckets,
	                                  entityCount,
	                                  canonicalMapBuffer.data,
	                                  canonicalMapBuffer.size,
	                                  persistedOverrideCount)) {
		hadError = true;
	}
	if (!hadError && mqttEntitiesRtAvailable() && !mqttEntityCanApplyBuckets(buckets, entityCount)) {
		hadError = true;
	}
	if (!hadError) {
		if (persistUserPollingConfig(storedIntervalSeconds, canonicalMapBuffer.data)) {
			memcpy(g_portalBucketsScratch, buckets, entityCount * sizeof(BucketId));
			pollIntervalSeconds = storedIntervalSeconds;
			g_portalPollingCacheValid = true;
			g_portalPollingCacheEntityCount = entityCount;
			g_portalPollingCacheIntervalSeconds = storedIntervalSeconds;
			updatePollingLastChange();
			pollingConfigLoadedFromStorage = true;
		} else {
			hadError = true;
		}
	}

	// Polling save is user-driven config edit only. Never auto-reboot from this path.
	portalRebootScheduled = false;
	portalRebootIntent = BootIntent::Normal;
	portalMqttSaved = false;
	if (hadError) {
		g_portalPollingCacheValid = false;
	}

		// Optional hidden reboot for E2E; not shown in UI.
		if (wifiManager.server->hasArg("reboot") && wifiManager.server->arg("reboot") == "1") {
			setBootIntentAndReboot(portalNormalRebootIntent());
			return;
		}

	char location[96];
	snprintf(location,
	         sizeof(location),
	         "/config/polling?family=%s&page=%u%s%s",
	         familyKey,
	         static_cast<unsigned>(view.page.safePage),
	         hadError ? "" : "&saved=1",
	         hadError ? "&err=1" : "");
	wifiManager.server->sendHeader("Location", location);
	wifiManager.server->send(302, "text/plain", "");
}

static void
handlePortalPollingClear(WiFiManager &wifiManager)
{
	if (!wifiManager.server) {
		return;
	}
	if (!portalUpdateRequestHasValidToken(&wifiManager)) {
		wifiManager.server->send(403, "text/plain", "invalid csrf");
		return;
	}
	ensurePortalPollingRuntimeReady();

	const size_t entityCount = mqttEntitiesCount();
	if (entityCount == 0 || entityCount > kMqttEntityDescriptorCount) {
		wifiManager.server->send(500, "text/plain", "polling config unavailable");
		return;
	}
	if (!g_portalPollingCacheValid || g_portalPollingCacheEntityCount != entityCount) {
		primePortalPollingCache();
	}
	if (!g_portalPollingCacheValid || g_portalPollingCacheEntityCount != entityCount) {
		// Clear-all is still a save operation. Never invent a default cache if
		// loading the current persisted schedule failed.
		wifiManager.server->send(500, "text/plain", "polling config unavailable");
		return;
	}
	BucketId workingBuckets[kMqttEntityDescriptorCount]{};
	memcpy(workingBuckets, g_portalBucketsScratch, entityCount * sizeof(BucketId));
	BucketId *buckets = workingBuckets;
	uint32_t storedIntervalSeconds = g_portalPollingCacheIntervalSeconds;

	const String familyArg = wifiManager.server->arg("family");
	const uint16_t requestedPage = portalArgToU16(wifiManager.server->arg("page"), 0);
	PortalPollingPageView view{};
	if (!buildPortalPollingPageView(familyArg.c_str(), requestedPage, view)) {
		wifiManager.server->send(500, "text/plain", "polling config unavailable");
		return;
	}
	const char *familyKey = portalPollingFamilyKey(view.family);

	bool hadError = false;
	portalSetAllBuckets(buckets, entityCount, BucketId::Disabled);
	char disableAllMap[24];
	const bool mapBuilt = copyDisableAllBucketMap(disableAllMap, sizeof(disableAllMap));
	if (!mapBuilt) {
		hadError = true;
	}

	if (mapBuilt) {
		if (persistUserPollingConfig(storedIntervalSeconds, disableAllMap)) {
			memcpy(g_portalBucketsScratch, buckets, entityCount * sizeof(BucketId));
			pollIntervalSeconds = storedIntervalSeconds;
			g_portalPollingCacheValid = true;
			g_portalPollingCacheEntityCount = entityCount;
			g_portalPollingCacheIntervalSeconds = storedIntervalSeconds;
			updatePollingLastChange();
			pollingConfigLoadedFromStorage = true;
		} else {
			hadError = true;
		}
	} else {
		hadError = true;
	}
	portalRebootScheduled = false;
	portalRebootIntent = BootIntent::Normal;
	portalMqttSaved = false;
	if (hadError) {
		g_portalPollingCacheValid = false;
	}

	char location[96];
	snprintf(location,
	         sizeof(location),
	         "/config/polling?family=%s&page=%u%s%s",
	         familyKey,
	         static_cast<unsigned>(view.page.safePage),
	         hadError ? "" : "&saved=1",
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
	Serial.begin(9600);
#if defined(DEBUG_OVER_SERIAL) || defined(DEBUG_LEVEL2) || defined(DEBUG_OUTPUT_TX_RX)
	// Boot prints below are unconditional, so keep the serial port initialized even when
	// higher debug levels are off. This remains a diagnostics-only channel.
#ifdef DEBUG_OVER_SERIAL
	logHeapFreeOnly("very-early");
	diagDelay(100);
	logHeapFreeOnly("boot");
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
#if defined(MP_ESP8266)
	runtimeWifiDisconnectHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& event) {
		wifiLastDisconnectReason = static_cast<int>(event.reason);
		strlcpy(wifiLastDisconnectLabel,
		        wifiDisconnectReasonLabel(wifiLastDisconnectReason),
		        sizeof(wifiLastDisconnectLabel));
#ifdef DEBUG_OVER_SERIAL
		if (currentBootMode == BootMode::Normal) {
			Serial.printf("WiFi disconnect reason=%d (%s)\r\n",
			              wifiLastDisconnectReason,
			              wifiLastDisconnectLabel);
		}
#endif
	});
#endif
	{
		String resetReason = ESP.getResetReason();
		strlcpy(lastResetReason, resetReason.c_str(), sizeof(lastResetReason));
	}

	char storedIntent[kPrefBootIntentMaxLen] = "";
	char storedMode[kPrefBootModeMaxLen] = "";
	char storedInverterLabel[kPrefInverterLabelMaxLen] = "";
	char wifiSsid[kPrefWifiSsidMaxLen] = "";
	char wifiPass[kPrefWifiPasswordMaxLen] = "";
	char mqttServer[kPrefMqttServerMaxLen] = "";
	char mqttUser[kPrefMqttUsernameMaxLen] = "";
	char mqttPass[kPrefMqttPasswordMaxLen] = "";
	uint32_t storedRs485Baud = 0;
	bool hasStoredRs485Baud = false;

	preferences.begin(DEVICE_NAME, true); // RO
	preferences.getString(kPreferenceBootIntent, storedIntent, sizeof(storedIntent));
	preferences.getString(kPreferenceBootMode, storedMode, sizeof(storedMode));
	preferences.getString(kPreferenceInverterLabel, storedInverterLabel, sizeof(storedInverterLabel));
	preferences.getString("WiFi_SSID", wifiSsid, sizeof(wifiSsid));
	preferences.getString("WiFi_Password", wifiPass, sizeof(wifiPass));
		preferences.getString("MQTT_Server", mqttServer, sizeof(mqttServer));
		appConfig.mqttPort = preferences.getInt("MQTT_Port", 0);
		preferences.getString("MQTT_Username", mqttUser, sizeof(mqttUser));
		preferences.getString("MQTT_Password", mqttPass, sizeof(mqttPass));
		pollIntervalSeconds = clampPollInterval(preferences.getUInt(kPreferencePollInterval, kPollIntervalDefaultSeconds));
#if defined(MP_XIAO_ESP32C6)
		appConfig.extAntenna = preferences.getBool("Ext_Antenna", false);
#elif defined(MP_ESPUNO_ESP32C6)
	appConfig.extAntenna = preferences.getBool("Ext_Antenna", false);
#endif // MP_XIAO_ESP32C6
	preferences.end();
	loadConfiguredRs485Baud(storedRs485Baud, hasStoredRs485Baud);
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

#ifdef DEBUG_OVER_SERIAL
	Serial.printf("Stored boot intent='%s' -> %s\r\n", storedIntent, bootIntentToString(currentBootIntent));
	Serial.printf("Stored boot mode='%s' -> %s\r\n", storedMode, bootModeToString(currentBootMode));
	logHeapFreeOnly("after-pref-read");
#endif

	appConfig.wifiSSID = wifiSsid;
	appConfig.wifiPass = wifiPass;
	appConfig.mqttSrvr = mqttServer;
	appConfig.mqttUser = mqttUser;
	appConfig.mqttPass = mqttPass;
	appConfig.inverterLabel = storedInverterLabel;
	rs485BaudTracker.configuredBaud = storedRs485Baud;
	rs485BaudTracker.hasConfiguredBaud = hasStoredRs485Baud;
	rs485BaudTracker.syncState = hasStoredRs485Baud ? Rs485BaudSyncState::Unknown : Rs485BaudSyncState::Unknown;
	clearRuntimeInverterIdentity();
	bootPlan = planForBootMode(currentBootMode);
	BootMode startupMode = currentBootMode;

#ifdef DEBUG_OVER_SERIAL
	Serial.print("boot_intent=");
	Serial.println(bootIntentToString(currentBootIntent));
	Serial.print("boot_mode=");
	Serial.println(bootModeToString(currentBootMode));
#endif

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
	mqttConfigComplete = isMqttConfigComplete();
	mqttRuntimeEnabled = bootPlan.mqtt && mqttConfigComplete;

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
		Serial.println("Entering configHandler() from setup");
#endif
		// Explicit AP config mode forces the AP captive portal once; success will return to normal.
		updateOLED(false, "Config", "mode", "portal");
		configHandler();
		return;
	}
	if (startupMode == BootMode::WifiConfig) {
#ifdef DEBUG_OVER_SERIAL
		portalLog("Config mode boot (wifi_config); starting STA-only portal.");
		Serial.println("Entering configHandlerSta() from setup");
#endif
		if (!isWifiConfigComplete()) {
#ifdef DEBUG_OVER_SERIAL
			portalLog("wifi_config requested without saved WiFi credentials; falling back to AP portal.");
#endif
			currentBootMode = BootMode::ApConfig;
			updateOLED(false, "WiFi", "config", "ap portal");
			configHandler();
			return;
		}
			updateOLED(false, "WiFi", "config", "portal");
			configHandlerSta();
			return;
	}

	if (bootPlan.wifiSta) {
		// Wi-Fi is the minimum requirement for normal runtime. Missing MQTT config disables the
		// MQTT subsystem for this boot, but should not force the device back into the portal.
		if (!isWifiConfigComplete()) {
#ifdef DEBUG_OVER_SERIAL
			Serial.println("Missing required config; entering AP config loop");
#endif
			configLoop();
			setBootIntentAndReboot(BootIntent::WifiConfig);
		} else if (mqttSubsystemEnabled()) {
			updateOLED(false, "Found", "config", _version);
			diagDelay(250);
		}
	}

	if (bootPlan.wifiSta) {
		// Configure WIFI
#ifdef DEBUG_OVER_SERIAL
		logHeapFreeOnly("pre-wifi");
#endif
		recordBootMemPublishCheckpoint(BootMemPublishCheckpoint::PreWifi);
		setupWifi(true);
		lastWifiConnected = true;
#ifdef DEBUG_OVER_SERIAL
		logHeapFreeOnly("after WiFi");
#endif
		recordBootMemPublishCheckpoint(BootMemPublishCheckpoint::PostWifi);
		(void)ensureNormalRuntimeBuffers();
		recordBootMemStage(BootMemStage::Boot1);
		setupHttpControlPlane();
	}

	if (mqttSubsystemEnabled()) {
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
		for (int _bufferSize = (MAX_MQTT_PAYLOAD_SIZE + MQTT_HEADER_SIZE);
		     _bufferSize >= MIN_MQTT_PAYLOAD_SIZE + MQTT_HEADER_SIZE;
		     _bufferSize -= 512) {
#ifdef DEBUG_OVER_SERIAL
		sprintf(_debugOutput, "Requesting a buffer of : %d bytes", _bufferSize);
		Serial.println(_debugOutput);
#endif

		if (_mqtt.setBufferSize(_bufferSize)) {
			const int mqttBufferPayloadSize = _bufferSize - MQTT_HEADER_SIZE;
			_maxPayloadSize = MIN_MQTT_PAYLOAD_SIZE;
#ifdef DEBUG_OVER_SERIAL
			sprintf(_debugOutput,
			        "_bufferSize: %d,\r\n\r\n_bufferPayload (Including null terminator): %d\r\n\r\n_publishScratch: %d",
			        _bufferSize,
			        mqttBufferPayloadSize,
			        _maxPayloadSize);
			Serial.println(_debugOutput);
#endif
			_mqttPayload = new char[_maxPayloadSize];
			if (_mqttPayload != NULL) {
				emptyPayload();
#ifdef DEBUG_OVER_SERIAL
					logHeapFreeOnly("after MQTT payload");
#endif
					recordBootMemPublishCheckpoint(BootMemPublishCheckpoint::PostMqtt);
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
		logHeapFreeOnly("before RS485 init");
#endif
		recordBootMemPublishCheckpoint(BootMemPublishCheckpoint::PreRs485);
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
			if (!inverterSerialKnown()) {
				// Seed the fixed stub identity during RS485 init so online control transitions only need
				// to flip readiness/connectivity instead of rebuilding identifiers on the MQTT control path.
				primeStubRuntimeInverterIdentity("STUBSN000000000");
				inverterReady = false;
			}
#endif
#if defined(DEBUG_OVER_SERIAL)
			logHeapFreeOnly("after RS485 init");
#endif
			recordBootMemPublishCheckpoint(BootMemPublishCheckpoint::PostRs485);
			recordBootMemStage(BootMemStage::Boot3);
			publishBootMemEventOncePerBoot();

			rs485UartInfo = _modBus->uartInfo();

			// Start background probing; loop() will keep trying indefinitely with backoff capped at 15s.
			rs485RuntimeReconnectOnRediscoveryStart(rs485RuntimeReconnect);
			rs485ConnectState = Rs485ConnectState::ProbingBaud;
			resetRs485ProbeState(millis());

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

	// WiFi portal contract:
	// - wifi_config is STA-only (no SoftAP/DNS)
	// - connect timeout is not enough to fall back to AP
	// - only invalid-config classification should redirect to ap_config
	WiFi.mode(WIFI_STA);

#ifdef DEBUG_OVER_SERIAL
	portalLog("STA portal: connecting to saved WiFi (ssid=%s)", appConfig.wifiSSID.c_str());
	logHeapFreeOnly("sta-portal-entry");
#endif

	if (appConfig.wifiSSID == "") {
#ifdef DEBUG_OVER_SERIAL
		portalLog("STA portal: no persisted WiFi credentials; falling back to AP config.");
#endif
		setBootIntentAndReboot(bootIntentAfterStaPortalConnectFailure());
		return;
	}

	clearWifiFailureTracking();
	beginWifiStationWithStoredCredentials();

	const WifiRecoveryTiming recoveryTiming = wifiRecoveryTiming();
	unsigned long connectWindowStart = millis();
	while (WiFi.status() != WL_CONNECTED) {
		if (millis() - connectWindowStart >= recoveryTiming.bootValidationMs) {
			const WifiFailureClass failureClass = classifyWifiFailure(currentWifiFailureSignals());
			if (shouldRebootApAfterStaPortalConnectFailure(isWifiConfigComplete(), failureClass)) {
#ifdef DEBUG_OVER_SERIAL
				portalLog("STA portal: invalid WiFi config detected; falling back to AP config.");
#endif
				setBootIntentAndReboot(bootIntentAfterStaPortalConnectFailure());
				return;
			}
#ifdef DEBUG_OVER_SERIAL
			portalLog("STA portal: connect timeout without invalid-config classification; retrying STA connect.");
#endif
			clearWifiFailureTracking();
			beginWifiStationWithStoredCredentials();
			connectWindowStart = millis();
		}
		diagDelay(50);
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
	wifiManager.setCustomMenuHTML(portalMenuStaHtml());
	wifiManager.setConnectTimeout(20);
	wifiManager.setConfigPortalTimeout(0);
	wifiManager.setDisableConfigPortal(false);
	wifiManager.setCustomHeadElement(portalCustomHeadElement());
	wifiManager.setConfigResetCallback([&]() {
		clearUserWifiCredentials();
		clearSdkWifiCredentials();
		appConfig.wifiSSID = "";
		appConfig.wifiPass = "";
	});

	preparePortalPollingRuntime();
	primePortalPollingCache();
	// STA portal serves custom MQTT/polling pages via bindPortalRoutes(); keeping the old
	// WiFiManager parameter list registered here only inflates connected-portal heap and
	// leaves less headroom for OTA multipart parsing.

	portalStatus = portalStatusIdle;
	portalStatusReason[0] = '\0';
	portalStatusSsid[0] = '\0';
	portalSubmittedPass[0] = '\0';
	portalStatusIp[0] = '\0';
	portalLastDisconnectReason = -1;
	portalLastDisconnectLabel[0] = '\0';
	portalConnectStart = 0;
	portalNeedsMqttConfig = !isMqttConfigComplete();
	portalMqttSaved = false;
	portalWifiCredentialsChanged = false;
	portalRebootScheduled = false;
	portalRebootIntent = BootIntent::Normal;
	portalRebootAt = 0;
	portalRouteRebindRetriesRemaining = 0;
	portalRouteRebindRetryAt = 0;
	invalidatePortalRouteBinding("sta-portal-init");
	capturePortalActiveStaConnection();

	// WiFiManager recreates the server and then installs its built-in routes. Bind here so our
	// minimal handlers take precedence for overlapping paths like "/" and "/status".
	wifiManager.setWebServerCallback([&]() {
		bindPortalRoutes(wifiManager);
	});
	wifiManager.setConfigPortalBlocking(false);
	wifiManager.startWebPortal();

#ifdef DEBUG_OVER_SERIAL
	portalLog("STA portal URL: http://%s/", WiFi.localIP().toString().c_str());
	portalLog("STA portal started server=%p", wifiManager.server ? static_cast<void *>(wifiManager.server.get()) : nullptr);
	portalLog("Portal loop start free=%u max=%u frag=%u",
		ESP.getFreeHeap(),
		ESP.getMaxFreeBlockSize(),
		ESP.getHeapFragmentation());
#endif

	unsigned long portalStatsLast = 0;
	for (;;) {
		bindPortalRoutes(wifiManager);
		unsigned long processStart = millis();
		wifiManager.process();
		bindPortalRoutes(wifiManager);
		servicePortalRouteRebindRetries(wifiManager);
		processPendingPollingConfigPayload();
			unsigned long processElapsed = millis() - processStart;
#ifdef DEBUG_OVER_SERIAL
			(void)processElapsed;
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
#else
			(void)processElapsed;
#endif

		// Once MQTT params are saved, WiFi credentials should already be persisted from the
		// earlier WiFi save path. Do not rewrite them on every loop tick while waiting to reboot.
		if (portalMqttSaved && !portalNeedsMqttConfig && isWifiConfigComplete()) {
			if (!portalRebootScheduled) {
				schedulePortalReboot(portalNormalRebootIntent());
#ifdef DEBUG_OVER_SERIAL
				portalLog("MQTT configured and WiFi credentials present; reboot scheduled.");
#endif
			}
				}
				if (portalRebootScheduled && static_cast<long>(millis() - portalRebootAt) >= 0) {
					setBootIntentAndReboot(portalRebootIntent);
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

	// AP portal contract:
	// - keep the portal reachable while STA onboarding is in progress
	// - after WiFi is saved but setup is still incomplete, the intended handoff
	//   is to the STA WiFi portal so the user can finish setup on the LAN
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
	wifiManager.setConfigResetCallback([&]() {
		clearUserWifiCredentials();
		clearSdkWifiCredentials();
		appConfig.wifiSSID = "";
		appConfig.wifiPass = "";
	});
	refreshPortalCustomParameters();
	preparePortalPollingRuntime();
	primePortalPollingCache();
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

#ifdef DEBUG_OVER_SERIAL
	logHeapFreeOnly("ap-portal-entry");
#endif

#ifdef MP_XIAO_ESP32C6
	wifiManager.addParameter(&custom_ext_ant);
#endif // MP_XIAO_ESP32C6
#ifdef MP_ESPUNO_ESP32C6
	wifiManager.addParameter(&custom_ext_ant);
#endif // MP_ESPUNO_ESP32C6
	wifiManager.addParameter(&gPortalMqttSection);
	wifiManager.addParameter(&gPortalMqttServer);
	wifiManager.addParameter(&gPortalMqttPort);
	wifiManager.addParameter(&gPortalMqttUser);
	wifiManager.addParameter(&gPortalMqttPass);
	wifiManager.addParameter(&gPortalInverterLabel);
	wifiManager.addParameter(&gPortalPollingLink);

	portalStatus = portalStatusIdle;
	portalStatusReason[0] = '\0';
	portalStatusSsid[0] = '\0';
	portalSubmittedPass[0] = '\0';
	portalStatusIp[0] = '\0';
	portalLastDisconnectReason = -1;
	portalLastDisconnectLabel[0] = '\0';
	portalConnectStart = 0;
	portalNeedsMqttConfig = false;
	portalMqttSaved = false;
	portalWifiCredentialsChanged = false;
	portalRebootScheduled = false;
	portalRebootIntent = BootIntent::Normal;
	portalRebootAt = 0;
	portalRouteRebindRetriesRemaining = 0;
	portalRouteRebindRetryAt = 0;
	portalLastActivityAt = millis();

#if defined MP_ESP8266
	static WiFiEventHandler disconnectHandler;
	disconnectHandler = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& event) {
		portalLastDisconnectReason = static_cast<int>(event.reason);
		strlcpy(portalLastDisconnectLabel,
		        wifiDisconnectReasonLabel(portalLastDisconnectReason),
		        sizeof(portalLastDisconnectLabel));
#ifdef DEBUG_OVER_SERIAL
		portalLog("WiFi disconnect: SSID=%s reason=%d (%s)",
			portalStatusSsid,
			portalLastDisconnectReason,
			portalLastDisconnectLabel);
#endif
	});
#endif

	wifiManager.setCustomHeadElement(portalCustomHeadElement());
	invalidatePortalRouteBinding("ap-portal-init");
	// Called before WiFiManager begins the connect-on-save attempt.
	// Use this to mark "connecting" so timeouts and status reflect reality even if connect fails.
	wifiManager.setPreSaveConfigCallback([&]() {
		notePortalActivity();
		portalStatus = portalStatusConnecting;
		portalConnectStart = millis();
		String submittedSsid;
		String submittedPass;
		if (wifiManager.server) {
			portalLog("WiFi submit args=%d uri=%s s='%s' p_len=%u",
			          wifiManager.server->args(),
			          wifiManager.server->uri().c_str(),
			          wifiManager.server->arg("s").c_str(),
			          static_cast<unsigned>(wifiManager.server->arg("p").length()));
			submittedSsid = wifiManager.server->arg("s");
			submittedPass = wifiManager.server->arg("p");
		}
		strlcpy(portalStatusSsid, submittedSsid.c_str(), sizeof(portalStatusSsid));
		strlcpy(portalSubmittedPass, submittedPass.c_str(), sizeof(portalSubmittedPass));
		if (portalApWifiSaveShouldPersistOnSubmit(
		        appConfig.wifiSSID.c_str(), submittedSsid.c_str(), submittedPass.c_str())) {
			String persistedPass = submittedPass;
			if (portalWifiSaveKeepsExistingPassword(
			        appConfig.wifiSSID.c_str(), submittedSsid.c_str(), submittedPass.c_str(), false)) {
				persistedPass = appConfig.wifiPass;
			}
			const bool credentialsChanged = appConfig.wifiSSID != submittedSsid || appConfig.wifiPass != persistedPass;
			appConfig.wifiSSID = submittedSsid;
			appConfig.wifiPass = persistedPass;
			persistUserWifiCredentials(appConfig.wifiSSID.c_str(), appConfig.wifiPass.c_str());
			portalWifiCredentialsChanged = portalWifiCredentialsChanged || credentialsChanged;
		}
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
		notePortalActivity();
		const String requestUri = wifiManager.server ? wifiManager.server->uri() : String();
		if (requestUri != "/paramsave" && requestUri != "/config/mqtt/save") {
#ifdef DEBUG_OVER_SERIAL
			portalLog("Ignoring saveParams callback on uri=%s", requestUri.c_str());
#endif
			return;
		}
		if (!portalRequestHasMqttFields(wifiManager)) {
#ifdef DEBUG_OVER_SERIAL
			portalLog("Ignoring saveParams callback without MQTT fields.");
#endif
			return;
		}
		int port = strtol(gPortalMqttPort.getValue(), NULL, 10);
		if (port < 0 || port > SHRT_MAX) {
			port = 0;
		}
		persistUserMqttConfig(gPortalMqttServer.getValue(), port, gPortalMqttUser.getValue(), gPortalMqttPass.getValue());
		if (inverterLabelOverrideIsValid(gPortalInverterLabel.getValue())) {
			persistUserInverterLabel(gPortalInverterLabel.getValue());
			appConfig.inverterLabel = gPortalInverterLabel.getValue();
		}

		portalMqttSaved = true;
		portalNeedsMqttConfig = !mqttConfigIsComplete(
			gPortalMqttServer.getValue(), static_cast<uint16_t>(port), gPortalMqttUser.getValue(), gPortalMqttPass.getValue());
	#ifdef DEBUG_OVER_SERIAL
		portalLog("MQTT params saved (server=%s)", gPortalMqttServer.getValue());
	#endif
	});
	wifiManager.setWebServerCallback([&]() {
		bindPortalRoutes(wifiManager);
	});
	wifiManager.setConfigPortalBlocking(false);
	wifiManager.startConfigPortal(deviceName);

#ifdef DEBUG_OVER_SERIAL
	IPAddress ip = WiFi.softAPIP();
	portalLog("Config portal SSID: %s", deviceName);
	portalLog("Config portal IP: %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
	portalLog("AP portal started server=%p", wifiManager.server ? static_cast<void *>(wifiManager.server.get()) : nullptr);
	portalLog("Portal loop start free=%u max=%u frag=%u",
		ESP.getFreeHeap(),
		ESP.getMaxFreeBlockSize(),
		ESP.getHeapFragmentation());
#endif

	unsigned long portalStatsLast = 0;
	for (;;) {
		bindPortalRoutes(wifiManager);
		unsigned long processStart = millis();
		wifiManager.process();
		bindPortalRoutes(wifiManager);
		servicePortalRouteRebindRetries(wifiManager);
		processPendingPollingConfigPayload();
			unsigned long processElapsed = millis() - processStart;
#ifdef DEBUG_OVER_SERIAL
			(void)processElapsed;
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
#else
			(void)processElapsed;
#endif

		if (portalStatus == portalStatusConnecting) {
				if (WiFi.status() == WL_CONNECTED) {
					portalStatus = portalStatusSuccess;
					portalWifiCredentialsChanged =
						syncPortalWifiCredentials(&wifiManager, portalStatusSsid, portalSubmittedPass) ||
						portalWifiCredentialsChanged;
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

				char storedMqttServer[kPrefMqttServerMaxLen] = "";
				char storedMqttUser[kPrefMqttUsernameMaxLen] = "";
				char storedMqttPass[kPrefMqttPasswordMaxLen] = "";
				uint16_t storedMqttPort = 0;
				{
					Preferences prefsRo;
					prefsRo.begin(DEVICE_NAME, true);
					storedMqttPort = static_cast<uint16_t>(prefsRo.getInt("MQTT_Port", 0));
					prefsRo.getString("MQTT_Server", storedMqttServer, sizeof(storedMqttServer));
					prefsRo.getString("MQTT_Username", storedMqttUser, sizeof(storedMqttUser));
					prefsRo.getString("MQTT_Password", storedMqttPass, sizeof(storedMqttPass));
					prefsRo.end();
				}
				PortalPostWifiAction postWifiAction = portalPostWifiActionAfterWifiSave(
					storedMqttServer, storedMqttPort, storedMqttUser, storedMqttPass);
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

				// After AP onboarding succeeds, keep the AP portal up briefly so status clients can
				// observe the connected result and the runtime IP before we hand off to the next mode.
				// - MQTT configured: reboot straight to normal runtime.
				// - MQTT missing: reboot into the STA-only WiFi config portal on the known LAN IP.
				unsigned long statusStart = millis();
					while (millis() - statusStart < 10000) {
						wifiManager.process();
						diagDelay(50);
					}
					if (postWifiAction == PortalPostWifiAction::Reboot) {
						setBootIntentAndReboot(portalNormalRebootIntent());
					} else {
						setBootIntentAndReboot(BootIntent::WifiConfig);
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
				invalidatePortalRouteBinding("ap-connect-failed");
				updateOLED(false, "Web", "config", "failed");
				// Stay in the portal on failure so the device remains reachable for retries.
				portalConnectStart = 0;
			}
		}

			// After MQTT params are saved, reboot into normal if persisted WiFi credentials exist.
			// The WiFi save path already writes credentials, so avoid rewriting them on each loop tick.
			// Do not block inside nested loops here; it can run in a non-yieldable context depending on
			// the WiFiManager call path and cause a core panic in __yield().
			if (portalMqttSaved && !portalNeedsMqttConfig && isWifiConfigComplete()) {
				if (!portalRebootScheduled) {
					schedulePortalReboot(portalNormalRebootIntent());
#ifdef DEBUG_OVER_SERIAL
					portalLog("MQTT configured and WiFi credentials present; reboot scheduled.");
#endif
				}
			}
			if (portalRebootScheduled && static_cast<long>(millis() - portalRebootAt) >= 0) {
				setBootIntentAndReboot(portalRebootIntent);
			}
			const uint32_t portalIdleMs = millis() - portalLastActivityAt;
			if (!portalRebootScheduled &&
			    shouldRebootNormalAfterApIdle(isWifiConfigComplete(), portalIdleMs)) {
#ifdef DEBUG_OVER_SERIAL
				portalLog("AP portal idle timeout after %lu ms; rebooting to normal retry.",
				          static_cast<unsigned long>(portalIdleMs));
#endif
				setBootIntentAndReboot(portalNormalRebootIntent());
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
#if RS485_STUB
	bool hadPendingStubControlAtLoopEntry = pendingRs485StubControlSet;
#endif

#ifdef BUTTON_PIN
	// Read button state
	if (digitalRead(BUTTON_PIN) == LOW) {
		configHandler();
	}
#endif // BUTTON_PIN

	const uint32_t loopNowMs = millis();
	loopSequence++;
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
			if (mqttSubsystemEnabled()) {
				mqttReconnect();
				requestHaDataResend();
			}
		} else {
			lastWifiConnected = true;
		}
	}

	if (mqttSubsystemEnabled()) {
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

#if RS485_STUB
	if (mqttSubsystemEnabled() &&
	    !hadPendingStubControlAtLoopEntry &&
	    pendingRs485StubControlSet) {
		// Apply stub-control updates on the next loop turn after the MQTT callback that queued them.
		// Keeping the mode transition off the same turn as packet dispatch avoids compounding stack
		// depth from PubSubClient processing with the online-transition control path.
		return;
	}
#endif

	if (mqttSubsystemEnabled()) {
#if RS485_STUB
		rs485StubControlProcessedThisLoop = false;
		if (servicePendingRs485StubControlInLoop()) {
			return;
		}
#endif
		serviceDeferredMqttWork();
#if RS485_STUB
		if (servicePendingRs485StubControlInLoop()) {
			return;
		}
		if (rs485StubControlProcessedThisLoop) {
			return;
		}
#endif
	}

#if RS485_STUB
	const bool rs485StubControlSchedulerCoolingDown =
		static_cast<long>(rs485StubControlSchedulerCooldownUntilMs - millis()) > 0;
#endif
	if (mqttSubsystemEnabled() &&
	    pendingPollingConfigPublish &&
	    _mqtt.connected() &&
	    !inMqttCallback &&
#if RS485_STUB
	    !rs485StubControlProcessedThisLoop &&
	    !rs485StubControlSchedulerCoolingDown
#else
	    true
#endif
	    ) {
		publishPollingConfig();
		pendingPollingConfigPublish = false;
	}

	if (httpControlPlaneEnabled) {
		if (g_httpServer != nullptr) {
			httpServerRef().handleClient();
		}
	}
	if (deferredControlPlaneRebootScheduled &&
	    static_cast<long>(millis() - deferredControlPlaneRebootAt) >= 0) {
		deferredControlPlaneRebootScheduled = false;
		setBootIntentAndReboot(deferredControlPlaneRebootIntent);
	}
	bool subscribedInverterTopicsThisLoop = false;
	if (mqttSubsystemEnabled() &&
#if RS485_STUB
	    !rs485StubControlProcessedThisLoop &&
	    !rs485StubControlSchedulerCoolingDown
#else
	    true
#endif
	    ) {
		subscribedInverterTopicsThisLoop = subscribeInverterTopics();
	}

	// Keep attempting RS485/inverter connection in the background. This must not block loop() so MQTT, HTTP,
	// and the scheduler remain responsive even when RS485 is disconnected.
	if (bootPlan.inverter) {
		rs485ProbeTick();
		serviceRs485BaudReconcile();
	}

	updateStatusLed();

	// Check and display the runstate on the display
	updateRunstate();

	// Send HA auto-discovery info
	if (!subscribedInverterTopicsThisLoop &&
	    mqttSubsystemEnabled() &&
#if RS485_STUB
	    !rs485StubControlSchedulerCoolingDown &&
#endif
	    resendHaData == true && _mqtt.connected()) {
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
	if (!subscribedInverterTopicsThisLoop &&
	    mqttSubsystemEnabled() &&
#if RS485_STUB
	    !rs485StubControlSchedulerCoolingDown
#else
	    true
#endif
	    ) {
		sendData();
	}
	if (!subscribedInverterTopicsThisLoop &&
	    bootPlan.inverter &&
	    mqttSubsystemEnabled() &&
#if RS485_STUB
	    !rs485StubControlSchedulerCoolingDown
#else
	    true
#endif
	    ) {
		dispatchService();
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

static bool
portalRequestHasMqttFields(WiFiManager &wifiManager)
{
	if (!wifiManager.server) {
		return false;
	}
	return wifiManager.server->hasArg("server") ||
	       wifiManager.server->hasArg("port") ||
	       wifiManager.server->hasArg("user") ||
	       wifiManager.server->hasArg("mpass") ||
	       wifiManager.server->hasArg("inverter_label");
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
	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	modbusRequestAndResponseStatusValues result;
	const char *fallback = "01/Jan/1970 00:00:00";

	if (_registerHandler != NULL && response != nullptr) {
		*response = modbusRequestAndResponse{};
		result = _registerHandler->readHandledRegister(REG_CUSTOM_SYSTEM_DATE_TIME, response);
		if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess &&
		    response->dataValueFormatted[0] != 0) {
			bool printable = true;
			for (size_t i = 0; response->dataValueFormatted[i] != '\0'; i++) {
				const unsigned char ch = static_cast<unsigned char>(response->dataValueFormatted[i]);
				if (ch < 0x20 || ch > 0x7e) {
					printable = false;
					break;
				}
			}
			if (printable) {
				strlcpy(target, response->dataValueFormatted, size);
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
	if (!ensurePortalBucketsScratch()) {
		persistLoadOk = 0;
		persistLoadErr = 1;
		return;
	}
	Preferences preferences;
	const size_t entityCount = mqttEntitiesCount();
	BucketId *buckets = g_portalBucketsScratch;
	bool appliedBucketMap = false;
	bool migrateLegacyIndexBucketMap = false;
	ScopedCharBuffer migratedBucketMapBuffer(0);
	const char *persistedBucketMap = nullptr;

	persistLoadOk = 0;
	persistLoadErr = 0;
	persistUnknownEntityCount = 0;
	persistInvalidBucketCount = 0;
	persistDuplicateEntityCount = 0;

	preferences.begin(DEVICE_NAME, true);
	bool prefsOpen = true;
	auto recoverRuntimeLoad = [&](const char *reason,
	                              PollingLoadFailureKind failureKind = PollingLoadFailureKind::Transient) {
		if (prefsOpen) {
			preferences.end();
			prefsOpen = false;
		}
		persistLoadOk = 0;
		persistLoadErr = 1;
		if (!recoverPollingConfigLoadToDefaults(
			    reason, buckets, entityCount, pollIntervalSeconds, failureKind)) {
			return;
		}
		if (!mqttEntityApplyBuckets(buckets, entityCount)) {
			return;
		}
		recomputeBucketCounts();
		pollingConfigLoadedFromStorage = shouldMarkRecoveredPollingConfigLoaded(failureKind);
	};
	const bool storedBucketMapPresent = preferences.isKey(kPreferenceBucketMap);
	const size_t bucketMapBufferSize = storedBucketMapPresent
	                                     ? preferenceStringBufferLen(
	                                           preferences, kPreferenceBucketMap, kPrefBucketMapMaxLen)
	                                     : kPrefBucketMapMaxLen;
	ScopedCharBuffer bucketMapBuffer(bucketMapBufferSize);
	if (!bucketMapBuffer.ok()) {
		recoverRuntimeLoad("polling config alloc");
		return;
	}
	char *bucketMap = bucketMapBuffer.data;

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
		mqttState entity{};
		if (!mqttEntityCopyByIndex(i, &entity)) {
			recoverRuntimeLoad("polling config catalog");
			return;
		}
		buckets[i] = bucketIdFromFreq(entity.updateFreq);
	}

	ScopedEntityCatalogCopy catalog;
	const mqttState *entities = nullptr;
	auto ensureCatalog = [&]() -> bool {
		if (entities != nullptr) {
			return true;
		}
		if (!catalog.load()) {
			return false;
		}
		entities = catalog.entities;
		return true;
	};

	bucketMap[0] = '\0';
	if (storedBucketMapPresent) {
		preferences.getString(kPreferenceBucketMap, bucketMap, bucketMapBuffer.size);
	}
	const bool legacyMigrated = preferences.getBool(kPreferenceBucketMapMigrated, false);
	if (bucketMap[0] != '\0') {
		if (bucketMapUsesDescriptorIndices(bucketMap)) {
			if (!ensureCatalog()) {
				recoverRuntimeLoad("polling config legacy catalog");
				return;
			}
			appliedBucketMap = applyLegacyBucketMapString(bucketMap,
			                                              entities,
			                                              entityCount,
			                                              buckets,
			                                              persistUnknownEntityCount,
			                                              persistInvalidBucketCount,
			                                              persistDuplicateEntityCount);
			if (appliedBucketMap) {
				if (!migratedBucketMapBuffer.reset(kPrefBucketMapMaxLen)) {
					recoverRuntimeLoad("polling config legacy migrate alloc");
					return;
				}
				size_t appliedCount = 0;
				if (buildBucketMapFromAssignments(
					    entities,
					    entityCount,
					    buckets,
					    migratedBucketMapBuffer.data,
					    migratedBucketMapBuffer.size,
					    appliedCount)) {
					migrateLegacyIndexBucketMap = true;
					persistedBucketMap = migratedBucketMapBuffer.data;
				}
				persistLoadOk = 1;
			} else {
				recoverRuntimeLoad(
					"polling config legacy apply", PollingLoadFailureKind::PersistedBucketMapCorrupt);
				return;
			}
		} else {
			appliedBucketMap = portalApplyBucketMapString(bucketMap,
			                                              entityCount,
			                                              buckets,
			                                              persistUnknownEntityCount,
			                                              persistInvalidBucketCount,
			                                              persistDuplicateEntityCount);
			if (appliedBucketMap) {
				persistLoadOk = 1;
			} else {
				recoverRuntimeLoad(
					"polling config apply", PollingLoadFailureKind::PersistedBucketMapCorrupt);
				return;
			}
		}
	} else if (!legacyMigrated) {
		if (!ensureCatalog()) {
			recoverRuntimeLoad("polling config migrate catalog");
			return;
		}
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
				recoverRuntimeLoad("polling config migrate apply");
				return;
			}
		} else {
			persistLoadOk = 1;
		}
	} else {
		persistLoadOk = 1;
	}

	preferences.end();
	prefsOpen = false;
	if (!mqttEntityApplyBuckets(buckets, entityCount)) {
		recoverRuntimeLoad("polling config apply runtime");
		return;
	}
	if (migrateLegacyIndexBucketMap && persistedBucketMap != nullptr) {
		persistUserBucketMap(persistedBucketMap);
	}
	recomputeBucketCounts();
	pollingConfigLoadedFromStorage = true;
}

static bool
buildPersistedPollingConfigMap(const BucketId *buckets, char *map, size_t mapLen)
{
	ScopedEntityCatalogCopy catalog;
	if (!catalog.load()) {
		return false;
	}
	return buildPersistedPollingConfigMapForCatalog(catalog.entities, catalog.count, buckets, map, mapLen);
}

static bool
buildPersistedPollingConfigMapForCatalog(const mqttState *entities,
                                         size_t entityCount,
                                         const BucketId *buckets,
                                         char *map,
                                         size_t mapLen)
{
	if (buckets == nullptr || map == nullptr || mapLen == 0 || entities == nullptr || entityCount == 0) {
		return false;
	}
	map[0] = '\0';
	size_t appliedCount = 0;
	return buildBucketMapFromAssignments(entities, entityCount, buckets, map, mapLen, appliedCount);
}

static bool
streamMqttWrite(const char *data, size_t len)
{
	if (data == nullptr) {
		return false;
	}
	if (len == 0) {
		return true;
	}
	return _mqtt.write(reinterpret_cast<const uint8_t *>(data), len) == len;
}

struct CountedMqttPayload {
	bool counting = true;
	bool ok = true;
	size_t length = 0;
};

static bool
appendCountedMqttText(CountedMqttPayload &payload, const char *text)
{
	if (text == nullptr) {
		payload.ok = false;
		return false;
	}
	const size_t len = strlen(text);
	if (payload.counting) {
		payload.length += len;
		return true;
	}
	if (!streamMqttWrite(text, len)) {
		payload.ok = false;
		return false;
	}
	payload.length += len;
	return true;
}

static bool
appendCountedMqttFmt(CountedMqttPayload &payload, char *scratch, size_t scratchSize, const char *fmt, ...)
{
	if (scratch == nullptr || scratchSize == 0 || fmt == nullptr) {
		payload.ok = false;
		return false;
	}
	va_list args;
	va_start(args, fmt);
	const int written = vsnprintf(scratch, scratchSize, fmt, args);
	va_end(args);
	if (written < 0 || static_cast<size_t>(written) >= scratchSize) {
		payload.ok = false;
		return false;
	}
	return appendCountedMqttText(payload, scratch);
}

using CountedMqttEmitter = bool (*)(CountedMqttPayload&, void *);

static bool
publishCountedMqttPayload(const char *topic, bool retain, CountedMqttEmitter emit, void *context)
{
	static unsigned long lastFailureLogMs = 0;
	const unsigned long nowMs = millis();
	if (topic == nullptr || emit == nullptr) {
		return false;
	}
	if (!_mqtt.connected()) {
#ifdef DEBUG_OVER_SERIAL
		if ((nowMs - lastFailureLogMs) >= 3000) {
			lastFailureLogMs = nowMs;
			Serial.printf("MQTT streamed publish skipped (disconnected): topic=%s\r\n", topic);
		}
#endif
		maybeYield();
		return false;
	}

	CountedMqttPayload countPass{};
	if (!emit(countPass, context) || !countPass.ok) {
		return false;
	}
	noteTrackedMqttPayload(currentRuntimeDiagPayloadKind, countPass.length);
	if (countPass.length == 0) {
		const bool published = _mqtt.publish(topic, "", retain);
		noteRuntimePhaseObservation(currentRuntimeDiagPhase);
		if (published) {
			noteMqttActivityPulse();
		}
		return published;
	}
	if (!_mqtt.beginPublish(topic, static_cast<unsigned int>(countPass.length), retain)) {
#ifdef DEBUG_OVER_SERIAL
		if ((nowMs - lastFailureLogMs) >= 3000) {
			lastFailureLogMs = nowMs;
			Serial.printf("MQTT beginPublish failed: topic=%s bytes=%u\r\n",
			              topic,
			              static_cast<unsigned>(countPass.length));
		}
#endif
		noteRuntimePhaseObservation(currentRuntimeDiagPhase);
		maybeYield();
		return false;
	}

	CountedMqttPayload streamPass{};
	streamPass.counting = false;
	const bool emitOk = emit(streamPass, context) && streamPass.ok;
	const bool endOk = _mqtt.endPublish();
	if (!emitOk || !endOk) {
#ifdef DEBUG_OVER_SERIAL
		if ((nowMs - lastFailureLogMs) >= 3000) {
			lastFailureLogMs = nowMs;
			Serial.printf("MQTT streamed publish failed: topic=%s bytes=%u emit_ok=%d end_ok=%d\r\n",
			              topic,
			              static_cast<unsigned>(countPass.length),
			              emitOk ? 1 : 0,
			              endOk ? 1 : 0);
		}
#endif
		noteRuntimePhaseObservation(currentRuntimeDiagPhase);
		maybeYield();
		return false;
	}
	noteRuntimePhaseObservation(currentRuntimeDiagPhase);
	noteMqttActivityPulse();
	return true;
}

static bool
emitPollingConfigPrelude(CountedMqttPayload &payload)
{
	char addition[256];
	bool first = true;
	if (!appendCountedMqttText(payload, "{")) {
		return false;
	}
	if (!appendCountedMqttFmt(payload,
	                          addition,
	                          sizeof(addition),
	                          "\"last_change\": \"%s\", \"poll_interval_s\": %lu, \"allowed_intervals\": [",
	                          _pollingConfigLastChange,
	                          static_cast<unsigned long>(pollIntervalSeconds))) {
		return false;
	}
	for (size_t i = 0; i < _pollingAllowedIntervalCount; i++) {
		if (!appendCountedMqttFmt(payload,
		                          addition,
		                          sizeof(addition),
		                          "%s\"%s\"",
		                          first ? "" : ", ",
		                          _pollingAllowedIntervals[i])) {
			return false;
		}
		first = false;
	}
	return true;
}

struct PollingConfigChunkPayloadContext {
	size_t chunkIndex = 0;
	size_t chunkCount = 0;
	const char *chunkMap = nullptr;
};

static bool
emitPollingConfigChunkPayload(CountedMqttPayload &payload, void *context)
{
	auto &chunk = *static_cast<PollingConfigChunkPayloadContext *>(context);
	char addition[256];
	return appendCountedMqttText(payload, "{") &&
	       appendCountedMqttFmt(payload,
	                            addition,
	                            sizeof(addition),
	                            "\"chunk_index\": %lu, \"chunk_count\": %lu, \"active_bucket_map\": \"",
	                            static_cast<unsigned long>(chunk.chunkIndex),
	                            static_cast<unsigned long>(chunk.chunkCount)) &&
	       appendCountedMqttText(payload, chunk.chunkMap) &&
	       appendCountedMqttText(payload, "\"}");
}

struct PollingConfigChunkSummaryContext {
	size_t chunkCount = 0;
	size_t activeCount = 0;
};

static bool
emitPollingConfigChunkSummary(CountedMqttPayload &payload, void *context)
{
	auto &summary = *static_cast<PollingConfigChunkSummaryContext *>(context);
	char addition[256];
	return emitPollingConfigPrelude(payload) &&
	       appendCountedMqttFmt(payload,
	                            addition,
	                            sizeof(addition),
	                            "], \"entity_intervals_encoding\": \"bucket_map_chunks\", \"entity_intervals_chunks\": %lu, \"entity_intervals_count\": %lu}",
	                            static_cast<unsigned long>(summary.chunkCount),
	                            static_cast<unsigned long>(summary.activeCount));
}

struct PollingConfigInlinePayloadContext {
	size_t entityCount = 0;
	const BucketId *buckets = nullptr;
};

static bool
emitPollingConfigInlinePayload(CountedMqttPayload &payload, void *context)
{
	auto &inlinePayload = *static_cast<PollingConfigInlinePayloadContext *>(context);
	char addition[256];
	bool first = true;
	if (!emitPollingConfigPrelude(payload) ||
	    !appendCountedMqttText(payload, "], \"entity_intervals\": {")) {
		return false;
	}

	for (size_t i = 0; i < inlinePayload.entityCount; i++) {
		if (inlinePayload.buckets[i] == BucketId::Disabled) {
			continue;
		}
		mqttState entity{};
		if (!mqttEntityCopyByIndex(i, &entity)) {
			return false;
		}
		if (!mqttEntityIncludedInPublicSurface(&entity)) {
			continue;
		}
		char entityName[64];
		mqttEntityNameCopy(&entity, entityName, sizeof(entityName));
		if (!appendCountedMqttFmt(payload,
		                          addition,
		                          sizeof(addition),
		                          "%s\"%s\": \"%s\"",
		                          first ? "" : ", ",
		                          entityName,
		                          bucketIdToString(inlinePayload.buckets[i]))) {
			return false;
		}
		first = false;
	}
	return appendCountedMqttText(payload, "}}");
}

static bool
publishPollingConfigChunkClear(size_t startIndex, size_t endIndex)
{
	char topic[128];
	for (size_t i = startIndex; i < endIndex; ++i) {
		snprintf(topic, sizeof(topic), "%s/config/entity_intervals/%lu",
		         deviceName,
		         static_cast<unsigned long>(i));
		if (!_mqtt.publish(topic, "", MQTT_RETAIN)) {
			return false;
		}
		noteMqttActivityPulse();
		maybeYield();
	}
	return true;
}

static bool
publishPollingConfigChunked(const mqttState *entities, size_t entityCount, const BucketId *buckets)
{
	ScopedCharBuffer chunkMap(kPollingConfigChunkMapMaxLen);
	if (!chunkMap.ok()) {
		return false;
	}
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
		                                              chunkMap.data,
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
		                                              chunkMap.data,
		                                              kPollingConfigChunkMapMaxLen,
		                                              nextIndex,
		                                              appliedCount) ||
		    appliedCount == 0) {
			return false;
		}

		PollingConfigChunkPayloadContext chunkPayload{ chunkIndex, chunkCount, chunkMap.data };
		snprintf(topic,
		         sizeof(topic),
		         "%s/config/entity_intervals/%lu",
		         deviceName,
		         static_cast<unsigned long>(chunkIndex));
		if (!publishCountedMqttPayload(topic, MQTT_RETAIN, emitPollingConfigChunkPayload, &chunkPayload)) {
			return false;
		}
		startIndex = nextIndex;
		maybeYield();
	}

	if (g_lastPublishedPollingConfigChunkCount > chunkCount &&
	    !publishPollingConfigChunkClear(chunkCount, g_lastPublishedPollingConfigChunkCount)) {
		return false;
	}

	PollingConfigChunkSummaryContext summary{ chunkCount, activeCount };
	snprintf(topic, sizeof(topic), "%s/config", deviceName);
	if (!publishCountedMqttPayload(topic, MQTT_RETAIN, emitPollingConfigChunkSummary, &summary)) {
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
	const size_t entityCount = mqttEntitiesCount();
	if (entityCount == 0 || entityCount > kMqttEntityDescriptorCount) {
		return;
	}
	BucketId *buckets = g_portalBucketsScratch;
	if (!mqttEntityCopyBuckets(buckets, entityCount)) {
		return;
	}

	char configTopic[64];
	snprintf(configTopic, sizeof(configTopic), "%s/config", deviceName);

	PollingConfigInlinePayloadContext inlinePayload{ entityCount, buckets };
	if (publishCountedMqttPayload(configTopic, MQTT_RETAIN, emitPollingConfigInlinePayload, &inlinePayload)) {
		if (g_lastPublishedPollingConfigChunkCount > 0 &&
		    !publishPollingConfigChunkClear(0, g_lastPublishedPollingConfigChunkCount)) {
			return;
		}
		g_lastPublishedPollingConfigChunkCount = 0;
		return;
	}

	ScopedEntityCatalogCopy catalog;
	if (!catalog.load()) {
#ifdef DEBUG_OVER_SERIAL
		Serial.println("publishPollingConfig: catalog load failed for chunked fallback");
#endif
		return;
	}
	BucketId *publicBuckets = new (std::nothrow) BucketId[entityCount];
	if (publicBuckets == nullptr) {
#ifdef DEBUG_OVER_SERIAL
		Serial.println("publishPollingConfig: public bucket scratch alloc failed for chunked fallback");
#endif
		return;
	}
	const size_t publicEntityCount = mqttEntityCopyCompactedPublicSurfaceAssignments(
		catalog.entities,
		buckets,
		entityCount,
		catalog.entities,
		publicBuckets);
	publishPollingConfigChunked(catalog.entities, publicEntityCount, publicBuckets);
	delete[] publicBuckets;
}

static bool
emitConfigDiscoveryPayload(CountedMqttPayload &payload, void * /* context */)
{
	const char *sensorName = "MQTT_Config";
	const char *prettyName = "MQTT Config";
	char addition[256];
	return appendCountedMqttText(payload, "{") &&
	       appendCountedMqttText(payload, "\"component\": \"sensor\"") &&
	       appendCountedMqttFmt(payload,
	                            addition,
	                            sizeof(addition),
	                            ", \"device\": {"
	                            " \"name\": \"%s\", \"model\": \"%s\", \"manufacturer\": \"AlphaESS\","
	                            " \"identifiers\": [\"%s\"]}",
	                            deviceName,
	                            kControllerModel,
	                            controllerIdentifier) &&
	       appendCountedMqttFmt(payload, addition, sizeof(addition), ", \"name\": \"%s\"", prettyName) &&
	       appendCountedMqttFmt(payload,
	                            addition,
	                            sizeof(addition),
	                            ", \"unique_id\": \"%s_%s\"",
	                            controllerIdentifier,
	                            sensorName) &&
	       appendCountedMqttFmt(payload,
	                            addition,
	                            sizeof(addition),
	                            ", \"state_topic\": \"%s/config\""
	                            ", \"value_template\": \"{{ value_json.last_change | default(\\\"\\\") }}\""
	                            ", \"json_attributes_topic\": \"%s/config\""
	                            ", \"entity_category\": \"diagnostic\"",
	                            deviceName,
	                            deviceName) &&
	       appendCountedMqttText(payload, "}");
}

bool
publishConfigDiscovery(void)
{
	char topic[128];
	snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", controllerIdentifier, "MQTT_Config");
	return publishCountedMqttPayload(topic, MQTT_RETAIN, emitConfigDiscoveryPayload, nullptr);
}

static bool
emitControllerInverterSerialDiscoveryPayload(CountedMqttPayload &payload, void * /* context */)
{
	char addition[256];
	return appendCountedMqttText(payload, "{") &&
	       appendCountedMqttText(payload, "\"component\": \"sensor\"") &&
	       appendCountedMqttFmt(payload,
	                            addition,
	                            sizeof(addition),
	                            ", \"device\": { \"name\": \"%s\", \"model\": \"%s\", \"manufacturer\": \"AlphaESS\", \"identifiers\": [\"%s\"]}",
	                            deviceName,
	                            kControllerModel,
	                            controllerIdentifier) &&
	       appendCountedMqttText(payload, ", \"name\": \"Inverter Serial\"") &&
	       appendCountedMqttFmt(payload,
	                            addition,
	                            sizeof(addition),
	                            ", \"unique_id\": \"%s_%s\"",
	                            controllerIdentifier,
	                            kControllerInverterSerialEntity) &&
	       appendCountedMqttFmt(payload,
	                            addition,
	                            sizeof(addition),
	                            ", \"state_topic\": \"%s/%s/%s/state\", \"entity_category\": \"diagnostic\"",
	                            deviceName,
	                            controllerIdentifier,
	                            kControllerInverterSerialEntity) &&
	       appendCountedMqttText(payload, "}");
}

bool
publishControllerInverterSerialDiscovery(void)
{
	char topic[160];
	snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", controllerIdentifier, kControllerInverterSerialEntity);
	return publishCountedMqttPayload(topic, MQTT_RETAIN, emitControllerInverterSerialDiscoveryPayload, nullptr);
}

void
publishControllerInverterSerialState(void)
{
	static char lastPublishedPayload[40] = "";
	static unsigned long lastPublishedReconnectCount = 0;
	char topic[160];
	char payload[40];
	snprintf(topic, sizeof(topic), "%s/%s/%s/state", deviceName, controllerIdentifier, kControllerInverterSerialEntity);
	if (inverterReady && inverterSerialIsValid(deviceSerialNumber)) {
		strlcpy(payload, deviceSerialNumber, sizeof(payload));
	} else {
		strlcpy(payload, "unknown", sizeof(payload));
	}
	if (lastPublishedReconnectCount == mqttReconnectCount &&
	    strcmp(lastPublishedPayload, payload) == 0) {
		return;
	}
	if (_mqtt.publish(topic, payload, true)) {
		strlcpy(lastPublishedPayload, payload, sizeof(lastPublishedPayload));
		lastPublishedReconnectCount = mqttReconnectCount;
		noteMqttActivityPulse();
	}
}

void
requestHaDataResend(void)
{
	resendHaData = true;
	resendHaPreludePending = true;
	resendHaNextEntityIndex = 0;
}

static void
queueCurrentHaDiscoveryClears(void)
{
	if (controllerIdentifier[0] != '\0') {
		queueStaleControllerDiscoveryClear(controllerIdentifier);
	}
	if (haUniqueId[0] != '\0' && strcmp(haUniqueId, "A2M-UNKNOWN") != 0) {
		queueStaleInverterDiscoveryClear(haUniqueId);
	}
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
clearHaControllerExtraDiscovery(const char *deviceId)
{
	if (deviceId == nullptr || deviceId[0] == '\0') {
		return false;
	}

	char topic[160];
	snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", deviceId, "MQTT_Config");
	emptyPayload();
	if (!sendMqtt(topic, MQTT_RETAIN)) {
		return false;
	}

	snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", deviceId, kControllerInverterSerialEntity);
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

	const size_t entityCount = mqttEntitiesCount();
	size_t batchCount = 0;
	while (resendHaClearStaleInverterIndex < entityCount && batchCount < kHaDiscoveryBatchSize) {
		const size_t idx = resendHaClearStaleInverterIndex;
		mqttState entity{};
		if (!mqttEntityCopyByIndex(idx, &entity)) {
			return true;
		}
		if (mqttEntityScope(entity.entityId) != DiscoveryDeviceScope::Inverter) {
			resendHaClearStaleInverterIndex++;
			continue;
		}
		if (!clearHaEntityDiscovery(&entity,
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
publishPendingStaleControllerDiscoveryClears(void)
{
	if (g_controllerDiscoveryClearScratch == nullptr) {
		return false;
	}
	if (!resendHaClearStaleControllerPending ||
	    resendHaClearStaleControllerQueueIndex >= resendHaClearStaleControllerQueueCount ||
	    g_controllerDiscoveryClearScratch->deviceIds[resendHaClearStaleControllerQueueIndex][0] == '\0') {
		return false;
	}
	if (!mqttEntitiesRtAvailable()) {
		return false;
	}

	const size_t entityCount = mqttEntitiesCount();
	size_t batchCount = 0;
	while (resendHaClearStaleControllerIndex < entityCount && batchCount < kHaDiscoveryBatchSize) {
		const size_t idx = resendHaClearStaleControllerIndex;
		mqttState entity{};
		if (!mqttEntityCopyByIndex(idx, &entity)) {
			return true;
		}
		if (mqttEntityScope(entity.entityId) != DiscoveryDeviceScope::Controller) {
			resendHaClearStaleControllerIndex++;
			continue;
		}
		if (!clearHaEntityDiscovery(
		        &entity,
		        g_controllerDiscoveryClearScratch->deviceIds[resendHaClearStaleControllerQueueIndex])) {
			return true;
		}
		resendHaClearStaleControllerIndex++;
		batchCount++;
		maybeYield();
	}
	if (resendHaClearStaleControllerIndex < entityCount) {
		return true;
	}

	if (!clearHaControllerExtraDiscovery(
	        g_controllerDiscoveryClearScratch->deviceIds[resendHaClearStaleControllerQueueIndex])) {
		return true;
	}

	resendHaClearStaleControllerIndex = 0;
	g_controllerDiscoveryClearScratch->deviceIds[resendHaClearStaleControllerQueueIndex][0] = '\0';
	resendHaClearStaleControllerQueueIndex++;
	if (resendHaClearStaleControllerQueueIndex < resendHaClearStaleControllerQueueCount) {
		return true;
	}

	resendHaClearStaleControllerPending = false;
	resendHaClearStaleControllerQueueIndex = 0;
	resendHaClearStaleControllerQueueCount = 0;
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
	size_t idx = 0;
	if (!lookupEntityIndex(entity->entityId, &idx)) {
		return true;
	}
	mqttUpdateFreq effectiveFreq = mqttEntityEffectiveFreqByIndex(idx);
	const DiscoveryDeviceScope scope = mqttEntityScope(entity->entityId);
	const char *deviceId = discoveryDeviceIdForScope(scope);
	if (deviceId[0] == '\0') {
		return true;
	}
	if (!includeEntityInPublicSurfaces(*entity)) {
		return clearHaEntityDiscovery(entity, deviceId);
	}

	if (effectiveFreq == mqttUpdateFreq::freqNever) {
		return sendDataFromMqttState(entity, true);
	}

	if (effectiveFreq == mqttUpdateFreq::freqDisabled) {
		return clearHaEntityDiscovery(entity, deviceId);
	}

	return sendDataFromMqttState(entity, true);
}

bool
handlePollingConfigSet(char *payload)
{
	struct PollingConfigSetContext {
		bool anyChange = false;
		bool bucketAssignmentsChanged = false;
		bool bucketsApplied = false;
		bool pollIntervalChanged = false;
		size_t entityCount = 0;
		BucketId *buckets = nullptr;
		BucketId *originalBuckets = nullptr;
		uint32_t stagedPollInterval = kPollIntervalDefaultSeconds;
	};

	const size_t entityCount = mqttEntitiesCount();
	if (entityCount == 0 || entityCount > kMqttEntityDescriptorCount) {
		return false;
	}
	BucketId *buckets = g_portalBucketsScratch;
	PollingConfigSetContext ctx{};
	uint32_t stagedPollIntervalSeconds = clampPollInterval(pollIntervalSeconds);
	if (!loadPollingBucketsForPortal(nullptr, entityCount, buckets, stagedPollIntervalSeconds)) {
		persistLoadOk = 0;
		persistLoadErr = 1;
		return false;
	}
	ctx.stagedPollInterval = stagedPollIntervalSeconds;
	const bool bucketsLoaded = mqttEntitiesRtAvailable();
	BucketId originalBuckets[kMqttEntityDescriptorCount]{};
	if (bucketsLoaded) {
		memcpy(originalBuckets, buckets, sizeof(originalBuckets));
	}
	ctx.entityCount = entityCount;
	ctx.buckets = buckets;
	ctx.originalBuckets = originalBuckets;

	const bool parsed = visitMutablePollingConfigEntries(
		payload,
		[](const char *key, char *value, void *opaque) -> bool {
			PollingConfigSetContext &ctx = *static_cast<PollingConfigSetContext *>(opaque);
			bool handled = false;

				if (!strcmp(key, kPreferencePollInterval)) {
					uint32_t parsedInterval = 0;
					if (parseStrictUint32(value, kPollIntervalMaxSeconds, parsedInterval)) {
						uint32_t clamped = clampPollInterval(parsedInterval);
						if (clamped != pollIntervalSeconds) {
							ctx.stagedPollInterval = clamped;
							ctx.pollIntervalChanged = true;
						}
#ifdef DEBUG_OVER_SERIAL
						Serial.printf("config/set poll_interval parsed=%lu clamped=%lu changed=%u current=%lu\r\n",
						              static_cast<unsigned long>(parsedInterval),
						              static_cast<unsigned long>(clamped),
						              ctx.pollIntervalChanged ? 1U : 0U,
						              static_cast<unsigned long>(pollIntervalSeconds));
#endif
					}
					handled = true;
				}

			if (!strcmp(key, "bucket_map")) {
				BucketId beforeBuckets[kMqttEntityDescriptorCount]{};
				memcpy(beforeBuckets, ctx.buckets, ctx.entityCount * sizeof(BucketId));
				persistUnknownEntityCount = 0;
				persistInvalidBucketCount = 0;
				persistDuplicateEntityCount = 0;
				const bool applied = portalApplyBucketMapString(value,
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
#ifdef DEBUG_OVER_SERIAL
				Serial.printf("config/set bucket_map applied changed=%u unknown=%lu invalid=%lu duplicate=%lu\r\n",
				              ctx.bucketAssignmentsChanged ? 1U : 0U,
				              static_cast<unsigned long>(persistUnknownEntityCount),
				              static_cast<unsigned long>(persistInvalidBucketCount),
				              static_cast<unsigned long>(persistDuplicateEntityCount));
#endif
				handled = true;
			}

			if (!handled) {
				size_t idx = 0;
				if (mqttEntityIndexByName(key, &idx) && idx < ctx.entityCount) {
					BucketId bucket = bucketIdFromString(value);
					if (bucket != BucketId::Unknown) {
						if (ctx.buckets[idx] != bucket) {
							ctx.buckets[idx] = bucket;
							ctx.bucketAssignmentsChanged = true;
						}
#ifdef DEBUG_OVER_SERIAL
						Serial.printf("config/set entity=%s bucket=%s idx=%u changed=%u\r\n",
						              key,
						              value,
						              static_cast<unsigned>(idx),
						              ctx.bucketAssignmentsChanged ? 1U : 0U);
#endif
					}
#ifdef DEBUG_OVER_SERIAL
					else {
						Serial.printf("config/set entity=%s bucket=%s invalid\r\n", key, value);
					}
#endif
				}
#ifdef DEBUG_OVER_SERIAL
				else {
					Serial.printf("config/set entity=%s lookup missed\r\n", key);
				}
#endif
			}

			maybeYield();
			return true;
		},
	&ctx);
	if (!parsed) {
		return false;
	}
	const bool bucketsCanApply = !mqttEntitiesRtAvailable() || mqttEntityCanApplyBuckets(ctx.buckets, entityCount);
	size_t persistedMapAppliedCount = 0;
	size_t persistedMapLen = 0;
	if ((ctx.pollIntervalChanged || ctx.bucketAssignmentsChanged) &&
	    !portalEstimatePersistedBucketMap(ctx.buckets, ctx.entityCount, persistedMapLen, persistedMapAppliedCount)) {
		persistLoadOk = 0;
		persistLoadErr = 1;
		return false;
	}
	const size_t persistedMapCapacityResolved =
		(ctx.pollIntervalChanged || ctx.bucketAssignmentsChanged) ? (persistedMapLen + 1) : 0;
	ScopedCharBuffer persistedMap(persistedMapCapacityResolved);
	if (persistedMapCapacityResolved > 0 && !persistedMap.ok()) {
		persistLoadOk = 0;
		persistLoadErr = 1;
		return false;
	}
	if ((ctx.pollIntervalChanged || ctx.bucketAssignmentsChanged) &&
	    !portalBuildPersistedBucketMap(ctx.buckets,
	                                   ctx.entityCount,
	                                   persistedMap.data,
	                                   persistedMapCapacityResolved,
	                                   persistedMapAppliedCount)) {
		persistLoadOk = 0;
		persistLoadErr = 1;
		return false;
	}

	if (ctx.bucketAssignmentsChanged) {
		if (!bucketsCanApply) {
			persistLoadOk = 0;
			persistLoadErr = 1;
			return false;
		}

		if (mqttEntitiesRtAvailable() && !mqttEntityApplyBuckets(buckets, entityCount)) {
			persistLoadOk = 0;
			persistLoadErr = 1;
			return false;
		}
		ctx.bucketsApplied = mqttEntitiesRtAvailable();
	}
	if ((ctx.pollIntervalChanged || ctx.bucketAssignmentsChanged) &&
	    !persistUserPollingConfig(ctx.stagedPollInterval,
	                             ctx.bucketAssignmentsChanged ? persistedMap.data : nullptr)) {
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
#ifdef DEBUG_OVER_SERIAL
		Serial.printf("config/set apply: poll_interval=%lu buckets_changed=%u map='%s'\r\n",
		              static_cast<unsigned long>(pollIntervalSeconds),
		              ctx.bucketAssignmentsChanged ? 1U : 0U,
		              (persistedMapCapacityResolved > 0 && persistedMap.data != nullptr) ? persistedMap.data : "");
#endif
		recomputeBucketCounts();
		updatePollingLastChange();
		pollingConfigLoadedFromStorage = true;
		publishPollingConfig();
#ifdef DEBUG_OVER_SERIAL
		Serial.printf("config/set publish complete: poll_interval=%lu free=%u max=%u frag=%u\r\n",
		              static_cast<unsigned long>(pollIntervalSeconds),
		              ESP.getFreeHeap(),
		              ESP.getMaxFreeBlockSize(),
		              ESP.getHeapFragmentation());
#endif
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
	bool bootConnectPhase = initialConnect;
	const WifiRecoveryTiming recoveryTiming = wifiRecoveryTiming();
	unsigned long recoveryWindowStart = millis();
	clearWifiFailureTracking();

	// We start by connecting to a WiFi network
#ifdef DEBUG_OVER_SERIAL
	if (initialConnect) {
		snprintf(_debugOutput, sizeof(_debugOutput), "Connecting to %s", appConfig.wifiSSID.c_str());
	} else {
		snprintf(_debugOutput, sizeof(_debugOutput), "Reconnect to %s", appConfig.wifiSSID.c_str());
	}
	Serial.println(_debugOutput);
#endif
	if (initialConnect) {
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
		const unsigned long nowMs = millis();
		const WifiFailureClass failureClass = classifyWifiFailure(currentWifiFailureSignals());
		const uint32_t windowElapsedMs = nowMs - recoveryWindowStart;
		const uint32_t windowBudgetMs =
			bootConnectPhase ? recoveryTiming.bootValidationMs : recoveryTiming.runtimeValidationMs;
		if (windowElapsedMs >= windowBudgetMs) {
			if (bootConnectPhase) {
				if (shouldRebootApOnInitialWifiFailure(currentBootMode, failureClass)) {
					setBootIntentAndReboot(BootIntent::ApConfig);
				}
#ifdef DEBUG_OVER_SERIAL
				Serial.println(F("Initial WiFi validation expired without invalid-config classification; continuing reconnect."));
#endif
				bootConnectPhase = false;
			} else if (shouldRebootApOnRuntimeWifiFailure(currentBootMode,
			                                             isWifiConfigComplete(),
			                                             failureClass)) {
				setBootIntentAndReboot(BootIntent::ApConfig);
			}
			recoveryWindowStart = nowMs;
			clearWifiFailureTracking();
		}

		snprintf(line3, sizeof(line3), "WiFi %d ...", tries);
#ifdef BUTTON_PIN
		// Read button state
		if (digitalRead(BUTTON_PIN) == LOW) {
			configHandler();
		}
#endif // BUTTON_PIN

		if (tries % 50 == 0) {
			beginWifiStationWithStoredCredentials();

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

		if (bootConnectPhase) {
			updateOLED(false, "Connecting", line3, line4);
		} else {
			updateOLED(false, "Reconnect", line3, line4);
		}
		diagDelay(500);
	}

	clearWifiFailureTracking();

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
		if (mqttSubsystemEnabled()) {
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
	if (mqttSubsystemEnabled()) {
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
	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		clearRuntimeInverterIdentity();
		strlcpy(deviceBatteryType, "UNKNOWN", sizeof(deviceBatteryType));
		return modbusRequestAndResponseStatusValues::preProcessing;
	}
#ifndef DEBUG_NO_RS485
	uint32_t tries = 0;
	const uint8_t kMaxIdentityReadAttempts = 4;
#endif
	char oledLine3[OLED_CHARACTER_WIDTH];
	char oledLine4[OLED_CHARACTER_WIDTH];

#ifdef DEBUG_NO_RS485
	result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
	*response = modbusRequestAndResponse{};
	strcpy(response->dataValueFormatted, "AL9876543210987");
#else // DEBUG_NO_RS485
	// Get the serial number
	*response = modbusRequestAndResponse{};
	result = _registerHandler->readHandledRegister(REG_SYSTEM_INFO_R_EMS_SN_BYTE_1_2, response);

	// Keep retries bounded so startup cannot stall indefinitely when RS485 is unavailable.
	uint8_t serialAttempts = 0;
		while (((result != modbusRequestAndResponseStatusValues::readDataRegisterSuccess) ||
		        !inverterSerialIsValid(response->dataValueFormatted)) &&
		       (serialAttempts++ < kMaxIdentityReadAttempts)) {
			tries++;
		// Preserve the legacy total for syntactically invalid serial payloads while
		// keeping the split transport/other counters reserved for classified RS485 results.
		if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
			rs485Errors++;
		} else {
			recordRs485Error(result);
			}
			snprintf(oledLine4, sizeof(oledLine4), "%ld", tries);
			updateOLED(false, "Alpha sys", "not known", oledLine4);
			pumpMqttDuringSetup(250);
			*response = modbusRequestAndResponse{};
			result = _registerHandler->readHandledRegister(REG_SYSTEM_INFO_R_EMS_SN_BYTE_1_2, response);
		}
#endif // DEBUG_NO_RS485
	const bool liveSerialReadOk =
		(result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) &&
		inverterSerialIsValid(response->dataValueFormatted);
	if (liveSerialReadOk) {
		applyLiveInverterIdentity(response->dataValueFormatted);
	} else {
		clearRuntimeInverterIdentity();
	}

#ifdef DEBUG_NO_RS485
	result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
	*response = modbusRequestAndResponse{};
	strcpy(response->dataValueFormatted, "FAKE-BAT");
#else // DEBUG_NO_RS485
	// Get the Battery Type
	*response = modbusRequestAndResponse{};
	result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_TYPE, response);
	// Keep retries bounded so startup cannot stall indefinitely when RS485 is unavailable.
	uint8_t batteryAttempts = 0;
	while ((result != modbusRequestAndResponseStatusValues::readDataRegisterSuccess) &&
	       (batteryAttempts++ < kMaxIdentityReadAttempts)) {
		tries++;
		recordRs485Error(result);
		snprintf(oledLine4, sizeof(oledLine4), "%ld", tries);
			updateOLED(false, "Bat type", "not known", oledLine4);
			pumpMqttDuringSetup(250);
			*response = modbusRequestAndResponse{};
			result = _registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_TYPE, response);
		}
#endif // DEBUG_NO_RS485
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		strlcpy(deviceBatteryType, response->dataValueFormatted, sizeof(deviceBatteryType));
	} else {
		strlcpy(deviceBatteryType, "UNKNOWN", sizeof(deviceBatteryType));
	}

#ifndef DISABLE_DISPLAY
#ifdef LARGE_DISPLAY
	if (inverterSerialKnown()) {
		strlcpy(oledLine3, deviceSerialNumber, sizeof(oledLine3));
	} else {
		strlcpy(oledLine3, "Serial wait", sizeof(oledLine3));
	}
	strlcpy(oledLine4, deviceBatteryType, sizeof(oledLine4));
#else // LARGE_DISPLAY
	if (inverterSerialKnown()) {
		strlcpy(oledLine3, deviceSerialNumber, sizeof(oledLine3));
	} else {
		strlcpy(oledLine3, "Serial", sizeof(oledLine3));
	}
	strlcpy(oledLine4, deviceBatteryType, sizeof(oledLine4));
#endif // LARGE_DISPLAY
	updateOLED(false, "Hello", oledLine3, oledLine4);
#endif // DISABLE_DISPLAY

#ifdef DEBUG_OVER_SERIAL
	sprintf(_debugOutput, "Alpha Serial Number: %s", deviceSerialNumber);
	Serial.println(_debugOutput);
#endif

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

static BootIntent
portalRestartIntent(void)
{
	switch (currentBootMode) {
	case BootMode::ApConfig:
		return BootIntent::ApConfig;
	case BootMode::WifiConfig:
		return BootIntent::WifiConfig;
	default:
		return BootIntent::Normal;
	}
}

static BootIntent
portalNormalRebootIntent(void)
{
	return ((currentBootMode == BootMode::ApConfig || currentBootMode == BootMode::WifiConfig) &&
	        portalWifiCredentialsChanged)
		       ? BootIntent::PortalNormal
		       : BootIntent::Normal;
}



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
	char mqttReconnectLine3[OLED_CHARACTER_WIDTH] = "";

	bool subscribed = false;
	bool inverterSubscriptionsAdded = false;

	// Throttle reconnect attempts; do not block the main loop.
	const unsigned long nowMs = millis();
	if ((nowMs - lastAttemptMs) < 5000) {
		return;
	}
	lastAttemptMs = nowMs;

	initMqttEntitiesRtIfNeeded(true);
	if (shouldReloadPollingConfigFromStorage(pendingPollingConfigSet, pollingConfigLoadedFromStorage)) {
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
		logHeapFreeOnly("before WiFi guard");
#endif
		guardPendingAsyncWifiScanBeforeMqttReconnect();
		if (!shouldStartWifiScan(currentBootMode)) {
#ifdef DEBUG_OVER_SERIAL
			Serial.println(F("WiFi guard: scans disabled in NORMAL."));
#endif
		}
#ifdef DEBUG_OVER_SERIAL
		logHeapFreeOnly("after WiFi guard");
#endif
#endif

#ifdef DEBUG_OVER_SERIAL
		Serial.print("Attempting MQTT connection...");
#endif

		snprintf(mqttReconnectLine3, sizeof(mqttReconnectLine3), "MQTT %d ...", tries);
		updateOLED(false, "Connecting", mqttReconnectLine3, _version);
		diagDelay(100);

#ifdef DEBUG_OVER_SERIAL
		debugLogMqttReconnectProbe();
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
#ifdef DEBUG_OVER_SERIAL
			Serial.println("Connected MQTT");
#endif
			// Publish boot intent early; RS485 init can stall before periodic status messages.
			publishBootEventOncePerBoot();
			publishBootMemEventOncePerBoot();
			mqttReconnectCount++;
			lastMqttConnectMs = millis();
			lastMqttConnected = true;
			resendAllData = true;
			if (pendingWifiDisconnectEvent) {
				publishEvent(MqttEventCode::WifiDisconnect, "");
				pendingWifiDisconnectEvent = false;
			}
			if (pendingMqttDisconnectEvent) {
				publishEvent(MqttEventCode::MqttDisconnect, "");
				pendingMqttDisconnectEvent = false;
			}

			subscribed = subscribeMqttReconnectTopics(&inverterSubscriptionsAdded);

			// Subscribe or resubscribe to topics.
			if (subscribed) {
				inverterSubscriptionsSet = false;
				inverterCommandSubscriptionsSet = false;
				inverterDispatchSubscriptionSet = false;
				inverterNextCommandSubscriptionIndex = 0;
				completeSuccessfulMqttReconnect(attemptStart, tries, inverterSubscriptionsAdded);
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

static bool
lookupSubscription(char *entityName, mqttState *outEntity)
{
	if (entityName == nullptr) {
		return false;
	}
	return lookupSubscriptionSpan(entityName, strlen(entityName), outEntity);
}

static bool
lookupSubscriptionSpan(const char *entityName, size_t entityNameLen, mqttState *outEntity)
{
	if (!ensureRuntimeScratch()) {
		return false;
	}
	RuntimeScratch &scratch = *g_runtimeScratch;
	if (entityName == nullptr || outEntity == nullptr || entityNameLen == 0) {
		return false;
	}
	int numberOfEntities = static_cast<int>(mqttEntitiesCount());
	for (int i = 0; i < numberOfEntities; i++) {
		if (!mqttEntityCopyByIndex(static_cast<size_t>(i), &scratch.inverterSubscriptionEntity)) {
			continue;
		}
		if (!includeEntityInPublicSurfaces(scratch.inverterSubscriptionEntity) ||
		    !scratch.inverterSubscriptionEntity.subscribe) {
			continue;
		}
		mqttEntityNameCopy(&scratch.inverterSubscriptionEntity,
		                   scratch.inverterSubscriptionEntityKey,
		                   sizeof(scratch.inverterSubscriptionEntityKey));
		const size_t candidateLen = strlen(scratch.inverterSubscriptionEntityKey);
		if (candidateLen != entityNameLen) {
			continue;
		}
		if (memcmp(scratch.inverterSubscriptionEntityKey, entityName, entityNameLen) == 0) {
			*outEntity = scratch.inverterSubscriptionEntity;
			return true;
		}
	}
	return false;
}

static bool
lookupEntity(mqttEntityId entityId, mqttState *outEntity)
{
	return mqttEntityCopyById(entityId, outEntity);
}

static bool
lookupEntityIndex(mqttEntityId entityId, size_t *outIdx)
{
	if (outIdx == nullptr) {
		return false;
	}
	return mqttEntityIndexById(entityId, outIdx);
}

static bool
beginAggregateEventsJson(char *out, size_t outSize, unsigned int count)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}
	const int written = snprintf(out, outSize, "{ \"numEvents\": %u", count);
	return written >= 0 && static_cast<size_t>(written) < outSize;
}

static bool
appendAggregateHexField(char *out,
	                        size_t outSize,
	                        const char *label,
	                        uint16_t reg,
	                        uint32_t value)
{
	if (out == nullptr || outSize == 0 || label == nullptr) {
		return false;
	}
	const size_t used = strnlen(out, outSize);
	if (used >= outSize) {
		return false;
	}
	const int written = snprintf(out + used,
	                             outSize - used,
	                             ", \"%s (0x%04X)\": \"0x%08lX\"",
	                             label,
	                             reg,
	                             static_cast<unsigned long>(value));
	return written >= 0 && static_cast<size_t>(written) < (outSize - used);
}

static bool
finishAggregateEventsJson(char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}
	const size_t used = strnlen(out, outSize);
	if (used >= outSize) {
		return false;
	}
	const int written = snprintf(out + used, outSize - used, " }");
	return written >= 0 && static_cast<size_t>(written) < (outSize - used);
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
				if (!beginAggregateEventsJson(rs->dataValueFormatted, sizeof(rs->dataValueFormatted), count) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Battery Faults",
				                             REG_BATTERY_HOME_R_BATTERY_FAULT_1,
				                             bf) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Battery Faults 1",
				                             REG_BATTERY_HOME_R_BATTERY_FAULT_1_1,
				                             bf1) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Battery Faults 2",
				                             REG_BATTERY_HOME_R_BATTERY_FAULT_2_1,
				                             bf2) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Battery Faults 3",
				                             REG_BATTERY_HOME_R_BATTERY_FAULT_3_1,
				                             bf3) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Battery Faults 4",
				                             REG_BATTERY_HOME_R_BATTERY_FAULT_4_1,
				                             bf4) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Battery Faults 5",
				                             REG_BATTERY_HOME_R_BATTERY_FAULT_5_1,
				                             bf5) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Battery Faults 6",
				                             REG_BATTERY_HOME_R_BATTERY_FAULT_6_1,
				                             bf6) ||
				    !finishAggregateEventsJson(rs->dataValueFormatted, sizeof(rs->dataValueFormatted))) {
					result = modbusRequestAndResponseStatusValues::payloadExceededCapacity;
				}
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
				if (!beginAggregateEventsJson(rs->dataValueFormatted, sizeof(rs->dataValueFormatted), count) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Battery Warnings",
				                             REG_BATTERY_HOME_R_BATTERY_WARNING_1,
				                             bw) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Battery Warnings 1",
				                             REG_BATTERY_HOME_R_BATTERY_WARNING_1_1,
				                             bw1) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Battery Warnings 2",
				                             REG_BATTERY_HOME_R_BATTERY_WARNING_2_1,
				                             bw2) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Battery Warnings 3",
				                             REG_BATTERY_HOME_R_BATTERY_WARNING_3_1,
				                             bw3) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Battery Warnings 4",
				                             REG_BATTERY_HOME_R_BATTERY_WARNING_4_1,
				                             bw4) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Battery Warnings 5",
				                             REG_BATTERY_HOME_R_BATTERY_WARNING_5_1,
				                             bw5) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Battery Warnings 6",
				                             REG_BATTERY_HOME_R_BATTERY_WARNING_6_1,
				                             bw6) ||
				    !finishAggregateEventsJson(rs->dataValueFormatted, sizeof(rs->dataValueFormatted))) {
					result = modbusRequestAndResponseStatusValues::payloadExceededCapacity;
				}
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
				if (!beginAggregateEventsJson(rs->dataValueFormatted, sizeof(rs->dataValueFormatted), count) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Inverter Faults 1",
				                             REG_INVERTER_HOME_R_INVERTER_FAULT_1_1,
				                             if1) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Inverter Faults 2",
				                             REG_INVERTER_HOME_R_INVERTER_FAULT_2_1,
				                             if2)
#ifdef EMS_35_36
				    || !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                                "Inverter Faults Extended 1",
				                                REG_INVERTER_HOME_R_INVERTER_FAULT_EXTEND_1_1,
				                                ife1) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Inverter Faults Extended 2",
				                             REG_INVERTER_HOME_R_INVERTER_FAULT_EXTEND_2_1,
				                             ife2) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Inverter Faults Extended 3",
				                             REG_INVERTER_HOME_R_INVERTER_FAULT_EXTEND_3_1,
				                             ife3) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Inverter Faults Extended 4",
				                             REG_INVERTER_HOME_R_INVERTER_FAULT_EXTEND_4_1,
				                             ife4)
#endif // EMS_35_36
				    || !finishAggregateEventsJson(rs->dataValueFormatted, sizeof(rs->dataValueFormatted))) {
					result = modbusRequestAndResponseStatusValues::payloadExceededCapacity;
				}
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
				if (!beginAggregateEventsJson(rs->dataValueFormatted, sizeof(rs->dataValueFormatted), count) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Inverter Warnings 1",
				                             REG_INVERTER_HOME_R_INVERTER_WARNING_1_1,
				                             iw1) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "Inverter Warnings 2",
				                             REG_INVERTER_HOME_R_INVERTER_WARNING_2_1,
				                             iw2) ||
				    !finishAggregateEventsJson(rs->dataValueFormatted, sizeof(rs->dataValueFormatted))) {
					result = modbusRequestAndResponseStatusValues::payloadExceededCapacity;
				}
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
				if (!beginAggregateEventsJson(rs->dataValueFormatted, sizeof(rs->dataValueFormatted), count) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "System Faults",
				                             REG_SYSTEM_INFO_R_SYSTEM_FAULT,
				                             sf) ||
				    !appendAggregateHexField(rs->dataValueFormatted, sizeof(rs->dataValueFormatted),
				                             "System Faults 1",
				                             REG_SYSTEM_OP_R_SYSTEM_FAULT_1,
				                             sf1) ||
				    !finishAggregateEventsJson(rs->dataValueFormatted, sizeof(rs->dataValueFormatted))) {
					result = modbusRequestAndResponseStatusValues::payloadExceededCapacity;
				}
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
	case mqttEntityId::entityDispatchDuration:
		snprintf(rs->dataValueFormatted,
		         sizeof(rs->dataValueFormatted),
		         "%lu",
		         static_cast<unsigned long>(timedDispatchState.configuredDurationSeconds));
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entityDispatchRemaining:
		snprintf(rs->dataValueFormatted,
		         sizeof(rs->dataValueFormatted),
		         "%lu",
		         static_cast<unsigned long>(dispatchRemainingSeconds(timedDispatchState.acceptedAtMs,
		                                                            timedDispatchState.acceptedDurationSeconds,
		                                                            millis())));
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entityDispatchRequestStatus:
		strlcpy(rs->dataValueFormatted,
		        (g_dispatchRequestStatus != nullptr) ? g_dispatchRequestStatus : "",
		        sizeof(rs->dataValueFormatted));
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
	case mqttEntityId::entityLoadPwr:
		if (opData.essPvPower == INT32_MAX ||
		    opData.essGridPower == INT32_MAX ||
		    opData.essBatteryPower == INT16_MAX) {
			result = modbusRequestAndResponseStatusValues::readDataInvalidValue;
		} else {
			const int32_t loadPower =
				opData.essPvPower +
				opData.essGridPower +
				static_cast<int32_t>(opData.essBatteryPower);
			sprintf(rs->dataValueFormatted, "%ld", loadPower);
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
	case mqttEntityId::entityRs485TransportErrors:
		sprintf(rs->dataValueFormatted, "%lu", rs485TransportErrors);
		result = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		break;
	case mqttEntityId::entityRs485OtherErrors:
		sprintf(rs->dataValueFormatted, "%lu", rs485OtherErrors);
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
		recordRs485Error(result);
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
	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		if (resultAddedToPayload != nullptr) {
			*resultAddedToPayload = modbusRequestAndResponseStatusValues::preProcessing;
		}
		return modbusRequestAndResponseStatusValues::preProcessing;
	}
	modbusRequestAndResponseStatusValues result;

	// Read the register(s)/data
	*response = modbusRequestAndResponse{};
	result = readEntity(singleEntity, response);

	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		// Let the onward process also know if the buffer failed.
		*resultAddedToPayload = addToPayload(response->dataValueFormatted);
	} else {
		*resultAddedToPayload = modbusRequestAndResponseStatusValues::preProcessing;
	}
	return result;
}

static void __attribute__((noinline))
populateStatusPollSnapshot(StatusPollSnapshot &poll, bool includeEssSnapshot)
{
	const bool essSnapshotOkNow = includeEssSnapshot && essSnapshotValid;
	poll.wifiStatus = wifiStatusLabel(WiFi.status());
	poll.wifiStatusCode = static_cast<int>(WiFi.status());
	poll.wifiReconnects = wifiReconnectCount;
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
	poll.rs485ErrorCount = rs485Errors;
	poll.rs485TransportErrorCount = rs485TransportErrors;
	poll.rs485OtherErrorCount = rs485OtherErrors;
	poll.lastPollMs = lastPollMs;
	poll.lastOkTsMs = lastOkTsMs;
	poll.lastErrTsMs = lastErrTsMs;
	poll.lastErrCode = lastErrCode;
	poll.rs485ProbeLastAttemptMs = rs485ProbeLastAttemptMs;
	poll.rs485ProbeBackoffMs = (rs485ConnectState == Rs485ConnectState::Connected) ? 0 : rs485CycleBackoffMs;
	poll.rs485ConnectionEpoch = rs485RuntimeReconnect.connectionEpoch;
	poll.rs485BaudConfigured = rs485BaudTracker.hasConfiguredBaud ? rs485BaudTracker.configuredBaud : 0;
	poll.rs485BaudActual = rs485BaudTracker.actualBaud;
	poll.rs485BaudSync = rs485BaudSyncStateLabel(rs485BaudTracker.syncState);
	poll.rs485Backend =
#if RS485_STUB
		"stub";
#else
		"real";
#endif
	poll.essSnapshotLastOk = essSnapshotLastOk;
	poll.essSnapshotAttempts = essSnapshotAttemptCount;
	poll.essPowerSnapshotLastBuildMs = essPowerSnapshotLastBuildMs;
	poll.snapshotPublishSkipCount = snapshotPublishSkipCount;
#if RS485_STUB
	poll.rs485StubMode = _modBus ? _modBus->stubModeLabel() : "uninit";
	poll.rs485StubFailRemaining = _modBus ? _modBus->stubFailRemaining() : 0;
	poll.rs485StubWriteCount = _modBus ? _modBus->stubWriteCount() : 0;
	poll.rs485StubLastWriteStartReg = _modBus ? _modBus->stubLastWriteStartReg() : 0;
	poll.rs485StubLastWriteRegCount = _modBus ? _modBus->stubLastWriteRegCount() : 0;
	poll.rs485StubLastWriteMs = _modBus ? _modBus->stubLastWriteMs() : 0;
#else
	poll.rs485StubMode = "";
	poll.rs485StubFailRemaining = 0;
	poll.rs485StubWriteCount = 0;
	poll.rs485StubLastWriteStartReg = 0;
	poll.rs485StubLastWriteRegCount = 0;
	poll.rs485StubLastWriteMs = 0;
#endif
	poll.dispatchRequestQueuedMs = dispatchRequestQueuedMs;
	poll.dispatchLastRunMs = dispatchLastRunMs;
	poll.dispatchWaitDueToSnapshotMs = dispatchWaitDueToSnapshotMs;
	poll.dispatchQueueCoalesceCount = dispatchQueueCoalesceCount;
	poll.dispatchBlockCacheHitCount = dispatchBlockCacheHitCount;
	poll.pvBlockCacheHitCount = pvBlockCacheHitCount;
	poll.pvMeterCacheHitCount = pvMeterCacheHitCount;
	poll.dispatchLastSkipReason = dispatchLastSkipReason;
	poll.worstPhase = runtimeDiagPhaseName(runtimeDiag.worstValid ? runtimeDiag.worstPhase : RuntimeDiagPhase::None);
	poll.worstFreeHeapB = runtimeDiag.worstValid ? runtimeDiag.worstSample.freeB : 0;
	poll.worstMaxBlockB = runtimeDiag.worstValid ? runtimeDiag.worstSample.maxBlockB : 0;
	poll.worstFragPct = runtimeDiag.worstValid ? runtimeDiag.worstSample.fragPct : 0;
	poll.mqttMaxPayloadSeen = runtimeDiag.mqttMaxPayloadSeen;
	poll.mqttMaxPayloadKind = runtimeDiag.mqttMaxPayloadKind;
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
}

#if RS485_STUB
static bool __attribute__((noinline))
populateStatusStubSnapshot(StatusStubSnapshot &stub)
{
	if (_modBus == nullptr) {
		return false;
	}
	stub.stubReads = _modBus->stubReadCount();
	stub.stubWrites = _modBus->stubWriteCount();
	stub.stubUnknownReads = _modBus->stubUnknownRegisterReads();
	stub.socX10 = _modBus->stubBatterySocX10();
	stub.lastReadStartReg = _modBus->stubLastReadStartReg();
	stub.lastReadRegCount = _modBus->stubLastReadRegCount();
	stub.lastFn = _modBus->stubLastFn();
	stub.lastFailStartReg = _modBus->stubLastFailStartReg();
	stub.lastFailFn = _modBus->stubLastFailFn();
	stub.lastFailType = _modBus->stubLastFailTypeLabel();
	stub.lastWriteFailStartReg = _modBus->stubLastWriteFailStartReg();
	stub.lastWriteFailFn = _modBus->stubLastWriteFailFn();
	stub.lastWriteFailType = _modBus->stubLastWriteFailTypeLabel();
	stub.failRegister = _modBus->stubFailRegister();
	stub.failType = _modBus->stubFailTypeLabel();
	stub.latencyMs = _modBus->stubLatencyMs();
	stub.strictUnknown = _modBus->stubStrictUnknown();
	stub.failReads = _modBus->stubFailReads();
	stub.failWrites = _modBus->stubFailWrites();
	stub.failEveryN = _modBus->stubFailEveryN();
	stub.failForMs = _modBus->stubFailForMs();
	stub.flapOnlineMs = _modBus->stubFlapOnlineMs();
	stub.flapOfflineMs = _modBus->stubFlapOfflineMs();
	stub.probeAttempts = _modBus->stubProbeAttempts();
	stub.probeSuccessAfterN = _modBus->stubProbeSuccessAfterN();
	stub.socStepX10PerSnapshot = _modBus->stubSocStepX10PerSnapshot();
	return true;
}
#endif

static bool __attribute__((noinline))
publishStatusPollSnapshot(const StatusPollSnapshot &poll)
{
	if (!ensureStatusJsonScratch()) {
		return false;
	}
	char pollTopic[160];
	snprintf(pollTopic, sizeof(pollTopic), "%s/poll", statusTopic);
	bool pollBuilt = buildStatusPollJson(poll, g_statusJsonScratch, kStatusJsonScratchSize);
	bool usedCompactPoll = false;
	if (!pollBuilt) {
		pollBuilt = buildStatusPollJsonCompact(poll, g_statusJsonScratch, kStatusJsonScratchSize);
		usedCompactPoll = pollBuilt;
	}
	if (!pollBuilt) {
		return false;
	}
	RuntimeDiagScope diagScope(RuntimeDiagPhase::StatusPublish, "poll");
	bool published = publishTrackedTextPayload(pollTopic, g_statusJsonScratch, MQTT_RETAIN);
	if (!published && !usedCompactPoll &&
	    buildStatusPollJsonCompact(poll, g_statusJsonScratch, kStatusJsonScratchSize)) {
		published = publishTrackedTextPayload(pollTopic, g_statusJsonScratch, MQTT_RETAIN);
	}
#ifdef DEBUG_OVER_SERIAL
	if (!published) {
		Serial.println("status/poll publish failed (full+compact)");
	}
#endif
	maybeYield();
	return published;
}

#if RS485_STUB
static bool __attribute__((noinline))
publishStatusStubSnapshot(const StatusStubSnapshot &stub)
{
	if (!ensureStatusJsonScratch()) {
		return false;
	}
	char stubTopic[160];
	snprintf(stubTopic, sizeof(stubTopic), "%s/stub", statusTopic);
	if (!buildStatusStubJson(stub, g_statusJsonScratch, kStatusJsonScratchSize)) {
		return false;
	}
	if (_mqtt.publish(stubTopic, g_statusJsonScratch, MQTT_RETAIN)) {
		noteMqttActivityPulse();
		maybeYield();
		return true;
	}
	maybeYield();
	return false;
}

static bool
publishStubControlStatusNow(bool includeEssSnapshot)
{
	if (!_mqtt.connected() || !ensureStatusJsonScratch()) {
		return false;
	}
	// Stub-control acknowledgements avoid the full status builders because the extra locals and
	// Wi-Fi status path can trip the ESP8266 watchdog/stack guard during control transitions.
	const bool essSnapshotOkNow = includeEssSnapshot && essSnapshotValid;
	const char *stubMode = _modBus ? _modBus->stubModeLabel() : "uninit";
	const char *skipReason = dispatchLastSkipReason ? dispatchLastSkipReason : "";
	const int pollWritten = snprintf(
		g_statusJsonScratch,
		kStatusJsonScratchSize,
		"{"
		"\"rs485_backend\":\"stub\","
		"\"rs485_stub_mode\":\"%s\","
		"\"rs485_stub_fail_remaining\":%lu,"
		"\"rs485_stub_writes\":%lu,"
		"\"rs485_stub_last_write_reg\":%u,"
		"\"rs485_stub_last_write_reg_count\":%u,"
		"\"rs485_stub_last_write_ms\":%lu,"
		"\"dispatch_request_queued_ms\":%lu,"
		"\"inverter_ready\":%s,"
		"\"ess_snapshot_ok\":%s,"
		"\"ess_snapshot_last_ok\":%s,"
		"\"ess_snapshot_attempts\":%lu,"
		"\"dispatch_last_run_ms\":%lu,"
		"\"dispatch_last_skip_reason\":\"%s\","
		"\"poll_interval_s\":%lu,"
		"\"last_poll_ms\":%lu,"
		"\"poll_ok_count\":%lu,"
		"\"poll_err_count\":%lu,"
		"\"rs485_error_count\":%lu,"
		"\"rs485_transport_error_count\":%lu,"
		"\"rs485_other_error_count\":%lu,"
		"\"rs485_probe_last_attempt_ms\":%lu,"
		"\"rs485_probe_backoff_ms\":%lu,"
		"\"rs485_connection_epoch\":%lu,"
		"\"rs485_baud_configured\":%lu,"
		"\"rs485_baud_actual\":%lu,"
		"\"rs485_baud_sync\":\"%s\""
		"}",
		stubMode,
		static_cast<unsigned long>(_modBus ? _modBus->stubFailRemaining() : 0),
		static_cast<unsigned long>(_modBus ? _modBus->stubWriteCount() : 0),
		static_cast<unsigned>(_modBus ? _modBus->stubLastWriteStartReg() : 0),
		static_cast<unsigned>(_modBus ? _modBus->stubLastWriteRegCount() : 0),
		static_cast<unsigned long>(_modBus ? _modBus->stubLastWriteMs() : 0),
		static_cast<unsigned long>(dispatchRequestQueuedMs),
		inverterReady ? "true" : "false",
		essSnapshotOkNow ? "true" : "false",
		essSnapshotLastOk ? "true" : "false",
		static_cast<unsigned long>(essSnapshotAttemptCount),
		static_cast<unsigned long>(dispatchLastRunMs),
		skipReason,
		static_cast<unsigned long>(pollIntervalSeconds),
		static_cast<unsigned long>(lastPollMs),
		static_cast<unsigned long>(pollOkCount),
		static_cast<unsigned long>(pollErrCount),
		static_cast<unsigned long>(rs485Errors),
		static_cast<unsigned long>(rs485TransportErrors),
		static_cast<unsigned long>(rs485OtherErrors),
		static_cast<unsigned long>(rs485ProbeLastAttemptMs),
		static_cast<unsigned long>((rs485ConnectState == Rs485ConnectState::Connected) ? 0 : rs485CycleBackoffMs),
		static_cast<unsigned long>(rs485RuntimeReconnect.connectionEpoch),
		static_cast<unsigned long>(rs485BaudTracker.hasConfiguredBaud ? rs485BaudTracker.configuredBaud : 0),
		static_cast<unsigned long>(rs485BaudTracker.actualBaud),
		rs485BaudSyncStateLabel(rs485BaudTracker.syncState));
	if (pollWritten <= 0 || static_cast<size_t>(pollWritten) >= kStatusJsonScratchSize) {
		return false;
	}
	char pollTopic[160];
	snprintf(pollTopic, sizeof(pollTopic), "%s/poll", statusTopic);
	const bool pollPublished = publishTrackedTextPayload(pollTopic, g_statusJsonScratch, MQTT_RETAIN);
	maybeYield();

	const int stubWritten = snprintf(
		g_statusJsonScratch,
		kStatusJsonScratchSize,
		"{"
		"\"strict_unknown\":%s,"
		"\"fail_reads\":%s,"
		"\"fail_writes\":%s,"
		"\"fail_every_n\":%lu,"
		"\"fail_for_ms\":%lu,"
		"\"flap_online_ms\":%lu,"
		"\"flap_offline_ms\":%lu,"
		"\"probe_success_after_n\":%lu"
		"}",
		(_modBus && _modBus->stubStrictUnknown()) ? "true" : "false",
		(_modBus && _modBus->stubFailReads()) ? "true" : "false",
		(_modBus && _modBus->stubFailWrites()) ? "true" : "false",
		static_cast<unsigned long>(_modBus ? _modBus->stubFailEveryN() : 0),
		static_cast<unsigned long>(_modBus ? _modBus->stubFailForMs() : 0),
		static_cast<unsigned long>(_modBus ? _modBus->stubFlapOnlineMs() : 0),
		static_cast<unsigned long>(_modBus ? _modBus->stubFlapOfflineMs() : 0),
		static_cast<unsigned long>(_modBus ? _modBus->stubProbeSuccessAfterN() : 0));
	if (stubWritten <= 0 || static_cast<size_t>(stubWritten) >= kStatusJsonScratchSize) {
		return false;
	}
	char stubTopic[160];
	snprintf(stubTopic, sizeof(stubTopic), "%s/stub", statusTopic);
	const bool stubPublished = _mqtt.publish(stubTopic, g_statusJsonScratch, MQTT_RETAIN);
	if (stubPublished) {
		noteMqttActivityPulse();
	}
	maybeYield();
	return pollPublished && stubPublished;
}
#endif

void
sendStatus(bool includeEssSnapshot)
{
	if (!ensureStatusJsonScratch()) {
		return;
	}
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
	core.haUniqueId = inverterReady ? haUniqueId : "A2M-UNKNOWN";

	net.uptimeS = getUptimeSeconds();
	{
		noteRuntimePhaseObservation(RuntimeDiagPhase::StatusPublish);
		const MemSample netSample = readMemSample();
		noteRuntimeMemObservation(netSample);
		net.freeHeap = netSample.freeB;
		net.maxBlock = netSample.maxBlockB;
		net.fragPct = netSample.fragPct;
		net.minFreeHeap = runtimeDiag.minimaValid ? runtimeDiag.minFreeHeap : netSample.freeB;
		net.minMaxBlock = runtimeDiag.minimaValid ? runtimeDiag.minMaxBlock : netSample.maxBlockB;
		net.maxFragPct = runtimeDiag.minimaValid ? runtimeDiag.maxFragPct : netSample.fragPct;
	}
	net.rssiDbm = WiFi.RSSI();
	net.ip = ipBuf;
	net.ssid = ssidBuf;
	net.mqttConnected = _mqtt.connected();
	net.mqttReconnects = mqttReconnectCount;
	net.wifiStatus = wifiStatusLabel(WiFi.status());
	net.wifiStatusCode = static_cast<int>(WiFi.status());
	net.wifiReconnects = wifiReconnectCount;

	populateStatusPollSnapshot(poll, includeEssSnapshot);

	if (!buildStatusCoreJson(core, g_statusJsonScratch, kStatusJsonScratchSize)) {
		return;
	}
	resultAddedToPayload = addToPayload(g_statusJsonScratch);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return;
	}

	{
		RuntimeDiagScope diagScope(RuntimeDiagPhase::StatusPublish, "status");
		sendMqtt(statusTopic, MQTT_RETAIN);
	}
	maybeYield();
	publishControllerInverterSerialState();
	maybeYield();

	char netTopic[160];
	snprintf(netTopic, sizeof(netTopic), "%s/net", statusTopic);
	if (buildStatusNetJson(net, g_statusJsonScratch, kStatusJsonScratchSize)) {
		RuntimeDiagScope diagScope(RuntimeDiagPhase::StatusPublish, "net");
		publishTrackedTextPayload(netTopic, g_statusJsonScratch, MQTT_RETAIN);
		maybeYield();
	}

	publishStatusPollSnapshot(poll);

#if RS485_STUB
	if (populateStatusStubSnapshot(stub)) {
		publishStatusStubSnapshot(stub);
	}
#else
	static bool clearedRetainedStubStatus = false;
	if (!clearedRetainedStubStatus) {
		char stubTopic[160];
		snprintf(stubTopic, sizeof(stubTopic), "%s/stub", statusTopic);
		if (_mqtt.publish(stubTopic, "", MQTT_RETAIN)) {
			noteMqttActivityPulse();
			clearedRetainedStubStatus = true;
		}
		maybeYield();
	}
#endif
}

struct EntityDiscoveryPayloadContext {
	const mqttState *singleEntity = nullptr;
	DiscoveryDeviceScope scope = DiscoveryDeviceScope::Inverter;
	const char *topicBase = nullptr;
};

static bool
emitEntityDiscoveryPayload(CountedMqttPayload &payload, void *context)
{
	auto &ctx = *static_cast<EntityDiscoveryPayloadContext *>(context);
	const mqttState *singleEntity = ctx.singleEntity;
	const DiscoveryDeviceScope scope = ctx.scope;
	const char *topicBase = ctx.topicBase;
	char stateAddition[256];
	char prettyName[64];
	char metricId[64];
	char uniqueId[128];
	char deviceDisplayName[48];
	char labelDisplay[16];
	char labelId[16];
	char defaultEntityId[96];
	const char *deviceId = discoveryDeviceIdForScope(scope);
	const bool inverterScope = (scope == DiscoveryDeviceScope::Inverter);
	char entityKey[64];
	const char *entityType = "sensor";
	stateAddition[0] = '\0';
	if (singleEntity == nullptr || deviceId[0] == '\0' || topicBase == nullptr || topicBase[0] == '\0') {
		payload.ok = false;
		return false;
	}
	mqttEntityNameCopy(singleEntity, entityKey, sizeof(entityKey));
	buildEntityMetricId(singleEntity, metricId, sizeof(metricId));
	buildEntityUniqueId(scope,
	                    controllerIdentifier,
	                    deviceSerialNumber,
	                    (inverterScope && metricId[0] != '\0') ? metricId : entityKey,
	                    uniqueId,
	                    sizeof(uniqueId));

	A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT("{"));
	if (!appendCountedMqttText(payload, stateAddition)) {
		return false;
	}

	switch (singleEntity->haClass) {
	case homeAssistantClass::haClassBox:
	case homeAssistantClass::haClassNumber:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT("\"component\": \"number\""));
		entityType = "number";
		break;
	case homeAssistantClass::haClassSelect:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT("\"component\": \"select\""));
		entityType = "select";
		break;
	case homeAssistantClass::haClassBinaryProblem:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT("\"component\": \"binary_sensor\""));
		entityType = "binary_sensor";
		break;
	default:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT("\"component\": \"sensor\""));
		entityType = "sensor";
		break;
	}
	if (!appendCountedMqttText(payload, stateAddition)) {
		return false;
	}

	if (inverterScope) {
		if (!buildInverterDeviceDisplayName(deviceSerialNumber,
		                                    appConfig.inverterLabel.c_str(),
		                                    deviceDisplayName,
		                                    sizeof(deviceDisplayName))) {
			payload.ok = false;
			return false;
		}
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
		         A2M_FMT(", \"device\": {"
		                 " \"name\": \"%s\", \"model\": \"%s\", \"manufacturer\": \"AlphaESS\","
		                 " \"identifiers\": [\"%s\"], \"via_device\": \"%s\"}"),
		         deviceDisplayName,
		         (deviceBatteryType[0] != '\0' ? deviceBatteryType : kInverterModelFallback),
		         deviceId,
		         controllerIdentifier);
	} else {
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
		         A2M_FMT(", \"device\": {"
		                 " \"name\": \"%s\", \"model\": \"%s\", \"manufacturer\": \"AlphaESS\","
		                 " \"identifiers\": [\"%s\"]}"),
		         deviceName,
		         kControllerModel,
		         deviceId);
	}
	if (!appendCountedMqttText(payload, stateAddition)) {
		return false;
	}

	buildEntityDisplayName(singleEntity, scope, prettyName, sizeof(prettyName));
	A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"name\": \"%s\""), prettyName);
	if (!appendCountedMqttText(payload, stateAddition)) {
		return false;
	}

	if (inverterScope) {
		if (!buildInverterLabelDisplay(deviceSerialNumber,
		                               appConfig.inverterLabel.c_str(),
		                               labelDisplay,
		                               sizeof(labelDisplay))) {
			payload.ok = false;
			return false;
		}
		buildInverterLabelId(labelDisplay, labelId, sizeof(labelId));
		if (labelId[0] == '\0') {
			payload.ok = false;
			return false;
		}
		A2M_SNPRINTF(defaultEntityId, sizeof(defaultEntityId), A2M_FMT("%s.alpha_%s_%s"),
		         entityType,
		         labelId,
		         (metricId[0] != '\0' ? metricId : entityKey));
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
		         A2M_FMT(", \"default_entity_id\": \"%s\", \"has_entity_name\": true"),
		         defaultEntityId);
		if (!appendCountedMqttText(payload, stateAddition)) {
			return false;
		}
	}

	A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"unique_id\": \"%s\""), uniqueId);
	if (!appendCountedMqttText(payload, stateAddition)) {
		return false;
	}

	switch (singleEntity->haClass) {
	case homeAssistantClass::haClassEnergy:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"device_class\": \"energy\""
			         ", \"state_class\": \"total_increasing\""
			         ", \"unit_of_measurement\": \"kWh\""
#ifdef MQTT_FORCE_UPDATE
			         ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			        ));
		break;
	case homeAssistantClass::haClassPower:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"device_class\": \"power\""
			         ", \"state_class\": \"measurement\""
			         ", \"unit_of_measurement\": \"W\""
#ifdef MQTT_FORCE_UPDATE
			         ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			        ));
		break;
	case homeAssistantClass::haClassFrequency:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"device_class\": \"frequency\""
			         ", \"state_class\": \"measurement\""
			         ", \"unit_of_measurement\": \"Hz\""
#ifdef MQTT_FORCE_UPDATE
			         ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			         ", \"entity_category\": \"diagnostic\""));
		break;
	case homeAssistantClass::haClassReactivePower:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"state_class\": \"measurement\""
			         ", \"unit_of_measurement\": \"var\""
#ifdef MQTT_FORCE_UPDATE
			         ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			         ", \"entity_category\": \"diagnostic\""));
		break;
	case homeAssistantClass::haClassApparentPower:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"state_class\": \"measurement\""
			         ", \"unit_of_measurement\": \"VA\""
#ifdef MQTT_FORCE_UPDATE
			         ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			         ", \"entity_category\": \"diagnostic\""));
		break;
	case homeAssistantClass::haClassPowerFactor:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"state_class\": \"measurement\""
#ifdef MQTT_FORCE_UPDATE
			         ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			         ", \"entity_category\": \"diagnostic\""));
		break;
	case homeAssistantClass::haClassBinaryProblem:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"device_class\": \"problem\""
			         ", \"payload_on\": \"Problem\""
			         ", \"payload_off\": \"OK\""
			         ", \"entity_category\": \"diagnostic\""));
		break;
	case homeAssistantClass::haClassBattery:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"device_class\": \"battery\""
			         ", \"state_class\": \"measurement\""
			         ", \"unit_of_measurement\": \"%%\""
#ifdef MQTT_FORCE_UPDATE
			         ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			        ));
		break;
	case homeAssistantClass::haClassVoltage:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"device_class\": \"voltage\""
			         ", \"state_class\": \"measurement\""
			         ", \"unit_of_measurement\": \"V\""
#ifdef MQTT_FORCE_UPDATE
			         ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			        ));
		break;
	case homeAssistantClass::haClassCurrent:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"device_class\": \"current\""
			         ", \"state_class\": \"measurement\""
			         ", \"unit_of_measurement\": \"A\""
#ifdef MQTT_FORCE_UPDATE
			         ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			        ));
		break;
	case homeAssistantClass::haClassTemp:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"device_class\": \"temperature\""
			         ", \"state_class\": \"measurement\""
			         ", \"unit_of_measurement\": \"°C\""
#ifdef MQTT_FORCE_UPDATE
//			         ", \"force_update\": \"true\""
#endif // MQTT_FORCE_UPDATE
			         ", \"entity_category\": \"diagnostic\""));
		break;
	case homeAssistantClass::haClassDuration:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"device_class\": \"duration\""
			         ", \"state_class\": \"measurement\""
			         ", \"unit_of_measurement\": \"s\""
			         ", \"entity_category\": \"diagnostic\""));
		break;
	case homeAssistantClass::haClassCounter:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"state_class\": \"total_increasing\""
			         ", \"unit_of_measurement\": \"errors\""
			         ", \"entity_category\": \"diagnostic\""));
		break;
	case homeAssistantClass::haClassBox:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"mode\": \"box\""));
		break;
	case homeAssistantClass::haClassInfo:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"entity_category\": \"diagnostic\""));
		break;
	case homeAssistantClass::haClassSelect:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"device_class\": \"enum\""));
		break;
	case homeAssistantClass::haClassNumber:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"entity_category\": \"diagnostic\""
			         ", \"entity_type\": \"number\""));
		break;
	default:
		stateAddition[0] = '\0';
		break;
	}
	if (strlen(stateAddition) != 0 && !appendCountedMqttText(payload, stateAddition)) {
		return false;
	}

	stateAddition[0] = '\0';
	switch (singleEntity->entityId) {
	case mqttEntityId::entityRegNum:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"entity_category\": \"diagnostic\""
			         ", \"icon\": \"mdi:pound\""
			         ", \"min\": -1, \"max\": 41000"));
		break;
	case mqttEntityId::entityRegValue:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:folder-pound-outline\""));
		break;
	case mqttEntityId::entityGridReg:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:security\""));
		break;
	case mqttEntityId::entityInverterMode:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:format-list-numbered\""));
		break;
	case mqttEntityId::entityPvPwr:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:solar-power\""));
		break;
	case mqttEntityId::entityPvEnergy:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:solar-power-variant-outline\""));
		break;
	case mqttEntityId::entityFrequency:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"icon\": \"mdi:sine-wave\""
			         ", \"suggested_display_precision\": 2"));
		break;
	case mqttEntityId::entityGridPwr:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:transmission-tower\""));
		break;
	case mqttEntityId::entityGridEnergyTo:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:transmission-tower-export\""));
		break;
	case mqttEntityId::entityGridEnergyFrom:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:transmission-tower-import\""));
		break;
	case mqttEntityId::entityBatPwr:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:battery-charging-100\""));
		break;
	case mqttEntityId::entityBatEnergyCharge:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:battery-plus\""));
		break;
	case mqttEntityId::entityBatEnergyDischarge:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:battery-minus\""));
		break;
	case mqttEntityId::entityBatCap:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"device_class\": \"energy\""
			         ", \"state_class\": \"total_increasing\""
			         ", \"unit_of_measurement\": \"kWh\""
			         ", \"icon\": \"mdi:home-battery\""));
		break;
	case mqttEntityId::entityOpMode:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"options\": [ \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\" ]"),
			 OP_MODE_DESC_NORMAL, OP_MODE_DESC_LOAD_FOLLOW, OP_MODE_DESC_TARGET, OP_MODE_DESC_PUSH,
			 OP_MODE_DESC_PV_CHARGE, OP_MODE_DESC_MAX_CHARGE, OP_MODE_DESC_NO_CHARGE);
		break;
	case mqttEntityId::entityDispatchDuration:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"device_class\": \"duration\""
			         ", \"state_class\": \"measurement\""
			         ", \"unit_of_measurement\": \"s\""
			         ", \"icon\": \"mdi:timer-cog-outline\""
			         ", \"min\": 0, \"max\": %lu"),
			 static_cast<unsigned long>(kDispatchDurationMaxSeconds));
		break;
	case mqttEntityId::entityDispatchRemaining:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"device_class\": \"duration\""
			         ", \"state_class\": \"measurement\""
			         ", \"unit_of_measurement\": \"s\""
			         ", \"icon\": \"mdi:timer-sand\""));
		break;
	case mqttEntityId::entityDispatchRequestStatus:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:message-alert-outline\""));
		break;
	case mqttEntityId::entitySocTarget:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"device_class\": \"battery\""
			         ", \"state_class\": \"measurement\""
			         ", \"unit_of_measurement\": \"%%\""
			         ", \"icon\": \"mdi:battery\""
			         ", \"min\": %d, \"max\": %d"),
			 SOC_TARGET_MIN, SOC_TARGET_MAX);
		break;
	case mqttEntityId::entityChargePwr:
	case mqttEntityId::entityDischargePwr:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"device_class\": \"power\""
			         ", \"state_class\": \"measurement\""
			         ", \"unit_of_measurement\": \"W\""
			         ", \"icon\": \"mdi:lightning-bolt-circle\""
			         ", \"min\": %d, \"max\": %d"),
			 0, INVERTER_POWER_MAX);
		break;
	case mqttEntityId::entityPushPwr:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"device_class\": \"power\""
			         ", \"state_class\": \"measurement\""
			         ", \"unit_of_measurement\": \"W\""
			         ", \"icon\": \"mdi:lightning-bolt-circle\""
			         ", \"min\": %d, \"max\": %d"),
			 0, INVERTER_POWER_MAX);
		break;
	case mqttEntityId::entityMaxFeedinPercent:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			 A2M_FMT(", \"unit_of_measurement\": \"%%\""
			         ", \"icon\": \"mdi:transmission-tower-export\""
			         ", \"min\": 0, \"max\": 100"));
		break;
#ifdef A2M_DEBUG_WIFI
	case mqttEntityId::entityRSSI:
	case mqttEntityId::entityBSSID:
	case mqttEntityId::entityTxPower:
	case mqttEntityId::entityWifiRecon:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:wifi\""));
		break;
#endif // A2M_DEBUG_WIFI
	case mqttEntityId::entityA2MVersion:
	case mqttEntityId::entityInverterVersion:
	case mqttEntityId::entityEmsVersion:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:numeric\""));
		break;
	case mqttEntityId::entityInverterSn:
	case mqttEntityId::entityEmsSn:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:identifier\""));
		break;
	case mqttEntityId::entityRs485Errors:
	case mqttEntityId::entityRs485TransportErrors:
	case mqttEntityId::entityRs485OtherErrors:
	case mqttEntityId::entityBatFaults:
	case mqttEntityId::entityBatWarnings:
	case mqttEntityId::entityInverterFaults:
	case mqttEntityId::entityInverterWarnings:
	case mqttEntityId::entitySystemFaults:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:alert-decagram-outline\""));
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
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:clock-alert-outline\""));
		break;
#ifdef DEBUG_FREEMEM
	case mqttEntityId::entityFreemem:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:memory\""));
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
			A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:battery-outline\""));
			break;
		case MqttEntityFamily::Pv:
			A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:solar-power\""));
			break;
		case MqttEntityFamily::Grid:
			A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:transmission-tower\""));
			break;
		case MqttEntityFamily::Backup:
			A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:power-plug-battery\""));
			break;
		case MqttEntityFamily::Inverter:
			A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"icon\": \"mdi:flash\""));
			break;
		case MqttEntityFamily::System:
		case MqttEntityFamily::Controller:
		default:
			stateAddition[0] = '\0';
			break;
		}
		break;
	}
	if (strlen(stateAddition) != 0 && !appendCountedMqttText(payload, stateAddition)) {
		return false;
	}

	if (singleEntity->subscribe) {
#ifdef HA_IS_OP_MODE_AUTHORITY
		if (singleEntity->retain) {
			A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"retain\": \"true\""));
			if (!appendCountedMqttText(payload, stateAddition)) {
				return false;
			}
		}
#endif // HA_IS_OP_MODE_AUTHORITY
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"qos\": %d"), MQTT_SUBSCRIBE_QOS);
		if (!appendCountedMqttText(payload, stateAddition)) {
			return false;
		}
	}

	switch (singleEntity->entityId) {
	case mqttEntityId::entityBatFaults:
	case mqttEntityId::entityBatWarnings:
	case mqttEntityId::entityInverterFaults:
	case mqttEntityId::entityInverterWarnings:
	case mqttEntityId::entitySystemFaults:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			A2M_FMT(", \"state_topic\": \"%s/state\""
			        ", \"value_template\": \"{{ \\\"OK\\\" if value_json.numEvents == 0 else \\\"Problem\\\" }}\""
			        ", \"json_attributes_topic\": \"%s/state\""),
			topicBase,
			topicBase);
		break;
	case mqttEntityId::entityFrequency:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			A2M_FMT(", \"state_topic\": \"%s/state\""
			        ", \"value_template\": \"{{ value_json[\\\"Use Frequency\\\"] | default(\\\"\\\") }}\""
			        ", \"json_attributes_topic\": \"%s/state\""),
			topicBase,
			topicBase);
		break;
	case mqttEntityId::entityRs485Avail:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			A2M_FMT(", \"state_topic\": \"%s\""
			        ", \"value_template\": \"{{ value_json.rs485Status | default(\\\"\\\") }}\""
			        ", \"json_attributes_topic\": \"%s\""),
			statusTopic, statusTopic);
		break;
	case mqttEntityId::entityGridAvail:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			A2M_FMT(", \"state_topic\": \"%s\""
			        ", \"value_template\": \"{{ value_json.gridStatus | default(\\\"\\\") }}\""
			        ", \"json_attributes_topic\": \"%s\""),
			statusTopic, statusTopic);
		break;
	default:
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			A2M_FMT(", \"state_topic\": \"%s/state\""),
			topicBase);
		break;
	}
	if (!appendCountedMqttText(payload, stateAddition)) {
		return false;
	}

	if (singleEntity->subscribe) {
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition), A2M_FMT(", \"command_topic\": \"%s/command\""), topicBase);
		if (!appendCountedMqttText(payload, stateAddition)) {
			return false;
		}
	}

	if (singleEntity->entityId == entityGridAvail) {
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			A2M_FMT(", \"availability_template\": \"{{ \\\"online\\\" if value_json.a2mStatus == \\\"online\\\" and value_json.rs485Status == \\\"OK\\\" and value_json.gridStatus in ( \\\"OK\\\", \\\"Problem\\\" ) else \\\"offline\\\" }}\""
			        ", \"availability_topic\": \"%s\""), statusTopic);
	} else if (singleEntity->scope == MqttEntityScope::Controller ||
	           singleEntity->readKind == MqttEntityReadKind::Control ||
	           singleEntity->readKind == MqttEntityReadKind::Identity ||
	           singleEntity->entityId == entityBatCap ||
	           singleEntity->entityId == entityGridReg) {
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			A2M_FMT(", \"availability_template\": \"{{ value_json.a2mStatus | default(\\\"\\\") }}\""
			        ", \"availability_topic\": \"%s\""), statusTopic);
	} else if (singleEntity->family == MqttEntityFamily::Grid) {
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			A2M_FMT(", \"availability_template\": \"{{ \\\"online\\\" if value_json.a2mStatus == \\\"online\\\" and value_json.rs485Status == \\\"OK\\\" and value_json.gridStatus == \\\"OK\\\" else \\\"offline\\\" }}\""
			        ", \"availability_topic\": \"%s\""), statusTopic);
	} else {
		A2M_SNPRINTF(stateAddition, sizeof(stateAddition),
			A2M_FMT(", \"availability_template\": \"{{ \\\"online\\\" if value_json.a2mStatus == \\\"online\\\" and value_json.rs485Status == \\\"OK\\\" else \\\"offline\\\" }}\""
			        ", \"availability_topic\": \"%s\""), statusTopic);
	}
	if (!appendCountedMqttText(payload, stateAddition)) {
		return false;
	}

	return appendCountedMqttText(payload, "}");
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
	const size_t numberOfEntities = mqttEntitiesCount();

	// Spread retained HA discovery publishes across multiple loop() turns so a config change
	// cannot monopolize the ESP8266 network stack long enough to trigger the watchdog.
	if (publishPendingStaleControllerDiscoveryClears()) {
		return;
	}
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
		mqttState entity{};
		if (!mqttEntityCopyByIndex(resendHaNextEntityIndex, &entity) ||
		    !publishHaEntityDiscovery(&entity)) {
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

static void
beginSchedulerPass(void)
{
	schedulerPassSequence++;
	schedulerPassCache = SchedulerPassSourceCache{};
	schedulerPassCache.passId = schedulerPassSequence;
	schedulerPassCache.active = true;
}

static void
endSchedulerPass(void)
{
	schedulerPassCache.active = false;
	schedulerPassCache.snapshotBuildInProgress = false;
}

static uint16_t
decodeRegisterWord(const modbusRequestAndResponse &response, size_t wordOffset)
{
	const size_t byteOffset = wordOffset * 2U;
	return static_cast<uint16_t>((response.data[byteOffset] << 8) | response.data[byteOffset + 1]);
}

static uint32_t
decodeRegisterUnsignedInt(const modbusRequestAndResponse &response, size_t wordOffset)
{
	const size_t byteOffset = wordOffset * 2U;
	return (static_cast<uint32_t>(response.data[byteOffset]) << 24) |
	       (static_cast<uint32_t>(response.data[byteOffset + 1]) << 16) |
	       (static_cast<uint32_t>(response.data[byteOffset + 2]) << 8) |
	       static_cast<uint32_t>(response.data[byteOffset + 3]);
}

static int32_t
decodeRegisterSignedInt(const modbusRequestAndResponse &response, size_t wordOffset)
{
	return static_cast<int32_t>(decodeRegisterUnsignedInt(response, wordOffset));
}

static void
prepareUnsignedShortResponse(uint16_t value, modbusRequestAndResponse &prepared)
{
	prepared = modbusRequestAndResponse{};
	prepared.dataSize = 2;
	prepared.returnDataType = modbusReturnDataType::unsignedShort;
	prepared.unsignedShortValue = value;
	prepared.data[0] = static_cast<uint8_t>((value >> 8) & 0xff);
	prepared.data[1] = static_cast<uint8_t>(value & 0xff);
	snprintf(prepared.dataValueFormatted, sizeof(prepared.dataValueFormatted), "%u", value);
}

static void
prepareScaledUnsignedShortResponse(uint16_t rawValue,
                                   float scaledValue,
                                   uint8_t decimalPlaces,
                                   modbusRequestAndResponse &prepared)
{
	prepareUnsignedShortResponse(rawValue, prepared);
	snprintf(prepared.dataValueFormatted,
	         sizeof(prepared.dataValueFormatted),
	         decimalPlaces == 1 ? "%0.1f" : "%0.02f",
	         scaledValue);
}

static void
prepareUnsignedIntResponse(uint32_t value, modbusRequestAndResponse &prepared)
{
	prepared = modbusRequestAndResponse{};
	prepared.dataSize = 4;
	prepared.returnDataType = modbusReturnDataType::unsignedInt;
	prepared.unsignedIntValue = value;
	prepared.data[0] = static_cast<uint8_t>((value >> 24) & 0xff);
	prepared.data[1] = static_cast<uint8_t>((value >> 16) & 0xff);
	prepared.data[2] = static_cast<uint8_t>((value >> 8) & 0xff);
	prepared.data[3] = static_cast<uint8_t>(value & 0xff);
	snprintf(prepared.dataValueFormatted,
	         sizeof(prepared.dataValueFormatted),
	         "%lu",
	         static_cast<unsigned long>(value));
}

static void
prepareSignedIntResponse(int32_t value, modbusRequestAndResponse &prepared)
{
	prepared = modbusRequestAndResponse{};
	prepared.dataSize = 4;
	prepared.returnDataType = modbusReturnDataType::signedInt;
	prepared.signedIntValue = value;
	prepared.data[0] = static_cast<uint8_t>((static_cast<uint32_t>(value) >> 24) & 0xff);
	prepared.data[1] = static_cast<uint8_t>((static_cast<uint32_t>(value) >> 16) & 0xff);
	prepared.data[2] = static_cast<uint8_t>((static_cast<uint32_t>(value) >> 8) & 0xff);
	prepared.data[3] = static_cast<uint8_t>(static_cast<uint32_t>(value) & 0xff);
	snprintf(prepared.dataValueFormatted,
	         sizeof(prepared.dataValueFormatted),
	         "%ld",
	         static_cast<long>(value));
}

static bool
fetchDispatchBlockSnapshot(DispatchBlockSnapshot &out,
                           modbusRequestAndResponseStatusValues *resultOut)
{
	if (schedulerPassCache.active &&
	    sourceGroupCacheReusableForPass(schedulerPassCache.dispatch.meta, schedulerPassCache.passId)) {
		schedulerPassCache.dispatchCacheHitCount++;
		dispatchBlockCacheHitCount++;
		out = schedulerPassCache.dispatch;
		if (resultOut != nullptr) {
			*resultOut = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		}
		return true;
	}

	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		if (resultOut != nullptr) {
			*resultOut = modbusRequestAndResponseStatusValues::preProcessing;
		}
		return false;
	}
	*response = modbusRequestAndResponse{};
	response->returnDataType = modbusReturnDataType::unsignedShort;
	const uint32_t startedMs = millis();
	const modbusRequestAndResponseStatusValues result =
		(_registerHandler != nullptr)
			? _registerHandler->readRawRegisterBlock(kDispatchBlockStartReg, kDispatchBlockRegisterCount, response)
			: modbusRequestAndResponseStatusValues::preProcessing;
	const uint32_t completedMs = millis();
	if (resultOut != nullptr) {
		*resultOut = result;
	}
	if (result != modbusRequestAndResponseStatusValues::readDataRegisterSuccess ||
	    response->dataSize < (kDispatchBlockRegisterCount * 2U)) {
		return false;
	}

	DispatchBlockSnapshot snapshot{};
	snapshot.meta.passId = schedulerPassCache.active ? schedulerPassCache.passId : 0;
	snapshot.meta.readStartedMs = startedMs;
	snapshot.meta.readCompletedMs = completedMs;
	snapshot.meta.valid = true;
	snapshot.dispatchStart = decodeRegisterWord(*response, 0);
	snapshot.dispatchActivePower = decodeRegisterSignedInt(*response, 1);
	snapshot.dispatchMode = decodeRegisterWord(*response, 5);
	snapshot.dispatchSocRaw = decodeRegisterWord(*response, 6);
	snapshot.dispatchTimeRaw = decodeRegisterUnsignedInt(*response, 7);
	out = snapshot;
	if (schedulerPassCache.active) {
		schedulerPassCache.dispatch = snapshot;
	}
	return true;
}

static void
invalidateDispatchBlockSnapshotCache(void)
{
	schedulerPassCache.dispatch.meta = SourceGroupReadMeta{};
}

static bool
fetchPvStringBlockSnapshot(PvStringBlockSnapshot &out,
                           modbusRequestAndResponseStatusValues *resultOut)
{
	if (schedulerPassCache.active &&
	    sourceGroupCacheReusableForPass(schedulerPassCache.pvBlock.meta, schedulerPassCache.passId)) {
		schedulerPassCache.pvBlockCacheHitCount++;
		pvBlockCacheHitCount++;
		out = schedulerPassCache.pvBlock;
		if (resultOut != nullptr) {
			*resultOut = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		}
		return true;
	}

	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		if (resultOut != nullptr) {
			*resultOut = modbusRequestAndResponseStatusValues::preProcessing;
		}
		return false;
	}
	*response = modbusRequestAndResponse{};
	response->returnDataType = modbusReturnDataType::unsignedShort;
	const uint32_t startedMs = millis();
	const modbusRequestAndResponseStatusValues result =
		(_registerHandler != nullptr)
			? _registerHandler->readRawRegisterBlock(kPvStringBlockStartReg, kPvStringBlockRegisterCount, response)
			: modbusRequestAndResponseStatusValues::preProcessing;
	const uint32_t completedMs = millis();
	if (resultOut != nullptr) {
		*resultOut = result;
	}
	if (result != modbusRequestAndResponseStatusValues::readDataRegisterSuccess ||
	    response->dataSize < (kPvStringBlockRegisterCount * 2U)) {
		return false;
	}

	PvStringBlockSnapshot snapshot{};
	snapshot.meta.passId = schedulerPassCache.active ? schedulerPassCache.passId : 0;
	snapshot.meta.readStartedMs = startedMs;
	snapshot.meta.readCompletedMs = completedMs;
	snapshot.meta.valid = true;
	for (size_t pv = 0; pv < kPvStringCount; ++pv) {
		const size_t wordOffset = pv * 4U;
		snapshot.voltage[pv] = decodeRegisterWord(*response, wordOffset);
		snapshot.current[pv] = decodeRegisterWord(*response, wordOffset + 1U);
		snapshot.power[pv] = decodeRegisterSignedInt(*response, wordOffset + 2U);
	}
	out = snapshot;
	if (schedulerPassCache.active) {
		schedulerPassCache.pvBlock = snapshot;
	}
	return true;
}

static bool
fetchPvMeterTotalSnapshot(PvMeterTotalSnapshot &out,
                          modbusRequestAndResponseStatusValues *resultOut)
{
	if (schedulerPassCache.active &&
	    sourceGroupCacheReusableForPass(schedulerPassCache.pvMeter.meta, schedulerPassCache.passId)) {
		schedulerPassCache.pvMeterCacheHitCount++;
		pvMeterCacheHitCount++;
		out = schedulerPassCache.pvMeter;
		if (resultOut != nullptr) {
			*resultOut = modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
		}
		return true;
	}

	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		if (resultOut != nullptr) {
			*resultOut = modbusRequestAndResponseStatusValues::preProcessing;
		}
		return false;
	}
	*response = modbusRequestAndResponse{};
	response->returnDataType = modbusReturnDataType::signedInt;
	const uint32_t startedMs = millis();
	const modbusRequestAndResponseStatusValues result =
		(_registerHandler != nullptr)
			? _registerHandler->readRawRegisterBlock(REG_PV_METER_R_TOTAL_ACTIVE_POWER_1, 2, response)
			: modbusRequestAndResponseStatusValues::preProcessing;
	const uint32_t completedMs = millis();
	if (resultOut != nullptr) {
		*resultOut = result;
	}
	if (result != modbusRequestAndResponseStatusValues::readDataRegisterSuccess || response->dataSize < 4U) {
		return false;
	}

	PvMeterTotalSnapshot snapshot{};
	snapshot.meta.passId = schedulerPassCache.active ? schedulerPassCache.passId : 0;
	snapshot.meta.readStartedMs = startedMs;
	snapshot.meta.readCompletedMs = completedMs;
	snapshot.meta.valid = true;
	snapshot.totalPower = decodeRegisterSignedInt(*response, 0);
	out = snapshot;
	if (schedulerPassCache.active) {
		schedulerPassCache.pvMeter = snapshot;
	}
	return true;
}

static bool
prepareDispatchBlockResponse(mqttEntityId entityId,
                             const DispatchBlockSnapshot &snapshot,
                             modbusRequestAndResponse &prepared)
{
	switch (entityId) {
	case mqttEntityId::entityDispatchStart:
		prepareUnsignedShortResponse(snapshot.dispatchStart, prepared);
		return formatDispatchStartValue(prepared.dataValueFormatted,
		                                sizeof(prepared.dataValueFormatted),
		                                snapshot.dispatchStart);
	case mqttEntityId::entityDispatchMode:
		prepareUnsignedShortResponse(snapshot.dispatchMode, prepared);
		return formatDispatchModeValue(prepared.dataValueFormatted,
		                               sizeof(prepared.dataValueFormatted),
		                               snapshot.dispatchMode);
	case mqttEntityId::entityDispatchPower:
		prepareSignedIntResponse(dispatchActivePowerRawToWatts(snapshot.dispatchActivePower), prepared);
		return true;
	case mqttEntityId::entityDispatchSoc:
		prepareScaledUnsignedShortResponse(snapshot.dispatchSocRaw,
		                                   dispatchSocPercentFromRaw(snapshot.dispatchSocRaw),
		                                   2,
		                                   prepared);
		return true;
	case mqttEntityId::entityDispatchTime:
		prepareUnsignedIntResponse(snapshot.dispatchTimeRaw, prepared);
		return true;
	default:
		return false;
	}
}

static bool
preparePvBlockResponse(const mqttState &entity,
                       const PvStringBlockSnapshot &snapshot,
                       modbusRequestAndResponse &prepared)
{
	if (!isPvStringBlockReadKey(entity.readKey)) {
		return false;
	}
	const uint16_t relative = static_cast<uint16_t>(entity.readKey - kPvStringBlockStartReg);
	const size_t pvIndex = relative / 4U;
	const uint16_t fieldOffset = static_cast<uint16_t>(relative % 4U);
	if (pvIndex >= kPvStringCount) {
		return false;
	}
	switch (fieldOffset) {
	case 0:
		prepareScaledUnsignedShortResponse(snapshot.voltage[pvIndex],
		                                   pvVoltageCurrentFromRaw(snapshot.voltage[pvIndex]),
		                                   1,
		                                   prepared);
		return true;
	case 1:
		prepareScaledUnsignedShortResponse(snapshot.current[pvIndex],
		                                   pvVoltageCurrentFromRaw(snapshot.current[pvIndex]),
		                                   1,
		                                   prepared);
		return true;
	case 2:
		prepareUnsignedIntResponse(static_cast<uint32_t>(snapshot.power[pvIndex]), prepared);
		return true;
	default:
		return false;
	}
}

static void
noteSnapshotReadFailure(const char *name,
                        uint16_t reg,
                        modbusRequestAndResponseStatusValues result,
                        const char *detail)
{
#ifdef DEBUG_OVER_SERIAL
	snprintf(_debugOutput,
	         sizeof(_debugOutput),
	         "snapshot read fail: %s reg=%u result=%u detail=%s",
	         name,
	         static_cast<unsigned>(reg),
	         static_cast<unsigned>(result),
	         detail != nullptr ? detail : "");
	Serial.println(_debugOutput);
#else
	(void)name;
	(void)reg;
	(void)result;
	(void)detail;
#endif
}

static void
recordSnapshotReadFailure(const char *name,
                          uint16_t reg,
                          modbusRequestAndResponseStatusValues result,
                          const char *detail,
                          bool &rs485TimedOut,
                          bool &rs485TransportFailure,
                          int &gotError)
{
	rs485TimedOut = rs485TimedOut || (result == modbusRequestAndResponseStatusValues::noResponse);
	rs485TransportFailure =
		rs485TransportFailure || (classifyRs485Error(result) == Rs485ErrorClass::Transport);
	noteSnapshotReadFailure(name, reg, result, detail);
	recordRs485Error(result);
	noteRs485Error(result, detail != nullptr ? detail : "");
	gotError++;
}

static void __attribute__((noinline))
refreshSnapshotDispatchFields(bool &rs485TimedOut, bool &rs485TransportFailure, int &gotError)
{
	DispatchBlockSnapshot dispatchSnapshot{};
	modbusRequestAndResponseStatusValues result = modbusRequestAndResponseStatusValues::preProcessing;
	if (fetchDispatchBlockSnapshot(dispatchSnapshot, &result)) {
		opData.essDispatchStart = dispatchSnapshot.dispatchStart;
		opData.essDispatchMode = dispatchSnapshot.dispatchMode;
		opData.essDispatchActivePower = dispatchSnapshot.dispatchActivePower;
		opData.essDispatchSoc = dispatchSnapshot.dispatchSocRaw;
		opData.essDispatchTime = dispatchSnapshot.dispatchTimeRaw;
		return;
	}

	opData.essDispatchStart = UINT16_MAX;
	opData.essDispatchMode = UINT16_MAX;
	opData.essDispatchActivePower = INT32_MAX;
	opData.essDispatchSoc = UINT16_MAX;
	opData.essDispatchTime = UINT32_MAX;
	recordSnapshotReadFailure("dispatch_block",
	                          kDispatchBlockStartReg,
	                          result,
	                          "",
	                          rs485TimedOut,
	                          rs485TransportFailure,
	                          gotError);
}

static void __attribute__((noinline))
refreshSnapshotBatterySoc(bool &rs485TimedOut, bool &rs485TransportFailure, int &gotError)
{
	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		opData.essBatterySoc = UINT16_MAX;
		recordSnapshotReadFailure("battery_soc",
		                          REG_BATTERY_HOME_R_SOC,
		                          modbusRequestAndResponseStatusValues::preProcessing,
		                          "",
		                          rs485TimedOut,
		                          rs485TransportFailure,
		                          gotError);
		return;
	}
	*response = modbusRequestAndResponse{};
	const modbusRequestAndResponseStatusValues result =
		_registerHandler->readHandledRegister(REG_BATTERY_HOME_R_SOC, response);
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		opData.essBatterySoc = response->unsignedShortValue;
		return;
	}

	opData.essBatterySoc = UINT16_MAX;
	recordSnapshotReadFailure("battery_soc",
	                          REG_BATTERY_HOME_R_SOC,
	                          result,
	                          response->statusMqttMessage,
	                          rs485TimedOut,
	                          rs485TransportFailure,
	                          gotError);
}

static void __attribute__((noinline))
refreshSnapshotBatteryPower(bool &rs485TimedOut, bool &rs485TransportFailure, int &gotError)
{
	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		opData.essBatteryPower = INT16_MAX;
		recordSnapshotReadFailure("battery_power",
		                          REG_BATTERY_HOME_R_BATTERY_POWER,
		                          modbusRequestAndResponseStatusValues::preProcessing,
		                          "",
		                          rs485TimedOut,
		                          rs485TransportFailure,
		                          gotError);
		return;
	}
	*response = modbusRequestAndResponse{};
	const modbusRequestAndResponseStatusValues result =
		_registerHandler->readHandledRegister(REG_BATTERY_HOME_R_BATTERY_POWER, response);
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		opData.essBatteryPower = response->signedShortValue;
		return;
	}

	opData.essBatteryPower = INT16_MAX;
	recordSnapshotReadFailure("battery_power",
	                          REG_BATTERY_HOME_R_BATTERY_POWER,
	                          result,
	                          response->statusMqttMessage,
	                          rs485TimedOut,
	                          rs485TransportFailure,
	                          gotError);
}

static void __attribute__((noinline))
refreshSnapshotGridPower(bool &rs485TimedOut, bool &rs485TransportFailure, int &gotError)
{
	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		opData.essGridPower = INT32_MAX;
		recordSnapshotReadFailure("grid_power",
		                          REG_GRID_METER_R_TOTAL_ACTIVE_POWER_1,
		                          modbusRequestAndResponseStatusValues::preProcessing,
		                          "",
		                          rs485TimedOut,
		                          rs485TransportFailure,
		                          gotError);
		return;
	}
	*response = modbusRequestAndResponse{};
	const modbusRequestAndResponseStatusValues result =
		_registerHandler->readHandledRegister(REG_GRID_METER_R_TOTAL_ACTIVE_POWER_1, response);
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		opData.essGridPower = response->signedIntValue;
		return;
	}

	opData.essGridPower = INT32_MAX;
	recordSnapshotReadFailure("grid_power",
	                          REG_GRID_METER_R_TOTAL_ACTIVE_POWER_1,
	                          result,
	                          response->statusMqttMessage,
	                          rs485TimedOut,
	                          rs485TransportFailure,
	                          gotError);
}

static void __attribute__((noinline))
refreshSnapshotSolarPower(bool &rs485TimedOut, bool &rs485TransportFailure, int &gotError)
{
	PvMeterTotalSnapshot pvMeterSnapshot{};
	PvStringBlockSnapshot pvBlockSnapshot{};
	modbusRequestAndResponseStatusValues pvMeterResult = modbusRequestAndResponseStatusValues::preProcessing;
	modbusRequestAndResponseStatusValues pvBlockResult = modbusRequestAndResponseStatusValues::preProcessing;
	const bool pvMeterOk = fetchPvMeterTotalSnapshot(pvMeterSnapshot, &pvMeterResult);
	const bool pvBlockOk = fetchPvStringBlockSnapshot(pvBlockSnapshot, &pvBlockResult);
	if (pvMeterOk && pvBlockOk) {
		int32_t solarPower = pvMeterSnapshot.totalPower;
		for (size_t pv = 0; pv < kPvStringCount; ++pv) {
			solarPower += pvBlockSnapshot.power[pv];
		}
		opData.essPvPower = solarPower;
		return;
	}

	opData.essPvPower = INT32_MAX;
	recordSnapshotReadFailure("pv_power",
	                          REG_CUSTOM_TOTAL_SOLAR_POWER,
	                          pvMeterOk ? pvBlockResult : pvMeterResult,
	                          "",
	                          rs485TimedOut,
	                          rs485TransportFailure,
	                          gotError);
}

static void __attribute__((noinline))
refreshSnapshotWorkingMode(bool &rs485TimedOut, bool &rs485TransportFailure, int &gotError)
{
	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		opData.essInverterMode = UINT16_MAX;
		recordSnapshotReadFailure("working_mode",
		                          REG_INVERTER_HOME_R_WORKING_MODE,
		                          modbusRequestAndResponseStatusValues::preProcessing,
		                          "",
		                          rs485TimedOut,
		                          rs485TransportFailure,
		                          gotError);
		return;
	}
	*response = modbusRequestAndResponse{};
	const modbusRequestAndResponseStatusValues result =
		_registerHandler->readHandledRegister(REG_INVERTER_HOME_R_WORKING_MODE, response);
	if (result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		opData.essInverterMode = response->unsignedShortValue;
		return;
	}

	opData.essInverterMode = UINT16_MAX;
	recordSnapshotReadFailure("working_mode",
	                          REG_INVERTER_HOME_R_WORKING_MODE,
	                          result,
	                          response->statusMqttMessage,
	                          rs485TimedOut,
	                          rs485TransportFailure,
	                          gotError);
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
	int powerSnapshotErrors = 0;
	uint32_t pollStartMs = millis();
	uint32_t powerSnapshotStartedMs = pollStartMs;
	uint32_t powerSnapshotCompletedMs = pollStartMs;
	bool rs485TimedOut = false;
	bool rs485TransportFailure = false;
	struct SnapshotDiagGuard {
		~SnapshotDiagGuard()
		{
			noteRuntimePhaseObservation(RuntimeDiagPhase::PollSnapshot);
		}
	} snapshotDiagGuard;
	diag_rs485_poll_begin(pollStartMs);
	essSnapshotAttemptCount++;

	// Do not issue snapshot register reads until RS485 link is actually established.
	// Inverter identity may be known from persisted metadata, but that is not proof of a live bus.
	if (rs485ConnectState != Rs485ConnectState::Connected || _registerHandler == NULL) {
		essSnapshotValid = false;
		essPowerSnapshotValid = false;
		essPowerSnapshotLastBuildMs = 0;
		essSnapshotLastOk = false;
		if (schedulerPassCache.active) {
			populateEssSnapshotMeta(schedulerPassCache.essSnapshot,
			                        schedulerPassCache.passId,
			                        pollStartMs,
			                        pollStartMs,
			                        false);
		}
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
		opData.essDispatchTime = 30;
		opData.essBatterySoc = 65 / BATTERY_SOC_MULTIPLIER;
		opData.essBatteryPower = -1357;
		opData.essGridPower = -1368;
		opData.essPvPower = -1379;
		opData.essInverterMode = essInverterMode;
		essPowerSnapshotValid = true;
	} else {
		opData.essDispatchStart = UINT16_MAX;
		opData.essDispatchMode = UINT16_MAX;
		opData.essDispatchActivePower = INT32_MAX;
		opData.essDispatchSoc = UINT16_MAX;
		opData.essDispatchTime = UINT32_MAX;
		opData.essBatterySoc = UINT16_MAX;
		opData.essBatteryPower = INT16_MAX;
		opData.essGridPower = INT32_MAX;
		opData.essPvPower = INT32_MAX;
		opData.essInverterMode = UINT16_MAX;
		essPowerSnapshotValid = false;
		lastErrCode = static_cast<int>(MqttEventCode::Rs485Timeout);
		gotError = 9;
	}
#else // DEBUG_NO_RS485
	if (_registerHandler == NULL) {
		opData.essDispatchStart = UINT16_MAX;
		opData.essDispatchMode = UINT16_MAX;
		opData.essDispatchActivePower = INT32_MAX;
		opData.essDispatchSoc = UINT16_MAX;
		opData.essDispatchTime = UINT32_MAX;
		opData.essBatterySoc = UINT16_MAX;
		opData.essBatteryPower = INT16_MAX;
		opData.essGridPower = INT32_MAX;
		opData.essPvPower = INT32_MAX;
		opData.essInverterMode = UINT16_MAX;
		opData.essRs485Connected = false;
		essPowerSnapshotValid = false;
		lastErrCode = static_cast<int>(MqttEventCode::Rs485Timeout);
		gotError = 1;
	} else {
		refreshSnapshotDispatchFields(rs485TimedOut, rs485TransportFailure, gotError);
		refreshSnapshotBatterySoc(rs485TimedOut, rs485TransportFailure, gotError);
		struct SnapshotBuildGuard {
			SnapshotBuildGuard()
			{
				essSnapshotBuildInProgress = true;
				if (schedulerPassCache.active) {
					schedulerPassCache.snapshotBuildInProgress = true;
				}
			}
			~SnapshotBuildGuard()
			{
				essSnapshotBuildInProgress = false;
				if (schedulerPassCache.active) {
					schedulerPassCache.snapshotBuildInProgress = false;
				}
			}
		} snapshotBuildGuard;
		powerSnapshotStartedMs = millis();
		const int powerErrorsBeforeBattery = gotError;
		refreshSnapshotBatteryPower(rs485TimedOut, rs485TransportFailure, gotError);
		if (gotError != powerErrorsBeforeBattery) {
			powerSnapshotErrors++;
		}
		const int powerErrorsBeforeGrid = gotError;
		refreshSnapshotGridPower(rs485TimedOut, rs485TransportFailure, gotError);
		if (gotError != powerErrorsBeforeGrid) {
			powerSnapshotErrors++;
		}
		const int powerErrorsBeforeSolar = gotError;
		refreshSnapshotSolarPower(rs485TimedOut, rs485TransportFailure, gotError);
		if (gotError != powerErrorsBeforeSolar) {
			powerSnapshotErrors++;
		}
		powerSnapshotCompletedMs = millis();
		essPowerSnapshotValid = (powerSnapshotErrors == 0);
		essPowerSnapshotLastBuildMs = powerSnapshotCompletedMs - powerSnapshotStartedMs;
		refreshSnapshotWorkingMode(rs485TimedOut, rs485TransportFailure, gotError);
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
		essSnapshotValid = false;
		strlcpy(dispatchLastSkipReason, "ess_snapshot_failed", sizeof(dispatchLastSkipReason));
	} else {
		pollOkCount++;
		lastOkTsMs = millis();
		essSnapshotValid = true;
		dispatchLastSkipReason[0] = '\0';
	}
	essSnapshotLastOk = essSnapshotValid;
	const Rs485RuntimeReconnectEvaluation reconnectEvaluation =
		rs485EvaluateRuntimeReconnect(rs485ConnectState == Rs485ConnectState::Connected,
		                              rs485RuntimeReconnect,
		                              essSnapshotValid,
		                              rs485TransportFailure,
		                              opData.essRs485Connected);
	rs485RuntimeReconnect.consecutiveTransportFailures =
		reconnectEvaluation.consecutiveTransportFailures;
	rs485RuntimeReconnect.liveSnapshotSeenInEpoch =
		reconnectEvaluation.liveSnapshotSeenInEpoch;
	if (reconnectEvaluation.triggerRediscovery) {
		beginRs485RuntimeRediscovery("snapshot_transport_loss");
	}
	if (schedulerPassCache.active) {
		populateEssSnapshotMeta(schedulerPassCache.essSnapshot,
		                        schedulerPassCache.passId,
		                        powerSnapshotStartedMs,
		                        powerSnapshotCompletedMs,
		                        essPowerSnapshotValid);
	}
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

static bool
refreshEssSnapshotAfterDispatch(bool primeForCurrentSendDataPass)
{
	const bool snapshotOk = refreshEssSnapshot();
	if (primeForCurrentSendDataPass) {
		essSnapshotPrimedForSendDataLoop = snapshotOk ? loopSequence : 0;
	}
	return snapshotOk;
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

static void
resetBootstrapPublishState(bool pending)
{
	bootstrapPublishPending = pending;
	bootstrapPublishCursor = 0;
	memset(bootstrapPublishedEntities, 0, sizeof(bootstrapPublishedEntities));
}

static bool
bootstrapEntityWasPublished(size_t idx)
{
	if (idx >= kMqttEntityDescriptorCount) {
		return true;
	}
	const size_t byteIdx = idx / 8U;
	const uint8_t mask = static_cast<uint8_t>(1U << (idx % 8U));
	return (bootstrapPublishedEntities[byteIdx] & mask) != 0;
}

static void
markBootstrapEntityPublished(size_t idx)
{
	if (!bootstrapPublishPending || idx >= kMqttEntityDescriptorCount) {
		return;
	}
	const size_t byteIdx = idx / 8U;
	const uint8_t mask = static_cast<uint8_t>(1U << (idx % 8U));
	bootstrapPublishedEntities[byteIdx] |= mask;
}

static bool
bootstrapPublishComplete(size_t entityCount)
{
	for (size_t idx = 0; idx < entityCount; ++idx) {
		if (!bootstrapEntityWasPublished(idx)) {
			return false;
		}
	}
	return true;
}

static bool
snapshotPublishAllowedForEntityIndex(size_t idx)
{
	mqttState entity{};
	if (!mqttEntityCopyByIndex(idx, &entity)) {
		return false;
	}
	if (!entity.needsEssSnapshot) {
		return true;
	}
	if (isEssPowerSnapshotEntityId(entity.entityId)) {
		if (!essPowerSnapshotValid) {
			return false;
		}
		if (!schedulerPassCache.active) {
			return true;
		}
		return snapshotPublishAllowedForPass(schedulerPassCache.essSnapshot, schedulerPassCache.passId);
	}
	return essSnapshotValid;
}

static void
serviceBootstrapPublishPass(void)
{
	if (!bootstrapPublishPending || !_mqtt.connected() || !mqttEntitiesRtAvailable()) {
		return;
	}
	const size_t entityCount = mqttEntitiesCount();
	if (entityCount == 0 || entityCount > kMqttEntityDescriptorCount) {
		bootstrapPublishPending = false;
		bootstrapPublishCursor = 0;
		return;
	}

	size_t cursor = normalizeDeferredCursor(bootstrapPublishCursor, entityCount);
	size_t scanned = 0;
	size_t attempted = 0;

	while (scanned < entityCount && attempted < kBootstrapPublishPerTick) {
		const size_t idx = (cursor + scanned) % entityCount;
		scanned++;
		if (bootstrapEntityWasPublished(idx)) {
			continue;
		}

		mqttState entity{};
		if (!mqttEntityCopyByIndex(idx, &entity)) {
			markBootstrapEntityPublished(idx);
			continue;
		}
		if (!includeEntityInPublicSurfaces(entity)) {
			markBootstrapEntityPublished(idx);
			continue;
		}
		const mqttUpdateFreq effectiveFreq = mqttEntityEffectiveFreqByIndex(idx);
		if (!shouldBootstrapPublishEntity(effectiveFreq)) {
			markBootstrapEntityPublished(idx);
			continue;
		}
		const DiscoveryDeviceScope scope = mqttEntityScope(entity.entityId);
		const char *deviceId = discoveryDeviceIdForScope(scope);
		if (deviceId == nullptr || deviceId[0] == '\0') {
			markBootstrapEntityPublished(idx);
			continue;
		}
		if (!snapshotPublishAllowedForEntityIndex(idx)) {
			markBootstrapEntityPublished(idx);
			continue;
		}

			// Bootstrap refresh is a bounded best-effort pass. If an entity is still
			// unreadable here, fall back to its normal bucket cadence instead of
			// retrying it on every idle loop while MQTT remains connected.
			sendDataFromMqttState(&entity, false, nullptr);
			markBootstrapEntityPublished(idx);
			attempted++;
		}

	if (scanned >= entityCount) {
		if (bootstrapPublishComplete(entityCount)) {
			bootstrapPublishPending = false;
			bootstrapPublishCursor = 0;
		} else {
			bootstrapPublishCursor = 0;
		}
		return;
	}
	bootstrapPublishCursor = normalizeDeferredCursor(cursor + scanned, entityCount);
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
__attribute__((noinline))
executeDispatchBlockTransaction(const MqttEntityActiveBucket &bucketPlan,
                                const MqttPollTransaction &transaction)
{
	DispatchBlockSnapshot dispatchSnapshot{};
	modbusRequestAndResponseStatusValues dispatchResult = modbusRequestAndResponseStatusValues::preProcessing;
	if (!fetchDispatchBlockSnapshot(dispatchSnapshot, &dispatchResult)) {
		recordRs485Error(dispatchResult);
		return;
	}
	modbusRequestAndResponse *prepared = runtimeModbusReadScratch();
	if (prepared == nullptr) {
		return;
	}

	for (size_t member = 0; member < transaction.entityCount; ++member) {
		const size_t offset = static_cast<size_t>(transaction.firstMemberOffset) + member;
		if (offset >= bucketPlan.count) {
			break;
		}
		mqttState entity{};
		*prepared = modbusRequestAndResponse{};
		if (!mqttEntityCopyByIndex(bucketPlan.members[offset], &entity) ||
		    !prepareDispatchBlockResponse(entity.entityId, dispatchSnapshot, *prepared)) {
			continue;
		}
		sendDataFromMqttState(&entity, false, prepared);
	}
}

static void
__attribute__((noinline))
executePvBlockTransaction(const MqttEntityActiveBucket &bucketPlan,
                          const MqttPollTransaction &transaction)
{
	PvStringBlockSnapshot pvBlockSnapshot{};
	modbusRequestAndResponseStatusValues pvResult = modbusRequestAndResponseStatusValues::preProcessing;
	if (!fetchPvStringBlockSnapshot(pvBlockSnapshot, &pvResult)) {
		recordRs485Error(pvResult);
		return;
	}
	modbusRequestAndResponse *prepared = runtimeModbusReadScratch();
	if (prepared == nullptr) {
		return;
	}

	for (size_t member = 0; member < transaction.entityCount; ++member) {
		const size_t offset = static_cast<size_t>(transaction.firstMemberOffset) + member;
		if (offset >= bucketPlan.count) {
			break;
		}
		mqttState entity{};
		*prepared = modbusRequestAndResponse{};
		if (!mqttEntityCopyByIndex(bucketPlan.members[offset], &entity) ||
		    !preparePvBlockResponse(entity, pvBlockSnapshot, *prepared)) {
			continue;
		}
		sendDataFromMqttState(&entity, false, prepared);
	}
}

static void
__attribute__((noinline))
executeGenericPollTransaction(const MqttEntityActiveBucket &bucketPlan,
                              const MqttPollTransaction &transaction,
                              const mqttState &leader)
{
	if (shouldSkipScheduledEntityRead(mqttEntityScope(leader.entityId),
	                                  inverterReady,
	                                  inverterSerialKnown())) {
		return;
	}

	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		return;
	}
	*response = modbusRequestAndResponse{};
	const modbusRequestAndResponseStatusValues result = readEntity(&leader, response);
	if (result != modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		return;
	}

	for (size_t member = 0; member < transaction.entityCount; ++member) {
		const size_t offset = static_cast<size_t>(transaction.firstMemberOffset) + member;
		if (offset >= bucketPlan.count) {
			break;
		}
		mqttState entity{};
		if (!mqttEntityCopyByIndex(bucketPlan.members[offset], &entity)) {
			continue;
		}
		sendDataFromMqttState(&entity, false, response);
	}
}

static void
executePollTransaction(const MqttEntityActiveBucket &bucketPlan,
                       const MqttPollTransaction &transaction,
                       bool snapshotOkThisBucket)
{
	(void)snapshotOkThisBucket;
	if (transaction.entityCount == 0 || bucketPlan.members == nullptr) {
		return;
	}

	const size_t leaderOffset = transaction.firstMemberOffset;
	if (leaderOffset >= bucketPlan.count) {
		return;
	}

	switch (transaction.kind) {
	case MqttPollTransactionKind::SnapshotFanout:
		for (size_t member = 0; member < transaction.entityCount; ++member) {
			const size_t offset = static_cast<size_t>(transaction.firstMemberOffset) + member;
			if (offset >= bucketPlan.count) {
				break;
			}
			mqttState entity{};
			if (!mqttEntityCopyByIndex(bucketPlan.members[offset], &entity)) {
				continue;
			}
			sendDataFromMqttState(&entity, false, nullptr);
		}
		return;
	case MqttPollTransactionKind::RegisterFanout:
	case MqttPollTransactionKind::SingleEntity:
	default:
		break;
	}

	const size_t leaderIdx = bucketPlan.members[leaderOffset];
	mqttState leader{};
	if (!mqttEntityCopyByIndex(leaderIdx, &leader)) {
		return;
	}
	if (shouldSkipScheduledEntityRead(mqttEntityScope(leader.entityId),
	                                  inverterReady,
	                                  inverterSerialKnown())) {
		return;
	}
	if (leader.readKind == MqttEntityReadKind::Register && isDispatchBlockReadKey(leader.readKey)) {
		executeDispatchBlockTransaction(bucketPlan, transaction);
		return;
	}
	if (leader.readKind == MqttEntityReadKind::Register && isPvStringBlockReadKey(leader.readKey)) {
		executePvBlockTransaction(bucketPlan, transaction);
		return;
	}
	executeGenericPollTransaction(bucketPlan, transaction, leader);
}

static bool __attribute__((noinline))
ensureSnapshotForBucketPass(bool bucketNeedsSnapshot,
                            bool &snapshotAttemptedThisPass,
                            bool &snapshotOkThisPass)
{
	if (shouldReusePrimedEssSnapshotForBucket(bucketNeedsSnapshot,
	                                          bootPlan.inverter,
	                                          inverterReady,
	                                          snapshotAttemptedThisPass,
	                                          essSnapshotPrimedForSendDataLoop == loopSequence)) {
		snapshotAttemptedThisPass = true;
		snapshotOkThisPass = essSnapshotValid;
		if (schedulerPassCache.active) {
			const uint32_t primedSnapshotMs = millis();
			populateEssSnapshotMeta(schedulerPassCache.essSnapshot,
			                        schedulerPassCache.passId,
			                        primedSnapshotMs,
			                        primedSnapshotMs,
			                        essPowerSnapshotValid);
		}
	} else if (shouldAttemptEssSnapshotRefreshForBucket(bucketNeedsSnapshot,
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
}

static bool __attribute__((noinline))
runBucketTransactionsForPlan(BucketId bucketId,
                             const MqttEntityActiveBucket &bucketPlan,
                             bool snapshotOkThisBucket,
                             uint32_t pollIntervalSecondsLocal)
{
	BucketRuntimeBudgetState *budgetState = bucketBudgetStateFor(bucketId);
	const uint32_t budgetMs = bucketBudgetMs(bucketId, pollIntervalSecondsLocal * 1000UL, kPollOverrunMs);
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
	RuntimeDiagScope diagScope(RuntimeDiagPhase::BucketPublish, "entity");

	while (processed < bucketPlan.transactionCount) {
		const size_t txnIndex = (startCursor + processed) % bucketPlan.transactionCount;
#ifdef DEBUG_OVER_SERIAL
		if (pollIntervalSecondsLocal <= 1) {
			const size_t leaderIdx = bucketPlan.members[bucketPlan.transactions[txnIndex].firstMemberOffset];
			mqttState leader{};
			if (mqttEntityCopyByIndex(leaderIdx, &leader)) {
				char leaderName[64];
				mqttEntityNameCopy(&leader, leaderName, sizeof(leaderName));
				Serial.printf("bucket txn start: bucket=%s idx=%u kind=%u entity=%s reg=%u free=%u max=%u frag=%u\r\n",
				              bucketIdToString(bucketId),
				              static_cast<unsigned>(txnIndex),
				              static_cast<unsigned>(bucketPlan.transactions[txnIndex].kind),
				              leaderName,
				              static_cast<unsigned>(leader.readKey),
				              ESP.getFreeHeap(),
				              ESP.getMaxFreeBlockSize(),
				              ESP.getHeapFragmentation());
			}
		}
#endif
		executePollTransaction(bucketPlan, bucketPlan.transactions[txnIndex], snapshotOkThisBucket);
#ifdef DEBUG_OVER_SERIAL
		if (pollIntervalSecondsLocal <= 1) {
			Serial.printf("bucket txn done: bucket=%s idx=%u free=%u max=%u frag=%u\r\n",
			              bucketIdToString(bucketId),
			              static_cast<unsigned>(txnIndex),
			              ESP.getFreeHeap(),
			              ESP.getMaxFreeBlockSize(),
			              ESP.getHeapFragmentation());
		}
#endif
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
	static bool pendingImmediateStatusPass = false;

	if (resendAllData) {
		resendAllData = false;
		const uint32_t now = nowMillis();
		// Resuming every bucket from "never run" causes an all-buckets catch-up flood on the
		// next pass. That spike is both unrepresentative of steady-state cadence and expensive
		// enough to destabilize ESP8266 when config/connectivity changes arrive close together.
		lastRunTenSeconds = resetScheduleBaseline(now);
		lastRunOneMinute = resetScheduleBaseline(now);
		lastRunFiveMinutes = resetScheduleBaseline(now);
		lastRunOneHour = resetScheduleBaseline(now);
		lastRunOneDay = resetScheduleBaseline(now);
		lastRunUser = resetScheduleBaseline(now);
		pendingImmediateStatusPass = true;
		resetBucketCursors();
		resetBucketBudgetStates();
		// Refresh slow-bucket entity states gradually after reconnect/boot without
		// reviving the old all-buckets catch-up spike on ESP8266.
		resetBootstrapPublishState(true);
	}

	bool dueTenSeconds = checkTimer(&lastRunTenSeconds, STATUS_INTERVAL_TEN_SECONDS);
	const bool dueOneMinute = checkTimer(&lastRunOneMinute, STATUS_INTERVAL_ONE_MINUTE);
	const bool dueFiveMinutes = checkTimer(&lastRunFiveMinutes, STATUS_INTERVAL_FIVE_MINUTE);
	const bool dueOneHour = checkTimer(&lastRunOneHour, STATUS_INTERVAL_ONE_HOUR);
	const bool dueOneDay = checkTimer(&lastRunOneDay, STATUS_INTERVAL_ONE_DAY);
	const bool dueUser = checkTimer(&lastRunUser, pollIntervalSeconds * 1000UL);
#if RS485_STUB
	const bool rs485StubRecentOnlineControl =
		(rs485StubLastOnlineControlMs != 0) &&
		(static_cast<uint32_t>(millis() - rs485StubLastOnlineControlMs) < 20000U);
#endif
	if (pendingImmediateStatusPass) {
		lastRunTenSeconds = nowMillis();
		dueTenSeconds = true;
		pendingImmediateStatusPass = false;
	}
	const bool anyDue = (dueTenSeconds || dueOneMinute || dueFiveMinutes || dueOneHour || dueOneDay || dueUser);

	if (!anyDue) {
		serviceBootstrapPublishPass();
		return;
	}

#ifdef DEBUG_OVER_SERIAL
	if (pollIntervalSeconds <= 1) {
		Serial.printf("sendData due: 10=%u 60=%u 300=%u 3600=%u 86400=%u user=%u free=%u max=%u frag=%u\r\n",
		              dueTenSeconds ? 1U : 0U,
		              dueOneMinute ? 1U : 0U,
		              dueFiveMinutes ? 1U : 0U,
		              dueOneHour ? 1U : 0U,
		              dueOneDay ? 1U : 0U,
		              dueUser ? 1U : 0U,
		              ESP.getFreeHeap(),
		              ESP.getMaxFreeBlockSize(),
		              ESP.getHeapFragmentation());
	}
#endif

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

	const MqttEntityActivePlan *plan = mqttActivePlan();
	if (plan == nullptr) {
		return;
	}

	beginSchedulerPass();

	// Bucket processing is runtime-driven: due buckets iterate their pre-built membership list.
	// ESS snapshot is a bucket-scoped prerequisite and is refreshed once per scheduler pass
	// (even if multiple buckets are due at the same time).
	bool snapshotAttemptedThisPass = false;
	bool snapshotOkThisPass = essSnapshotValid;

	if (dueTenSeconds) {
		const bool snapshotOkThisBucket =
			ensureSnapshotForBucketPass(plan->tenSec.hasEssSnapshot,
			                            snapshotAttemptedThisPass,
			                            snapshotOkThisPass);
		maybeYield();
#if RS485_STUB
		if (rs485StubSkipNextScheduledStatusPublish) {
			rs485StubSkipNextScheduledStatusPublish = false;
		} else {
			sendStatus(snapshotOkThisBucket);
		}
#else
		sendStatus(snapshotOkThisBucket);
#endif
		runBucketTransactionsForPlan(BucketId::TenSec, plan->tenSec, snapshotOkThisBucket, pollIntervalSeconds);
	}

	if (dueOneMinute) {
		const bool snapshotOkThisBucket =
			ensureSnapshotForBucketPass(plan->oneMin.hasEssSnapshot,
			                            snapshotAttemptedThisPass,
			                            snapshotOkThisPass);
		maybeYield();
		runBucketTransactionsForPlan(BucketId::OneMin, plan->oneMin, snapshotOkThisBucket, pollIntervalSeconds);
	}

	if (dueFiveMinutes) {
		const bool snapshotOkThisBucket =
			ensureSnapshotForBucketPass(plan->fiveMin.hasEssSnapshot,
			                            snapshotAttemptedThisPass,
			                            snapshotOkThisPass);
		maybeYield();
		runBucketTransactionsForPlan(BucketId::FiveMin, plan->fiveMin, snapshotOkThisBucket, pollIntervalSeconds);
	}

	if (dueOneHour) {
		const bool snapshotOkThisBucket =
			ensureSnapshotForBucketPass(plan->oneHour.hasEssSnapshot,
			                            snapshotAttemptedThisPass,
			                            snapshotOkThisPass);
		maybeYield();
		runBucketTransactionsForPlan(BucketId::OneHour, plan->oneHour, snapshotOkThisBucket, pollIntervalSeconds);
	}

	if (dueOneDay) {
		const bool snapshotOkThisBucket =
			ensureSnapshotForBucketPass(plan->oneDay.hasEssSnapshot,
			                            snapshotAttemptedThisPass,
			                            snapshotOkThisPass);
		maybeYield();
		runBucketTransactionsForPlan(BucketId::OneDay, plan->oneDay, snapshotOkThisBucket, pollIntervalSeconds);
	}

	if (dueUser) {
		const bool snapshotOkThisBucket =
			ensureSnapshotForBucketPass(plan->user.hasEssSnapshot,
			                            snapshotAttemptedThisPass,
			                            snapshotOkThisPass);
		maybeYield();
		runBucketTransactionsForPlan(BucketId::User, plan->user, snapshotOkThisBucket, pollIntervalSeconds);
	}

	endSchedulerPass();
}

bool
sendDataFromMqttState(const mqttState *singleEntity,
                      bool doHomeAssistant,
                      const modbusRequestAndResponse *preparedResponse,
                      bool forcePublish)
{
	MqttPublishTopicScratch *publishScratch = runtimePublishTopicScratch();
	if (publishScratch == nullptr) {
		return false;
	}
	char *const topic = publishScratch->topic;
	char *const topicBase = publishScratch->topicBase;
	char *const entityKey = publishScratch->entityKey;
	modbusRequestAndResponseStatusValues result;
	modbusRequestAndResponseStatusValues resultAddedToPayload;
	DiscoveryDeviceScope scope = DiscoveryDeviceScope::Controller;
	const char *deviceId = "";

	if (singleEntity == NULL)
		return !forcePublish;
	if (!includeEntityInPublicSurfaces(*singleEntity) && !doHomeAssistant) {
		return !forcePublish;
	}
	scope = mqttEntityScope(singleEntity->entityId);
	deviceId = discoveryDeviceIdForScope(scope);
	if (!mqttEntitiesRtAvailable()) {
		return !forcePublish;
	}
	size_t idx = 0;
	if (!lookupEntityIndex(singleEntity->entityId, &idx)) {
		return !forcePublish;
	}
	mqttUpdateFreq effectiveFreq = mqttEntityEffectiveFreqByIndex(idx);
	if (!doHomeAssistant && !forcePublish &&
	    (effectiveFreq == mqttUpdateFreq::freqNever ||
	     effectiveFreq == mqttUpdateFreq::freqDisabled)) {
		return true;
	}
	if (deviceId[0] == '\0') {
		return !forcePublish;
	}
	mqttEntityNameCopy(singleEntity, entityKey, sizeof(publishScratch->entityKey));
	if (!buildEntityTopicBase(deviceName,
	                          scope,
	                          controllerIdentifier,
	                          deviceSerialNumber,
	                          entityKey,
	                          topicBase,
	                          sizeof(publishScratch->topicBase))) {
		return !forcePublish;
	}
	if (!doHomeAssistant && !snapshotPublishAllowedForEntityIndex(idx)) {
		snapshotPublishSkipCount++;
		return !forcePublish;
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

		snprintf(topic, sizeof(publishScratch->topic), "homeassistant/%s/%s/%s/config", entityType, deviceId, entityKey);
		EntityDiscoveryPayloadContext discoveryPayload{ singleEntity, scope, topicBase };
		return publishCountedMqttPayload(topic, singleEntity->retain ? MQTT_RETAIN : false, emitEntityDiscoveryPayload, &discoveryPayload);
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
			snprintf(topic, sizeof(publishScratch->topic), "%s/state", topicBase);
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
			const bool published = sendMqtt(topic, singleEntity->retain ? MQTT_RETAIN : false);
			if (published && !doHomeAssistant) {
				markBootstrapEntityPublished(idx);
			}
		return published;
	}
	return !forcePublish;
}

static bool
publishManualRegisterValueState(const mqttState *valueEntity,
                               const modbusRequestAndResponse &response,
                               bool forcePublish)
{
	if (valueEntity == nullptr || !mqttEntitiesRtAvailable()) {
		return false;
	}
	size_t idx = 0;
	if (!lookupEntityIndex(valueEntity->entityId, &idx)) {
		return false;
	}
	mqttUpdateFreq effectiveFreq = mqttEntityEffectiveFreqByIndex(idx);
	if (!forcePublish && (effectiveFreq == mqttUpdateFreq::freqNever ||
	    effectiveFreq == mqttUpdateFreq::freqDisabled)) {
		return false;
	}
	MqttPublishTopicScratch *publishScratch = runtimePublishTopicScratch();
	if (publishScratch == nullptr) {
		return false;
	}
	char *const topicBase = publishScratch->topicBase;
	char *const topic = publishScratch->topic;
	char *const entityKey = publishScratch->entityKey;
	mqttEntityNameCopy(valueEntity, entityKey, sizeof(publishScratch->entityKey));
	if (!buildEntityTopicBase(deviceName,
	                          mqttEntityScope(valueEntity->entityId),
	                          controllerIdentifier,
	                          deviceSerialNumber,
	                          entityKey,
	                          topicBase,
	                          sizeof(publishScratch->topicBase))) {
		return false;
	}
	emptyPayload();
	modbusRequestAndResponseStatusValues resultAddedToPayload = addToPayload(response.dataValueFormatted);
	if (resultAddedToPayload == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return false;
	}
	snprintf(topic, sizeof(publishScratch->topic), "%s/state", topicBase);
	sendMqtt(topic, valueEntity->retain ? MQTT_RETAIN : false);
	return true;
}

static void
publishManualRegisterReadStatus(int32_t requestedReg, const modbusRequestAndResponse &response)
{
	if (!ensureStatusJsonScratch()) {
		return;
	}
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
	if (buildStatusManualReadJson(snapshot, g_statusJsonScratch, kStatusJsonScratchSize)) {
		if (_mqtt.publish(manualReadTopic, g_statusJsonScratch, true)) {
			noteMqttActivityPulse();
		}
		maybeYield();
	}
}

static void
publishManualRegisterReadState(int32_t requestedReg)
{
#ifdef DEBUG_OVER_SERIAL
	snprintf(_debugOutput,
	         sizeof(_debugOutput),
	         "manual read begin: reg=%ld free=%u max=%u frag=%u",
	         static_cast<long>(requestedReg),
	         ESP.getFreeHeap(),
	         ESP.getMaxFreeBlockSize(),
	         ESP.getHeapFragmentation());
	Serial.println(_debugOutput);
#endif
	mqttState valueEntity{};
	if (!lookupEntity(mqttEntityId::entityRegValue, &valueEntity)) {
#ifdef DEBUG_OVER_SERIAL
		Serial.println(F("manual read abort: entityRegValue lookup failed"));
#endif
		return;
	}
	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		return;
	}
	*response = modbusRequestAndResponse{};
	const modbusRequestAndResponseStatusValues result = readEntity(&valueEntity, response);
	if (result != modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
#ifdef DEBUG_OVER_SERIAL
		snprintf(_debugOutput,
		         sizeof(_debugOutput),
		         "manual read abort: reg=%ld result=%d free=%u max=%u frag=%u",
		         static_cast<long>(requestedReg),
		         static_cast<int>(result),
		         ESP.getFreeHeap(),
		         ESP.getMaxFreeBlockSize(),
		         ESP.getHeapFragmentation());
		Serial.println(_debugOutput);
#endif
		return;
	}
#ifdef DEBUG_OVER_SERIAL
	snprintf(_debugOutput,
	         sizeof(_debugOutput),
	         "manual read value: reg=%ld observed='%s' free=%u max=%u frag=%u",
	         static_cast<long>(requestedReg),
	         response->dataValueFormatted,
	         ESP.getFreeHeap(),
	         ESP.getMaxFreeBlockSize(),
	         ESP.getHeapFragmentation());
	Serial.println(_debugOutput);
#endif
	publishManualRegisterValueState(&valueEntity, *response, true);
	publishManualRegisterReadStatus(requestedReg, *response);
#ifdef DEBUG_OVER_SERIAL
	snprintf(_debugOutput,
	         sizeof(_debugOutput),
	         "manual read done: reg=%ld free=%u max=%u frag=%u",
	         static_cast<long>(requestedReg),
	         ESP.getFreeHeap(),
	         ESP.getMaxFreeBlockSize(),
	         ESP.getHeapFragmentation());
	Serial.println(_debugOutput);
#endif
}

static bool
publishRegisterNumberStateValue(int32_t requestedReg)
{
	if (!mqttEntitiesRtAvailable()) {
		return false;
	}
	mqttState regEntity{};
	if (!lookupEntity(mqttEntityId::entityRegNum, &regEntity)) {
		return false;
	}
	size_t idx = 0;
	if (!lookupEntityIndex(regEntity.entityId, &idx)) {
		return false;
	}
	const mqttUpdateFreq effectiveFreq = mqttEntityEffectiveFreqByIndex(idx);
	if (effectiveFreq == mqttUpdateFreq::freqNever || effectiveFreq == mqttUpdateFreq::freqDisabled) {
		return false;
	}
	MqttPublishTopicScratch *publishScratch = runtimePublishTopicScratch();
	if (publishScratch == nullptr) {
		return false;
	}
	char *const topicBase = publishScratch->topicBase;
	char *const topic = publishScratch->topic;
	char *const entityKey = publishScratch->entityKey;
	mqttEntityNameCopy(&regEntity, entityKey, sizeof(publishScratch->entityKey));
	if (!buildEntityTopicBase(deviceName,
	                          mqttEntityScope(regEntity.entityId),
	                          controllerIdentifier,
	                          deviceSerialNumber,
	                          entityKey,
	                          topicBase,
	                          sizeof(publishScratch->topicBase))) {
		return false;
	}
	emptyPayload();
	char valueBuf[16];
	const int written = snprintf(valueBuf, sizeof(valueBuf), "%ld", static_cast<long>(requestedReg));
	if (written <= 0 || static_cast<size_t>(written) >= sizeof(valueBuf)) {
		return false;
	}
	if (addToPayload(valueBuf) == modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		return false;
	}
	snprintf(topic, sizeof(publishScratch->topic), "%s/state", topicBase);
	return sendMqtt(topic, regEntity.retain ? MQTT_RETAIN : false);
}

static void
processPendingEntityCommand(void)
{
	if (!pendingEntityCommandSet) {
		return;
	}
	if (pendingDeferredControlPayload == nullptr) {
		pendingEntityCommandSet = false;
		pendingEntityCommandId = mqttEntityId::entityRegNum;
		return;
	}

	struct PendingEntityCommandGuard {
		~PendingEntityCommandGuard()
		{
			pendingEntityCommandSet = false;
			pendingEntityCommandId = mqttEntityId::entityRegNum;
			pendingDeferredControlPayload[0] = '\0';
		}
	} guard;

	mqttState mqttEntity{};
	if (!mqttEntityCopyById(pendingEntityCommandId, &mqttEntity)) {
		return;
	}
	const char *mqttIncomingPayload = pendingDeferredControlPayload;

	int32_t singleInt32 = -1;
	const char *singleString = NULL;
	char *endPtr = NULL;
	bool valueProcessingError = false;
	bool dispatchRelevantChange = false;
	bool timedGenerationRequested = false;
	bool handledOwnStatePublish = false;

	// First, process value.
	switch (mqttEntity.entityId) {
	case mqttEntityId::entitySocTarget:
	case mqttEntityId::entityChargePwr:
	case mqttEntityId::entityDischargePwr:
	case mqttEntityId::entityPushPwr:
	case mqttEntityId::entityDispatchDuration:
	case mqttEntityId::entityMaxFeedinPercent:
	case mqttEntityId::entityRegNum:
		errno = 0;
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
		sprintf(_debugOutput, "Trying to update an unhandled entity! %d", mqttEntity.entityId);
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
		mqttEntityNameCopy(&mqttEntity, entityName, sizeof(entityName));
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
	switch (mqttEntity.entityId) {
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
			dispatchRelevantChange = true;
			timedGenerationRequested = true;
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
			dispatchRelevantChange = true;
			timedGenerationRequested = true;
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
			dispatchRelevantChange = true;
			timedGenerationRequested = true;
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
			dispatchRelevantChange = true;
			timedGenerationRequested = true;
		}
		break;
	case mqttEntityId::entityDispatchDuration:
		if (singleInt32 < 0) {
#ifdef DEBUG_OVER_SERIAL
			sprintf(_debugOutput, "HA sent invalid Dispatch Duration! %ld", singleInt32);
			Serial.println(_debugOutput);
#endif
#ifdef DEBUG_CALLBACKS
			badCallbacks++;
#endif // DEBUG_CALLBACKS
		} else {
			timedDispatchState.configuredDurationSeconds =
				clampDispatchDurationSeconds(static_cast<uint32_t>(singleInt32));
#ifdef DEBUG_OVER_SERIAL
			snprintf(_debugOutput,
			         sizeof(_debugOutput),
			         "Dispatch_Duration applied: requested=%ld configured=%lu",
			         singleInt32,
			         static_cast<unsigned long>(timedDispatchState.configuredDurationSeconds));
			Serial.println(_debugOutput);
#endif
			dispatchRelevantChange = true;
			timedGenerationRequested = dispatchDurationIsTimed(timedDispatchState.configuredDurationSeconds);
		}
		break;
	case mqttEntityId::entityMaxFeedinPercent:
		if ((singleInt32 < 0) || (singleInt32 > 100)) {
#ifdef DEBUG_OVER_SERIAL
			sprintf(_debugOutput, "HA sent invalid Max_Feedin_Percent! %ld", singleInt32);
			Serial.println(_debugOutput);
#endif
#ifdef DEBUG_CALLBACKS
			badCallbacks++;
#endif // DEBUG_CALLBACKS
		} else {
			handledOwnStatePublish = true;
			applyMaxFeedinPercentCommand(&mqttEntity, singleInt32);
		}
		break;
	case mqttEntityId::entityRegNum:
#ifdef DEBUG_OVER_SERIAL
		snprintf(_debugOutput,
		         sizeof(_debugOutput),
		         "Register_Number apply: payload=%ld free=%u max=%u frag=%u",
		         static_cast<long>(singleInt32),
		         ESP.getFreeHeap(),
		         ESP.getMaxFreeBlockSize(),
		         ESP.getHeapFragmentation());
		Serial.println(_debugOutput);
#endif
		regNumberToRead = singleInt32; // Set local variable
		publishManualRegisterReadState(singleInt32);
		handledOwnStatePublish = publishRegisterNumberStateValue(singleInt32);
		break;
	case mqttEntityId::entityOpMode:
		{
			enum opMode tempOpMode = lookupOpMode(singleString);
			if (tempOpMode != (enum opMode)-1) {
				opData.a2mOpMode = tempOpMode;
				opData.a2mReadyToUseOpMode = true;
				dispatchRelevantChange = true;
				if (tempOpMode == opMode::opModeNormal) {
					modbusRequestAndResponse *response = runtimeModbusReadScratch();
					if (_registerHandler != NULL && response != nullptr) {
						*response = modbusRequestAndResponse{};
						modbusRequestAndResponseStatusValues result = _registerHandler->writeDispatchStop(response);
						if (result == modbusRequestAndResponseStatusValues::writeDataRegisterSuccess) {
							opData.essDispatchStart = DISPATCH_START_STOP;
							refreshEssSnapshotAfterDispatch(true);
						} else {
							recordRs485Error(result);
						}
					}
					timedDispatchState.completedGeneration = timedDispatchState.requestedGeneration;
					timedDispatchState.restartAfterStop = false;
				} else {
					timedGenerationRequested = dispatchDurationIsTimed(timedDispatchState.configuredDurationSeconds);
				}
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
		sprintf(_debugOutput, "Trying to write an unhandled entity! %d", mqttEntity.entityId);
		Serial.println(_debugOutput);
#endif
		break;
	}

	if (dispatchRelevantChange) {
		timedDispatchState.evalPending = true;
		timedDispatchState.lastEvalMs = millis() - kDispatchHandshakeIntervalMs;
		if (timedGenerationRequested) {
			dispatchNoteRequestedGeneration(timedDispatchState);
		}
	}

	// Send (hopefully) updated state. If we failed to update, sender should notice value not changing.
#ifdef DEBUG_OVER_SERIAL
	if (mqttEntity.entityId == mqttEntityId::entityDispatchDuration) {
		snprintf(_debugOutput,
		         sizeof(_debugOutput),
		         "Dispatch_Duration publishing state=%lu",
		         static_cast<unsigned long>(timedDispatchState.configuredDurationSeconds));
		Serial.println(_debugOutput);
	} else if (mqttEntity.entityId == mqttEntityId::entityRegNum) {
		snprintf(_debugOutput,
		         sizeof(_debugOutput),
		         "Register_Number publish state begin: reg=%ld free=%u max=%u frag=%u",
		         static_cast<long>(regNumberToRead),
		         ESP.getFreeHeap(),
		         ESP.getMaxFreeBlockSize(),
		         ESP.getHeapFragmentation());
		Serial.println(_debugOutput);
	}
#endif
	if (!handledOwnStatePublish) {
		sendDataFromMqttState(&mqttEntity, false, nullptr, true);
	}
#ifdef DEBUG_OVER_SERIAL
	if (mqttEntity.entityId == mqttEntityId::entityRegNum) {
		snprintf(_debugOutput,
		         sizeof(_debugOutput),
		         "Register_Number publish state done: reg=%ld free=%u max=%u frag=%u",
		         static_cast<long>(regNumberToRead),
		         ESP.getFreeHeap(),
		         ESP.getMaxFreeBlockSize(),
		         ESP.getHeapFragmentation());
		Serial.println(_debugOutput);
	}
#endif
}

static bool
entityCommandEnabled(mqttEntityId entityId)
{
	size_t idx = 0;
	if (!lookupEntityIndex(entityId, &idx)) {
		return false;
	}
	const mqttUpdateFreq effectiveFreq = mqttEntityEffectiveFreqByIndex(idx);
	return effectiveFreq != mqttUpdateFreq::freqNever && effectiveFreq != mqttUpdateFreq::freqDisabled;
}

static bool
applyMaxFeedinPercentCommand(const mqttState *entity, int32_t requestedPercent)
{
	if (entity == nullptr || _registerHandler == nullptr) {
		return false;
	}
	if (!entityCommandEnabled(entity->entityId)) {
#ifdef DEBUG_OVER_SERIAL
		Serial.println(F("Ignoring Max_Feedin_Percent command while entity is disabled"));
#endif
		return false;
	}

	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		return false;
	}
	*response = modbusRequestAndResponse{};
	const modbusRequestAndResponseStatusValues writeResult =
		_registerHandler->writeRawSingleRegister(
			REG_SYSTEM_CONFIG_RW_MAX_FEED_INTO_GRID_PERCENT,
			static_cast<uint16_t>(requestedPercent),
			response);
	if (writeResult != modbusRequestAndResponseStatusValues::writeSingleRegisterSuccess) {
		recordRs485Error(writeResult);
		noteRs485Error(writeResult, response->statusMqttMessage);
		return false;
	}

	*response = modbusRequestAndResponse{};
	response->functionCode = MODBUS_FN_READDATAREGISTER;
	response->registerCount = 1;
	response->returnDataType = modbusReturnDataType::unsignedShort;
	strlcpy(response->returnDataTypeDesc,
	        MODBUS_RETURN_DATA_TYPE_UNSIGNED_SHORT_DESC,
	        sizeof(response->returnDataTypeDesc));
	const modbusRequestAndResponseStatusValues readResult =
		_registerHandler->readRawRegister(REG_SYSTEM_CONFIG_RW_MAX_FEED_INTO_GRID_PERCENT, response);
	if (readResult != modbusRequestAndResponseStatusValues::readDataRegisterSuccess) {
		recordRs485Error(readResult);
		noteRs485Error(readResult, response->statusMqttMessage);
		return false;
	}
	if (response->unsignedShortValue != static_cast<uint16_t>(requestedPercent)) {
#ifdef DEBUG_OVER_SERIAL
		snprintf(_debugOutput,
		         sizeof(_debugOutput),
		         "Max_Feedin_Percent readback mismatch requested=%ld readback=%u",
		         static_cast<long>(requestedPercent),
		         static_cast<unsigned>(response->unsignedShortValue));
		Serial.println(_debugOutput);
#endif
		return false;
	}
	return sendDataFromMqttState(entity, false, response, true);
}

static void __attribute__((noinline))
servicePendingRs485StubControl(void)
{
#if RS485_STUB
	Rs485StubControlRequest *request = ensureRs485StubControlRequestScratch();
	if (request == nullptr) {
		return;
	}
	applyParsedRs485StubControl(*request);
#endif
}

static bool __attribute__((noinline))
servicePendingRs485StubControlInLoop(void)
{
	if (!pendingRs485StubControlSet) {
		return false;
	}

	pendingRs485StubControlSet = false;
	servicePendingRs485StubControl();
#if RS485_STUB
	if (rs485StubStatusAckPending) {
		(void)publishStubControlStatusNow(false);
		(void)publishStatusCoreNow();
		rs485StubStatusAckPending = false;
	}
	#ifdef DEBUG_OVER_SERIAL
	Serial.println(F("RS485 stub control helper complete"));
	#endif
	rs485StubControlProcessedThisLoop = true;
#endif
	return true;
}

static void __attribute__((noinline))
serviceDeferredMqttWork(void)
{
	for (uint8_t iteration = 0; iteration < kDeferredMqttDrainMaxIterations; ++iteration) {
		bool didWork = false;

		if (dispatchMirrorPublishPending && publishPendingDispatchMirror()) {
			didWork = true;
		}
		if (dispatchRequestStatusDirty && publishDispatchRequestStatus()) {
			didWork = true;
		}
		if (pendingPollingConfigSet) {
			processPendingPollingConfigPayload();
			didWork = true;
		}
		if (pendingRs485StubControlSet) {
			// Process deferred stub-control publishes from loop() on a dedicated smaller frame.
			return;
		}
		if (pendingEntityCommandSet) {
			processPendingEntityCommand();
			didWork = true;
		}
		if (pendingDispatchRequestSet) {
			processPendingDispatchRequest();
			didWork = true;
		}
		if (atomicDispatchState.inFlight) {
			serviceAtomicDispatchRequest();
			didWork = true;
		}
		if (pendingPollingConfigSet || pendingRs485StubControlSet || pendingEntityCommandSet ||
		    pendingDispatchRequestSet) {
			continue;
		}

		const uint32_t callbackSequenceBeforePump = mqttCallbackSequence;
		if (!pumpMqttOnce()) {
			return;
		}
		if (mqttCallbackSequence != callbackSequenceBeforePump) {
			didWork = true;
		}
		if (!didWork) {
			return;
		}
	}
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
	bool hasVirtualBaud = false;
	uint32_t virtualBaud = 9600;

	bool hasVirtualDispatch = false;
	uint16_t virtualDispatchStart = 0;
	uint16_t virtualDispatchMode = DISPATCH_MODE_NORMAL_MODE;
	int32_t virtualDispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET;
	uint16_t virtualDispatchSoc = 0;
	uint32_t virtualDispatchTime = 0;
};

// ESP8266 loop stack is tight once deferred MQTT control handling, stub state mutation,
// and reconnect/bootstrap logic share the same frame. Keep the parsed request off the
// loop stack, but allocate it lazily after boot so it does not count against boot heap
// checkpoints in NORMAL startup.
static Rs485StubControlRequest *g_rs485StubControlRequestScratch = nullptr;

static Rs485StubControlRequest *
ensureRs485StubControlRequestScratch(void)
{
	if (g_rs485StubControlRequestScratch != nullptr) {
		return g_rs485StubControlRequestScratch;
	}
	g_rs485StubControlRequestScratch = new (std::nothrow) Rs485StubControlRequest();
	return g_rs485StubControlRequestScratch;
}

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

static bool
parseRs485StubControlPayload(const char *payload, Rs485StubControlRequest &request)
{
	int32_t value = 0;

	if (!parseStubControlMode(payload, request.mode)) {
		return false;
	}

	if (parseStubControlInt(payload, "fail_n", value) ||
	    parseStubControlInt(payload, "failFirstN", value)) {
		if (value >= 0) {
			request.failN = static_cast<uint32_t>(value);
		}
	}

	if (parseStubControlInt(payload, "reg", value) ||
	    parseStubControlInt(payload, "register", value)) {
		if (value >= 0) {
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
	if (parseStubControlInt(payload, "fail_every_n", value) && value >= 0) {
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
	if (parseStubControlInt(payload, "modbus_baud", value) && value >= 0 &&
	    rs485BaudValueSupported(static_cast<uint32_t>(value))) {
		request.virtualBaud = static_cast<uint32_t>(value);
		request.hasVirtualBaud = true;
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
	if (parseStubControlInt(payload, "dispatch_time", value) && value >= 0) {
		request.virtualDispatchTime = static_cast<uint32_t>(value);
		request.hasVirtualDispatch = true;
	}

	return true;
}

static void __attribute__((noinline))
applyParsedRs485StubControl(const Rs485StubControlRequest &request)
{
	_modBus->applyStubControl(request.mode, request.failN, request.failReg, request.failType, request.latencyMs);
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
	if (request.hasVirtualEss) {
		_modBus->applyVirtualInverterState(
			request.virtualSocPct,
			request.virtualBatteryPowerW,
			request.virtualGridPowerW,
			request.virtualPvCtPowerW,
			request.virtualInverterMode);
	}
	if (request.hasVirtualBaud) {
		uint16_t regValue = 0;
		if (rs485BaudValueToRegister(request.virtualBaud, regValue)) {
			_modBus->applyVirtualModbusBaud(regValue);
		}
	}
	if (request.hasVirtualDispatch) {
		_modBus->applyVirtualDispatchState(
			request.virtualDispatchStart,
			request.virtualDispatchMode,
			request.virtualDispatchActivePower,
			request.virtualDispatchSoc,
			request.virtualDispatchTime);
	}
	rs485ApplyStubConnectivityMode(request.mode);
#ifdef DEBUG_OVER_SERIAL
	Serial.print(F("RS485 stub control applied: mode="));
	Serial.print(_modBus->stubModeLabel());
	Serial.print(F(" ess="));
	Serial.print(request.hasVirtualEss ? 1 : 0);
	Serial.print(F(" dispatch="));
	Serial.println(request.hasVirtualDispatch ? 1 : 0);
#endif
	if (rs485StubModeUsesProbeLifecycle(request.mode)) {
		// Offline/probe-style stub transitions can publish status immediately because they do not
		// also trigger inverter subscription setup and the first live snapshot path.
		rs485StubStatusAckPending = false;
		publishStatusNow();
	} else {
		// Online-like stub transitions still get a lightweight immediate control acknowledgement.
		// Keep the scheduler cooldown and skip the next full 10 s status pass so the immediate
		// poll/stub/core acknowledgement does not stack with a second publish burst on the same
		// transition.
		rs485StubStatusAckPending = true;
		rs485StubSkipNextScheduledStatusPublish = true;
		rs485StubControlSchedulerCooldownUntilMs = millis() + 5000U;
		rs485StubLastOnlineControlMs = millis();
#ifdef DEBUG_OVER_SERIAL
		Serial.println(F("RS485 stub control online cooldown armed"));
#endif
	}
}

static void __attribute__((noinline))
applyRs485StubControlPayload(const char *payload)
{
	if (payload == nullptr || _modBus == nullptr) {
		return;
	}
	Rs485StubControlRequest *request = ensureRs485StubControlRequestScratch();
	if (request == nullptr) {
		return;
	}

	// Chose a helper-based parser here because ESP8266 loop stack is tight and the
	// earlier monolithic local-heavy parser triggered watchdog resets on control publish.
	*request = Rs485StubControlRequest{};
	if (!parseRs485StubControlPayload(payload, *request)) {
		return;
	}
	maybeYield();
	applyParsedRs485StubControl(*request);
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
	mqttCallbackSequence++;
	noteMqttActivityPulse();

#ifdef DEBUG_OVER_SERIAL
	sprintf(_debugOutput, "Topic: %s", topic);
	Serial.println(_debugOutput);
	if (strstr(topic, "Dispatch_Duration") != nullptr) {
		const char *dbgInverterId = discoveryDeviceIdForScope(DiscoveryDeviceScope::Inverter);
		snprintf(_debugOutput,
		         sizeof(_debugOutput),
		         "Dispatch_Duration pre-gate: ready=%u device=%s inverter=%s",
		         inverterReady ? 1U : 0U,
		         deviceName,
		         dbgInverterId);
		Serial.println(_debugOutput);
	}
#endif

#ifdef DEBUG_CALLBACKS
	receivedCallbacks++;
#endif // DEBUG_CALLBACKS

	if (topicEqualsDeviceSuffix(topic, "/config/set")) {
		// Defer config/set out of callback context without pinning a 2 KB global scratch
		// in NORMAL mode. Queue an exact-size copy and let loop() parse/free it later.
		if (!queuePendingPollingConfigPayload(reinterpret_cast<const char *>(message), length)) {
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

#if RS485_STUB
	if (topicEqualsDeviceSuffix(topic, "/debug/rs485_stub/set")) {
		Rs485StubControlRequest *request = ensureRs485StubControlRequestScratch();
		if (request == nullptr ||
		    !ensureDeferredControlPayload() ||
		    !copyLengthDelimitedString(reinterpret_cast<const char *>(message),
		                              length,
		                              pendingDeferredControlPayload,
		                              kPendingDeferredControlPayloadSize)) {
#ifdef DEBUG_OVER_SERIAL
			sprintf(_debugOutput, "mqttCallback: bad stub control length: %d", length);
			Serial.println(_debugOutput);
#endif
#ifdef DEBUG_CALLBACKS
			badCallbacks++;
#endif // DEBUG_CALLBACKS
			return;
		}
		*request = Rs485StubControlRequest{};
		if (!parseRs485StubControlPayload(pendingDeferredControlPayload, *request)) {
#ifdef DEBUG_OVER_SERIAL
			sprintf(_debugOutput, "mqttCallback: bad stub control payload");
			Serial.println(_debugOutput);
#endif
#ifdef DEBUG_CALLBACKS
			badCallbacks++;
#endif // DEBUG_CALLBACKS
			pendingDeferredControlPayload[0] = '\0';
			return;
		}
		pendingDeferredControlPayload[0] = '\0';
#ifdef DEBUG_OVER_SERIAL
		sprintf(_debugOutput, "Stub control payload: %d", length);
		Serial.println(_debugOutput);
#endif
		pendingRs485StubControlSet = true;
		return;
	}
#endif

	if (!ensureDeferredControlPayload() ||
	    !copyLengthDelimitedString(reinterpret_cast<const char *>(message),
	                              length,
	                              pendingDeferredControlPayload,
	                              kPendingDeferredControlPayloadSize)) {
#ifdef DEBUG_OVER_SERIAL
		sprintf(_debugOutput, "mqttCallback: bad length: %d", length);
		Serial.println(_debugOutput);
#endif
#ifdef DEBUG_CALLBACKS
		badCallbacks++;
#endif // DEBUG_CALLBACKS
		return; // We won't be doing anything
	}
	char *mqttIncomingPayload = pendingDeferredControlPayload;
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
	} else if (topicEqualsDeviceSuffix(topic, "/config/set")) {
		return; // handled above
	} else {
		mqttState mqttEntity{};
		bool haveMqttEntity = false;
		const char *inverterDeviceId = discoveryDeviceIdForScope(DiscoveryDeviceScope::Inverter);
		const size_t deviceNameLen = strlen(deviceName);
		const size_t topicLen = strlen(topic);
		const bool topicUnderDevice =
			(deviceNameLen > 0) &&
			(strncmp(topic, deviceName, deviceNameLen) == 0) &&
			(topic[deviceNameLen] == '/');
		const size_t inverterDeviceIdLen = strlen(inverterDeviceId);

		if (inverterReady &&
		    inverterDeviceId[0] != '\0' &&
		    topicLen == inverterDeviceIdLen + strlen("/dispatch/set") &&
		    strncmp(topic, inverterDeviceId, inverterDeviceIdLen) == 0 &&
		    strcmp(topic + inverterDeviceIdLen, "/dispatch/set") == 0) {
#ifdef DEBUG_OVER_SERIAL
				Serial.println(F("dq:cb:hit"));
#endif
				if (mqttCommandWarmupActive()) {
#ifdef DEBUG_OVER_SERIAL
					snprintf(_debugOutput,
					         sizeof(_debugOutput),
					         "Atomic dispatch warmup drop: age_ms=%lu",
					         static_cast<unsigned long>(millis() - lastMqttConnectMs));
					Serial.println(_debugOutput);
#endif
					return;
				}
				const bool coalesceDuringSnapshot =
					shouldQueueDispatchRequest(pendingDispatchRequestSet,
					                           atomicDispatchState.inFlight,
					                           essSnapshotBuildInProgress);
				if (!coalesceDuringSnapshot &&
				    dispatchRequestShouldRejectNewRequest(pendingDispatchRequestSet,
				                                        atomicDispatchState.inFlight)) {
#ifdef DEBUG_OVER_SERIAL
					Serial.println(F("dq:cb:reject"));
#endif
					setDispatchRequestStatus("dispatch request already in progress");
					return;
				}
				if (!ensureDispatchPayload() ||
				    !copyLengthDelimitedString(reinterpret_cast<const char *>(message),
				                              length,
				                              pendingDispatchPayload,
				                              kPendingDispatchPayloadSize)) {
#ifdef DEBUG_CALLBACKS
					badCallbacks++;
#endif // DEBUG_CALLBACKS
#ifdef DEBUG_OVER_SERIAL
					Serial.println(F("dq:cb:bad-payload"));
#endif
					setDispatchRequestStatus("invalid mode");
					return;
				}
				dispatchRequestQueuedMs = millis();
				if (essSnapshotBuildInProgress) {
					schedulerPassCache.dispatchQueuedDuringSnapshot = true;
					if (coalesceDuringSnapshot) {
						schedulerPassCache.dispatchQueueCoalesceCount++;
						dispatchQueueCoalesceCount++;
					}
				}
				pendingDispatchRequestSet = true;
#ifdef DEBUG_OVER_SERIAL
				Serial.print(F("dq:cb:queued:"));
				Serial.println(static_cast<unsigned long>(dispatchRequestQueuedMs));
#endif
				return;
		}
#ifdef DEBUG_OVER_SERIAL
		if (topicUnderDevice &&
		    topicLen >= strlen("/command") &&
		    !strcmp(&topic[topicLen - strlen("/command")], "/command")) {
			snprintf(_debugOutput,
			         sizeof(_debugOutput),
			         "MQTT command gate: ready=%u inverter='%s' topic=%s",
			         inverterReady ? 1U : 0U,
			         inverterDeviceId,
			         topic);
			Serial.println(_debugOutput);
		}
#endif
		if (inverterReady &&
		    inverterDeviceId[0] != '\0' &&
		    topicUnderDevice &&
		    topicLen >= strlen("/command") &&
		    !strcmp(&topic[topicLen - strlen("/command")], "/command")) {
			if (mqttCommandWarmupActive()) {
#ifdef DEBUG_OVER_SERIAL
				snprintf(_debugOutput,
				         sizeof(_debugOutput),
				         "MQTT command warmup drop: topic=%s age_ms=%lu",
				         topic,
				         static_cast<unsigned long>(millis() - lastMqttConnectMs));
				Serial.println(_debugOutput);
#endif
				return;
			}
			const char *topicAfterDevice = &topic[deviceNameLen + 1];
			const char *entitySep = strchr(topicAfterDevice, '/');
			if (entitySep != nullptr) {
				size_t topicDeviceIdLen = static_cast<size_t>(entitySep - topicAfterDevice);
				const char *entityNameStart = entitySep + 1;
				const char *entityNameEnd = &topic[topicLen - strlen("/command")];
				if (entityNameEnd > entityNameStart &&
				    topicDeviceIdLen == inverterDeviceIdLen &&
				    strncmp(topicAfterDevice, inverterDeviceId, inverterDeviceIdLen) == 0) {
						haveMqttEntity = lookupSubscriptionSpan(entityNameStart,
						                                        static_cast<size_t>(entityNameEnd - entityNameStart),
						                                        &mqttEntity);
#ifdef DEBUG_OVER_SERIAL
						snprintf(_debugOutput,
						         sizeof(_debugOutput),
						         "MQTT command lookup len=%u have=%u",
						         static_cast<unsigned>(entityNameEnd - entityNameStart),
						         haveMqttEntity ? 1U : 0U);
						Serial.println(_debugOutput);
#endif
				}
			}
		}
		if (!haveMqttEntity) {
	#ifdef DEBUG_CALLBACKS
			unknownCallbacks++;
	#endif // DEBUG_CALLBACKS
			return; // No further processing possible.
		}
#ifdef DEBUG_OVER_SERIAL
		if (mqttEntity.entityId == mqttEntityId::entityDispatchDuration) {
			snprintf(_debugOutput,
			         sizeof(_debugOutput),
			         "Dispatch_Duration command queued: topic=%s payload=%s",
			         topic,
			         mqttIncomingPayload);
			Serial.println(_debugOutput);
		}
#endif

		// Defer command application out of callback context to avoid deep call chains while PubSubClient
		// is executing loop() and to keep RS485 writes on the main loop path.
		if (strlen(mqttIncomingPayload) >= kPendingDeferredControlPayloadSize) {
	#ifdef DEBUG_CALLBACKS
			badCallbacks++;
	#endif // DEBUG_CALLBACKS
			return;
		}
		pendingEntityCommandId = mqttEntity.entityId;
		pendingEntityCommandSet = true;
		return;
	}
}


/*
 * noteMqttActivityPulse
 *
 * Latches a short activity pulse so updateStatusLed() can render it without
 * doing any GPIO work in MQTT callback/publish call paths.
 */
static void
noteMqttActivityPulse(void)
{
	const uint32_t nowMs = millis();
	mqttActivityPulseUntilMs = nowMs + kMqttActivityPulseMs;
}

static bool
subscribeMqttReconnectTopic(bool current, const char *topic)
{
	if (topic == nullptr || topic[0] == '\0') {
		return false;
	}
	return current && _mqtt.subscribe(topic, MQTT_SUBSCRIBE_QOS);
}

#if defined(MP_ESP8266)
static void
guardPendingAsyncWifiScanBeforeMqttReconnect(void)
{
	// Guard any in-flight async scan before reconnect. This is kept out of
	// mqttReconnect() so the reconnect frame stays small on ESP8266.
	const int8_t scanState = WiFi.scanComplete();
#ifdef DEBUG_OVER_SERIAL
	Serial.printf("WiFi guard scan state %d\r\n", scanState);
#endif
	const WifiScanGuardAction action = classifyWifiScanGuard(scanState);
	if (action == WifiScanGuardAction::RebindNoopCallback) {
		WiFi.scanNetworksAsync(wifiScanCompleteNoop, false);
		return;
	}
	if (action == WifiScanGuardAction::DeleteResults) {
		WiFi.scanDelete();
	}
}
#endif

#ifdef DEBUG_OVER_SERIAL
static void
debugLogMqttReconnectProbe(void)
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

static bool __attribute__((noinline))
subscribeMqttReconnectTopics(bool *inverterSubscriptionsAdded)
{
	char subscription[kRuntimeTopicScratchSize] = "";
	if (inverterSubscriptionsAdded != nullptr) {
		*inverterSubscriptionsAdded = false;
	}

	snprintf(subscription,
	         sizeof(subscription),
	         "%s",
	         MQTT_SUB_HOMEASSISTANT);
	bool subscribed = subscribeMqttReconnectTopic(true, subscription);
#ifdef DEBUG_OVER_SERIAL
	snprintf(_debugOutput,
	         sizeof(_debugOutput),
	         "Subscribed to \"%s\" : %d",
	         subscription,
	         subscribed);
	Serial.println(_debugOutput);
#endif

	snprintf(subscription,
	         sizeof(subscription),
	         "%s/config/set",
	         deviceName);
	subscribed = subscribeMqttReconnectTopic(subscribed, subscription);
#ifdef DEBUG_OVER_SERIAL
	snprintf(_debugOutput,
	         sizeof(_debugOutput),
	         "Subscribed to \"%s\" : %d",
	         subscription,
	         subscribed);
	Serial.println(_debugOutput);
#endif

#if RS485_STUB
	snprintf(subscription,
	         sizeof(subscription),
	         "%s/debug/rs485_stub/set",
	         deviceName);
	subscribed = subscribeMqttReconnectTopic(subscribed, subscription);
#ifdef DEBUG_OVER_SERIAL
	snprintf(_debugOutput, sizeof(_debugOutput), "Subscribed to \"%s\" : %d", subscription, subscribed);
	Serial.println(_debugOutput);
#endif
#endif

	return subscribed;
}

static void __attribute__((noinline))
completeSuccessfulMqttReconnect(unsigned long attemptStart, int tries, bool inverterSubscriptionsAdded)
{
#ifdef DEBUG_OVER_SERIAL
	Serial.printf("mqttReconnect attempt %d succeeded after %lu ms\r\n", tries, millis() - attemptStart);
#endif
	(void)inverterSubscriptionsAdded;
	setStatusLedColor(0, 255, 0);
	updateRunstate();
	handleMqttReconnectDispatchReset();
	requestHaDataResend();
	pendingPollingConfigPublish = true;
}


/*
 * sendMqtt
 *
 * Sends whatever is in the modular level payload to the specified topic.
 */
static bool
publishTrackedTextPayload(const char *topic, const char *payload, bool retain)
{
	static unsigned long lastFailureLogMs = 0;
	const unsigned long nowMs = millis();
	const size_t payloadLen = (payload != nullptr) ? strlen(payload) : 0;

	noteTrackedMqttPayload(currentRuntimeDiagPayloadKind, payloadLen);

	if (!_mqtt.publish(topic, (payload != nullptr) ? payload : "", retain)) {
#ifdef DEBUG_OVER_SERIAL
		if ((nowMs - lastFailureLogMs) >= 3000) {
			const size_t previewLen = (payloadLen < 96) ? payloadLen : 96;
			lastFailureLogMs = nowMs;
			Serial.printf("MQTT publish failed: topic=%s bytes=%u preview=%.*s\r\n",
			              topic,
			              static_cast<unsigned>(payloadLen),
			              static_cast<int>(previewLen),
			              payload != nullptr ? payload : "");
		}
#endif
		noteRuntimePhaseObservation(currentRuntimeDiagPhase);
		maybeYield();
		return false;
	}

	noteRuntimePhaseObservation(currentRuntimeDiagPhase);
	noteMqttActivityPulse();
	return true;
}

bool sendMqtt(const char *topic, bool retain)
{
	static unsigned long lastFailureLogMs = 0;
	const unsigned long nowMs = millis();
	const size_t payloadLen = _mqttPayload ? strlen(_mqttPayload) : 0;

	noteTrackedMqttPayload(currentRuntimeDiagPayloadKind, payloadLen);

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
		noteRuntimePhaseObservation(currentRuntimeDiagPhase);
		maybeYield();
		emptyPayload();
		return false;
	} else {
	}

	// Empty payload for next use.
	noteRuntimePhaseObservation(currentRuntimeDiagPhase);
	noteMqttActivityPulse();
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
	case opMode::opModeNormal:
		strlcpy(dest, OP_MODE_DESC_NORMAL, size);
		break;
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
	if (!strcmp(opModeDesc, OP_MODE_DESC_NORMAL))
		return opMode::opModeNormal;
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
publishDispatchStateEntity(mqttEntityId entityId)
{
	if (!mqttSubsystemEnabled()) {
		return;
	}
	mqttState entity{};
	if (!lookupEntity(entityId, &entity)) {
		return;
	}
	sendDataFromMqttState(&entity, false, nullptr, true);
}

static void
publishDispatchAuxiliaryStates(bool publishRawTime)
{
	publishDispatchStateEntity(mqttEntityId::entityDispatchStart);
	publishDispatchStateEntity(mqttEntityId::entityDispatchRemaining);
	if (publishRawTime) {
		publishDispatchStateEntity(mqttEntityId::entityDispatchTime);
	}
}

static bool
publishDispatchAuxiliaryStatesIfReady(bool publishRawTime)
{
	if (!mqttSubsystemEnabled() || !inverterReady || !inverterSerialKnown()) {
		return false;
	}
	publishDispatchAuxiliaryStates(publishRawTime);
	return true;
}

static bool
publishDispatchRequestStatus(void)
{
	if (!dispatchRequestStatusDirty || !mqttSubsystemEnabled()) {
		return !dispatchRequestStatusDirty;
	}
	if (!loopSequenceReached(dispatchRequestStatusPublishEarliestLoop)) {
		return false;
	}
	if (!dispatchRequestStatusShouldPublish(
	        (g_dispatchRequestStatus != nullptr) ? g_dispatchRequestStatus : "")) {
		dispatchRequestStatusDirty = false;
		A2M_DEBUG_LINE("dq:stat:skip-empty");
		return true;
	}
	if (!inverterReady || !inverterSerialKnown()) {
		A2M_DEBUG_LINE("dq:stat:wait-ready");
		return false;
	}
	mqttState entity{};
	if (!lookupEntity(mqttEntityId::entityDispatchRequestStatus, &entity)) {
		A2M_DEBUG_LINE("dq:stat:no-entity");
		return false;
	}
	MqttPublishTopicScratch *publishScratch = runtimePublishTopicScratch();
	if (publishScratch == nullptr) {
		A2M_DEBUG_LINE("dq:stat:no-scratch");
		return false;
	}
	const DiscoveryDeviceScope scope = mqttEntityScope(entity.entityId);
	const char *deviceId = discoveryDeviceIdForScope(scope);
	if (deviceId == nullptr || deviceId[0] == '\0') {
		A2M_DEBUG_LINE("dq:stat:no-device");
		return false;
	}
	mqttEntityNameCopy(&entity, publishScratch->entityKey, sizeof(publishScratch->entityKey));
	if (!buildEntityTopicBase(deviceName,
	                          scope,
	                          controllerIdentifier,
	                          deviceSerialNumber,
	                          publishScratch->entityKey,
	                          publishScratch->topicBase,
	                          sizeof(publishScratch->topicBase))) {
		A2M_DEBUG_LINE("dq:stat:topic-fail");
		return false;
	}
	snprintf(publishScratch->topic,
	         sizeof(publishScratch->topic),
	         "%s/state",
	         publishScratch->topicBase);
	emptyPayload();
	if (addToPayload((g_dispatchRequestStatus != nullptr) ? g_dispatchRequestStatus : "") ==
	    modbusRequestAndResponseStatusValues::payloadExceededCapacity) {
		A2M_DEBUG_LINE("dq:stat:payload-too-large");
		return false;
	}
	RuntimeDiagScope diagScope(RuntimeDiagPhase::StatusPublish, "req");
	if (!sendMqtt(publishScratch->topic, entity.retain ? MQTT_RETAIN : false)) {
		A2M_DEBUG_LINE("dq:stat:pub-fail");
		return false;
	}
	dispatchRequestStatusDirty = false;
	A2M_DEBUG_LINE("dq:stat:pub-ok");
	return true;
}

static bool
formatDispatchStartValue(char *dest, size_t destSize, uint16_t dispatchStart)
{
	switch (dispatchStart) {
	case DISPATCH_START_START:
		strlcpy(dest, DISPATCH_START_START_DESC, destSize);
		return true;
	case DISPATCH_START_STOP:
		strlcpy(dest, DISPATCH_START_STOP_DESC, destSize);
		return true;
	default:
		strlcpy(dest, "Unknown", destSize);
		return true;
	}
}

static bool
formatDispatchModeValue(char *dest, size_t destSize, uint16_t dispatchMode)
{
	switch (dispatchMode) {
	case DISPATCH_MODE_BATTERY_ONLY_CHARGED_VIA_PV:
		strlcpy(dest, DISPATCH_MODE_BATTERY_ONLY_CHARGED_VIA_PV_DESC, destSize);
		return true;
	case DISPATCH_MODE_STATE_OF_CHARGE_CONTROL:
		strlcpy(dest, DISPATCH_MODE_STATE_OF_CHARGE_CONTROL_DESC, destSize);
		return true;
	case DISPATCH_MODE_LOAD_FOLLOWING:
		strlcpy(dest, DISPATCH_MODE_LOAD_FOLLOWING_DESC, destSize);
		return true;
	case DISPATCH_MODE_MAXIMISE_OUTPUT:
		strlcpy(dest, DISPATCH_MODE_MAXIMISE_OUTPUT_DESC, destSize);
		return true;
	case DISPATCH_MODE_NORMAL_MODE:
		strlcpy(dest, DISPATCH_MODE_NORMAL_MODE_DESC, destSize);
		return true;
	case DISPATCH_MODE_OPTIMISE_CONSUMPTION:
		strlcpy(dest, DISPATCH_MODE_OPTIMISE_CONSUMPTION_DESC, destSize);
		return true;
	case DISPATCH_MODE_MAXIMISE_CONSUMPTION:
		strlcpy(dest, DISPATCH_MODE_MAXIMISE_CONSUMPTION_DESC, destSize);
		return true;
	case DISPATCH_MODE_ECO_MODE:
		strlcpy(dest, DISPATCH_MODE_ECO_MODE_DESC, destSize);
		return true;
	case DISPATCH_MODE_FCAS_MODE:
		strlcpy(dest, DISPATCH_MODE_FCAS_MODE_DESC, destSize);
		return true;
	case DISPATCH_MODE_PV_POWER_SETTING:
		strlcpy(dest, DISPATCH_MODE_PV_POWER_SETTING_DESC, destSize);
		return true;
	case DISPATCH_MODE_NO_BATTERY_CHARGE:
		strlcpy(dest, DISPATCH_MODE_NO_BATTERY_CHARGE_DESC, destSize);
		return true;
	case DISPATCH_MODE_BURNIN_MODE:
		strlcpy(dest, DISPATCH_MODE_BURNIN_MODE_DESC, destSize);
		return true;
	default:
		strlcpy(dest, "Unknown", destSize);
		return true;
	}
}

static bool
prepareDispatchMirrorResponse(mqttEntityId entityId,
                              const DispatchRegisterReadback &readback,
                              modbusRequestAndResponse &response)
{
	response = modbusRequestAndResponse{};
	switch (entityId) {
	case mqttEntityId::entityDispatchStart:
		response.unsignedShortValue = readback.dispatchStart;
		return formatDispatchStartValue(response.dataValueFormatted,
		                                sizeof(response.dataValueFormatted),
		                                readback.dispatchStart);
	case mqttEntityId::entityDispatchMode:
		response.unsignedShortValue = readback.dispatchMode;
		return formatDispatchModeValue(response.dataValueFormatted,
		                               sizeof(response.dataValueFormatted),
		                               readback.dispatchMode);
	case mqttEntityId::entityDispatchPower:
		response.signedIntValue = dispatchActivePowerRawToWatts(readback.dispatchActivePower);
		snprintf(response.dataValueFormatted,
		         sizeof(response.dataValueFormatted),
		         "%ld",
		         static_cast<long>(response.signedIntValue));
		return true;
	case mqttEntityId::entityDispatchSoc:
		response.unsignedShortValue = readback.dispatchSocRaw;
		snprintf(response.dataValueFormatted,
		         sizeof(response.dataValueFormatted),
		         "%0.02f",
		         readback.dispatchSocRaw * DISPATCH_SOC_MULTIPLIER);
		return true;
	case mqttEntityId::entityDispatchTime:
		response.unsignedIntValue = readback.dispatchTimeRaw;
		snprintf(response.dataValueFormatted,
		         sizeof(response.dataValueFormatted),
		         "%lu",
		         static_cast<unsigned long>(readback.dispatchTimeRaw));
		return true;
	default:
		return false;
	}
}

static bool
publishPendingDispatchMirror(void)
{
	if (!dispatchMirrorPublishPending || !mqttSubsystemEnabled()) {
		return !dispatchMirrorPublishPending;
	}
	if (!loopSequenceReached(dispatchMirrorPublishEarliestLoop)) {
		return false;
	}
	if (!inverterReady || !inverterSerialKnown()) {
		return false;
	}
	if (!dispatchMirrorPublishReadbackValid) {
		dispatchMirrorPublishPending = false;
		return true;
	}
	RuntimeDiagScope diagScope(RuntimeDiagPhase::DispatchForcePublish, "mirror");
	const mqttEntityId mirrorIds[] = {
		mqttEntityId::entityDispatchStart,
		mqttEntityId::entityDispatchMode,
		mqttEntityId::entityDispatchPower,
		mqttEntityId::entityDispatchSoc,
		mqttEntityId::entityDispatchTime,
	};
	modbusRequestAndResponse *prepared = runtimeModbusReadScratch();
	if (prepared == nullptr) {
		return false;
	}
	for (mqttEntityId entityId : mirrorIds) {
		mqttState entity{};
		*prepared = modbusRequestAndResponse{};
		if (!lookupEntity(entityId, &entity)) {
			return false;
		}
		if (!prepareDispatchMirrorResponse(entityId, dispatchMirrorPublishReadback, *prepared)) {
			return false;
		}
		if (!sendDataFromMqttState(&entity, false, prepared, true)) {
			return false;
		}
	}
	// Dispatch_Remaining is controller-derived rather than part of the inverter
	// register readback snapshot, so it still needs a normal derived publish.
	publishDispatchStateEntity(mqttEntityId::entityDispatchRemaining);
	dispatchMirrorPublishPending = false;
	dispatchMirrorPublishReadbackValid = false;
	return true;
}

static bool
readDispatchRegisterReadback(const DispatchRequestPlan &plan,
                            DispatchRegisterReadback &readback,
                            char *error,
                            size_t errorSize)
{
	(void)plan;
	readback = DispatchRegisterReadback{};
	DispatchBlockSnapshot snapshot{};
	modbusRequestAndResponseStatusValues result = modbusRequestAndResponseStatusValues::preProcessing;
	if (!fetchDispatchBlockSnapshot(snapshot, &result)) {
		recordRs485Error(result);
		strlcpy(error, "readback timeout", errorSize);
		return false;
	}
	readback.dispatchStart = snapshot.dispatchStart;
	readback.dispatchMode = snapshot.dispatchMode;
	readback.dispatchActivePower = snapshot.dispatchActivePower;
	readback.dispatchSocRaw = snapshot.dispatchSocRaw;
	readback.dispatchTimeRaw = snapshot.dispatchTimeRaw;

	opData.essDispatchStart = readback.dispatchStart;
	opData.essDispatchMode = readback.dispatchMode;
	opData.essDispatchActivePower = readback.dispatchActivePower;
	opData.essDispatchSoc = readback.dispatchSocRaw;
	opData.essDispatchTime = readback.dispatchTimeRaw;
	return true;
}

static void
processPendingDispatchRequest(void)
{
	if (!pendingDispatchRequestSet) {
		return;
	}
#ifdef DEBUG_OVER_SERIAL
	Serial.println(F("dq:proc:begin"));
#endif
	if (pendingDispatchPayload == nullptr) {
		pendingDispatchRequestSet = false;
#ifdef DEBUG_OVER_SERIAL
		Serial.println(F("dq:proc:no-buf"));
#endif
		setDispatchRequestStatus("dispatch request buffer unavailable");
		return;
	}

	struct PendingDispatchGuard {
		~PendingDispatchGuard()
		{
			pendingDispatchRequestSet = false;
			pendingDispatchPayload[0] = '\0';
		}
	} guard;

	DispatchRequestPayload payload{};
	DispatchRequestPlan plan{};
	char error[64] = "";
	if (!parseDispatchRequestPayload(pendingDispatchPayload, payload, error, sizeof(error)) ||
	    !buildDispatchRequestPlan(payload, plan, error, sizeof(error))) {
#ifdef DEBUG_OVER_SERIAL
		Serial.print(F("dq:proc:bad:"));
		Serial.println(error);
#endif
		setDispatchRequestStatus(error);
		return;
	}
	if (schedulerPassCache.dispatchQueuedDuringSnapshot) {
		schedulerPassCache.dispatchWaitDueToSnapshotMs =
			(dispatchRequestQueuedMs != 0) ? (millis() - dispatchRequestQueuedMs) : 0;
		dispatchWaitDueToSnapshotMs = schedulerPassCache.dispatchWaitDueToSnapshotMs;
		schedulerPassCache.dispatchQueuedDuringSnapshot = false;
	}

	modbusRequestAndResponse *response = runtimeModbusReadScratch();
	if (response == nullptr) {
		setDispatchRequestStatus("dispatch scratch unavailable");
		return;
	}
	*response = modbusRequestAndResponse{};
	modbusRequestAndResponseStatusValues result = modbusRequestAndResponseStatusValues::preProcessing;
	if (plan.stop) {
		result = (_registerHandler != nullptr)
		             ? _registerHandler->writeDispatchStop(response)
		             : modbusRequestAndResponseStatusValues::preProcessing;
	} else {
		result = (_registerHandler != nullptr)
		             ? _registerHandler->writeDispatchRegisters(static_cast<uint32_t>(plan.dispatchActivePower),
		                                                        plan.dispatchMode,
		                                                        plan.dispatchSocRaw,
		                                                        plan.dispatchTimeRaw,
		                                                        response)
		             : modbusRequestAndResponseStatusValues::preProcessing;
	}
	noteRuntimePhaseObservation(RuntimeDiagPhase::DispatchWrite);
	dispatchLastRunMs = millis();
	if (result != modbusRequestAndResponseStatusValues::writeDataRegisterSuccess) {
		recordRs485Error(result);
#ifdef DEBUG_OVER_SERIAL
		Serial.println(F("dq:proc:write-fail"));
#endif
		setDispatchRequestStatus("modbus write failed");
		return;
	}
#ifdef DEBUG_OVER_SERIAL
	Serial.println(F("dq:proc:write-ok"));
#endif
	invalidateDispatchBlockSnapshotCache();

	atomicDispatchState = AtomicDispatchRuntimeState{};
	atomicDispatchState.inFlight = true;
	atomicDispatchState.payload = payload;
	atomicDispatchState.plan = plan;
	atomicDispatchState.queuedMs = dispatchRequestQueuedMs;
	atomicDispatchState.readbackStartedMs = millis();
	atomicDispatchState.nextReadbackAtMs = atomicDispatchState.readbackStartedMs;
	atomicDispatchState.readbackAttempts = 0;
	timedDispatchState.bootStopPending = false;
	timedDispatchState.awaitingStopAck = false;
	timedDispatchState.restartAfterStop = false;
	dispatchLastSkipReason[0] = '\0';
}

static void
serviceAtomicDispatchRequest(void)
{
	if (!atomicDispatchState.inFlight) {
		return;
	}
	const uint32_t nowMs = millis();
	if (static_cast<int32_t>(nowMs - atomicDispatchState.nextReadbackAtMs) < 0) {
		return;
	}
#ifdef DEBUG_OVER_SERIAL
	Serial.println(F("dq:rb:tick"));
#endif

	char error[64] = "";
	DispatchRegisterReadback readback{};
	atomicDispatchState.readbackAttempts++;
	if (!readDispatchRegisterReadback(atomicDispatchState.plan, readback, error, sizeof(error))) {
		noteRuntimePhaseObservation(RuntimeDiagPhase::DispatchReadback);
#ifdef DEBUG_OVER_SERIAL
		Serial.print(F("dq:rb:read-fail:"));
		Serial.println(error);
#endif
		const uint32_t retryDecisionMs = millis();
		if ((retryDecisionMs - atomicDispatchState.readbackStartedMs) >= kDispatchReadbackTimeoutMs) {
			setDispatchRequestStatus(error[0] != '\0' ? error : "readback timeout");
			atomicDispatchState = AtomicDispatchRuntimeState{};
		} else {
			atomicDispatchState.nextReadbackAtMs = retryDecisionMs + kDispatchReadbackRetryMs;
		}
		return;
	}
	noteRuntimePhaseObservation(RuntimeDiagPhase::DispatchReadback);

	if (!dispatchRequestReadbackMatches(atomicDispatchState.plan, readback, error, sizeof(error))) {
#ifdef DEBUG_OVER_SERIAL
		Serial.print(F("dq:rb:mismatch:"));
		Serial.println(error);
#endif
		const uint32_t retryDecisionMs = millis();
		if ((retryDecisionMs - atomicDispatchState.readbackStartedMs) >= kDispatchReadbackTimeoutMs) {
			setDispatchRequestStatus(error);
			atomicDispatchState = AtomicDispatchRuntimeState{};
		} else {
			atomicDispatchState.nextReadbackAtMs = retryDecisionMs + kDispatchReadbackRetryMs;
		}
		return;
	}

	timedDispatchState.configuredDurationSeconds =
		dispatchAcceptedDurationSeconds(atomicDispatchState.payload, atomicDispatchState.plan);
	if (atomicDispatchState.plan.stop) {
		dispatchMarkStopped(timedDispatchState, true);
	} else if (dispatchDurationIsTimed(timedDispatchState.configuredDurationSeconds)) {
		dispatchNoteRequestedGeneration(timedDispatchState);
		dispatchMarkAccepted(timedDispatchState,
		                     timedDispatchState.requestedGeneration,
		                     nowMs,
		                     timedDispatchState.configuredDurationSeconds);
	} else {
		dispatchMarkStopped(timedDispatchState, false);
	}
	// A disconnected MQTT session must not orphan an accepted timed dispatch.
	// Readback-confirmed lifecycle state is updated first; mirror publish is
	// best-effort and can replay later once MQTT recovers.
	dispatchMirrorPublishReadback = readback;
	dispatchMirrorPublishReadbackValid = true;
	dispatchMirrorPublishPending = true;
	dispatchMirrorPublishEarliestLoop = loopSequence + 1;
	refreshEssSnapshotAfterDispatch(true);

#ifdef DEBUG_OVER_SERIAL
	Serial.println(F("dq:rb:ok"));
#endif
	setDispatchRequestStatus("ok");
	atomicDispatchState = AtomicDispatchRuntimeState{};
}

static bool
computeDispatchCommand(uint16_t &essDispatchMode,
                       int32_t &essDispatchActivePower,
                       uint16_t &essDispatchSoc,
                       bool &checkActivePower)
{
	if (!opData.a2mReadyToUseSocTarget || !opData.a2mReadyToUsePwrCharge ||
	    !opData.a2mReadyToUsePwrDischarge || !opData.a2mReadyToUsePwrPush) {
		return false;
	}

	const uint16_t essBatterySocPct = opData.essBatterySoc * BATTERY_SOC_MULTIPLIER;
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
	case opMode::opModeNormal:
		return false;
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
		return false; // Shouldn't happen!  opMode is corrupt.
	}
	return true;
}

static void
dispatchService(void)
{
#ifndef DEBUG_NO_RS485
	if (_registerHandler == nullptr) {
		return;
	}
	if (!mqttSubsystemEnabled()) {
		return;
	}

	const uint32_t nowMs = millis();
	const bool timedEnabled = dispatchDurationIsTimed(timedDispatchState.configuredDurationSeconds);
	const bool rs485Live = (rs485ConnectState == Rs485ConnectState::Connected) && inverterReady;
	const bool pendingGeneration = timedEnabled && dispatchHasPendingGeneration(timedDispatchState);
	const bool fastEvalCadence =
		dispatchUseFastEvalCadence(timedDispatchState, timedEnabled, rs485Live);
	const uint32_t evalIntervalMs = fastEvalCadence ? kDispatchHandshakeIntervalMs :
	                                                  (pollIntervalSeconds * 1000UL);
	const bool dueEval = dispatchEvalDue(
		timedDispatchState.lastEvalMs, nowMs, evalIntervalMs, false);
	const bool dueCountdown = timedEnabled &&
	                          (timedDispatchState.activeGeneration != 0) &&
	                          dispatchCountdownPublishDue(timedDispatchState.lastCountdownPublishMs, nowMs);
	if (!dueEval && !dueCountdown) {
		return;
	}
#ifdef DEBUG_OVER_SERIAL
	if (pollIntervalSeconds <= 1) {
		Serial.printf("dispatchService due: eval=%u countdown=%u enabled=%u pending=%u active=%lu skip=%s free=%u max=%u frag=%u\r\n",
		              dueEval ? 1U : 0U,
		              dueCountdown ? 1U : 0U,
		              timedEnabled ? 1U : 0U,
		              pendingGeneration ? 1U : 0U,
		              static_cast<unsigned long>(timedDispatchState.activeGeneration),
		              dispatchLastSkipReason,
		              ESP.getFreeHeap(),
		              ESP.getMaxFreeBlockSize(),
		              ESP.getHeapFragmentation());
	}
#endif
	if (dueEval) {
		timedDispatchState.lastEvalMs = nowMs;
		if (!refreshEssSnapshot()) {
			strlcpy(dispatchLastSkipReason, "ess_snapshot_failed", sizeof(dispatchLastSkipReason));
			return;
		}
		timedDispatchState.evalPending = false;
	}

	auto writeStop = [&](const char *reason, bool restartAfterStop) -> bool {
		modbusRequestAndResponse *response = runtimeModbusReadScratch();
		if (response == nullptr) {
			return false;
		}
		*response = modbusRequestAndResponse{};
		const modbusRequestAndResponseStatusValues result = _registerHandler->writeDispatchStop(response);
		dispatchLastRunMs = millis();
		if (result != modbusRequestAndResponseStatusValues::writeDataRegisterSuccess) {
			recordRs485Error(result);
			return false;
		}
		timedDispatchState.awaitingStopAck = true;
		timedDispatchState.restartAfterStop = restartAfterStop;
		refreshEssSnapshotAfterDispatch(false);
		strlcpy(dispatchLastSkipReason, reason, sizeof(dispatchLastSkipReason));
#ifdef DEBUG_OVER_SERIAL
		snprintf(_debugOutput, sizeof(_debugOutput), "dispatch stop sent: reason=%s restart=%u",
		         reason, restartAfterStop ? 1U : 0U);
		Serial.println(_debugOutput);
#endif
		return true;
	};

	auto writeStart = [&](uint16_t mode, int32_t activePower, uint16_t soc, uint32_t rawTime) -> bool {
		modbusRequestAndResponse *response = runtimeModbusReadScratch();
		if (response == nullptr) {
			return false;
		}
		*response = modbusRequestAndResponse{};
		const modbusRequestAndResponseStatusValues result =
			_registerHandler->writeDispatchRegisters(activePower, mode, soc, rawTime, response);
		dispatchLastRunMs = millis();
		if (result != modbusRequestAndResponseStatusValues::writeDataRegisterSuccess) {
			recordRs485Error(result);
			return false;
		}
		strlcpy(dispatchLastSkipReason, "awaiting_start_ack", sizeof(dispatchLastSkipReason));
		refreshEssSnapshotAfterDispatch(false);
		publishDispatchStateEntity(mqttEntityId::entityDispatchStart);
		publishDispatchStateEntity(mqttEntityId::entityDispatchTime);
#ifdef DEBUG_OVER_SERIAL
		snprintf(_debugOutput, sizeof(_debugOutput),
		         "dispatch start sent: mode=%u power=%ld soc=%u time=%lu",
		         static_cast<unsigned>(mode),
		         static_cast<long>(activePower),
		         static_cast<unsigned>(soc),
		         static_cast<unsigned long>(rawTime));
		Serial.println(_debugOutput);
#endif
		return true;
	};

	if (timedDispatchState.bootStopPending) {
		if (opData.essDispatchStart == DISPATCH_START_START) {
			if (!timedDispatchState.awaitingStopAck) {
				writeStop("boot_stop", false);
			} else if (dueEval) {
				writeStop("waiting_stop_ack", false);
			}
			return;
		}
		// If we already issued boot_stop, preserve awaitingStopAck so the acknowledgement
		// branch below can publish the stopped state once the inverter reflects Stop.
		if (!timedDispatchState.awaitingStopAck) {
			timedDispatchState.bootStopPending = false;
			timedDispatchState.awaitingStopAck = false;
		}
	}

	if (timedDispatchState.awaitingStopAck) {
		if (opData.essDispatchStart != DISPATCH_START_START) {
			const bool completed = (timedDispatchState.activeGeneration != 0);
			const bool restartAfterStop = timedDispatchState.restartAfterStop;
			dispatchMarkStopped(timedDispatchState, completed);
			timedDispatchState.bootStopPending = false;
			publishDispatchAuxiliaryStates(true);
			if (!restartAfterStop) {
				return;
			}
		} else if (dueEval) {
			writeStop("waiting_stop_ack", timedDispatchState.restartAfterStop);
			return;
		}
	}

	if (!legacyDispatchControlSurfaceEnabled()) {
		if (timedDispatchState.activeGeneration != 0) {
			const uint32_t remainingSeconds = dispatchRemainingSeconds(timedDispatchState.acceptedAtMs,
			                                                           timedDispatchState.acceptedDurationSeconds,
			                                                           nowMs);
			if (dueCountdown) {
				timedDispatchState.lastCountdownPublishMs = nowMs;
				publishDispatchAuxiliaryStates(false);
			}
			if (dueEval && opData.essDispatchStart != DISPATCH_START_START) {
				dispatchMarkStopped(timedDispatchState, true);
				publishDispatchAuxiliaryStates(true);
				dispatchLastSkipReason[0] = '\0';
				return;
			}
			if (remainingSeconds == 0) {
				strlcpy(dispatchLastSkipReason, "timed_complete", sizeof(dispatchLastSkipReason));
				if (opData.essDispatchStart == DISPATCH_START_START) {
					writeStop("timed_complete", false);
					return;
				}
				dispatchMarkStopped(timedDispatchState, true);
				publishDispatchAuxiliaryStates(true);
				dispatchLastSkipReason[0] = '\0';
				return;
			}
		}
		dispatchLastSkipReason[0] = '\0';
		return;
	}

	if (!opData.a2mReadyToUseOpMode) {
		strlcpy(dispatchLastSkipReason, "op_mode_not_ready", sizeof(dispatchLastSkipReason));
		return;
	}

	if (opData.a2mOpMode == opMode::opModeNormal) {
		if (opData.essDispatchStart == DISPATCH_START_START) {
			writeStop("normal_mode", false);
			return;
		}
		bool publishedStoppedState = false;
		if (dispatchLastSkipReason[0] != '\0') {
			// A stop can complete before inverter-scoped MQTT topics are publishable after boot.
			// Replay the final stopped state once identity is ready so subscribers do not miss it.
			publishedStoppedState = publishDispatchAuxiliaryStatesIfReady(true);
		}
		dispatchMarkStopped(timedDispatchState, false);
		if (dispatchLastSkipReason[0] == '\0' || publishedStoppedState) {
			dispatchLastSkipReason[0] = '\0';
		}
		return;
	}

	uint16_t desiredMode = DISPATCH_MODE_NORMAL_MODE;
	int32_t desiredActivePower = DISPATCH_ACTIVE_POWER_OFFSET;
	uint16_t desiredSoc = 0;
	bool checkActivePower = true;
	if (!computeDispatchCommand(desiredMode, desiredActivePower, desiredSoc, checkActivePower)) {
		strlcpy(dispatchLastSkipReason, "control_values_not_ready", sizeof(dispatchLastSkipReason));
		return;
	}

	const bool snapshotActive = (opData.essDispatchStart == DISPATCH_START_START);
	const bool snapshotMatchesMaintenance =
		snapshotActive &&
		opData.essDispatchMode == desiredMode &&
		(!checkActivePower || opData.essDispatchActivePower == desiredActivePower) &&
		opData.essDispatchSoc == desiredSoc;
	const uint32_t desiredRawTime = dispatchRawTimeForDuration(timedDispatchState.configuredDurationSeconds);
	const bool snapshotMatchesAcceptance = snapshotMatchesMaintenance &&
	                                      opData.essDispatchTime == desiredRawTime;

	if (!timedEnabled) {
		bool publishedStoppedState = false;
		if (!snapshotActive || !snapshotMatchesMaintenance ||
		    opData.essDispatchTime != kDispatchRawForeverSeconds) {
#ifdef DEBUG_OPS
			opCounter++;
#endif
			writeStart(desiredMode, desiredActivePower, desiredSoc, desiredRawTime);
			return;
		}
		if (dispatchLastSkipReason[0] != '\0') {
			publishedStoppedState = publishDispatchAuxiliaryStatesIfReady(true);
		}
		dispatchMarkStopped(timedDispatchState, false);
		if (dispatchLastSkipReason[0] == '\0' || publishedStoppedState) {
			dispatchLastSkipReason[0] = '\0';
		}
		return;
	}

	if (timedDispatchState.activeGeneration != 0) {
		const uint32_t remainingSeconds = dispatchRemainingSeconds(timedDispatchState.acceptedAtMs,
		                                                           timedDispatchState.acceptedDurationSeconds,
		                                                           nowMs);
		if (dueCountdown) {
			timedDispatchState.lastCountdownPublishMs = nowMs;
			publishDispatchAuxiliaryStates(false);
		}
		if (timedDispatchState.requestedGeneration > timedDispatchState.activeGeneration) {
			if (snapshotActive) {
				writeStop("restart_wait_stop", true);
				return;
			}
			dispatchMarkStopped(timedDispatchState, true);
			publishDispatchAuxiliaryStates(true);
		} else if (remainingSeconds == 0) {
			if (snapshotActive) {
				publishDispatchAuxiliaryStates(false);
				writeStop("timed_complete", false);
				return;
			}
			dispatchMarkStopped(timedDispatchState, true);
			publishDispatchAuxiliaryStates(true);
			strlcpy(dispatchLastSkipReason, "timed_complete", sizeof(dispatchLastSkipReason));
			return;
		} else if (!snapshotMatchesMaintenance) {
			if (!dueEval) {
				strlcpy(dispatchLastSkipReason, "waiting_maintenance_eval", sizeof(dispatchLastSkipReason));
				return;
			}
#ifdef DEBUG_OPS
			opCounter++;
#endif
			writeStart(desiredMode, desiredActivePower, desiredSoc, desiredRawTime);
			return;
		}
	}

	if (dispatchHasPendingGeneration(timedDispatchState) && timedDispatchState.activeGeneration == 0 &&
	    !timedDispatchState.awaitingStopAck) {
		if (snapshotMatchesAcceptance) {
			dispatchMarkAccepted(timedDispatchState,
			                     timedDispatchState.requestedGeneration,
			                     nowMs,
			                     timedDispatchState.configuredDurationSeconds);
			dispatchLastSkipReason[0] = '\0';
			publishDispatchAuxiliaryStates(true);
#ifdef DEBUG_OVER_SERIAL
			snprintf(_debugOutput, sizeof(_debugOutput), "dispatch accepted: gen=%lu time=%lu",
			         static_cast<unsigned long>(timedDispatchState.activeGeneration),
			         static_cast<unsigned long>(desiredRawTime));
			Serial.println(_debugOutput);
#endif
			return;
		}
		if (snapshotActive) {
			writeStop("restart_wait_stop", true);
			return;
		}
#ifdef DEBUG_OPS
		opCounter++;
#endif
		writeStart(desiredMode, desiredActivePower, desiredSoc, desiredRawTime);
		return;
	}

	dispatchLastSkipReason[0] = '\0';
#else
	(void)timedDispatchState;
#endif // ! DEBUG_NO_RS485
}

void
getA2mOpDataFromEss(void)
{
#ifdef DEBUG_NO_RS485
	opData.a2mOpMode = opMode::opModeNormal;
	opData.a2mSocTarget = SOC_TARGET_MAX;
	opData.a2mPwrCharge = INVERTER_POWER_MAX;
	opData.a2mPwrDischarge = INVERTER_POWER_MAX;
#else // DEBUG_NO_RS485
	// Defaults keep control logic in a safe, bounded state if ESS reads fail repeatedly.
	opData.a2mOpMode = opMode::opModeNormal;
	opData.a2mSocTarget = SOC_TARGET_MAX;
	opData.a2mPwrCharge = INVERTER_POWER_MAX;
	opData.a2mPwrDischarge = INVERTER_POWER_MAX;

	const uint8_t kMaxReadAttempts = 4;
	DispatchBlockSnapshot dispatchSnapshot{};
	bool dispatchActive = false;
	bool dispatchReadOk = false;
	for (uint8_t attempt = 0; attempt < kMaxReadAttempts; attempt++) {
		modbusRequestAndResponseStatusValues dispatchResult = modbusRequestAndResponseStatusValues::preProcessing;
		if (fetchDispatchBlockSnapshot(dispatchSnapshot, &dispatchResult)) {
			dispatchReadOk = true;
			break;
		}
		recordRs485Error(dispatchResult);
#ifdef DEBUG_OVER_SERIAL
		if (attempt == 0 || (attempt + 1) == kMaxReadAttempts) {
			snprintf(_debugOutput, sizeof(_debugOutput),
			         "getA2mOpDataFromEss: dispatch_block read failed (%u/%u)",
			         static_cast<unsigned>(attempt + 1),
			         static_cast<unsigned>(kMaxReadAttempts));
			Serial.println(_debugOutput);
		}
#endif // DEBUG_OVER_SERIAL
		diagDelay(10);
	}

	if (dispatchReadOk) {
		opData.essDispatchStart = dispatchSnapshot.dispatchStart;
		opData.essDispatchMode = dispatchSnapshot.dispatchMode;
		opData.essDispatchTime = dispatchSnapshot.dispatchTimeRaw;
		dispatchActive = (dispatchSnapshot.dispatchStart == DISPATCH_START_START);
		if (!dispatchActive) {
			opData.a2mOpMode = opMode::opModeNormal;
		} else {
			switch (dispatchSnapshot.dispatchMode) {
			case DISPATCH_MODE_BATTERY_ONLY_CHARGED_VIA_PV:
				opData.a2mOpMode = opMode::opModePvCharge;
				break;
			case DISPATCH_MODE_STATE_OF_CHARGE_CONTROL:
				opData.a2mOpMode = opMode::opModeTarget;
				break;
			case DISPATCH_MODE_LOAD_FOLLOWING:
				opData.a2mOpMode = opMode::opModeLoadFollow;
				break;
			case DISPATCH_MODE_NORMAL_MODE:
				opData.a2mOpMode = opMode::opModeNormal;
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
				snprintf(_debugOutput,
				         sizeof(_debugOutput),
				         "getA2mOpDataFromEss: Unhandled Dispatch Mode: %u",
				         static_cast<unsigned>(dispatchSnapshot.dispatchMode));
				Serial.println(_debugOutput);
#endif
				opData.a2mOpMode = opMode::opModeLoadFollow;
				break;
			}
		}

		opData.a2mSocTarget = dispatchSnapshot.dispatchSocRaw * DISPATCH_SOC_MULTIPLIER;
		if (dispatchSnapshot.dispatchActivePower > DISPATCH_ACTIVE_POWER_OFFSET) {
			opData.a2mPwrCharge = INVERTER_POWER_MAX;
			opData.a2mPwrDischarge = dispatchSnapshot.dispatchActivePower - DISPATCH_ACTIVE_POWER_OFFSET;
		} else if (dispatchSnapshot.dispatchActivePower < DISPATCH_ACTIVE_POWER_OFFSET) {
			opData.a2mPwrCharge = DISPATCH_ACTIVE_POWER_OFFSET - dispatchSnapshot.dispatchActivePower;
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
	const bool mqttPulseActive = static_cast<long>(mqttActivityPulseUntilMs - millis()) > 0;
	const StatusLedRender render = computeStatusLedRender(WiFi.status() == WL_CONNECTED,
	                                                     _mqtt.connected(),
	                                                     opData.essRs485Connected,
	                                                     mqttPulseActive);
#ifdef MP_ESPUNO_ESP32C6
	setStatusLedColor(render.red, render.green, render.blue);
#else
	setStatusLed(render.monochromeOn);
#endif
}
