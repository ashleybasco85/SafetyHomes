#include "Communication.h"
#include "Protocol.h"
void Communication::begin(unsigned long baud){ Serial.begin(baud); }
void Communication::poll(){
  while(txPos<txLen && Serial.availableForWrite()>0) Serial.write(tx[txPos++]);
  if(txPos==txLen) txPos=txLen=0;
  while(!ready && Serial.available()){
    char c=Serial.read();
    if(c=='\r') continue;
    if(c=='\n'){
      if(!dropping && rxPos){ rx[rxPos]=0; ready=protocol::verify(rx,rxPos); }
      rxPos=0; dropping=false;
    }else if(!dropping){
      if(rxPos<sizeof(rx)-1) rx[rxPos++]=c;
      else { dropping=true; rxPos=0; }
    }
  }
}
bool Communication::receive(char* out,size_t n){
  if(!ready) return false;
  strncpy(out,rx,n-1);out[n-1]=0;ready=false;return true;
}
bool Communication::send(const String& line){
  if(txLen || line.length()+6>=sizeof(tx)) return false;
  uint16_t crc=protocol::crc16(line.c_str(),line.length());
  memcpy(tx,line.c_str(),line.length());txLen=line.length();
  const char* hex="0123456789ABCDEF";tx[txLen++]='*';
  for(int8_t shift=12;shift>=0;shift-=4)tx[txLen++]=hex[(crc>>shift)&15];
  tx[txLen++]='\n';txPos=0;return true;
}
bool Communication::idle()const{
  // availableForWrite does not include the byte still in the UART shift register.
  return !txLen && (UCSR0A & _BV(TXC0));
}
