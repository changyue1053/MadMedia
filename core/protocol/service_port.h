#pragma once
#include <string>
#include <vector>
#include "core/util/error.h"
namespace immd {
enum class ControlOperation { Init, PartUrls, Complete, Challenge, DownloadUrl, MediaStatus };
struct ControlRequest {std::string method,url,body;std::vector<std::string> headers;};
struct ControlResponse {long status=0;std::string body;};
// Source-level extension only. Pure bounded transforms on the scheduler thread.
// No I/O, blocking, reentry or Engine destruction. Shared immutable lifetime.
class ControlPlanePort {
 public:
 virtual ~ControlPlanePort()=default;
 virtual Expected<ControlRequest> encode(ControlOperation,const ControlRequest&) const=0;
 virtual Expected<ControlResponse> decode(ControlOperation,const ControlResponse&) const=0;
};
// Contains adapter exceptions/errors; never propagates adapter diagnostic text.
Expected<ControlRequest> encode_control(const ControlPlanePort&,ControlOperation,const ControlRequest&);
Expected<ControlResponse> decode_control(const ControlPlanePort&,ControlOperation,const ControlResponse&);
}
