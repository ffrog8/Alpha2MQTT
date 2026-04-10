/*
  RS485HandlerStub.h

  Compile-time RS485/Modbus stub backend to allow deterministic scheduling/snapshot testing
  without inverter hardware.

  Enabled via: -DRS485_STUB=1
*/
#pragma once

#include <Arduino.h>

#include "Definitions.h"
#include "Rs485StubLogic.h"

// Keep behavior consistent with the real backend defaults.
#ifndef DEFAULT_BAUD_RATE
#define DEFAULT_BAUD_RATE 9600
#endif

// Defaults are safe and deterministic; override at build time if desired.
#ifndef RS485_STUB_MODE
#define RS485_STUB_MODE 0
#endif
#ifndef RS485_STUB_FAIL_FIRST_N
#define RS485_STUB_FAIL_FIRST_N 0
#endif
#ifndef RS485_STUB_FAIL_REGISTER
#define RS485_STUB_FAIL_REGISTER 0
#endif
#ifndef RS485_STUB_FAIL_TYPE
// 0 = noResponse, 1 = slaveError
#define RS485_STUB_FAIL_TYPE 0
#endif
#ifndef RS485_STUB_LATENCY_MS
#define RS485_STUB_LATENCY_MS 0
#endif

class RS485Handler
{
	private:
		struct VirtualInverterState {
			// Identity (16 bytes / 8 registers)
			uint8_t serialBytes[16] = { 0 };

			// Snapshot-relevant values (raw register units).
			uint16_t dispatchStart = 0;
			uint16_t dispatchMode = DISPATCH_MODE_NORMAL_MODE;
			int32_t dispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET;
			uint16_t dispatchSoc = 0;
			uint32_t dispatchTime = 0;

			uint16_t batterySocX10 = 650; // 65.0% => 650 with BATTERY_SOC_MULTIPLIER=0.1
			int16_t batteryPowerW = 0;
			int32_t gridPowerW = 0;

			// PV meter and per-string PV (used by REG_CUSTOM_TOTAL_SOLAR_POWER).
			int32_t pvCtPowerW = 0;
			int32_t pvPowerW[6] = { 0, 0, 0, 0, 0, 0 };

			uint16_t inverterWorkingMode = INVERTER_OPERATION_MODE_UPS_MODE;
			uint16_t maxFeedinPercent = 100;
			uint16_t modbusBaudRateEnum = MODBUS_BAUD_RATE_9600;
		};

		void (*_serviceHook)() = nullptr;
		unsigned long _baudRate = DEFAULT_BAUD_RATE;
		bool _rs485IsOnline = false;
		uint32_t _snapshotAttemptIndex = 0;
		bool _inSnapshot = false;
		char _uartInfoString[OLED_CHARACTER_WIDTH] = "RS485-STUB";
		Rs485StubConfig _cfg;
		bool _cfgRuntime = false;
		VirtualInverterState _state;
		uint32_t _cfgAppliedMs = 0;
		uint32_t _probeAttempts = 0;
		int16_t _socStepX10PerSnapshot = 0;
		bool _inTransaction = false;

		uint32_t _readCount = 0;
		uint32_t _writeCount = 0;
		uint32_t _unknownRegisterReads = 0;
		uint16_t _lastReadStartReg = 0;
		uint16_t _lastReadRegCount = 0;
		uint8_t _lastFn = 0;
		uint16_t _lastFailStartReg = 0;
		uint8_t _lastFailFn = 0;
		Rs485StubFailType _lastFailType = Rs485StubFailType::NoResponse;
		uint16_t _lastWriteFailStartReg = 0;
		uint8_t _lastWriteFailFn = 0;
		Rs485StubFailType _lastWriteFailType = Rs485StubFailType::NoResponse;
		uint16_t _lastWriteStartReg = 0;
		uint16_t _lastWriteRegCount = 0;
		uint32_t _lastWriteMs = 0;

		static inline uint16_t hi16(uint32_t value) { return static_cast<uint16_t>((value >> 16) & 0xFFFF); }
		static inline uint16_t lo16(uint32_t value) { return static_cast<uint16_t>(value & 0xFFFF); }

		void initDefaultSerial()
		{
			// 15 characters + '\0' for rs485TryReadIdentityOnce() length >= 15 check.
			const char *serial = "STUBSN000000000";
			memset(_state.serialBytes, 0, sizeof(_state.serialBytes));
			for (size_t i = 0; i < 15 && serial[i] != '\0'; i++) {
				_state.serialBytes[i] = static_cast<uint8_t>(serial[i]);
			}
		}

