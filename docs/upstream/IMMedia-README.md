# IMMedia — 企业 IM 媒体传输内核（C++17）

《[IMMedia 架构设计](../immedia-architecture.html)》与《[实施计划](../immedia-implementation-plan.html)》的工程落地。
端侧传输内核 + 测试基建：调度 Actor、任务账本、curl multi 传输引擎、三段式协议
（断点续传 / 秒传挑战 / 409 回补）、弱网自适应、优先级调度、下载与内容寻址缓存、
parser 安全模块（MP4 faststart 含 stco→co64 升级 / 图片元数据）**已接入 prepare 管线**、
图片处理参考链（stb：解码/缩放/编码/三层产物/blurhash）。

## 一键构建与测试

```bash
./scripts/bootstrap.sh                              # 配置 + 构建 + 全量测试（52 单测 + 23 e2e 剧本）
./scripts/bootstrap.sh build-asan -DIMMD_ASAN=ON    # ASan + UBSan 矩阵
./scripts/bootstrap.sh build-tsan -DIMMD_TSAN=ON    # TSan 矩阵
./build/immd_e2e --soak 60                          # 缩量长跑：多代硬停/恢复 × 随机操作 × RSS 曲线
./build/immd-bench                                  # 热路径基线（回归门槛 ±10%）
```

依赖：系统 curl / sqlite3（macOS SDK 自带；Linux 需 libcurl/libsqlite3 开发包）、python3（mock 控制面）。
密码学（SHA-256/HMAC/CRC32C）自带实现，NIST/RFC 向量验证。图片参考链用 vendored stb
（`third_party/stb/`，public domain，随仓库；无网络依赖）——生产移动端走系统解码器/libwebp，
stb 是桌面/CI 参考实现，口径对齐。

## 目录（对照架构 §3 分域）

```
core/
  scheduler/   调度域：状态机转移表 · 优先级/预算 · 退避 · EWMA 自适应
  journal/     任务账本：SQLite/WAL，先落账再传输，崩溃即现场
  transport/   传输域：单线程 curl multi（poll+wakeup）· pread/pwrite 流式 · 限速接口
  protocol/    《媒体协议契约 v1》编解码：init/part_urls/complete/challenge/dl_url
  engine/      调度 Actor：任务状态唯一主人，命令+事件驱动（std::variant）
  compute/     计算池：快照拷贝+单遍 hash（SHA-256+分片 CRC32C）· 协作取消
  parser/      安全模块（唯一触碰不可信字节处，fuzz 覆盖）：
                 mp4box    — box 遍历 · moov 检测 · faststart remux（stco→co64 定点升级）
                 imagemeta — 魔数嗅探（含 SVG/HTML XSS 面）· EXIF 方向/GPS · 炸弹预检 · 元数据剥离
  pipeline/image/  图片参考链（stb）：解码 → 缩放 → 编码 · 三层产物 · blurhash
  engine/      prepare 已接线：魔数纠偏 mime · 解码炸弹拒绝 · video/mp4 尾置 moov 就地 remux
  cache/       媒体缓存：内容寻址 + LRU；已接入下载链路（命中零网络，校验后异步回填）
  util/        clock（虚拟时钟注入）· 有界 MPSC · 防御式 JSON · 位图 · fs
third_party/stb/     vendored 单头库（解码/缩放/编码，public domain）；sanitizer 已隔离
testkit/mockctl.py   mock 控制面：契约语义（含流式/带宽整形/断连注入）+ 故障注入 + 零重传统计
tests/               harness（零依赖）· 单测 52 项 · e2e 23 剧本 · soak 模式 · fuzz/（libFuzzer 入口）
tools/immd-cli       up / down / recover / journal / mp4 / img     tools/immd-bench  基线
docs/INTEGRATION.md  接入指南（生命周期/事件流/错误分类/云控/排障）
```

## 已验证的架构承诺（e2e 剧本 ↔ 计划 DoD）

