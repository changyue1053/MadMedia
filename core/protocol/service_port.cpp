#include "service_port.h"
namespace immd {
namespace {
constexpr size_t kBody=1024*1024;
Error rejected(){return make_err(Err::Protocol,"control adapter rejected");}
bool safe(const std::string& s){for(unsigned char c:s)if(c<32 || c==127)return false;return true;}
bool valid(const ControlRequest& r){
 if((r.method!="GET" && r.method!="POST") || r.body.size()>kBody || r.url.size()>8192 || r.url.find(' ')!=std::string::npos || !safe(r.url))return false;
 if(r.url.rfind("https://",0)!=0 && r.url.rfind("http://",0)!=0)return false;
 auto start=r.url.find("://")+3;auto end=r.url.find_first_of("/?#",start);
 auto authority=r.url.substr(start,end-start);
 if(authority.empty() || authority.find('@')!=std::string::npos || r.url.find('#')!=std::string::npos)return false;
 if(r.headers.size()>64)return false;
 size_t bytes=0;
 for(const auto& h:r.headers){
  if(h.size()>32768 || !safe(h))return false;
  bytes+=h.size();auto colon=h.find(':');
  if(bytes>32768 || colon==0 || colon==std::string::npos)return false;
  std::string name=h.substr(0,colon);
  for(char& c:name){if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'))return false;if(c>='A'&&c<='Z')c=char(c-'A'+'a');}
  if(name=="host" || name=="content-length" || name=="transfer-encoding")return false;
 }
 return r.method!="GET" || r.body.empty();
}
}
Expected<ControlRequest> encode_control(const ControlPlanePort& p,ControlOperation op,const ControlRequest& r){
 try{auto v=p.encode(op,r);if(!v.ok() || !valid(v.value()))return rejected();return std::move(v.value());}
 catch(...){return rejected();}
}
Expected<ControlResponse> decode_control(const ControlPlanePort& p,ControlOperation op,const ControlResponse& r){
 try{
  if(r.body.size()>16u*1024*1024)return rejected();
  auto v=p.decode(op,r);
  if(!v.ok() || v.value().body.size()>kBody || v.value().status<100 || v.value().status>599)return rejected();
  return std::move(v.value());
 }catch(...){return rejected();}
}
}
