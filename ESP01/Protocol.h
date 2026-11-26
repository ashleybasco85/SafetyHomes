#pragma once
#include <stdint.h>
#include <stddef.h>
namespace protocol {
inline uint16_t crc16(const char* p,size_t n){
  uint16_t crc=0xffff;
  while(n--){crc^=(uint16_t)(uint8_t)*p++<<8;for(uint8_t i=0;i<8;i++)crc=(crc&0x8000)?(crc<<1)^0x1021:crc<<1;}
  return crc;
}
inline int hexValue(char c){return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:-1;}
inline bool verify(char* line,size_t n){
  if(n<6 || line[n-5]!='*')return false;
  uint16_t got=0;for(size_t i=n-4;i<n;i++){int h=hexValue(line[i]);if(h<0)return false;got=(got<<4)|h;}
  if(got!=crc16(line,n-5))return false;
  line[n-5]=0;return true;
}
}
