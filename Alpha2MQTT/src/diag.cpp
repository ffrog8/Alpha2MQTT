#include "../include/diag.h"

#include <Arduino.h>

#if defined(MP_ESP8266)
#include <ESP8266WiFi.h>
extern "C" {
#include <user_interface.h>
}
#elif defined(MP_ESP32)
#include <WiFi.h>
#include <esp_system.h>
#endif

Diag g_diag = {};

namespace {

static constexpr uint32_t kDiagRateLimitMs = 60000UL;
#if defined(MP_ESP8266)
static constexpr uint32_t kLoopGapThresholdMs = 500UL;
#else
static constexpr uint32_t kLoopGapThresholdMs = 1000UL;
#endif

static uint32_t last_print_gap_ms = 0;
static uint32_t last_print_mqtt_ms = 0;
static uint32_t last_print_wifi_ms = 0;
static uint32_t last_print_rs485_ms = 0;

static bool
shouldPrint(uint32_t now, uint32_t *last)
{
	if (*last != 0 && static_cast<uint32_t>(now - *last) < kDiagRateLimitMs) {
		return false;
	}
	*last = now;
	return true;
}

#if defined(MP_ESP8266)
#define DIAG_PRINTF(fmt, ...) Serial.printf_P(PSTR(fmt), ##__VA_ARGS__)
#else
#define DIAG_PRINTF(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#endif

} // namespace

void
diag_init()
{
	g_diag = {};
	g_diag.mqtt_state = MQTT_DISC;
	g_diag.mqtt_last_rc = 0;
	g_diag.reset_reason_num = -1;

#if defined(MP_ESP32)
	g_diag.reset_reason_num = static_cast<int16_t>(esp_reset_reason());
#elif defined(MP_ESP8266)
	const rst_info *ri = ESP.getResetInfoPtr();
	g_diag.reset_reason_num = ri ? static_cast<int16_t>(ri->reason) : static_cast<int16_t>(-1);
#endif

	const uint32_t now = millis();
	g_diag.loop_last_ms = now;
	g_diag.wifi_status = static_cast<int16_t>(WiFi.status());
	g_diag.wifi_last_change_ms = now;

	DIAG_PRINTF("BOOT rr=%d heap=%u\n", g_diag.reset_reason_num, ESP.getFreeHeap());
}

void
diag_loop_tick(uint32_t now)
{
	const uint32_t last = g_diag.loop_last_ms;
	g_diag.loop_last_ms = now;
	if (last == 0) {
		return;
	}

	const uint32_t gap = now - last;
	if (gap <= g_diag.loop_max_gap_ms) {
		return;
	}

	g_diag.loop_max_gap_ms = gap;
	if (gap < kLoopGapThresholdMs) {
		return;
	}

	if (!shouldPrint(now, &last_print_gap_ms)) {
		return;
	}

	DIAG_PRINTF("GAP max=%lu last=%lu heap=%u wifi=%d mqtt=%u rc=%d rto=%u\n",
	            static_cast<unsigned long>(g_diag.loop_max_gap_ms),
	            static_cast<unsigned long>(gap),
	            ESP.getFreeHeap(),
	            static_cast<int>(g_diag.wifi_status),
	            static_cast<unsigned>(g_diag.mqtt_state),
	            static_cast<int>(g_diag.mqtt_last_rc),
	            static_cast<unsigned>(g_diag.rs485_timeout_count));
}

void
diag_note_yield(uint32_t now)
{
	g_diag.last_yield_ms = now;
	g_diag.wdt_feed_count++;
}

void
diag_wifi_status(int16_t st, uint32_t now)
{
	if (g_diag.wifi_status == st) {
		return;
	}

	g_diag.wifi_status = st;
	g_diag.wifi_last_change_ms = now;
	if (!shouldPrint(now, &last_print_wifi_ms)) {
		return;
	}

	DIAG_PRINTF("WIFI st=%d at=%lu\n",
	            static_cast<int>(st),
	            static_cast<unsigned long>(now));
}

void
diag_mqtt_attempt(uint32_t now)
{
	g_diag.mqtt_state = MQTT_CONN;
	g_diag.mqtt_last_attempt_ms = now;
}

void
diag_mqtt_result(bool ok, int16_t rc, uint32_t now)
{
	g_diag.mqtt_last_rc = rc;
	if (ok) {
		g_diag.mqtt_state = MQTT_UP;
		return;
	}

	g_diag.mqtt_state = MQTT_DISC;
	g_diag.mqtt_fail_count++;
	if (!shouldPrint(now, &last_print_mqtt_ms)) {
		return;
	}

	DIAG_PRINTF("MQTT fail=%u rc=%d at=%lu wifi=%d\n",
	            static_cast<unsigned>(g_diag.mqtt_fail_count),
	            static_cast<int>(g_diag.mqtt_last_rc),
	            static_cast<unsigned long>(now),
	            static_cast<int>(g_diag.wifi_status));
}

void
diag_rs485_poll_begin(uint32_t now)
{
	g_diag.rs485_last_poll_ms = now;
}

void
diag_rs485_poll_end(uint32_t now, bool timed_out)
{
	if (g_diag.rs485_last_poll_ms != 0) {
		const uint32_t duration = now - g_diag.rs485_last_poll_ms;
		g_diag.rs485_poll_duration_ms = duration > UINT16_MAX ? UINT16_MAX : static_cast<uint16_t>(duration);
	} else {
		g_diag.rs485_poll_duration_ms = 0;
	}

	if (!timed_out) {
		return;
	}

	g_diag.rs485_timeout_count++;
	if (!shouldPrint(now, &last_print_rs485_ms)) {
		return;
	}

	DIAG_PRINTF("RS485 rto=%u dur=%u at=%lu\n",
	            static_cast<unsigned>(g_diag.rs485_timeout_count),
	            static_cast<unsigned>(g_diag.rs485_poll_duration_ms),
	            static_cast<unsigned long>(now));
}
