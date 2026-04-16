/*
Name:		RS485Handler.h
Created:	24/Aug/2022
Author:		Daniel Young

This file is part of Alpha2MQTT (A2M) which is released under GNU GENERAL PUBLIC LICENSE.
See file LICENSE or go to https://choosealicense.com/licenses/gpl-3.0/ for full license details.

Notes

Handles Modbus requests and responses in a tidy class separate from main program logic.
*/
#ifndef _RS485Handler_h
#define _RS485Handler_h

#include "Definitions.h"

#if RS485_STUB
#include "RS485HandlerStub.h"
#else

#include <Arduino.h>

#if defined MP_ESP8266
#include <SoftwareSerial.h>
#elif defined MP_ESP32
#include <HardwareSerial.h>
#endif

#ifndef A2M_RS485_LAST_TRANSACTION_STATS_DEFINED
#define A2M_RS485_LAST_TRANSACTION_STATS_DEFINED
struct Rs485LastTransactionStats {
	uint16_t quietMs = 0;
	uint16_t waitMs = 0;
	uint8_t attempts = 0;
	uint8_t retries = 0;
};
#endif

// SoftwareSerial is used to create a second serial port, which will be deidcated to RS485.
// The built-in serial port remains available for flashing and debugging.

#define RS485_TX HIGH						// Transmit control pin goes high
#define RS485_RX LOW						// Receive control pin goes low
#define DEFAULT_BAUD_RATE 9600				// Just a default, Alpha2MQTT will cycle baud rates until one is found

#if defined MP_ESP8266
#define SERIAL_COMMUNICATION_CONTROL_PIN D5	// Transmission set pin
#define RX_PIN D6							// Serial Receive pin
#define TX_PIN D7							// Serial Transmit pin
#elif defined MP_ESP32
#if defined(MP_XIAO_ESP32C6)
#define SERIAL_COMMUNICATION_CONTROL_PIN D8	// Transmission set pin - GPIO21 / D3
#define RX_PIN D7							// Serial Receive pin - GPIO17 / D7
#define TX_PIN D6							// Serial Transmit pin - GPIO16 / D6
#define HW_UART_NUM 0				// Hardware UART
#elif defined(MP_ESPUNO_ESP32C6)
#define SERIAL_COMMUNICATION_CONTROL_PIN 23	// Transmission set pin
#define RX_PIN 4							// Serial Receive pin
#define TX_PIN 5							// Serial Transmit pin
#define HW_UART_NUM 1				// Hardware UART
#else // MP_XIAO_ESP32C6 || MP_ESPUNO_ESP32C6
#define SERIAL_COMMUNICATION_CONTROL_PIN 33	// Transmission set pin
#define RX_PIN 16							// Serial Receive pin
#define TX_PIN 17							// Serial Transmit pin
#define HW_UART_NUM 2				// Hardware UART
#endif // MP_XIAO_ESP32C6
#endif // MP_ESP32

// Ensure RS485 is quiet for this many millis before transmitting to help avoid collisions
#ifndef QUIET_MILLIS_BEFORE_TX
#define QUIET_MILLIS_BEFORE_TX 10
#endif

class RS485Handler
{

	private:
#if defined MP_ESP8266
		SoftwareSerial* _RS485Serial;
#elif defined MP_ESP32
		HardwareSerial* _RS485Serial;
#endif

		char* _debugOutput;
		void flushRS485();
		uint16_t checkRS485IsQuiet();
		modbusRequestAndResponseStatusValues listenResponse(modbusRequestAndResponse* resp, uint16_t *waitMsOut);
		bool checkForData(uint16_t *waitMsOut);
		void (*_serviceHook)() = nullptr;
#ifdef DEBUG_OUTPUT_TX_RX
		void outputFrameToSerial(bool transmit, uint8_t frame[], byte actualFrameSize);
#endif // DEBUG_OUTPUT_TX_RX
		bool _inTransaction = false;
		Rs485LastTransactionStats _lastTransactionStats{};
		unsigned long baudRate;
		bool _rs485IsOnline;
		char uartInfoString[OLED_CHARACTER_WIDTH];

	protected:


	public:
		RS485Handler();
		~RS485Handler();
		modbusRequestAndResponseStatusValues sendModbus(uint8_t frame[], byte actualFrameSize, modbusRequestAndResponse* resp);
		void setServiceHook(void (*hook)());
		bool checkCRC(uint8_t frame[], byte actualFrameSize);
		void calcCRC(uint8_t frame[], byte actualFrameSize);
#if defined(DEBUG_OVER_SERIAL) || defined(DEBUG_LEVEL2) || defined(DEBUG_OUTPUT_TX_RX)
		void setDebugOutput(char* _db);
#endif // DEBUG_OVER_SERIAL || DEBUG_LEVEL2 || DEBUG_OUTPUT_TX_RX
		void setBaudRate(unsigned long baudRate);
		bool isRs485Online();
		bool inTransaction() const { return _inTransaction; }
		const Rs485LastTransactionStats &lastTransactionStats() const { return _lastTransactionStats; }
		char *uartInfo();
};


#endif

#endif // !RS485_STUB
