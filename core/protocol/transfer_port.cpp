#include "transfer_port.h"
#include <curl/curl.h>
#include <memory>
namespace immd {
namespace {
using Url=std::unique_ptr<CURLU,decltype(&curl_url_cleanup)>;
struct Free{void operator()(char* p)const{curl_free(p);}};
std::string part(CURLU* u,CURLUPart p,unsigned flags=0){char* out=nullptr;if(curl_url_get(u,p,&out,flags)!=CURLUE_OK)return {};std::unique_ptr<char,Free> holder(out);return out;}
bool present(CURLU* u,CURLUPart p){char* out=nullptr;auto code=curl_url_get(u,p,&out,0);curl_free(out);return code==CURLUE_OK;}
std::string lower(std::string s){for(auto& c:s)if(c>='A'&&c<='Z')c=char(c-'A'+'a');return s;}
std::string origin(const std::string& s,bool origin_only=false){
 if(s.size()>8192 || s.find('#')!=std::string::npos || (origin_only && s.find('?')!=std::string::npos))return {};
 for(unsigned char c:s)if(c<=32||c==127)return {};
 if(s.rfind("http://",0)!=0&&s.rfind("https://",0)!=0)return {};
 const auto start=s.find("://")+3;
 const auto authority=s.substr(start,s.find_first_of("/?#",start)-start);
 // Reject encoded/scoped authorities before libcurl-version-specific parsing.
 if(authority.find('%')!=std::string::npos || authority.find('@')!=std::string::npos)return {};
 Url u(curl_url(),curl_url_cleanup);if(!u||curl_url_set(u.get(),CURLUPART_URL,s.c_str(),0)!=CURLUE_OK)return {};
 if(present(u.get(),CURLUPART_ZONEID)||present(u.get(),CURLUPART_USER)||present(u.get(),CURLUPART_PASSWORD)||present(u.get(),CURLUPART_FRAGMENT))return {};
 if(origin_only&&(present(u.get(),CURLUPART_QUERY)||part(u.get(),CURLUPART_PATH)!="/"))return {};
 auto host=lower(part(u.get(),CURLUPART_HOST));auto port=part(u.get(),CURLUPART_PORT,CURLU_DEFAULT_PORT);
 if(host.empty()||port.empty())return {};
 return lower(part(u.get(),CURLUPART_SCHEME))+"://"+host+":"+port;
}
bool headers_ok(const std::vector<std::string>& headers){
 if(headers.size()>64)return false;
 size_t total=0;std::vector<std::string> names;
 for(const auto& h:headers){
  if(h.size()>32768)return false;
  total+=h.size();if(total>32768)return false;
  for(unsigned char c:h)if(c<32||c==127)return false;
  auto colon=h.find(':');if(colon==0||colon==std::string::npos)return false;
  auto name=lower(h.substr(0,colon));
  for(char c:name)if(!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'))return false;
  for(const auto* reserved:{"host","content-length","transfer-encoding","range","content-range","x-part-crc32c","expect","content-encoding","connection","te","trailer","upgrade","proxy-authorization","proxy-connection"})if(name==reserved)return false;
  for(const auto& prior:names)if(prior==name)return false;
  names.push_back(std::move(name));
 }
 return true;
}
Error rejected(){return make_err(Err::Protocol,"transfer adapter rejected");}
}
bool valid_transfer_origins(const std::vector<std::string>& origins){
 try{if(origins.empty()||origins.size()>32)return false;for(const auto& s:origins)if(origin(s,true).empty())return false;return true;}catch(...){return false;}
}
Expected<TransferTarget> prepare_transfer(const DataPlanePort& port,const std::vector<std::string>& allowed,const TransferRequest& request){
 try{
  const auto source=origin(request.url);if(source.empty()||request.offset<0||request.length<=0)return rejected();
  bool admitted=false;for(const auto& item:allowed)if(origin(item,true)==source){admitted=true;break;}
  if(!admitted)return rejected(); // no host credential callback for untrusted origin
  auto mapped=port.prepare(request);
  if(!mapped.ok()||origin(mapped.value().url)!=source||!headers_ok(mapped.value().headers))return rejected();
  return std::move(mapped.value());
 }catch(...){return rejected();}
}
}