		Rs485StubConfig buildConfig() const
		{
			Rs485StubConfig cfg;
			cfg.mode = static_cast<Rs485StubMode>(RS485_STUB_MODE);
			cfg.failFirstN = static_cast<uint32_t>(RS485_STUB_FAIL_FIRST_N);
			cfg.failRegister = static_cast<uint16_t>(RS485_STUB_FAIL_REGISTER);
			cfg.failType = static_cast<Rs485StubFailType>(RS485_STUB_FAIL_TYPE);
			cfg.latencyMs = static_cast<uint16_t>(RS485_STUB_LATENCY_MS);
			return cfg;
		}

		void simulateLatency(uint16_t latencyMs)
		{
			if (latencyMs == 0) {
				return;
			}
			const uint32_t start = millis();
			while (static_cast<uint32_t>(millis() - start) < latencyMs) {
				if (_serviceHook != nullptr) {
					_serviceHook();
				}
				delay(1);
			}
		}

		bool wordForRegisterVirtual(uint16_t reg, uint16_t *outWord)
		{
			if (outWord == nullptr) {
				return false;
			}

			// Identity block: 8 registers of 2 bytes each.
			if (reg >= REG_SYSTEM_INFO_R_EMS_SN_BYTE_1_2 &&
			    reg < static_cast<uint16_t>(REG_SYSTEM_INFO_R_EMS_SN_BYTE_1_2 + 8)) {
				const uint16_t offset = static_cast<uint16_t>(reg - REG_SYSTEM_INFO_R_EMS_SN_BYTE_1_2);
				const uint8_t b0 = _state.serialBytes[offset * 2];
				const uint8_t b1 = _state.serialBytes[offset * 2 + 1];
				*outWord = static_cast<uint16_t>((b0 << 8) | b1);
				return true;
			}

			// Probe register: anything stable works; "1" is fine.
			if (reg == REG_SAFETY_TEST_RW_GRID_REGULATION) {
				*outWord = 1;
				return true;
			}

			// Dispatch registers.
			if (reg == REG_DISPATCH_RW_DISPATCH_START) {
				*outWord = _state.dispatchStart;
				return true;
			}
			if (reg == REG_DISPATCH_RW_DISPATCH_MODE) {
				*outWord = _state.dispatchMode;
				return true;
			}
			if (reg == REG_DISPATCH_RW_ACTIVE_POWER_1) {
				*outWord = hi16(static_cast<uint32_t>(_state.dispatchActivePower));
				return true;
			}
			if (reg == static_cast<uint16_t>(REG_DISPATCH_RW_ACTIVE_POWER_1 + 1)) {
				*outWord = lo16(static_cast<uint32_t>(_state.dispatchActivePower));
				return true;
			}
			if (reg == REG_DISPATCH_RW_REACTIVE_POWER_1 ||
			    reg == static_cast<uint16_t>(REG_DISPATCH_RW_REACTIVE_POWER_1 + 1)) {
				*outWord = 0;
				return true;
			}
			if (reg == REG_DISPATCH_RW_DISPATCH_SOC) {
				*outWord = _state.dispatchSoc;
				return true;
			}
			if (reg == REG_DISPATCH_RW_DISPATCH_TIME_1) {
				*outWord = hi16(_state.dispatchTime);
				return true;
			}
			if (reg == static_cast<uint16_t>(REG_DISPATCH_RW_DISPATCH_TIME_1 + 1)) {
				*outWord = lo16(_state.dispatchTime);
				return true;
			}

			// Battery / grid.
			if (reg == REG_BATTERY_HOME_R_SOC) {
				*outWord = _state.batterySocX10;
				return true;
			}
			if (reg == REG_BATTERY_HOME_R_BATTERY_POWER) {
				*outWord = static_cast<uint16_t>(_state.batteryPowerW);
				return true;
			}
			if (reg == REG_GRID_METER_R_TOTAL_ACTIVE_POWER_1) {
				*outWord = hi16(static_cast<uint32_t>(_state.gridPowerW));
				return true;
			}
			if (reg == static_cast<uint16_t>(REG_GRID_METER_R_TOTAL_ACTIVE_POWER_1 + 1)) {
				*outWord = lo16(static_cast<uint32_t>(_state.gridPowerW));
				return true;
			}

			// PV CT power.
			if (reg == REG_PV_METER_R_TOTAL_ACTIVE_POWER_1) {
				*outWord = hi16(static_cast<uint32_t>(_state.pvCtPowerW));
				return true;
			}
			if (reg == static_cast<uint16_t>(REG_PV_METER_R_TOTAL_ACTIVE_POWER_1 + 1)) {
				*outWord = lo16(static_cast<uint32_t>(_state.pvCtPowerW));
				return true;
			}

			// PV1..PV6 grouped block. The real grouped read spans voltage/current/power,
			// so strict-unknown mode must virtualize the whole contiguous region, not
			// only the power words that the current snapshot math consumes.
			const uint16_t pvVoltageRegs[6] = {
				REG_INVERTER_HOME_R_PV1_VOLTAGE,
				REG_INVERTER_HOME_R_PV2_VOLTAGE,
				REG_INVERTER_HOME_R_PV3_VOLTAGE,
				REG_INVERTER_HOME_R_PV4_VOLTAGE,
				REG_INVERTER_HOME_R_PV5_VOLTAGE,
				REG_INVERTER_HOME_R_PV6_VOLTAGE,
			};
			const uint16_t pvCurrentRegs[6] = {
				REG_INVERTER_HOME_R_PV1_CURRENT,
				REG_INVERTER_HOME_R_PV2_CURRENT,
				REG_INVERTER_HOME_R_PV3_CURRENT,
				REG_INVERTER_HOME_R_PV4_CURRENT,
				REG_INVERTER_HOME_R_PV5_CURRENT,
				REG_INVERTER_HOME_R_PV6_CURRENT,
			};
			const uint16_t pvRegs[6] = {
				REG_INVERTER_HOME_R_PV1_POWER_1,
				REG_INVERTER_HOME_R_PV2_POWER_1,
				REG_INVERTER_HOME_R_PV3_POWER_1,
				REG_INVERTER_HOME_R_PV4_POWER_1,
				REG_INVERTER_HOME_R_PV5_POWER_1,
				REG_INVERTER_HOME_R_PV6_POWER_1,
			};
			for (uint8_t i = 0; i < 6; i++) {
				if (reg == pvVoltageRegs[i] || reg == pvCurrentRegs[i]) {
					*outWord = 0;
					return true;
				}
				if (reg == pvRegs[i]) {
					*outWord = hi16(static_cast<uint32_t>(_state.pvPowerW[i]));
					return true;
				}
				if (reg == static_cast<uint16_t>(pvRegs[i] + 1)) {
					*outWord = lo16(static_cast<uint32_t>(_state.pvPowerW[i]));
					return true;
				}
			}

			if (reg == REG_INVERTER_HOME_R_WORKING_MODE) {
				*outWord = _state.inverterWorkingMode;
				return true;
			}
			if (reg == REG_SYSTEM_CONFIG_RW_MAX_FEED_INTO_GRID_PERCENT ||
			    reg == REG_SYSTEM_INFO_RW_FEED_INTO_GRID_PERCENT) {
				*outWord = _state.maxFeedinPercent;
				return true;
			}
			if (reg == REG_SYSTEM_CONFIG_RW_MODBUS_BAUD_RATE) {
				*outWord = _state.modbusBaudRateEnum;
				return true;
			}

			return false;
		}

