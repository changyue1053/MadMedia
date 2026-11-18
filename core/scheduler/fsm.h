// 显式状态机转移表（架构 §10 / 实施计划 P1.2）：非法转移不是 bug 的症状，是 bug 本身——
// 立即上报并拒绝，绝不静默容忍。
#pragma once

#include "core/scheduler/types.h"

namespace immd {

bool fsm_can(TaskState from, TaskState to);
bool fsm_terminal(TaskState s);   // Ready / Canceled：不可再转移
bool fsm_resumable(TaskState s);  // Failed / Paused：保留断点可回 Queued

}  // namespace immd

