#pragma once
#include <LittleFS.h>
#include "RingQueueCore.h"
constexpr uint32_t QUEUE_SLOTS=896; // 458752 bytes; 44.8 hours of 3-min samples, minus events
class FlashBackend:public durable::Backend {
 public:
  bool readIndex(durable::Index& index)override{
    File f=LittleFS.open("/r2.index","r");if(!f)return false;
    bool ok=f.readBytes((char*)&index,sizeof(index))==sizeof(index);f.close();return ok;
  }
  bool saveIndex(const durable::Index& index)override{
    File f=LittleFS.open("/r2.index.tmp","w");if(!f)return false;
    bool ok=f.write((const uint8_t*)&index,sizeof(index))==sizeof(index);f.flush();f.close();
    return ok && LittleFS.rename("/r2.index.tmp","/r2.index");
  }
  bool readRecord(uint32_t slot,durable::Header& header,char* payload)override{
    File f=LittleFS.open("/r2.ring","r");if(!f)return false;
    bool ok=f.seek(slot*durable::SLOT,SeekSet) && f.readBytes((char*)&header,sizeof(header))==sizeof(header);
    if(ok && header.magic==durable::MAGIC && header.length<=durable::PAYLOAD && header.length>0)
      ok=f.readBytes(payload,header.length)==header.length;
    else ok=false;
    f.close();return ok;
  }
  bool writeRecord(uint32_t slot,const durable::Header& header,const char* payload)override{
    File f=LittleFS.open("/r2.ring",LittleFS.exists("/r2.ring")?"r+":"w+");if(!f)return false;
    durable::Header pending=header;pending.magic=0;
    const uint32_t offset=slot*durable::SLOT;
    // ESP8266 LittleFS rejects seeking past EOF. Extend old short slots
    // before writing the next record, without changing the on-disk format.
    const size_t end=f.size();
    bool ok=true;
    if(end<offset){
      const uint8_t padding[durable::SLOT]={};
      const size_t gap=offset-end;
      ok=gap<=sizeof(padding) && f.seek(end,SeekSet) && f.write(padding,gap)==gap;
    }
    ok=ok && f.seek(offset,SeekSet);
    if(ok)ok=f.write((const uint8_t*)&pending,sizeof(pending))==sizeof(pending) && f.write((const uint8_t*)payload,header.length)==header.length;
    f.flush(); // Uncommitted slot must reach storage before the commit marker.
    if(ok)ok=f.seek(slot*durable::SLOT,SeekSet) && f.write((const uint8_t*)&header.magic,4)==4;
    f.flush();f.close();return ok;
  }
};
class OfflineStore {
 public:
  FlashBackend backend;durable::Queue queue;
  OfflineStore():queue(backend,QUEUE_SLOTS){}
  bool begin(){return queue.begin();}
  bool append(const String& packet){return queue.enqueue(packet.c_str(),packet.length());}
  bool front(String& packet){char data[durable::PAYLOAD+1];if(!queue.peek(data,sizeof(data)))return false;packet=data;return true;}
  bool acknowledge(){return queue.acknowledge();}
};