	public:
		RS485Handler()
		{
			_cfg = buildConfig();
			initDefaultSerial();
			_cfgAppliedMs = millis();
		}

		void beginSnapshotAttempt()
		{
			_inSnapshot = true;
			_snapshotAttemptIndex++;
			if (_socStepX10PerSnapshot != 0) {
				int32_t next = static_cast<int32_t>(_state.batterySocX10) + static_cast<int32_t>(_socStepX10PerSnapshot);
				if (next < 0) next = 0;
				if (next > 1000) next = 1000;
				_state.batterySocX10 = static_cast<uint16_t>(next);
			}
		}

		void endSnapshotAttempt()
		{
			_inSnapshot = false;
		}

		void applyStubControl(Rs485StubMode mode, uint32_t failFirstN, uint16_t failRegister)
		{
			applyStubControl(mode, failFirstN, failRegister, _cfg.failType, _cfg.latencyMs);
		}

		void applyStubControl(Rs485StubMode mode, uint32_t failFirstN, uint16_t failRegister, Rs485StubFailType failType, uint16_t latencyMs)
		{
			_cfgRuntime = true;
			_cfgAppliedMs = millis();
			_probeAttempts = 0;
			_cfg.mode = mode;
			_cfg.failFirstN = failFirstN;
			_cfg.failRegister = failRegister;
			_cfg.failType = failType;
			_cfg.latencyMs = latencyMs;
			_snapshotAttemptIndex = 0;
			_rs485IsOnline = false;
		}

