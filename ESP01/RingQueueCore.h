#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Storage adapter must make writeRecord crash-safe and saveIndex atomic.
// The exact same core is exercised by the host fault-injection tests.
namespace durable {
constexpr uint32_t MAGIC=0x32514853;
constexpr size_t SLOT=512, PAYLOAD=496;
#pragma pack(push,1)
struct Header {uint32_t magic,sequence,crc;uint16_t length,reserved;};
struct Index {uint32_t magic,head,tail,crc;};
#pragma pack(pop)
static_assert(sizeof(Header)==16,"Record header size");
static_assert(sizeof(Index)==16,"Index size");
inline uint32_t crc32(const void* bytes,size_t n){
  uint32_t crc=0xffffffff;const uint8_t* p=(const uint8_t*)bytes;
  while(n--){crc^=*p++;for(uint8_t i=0;i<8;i++)crc=(crc>>1)^((crc&1)?0xedb88320:0);}
  return ~crc;
}
inline uint32_t recordCRC(uint32_t sequence,uint16_t length,const char* data){
  // Cover the payload and the sequence/length metadata.
  uint32_t value=crc32(data,length);
  return value ^ crc32(&sequence,sizeof(sequence)) ^ crc32(&length,sizeof(length));
}
struct Backend {
  virtual bool readIndex(Index&)=0;
  virtual bool saveIndex(const Index&)=0;
  virtual bool readRecord(uint32_t slot,Header&,char* payload)=0;
  virtual bool writeRecord(uint32_t slot,const Header&,const char*)=0;
  virtual ~Backend(){}
};
class Queue {
 public:
  Queue(Backend& b,uint32_t slots):disk(b),capacity(slots){}
  uint32_t head=0,tail=0,errors=0,recovered=0;
  uint32_t count()const{return tail-head;}
  bool begin(){
    Index m;char buffer[PAYLOAD];Header h;
    bool valid=disk.readIndex(m) && m.magic==MAGIC && m.crc==crc32(&m,12) && m.tail>=m.head && m.tail-m.head<=capacity;
    if(valid){head=m.head;tail=m.tail;}
    else{
      // No trustworthy index: replay committed slots, including possibly ACKed
      // ones. The server de-duplicates IDs, so replay is safer than data loss.
      bool any=false;uint32_t smallest=0xffffffff,largest=0;
      for(uint32_t i=0;i<capacity;i++)if(readValid(i,h,buffer)){
        if(h.sequence%capacity!=i)continue;
        any=true;if(h.sequence<smallest)smallest=h.sequence;if(h.sequence>largest)largest=h.sequence;
      }
      head=any?smallest:0;tail=any?largest+1:0;
      if(tail-head>capacity)head=tail-capacity;
      if(any)recovered++;
    }
    // Recover record(s) committed just before a power cut that preceded index save.
    while(count()<capacity && readValid(tail%capacity,h,buffer) && h.sequence==tail){tail++;recovered++;}
    return checkpoint(head,tail);
  }
  bool enqueue(const char* payload,size_t length){
    if(!length || length>PAYLOAD || count()>=capacity)return false;
    Header h={MAGIC,tail,recordCRC(tail,(uint16_t)length,payload),(uint16_t)length,0};
    if(!disk.writeRecord(tail%capacity,h,payload)){errors++;return false;}
    tail++; // Committed data is recoverable even if the index write now fails.
    if(!checkpoint(head,tail))errors++;
    return true;
  }
  bool peek(char* out,size_t size){
    if(!count())return false;
    Header h;
    if(size<PAYLOAD+1 || !readValid(head%capacity,h,out) || h.sequence!=head){errors++;return false;}
    out[h.length]=0;return true;
  }
  bool acknowledge(){
    if(!count())return false;
    // Never advance RAM head when the index write failed: retry the same ID.
    if(!checkpoint(head+1,tail)){errors++;return false;}
    head++;return true;
  }
 private:
  Backend& disk;uint32_t capacity;
  bool readValid(uint32_t slot,Header& h,char* p){
    return disk.readRecord(slot,h,p) && h.magic==MAGIC && h.length>0 && h.length<=PAYLOAD && h.crc==recordCRC(h.sequence,h.length,p);
  }
  bool checkpoint(uint32_t h,uint32_t t){Index m={MAGIC,h,t,0};m.crc=crc32(&m,12);return disk.saveIndex(m);}
};
}
