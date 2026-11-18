#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "core/util/error.h"
namespace immd {
enum class TransferOperation { UploadPart, DownloadPart };
struct TransferRequest {TransferOperation operation;std::string url;int64_t offset=0,length=0;};
struct TransferTarget {std::string url;std::vector<std::string> headers;};
class DataPlanePort {
 public:
 virtual ~DataPlanePort()=default;
 // Pure, prompt-return transform. Read credentials from a thread-safe snapshot.
 // Cannot replace method, file range, storage path or native integrity headers.
 virtual Expected<TransferTarget> prepare(const TransferRequest&) const=0;
};
bool valid_transfer_origins(const std::vector<std::string>&);
Expected<TransferTarget> prepare_transfer(const DataPlanePort&,const std::vector<std::string>& allowed_origins,const TransferRequest&);
}