		void applyAdvancedControl(
			bool strictUnknown,
			uint32_t failEveryN,
			bool failReads,
			bool failWrites,
			uint32_t failForMs,
			uint32_t flapOnlineMs,
			uint32_t flapOfflineMs,
			uint32_t probeSuccessAfterN,
			int16_t socStepX10PerSnapshot)
		{
			_cfg.strictUnknownRegisters = strictUnknown;
			_cfg.failEveryN = failEveryN;
			_cfg.failReads = failReads;
			_cfg.failWrites = failWrites;
			_cfg.failForMs = failForMs;
			_cfg.flapOnlineMs = flapOnlineMs;
			_cfg.flapOfflineMs = flapOfflineMs;
			_cfg.probeSuccessAfterN = probeSuccessAfterN;
			_socStepX10PerSnapshot = socStepX10PerSnapshot;
		}

		void applyVirtualInverterState(
			uint16_t batterySocPct,
			int16_t batteryPowerW,
			int32_t gridPowerW,
			int32_t pvCtPowerW,
			uint16_t inverterWorkingMode)
		{
			_state.batterySocX10 = static_cast<uint16_t>(batterySocPct * 10);
			_state.batteryPowerW = batteryPowerW;
			_state.gridPowerW = gridPowerW;
			_state.pvCtPowerW = pvCtPowerW;
			_state.inverterWorkingMode = inverterWorkingMode;
		}

		void applyVirtualModbusBaud(uint16_t modbusBaudRateEnum)
		{
			_state.modbusBaudRateEnum = modbusBaudRateEnum;
		}

		void applyVirtualDispatchState(uint16_t start,
		                             uint16_t mode,
		                             int32_t activePower,
		                             uint16_t dispatchSoc,
		                             uint32_t dispatchTime)
		{
			_state.dispatchStart = start;
			_state.dispatchMode = mode;
			_state.dispatchActivePower = activePower;
			_state.dispatchSoc = dispatchSoc;
			_state.dispatchTime = dispatchTime;
		}

		void setSerialString(const char *serial)
		{
			if (serial == nullptr || *serial == '\0') {
				return;
			}
			memset(_state.serialBytes, 0, sizeof(_state.serialBytes));
			for (size_t i = 0; i < sizeof(_state.serialBytes) - 1 && serial[i] != '\0'; i++) {
				_state.serialBytes[i] = static_cast<uint8_t>(serial[i]);
			}
		}

		const char *stubModeLabel() const
		{
			switch (_cfg.mode) {
			case Rs485StubMode::OfflineForever:
				return "offline";
			case Rs485StubMode::OnlineAlways:
				return "online";
			case Rs485StubMode::FailFirstNThenRecover:
				return "fail_then_recover";
			case Rs485StubMode::FlapTime:
				return "flap";
			case Rs485StubMode::ProbeDelayedOnline:
				return "probe_delayed";
			default:
				return "unknown";
			}
		}

		const Rs485StubConfig &stubConfig() const
		{
			return _cfg;
		}

		uint32_t stubFailRemaining() const
		{
			if (_cfg.mode != Rs485StubMode::FailFirstNThenRecover) {
				return 0;
			}
			if (_snapshotAttemptIndex >= _cfg.failFirstN) {
				return 0;
			}
			return _cfg.failFirstN - _snapshotAttemptIndex;
		}

