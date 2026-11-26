#include "RtcClock.h"
#include <SoftwareWire.h>

namespace {
constexpr uint8_t ADDRESS = 0x68;
constexpr uint32_t LOCAL_OFFSET = 8UL * 3600; // RTC stores Philippine local time.
SoftwareWire bus(A1, A2, false); // External pull-ups are on the module.

bool readRegisters(uint8_t address, uint8_t* data, uint8_t count) {
  bus.beginTransmission(ADDRESS);
  bus.write(address);
  if (bus.endTransmission() != 0) return false;
  bus.requestFrom(ADDRESS, count);
  if (bus.available() != count) return false;
  for (uint8_t i = 0; i < count; ++i) data[i] = bus.read();
  return true;
}

uint8_t fromBcd(uint8_t value) {
  return (value >> 4) * 10 + (value & 15);
}

uint8_t toBcd(uint8_t value) {
  return ((value / 10) << 4) | (value % 10);
}

constexpr uint8_t MONTH_DAYS[]={31,28,31,30,31,30,31,31,30,31,30,31};
uint8_t daysInMonth(uint8_t year,uint8_t month){
  return MONTH_DAYS[month-1]+(month==2 && year%4==0);
}

void writeClock(uint8_t year,uint8_t month,uint8_t day,
                uint8_t hour,uint8_t minute,uint8_t second,uint8_t weekday){
  bus.beginTransmission(ADDRESS);
  bus.write((uint8_t)0);
  bus.write(toBcd(second));
  bus.write(toBcd(minute));
  bus.write(toBcd(hour));
  bus.write(weekday);
  bus.write(toBcd(day));
  bus.write(toBcd(month));
  bus.write(toBcd(year));
  if(bus.endTransmission()!=0)return;

  uint8_t status;
  if(readRegisters(0x0F,&status,1)){
    bus.beginTransmission(ADDRESS);
    bus.write((uint8_t)0x0F);
    bus.write((uint8_t)(status & 0x7F));
    bus.endTransmission();
  }
}
}

void beginRtc() {
  bus.begin();
  // Enable only for one upload, then restore to false and upload again.
  constexpr bool SET_RTC_ON_BOOT = false;
  if (!SET_RTC_ON_BOOT) return;
  const char date[] = __DATE__;
  const char time[] = __TIME__;
  const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  uint8_t month = 1;
  while (month <= 12 && strncmp(date, months + 3 * (month - 1), 3)) ++month;
  if (month > 12) return;

  writeClock(atoi(date+7)%100,month,atoi(date+4),atoi(time),
             atoi(time+3),atoi(time+6),1);
}

void syncRtc(uint32_t epoch){
  // Only accept dates this firmware can read back, in 2024–2099 local time.
  if(epoch<1704038400UL || epoch>=4102416000UL)return;
  const uint32_t current=rtcEpoch();
  const uint32_t difference=current>epoch ? current-epoch : epoch-current;
  if(current && difference<=2)return;

  const uint32_t local=epoch+LOCAL_OFFSET;
  const uint32_t dayNumber=local/86400UL;
  uint32_t days=dayNumber-10957UL;
  uint8_t year=0;
  while(days>=365U+(year%4==0)){
    days-=365U+(year%4==0);
    ++year;
  }
  uint8_t month=1;
  while(days>=daysInMonth(year,month)){
    days-=daysInMonth(year,month);
    ++month;
  }
  const uint32_t seconds=local%86400UL;
  writeClock(year,month,days+1,seconds/3600UL,(seconds/60UL)%60,seconds%60,
             ((dayNumber+4)%7)+1);
}

uint32_t rtcEpoch() {
  uint8_t status;
  uint8_t raw[7];
  if (!readRegisters(0x0F, &status, 1) || (status & 0x80) ||
      !readRegisters(0, raw, sizeof(raw))) return 0;

  const uint8_t second = fromBcd(raw[0] & 0x7F);
  const uint8_t minute = fromBcd(raw[1] & 0x7F);
  const uint8_t hour = (raw[2] & 0x40)
      ? fromBcd(raw[2] & 0x1F) % 12 + ((raw[2] & 0x20) ? 12 : 0)
      : fromBcd(raw[2] & 0x3F);
  const uint8_t day = fromBcd(raw[4] & 0x3F);
  const uint8_t month = fromBcd(raw[5] & 0x1F);
  const uint8_t year = fromBcd(raw[6]); // Offset from 2000.
  if (year < 24 || year > 99 || (raw[5] & 0x80) || month < 1 || month > 12 ||
      day < 1 || hour > 23 || minute > 59 || second > 59) return 0;

  if (day > daysInMonth(year,month)) return 0;
  uint32_t days = 10957UL + 365UL * year + (year + 3) / 4; // Days since 1970.
  for (uint8_t m = 1; m < month; ++m) days += daysInMonth(year,m);
  return (days + day - 1) * 86400UL + hour * 3600UL + minute * 60UL + second - LOCAL_OFFSET;
}
