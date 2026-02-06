// Purpose: Provide a flash-resident descriptor table for MQTT/HA entities and a
// lazily-allocated runtime table for per-boot mutable fields (polling config).
// Key dependency: `Definitions.h` for the entity enums and descriptor struct.
#include "../include/MqttEntities.h"

namespace {

#define MQTT_ENTITY_DESC(id, name, freq, subscribe, retain, haClass) \
	{ id, name, freq, subscribe, retain, haClass }

// Build the descriptor table with the same conditional compilation as before.
// Entities that are not compiled in are also absent from the metadata table.
#define MQTT_ENTITY_ROW(id, name, freq, subscribe, retain, haClass, needsEss) MQTT_ENTITY_DESC(id, name, freq, subscribe, retain, haClass),

static const mqttState kMqttEntities[] = {
#ifdef DEBUG_FREEMEM
	MQTT_ENTITY_ROW(mqttEntityId::entityFreemem, "A2M_freemem", mqttUpdateFreq::freqOneMin, false, false, homeAssistantClass::haClassInfo, false)
#endif
#ifdef DEBUG_CALLBACKS
	MQTT_ENTITY_ROW(mqttEntityId::entityCallbacks, "A2M_Callbacks", mqttUpdateFreq::freqTenSec, false, false, homeAssistantClass::haClassInfo, false)
#endif
#ifdef DEBUG_RS485
	MQTT_ENTITY_ROW(mqttEntityId::entityRs485Errors, "A2M_RS485_Errors", mqttUpdateFreq::freqTenSec, false, false, homeAssistantClass::haClassInfo, false)
#endif
#ifdef A2M_DEBUG_WIFI
	MQTT_ENTITY_ROW(mqttEntityId::entityRSSI, "A2M_RSSI", mqttUpdateFreq::freqOneMin, false, false, homeAssistantClass::haClassInfo, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityBSSID, "A2M_BSSID", mqttUpdateFreq::freqOneMin, false, false, homeAssistantClass::haClassInfo, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityTxPower, "A2M_TX_Power", mqttUpdateFreq::freqOneMin, false, false, homeAssistantClass::haClassInfo, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityWifiRecon, "A2M_reconnects", mqttUpdateFreq::freqOneMin, false, false, homeAssistantClass::haClassInfo, false)
#endif
	// The remaining entities are always compiled in.
	MQTT_ENTITY_ROW(mqttEntityId::entityRs485Avail, "RS485_Connected", mqttUpdateFreq::freqNever, false, true, homeAssistantClass::haClassBinaryProblem, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityA2MUptime, "A2M_uptime", mqttUpdateFreq::freqTenSec, false, false, homeAssistantClass::haClassDuration, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityA2MVersion, "A2M_version", mqttUpdateFreq::freqOneDay, false, true, homeAssistantClass::haClassInfo, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityInverterVersion, "Inverter_version", mqttUpdateFreq::freqOneDay, false, true, homeAssistantClass::haClassInfo, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityInverterSn, "Inverter_SN", mqttUpdateFreq::freqOneDay, false, true, homeAssistantClass::haClassInfo, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityEmsVersion, "EMS_version", mqttUpdateFreq::freqOneDay, false, true, homeAssistantClass::haClassInfo, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityEmsSn, "EMS_SN", mqttUpdateFreq::freqOneDay, false, true, homeAssistantClass::haClassInfo, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityBatSoc, "State_of_Charge", mqttUpdateFreq::freqOneMin, false, true, homeAssistantClass::haClassBattery, true)
	MQTT_ENTITY_ROW(mqttEntityId::entityBatPwr, "ESS_Power", mqttUpdateFreq::freqTenSec, false, true, homeAssistantClass::haClassPower, true)
	MQTT_ENTITY_ROW(mqttEntityId::entityBatEnergyCharge, "ESS_Energy_Charge", mqttUpdateFreq::freqOneMin, false, true, homeAssistantClass::haClassEnergy, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityBatEnergyDischarge, "ESS_Energy_Discharge", mqttUpdateFreq::freqOneMin, false, true, homeAssistantClass::haClassEnergy, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityGridAvail, "Grid_Connected", mqttUpdateFreq::freqNever, false, true, homeAssistantClass::haClassBinaryProblem, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityGridPwr, "Grid_Power", mqttUpdateFreq::freqTenSec, false, true, homeAssistantClass::haClassPower, true)
	MQTT_ENTITY_ROW(mqttEntityId::entityGridEnergyTo, "Grid_Energy_To", mqttUpdateFreq::freqOneMin, false, true, homeAssistantClass::haClassEnergy, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityGridEnergyFrom, "Grid_Energy_From", mqttUpdateFreq::freqOneMin, false, true, homeAssistantClass::haClassEnergy, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityPvPwr, "Solar_Power", mqttUpdateFreq::freqTenSec, false, true, homeAssistantClass::haClassPower, true)
	MQTT_ENTITY_ROW(mqttEntityId::entityPvEnergy, "Solar_Energy", mqttUpdateFreq::freqOneMin, false, true, homeAssistantClass::haClassEnergy, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityFrequency, "Frequency", mqttUpdateFreq::freqOneMin, false, true, homeAssistantClass::haClassFrequency, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqOneMin, true, true, homeAssistantClass::haClassSelect, false)
	MQTT_ENTITY_ROW(mqttEntityId::entitySocTarget, "SOC_Target", mqttUpdateFreq::freqOneMin, true, true, homeAssistantClass::haClassBox, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityChargePwr, "Charge_Power", mqttUpdateFreq::freqOneMin, true, true, homeAssistantClass::haClassBox, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityDischargePwr, "Discharge_Power", mqttUpdateFreq::freqOneMin, true, true, homeAssistantClass::haClassBox, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityPushPwr, "Push_Power", mqttUpdateFreq::freqOneMin, true, true, homeAssistantClass::haClassBox, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityBatCap, "Battery_Capacity", mqttUpdateFreq::freqOneDay, false, true, homeAssistantClass::haClassInfo, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityBatTemp, "Battery_Temp", mqttUpdateFreq::freqFiveMin, false, true, homeAssistantClass::haClassTemp, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityInverterTemp, "Inverter_Temp", mqttUpdateFreq::freqFiveMin, false, true, homeAssistantClass::haClassTemp, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityBatFaults, "Battery_Faults", mqttUpdateFreq::freqOneMin, false, true, homeAssistantClass::haClassBinaryProblem, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityBatWarnings, "Battery_Warnings", mqttUpdateFreq::freqOneMin, false, true, homeAssistantClass::haClassBinaryProblem, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityInverterFaults, "Inverter_Faults", mqttUpdateFreq::freqOneMin, false, true, homeAssistantClass::haClassBinaryProblem, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityInverterWarnings, "Inverter_Warnings", mqttUpdateFreq::freqOneMin, false, true, homeAssistantClass::haClassBinaryProblem, false)
	MQTT_ENTITY_ROW(mqttEntityId::entitySystemFaults, "System_Faults", mqttUpdateFreq::freqOneMin, false, true, homeAssistantClass::haClassBinaryProblem, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityInverterMode, "Inverter_Mode", mqttUpdateFreq::freqTenSec, false, true, homeAssistantClass::haClassInfo, true)
	MQTT_ENTITY_ROW(mqttEntityId::entityGridReg, "Grid_Regulation", mqttUpdateFreq::freqOneDay, false, false, homeAssistantClass::haClassInfo, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityRegNum, "Register_Number", mqttUpdateFreq::freqOneMin, true, false, homeAssistantClass::haClassBox, false)
	MQTT_ENTITY_ROW(mqttEntityId::entityRegValue, "Register_Value", mqttUpdateFreq::freqOneMin, false, false, homeAssistantClass::haClassInfo, false)
};

#undef MQTT_ENTITY_ROW

static const bool kNeedsEssSnapshot[] = {
#ifdef DEBUG_FREEMEM
	false,
#endif
#ifdef DEBUG_CALLBACKS
	false,
#endif
#ifdef DEBUG_RS485
	false,
#endif
#ifdef A2M_DEBUG_WIFI
	false,
	false,
	false,
	false,
#endif
	false, // RS485_Connected
	false, // A2M_uptime
	false, // A2M_version
	false, // Inverter_version
	false, // Inverter_SN
	false, // EMS_version
	false, // EMS_SN
	true,  // State_of_Charge
	true,  // ESS_Power
	false, // ESS_Energy_Charge
	false, // ESS_Energy_Discharge
	false, // Grid_Connected
	true,  // Grid_Power
	false, // Grid_Energy_To
	false, // Grid_Energy_From
	true,  // Solar_Power
	false, // Solar_Energy
	false, // Frequency
	false, // Op_Mode
	false, // SOC_Target
	false, // Charge_Power
	false, // Discharge_Power
	false, // Push_Power
	false, // Battery_Capacity
	false, // Battery_Temp
	false, // Inverter_Temp
	false, // Battery_Faults
	false, // Battery_Warnings
	false, // Inverter_Faults
	false, // Inverter_Warnings
	false, // System_Faults
	true,  // Inverter_Mode
	false, // Grid_Regulation
	false, // Register_Number
	false, // Register_Value
};

constexpr size_t kMqttEntityCount = sizeof(kMqttEntities) / sizeof(kMqttEntities[0]);
static_assert(sizeof(kNeedsEssSnapshot) / sizeof(kNeedsEssSnapshot[0]) == kMqttEntityCount,
              "kNeedsEssSnapshot must match kMqttEntities length");

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
mqttEntityNeedsEssSnapshotByIndex(size_t idx)
{
	if (idx >= kMqttEntityCount) {
		return false;
	}
	return kNeedsEssSnapshot[idx];
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
