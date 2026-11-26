#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <EEPROM.h>
#include <LittleFS.h>
#include <ArduinoJson.h> // version 6.21.5
#include <time.h>
#include <sys/time.h>
extern "C" {
#include <user_interface.h>
}
#include "Config.h"
#include "Communication.h"
#include "OfflineStore.h"
#include "Protocol.h"

struct Settings {uint32_t magic;char host[40];char key[65];uint8_t fsInitialized;};
Settings settings;
WiFiManager wm;
WiFiManagerParameter hostParam("server","Python PC IPv4 (no http or port)","",40);
WiFiManagerParameter keyParam("key","Device key (must match Python)","",65);
Communication net;
OfflineStore disk;
bool bootConfigAccepted=false,savePending=false,fsOK=false,queueOK=false,webPortalStarted=false;
String serialLine,latest="",pendingWire="",bootID,activePacket;
constexpr uint8_t RAM_SLOTS=4;
String pendingLogs[RAM_SLOTS];
uint8_t ramHead=0,ramCount=0;
unsigned long seq=0,lastLog=0,lastPoll=0,lastLive=0,lastSend=0,lastFrame=0;
unsigned long lastRetry=0,wakeAt=0,commandAt=0,awaiting=0,ackID=0;
bool firstLog=true,wakeLow=false,wakeSettling=false,isWakeCommand=false,ackOK=true;
String ackMessage="";
uint32_t dropped=0,storageErrors=0,uartErrors=0;
enum RequestType{NOTHING,POLL,QUEUE,LIVE,RAM_LOG};RequestType active=NOTHING;
void saveSettings(){EEPROM.put(0,settings);EEPROM.commit();}
void configure(){
  EEPROM.begin(512);EEPROM.get(0,settings);
  if(settings.magic!=0x53485232){
    memset(&settings,0,sizeof(settings));settings.magic=0x53485232;
    strlcpy(settings.key,DEFAULT_DEVICE_KEY,sizeof(settings.key));saveSettings();
  }
  settings.host[39]=0;settings.key[64]=0;
  wm.setDebugOutput(false);
  hostParam.setValue(settings.host,sizeof(settings.host));keyParam.setValue(settings.key,sizeof(settings.key));
  wm.addParameter(&hostParam);wm.addParameter(&keyParam);
  wm.setSaveParamsCallback([](){savePending=true;});
  wm.setConfigPortalBlocking(false);wm.setConfigPortalTimeout(0);wm.setConnectTimeout(10);
  wm.setSaveConfigCallback([](){
    bootConfigAccepted=true;
    savePending=true;
    WiFi.setAutoReconnect(true);
  });
  WiFi.setAutoReconnect(false);
  wm.startConfigPortal("SafetyHomes_Setup","SafetySetup1");
}
void initStorage(){
  LittleFSConfig cfg;cfg.setAutoFormat(false);LittleFS.setConfig(cfg);
  fsOK=LittleFS.begin();
  if(!fsOK && !settings.fsInitialized)fsOK=LittleFS.format() && LittleFS.begin();
  if(fsOK){
    if(!settings.fsInitialized){settings.fsInitialized=1;saveSettings();}
    queueOK=disk.begin();
    // Never erase/reformat an initialized store as a recovery shortcut.
    if(!queueOK)storageErrors++;
  }else storageErrors++;
}
String envelope(const String& data,bool log,bool persistent){
  StaticJsonDocument<1024> doc;StaticJsonDocument<512> parsed;
  if(deserializeJson(parsed,data))return "";
  doc["data"].set(parsed.as<JsonObject>());
  doc["id"]=bootID+"-"+String(++seq);doc["boot"]=bootID;doc["tick"]=millis();
  const uint32_t rtc=parsed["rtc_epoch"] | 0UL;
  time_t now=time(nullptr);
  doc["epoch"]=rtc>1700000000UL ? rtc : (now>1700000000 ? now : 0);
  doc["log"]=log;doc["durable"]=persistent;
  String out;serializeJson(doc,out);return out;
}
void acceptFrame(const String& frame){
  StaticJsonDocument<512> d;if(deserializeJson(d,frame)){uartErrors++;return;}
  const char* mode=d["mode"]|"";const char* kind=d["kind"]|"";
  if(strcmp(mode,"AUTO")&&strcmp(mode,"MAINTENANCE")&&strcmp(mode,"SLEEP"))return;
  if(d["ack"].as<unsigned long>()){
    ackID=d["ack"].as<unsigned long>();ackOK=d["ok"]|false;ackMessage=d["message"].as<String>();
  }
  if(awaiting && d["ack"].as<unsigned long>()==awaiting){awaiting=0;pendingWire="";}
  lastFrame=millis();
  static unsigned long lastAutoAttempt=0;
  static bool attemptedAuto=false;
  const bool autoLog=!strcmp(mode,"AUTO") && !strcmp(kind,"reading") && (firstLog||millis()-lastLog>=LOG_INTERVAL_MS)
      && (!attemptedAuto || millis()-lastAutoAttempt>=10000UL);
  if(autoLog){attemptedAuto=true;lastAutoAttempt=millis();}
  const bool manualLog=!strcmp(mode,"MAINTENANCE") && !strcmp(kind,"manual_reading");
  const bool log=autoLog || manualLog;
  bool persistent=log || !strcmp(kind,"mode") || d["ack"].as<unsigned long>();
  if(persistent){
    String packet=envelope(frame,log,true);
    // New history stays behind any pending RAM events to preserve order.
    bool saved=packet.length() && !ramCount && fsOK && queueOK && disk.append(packet);
    if(!saved){
      if(!ramCount)storageErrors++;
      if((log || !strcmp(kind,"mode")) && packet.length() && ramCount<RAM_SLOTS){
        pendingLogs[(ramHead+ramCount)%RAM_SLOTS]=packet;
        ++ramCount;
        saved=true;
      }else if(log || !strcmp(kind,"mode") || !ramCount){
        dropped++;
        if(manualLog){ackOK=false;ackMessage="reading_storage_failed";}
      }
    }
    if(autoLog && saved){firstLog=false;lastLog=millis();}
  }
  latest=envelope(frame,false,false);
}
void uartPoll(){
  static bool discard=false;
  while(Serial.available()){
    char c=Serial.read();
    if(c=='\n'){
      if(!discard && serialLine.length()){
        char line[426];serialLine.toCharArray(line,sizeof(line));
        if(protocol::verify(line,strlen(line)) && line[0]=='{')acceptFrame(String(line));else uartErrors++;
      }
      serialLine="";discard=false;
    }else if(c!='\r' && !discard){
      if(serialLine.length()<425)serialLine+=c;else{discard=true;serialLine="";uartErrors++;}
    }
  }
}
void writeWire(const String& payload){
  uint16_t crc=protocol::crc16(payload.c_str(),payload.length());char tail[7];
  snprintf(tail,sizeof(tail),"*%04X\n",crc);
  Serial.print(payload);Serial.print(tail);
}
void sendWire(){writeWire(pendingWire);lastRetry=millis();}
void startWake(){digitalWrite(WAKE_PIN,LOW);pinMode(WAKE_PIN,OUTPUT);wakeLow=true;wakeAt=millis();}
void dispatch(const String& wire){
  int sep=wire.indexOf('|');if(sep<1)return;
  unsigned long id=wire.substring(0,sep).toInt();String cmd=wire.substring(sep+1);cmd.trim();
  if(!id||cmd=="NONE" || (awaiting && id==awaiting))return;
  awaiting=id;pendingWire=String(id)+"|"+cmd;commandAt=millis();
  isWakeCommand=cmd=="WAKE";
  if(isWakeCommand)startWake();else sendWire();
}
void wakePoll(){
  if(wakeLow && millis()-wakeAt>=WAKE_LOW_MS){pinMode(WAKE_PIN,INPUT_PULLUP);wakeLow=false;wakeSettling=true;}
  if(wakeSettling && millis()-wakeAt>=WAKE_SETTLE_MS){wakeSettling=false;if(awaiting)sendWire();}
  if(awaiting && millis()-commandAt>=28000){awaiting=0;pendingWire="";}
  if(awaiting && !wakeLow && !wakeSettling && millis()-lastRetry>=1500){if(isWakeCommand)startWake();else sendWire();}
}
String historyPacket(const String& stored){
  StaticJsonDocument<1024> d;if(deserializeJson(d,stored)){storageErrors++;return "";}
  if(d["epoch"].as<long>()<1700000000 && d["boot"].as<String>()==bootID && time(nullptr)>1700000000){
    d["epoch"]=time(nullptr)-(millis()-d["tick"].as<uint32_t>())/1000;
  }
  d["backlog"]=true;String out;serializeJson(d,out);return out;
}
void setup(){
  pinMode(WAKE_PIN,INPUT_PULLUP);Serial.begin(SERIAL_BAUD);Serial.setRxBufferSize(1024);serialLine.reserve(426);
  bootID=String(ESP.getChipId(),HEX)+"-"+String(os_random(),HEX);
  configure();initStorage();net.begin();
}
void loop(){
  uartPoll();wakePoll();wm.process();net.poll();
  if(!bootConfigAccepted){
    if(!wm.getConfigPortalActive())wm.startConfigPortal("SafetyHomes_Setup","SafetySetup1");
    return;
  }
  if(WiFi.status()==WL_CONNECTED && !wm.getConfigPortalActive() && !webPortalStarted){wm.startWebPortal();webPortalStarted=true;}
  if(savePending){savePending=false;strlcpy(settings.host,hostParam.getValue(),sizeof(settings.host));strlcpy(settings.key,keyParam.getValue(),sizeof(settings.key));saveSettings();}
  if(net.done){
    bool success=net.status==200;
    if((active==QUEUE || active==RAM_LOG) && success){
      StaticJsonDocument<64> response;
      if(!deserializeJson(response,net.body) && response["ok"].as<bool>()){
        if(active==RAM_LOG){
          pendingLogs[ramHead]="";
          ramHead=(ramHead+1)%RAM_SLOTS;
          --ramCount;
        }
        else if(!disk.acknowledge())storageErrors++;
      }
    }
    if(active==POLL && success){
      int split=net.body.indexOf('|');if(split>0){
        const time_t epoch=net.body.substring(0,split).toInt();
        const String wire=net.body.substring(split+1);
        dispatch(wire);
        static unsigned long lastRtcSync=0;
        if(epoch>1700000000){
          timeval tv={epoch,0};settimeofday(&tv,nullptr);
          if(!awaiting && !wakeLow && !wakeSettling &&
             (!lastRtcSync || millis()-lastRtcSync>=30000UL)){
            writeWire(String("0|RTC_SYNC|")+String(static_cast<unsigned long>(epoch)));
            lastRtcSync=millis();
          }
        }
      }
    }
    net.done=false;active=NOTHING;lastSend=millis();
  }
  if(net.busy || WiFi.status()!=WL_CONNECTED || !strlen(settings.host) || millis()-lastSend<100)return;
  // Rotate among eligible requests so slow command/live traffic cannot
  // indefinitely postpone the flash queue (history and sleep/wake events).
  static uint8_t nextRequest=0;
  for(uint8_t offset=0;offset<3;++offset){
    const uint8_t slot=(nextRequest+offset)%3;
    if(slot==0){
      if(millis()-lastPoll<1000)continue;
      lastPoll=millis();active=POLL;
      net.start(settings.host,8080,"GET","/api/get_command","",settings.key);
    }else if(slot==1){
      if(millis()-lastLive<1000 || !latest.length())continue;
      lastLive=millis();active=LIVE;StaticJsonDocument<1024> d;deserializeJson(d,latest);
      d["id"]=bootID+"-"+String(++seq);d["log"]=false;d["durable"]=false;d["data"]["kind"]="heartbeat";
      d["data"]["ack"]=ackID;d["data"]["ok"]=ackOK;d["data"]["message"]=ackMessage;
      d["mcu_age_ms"]=millis()-lastFrame;d["storage_ok"]=fsOK&&queueOK;d["queued_records"]=disk.queue.count()+ramCount;
      d["dropped"]=dropped;d["storage_errors"]=storageErrors+disk.queue.errors;d["recovered"]=disk.queue.recovered;d["uart_errors"]=uartErrors;
      String payload;serializeJson(d,payload);net.start(settings.host,8080,"POST","/api/update",payload,settings.key);
    }else{
      String stored;
      activePacket="";
      if(fsOK && queueOK && disk.queue.count() && disk.front(stored)){
        activePacket=historyPacket(stored);
      }
      if(activePacket.length())active=QUEUE;
      else if(ramCount){
        activePacket=historyPacket(pendingLogs[ramHead]);
        if(!activePacket.length())continue;
        active=RAM_LOG;
      }else continue;
      net.start(settings.host,8080,"POST","/api/update",activePacket,settings.key);
    }
    nextRequest=(slot+1)%3;
    break;
  }
  yield();
}
