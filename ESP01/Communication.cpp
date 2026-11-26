#include "Communication.h"
void Communication::begin(){
  client.onConnect([this](void*,AsyncClient* c){c->write(request.c_str(),request.length());},nullptr);
  client.onData([this](void*,AsyncClient* c,void* data,size_t len){
    if(response.length()+len>2048){c->close(true);return;}
    response.concat(static_cast<const char*>(data),len);
  },nullptr);
  client.onDisconnect([this](void*,AsyncClient*){
    if(!busy)return;
    int boundary=response.indexOf("\r\n\r\n");
    status=response.startsWith("HTTP/")?response.substring(9,12).toInt():0;
    body=boundary>=0?response.substring(boundary+4):String();
    busy=false;done=true;
  },nullptr);
  client.onError([this](void*,AsyncClient*,int8_t){status=0;body="";busy=false;done=true;},nullptr);
}
bool Communication::start(const char* host,uint16_t port,const String& method,const String& path,const String& payload,const char* key){
  if(busy||done)return false;
  request=method+" "+path+" HTTP/1.0\r\nHost: "+host+":"+String(port)+"\r\nConnection: close\r\nX-Device-Key: "+key+"\r\n";
  if(method=="POST")request+="Content-Type: application/json\r\nContent-Length: "+String(payload.length())+"\r\n";
  request+="\r\n";request+=payload;
  response="";body="";status=0;busy=true;started=millis();
  if(!client.connect(host,port)){busy=false;done=true;return false;}
  return true;
}
void Communication::poll(){if(busy && millis()-started>=5000){client.abort();busy=false;done=true;status=0;body="";}}