| 剧本 | 验证的承诺 |
|---|---|
| `upload_small_then_dedup_via_challenge` | 秒传 + 持有证明（HMAC 区间挑战），第二次上传零字节 |
| `multipart_upload_survives_injected_5xx` | 分片级重试（全抖动退避），每片最终恰好成功一次 |
| `complete_409_missing_parts_backfill` | complete 幂等：服务端缺片 → 位图回补 → 仅重传缺片 |
| `url_expiry_triggers_refresh` | 凭证过期 403 → 按节奏换新（不烧重试预算），零字节浪费 |
| `crash_recovery_zero_reupload` | 硬停后 init 幂等对账（服务端为真相源）：非在途分片零重传，在途 ≤ 并发窗口 |
| `p0_voice_preempts_big_upload` | P0 语音在 3MB 大文件传输中 <3s 就绪（分片边界抢占 + 预算留量） |
| `chaos_random_kill_faults_converge` | 随机故障 × 随机硬停 × recover → 终态收敛、对象完整（固定种子可复现） |
| `download_roundtrip_and_integrity_check` | Range 分片下载 + 断点 + SHA-256 完整性门 |
| `download_second_time_hits_cache_zero_network` | 内容寻址缓存：二次下载零网络（凭证请求数不变） |
| `pause_resume_keeps_breakpoint` | 暂停保留断点，恢复不重传已完成分片 |
| `sync_review_waits_for_available` | 合规窗口严档（ready_on_available）：等流水线 AVAILABLE 才 READY |
| `rejected_by_policy_fails_with_rejected_code` | REJECTED 联动：事件携带 `Err::Rejected` 分类，业务据此撤回+告警 |
| `cancel_mid_upload_cleans_and_persists_terminal` | 取消即清快照，journal 保留终态供审计 |
| `runtime_rate_limit_takes_effect` | 运行时限速（token bucket 实测生效）——IMLink RTT 让路信号的落点 |
| `voice_streaming_records_then_finishes_subsecond` | 语音边录边传：录制中分片已在途，松手仅补尾片+hash+complete，服务端对象逐字节一致 |
| `voice_stream_cancel_discards_cleanly` | 录音取消即废单，残片零残留 |
| `adaptive_suggestion_feeds_next_task` | 弱网自适应闭环：任务 1 实测吞吐传导进任务 2 的开单建议（冷启动 1MB → 钳位 8MB） |
| `weaknet_mini_matrix` | 带宽整形 / 慢响应 / 半途断连 × 尺寸的矩阵缩量版，弱网下分片账仍平 |
| `mp4_tail_moov_remuxed_in_prepare_pipeline` | **parser 装车**：上传尾置 moov MP4 → prepare 就地 remux → 服务端对象已 faststart |
| `prepare_gate_rejects_bomb_and_corrects_mime` | 解码炸弹上传前拒（零字节出门）· 改后缀 MZ 伪装被魔数纠偏 |
| 单测 `mp4.*` / `image.*` | faststart 字节级验证 + **stco→co64 定点升级**（含级联用例）· cmov 拒绝 · 魔数表 · EXIF 方向/GPS · 解码炸弹 · 元数据剥离 |
| 单测 `image_pipeline.*` | 解码/缩放/编码往返 · 保长宽比不放大 · 三层产物 + blurhash（28 字节）· 解码炸弹二次防线 |

**量化基线**（`immd-bench`，RelWithDebInfo，Apple M 系）：SHA-256 软实现 432 MB/s ·
CRC32C 硬件路径 11.8 GB/s · JSON 解析 2.0M ops/s · journal WAL upsert 76k ops/s ·
端到端上传 **双口径**：17 MB/s（mock CRC 校验开——回归基线）/ **303 MB/s**（校验关——
传输路径本身的量级证明，含快照 + 单遍 hash；瓶颈在 python mock 不在内核）。

**soak（60s）**：99 代引擎硬停/恢复，188 上传 + 47 下载 + 23 暂停 + 13 取消，
全部收敛零失败零悬挂，RSS 增长 +7.7MB。

**消毒剂矩阵**：ASan+UBSan / TSan 全量零报告（52 单测 + 23 e2e ×2）——"状态只在调度线程变更"
是 TSan 证明的机械事实；vendored stb 的 UBSan 噪声已隔离（见勘误 9），本项目自有代码维持全覆盖。

## 相对实施计划的范围与偏差（诚实账）

