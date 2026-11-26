#pragma once
#include <Arduino.h>
// D7 drives Q1, the low-side ground switch for the DHT22 and HC-SR04.
// Pro Mini 5V / 16MHz; ESP remains powered from unswitched 3.3V.
constexpr uint8_t WAKE_PIN=2, FAN_PIN=3, DHT_PIN=4, SENSOR_POWER_PIN=7;
constexpr uint8_t BUZZER_PIN=8, TRIG_PIN=5, ECHO_PIN=6, SMOKE_PIN=A0, SMOKE_DO_PIN=13; // DO reserved for future digitalRead()
constexpr unsigned long SERIAL_BAUD=38400, READ_INTERVAL_MS=1000;
constexpr unsigned long DHT_READ_INTERVAL_MS=2000;
constexpr unsigned long DHT_STARTUP_MS=2200, GAS_SETTLE_MS=180000;
// GAS_SETTLE_MS is a demonstration inhibit after startup, NOT calibration.
// Condition a new MQ-2 per its datasheet; measure baseline and adjust these values.
constexpr int SMOKE_THRESHOLD=400;
constexpr bool SMOKE_HIGH_IS_ALARM=true; // Typical MQ-2 AO increases with gas.
constexpr int INTRUSION_CM=10;
