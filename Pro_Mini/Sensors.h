#pragma once
#include <Arduino.h>
#include <DHT.h>
class SafetySensors {
 public:
  SafetySensors();
  void begin();
  void powerOn();
  void powerOff();
  bool ready() const;
  void read();
  void appendJson(String& target) const;
  void readSmoke();
  bool smokeAlarm() const;
  bool intrusion() const;
 private:
  unsigned long gasStartedAt=0;
  DHT dht;
  bool powered=false;
  unsigned long poweredAt=0;
  unsigned long lastDhtRead=0;
  float temperature=NAN,humidity=NAN,distance=NAN;
  int smokeRaw=0;
};
