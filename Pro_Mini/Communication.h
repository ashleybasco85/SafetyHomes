#pragma once
#include <Arduino.h>
class Communication {
 public:
  void begin(unsigned long baud);
  void poll();
  bool receive(char* destination, size_t size);
  bool send(const String& line);
  bool idle() const;
 private:
  char rx[80]={0}, tx[420]={0};
  uint8_t rxPos=0;
  uint16_t txPos=0, txLen=0;
  bool ready=false, dropping=false;
};