		uint32_t stubReadCount() const { return _readCount; }
		uint32_t stubWriteCount() const { return _writeCount; }
		uint32_t stubUnknownRegisterReads() const { return _unknownRegisterReads; }
		uint16_t stubLastReadStartReg() const { return _lastReadStartReg; }
		uint16_t stubLastReadRegCount() const { return _lastReadRegCount; }
		uint8_t stubLastFn() const { return _lastFn; }
		uint16_t stubLastFailStartReg() const { return _lastFailStartReg; }
		uint8_t stubLastFailFn() const { return _lastFailFn; }
		const char *stubLastFailTypeLabel() const
		{
			switch (_lastFailType) {
			case Rs485StubFailType::SlaveError:
				return "slave_error";
			case Rs485StubFailType::NoResponse:
			default:
				return "no_response";
			}
		}
		uint16_t stubLastWriteFailStartReg() const { return _lastWriteFailStartReg; }
		uint8_t stubLastWriteFailFn() const { return _lastWriteFailFn; }
		const char *stubLastWriteFailTypeLabel() const
		{
			switch (_lastWriteFailType) {
			case Rs485StubFailType::SlaveError:
				return "slave_error";
			case Rs485StubFailType::NoResponse:
			default:
				return "no_response";
			}
		}
		uint16_t stubFailRegister() const { return _cfg.failRegister; }
		const char *stubFailTypeLabel() const
		{
			switch (_cfg.failType) {
			case Rs485StubFailType::SlaveError:
				return "slave_error";
			case Rs485StubFailType::NoResponse:
			default:
				return "no_response";
			}
		}
		uint16_t stubLatencyMs() const { return _cfg.latencyMs; }
		bool stubStrictUnknown() const { return _cfg.strictUnknownRegisters; }
		bool stubFailReads() const { return _cfg.failReads; }
		bool stubFailWrites() const { return _cfg.failWrites; }
		uint32_t stubFailEveryN() const { return _cfg.failEveryN; }
		uint32_t stubFailForMs() const { return _cfg.failForMs; }
		uint32_t stubFlapOnlineMs() const { return _cfg.flapOnlineMs; }
		uint32_t stubFlapOfflineMs() const { return _cfg.flapOfflineMs; }
		uint32_t stubProbeAttempts() const { return _probeAttempts; }
		uint32_t stubProbeSuccessAfterN() const { return _cfg.probeSuccessAfterN; }
		int16_t stubSocStepX10PerSnapshot() const { return _socStepX10PerSnapshot; }
		uint16_t stubBatterySocX10() const { return _state.batterySocX10; }
		uint16_t stubLastWriteStartReg() const { return _lastWriteStartReg; }
		uint16_t stubLastWriteRegCount() const { return _lastWriteRegCount; }
		uint32_t stubLastWriteMs() const { return _lastWriteMs; }
		bool inTransaction() const { return _inTransaction; }

		~RS485Handler() = default;

		void setServiceHook(void (*hook)())
		{
			_serviceHook = hook;
		}

