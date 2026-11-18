#include "core/engine/callback_fault.h"
namespace immd::detail {
namespace { thread_local uint8_t* current_faults=nullptr; }
CallbackFaultScope::CallbackFaultScope() noexcept : previous_(current_faults){current_faults=&flags_;}
CallbackFaultScope::~CallbackFaultScope(){current_faults=previous_;}
void report_callback_fault(bool observer) noexcept {
 if(current_faults)*current_faults|=observer?2u:1u;
}
} // namespace immd::detail
