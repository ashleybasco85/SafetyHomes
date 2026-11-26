#pragma once
#include <Arduino.h>

void beginRtc();
// Unix UTC seconds, or zero when the RTC time is unavailable.
uint32_t rtcEpoch();
void syncRtc(uint32_t epoch);