		modbusRequestAndResponseStatusValues sendModbus(uint8_t frame[], byte actualFrameSize, modbusRequestAndResponse* resp)
		{
			(void)actualFrameSize;
			if (resp == nullptr || frame == nullptr) {
				return modbusRequestAndResponseStatusValues::invalidFrame;
			}
			struct TxnGuard {
				bool &flag;
				explicit TxnGuard(bool &f) : flag(f) { flag = true; }
				~TxnGuard() { flag = false; }
			} txnGuard(_inTransaction);

			const uint8_t fn = frame[1];
			resp->functionCode = fn;
			_lastFn = fn;

			const uint16_t startRegister = static_cast<uint16_t>((frame[2] << 8) | frame[3]);
			const uint16_t registerCount = static_cast<uint16_t>((frame[4] << 8) | frame[5]);
			if (fn == MODBUS_FN_READDATAREGISTER) {
				_lastReadStartReg = startRegister;
				_lastReadRegCount = registerCount;
			}

			if (_serviceHook != nullptr) {
				_serviceHook();
			}

			simulateLatency(_cfg.latencyMs);

			bool shouldFail = false;
			if (_cfg.failForMs != 0 && static_cast<uint32_t>(millis() - _cfgAppliedMs) < _cfg.failForMs) {
				shouldFail = true;
			}

			if (!shouldFail &&
			    _cfg.mode == Rs485StubMode::ProbeDelayedOnline &&
			    _cfg.probeSuccessAfterN > 0) {
				if (fn == MODBUS_FN_READDATAREGISTER) {
					_probeAttempts++;
				}
				if (_probeAttempts < _cfg.probeSuccessAfterN) {
					shouldFail = true;
				}
			}

			if (!shouldFail &&
			    _cfg.mode == Rs485StubMode::FlapTime &&
			    _cfg.flapOnlineMs > 0 &&
			    _cfg.flapOfflineMs > 0) {
				const uint32_t period = _cfg.flapOnlineMs + _cfg.flapOfflineMs;
				const uint32_t phase = static_cast<uint32_t>(millis() - _cfgAppliedMs) % period;
				const bool isOnline = phase < _cfg.flapOnlineMs;
				shouldFail = !isOnline;
			}

			if (!shouldFail && _cfg.failEveryN > 0 && _inSnapshot) {
				if ((_snapshotAttemptIndex % _cfg.failEveryN) == 0) {
					shouldFail = true;
				}
			}

			if (!shouldFail && _cfg.failRegister != 0 && startRegister == _cfg.failRegister) {
				shouldFail = true;
			}

			if (!shouldFail) {
				switch (_cfg.mode) {
				case Rs485StubMode::OfflineForever:
					shouldFail = true;
					break;
				case Rs485StubMode::OnlineAlways:
					shouldFail = false;
					break;
				case Rs485StubMode::FailFirstNThenRecover:
					// Interpret "attempts" as ESS snapshot refresh attempts (not raw Modbus transactions).
					// Only fail while inside a snapshot attempt, so unrelated reads don't consume the failure budget.
					shouldFail = _inSnapshot && rs485StubShouldFail(_cfg, _snapshotAttemptIndex, startRegister);
					break;
				case Rs485StubMode::FlapTime:
				case Rs485StubMode::ProbeDelayedOnline:
					// Determined above.
					break;
				default:
					shouldFail = true;
					break;
				}
			}

			if (shouldFail) {
				if (fn == MODBUS_FN_READDATAREGISTER && !_cfg.failReads) {
					shouldFail = false;
				}
				if ((fn == MODBUS_FN_WRITEDATAREGISTER || fn == MODBUS_FN_WRITESINGLEREGISTER) &&
				    !_cfg.failWrites) {
					shouldFail = false;
				}
			}

			if (shouldFail) {
#ifdef DEBUG_OVER_SERIAL
				char dbg[192];
				snprintf(dbg,
				         sizeof(dbg),
				         "stub shouldFail: reg=%u fn=%u mode=%u strict=%u failN=%lu failReg=%u failEveryN=%lu failReads=%u failWrites=%u failForMs=%lu failType=%u inSnapshot=%u snapIdx=%lu",
				         static_cast<unsigned>(startRegister),
				         static_cast<unsigned>(fn),
				         static_cast<unsigned>(_cfg.mode),
				         static_cast<unsigned>(_cfg.strictUnknownRegisters ? 1 : 0),
				         static_cast<unsigned long>(_cfg.failFirstN),
				         static_cast<unsigned>(_cfg.failRegister),
				         static_cast<unsigned long>(_cfg.failEveryN),
				         static_cast<unsigned>(_cfg.failReads ? 1 : 0),
				         static_cast<unsigned>(_cfg.failWrites ? 1 : 0),
				         static_cast<unsigned long>(_cfg.failForMs),
				         static_cast<unsigned>(_cfg.failType),
				         static_cast<unsigned>(_inSnapshot ? 1 : 0),
				         static_cast<unsigned long>(_snapshotAttemptIndex));
				Serial.println(dbg);
#endif
				// For slave-exception responses, the bus is still "online" (we got a response).
				// For timeouts/no-response, treat the bus as offline.
				_rs485IsOnline = (_cfg.failType == Rs485StubFailType::SlaveError);
				resp->dataSize = 0;
				_lastFailStartReg = startRegister;
				_lastFailFn = fn;
				_lastFailType = _cfg.failType;
				if (fn == MODBUS_FN_WRITEDATAREGISTER || fn == MODBUS_FN_WRITESINGLEREGISTER) {
					_lastWriteFailStartReg = startRegister;
					_lastWriteFailFn = fn;
					_lastWriteFailType = _cfg.failType;
				}
				if (_cfg.failType == Rs485StubFailType::SlaveError) {
					strcpy(resp->statusMqttMessage, MODBUS_REQUEST_AND_RESPONSE_ERROR_MQTT_DESC);
					strcpy(resp->displayMessage, MODBUS_REQUEST_AND_RESPONSE_ERROR_DISPLAY_DESC);
					return modbusRequestAndResponseStatusValues::slaveError;
				}
				strcpy(resp->statusMqttMessage, MODBUS_REQUEST_AND_RESPONSE_NO_RESPONSE_MQTT_DESC);
				strcpy(resp->displayMessage, MODBUS_REQUEST_AND_RESPONSE_NO_RESPONSE_DISPLAY_DESC);
				return modbusRequestAndResponseStatusValues::noResponse;
			}

			_rs485IsOnline = true;

			if (fn == MODBUS_FN_READDATAREGISTER) {
				_readCount++;
				const uint16_t count = registerCount == 0 ? 1 : registerCount;
				const uint16_t maxWords = static_cast<uint16_t>(MAX_FRAME_SIZE / 2);
				const uint16_t wordsToWrite = (count > maxWords) ? maxWords : count;

				resp->dataSize = static_cast<uint8_t>(wordsToWrite * 2);
				for (uint16_t i = 0; i < wordsToWrite; i++) {
					const uint16_t reg = static_cast<uint16_t>(startRegister + i);
					uint16_t word;
					if (!wordForRegisterVirtual(reg, &word)) {
						_unknownRegisterReads++;
						if (_cfg.strictUnknownRegisters) {
							// Unknown-register strict mode returns a slave error, which implies a response.
							_rs485IsOnline = true;
							resp->dataSize = 0;
							_lastFailStartReg = startRegister;
							_lastFailFn = fn;
							_lastFailType = Rs485StubFailType::SlaveError;
							strcpy(resp->statusMqttMessage, MODBUS_REQUEST_AND_RESPONSE_ERROR_MQTT_DESC);
							strcpy(resp->displayMessage, MODBUS_REQUEST_AND_RESPONSE_ERROR_DISPLAY_DESC);
							return modbusRequestAndResponseStatusValues::slaveError;
						}
						word = rs485StubWordForRegister(reg);
					}
					resp->data[i * 2] = static_cast<uint8_t>((word >> 8) & 0xFF);
					resp->data[i * 2 + 1] = static_cast<uint8_t>(word & 0xFF);
				}

				if (wordsToWrite > 0) {
					const uint16_t firstWord =
						static_cast<uint16_t>((resp->data[0] << 8) | resp->data[1]);
					switch (resp->returnDataType) {
					case modbusReturnDataType::unsignedShort:
						resp->unsignedShortValue = firstWord;
						snprintf(resp->dataValueFormatted, sizeof(resp->dataValueFormatted), "%u", resp->unsignedShortValue);
						break;
					case modbusReturnDataType::signedShort:
						resp->signedShortValue = static_cast<int16_t>(firstWord);
						snprintf(resp->dataValueFormatted, sizeof(resp->dataValueFormatted), "%d", resp->signedShortValue);
						break;
					case modbusReturnDataType::unsignedInt:
						if (wordsToWrite >= 2) {
							resp->unsignedIntValue = (static_cast<uint32_t>(firstWord) << 16) |
							                         static_cast<uint32_t>((resp->data[2] << 8) | resp->data[3]);
							snprintf(resp->dataValueFormatted,
							         sizeof(resp->dataValueFormatted),
							         "%lu",
							         static_cast<unsigned long>(resp->unsignedIntValue));
						}
						break;
					case modbusReturnDataType::signedInt:
						if (wordsToWrite >= 2) {
							const uint32_t raw = (static_cast<uint32_t>(firstWord) << 16) |
							                     static_cast<uint32_t>((resp->data[2] << 8) | resp->data[3]);
							resp->signedIntValue = static_cast<int32_t>(raw);
							snprintf(resp->dataValueFormatted,
							         sizeof(resp->dataValueFormatted),
							         "%ld",
							         static_cast<long>(resp->signedIntValue));
						}
						break;
					default:
						break;
					}
				}

				strcpy(resp->statusMqttMessage, MODBUS_REQUEST_AND_RESPONSE_READ_DATA_REGISTER_SUCCESS_MQTT_DESC);
				strcpy(resp->displayMessage, MODBUS_REQUEST_AND_RESPONSE_READ_DATA_REGISTER_SUCCESS_DISPLAY_DESC);
				return modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
			}

			if (fn == MODBUS_FN_WRITEDATAREGISTER) {
				_writeCount++;
				_lastWriteStartReg = startRegister;
				_lastWriteRegCount = registerCount;
				_lastWriteMs = millis();

				// Best-effort parse of the 0x10 payload.
				const uint8_t byteCount = frame[6];
				const uint16_t words = static_cast<uint16_t>(byteCount / 2);
				const uint16_t usableWords = (registerCount < words) ? registerCount : words;
				if (usableWords > 0) {
					const uint8_t *data = &frame[7];
					if (startRegister == REG_DISPATCH_RW_DISPATCH_START && usableWords >= 1) {
						_state.dispatchStart = static_cast<uint16_t>((data[0] << 8) | data[1]);
					}
					if (startRegister == REG_SYSTEM_CONFIG_RW_MODBUS_BAUD_RATE && usableWords >= 1) {
						_state.modbusBaudRateEnum = static_cast<uint16_t>((data[0] << 8) | data[1]);
					}
					// Special-case dispatch block write, as used by writeDispatchRegisters().
					if (startRegister == REG_DISPATCH_RW_DISPATCH_START && usableWords >= 9) {
						const uint16_t w0 = static_cast<uint16_t>((data[0] << 8) | data[1]);
						const uint16_t w1 = static_cast<uint16_t>((data[2] << 8) | data[3]);
						const uint16_t w2 = static_cast<uint16_t>((data[4] << 8) | data[5]);
						const uint16_t w5 = static_cast<uint16_t>((data[10] << 8) | data[11]);
						const uint16_t w6 = static_cast<uint16_t>((data[12] << 8) | data[13]);
						const uint16_t w7 = static_cast<uint16_t>((data[14] << 8) | data[15]);
						const uint16_t w8 = static_cast<uint16_t>((data[16] << 8) | data[17]);

						const uint32_t ap = (static_cast<uint32_t>(w1) << 16) | w2;
						const uint32_t dispatchTime = (static_cast<uint32_t>(w7) << 16) | w8;
						_state.dispatchStart = w0;
						_state.dispatchActivePower = static_cast<int32_t>(ap);
						_state.dispatchMode = w5;
						_state.dispatchSoc = w6;
						_state.dispatchTime = dispatchTime;
					}
				}

				resp->dataSize = 0;
				strcpy(resp->statusMqttMessage, MODBUS_REQUEST_AND_RESPONSE_WRITE_DATA_REGISTER_SUCCESS_MQTT_DESC);
				strcpy(resp->displayMessage, MODBUS_REQUEST_AND_RESPONSE_WRITE_DATA_REGISTER_SUCCESS_DISPLAY_DESC);
				return modbusRequestAndResponseStatusValues::writeDataRegisterSuccess;
			}

			if (fn == MODBUS_FN_WRITESINGLEREGISTER) {
				_writeCount++;
				_lastWriteStartReg = startRegister;
				_lastWriteRegCount = 1;
				_lastWriteMs = millis();

				const uint16_t value = static_cast<uint16_t>((frame[4] << 8) | frame[5]);
				if (startRegister == REG_SYSTEM_CONFIG_RW_MAX_FEED_INTO_GRID_PERCENT) {
					_state.maxFeedinPercent = value;
				}

				resp->dataSize = 0;
				strcpy(resp->statusMqttMessage, MODBUS_REQUEST_AND_RESPONSE_WRITE_SINGLE_REGISTER_SUCCESS_MQTT_DESC);
				strcpy(resp->displayMessage, MODBUS_REQUEST_AND_RESPONSE_WRITE_SINGLE_REGISTER_SUCCESS_DISPLAY_DESC);
				return modbusRequestAndResponseStatusValues::writeSingleRegisterSuccess;
			}

			strcpy(resp->statusMqttMessage, MODBUS_REQUEST_AND_RESPONSE_INVALID_FRAME_MQTT_DESC);
			strcpy(resp->displayMessage, MODBUS_REQUEST_AND_RESPONSE_INVALID_FRAME_DISPLAY_DESC);
			return modbusRequestAndResponseStatusValues::invalidFrame;
		}

		bool checkCRC(uint8_t frame[], byte actualFrameSize)
		{
			(void)frame;
			(void)actualFrameSize;
			return true;
		}

		void calcCRC(uint8_t frame[], byte actualFrameSize)
		{
			(void)frame;
			(void)actualFrameSize;
		}

#if defined(DEBUG_OVER_SERIAL) || defined(DEBUG_LEVEL2) || defined(DEBUG_OUTPUT_TX_RX)
		void setDebugOutput(char* _db)
		{
			(void)_db;
		}
#endif

		void setBaudRate(unsigned long baudRate)
		{
			_baudRate = baudRate;
			snprintf(_uartInfoString, sizeof(_uartInfoString), "RS485-STUB %lu", _baudRate);
		}

		bool isRs485Online()
		{
			return _rs485IsOnline;
		}

		char *uartInfo()
		{
			return _uartInfoString;
		}
};
