#pragma once
#include <Arduino.h>
#include <ESPAsyncTCP.h>
// One asynchronous HTTP/1.0 request at a time. No blocking HTTPClient.GET/POST.
class Communication {
 public:
  void begin();
  bool start(const char* host,uint16_t port,const String& method,const String& path,const String& body,const char* key);
  void poll();
  bool busy=false,done=false;
  int status=0;
  String body;
 private:
  AsyncClient client;
  String request,response;
  unsigned long started=0;
};
