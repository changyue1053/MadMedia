#pragma once
#include <cstdint>
namespace immd::detail {
// Internal notification-local reporting. No user exception data is retained.
void report_callback_fault(bool observer) noexcept;
class CallbackFaultScope {
 public:
  CallbackFaultScope() noexcept;
  ~CallbackFaultScope();
  CallbackFaultScope(const CallbackFaultScope&)=delete;
  CallbackFaultScope& operator=(const CallbackFaultScope&)=delete;
  uint8_t flags() const noexcept { return flags_; }
 private:
  uint8_t flags_=0;
  uint8_t* previous_=nullptr;
};
constexpr uint64_t callback_fault_count_max=(UINT64_MAX >> 4);
inline uint64_t merge_callback_faults(uint64_t previous,uint8_t sources,bool terminal,bool receipt) noexcept {
 if(!sources)return previous;
 uint64_t flags=sources&3u;
 if(sources&1u){if(terminal)flags|=4u;if(receipt)flags|=8u;}
 const uint64_t count=previous>>4;
 return ((count<callback_fault_count_max?count+1:count)<<4)|(previous&15u)|flags;
}
} // namespace immd::detail
