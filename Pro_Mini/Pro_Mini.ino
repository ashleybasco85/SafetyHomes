#include "Config.h"
#include "Sensors.h"
#include "Communication.h"
#include "RtcClock.h"
#include <avr/sleep.h>
#include <avr/power.h>
#include <avr/interrupt.h>

enum SystemMode { AUTO, MAINTENANCE, SLEEP_MODE };
SystemMode mode=AUTO;
SafetySensors sensors;
Communication link;
bool buzzer=false,fan=false;
bool sleepPending=false,sleepReported=false;
unsigned long lastRead=0,lastHeartbeat=0,lastSmokeRead=0;
unsigned long lastCommand=0;
bool lastOK=true;
const char* lastMessage="boot";
volatile bool wakeFlag=false;

const char* modeName(){return mode==AUTO?"AUTO":mode==MAINTENANCE?"MAINTENANCE":"SLEEP";}
void wakeISR(){wakeFlag=true;EIMSK &= ~_BV(INT0);}
void setBuzzer(bool on){buzzer=on;digitalWrite(BUZZER_PIN,on?HIGH:LOW);}
void setFan(bool on){fan=on;digitalWrite(FAN_PIN,on?HIGH:LOW);}
void report(const char* kind,unsigned long ack=0,bool ok=true,const char* message="ok"){
  String s;s.reserve(395);
  s=F("{\"kind\":\"");s+=kind;s+=F("\",\"mode\":\"");s+=modeName();
  s+=F("\",\"ack\":");s+=ack;s+=F(",\"ok\":");s+=ok?F("true"):F("false");
  s+=F(",\"message\":\"");s+=message;s+=F("\",\"actuator\":\"");s+=buzzer?F("ON"):F("OFF");
  s+=F("\",\"fan\":\"");s+=fan?F("ON"):F("OFF");
  s+=F("\",\"power\":");s+=sleepReported?F("false"):F("true");s+=',';
  sensors.appendJson(s);
  s+=F(",\"rtc_epoch\":");s+=rtcEpoch();
  s+='}';link.send(s);
}
void restoreRail(){
  sensors.powerOn();
}
void setup(){
  link.begin(SERIAL_BAUD);
  pinMode(WAKE_PIN,INPUT);digitalWrite(WAKE_PIN,LOW); // no AVR 5V pull-up
  attachInterrupt(digitalPinToInterrupt(WAKE_PIN),wakeISR,LOW);
  EIMSK &= ~_BV(INT0);
  pinMode(BUZZER_PIN,OUTPUT);setBuzzer(false);
  digitalWrite(FAN_PIN,LOW);pinMode(FAN_PIN,OUTPUT);setFan(false);
  const uint8_t unused[]={9,10,11,12,A3,A4,A5};
  for(uint8_t p:unused){pinMode(p,OUTPUT);digitalWrite(p,LOW);}
  ACSR |= _BV(ACD);DIDR0 |= _BV(ADC0D);
  power_spi_disable();power_twi_disable();power_timer2_disable();
  beginRtc();sensors.begin();report("mode",0,true,"boot");
}
void command(char* line){
  constexpr char prefix[]="0|RTC_SYNC|";
  if(!strncmp(line,prefix,sizeof(prefix)-1)){
    const char* value=line+sizeof(prefix)-1;
    char* end=nullptr;
    const unsigned long epoch=strtoul(value,&end,10);
    if(end!=value && !*end)syncRtc(epoch);
    return;
  }
  char* split=strchr(line,'|');if(!split)return;
  *split=0;char* end=nullptr;unsigned long id=strtoul(line,&end,10);
  if(!id || !end || *end)return;
  const char* cmd=split+1;
  if(id==lastCommand){report("ack",id,lastOK,lastMessage);return;}
  bool ok=true;const char* msg="ok";const char* kind="ack";
  if(!strcmp(cmd,"AUTO")||!strcmp(cmd,"WAKE")){
    mode=AUTO;setBuzzer(false);setFan(sensors.smokeAlarm());kind="mode";
  }else if(!strcmp(cmd,"MAINTENANCE")){
    mode=MAINTENANCE;setBuzzer(false);setFan(false);kind="mode";
  }else if(!strcmp(cmd,"SLEEP")){
    if(digitalRead(WAKE_PIN)==LOW){ok=false;msg="wake_pin_low";}
    else{
      mode=SLEEP_MODE;setBuzzer(false);setFan(false);
      sleepPending=true;sleepReported=false;
      lastCommand=id;lastOK=true;lastMessage="sleep_entry";
      return; // Acknowledge after the switched sensor ground is off.
    }
  }else if(!strcmp(cmd,"MANUAL_READ")){
    if(mode!=MAINTENANCE){ok=false;msg="maintenance_required";}
    else if(!sensors.ready()){ok=false;msg="sensors_starting";}
    else{sensors.readSmoke();sensors.read();kind="manual_reading";msg="reading_captured";}
  }else if(!strcmp(cmd,"ACTUATOR_TOGGLE")){
    if(mode!=MAINTENANCE){ok=false;msg="maintenance_required";}else setBuzzer(!buzzer);
  }else if(!strcmp(cmd,"FAN_TOGGLE")){
    if(mode!=MAINTENANCE){ok=false;msg="maintenance_required";}else setFan(!fan);
  }else{ok=false;msg="unknown_command";}
  lastCommand=id;lastOK=ok;lastMessage=msg;report(kind,id,ok,msg);
}
void enterSleep(){
  if(!sleepReported){
    if(!link.idle())return;
    sensors.powerOff();sleepReported=true;
    report("mode",lastCommand,true,"sleep_entry");return;
  }
  if(!link.idle())return; // wait for checksum + newline to leave UART physically
  uint8_t savedADC=ADCSRA;ADCSRA &= ~_BV(ADEN);
  cli();wakeFlag=false;
  EICRA &= ~(_BV(ISC01)|_BV(ISC00)); // LOW-level INT0 works with the I/O clock stopped
  EIFR=_BV(INTF0);EIMSK |= _BV(INT0);
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);sleep_enable();
  if(digitalRead(WAKE_PIN)==HIGH){
    sleep_bod_disable();
    sei();
    sleep_cpu();
  }else sei();
  sleep_disable();EIMSK &= ~_BV(INT0);ADCSRA=savedADC;
  sleepPending=false;sleepReported=false;mode=AUTO;
  restoreRail();lastRead=millis();
  sensors.readSmoke();setFan(sensors.smokeAlarm());
  report("mode",0,true,wakeFlag?"hardware_wake":"wake_line_low");
}
void loop(){
  link.poll();
  if(sleepPending){enterSleep();return;}
  if(millis()-lastSmokeRead>=READ_INTERVAL_MS){
    lastSmokeRead=millis();
    sensors.readSmoke();
    const bool wasOn=fan;
    if(mode==AUTO)setFan(sensors.smokeAlarm());
    if(fan!=wasOn && link.idle())report("outputs");
  }
  char line[80];
  if(link.idle() && link.receive(line,sizeof(line)))command(line);
  if(!link.idle() || sleepPending)return;
  switch(mode){
    case AUTO:
      if(sensors.ready() && millis()-lastRead>=READ_INTERVAL_MS){
        lastRead=millis();sensors.read();setBuzzer(sensors.smokeAlarm()||sensors.intrusion());report("reading");
      }
      break;
    case MAINTENANCE:
      if(millis()-lastHeartbeat>=5000){lastHeartbeat=millis();report("heartbeat");}
      break;
    case SLEEP_MODE:break;
  }
}
