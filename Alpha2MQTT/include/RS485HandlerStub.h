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

class RS485Handler
{
	private:
		void (*_serviceHook)() = nullptr;
		unsigned long _baudRate = DEFAULT_BAUD_RATE;
		bool _rs485IsOnline = false;
		uint32_t _snapshotAttemptIndex = 0;
		bool _inSnapshot = false;
		char _uartInfoString[OLED_CHARACTER_WIDTH] = "RS485-STUB";
		Rs485StubConfig _cfg;
		bool _cfgRuntime = false;

		Rs485StubConfig buildConfig() const
		{
			Rs485StubConfig cfg;
			cfg.mode = static_cast<Rs485StubMode>(RS485_STUB_MODE);
			cfg.failFirstN = static_cast<uint32_t>(RS485_STUB_FAIL_FIRST_N);
			cfg.failRegister = static_cast<uint16_t>(RS485_STUB_FAIL_REGISTER);
			return cfg;
		}

	public:
		RS485Handler()
		{
			_cfg = buildConfig();
		}

		void beginSnapshotAttempt()
		{
			_inSnapshot = true;
			_snapshotAttemptIndex++;
		}

		void endSnapshotAttempt()
		{
			_inSnapshot = false;
		}

		void applyStubControl(Rs485StubMode mode, uint32_t failFirstN, uint16_t failRegister)
		{
			_cfgRuntime = true;
			_cfg.mode = mode;
			_cfg.failFirstN = failFirstN;
			_cfg.failRegister = failRegister;
			_snapshotAttemptIndex = 0;
			_rs485IsOnline = false;
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
			default:
				return "unknown";
			}
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

			const uint8_t fn = frame[1];
			resp->functionCode = fn;

			const uint16_t startRegister = static_cast<uint16_t>((frame[2] << 8) | frame[3]);
			const uint16_t registerCount = static_cast<uint16_t>((frame[4] << 8) | frame[5]);

			if (_serviceHook != nullptr) {
				_serviceHook();
			}

			if (fn != MODBUS_FN_READDATAREGISTER) {
				strcpy(resp->statusMqttMessage, MODBUS_REQUEST_AND_RESPONSE_INVALID_FRAME_MQTT_DESC);
				strcpy(resp->displayMessage, MODBUS_REQUEST_AND_RESPONSE_INVALID_FRAME_DISPLAY_DESC);
				return modbusRequestAndResponseStatusValues::invalidFrame;
			}

			bool shouldFail = false;
			if (_cfg.failRegister != 0 && startRegister == _cfg.failRegister) {
				shouldFail = true;
			} else {
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
				default:
					shouldFail = true;
					break;
				}
			}

			if (shouldFail) {
				_rs485IsOnline = false;
				resp->dataSize = 0;
				strcpy(resp->statusMqttMessage, MODBUS_REQUEST_AND_RESPONSE_NO_RESPONSE_MQTT_DESC);
				strcpy(resp->displayMessage, MODBUS_REQUEST_AND_RESPONSE_NO_RESPONSE_DISPLAY_DESC);
				return modbusRequestAndResponseStatusValues::noResponse;
			}

			_rs485IsOnline = true;

			const uint16_t count = registerCount == 0 ? 1 : registerCount;
			const uint16_t maxWords = static_cast<uint16_t>(MAX_FRAME_SIZE / 2);
			const uint16_t wordsToWrite = (count > maxWords) ? maxWords : count;

			resp->dataSize = static_cast<uint8_t>(wordsToWrite * 2);
			for (uint16_t i = 0; i < wordsToWrite; i++) {
				const uint16_t word = rs485StubWordForRegister(static_cast<uint16_t>(startRegister + i));
				resp->data[i * 2] = static_cast<uint8_t>((word >> 8) & 0xFF);
				resp->data[i * 2 + 1] = static_cast<uint8_t>(word & 0xFF);
			}

			strcpy(resp->statusMqttMessage, MODBUS_REQUEST_AND_RESPONSE_READ_DATA_REGISTER_SUCCESS_MQTT_DESC);
			strcpy(resp->displayMessage, MODBUS_REQUEST_AND_RESPONSE_READ_DATA_REGISTER_SUCCESS_DISPLAY_DESC);
			return modbusRequestAndResponseStatusValues::readDataRegisterSuccess;
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
