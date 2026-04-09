// Purpose: Provide stable compile-time-gated serial logging helpers for
// firmware diagnostics without add/remove churn in the source tree.
// Responsibilities: Keep disabled logging cost at zero, keep literal strings
// in flash on ESP8266-class targets, and offer basic once/rate-limited helpers.
// Invariants: All macros are no-ops unless both ARDUINO and DEBUG_OVER_SERIAL
// are defined for the current build.
#pragma once

#include <stdint.h>

#if defined(ARDUINO)
#include <Arduino.h>
#endif

#if defined(ARDUINO) && defined(DEBUG_OVER_SERIAL)
#define A2M_DEBUG_ENABLED 1
#else
#define A2M_DEBUG_ENABLED 0
#endif

#if A2M_DEBUG_ENABLED

#define A2M_DEBUG_LINE(literal) \
	do { \
		Serial.println(F(literal)); \
	} while (0)

#if defined(MP_ESP8266) || defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
#define A2M_DEBUG_PRINTF(fmt, ...) \
	do { \
		Serial.printf_P(PSTR(fmt), ##__VA_ARGS__); \
	} while (0)
#else
#define A2M_DEBUG_PRINTF(fmt, ...) \
	do { \
		Serial.printf((fmt), ##__VA_ARGS__); \
	} while (0)
#endif

#define A2M_DEBUG_ONCE(code) \
	do { \
		static bool _a2m_debug_once_logged = false; \
		if (!_a2m_debug_once_logged) { \
			_a2m_debug_once_logged = true; \
			code; \
		} \
	} while (0)

#define A2M_DEBUG_EVERY(interval_ms, code) \
	do { \
		static uint32_t _a2m_debug_last_ms = 0; \
		const uint32_t _a2m_debug_now_ms = millis(); \
		if (_a2m_debug_last_ms == 0 || \
		    static_cast<uint32_t>(_a2m_debug_now_ms - _a2m_debug_last_ms) >= static_cast<uint32_t>(interval_ms)) { \
			_a2m_debug_last_ms = _a2m_debug_now_ms; \
			code; \
		} \
	} while (0)

#else

#define A2M_DEBUG_LINE(literal) \
	do { \
	} while (0)

#define A2M_DEBUG_PRINTF(fmt, ...) \
	do { \
	} while (0)

#define A2M_DEBUG_ONCE(code) \
	do { \
	} while (0)

#define A2M_DEBUG_EVERY(interval_ms, code) \
	do { \
	} while (0)

#endif
