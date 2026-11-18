#include "core/scheduler/fsm.h"

namespace immd {

namespace {
constexpr int kN = 8;
// 行 = from，列 = to。表即文档：与架构 §6/§10 状态图逐项对照。
constexpr bool kTable[kN][kN] = {
    //               Queued Prep  Xfer  Final Ready Fail  Pause Cancel
    /* Queued    */ {false, true, false, false, false, true, true, true},
    /* Preparing */ {false, false, true, false, true, true, true, true},   // →Ready = 秒传命中
    /* Transfer  */ {false, false, false, true, false, true, true, true},
    /* Finalizing*/ {false, false, true, false, true, true, false, true},  // →Xfer = complete 发现缺片回补
    /* Ready     */ {false, false, false, false, false, false, false, false},
    /* Failed    */ {true, false, false, false, false, false, false, true},  // 重试回 Queued
    /* Paused    */ {true, false, false, false, false, false, false, true},  // 恢复回 Queued
    /* Canceled  */ {false, false, false, false, false, false, false, false},
};
}  // namespace

bool fsm_can(TaskState from, TaskState to) {
  int f = static_cast<int>(from), t = static_cast<int>(to);
  if (f < 0 || f >= kN || t < 0 || t >= kN) return false;
  return kTable[f][t];
}

bool fsm_terminal(TaskState s) { return s == TaskState::Ready || s == TaskState::Canceled; }

bool fsm_resumable(TaskState s) { return s == TaskState::Failed || s == TaskState::Paused; }

const char* state_name(TaskState s) {
  switch (s) {
    case TaskState::Queued: return "Queued";
    case TaskState::Preparing: return "Preparing";
    case TaskState::Transferring: return "Transferring";
    case TaskState::Finalizing: return "Finalizing";
    case TaskState::Ready: return "Ready";
    case TaskState::Failed: return "Failed";
    case TaskState::Paused: return "Paused";
    case TaskState::Canceled: return "Canceled";
  }
  return "?";
}

const char* kind_name(TaskKind k) { return k == TaskKind::Upload ? "Upload" : "Download"; }

}  // namespace immd

