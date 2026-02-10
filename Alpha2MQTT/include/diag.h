#pragma once

#include <stdint.h>

enum DiagMqttState : uint8_t {
	MQTT_DISC = 0,
	MQTT_CONN = 1,
	MQTT_UP = 2
};

struct Diag {
	uint32_t loop_last_ms;
	uint32_t loop_max_gap_ms;

	uint8_t mqtt_state;
	uint32_t mqtt_last_attempt_ms;
	uint16_t mqtt_fail_count;
	int16_t mqtt_last_rc;

	int16_t wifi_status;
	uint32_t wifi_last_change_ms;

	uint32_t rs485_last_poll_ms;
	uint16_t rs485_poll_duration_ms;
	uint16_t rs485_timeout_count;

	uint32_t last_yield_ms;
	uint32_t wdt_feed_count;

	int16_t reset_reason_num;
};

extern Diag g_diag;

void diag_init();
void diag_loop_tick(uint32_t now);
void diag_note_yield(uint32_t now);
void diag_wifi_status(int16_t st, uint32_t now);
void diag_mqtt_attempt(uint32_t now);
void diag_mqtt_result(bool ok, int16_t rc, uint32_t now);
void diag_rs485_poll_begin(uint32_t now);
void diag_rs485_poll_end(uint32_t now, bool timed_out);
