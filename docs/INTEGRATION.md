# IMMedia 接入指南

面向业务/UI 层的集成手册。IMMedia 是端侧媒体传输内核，负责"字节从选中到可被全会话下载"
的全链路；消息通道是 IMLink 的事，元数据落库是 IMDB 的事（架构 §1）。本指南覆盖当前已落地
的 C++ 接口面；FFI/三端绑定（P8）落地后另出各端语言样例。

> 状态：本指南对应本机可运行的内核（`Engine` C++ API + `immd-cli`）。iOS 后台移交、
> 三端绑定、真实控制面联调的接入步骤标注为「待 P8/联调」。

## 1. 生命周期

```cpp
immd::Config cfg;
cfg.base_url = "https://media.corp.example";  // 控制面（真实/mock 同契约）
cfg.data_dir = "/path/to/account/media";      // 每账号一个目录（登出即整目录清理）
cfg.tenant_id = "t_acme";
cfg.cache_capacity = 512ll * 1024 * 1024;     // 内容寻址下载缓存；0 = 关
// 其余分片/并发/重试/限速参数见 core/scheduler/types.h，全部可云控下发

auto eng = immd::Engine::start(cfg, on_progress);   // on_progress 见 §3
eng.value()->recover();                              // 崩溃恢复：续跑账本内未完成任务
```

- `Engine::start` 失败仅当 `base_url/data_dir` 缺失或 journal 打不开——其余都在运行时通过事件报告。
- 进程重启后必须调用 `recover()`；时机由业务掌握（可等网络就绪后再调，避免开机风暴）。
- 析构即优雅停机：拒新命令、排空在途、取消未完成 HTTP（不清账，下次 `recover` 续跑）。

## 2. 发送一个媒体（选图 → 上传 → 组消息）

```cpp
// 1) 用户选中文件后立即预上传（架构 §4：把传输藏进用户配文字的时间里）
immd::UploadRequest r;
r.file_path = picked_path;      // 内核会在沙箱内做快照副本，原文件可随意
r.mime = "image/jpeg";          // 声明值；内核用魔数嗅探纠偏（防改后缀，§14）
r.priority = immd::Priority::P2_Image;
auto id = eng->upload(r);       // 立即返回 task_id；失败=Err::Busy（队列满，极罕见）

// 2) prepare 阶段内核自动完成（无需业务干预）：
//    - 沙箱快照 + 单遍 SHA-256/分片 CRC
//    - 魔数嗅探纠偏 mime；解码炸弹在上传前拒绝（Failed + Err::Rejected）
//    - video/mp4 尾置 moov → 就地 faststart remux（点开即播，§13）
//    - 秒传两段探测（命中零流量）

// 3) 收到 READY 事件后，用 media_id 组消息交给 IMDB outbox（业务编排层）
//    —— 见 §3 事件流
```

**图片三层产物**（缩略图内联 + 大图 + blurhash 占位）目前由 `image_pipeline` 提供
独立 API（`make_variants`），业务在发送前调用生成 thumb/preview/blurhash，thumb 可内联进
IMLink 消息体（≤6KB），preview 走 media 引用。端上生产用系统解码器时口径一致（P8.2）。

## 3. 事件流（回调线程约定）

`on_progress` 在**内核回调线程**触发，业务需自行切主线程（与 IMLink/IMDB 同约定）。

```cpp
auto on_progress = [](const immd::ProgressEvent& ev) {
  switch (ev.state) {
    case immd::TaskState::Transferring:  // ev.bytes_done/bytes_total：进度条（已节流，≥100ms）
    case immd::TaskState::Ready:         // ev.media_id 就绪 → 组消息 enqueue 到 IMDB outbox
    case immd::TaskState::Failed:        // ev.error 是分类错误码，按 §4 决定 UI
    default: break;
  }
};
```

进度按**服务端确认的分片字节**计，不是写进 socket 的字节（不做虚假 99%，§24 坑 13）。

## 4. 错误分类 → UI 动作（REJECTED 联动是重点）

| `ev.error` | 含义 | 建议 UI/编排动作 |
|---|---|---|
| `Rejected` | 安全策略拦截（解码炸弹/病毒/DLP/审核） | **消息侧撤回 + 提示"被安全策略拦截"**；不重试 |
| `Integrity` | 下载校验失败 / 源文件上传中被改写 | 自动重下/重传；持续失败上报 |
| `Timeout` / `Network` | 弱网/中断 | 内核已自动重试；耗尽预算后 UI 给"重试"按钮，`resume(task_id)` 从断点续传 |
| `Canceled` | 用户取消 | 无需动作 |

**合规窗口两档**（架构 §6，`cfg.ready_on_uploaded`）：
- 普通租户 `true`：UPLOADED 即 READY，消息秒发；若事后审出问题，服务端推 REJECTED，
  业务收到后撤回消息 + 告警。
- 高敏租户 `false`：等流水线 AVAILABLE 才 READY，UI 显示"安全检查中"。

## 5. 运行时控制

```cpp
eng->pause(task_id);   eng->resume(task_id);   eng->cancel(task_id);
eng->set_rate_limit(bytes_per_sec);  // 让路信号落点：IMLink RTT 劣化时降档，恢复置 0（§7）
```

语音走流式 API（松手即达，§14）：
```cpp
auto vid = eng->stream_begin({.mime="audio/opus", .priority=Priority::P0_Voice});
eng->stream_append(vid, pcm_chunk, len);   // 录制中反复调用，内核边录边传
eng->stream_finish(vid);                    // 松手：只补尾片 + hash + complete
```

## 6. 云控参数消费

`Config` 的分片/并发/重试/压缩档位/预取/H3 开关/限速全部设计为云控下发的落点。
建议业务在 `Engine::start` 前从配置中心拉一次，运行时变更通过重建引擎或
`set_rate_limit` 等运行时 API 生效。坏参数应能小时级回滚——这是端上敢做自适应策略的前提。

## 7. 排障（immd-cli）

```bash
immd-cli up   报告.pdf --base <url> --data <dir>     # 上传，打印 media_id
immd-cli down <media_id> 下载.pdf --base <url> --data <dir>
immd-cli journal --data <dir>                        # 账本现场 dump（状态机/位图/错误）
immd-cli mp4  视频.mp4 [--remux 输出.mp4]            # moov 检视 / faststart 重排（可触发 stco→co64）
immd-cli img  图片.png [--out 前缀]                   # 嗅探/尺寸/GPS/炸弹检查 + 三层产物 + blurhash
```

## 8. 待接入（P7/P8/联调）

- **IMLink 接缝**：媒体状态推送订阅（REJECTED/AVAILABLE）、RTT 让路信号真源、trace_id 贯穿三内核。
- **IMDB 接缝**：READY→outbox 组消息、attachment 元数据 ↔ 缓存文件双向通知（接口已在引擎侧就位）。
- **P8 平台**：C ABI 完整化 + Swift/Kotlin/ArkTS 绑定、系统解码/硬编桥、iOS 后台移交（background URLSession）。
- **服务端联调**：真实控制面须通过与 mock 相同的契约测试集（`tests/e2e_main.cpp` 的 p2_smoke/engine.* 剧本）验收。

