// Purpose: Provide a flash-resident descriptor table for MQTT/HA entities and a
// lazily-allocated runtime table for per-boot mutable fields (polling config).
// Key dependency: `Definitions.h` for the entity enums and descriptor struct.
#include "../include/MqttEntities.h"

namespace {

#define MQTT_ENTITY_DESC(id, name, freq, subscribe, retain, haClass) \
	{ id, name, freq, subscribe, retain, haClass }

static const mqttState kMqttEntities[] = {
	// Entity,                                "Name",                 Update Frequency,        Subscribe, Retain, HA Class
#ifdef DEBUG_FREEMEM
	MQTT_ENTITY_DESC(mqttEntityId::entityFreemem,            "A2M_freemem",          mqttUpdateFreq::freqOneMin,  false, false, homeAssistantClass::haClassInfo),
#endif
#ifdef DEBUG_CALLBACKS
	MQTT_ENTITY_DESC(mqttEntityId::entityCallbacks,          "A2M_Callbacks",        mqttUpdateFreq::freqTenSec,  false, false, homeAssistantClass::haClassInfo),
#endif // DEBUG_CALLBACKS
#ifdef DEBUG_RS485
	MQTT_ENTITY_DESC(mqttEntityId::entityRs485Errors,        "A2M_RS485_Errors",     mqttUpdateFreq::freqTenSec,  false, false, homeAssistantClass::haClassInfo),
#endif // DEBUG_RS485
#ifdef A2M_DEBUG_WIFI
	MQTT_ENTITY_DESC(mqttEntityId::entityRSSI,               "A2M_RSSI",             mqttUpdateFreq::freqOneMin,  false, false, homeAssistantClass::haClassInfo),
	MQTT_ENTITY_DESC(mqttEntityId::entityBSSID,              "A2M_BSSID",            mqttUpdateFreq::freqOneMin,  false, false, homeAssistantClass::haClassInfo),
	MQTT_ENTITY_DESC(mqttEntityId::entityTxPower,            "A2M_TX_Power",         mqttUpdateFreq::freqOneMin,  false, false, homeAssistantClass::haClassInfo),
	MQTT_ENTITY_DESC(mqttEntityId::entityWifiRecon,          "A2M_reconnects",       mqttUpdateFreq::freqOneMin,  false, false, homeAssistantClass::haClassInfo),
#endif // A2M_DEBUG_WIFI
	MQTT_ENTITY_DESC(mqttEntityId::entityRs485Avail,         "RS485_Connected",      mqttUpdateFreq::freqNever,   false, true,  homeAssistantClass::haClassBinaryProblem),
	MQTT_ENTITY_DESC(mqttEntityId::entityA2MUptime,          "A2M_uptime",           mqttUpdateFreq::freqTenSec,  false, false, homeAssistantClass::haClassDuration),
	MQTT_ENTITY_DESC(mqttEntityId::entityA2MVersion,         "A2M_version",          mqttUpdateFreq::freqOneDay,  false, true,  homeAssistantClass::haClassInfo),
	MQTT_ENTITY_DESC(mqttEntityId::entityInverterVersion,    "Inverter_version",     mqttUpdateFreq::freqOneDay,  false, true,  homeAssistantClass::haClassInfo),
	MQTT_ENTITY_DESC(mqttEntityId::entityInverterSn,         "Inverter_SN",          mqttUpdateFreq::freqOneDay,  false, true,  homeAssistantClass::haClassInfo),
	MQTT_ENTITY_DESC(mqttEntityId::entityEmsVersion,         "EMS_version",          mqttUpdateFreq::freqOneDay,  false, true,  homeAssistantClass::haClassInfo),
	MQTT_ENTITY_DESC(mqttEntityId::entityEmsSn,              "EMS_SN",               mqttUpdateFreq::freqOneDay,  false, true,  homeAssistantClass::haClassInfo),
	MQTT_ENTITY_DESC(mqttEntityId::entityBatSoc,             "State_of_Charge",      mqttUpdateFreq::freqOneMin,  false, true,  homeAssistantClass::haClassBattery),
	MQTT_ENTITY_DESC(mqttEntityId::entityBatPwr,             "ESS_Power",            mqttUpdateFreq::freqTenSec,  false, true,  homeAssistantClass::haClassPower),
	MQTT_ENTITY_DESC(mqttEntityId::entityBatEnergyCharge,    "ESS_Energy_Charge",    mqttUpdateFreq::freqOneMin,  false, true,  homeAssistantClass::haClassEnergy),
	MQTT_ENTITY_DESC(mqttEntityId::entityBatEnergyDischarge, "ESS_Energy_Discharge", mqttUpdateFreq::freqOneMin,  false, true,  homeAssistantClass::haClassEnergy),
	MQTT_ENTITY_DESC(mqttEntityId::entityGridAvail,          "Grid_Connected",       mqttUpdateFreq::freqNever,   false, true,  homeAssistantClass::haClassBinaryProblem),
	MQTT_ENTITY_DESC(mqttEntityId::entityGridPwr,            "Grid_Power",           mqttUpdateFreq::freqTenSec,  false, true,  homeAssistantClass::haClassPower),
	MQTT_ENTITY_DESC(mqttEntityId::entityGridEnergyTo,       "Grid_Energy_To",       mqttUpdateFreq::freqOneMin,  false, true,  homeAssistantClass::haClassEnergy),
	MQTT_ENTITY_DESC(mqttEntityId::entityGridEnergyFrom,     "Grid_Energy_From",     mqttUpdateFreq::freqOneMin,  false, true,  homeAssistantClass::haClassEnergy),
	MQTT_ENTITY_DESC(mqttEntityId::entityPvPwr,              "Solar_Power",          mqttUpdateFreq::freqTenSec,  false, true,  homeAssistantClass::haClassPower),
	MQTT_ENTITY_DESC(mqttEntityId::entityPvEnergy,           "Solar_Energy",         mqttUpdateFreq::freqOneMin,  false, true,  homeAssistantClass::haClassEnergy),
	MQTT_ENTITY_DESC(mqttEntityId::entityFrequency,          "Frequency",            mqttUpdateFreq::freqOneMin,  false, true,  homeAssistantClass::haClassFrequency),
	MQTT_ENTITY_DESC(mqttEntityId::entityOpMode,             "Op_Mode",              mqttUpdateFreq::freqOneMin,  true,  true,  homeAssistantClass::haClassSelect),
	MQTT_ENTITY_DESC(mqttEntityId::entitySocTarget,          "SOC_Target",           mqttUpdateFreq::freqOneMin,  true,  true,  homeAssistantClass::haClassBox),
	MQTT_ENTITY_DESC(mqttEntityId::entityChargePwr,          "Charge_Power",         mqttUpdateFreq::freqOneMin,  true,  true,  homeAssistantClass::haClassBox),
	MQTT_ENTITY_DESC(mqttEntityId::entityDischargePwr,       "Discharge_Power",      mqttUpdateFreq::freqOneMin,  true,  true,  homeAssistantClass::haClassBox),
	MQTT_ENTITY_DESC(mqttEntityId::entityPushPwr,            "Push_Power",           mqttUpdateFreq::freqOneMin,  true,  true,  homeAssistantClass::haClassBox),
	MQTT_ENTITY_DESC(mqttEntityId::entityBatCap,             "Battery_Capacity",     mqttUpdateFreq::freqOneDay,  false, true,  homeAssistantClass::haClassInfo),
	MQTT_ENTITY_DESC(mqttEntityId::entityBatTemp,            "Battery_Temp",         mqttUpdateFreq::freqFiveMin, false, true,  homeAssistantClass::haClassTemp),
	MQTT_ENTITY_DESC(mqttEntityId::entityInverterTemp,       "Inverter_Temp",        mqttUpdateFreq::freqFiveMin, false, true,  homeAssistantClass::haClassTemp),
	MQTT_ENTITY_DESC(mqttEntityId::entityBatFaults,          "Battery_Faults",       mqttUpdateFreq::freqOneMin,  false, true,  homeAssistantClass::haClassBinaryProblem),
	MQTT_ENTITY_DESC(mqttEntityId::entityBatWarnings,        "Battery_Warnings",     mqttUpdateFreq::freqOneMin,  false, true,  homeAssistantClass::haClassBinaryProblem),
	MQTT_ENTITY_DESC(mqttEntityId::entityInverterFaults,     "Inverter_Faults",      mqttUpdateFreq::freqOneMin,  false, true,  homeAssistantClass::haClassBinaryProblem),
	MQTT_ENTITY_DESC(mqttEntityId::entityInverterWarnings,   "Inverter_Warnings",    mqttUpdateFreq::freqOneMin,  false, true,  homeAssistantClass::haClassBinaryProblem),
	MQTT_ENTITY_DESC(mqttEntityId::entitySystemFaults,       "System_Faults",        mqttUpdateFreq::freqOneMin,  false, true,  homeAssistantClass::haClassBinaryProblem),
	MQTT_ENTITY_DESC(mqttEntityId::entityInverterMode,       "Inverter_Mode",        mqttUpdateFreq::freqTenSec,  false, true,  homeAssistantClass::haClassInfo),
	MQTT_ENTITY_DESC(mqttEntityId::entityGridReg,            "Grid_Regulation",      mqttUpdateFreq::freqOneDay,  false, false, homeAssistantClass::haClassInfo),
	MQTT_ENTITY_DESC(mqttEntityId::entityRegNum,             "Register_Number",      mqttUpdateFreq::freqOneMin,  true,  false, homeAssistantClass::haClassBox),
	MQTT_ENTITY_DESC(mqttEntityId::entityRegValue,           "Register_Value",       mqttUpdateFreq::freqOneMin,  false, false, homeAssistantClass::haClassInfo),
};

constexpr size_t kMqttEntityCount = sizeof(kMqttEntities) / sizeof(kMqttEntities[0]);

static MqttEntityRuntime *g_runtime = nullptr;

} // namespace

const mqttState *
mqttEntitiesDesc()
{
	return kMqttEntities;
}

size_t
mqttEntitiesCount()
{
	return kMqttEntityCount;
}

bool
mqttEntitiesRtAvailable()
{
	return g_runtime != nullptr;
}

MqttEntityRuntime *
mqttEntitiesRt()
{
	return g_runtime;
}

void
initMqttEntitiesRtIfNeeded(bool mqttEnabled)
{
	if (!mqttEnabled || g_runtime != nullptr) {
		return;
	}
	g_runtime = new MqttEntityRuntime[kMqttEntityCount];
	for (size_t i = 0; i < kMqttEntityCount; ++i) {
		g_runtime[i].defaultFreq = kMqttEntities[i].updateFreq;
		g_runtime[i].effectiveFreq = kMqttEntities[i].updateFreq;
	}
}