- **已落地**：P0（骨架/测试基建/mock）、P1（Actor/账本/恢复）、P2（传输引擎）、P3（协议闭环/续传/秒传/自适应
  + 闭环 e2e）、P4（下载器/缓存接入/抢占/暂停取消）、
  **P5.1+5.2+5.3（图片 parser + stb 解码/缩放/编码参考链 + 三层产物 + blurhash）**、
  P6.1（MP4 faststart + stco→co64 定点升级）、P6.5（语音流式，松手即达）、
  **parser 接线（prepare 管线：魔数纠偏 / 炸弹拒绝 / 尾置 moov 就地 remux）**、
  P7 端侧半边（合规窗口两档 + REJECTED 分类 + set_rate_limit 让路落点）、
  P9 缩量（混沌/soak/fuzz-lite + 弱网矩阵缩量版）、**《接入指南》docs/INTEGRATION.md**。
- **计划内、未落**：视频硬编桥与分段流水线（需 VideoToolbox/MediaCodec 或桌面 FFmpeg）、
  移动端系统解码桥（stb 是桌面参考实现，口径已对齐）、P7 其余（IMLink 推送订阅/trace 贯穿三内核）、
  P8 FFI/绑定/后台移交、toxiproxy 真弱网矩阵（mock 近似已覆盖）、MinIO 真实存储、48h 长跑。
- **实现偏差**：传输线程用 `curl_multi_poll + wakeup` 而非自建 reactor（语义等价，升级缝已留）；
  quick_hash 与全量 SHA 在快照单遍 IO 中合并计算（协议保留两段语义）；
  libFuzzer 入口已按标准预留（`tests/fuzz/`，`-DIMMD_FUZZ=ON` 需开源 LLVM clang），每提交由 fuzz-lite 兜底。

## 实战勘误（真实踩坑，全部有回归测试；详见架构文档 §24）

1. **HTTP keep-alive 连接失同步**：handler 未读完请求体就响应 → 下一请求被解析成 `{}POST` → 501。教训：*测试的 setup 也要有断言*。
2. **`HTTPServer.server_bind()` 的 getfqdn 反向 DNS**：DNS 不通时服务启动挂 35 秒。
3. **recover 后按过期位图抢跑**（混沌测试抓获）：规则固化为**对账优先于传输**。
4. **403 凭证过期烧毁重试预算**：换新是节奏问题不是失败问题——固定步频 + 任务级时间预算兜底。
5. **零重传的语义边界**：崩溃瞬间在途分片的服务端记账可能晚于对账快照（S3 ListParts 同语义）——承诺"非在途绝不重传，在途 ≤ 并发窗口"。
6. **`submit()` 返回 `j->id` 的 use-after-free**：job 入队即移交所有权，返回值必须先快照（自查抓获，ASan 矩阵兜底）。
7. **协议校验的"严"要跟着契约演进**：`nparts == 0` 是流式开单的合法应答（总片数 complete 补报），
   parse_init 的旧校验把它当畸形拒绝 → 语音 init 循环失败（e2e 抓获）。收严校验时，先问一遍"契约里谁会合法地发这个值"。
8. **测试基建的偶发失败必须当 bug 修**：前序用例取消在途请求留下半死 keep-alive 连接，
   管理面调用偶发复用坏连接 → 空响应 → 断言假信号（间歇 1/5）。管理面 helper 统一带重试换新连接后归零——
   否则每次红灯都要人肉分辨真假，测试信号就废了。
9. **第三方 vendored 代码的 UBSan 豁免**：stb 的 SIMD 用了有符号左移与负索引指针技巧，
   UBSan 会报（stb 已知噪声，非本项目 bug）。隔离在唯一实现 TU（`stb_impl.cpp`）单独 `-fno-sanitize`——
   本项目自有代码维持 ASan/UBSan/TSan 全覆盖。原则：*不给第三方代码背 UB 账，但豁免范围收到最小 TU*。

## CLI 速用

```bash
python3 testkit/mockctl.py --port 8787 --root /tmp/mock &
./build/immd-cli up 报告.pdf --base http://127.0.0.1:8787 --data /tmp/immd
./build/immd-cli down m_xxx 下载.pdf --base http://127.0.0.1:8787 --data /tmp/immd
./build/immd-cli journal --data /tmp/immd
./build/immd-cli mp4 视频.mp4 --remux 输出.mp4     # moov 检视 / faststart 重排
./build/immd-cli img 图片.png --out /tmp/变体      # 嗅探/GPS/炸弹 + 三层产物 + blurhash
```

接入手册见 [`docs/INTEGRATION.md`](docs/INTEGRATION.md)。

