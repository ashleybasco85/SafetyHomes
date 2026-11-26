#include "Sensors.h"
#include "Config.h"
SafetySensors::SafetySensors():dht(DHT_PIN,DHT22){}
void SafetySensors::begin(){
  pinMode(SENSOR_POWER_PIN,OUTPUT);
  pinMode(SMOKE_PIN,INPUT);
  digitalWrite(SMOKE_PIN,LOW);
  pinMode(SMOKE_DO_PIN,INPUT);
  digitalWrite(SMOKE_DO_PIN,LOW);
  gasStartedAt=millis();
  powerOn();
}
void SafetySensors::powerOn(){
  digitalWrite(SENSOR_POWER_PIN,HIGH); powered=true;poweredAt=millis();
  temperature=humidity=distance=NAN;
  pinMode(TRIG_PIN,OUTPUT);digitalWrite(TRIG_PIN,LOW);
  pinMode(ECHO_PIN,INPUT);digitalWrite(ECHO_PIN,LOW);
  lastDhtRead=poweredAt;
  dht.begin();
}
void SafetySensors::powerOff(){
  // Release signal lines before Q1 opens the DHT22 and HC-SR04 ground.
  const uint8_t signals[]={DHT_PIN,TRIG_PIN,ECHO_PIN};
  for(uint8_t p:signals){
    pinMode(p,INPUT);digitalWrite(p,LOW); // no pullups
  }
  digitalWrite(SENSOR_POWER_PIN,LOW);powered=false;
}
bool SafetySensors::ready()const{return powered && millis()-poweredAt>=DHT_STARTUP_MS;}
void SafetySensors::read(){
  if(!ready()){temperature=humidity=distance=NAN;return;}
  const unsigned long now=millis();
  if(now-lastDhtRead>=DHT_READ_INTERVAL_MS){
    lastDhtRead=now;
    temperature=dht.readTemperature();
    humidity=dht.readHumidity();
  }
  digitalWrite(TRIG_PIN,LOW);delayMicroseconds(2);
  digitalWrite(TRIG_PIN,HIGH);delayMicroseconds(10);digitalWrite(TRIG_PIN,LOW);
  unsigned long echo=pulseIn(ECHO_PIN,HIGH,25000UL); // bounded synchronous measurement, <=25ms
  distance=echo ? echo*0.0343f/2.0f : NAN;
}
void SafetySensors::readSmoke(){smokeRaw=analogRead(SMOKE_PIN);}
bool SafetySensors::smokeAlarm()const{
  return millis()-gasStartedAt>=GAS_SETTLE_MS && (SMOKE_HIGH_IS_ALARM?smokeRaw>=SMOKE_THRESHOLD:smokeRaw<=SMOKE_THRESHOLD);
}
bool SafetySensors::intrusion()const{return isfinite(distance) && distance>0 && distance<=INTRUSION_CM;}
static String numeric(float value){return isfinite(value)?String(value,1):String("null");}
void SafetySensors::appendJson(String& s)const{
  s+=F("\"temperature\":");s+=numeric(temperature);s+=F(",\"humidity\":");s+=numeric(humidity);
  s+=F(",\"distance\":");s+=numeric(distance);s+=F(",\"smoke_raw\":");s+=smokeRaw;
  s+=F(",\"dht_ok\":");s+=(isfinite(temperature)&&isfinite(humidity))?F("true"):F("false");
  s+=F(",\"smoke\":\"");
  s+=millis()-gasStartedAt<GAS_SETTLE_MS?"Warming up":smokeAlarm()?"Detected!":"Normal";
  s+=F("\"");
}
