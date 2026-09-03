# FusionPlay vs ESP32 MiPlay 实现 — 全方位对齐差距报告

> 调研日期: 2026-09-03
> 参考项目: `E:\ESP\dlna\参考项目\FusionPlay-Android-main\FusionPlay-Android-main`
> 我们实现: `E:\ESP\dlna` (miplay.c / now_playing.c / dlna.c)

---

## 一、致命级差异（直接导致 bug）

### [F1] 0x18 ALONE_SET_MEDIA_INFO 不解析 —— 切歌不刷新的根本原因

| | FusionPlay | 我们 |
|---|---|---|
| 位置 | protocol.rs:1176-1192 | miplay.c:2548-2553 |
| 行为 | ACK + **调 emit_media_info() 解析全部字段**，注释 "Always publish it" | **只回 ACK，payload 全丢弃** |

HyperOS 切歌时**先在 companion socket 发 0x18**（比 OPEN 更早），然后才开新媒体通道。我们把 0x18 payload 扔了 → 切歌时新标题/封面永远到不了 UI。

### [F2] 媒体 EOF 无条件触发 pipeline_stop —— 切歌 UI 停滞

| | FusionPlay | 我们 |
|---|---|---|
| 位置 | media.rs:893-898 | miplay.c:1902-1903 |
| 行为 | RTSP EOF **不触发** session inactive，注释: "Marking the whole session inactive here makes the next title and artwork remain cached forever" | **无条件** `s_media_cb(false)` → `miplay_pipeline_stop()` |

切歌时序对比:
```
FusionPlay: 媒体EOF → 不通知UI → 控制会话继续 → 0x12/0x18到达 → UI秒刷
ESP32:      媒体EOF → pipeline_stop → UI认为停止 → 0x18被丢(F1) → 卡死
```

### [F3] 不剥嵌套 JSON 壳 —— 字段解析可能全空

| | FusionPlay | 我们 |
|---|---|---|
| 位置 | protocol.rs:2167-2181 `media_info_value()` | miplay.c:3071-3123 |
| 行为 | 先尝试 `mediaInfo` / `mediaInfoEx` / `metadata` 三层嵌套键（支持 Object 和 stringified JSON），剥壳后再搜字段 | **直接 strstr 整个 payload** |

HyperOS 新版把元数据包在 `{"mediaInfoEx":"{\"mTitle\":\"...\"}"}` 里。我们的 strstr 能找到内层的 `mTitle`，但值解析会因**嵌套引号**截断出错。

### [F4] decode_base64_cover 不去空白 —— 封面解码失败

| | FusionPlay | 我们 |
|---|---|---|
| 位置 | XiaomiPlaybackSnapshotReducer.kt:29 | dlna.c:1274-1308 |
| 行为 | `filterNot(Char::isWhitespace)` 去掉所有 `\r\n\t` 空格后才 base64 解码 | **原样传给 mbedtls_base64_decode** |

FusionPlay 注释: "Xiaomi's mArt field is commonly a MIME-less, line-wrapped Base64 WebP"。小米的 mArt base64 里**带换行**，我们不去掉 → `mbedtls_base64_decode` 失败 → 封面永远显示不出来。

### [F5] generation 不匹配时仍触发 cleanup —— 新会话替换旧会话时误停

| | FusionPlay | 我们 |
|---|---|---|
| 位置 | media.rs:884-890 | miplay.c:1899-1903 |
| 行为 | generation 不匹配 → 发 `media_session_replaced` 静默 return，**不触发** UI 停止 | 仍走 `s_media_cb(false)` → pipeline_stop |

---

## 二、重要级差异（功能缺失或不完整）

### [I1] cover_keys 缺字段

| | FusionPlay | 我们 |
|---|---|---|
| 字段 | `mArt`, `mCoverUrl`, `artwork`, `artworkUrl`, `coverUrl` (5个) | `mCoverUrl`, `mArt`, `artwork`, `cover` (4个) |
| 缺 | — | 缺 `artworkUrl`，多了个不标准的 `cover` |

### [I2] 缺 trackId / 切歌判断机制

FusionPlay (protocol.rs:2218-2233): 优先取 `mAudioId`/`mId`/`id`/`mediaId`，无则 SHA256(title+artist+album) 生成稳定 hash。用于去重"同一首歌的重复更新"。

我们: 完全没有 trackId 概念，每次 SET_MEDIA_INFO 都无条件 np_submit，无法区分"同一首歌的进度更新"和"换了新歌"。

### [I3] SET_POSITION(0x56) 不解析

| | FusionPlay | 我们 |
|---|---|---|
| 行为 | 从 body 前 8 字节解 position_ms(u64 BE)，emit "progress" 事件 | 只回 ACK，position 丢弃 |

### [I4] SET_MEDIA_STATE(0x5E) 不解析播放状态

| | FusionPlay | 我们 |
|---|---|---|
| 行为 | 支持 4 种编码格式解码 state(2=播放/3=暂停)，emit_playback | 只回 ACK |

### [I5] ALONE_SET_STATE(0x14) 不解析

同上，companion socket 上的播放/暂停控制被丢弃。

### [I6] 缺 position 字段解析

FusionPlay 备选键: `mPosition`/`position`/`positionMs`/`position_ms`。我们完全没有。

### [I7] 缺 metadataChangeType 字段

FusionPlay (protocol.rs:2253-2256): 提取 `mMetaChangeType`/`metaChangeType`，用于区分"全量更新"和"部分字段更新"。我们不解析。

### [I8] JSON 值解析不处理引号内逗号

我们 title/artist/album 的值提取遇到 `,` 就截断。如果歌名含逗号（如 "Song, Part 1"）会截断。FusionPlay 用 serde_json 正确处理所有转义边界。

### [I9] duration 备选键不全

FusionPlay: `mDuration`/`duration`/`durationMs`/`duration_ms`
我们: 缺 `durationMs`（驼峰无下划线）

### [I10] GET_MEDIA_INFO(0x14) 缺反向控制就绪状态

FusionPlay 发完 NOTIFY 后调 `mark_reverse_control_ready()`，后续的遥控按键(PAUSE/NEXT/PREV)才生效。我们没有这个状态机。

---

## 三、架构级差异（设计层面）

### [A1] 线程模型

| FusionPlay | ESP32 |
|---|---|
| 每个控制会话独立线程 + writer 线程 + RTSP 线程 + 媒体线程 = **4 线程/会话** | select 多路复用 + RTSP task + media task = **3 task 共享状态** |
| 控制 socket EOF ≠ 媒体 EOF，完全独立生命周期 | `s_running` 全局标志影响所有 task |

### [A2] 多会话支持

FusionPlay: `ControlHub` 用 `Arc<Mutex<HashMap>>` 管理多并发会话，多手机可同时连。
我们: `handle_client` 同步阻塞(:3215)，**一次只处理一个控制会话**。

### [A3] 封面传递链路

```
FusionPlay: Rust提取artwork字符串 → Kotlin normalizeXiaomiArtworkSource
             (去空白+判断格式+加data:前缀) → UI自行解码显示
             【只做字符串归一化，不碰像素】

ESP32:      miplay.c提取pending_cover → np_set_cover_url → PSRAM 48KB缓冲
             → fetch_album_art_async → album_art_task
             → base64解码 → 格式检测 → 像素解码 → 中心裁剪 → 双线性缩放48x48 → RGB565
             【全链路在ESP32完成】
```

### [A4] replaceTrack / merge 语义

FusionPlay 有精确的 `isNewTrack()` 判断（比较 trackId/title/artist/album），换歌时全量替换，更新时 merge 保留旧值。
我们: 没有 merge，每次 np_submit 覆盖整个 s_meta。

### [A5] mDNS 发现

FusionPlay: 同时发布 `_lyra-mdns._udp` + `_mi-connect._udp`，完整双向发现。
我们: 只被动查询 `_lyra-mdns._udp`，不发布自己的服务。

### [A6] 加密层

两者 CBC 加解密逻辑**完全一致**（SafetyData v1 容器、IV 演进、密钥派生、零填充 CRC32）。唯一差异是我们用纯软件 AES 替代了硬件 GDMA（已验证 FIPS-197 正确）。

---

## 四、FusionPlay "秒刷新" 的三层保障

FusionPlay 无论 MiPlay 还是 DLNA 都能秒刷新歌曲信息，靠三层保障:

1. **多命令无条件上报**: 0x12 和 0x18 都调 emit_media_info，不区分哪条 socket 来的
2. **媒体 EOF ≠ 会话结束**: RTSP 关闭只标记 channel_closed，不清 session，控制会话继续处理后续元数据
3. **JSON 结构化剥壳**: mediaInfo/mediaInfoEx/metadata 三层嵌套都能剥出来，不怕 payload 格式变化

我们目前只做到了第 1 层的**一半**（0x12 有，0x18 没有），第 2 层完全反着来（EOF 就停），第 3 层完全缺失。

---

## 五、修复优先级

| 优先级 | 差异 | 影响 | 修复复杂度 |
|---|---|---|---|
| **P0** | F4: base64 去空白 | 封面永不出 | **1行** |
| **P0** | F1: 0x18 不解析 | 切歌不刷新 | **中等** 复用 0x12 解析 |
| **P0** | F3: 不剥嵌套 JSON 壳 | 字段解析出错 | **中等** 加剥壳逻辑 |
| **P0** | F2: 媒体 EOF 无条件 stop | 切歌 UI 停滞 | **中等** 区分断开 vs 切歌 |
| **P0** | F5: generation 不匹配仍 cleanup | 新会话误停 | **小** |
| **P1** | I1-I9 | 功能缺失 | **低** 补字段/解析 |
| **P2** | A1-A5 | 架构限制 | **高** 需重构 |

建议修复顺序: **F4(去空白) → F1(0x18解析) → F3(JSON剥壳) → F2(EOF不停) → F5**

---

## 六、关键文件路径

### FusionPlay 参考项目
- `src/FusionPlay.MiPlaySdk/src/protocol.rs` — 命令分发/emit_media_info/media_info_value
- `src/FusionPlay.MiPlaySdk/src/media.rs` — RTSP/媒体/EOF 处理
- `flutter/.../XiaomiPlaybackSnapshotReducer.kt` — normalizeXiaomiArtworkSource/applyXiaomiMediaInfo
- `flutter/.../XiaomiPlaybackReducer.kt` — isNewTrack/mergeMediaInfo

### 我们的实现
- `components/miplay/miplay.c` — 协议处理/加密/元数据解析
- `components/now_playing/now_playing.c` — 统一元数据层
- `main/dlna.c` — np 回调/封面 worker/解码

---

## 七、封面(artwork)完整管线对比

### FusionPlay 三层管线

```
协议层(protocol.rs:2234)    →  提取 artwork 字符串(URL/裸base64/data:URI)，原样传
Bridge层(XiaomiPlaybackReducer) →  基于 trackId 续传：同一首歌新 session 到来但 artwork=null 时
                                   携带上一首的 artworkUrl，避免封面闪烁
状态层(XiaomiPlaybackSnapshot) →  normalizeXiaomiArtworkSource 归一化：
                                   ① trim  ② 去全部空白(含换行)  ③ 校验base64长度/字符集
                                   ④ 解码判格式(JPEG/PNG/WebP)  ⑤ 拼 data:$mimeType;base64,$encoded
UI层(FusionPlayMediaChannel)  →  decodeSampledBytes(inSampleSize降采样)
                                   → fitInside(384px) 硬上限
                                   → 写入 MediaSessionCompat
```

**关键参数**:
- 最大下载: 8 MiB
- 最终尺寸: 最大边 **384px**（`MAX_ARTWORK_SIZE = 384`）
- HTTP 缓存: `useCaches = false`（无磁盘缓存）
- 防抖: `LatestRequestGate` 确保只提交最新请求
- Placeholder: **无**，artwork 为 null 时调 `clearArtwork()` 清除

### ESP32 管线

```
协议层(miplay.c:3102)       →  strstr 搜 cover_keys，截取到 '"' 或 '}'，存 pending_cover
now_playing(np_cover_store)  →  heap_caps_malloc(PSRAM, 48KB) 存完整 base64
dlna.c(fetch_album_art_async)→  strdup(url) 入单槽队列，gen++ 代次防竞态
album_art_task               →  vTaskDelay(1000ms) 等音频稳定
                                → base64解码(mbedtls) → 格式检测(JPEG/PNG/WebP)
                                → 像素解码 → 中心裁剪 → 双线性缩放48x48 → RGB565
                                → lvgl_port_ui_set_cover(out_buf, 48, 48)
```

### 差距

| 维度 | FusionPlay | ESP32 | 差距 |
|------|-----------|-------|------|
| base64 去空白 | `filterNot(isWhitespace)` | **不去** | **封面解码失败根因** |
| 格式归一化 | 解码判格式+拼data:前缀 | 不归一化 | 若中间层需data:格式则不兼容 |
| 尺寸 | 384px | 48px | 嵌入式限制，可提至80-96px |
| trackId 续传 | 同一首歌保留旧封面 | 无 | 切歌时可能闪烁 |
| 防抖 | LatestRequestGate | gen代次 | 功能等价 |
| placeholder | 无(清空) | 无(清空) | 一致 |

---

## 八、播放进度同步对比

### FusionPlay 双重推进机制

**被动推送**: 手机发 `SET_POSITION(0x56)`，body 前 8 字节 big-endian u64 = `position_ms`（protocol.rs:1653-1676）

**本地计时**: `FusionPlayRuntime.kt:123-170` 有 500ms 定时循环（`PROGRESS_TICK_MS = 500L`），在两次手机推送之间**本地按 elapsed 时间平滑推进 position**，防止 UI 卡顿。

```
手机推送 SET_POSITION ──→ 权威值，直接覆盖
    ↓ 间隔期间
本地 500ms tick ──→ advanceXiaomiProgress(elapsedMs) 平滑推进
```

**去重**: `observe_position` 判断条件 `position_ms > previous_ms + 250ms`，避免微小抖动重复 emit。

### ESP32

- SET_POSITION(0x56): **只回 ACK，不解析 body**（miplay.c:3011-3014）
- 本地计时: **无**
- 进度条: 永远显示 0

### 差距

| 维度 | FusionPlay | ESP32 |
|------|-----------|-------|
| SET_POSITION 解析 | u64 BE 解码+去重+emit | 只回 ACK |
| 本地计时推进 | 500ms tick + elapsed 推进 | 无 |
| 拖拽 seek | NOTIFY key-seek(u64 BE) | 未实现 |

---

## 九、反向控制（遥控）对比

### FusionPlay 反向控制架构

**控制命令通过 NOTIFY(0x22) 帧发给手机**，payload 是 receiver-control 格式：

| 操作 | NOTIFY payload 格式 |
|------|-------------------|
| Pause | `[len][b"key-pause"][0x00,0x01]` |
| Resume | `[len][b"key-resume"][0x00,0x01]` |
| Previous | `[len][b"key-prev"][0x00,0x01]` |
| Next | `[len][b"key-next"][0x00,0x01]` |
| Seek(ms) | `[len][b"key-seek"][0x09][u64_be]` |
| Volume | `SET_VOLUME(0x0c)` 直接加密帧，body=u32 BE percent |

`receiver_control_boolean` 格式(protocol.rs:835-840): `[key_len:u8][key_bytes][0x00,0x01]`
`receiver_control_u64` 格式(protocol.rs:843-849): `[key_len:u8][key_bytes][0x09][value:u64_be]`

**前置条件**: 必须 `mark_reverse_control_ready`(protocol.rs:281-287)，在 OPEN 阶段发送 empty_media_info_notification 后自动置 true。

**路由选择**: `preferred_control_route`(protocol.rs:290) 优先选 `reverse_control_ready==true` 的 companion 连接。

### ESP32

- PAUSE(0x04)/RESUME(0x06): 只回 ACK（miplay.c:3011-3014）
- SET_VOLUME(0x0c): 只回 ACK
- 上一首/下一首: **完全没有实现**
- NOTIFY receiver-control: **未实现**
- reverse_control_ready 状态机: **未实现**

### 差距

| 操作 | FusionPlay | ESP32 |
|------|-----------|-------|
| Pause | NOTIFY key-pause | 只回 ACK |
| Resume | NOTIFY key-resume | 只回 ACK |
| Previous | NOTIFY key-prev | **未实现** |
| Next | NOTIFY key-next | **未实现** |
| Seek | NOTIFY key-seek(u64) | **未实现** |
| Volume | SET_VOLUME(0x0c) 直接帧 | 只回 ACK |
| 前置条件 | mark_reverse_control_ready | 无 |

---

## 十、DLNA 对齐差距

### DLNA 元数据

| 维度 | FusionPlay | ESP32 |
|------|-----------|-------|
| DIDL-Lite 解析 | 完整: title/artist/album/albumArtURI/res(protocolInfo:采样率/位深/声道)/duration | 基础: title/artist/albumArtURI |
| 封面获取 | 提取 URL 透传给原生层下载 | HTTP GET 下载+解码+缩放(48x48) |
| 进度 | 本地时钟推算 `settle_clock()` + GENA 推送 | 需 GetPositionInfo 轮询 |
| 歌词 | DIDL-Lite 中提取 LRC | 网易云 API 单独拉取 |

### DLNA 反向控制

| 操作 | FusionPlay | ESP32 |
|------|-----------|-------|
| Play/Pause/Stop | AVTransport SOAP | AVTransport SOAP ✓ |
| Seek | SOAP unit=REL_TIME(HH:MM:SS.f) | 有实现 ✓ |
| Next/Previous | AVTransport SOAP | **未确认** |
| Volume | RenderingControl SOAP SetVolume(0-100) | 可能未实现 |
| 音量 dB 转换 | GetVolumeDB/SetVolumeDB | 无 |
| GENA 事件订阅 | 完整实现 | 可能缺失 |

### 多源仲裁

| 维度 | FusionPlay | ESP32 |
|------|-----------|-------|
| 仲裁机制 | Rust epoch lease + Kotlin 双层：takeover 时先 suspend 旧源再激活新源 | np_set_source 递增 epoch，但无仲裁 |
| DLNA 抢占时机 | SetAVTransportURI 不抢占，**必须 Play 才接管** | 收到 SetAVTransportURI 直接切源 |
| 统一管线 | DLNA/MiPlay/AirPlay 都汇入同一 `playback.coverArt` → `publishMediaSession` | DLNA/MiPlay 走 now_playing 统一层 ✓ |

---

## 十一、更新后的完整修复优先级

### P0 — 直接导致功能不可用

| # | 差异 | 影响 | 复杂度 |
|---|------|------|--------|
| 1 | F4: base64 不去空白 | 封面永不出 | **1行** |
| 2 | F1: 0x18 不解析 | 切歌标题不刷新 | 中等 |
| 3 | F3: 不剥嵌套 JSON 壳 | 字段解析出错 | 中等 |
| 4 | F2: 媒体 EOF 无条件 stop | 切歌 UI 停滞 | 中等 |
| 5 | F5: generation 不匹配仍 cleanup | 新会话误停 | 小 |

### P1 — 功能缺失

| # | 差异 | 影响 | 复杂度 |
|---|------|------|--------|
| 6 | SET_POSITION 不解析 | 进度条永远为0 | 小 |
| 7 | 缺本地进度推进(500ms tick) | 进度不平滑 | 小 |
| 8 | 反向控制全部缺失 | 无法遥控手机 | 中等 |
| 9 | NOTIFY receiver-control 未实现 | 上一首/下一首不可用 | 中等 |
| 10 | SET_MEDIA_STATE 不解析 | 播放/暂停状态不同步 | 小 |
| 11 | trackId 机制缺失 | 无法去重/续传封面 | 小 |
| 12 | cover_keys 缺 artworkUrl/coverUrl | 某些版本漏封面 | 1行 |
| 13 | duration 缺 durationMs 备选键 | 某些版本漏时长 | 1行 |
| 14 | JSON 值解析逗号截断 | 歌名含逗号被截断 | 小 |
| 15 | metadataChangeType 不解析 | 无法区分全量/部分更新 | 小 |

### P2 — 架构/体验优化

| # | 差异 | 影响 | 复杂度 |
|---|------|------|--------|
| 16 | 封面尺寸 48px(可提至 80-96px) | 封面模糊 | 1行改常量 |
| 17 | DLNA RenderingControl 音量 | DLNA 音量不同步 | 中等 |
| 18 | DLNA GENA 事件订阅 | 控制点收不到状态变更 | 高 |
| 19 | 多源仲裁 | 多设备并发冲突 | 高 |
| 20 | DLNA DIDL 解析不完整 | 采样率/位深/声道缺失 | 中等 |

### 建议修复顺序

```
第一轮(立即): F4(1行去空白) → F1(0x18解析) → F3(JSON剥壳) → cover_keys补字段
第二轮(切歌): F2(EOF不停) → F5(generation cleanup) → trackId机制
第三轮(体验): SET_POSITION解析 → 本地进度推进 → SET_MEDIA_STATE → 反向控制
第四轮(完善): DLNA RenderingControl → 多源仲裁 → 封面尺寸提升
```

---

## 附录 A: protocol.rs 完整命令参考

### A.1 全部命令 ID

**控制层命令 (u8):**

| 常量 | 值 | 含义 | ACK |
|---|---|---|---|
| SAFETY_INFO | 0x00 | 安全参数协商 | 0x01 |
| SAFETY_AUTH | 0x02 | 互质询HMAC认证 | 0x03 |
| OPEN | 0x00 | 打开媒体流 | 0x01 |
| PAUSE | 0x04 | 暂停 | 0x05 |
| RESUME | 0x06 | 恢复 | 0x07 |
| SET_VOLUME | 0x0c | 设置音量 | 0x0d |
| GET_VOLUME | 0x0e | 查询音量 | 0x0f |
| SET_MEDIA_INFO | 0x12 | 设置媒体元数据 | 0x13 |
| GET_MEDIA_INFO | 0x14 | 请求媒体信息(回复NOTIFY) | — |
| HEART_BEAT | 0x1a | 心跳 | 0x1b |
| GET_STATE | 0x1c | 播放状态查询 | 0x1d |
| GET_DEVICE_INFO | 0x1e | 设备信息查询 | 0x1f |
| NOTIFY | 0x22 | 通知帧 | — |
| GET_MIRROR_MODE | 0x34 | 镜像模式查询 | 0x35 |
| GET_VERSION | 0x36 | 协议版本查询 | 0x37 |
| SET_PLAY_SOURCE | 0x40 | 注册控制中心来源 | 0x41 |
| DEVICE_ID | 0x28 | 发送设备数字ID | — |
| AUTH_ACK | 0x29 | 客户端认证确认 | — |
| SET_POSITION | 0x56 | 进度同步 | 0x57 |
| SET_MEDIA_STATE | 0x5e | 媒体状态快照 | 0x5f |
| SET_DEVICE_INFO | 0x58 | 源端设备信息 | 0x59 |
| SET_MIRROR_KEY | 0x6c | 流密钥交换 | 0x6d |

**ALONE 命名空间 (outer_type=0x04):**

| 低字节 | 含义 | ACK |
|---|---|---|
| 0x14 | SET_STATE | 0x15 |
| 0x16 | GET_STATE | 0x17 |
| 0x18 | SET_MEDIA_INFO | 0x19 |

### A.2 帧格式

TCP 帧头: `[0x24][outer_type:u8][command:u8][value_type:u16BE][length:u32BE]` + body

加密帧(SafetyData): `[0x00,0x07,0x01,0xe0][padding_len:u8][integrity:u32BE][ciphertext...]`

- 零填充(非PKCS#7)，补齐到16字节对齐
- integrity = miplay_integrity(padded_ciphertext)，CRC-32 多项式 0x04c11db7，初值 0xFFFFFFFF，每项字节交换
- AES-128-CBC，初始IV=key，加密后IV=密文最后16字节

### A.3 JSON 字段搜索降级顺序

**title**: mTitle → title
**artist**: mArtist → artist
**album**: mAlbum → album
**duration**: mDuration → duration → durationMs → duration_ms
**position**: mPosition → position → positionMs → position_ms
**artwork**: mArt → mCoverUrl → artwork → artworkUrl → coverUrl
**track_id**: mAudioId → audioId → mId → id → mediaId → SHA256(title+artist+album)前16字节

**嵌套壳剥壳**: mediaInfo / mediaInfoEx / metadata (支持 Object 和 stringified JSON)

### A.4 NOTIFY 帧类型

| type | 名称 | 格式 |
|---|---|---|
| 3 | mode | `{4,"mode",3,value:u8}` (2=移动音频流) |
| 4 | mediaInfoEx | 二进制 TLV 嵌套 |
| 5 | state | `{5,"state",3,value:u8}` (0=idle,2=playing,3=paused) |
| 7 | volume | `{6,"volume",7,percent:u32BE}` |
| 8 | receiver-control | 见下方 |

### A.5 反向控制格式 (NOTIFY type=8)

**receiver_control_boolean**: `[key_len:u8][key_bytes][0x00,0x01]`
**receiver_control_u64**: `[key_len:u8][key_bytes][0x09][value:u64BE]`

键名: `key-pause`, `key-resume`, `key-prev`, `key-next`, `key-seek`

**前置条件**: 必须 `mark_reverse_control_ready`(在收到 GET_MEDIA_INFO 0x14 时触发)

### A.6 auth_key 生成

`"{local_ip}{local_port}{remote_ip}{remote_port}"` → 每个ASCII数字+0x31('0'→'a') → MD5 → hex 32字符 → 前16字节作 AES key/IV

### A.7 完整 NOTIFY 认证序列

1. seq=5: `mode` NOTIFY (type=3, value=2=移动音频流)
2. seq=6: `mediaInfoEx` 空通知 (30字节固定)
3. seq=7: `state` NOTIFY (type=3, value=0=idle)

---

## 附录 B: media.rs 完整机制参考

### B.1 RTSP 握手流程

1. TCP 连接 RTSP 源，5秒超时
2. 收 OPTIONS → HMAC-SHA256 authMsgAck 回复 → 反发 OPTIONS
3. 收 GET_PARAMETER(wfd_audio_codecs_v2) → 建立 image + multi 两条 TCP 数据连接
4. 响应参数协商: audio_codecs_v2=15 3 3, video_formats=none
5. 收 SETUP trigger → 发 SETUP 携带 MultiPort
6. SETUP 200 → 发 PLAY → PLAY 200 → emit stream_started

### B.2 媒体流三层错误分支

| 条件 | 行为 | 注释 |
|---|---|---|
| generation 不匹配 | emit media_session_replaced, return | 不标记 session inactive |
| 正常 EOF (RemoteChannelClosed) | emit media_channel_closed(outcome=normal), return | **不标记 session inactive** |
| 其他错误 | emit error + playback_state(session_active=false) | 真正断开 |

**关键注释 (media.rs:896-899)**: "HyperOS commonly closes this channel while switching tracks and then keeps sending metadata/control events on the same session. Marking the whole session inactive here makes the next title and artwork remain cached forever."

### B.3 RTP/TS 解析

- TCP 通道: `$` 标记(1B) + channel(1B) + length(2B BE)
- RTP: version=2, timestamp 转微秒 = rtp_timestamp * 1_000_000 / 90_000
- TS: 188字节, sync=0x47, 只处理 PID=0x1100
- PES: start code `00 00 01 C0`, PTS 优先于 RTP timestamp
- ADTS: 帧同步 0xFFF0, 13种采样率, **严格要求 48000Hz**

### B.4 媒体流加密

- **不使用 SafetyData 容器**
- 直接在 PES payload 上做 AES-128-CBC
- per-packet IV 通过 PES private_data_extension 传递
- 仅解密 `min(len,256)/16*16` 字节的 prefix (NoPadding)

### B.5 时钟同步

- UDP 40字节包，client 写 local_now_micros(LE offset 0-7)，server 回 server_send(offset 16-23) + server_receive(offset 24-31)
- offset = (server_receive - client_send + server_send - client_receive) / 2
- 线性回归拟合频率偏移，clamp ±0.0005
- presentation offset 由 SET_PARAMETER wfd_timeoffset 设置

### B.6 图像流

`spawn_image_drain`: 仅循环读空 image TCP 连接并丢弃。目的: 保持 TCP 不被 RST(MiPlay 协商要求同时建 image + multi 通道)。

---

## 附录 C: Lyra/mDNS/lib.rs 参考

### C.1 Lyra 协议

- KCP-over-UDP + Protobuf，端口 55982
- **不是原生 KCP**，仅复用 24 字节头部格式

**三阶段握手:**
1. Physical Sync (kind=0x09, frame_type=1): 设备信息交换
2. Logical Sync (kind=0x11, inner_type=5): 超时/服务名协商
3. Auth/ECDH (inner_type=6): P-256 ECDH + HKDF-SHA256 → 32字节 session key

**加密**: AES-256-GCM，12字节随机 nonce 前置
**重传**: 固定 250ms 间隔，最多 8 次，无拥塞控制

### C.2 mDNS 发现

**发布两个服务:**
- `_lyra-mdns._udp.local.` — Lyra 发现，端口 5353
- `_mi-connect._udp.local.` — Mi Connect，端口 56666

**设备身份**: SHA256(seed) → Base64URL → short_id=前3字符, instance_suffix=前10字符

**设备类型:**

| MiPlayDeviceType | Mi Connect dev= | Lyra discovery |
|---|---|---|
| Television=2 | 2 | 3 |
| Speaker=4 | 4 | 5 |
| Vehicle=5 | 5 | 8 |
| DisplaySpeaker=16 | 16 | 5 |
| Tablet=18 | 18 | 2 |

**Mi Connect TXT Record:**
`version=196608 / apps=[5] / flags=CgE= / name=... / idHash=<Base64(3字符)> / dev=<type> / sec=2 / appsData=gQAEBIMiww==`

**AppData 编码**: 二进制结构 → Base64 → TXT record。含 lyra_device_type + instance_bytes + 设备名。

**Goodbye**: TTL=0 的 DNS 记录，启动时发 3 轮(间隔 250ms)。12 秒空闲自动 goodbye。

### C.3 lib.rs 公开 API

- `MiPlayReceiver::start(config, events) -> Result<Self>`
- `ReceiverController::send/set_volume/send_confirmed/suspend_output/resume_output/stop`
- 配置: name / identity / local_ip / device_type / initial_volume_percent(默认50)

---

## 附录 D: Kotlin Bridge/Backend 层参考

### D.1 XiaomiPlaybackReducer — 切歌判断与 merge

**isNewTrack() 四重判断:**
1. incoming 必须有 trackId/title/artist/album 任一
2. previous 为 null → 新歌
3. trackId 不同且都不是 synthetic(`metadata:` 前缀) → 新歌
4. title 变化，或 title 非空时 artist/album 变化 → 新歌

**mergeMediaInfo()**: `replaceTrack=true` → 全量替换; 否则 13 个字段 null-coalescing merge(非空才覆盖)

**pendingSeek**: seek 后立即设 positionMs 为目标值，后续 incoming position 在 3000ms tolerance 内则清除，5s 窗口过期自动释放。

### D.2 XiaomiPlaybackEvent — 事件解析

**13 个 nullable 字段**: title / artist / album / artworkUrl / durationMs / positionMs / sessionSequence / eventSequence / trackId / codec / bitrateBps / sampleRate / bitsPerSample / channels / metadataChangeType

**事件名路由**: `media_info` / `audio_format` / `progress` 三种
**sessionSequence 优先级**: WindowsBridgeXiaomiEvent.sessionSequence > payload.session_seq > session_sequence > session_id

### D.3 AppViewModel — 三源架构

- ReceiverProtocol: MIPLAY / AIRPLAY / DLNA
- `reduceEvent()`: 大型 when 分支，每个 Rust AppEvent 更新对应 PlaybackSnapshot
- epoch 校验: `eventMatchesSource()` 保证事件属于当前 epoch
- Xiaomi 播放激活: rawState==2 且 isPlaying → 自动 SourcePlaybackProjection.activate()

### D.4 FusionPlayRuntime — 主运行时

**500ms progress tick**: 协程循环 delay(500ms)
- AirPlay/DLNA 源: 从 networkPlayer 同步 position/playing/duration
- MiPlay 源: `advanceXiaomiProgress(elapsedMs)` 本地推进

**publishMediaSession()**: AppState.playback → FusionPlayMediaChannel
1. write metadata (title/artist/album/mediaIdentity)
2. write playback state
3. write timeline
4. write capabilities
5. artwork 异步加载 (artworkJob)

**源切换**: activatePlaybackSource() → 先暂停 Xiaomi(如非目标) → mediaSourceArbiter.activate() 暂停前一个源

### D.5 FusionPlayMediaChannel — 封面与 MediaSession

**封面处理链:**
1. `setArtworkDataUri()`: 提取 base64 → 限 `MAX_ARTWORK_BASE64_CHARS` → decode → 限 8MB
2. `decodeSampledBytes()`: `while (largest / (sample*2) >= 384) sample *= 2` (inSampleSize)
3. `fitInside(384)`: 最长边 > 384 则缩放
4. `LatestRequestGate`: 原子 generation 计数器防抖
5. 写入 `MediaSessionCompat` 的 METADATA_KEY_ALBUM_ART

**播放状态发布阈值**: timeline jump >=1500ms / 发布间隔 >=1000ms / playing/hasMedia 变化，满足任一才写入

### D.6 AppEvent — 22 种事件类型

Status / ReceiverReady / OutputDevice / ClientConnected / ClientDisconnected / StreamStarted / StreamStopped / SourceTakeover / NowPlaying / CoverArt / Volume / Progress / PlaybackState / VideoPlay / VideoSeek / VideoRate / VideoStop / DlnaReady / DlnaMedia / DlnaSeek / DlnaRate / DlnaStop / DlnaVolume / RemoteControlAvailable / RemoteControlUnavailable / CommandResult / Error / Log / Unknown

---

## 附录 E: DLNA 完整实现参考

### E.1 SSDP 发现

- 组播 239.255.255.250:1900，max-age=1800s，announce 周期=900s
- 设备类型: `urn:schemas-upnp-org:device:MediaRenderer:1`
- 3 服务: AVTransport / RenderingControl / ConnectionManager
- manufacturer: "Microsoft Corporation", DLNADOC=DMR-1.50

### E.2 AVTransport 全部 Action

| Action | 关键行为 |
|---|---|
| SetAVTransportURI | 解析 CurrentURI + DIDL-Lite XML，同 URI 刷新不重置位置 |
| SetNextAVTransportURI | 设置 next_uri/next_metadata |
| Play | 仅接受 Speed=1，申请 MediaLease |
| Pause | pause_from(peer) |
| Stop | stop_from(peer) |
| Seek | REL_TIME/ABS_TIME，解析 HH:MM:SS |
| GetTransportInfo | TransportState/OK |
| GetPositionInfo | Track/Duration/RelTime/AbsTime |
| GetMediaInfo | NrTracks/Duration/URI/MetaData/NextURI |
| Next/Previous | 切换队列相邻 track |
| GetCurrentTransportActions | 按状态返回可用操作 |

### E.3 DIDL-Lite 解析字段

title / artist(兼容creator) / album / albumArtURI / class / protocolInfo(mime/采样率/位深/声道) / duration / bitrate(UPnP字节/秒转bps，支持kHz后缀) / synchronizedLyrics / lyrics / lyricsURI

**资源选择评分**: preferred URI +10000, audio +1000, video +900, image -1000, lrc -2000, 有 bitrate/sampleRate/channels 各 +100, 有 duration +10

### E.4 RenderingControl

- GetVolume/SetVolume (0-100 整数)
- GetMute/SetMute
- GetVolumeDB/SetVolumeDB (对数: volume/100 → dB = log10 * 5120, 范围 -10240~0)
- GetVolumeDBRange (Min=-10240, Max=0)

### E.5 GENA 事件订阅

- SUBSCRIBE: NT=upnp:event + CALLBACK=\<http://...\>
- 续订: 只需 SID
- 超时默认 1800s，最大 86400s
- 初始事件 20ms 延迟发送
- 回调 IP 必须与订阅来源相同
- 过期订阅在 notify() 时自动清理

### E.6 状态机

5 状态: NoMediaPresent / Stopped / Transitioning / Playing / PausedPlayback

- SetURI → Stopped + position=0
- Play → Transitioning(未ready) 或 Playing, rate=1.0
- Pause → PausedPlayback, rate=0
- Stop → Stopped/NoMediaPresent

### E.7 进度管理

`settle_clock()`: `position_ms += elapsed_ms * rate`，clamp 到 duration。播放期间靠 Instant::now() 推算，无定时器轮询。

### E.8 SINK_PROTOCOL_INFO

audio/mpeg, audio/mp4, audio/aac, audio/flac, audio/wav, audio/x-wav, video/mp4, video/mpeg, video/x-matroska, application/vnd.apple.mpegurl

DLNA profile: FLAC/LPCM/WAV/MP3/AAC/M4A/ALAC/OGG/OPUS

### E.9 多源仲裁 (takeover.rs)

- `PlaybackArbiter`: AtomicU64 epoch + AtomicU8 source
- MediaSource: AirPlayAudio=1 / AirPlayVideo=2 / Dlna=3 / XiaomiMiPlay=4
- `takeover()`: 分配新 epoch → 替换 ownership → 暂停旧源(SuspendHook) → SourceTakeover 事件
- `release()`: source+epoch 完全匹配才释放

---

## 附录 F: 与我们实现的关键差距速查表

| 维度 | FusionPlay | ESP32 差距 | 影响 |
|------|-----------|-----------|------|
| 0x18 解析 | emit_media_info | 只回ACK | 切歌不刷新 |
| 嵌套 JSON 剥壳 | mediaInfo/mediaInfoEx/metadata | strstr 直搜 | 字段解析出错 |
| base64 去空白 | filterNot(isWhitespace) | 不去 | 封面永不出 |
| 媒体 EOF | 不标记 session inactive | 无条件 pipeline_stop | 切歌 UI 停滞 |
| SET_POSITION | 解析 u64 + 本地 500ms tick | 只回 ACK | 进度为 0 |
| 反向控制 | NOTIFY key-prev/next/pause/resume/seek | 未实现 | 无法遥控 |
| 音量同步 | SET_VOLUME 直接帧 + NOTIFY type=7 | 只回 ACK | 音量不同步 |
| trackId | mAudioId/SHA256 hash | 无 | 无法去重/续传 |
| pendingSeek | 5s 窗口 + 3s tolerance | 无 | seek 无反馈 |
| eventSequence 去重 | 递增序列号丢弃旧事件 | 无 | 旧事件可能覆盖 |
| 播放状态 | decode_playback_state(4种编码) | 只回 ACK | 状态不同步 |
| GET_MEDIA_INFO | 发空 NOTIFY + mark_reverse_control_ready | 只发 NOTIFY(mTitle) | 反向控制不就绪 |
| DLNA GENA | 完整 SUBSCRIBE/NOTIFY | 可能缺失 | 控制点收不到状态 |
| DLNA RenderingControl | 音量/静音/dB | 可能未实现 | DLNA 音量不同步 |
| DLNA DIDL 解析 | 完整(采样率/位深/声道/歌词) | 基础(title/artist/art) | 缺音频信息 |
| 多源仲裁 | epoch lease + suspend | np_set_source 简单切换 | 多设备冲突 |
| 封面归一化 | 去空白+判格式+data:前缀 | 无 | base64 解码失败 |
| 封面尺寸 | 384px + inSampleSize 降采样 | 48px | 模糊 |
| Lyra 发现 | 完整 ECDH + mDNS 发布 | 只被动扫描 | 不发布自己 |
| 认证 | HMAC-SHA256 双向质询 | 已实现 ✓ | 一致 |
| 加密 | SafetyData AES-128-CBC | 已实现(软件AES) ✓ | 一致 |
| IV 演进 | 密文最后16字节 | 一致 ✓ | 一致 |
| DLNA RenderingControl | 音量/静音/dB | **已实现** (custom_dlna.c) ✓ | 一致 |
| DLNA GENA 订阅 | SUBSCRIBE/NOTIFY | **已实现** (custom_dlna.c) ✓ | 一致 |
| SET_VOLUME 解析 | u32 BE + NOTIFY vol | **已实现** (miplay.c:2982) ✓ | 一致 |
| LatestRequestGate | 原子 generation 防抖 | **已等价** (s_album_art_gen) ✓ | 一致 |

---

## 附录 G: P0 致命级差距移植适配方案

### G.1 base64 去空白 (封面解码失败)

**修改位置**: dlna.c:1287 `decode_base64_cover()` 内，`strlen(encoded)` 之后、`mbedtls_base64_decode` 之前

**方案**: PSRAM 临时缓冲，逐字符拷贝跳过 `\r\n\t `
```c
char *clean = heap_caps_malloc(encoded_len + 1, MALLOC_CAP_SPIRAM);
if (!clean) return -1;
size_t j = 0;
for (size_t i = 0; i < encoded_len; i++) {
    unsigned char c = (unsigned char)encoded[i];
    if (c != '\r' && c != '\n' && c != '\t' && c != ' ')
        clean[j++] = encoded[i];
}
clean[j] = 0;
encoded = clean;
encoded_len = j;
// ... decode 后 heap_caps_free(clean)
```

**mbedtls_base64_decode 行为**: 内部用 `base64_dec_map` 查表，遇到非 base64 字符(含空白)返回 `MBEDTLS_ERR_BASE64_INVALID_CHARACTER`(-1)。**必须预处理**。

**内存**: 峰值多一份 PSRAM 临时缓冲(~base64 串长，典型 30-80KB)，decode 后立即 free。总峰值 ~2x base64 串大小，8MB PSRAM 可接受。

**复杂度**: 2/5 | **增量**: ~15 行 | **风险**: 无

### G.2 0x18 ALONE_SET_MEDIA_INFO 解析

**修改位置**: miplay.c:2548 (case 0x18) + :3066-3149 (CMD_SET_MEDIA_INFO 逻辑)

**方案**: 将 :3066-3149 的 80 行解析逻辑提取为独立函数:
```c
static void parse_set_media_info(const uint8_t *payload, uint32_t plen);
```
- 0x12 在 :3066 调 `parse_set_media_info(payload, plen)` + 发 ACK
- 0x18 在 :2548 也调 `parse_set_media_info(payload, plen)` + 发 ACK

**关键**: 0x18 的 payload 经外层解密后(:2513-2532 统一解密)，和 0x12 格式相同，都是裸 JSON。外层加密壳已剥掉，payload 指向明文 JSON。

**复杂度**: 2/5 | **增量**: ~50 行(提取函数+两处调用) | **风险**: 中(需验证 0x18 payload 格式)

### G.3 JSON 嵌套壳剥壳

**方案选择**:

| 方案 | 内存 | 复杂度 | 风险 |
|------|------|--------|------|
| A: cJSON 解析 | ~2-3x payload(PSRAM) | 低 | ESP-IDF 自带 |
| B: strstr 手动剥壳 | 0 | 中 | 边界情况多 |
| C: 增强 strstr | 0 | 低 | 转义引号误判 |

**推荐**: 保持 strstr 手动解析，不引入 cJSON。原因:
- cJSON 对 32KB payload 需分配 ~48KB(~1500 节点 x 32 字节)，碎片化后可能失败
- strstr 零分配、O(n) 扫描 32KB 约 0.1ms
- 对嵌套 JSON，用 strstr 定位 `"mediaInfoEx"` → 找冒号后 `{` 或 `"` → 提取子串即可

**嵌套剥壳实现** (strstr 方案):
```c
// 1. 先搜 "mediaInfoEx" / "mediaInfo" / "metadata" 键
const char *inner = strstr(p_str, "\"mediaInfoEx\"");
if (!inner) inner = strstr(p_str, "\"mediaInfo\"");
if (!inner) inner = strstr(p_str, "\"metadata\"");
if (inner) {
    // 2. 跳到冒号后的值
    inner = strchr(inner, ':'); if (inner) inner++;
    while (*inner == ' ') inner++;
    // 3. 如果值是引号开头(stringified JSON)，跳过引号找匹配闭引号
    // 4. 如果值是 { 开头(Object)，找匹配闭括号
    // 5. 用 inner 作为新的 p_str 继续 strstr 搜字段
}
```

**复杂度**: 3/5 | **增量**: ~40 行 | **风险**: 中(嵌套引号边界)

### G.4 媒体 EOF generation 感知

**修改位置**: miplay.c:1902 `m_cleanup` 标签

**方案**: 在 m_cleanup 处加 generation 分支:
```c
m_cleanup:
    if (s_media_generation != generation) {
        // 被新会话替换，最小清理，不回调
        free(rtp_buf);
        close(media_sock);
        if (rtsp_sock_to_close >= 0) close(rtsp_sock_to_close);
        ESP_LOGI(TAG, "[MEDIA] Replaced by newer session, minimal cleanup");
        vTaskDelete(NULL);
        return;
    }
    // 正常退出或错误 — 触发 pipeline_stop
    if (s_media_cb) s_media_cb(false);
    free(rtp_buf);
    close(media_sock);
    if (rtsp_sock_to_close >= 0) close(rtsp_sock_to_close);
    if (s_image_sock >= 0) { close(s_image_sock); s_image_sock = -1; }
    s_image_port = s_multi_port = 0;
```

**generation-replaced 时跳过**: pipeline_stop, ring buffer 排空, s_image_sock 关闭
**必须执行**: free(rtp_buf), close(media_sock), close(rtsp_sock)

**注意**: s_image_sock 在 generation-replaced 时不能关闭(新会话可能已重新绑定)

**复杂度**: 2/5 | **增量**: ~15 行 | **风险**: 低

### G.5 cover_keys 补字段

**修改位置**: miplay.c:3077

**方案**: 补 `"artworkUrl"` 和 `"coverUrl"`
```c
// 改前
static const char *cover_keys[] = {"\"mCoverUrl\"", "\"mArt\"", "\"artwork\"", "\"cover\"", NULL};
// 改后
static const char *cover_keys[] = {"\"mArt\"", "\"mCoverUrl\"", "\"artwork\"", "\"artworkUrl\"", "\"coverUrl\"", NULL};
```

优先级对齐 FusionPlay: mArt > mCoverUrl > artwork > artworkUrl > coverUrl

**复杂度**: 1/5 | **增量**: 1 行 | **风险**: 无

### P0 依赖拓扑与执行顺序

```
G.3 (JSON剥壳) ──→ G.2 (提取 parse_set_media_info，内含剥壳)
G.1 (base64去空白) ── 独立
G.4 (EOF generation) ── 独立
G.5 (cover_keys) ── 独立
```

建议: **G.1 → G.5 → G.3 → G.2 → G.4**

---

## 附录 H: P1 进度同步与反向控制适配方案

### H.1 SET_POSITION 解析

**修改位置**: miplay.c:3011 (拆出 CMD_SET_POSITION 单独处理)

**方案**: body 前 8 字节 big-endian u64 = position_ms。防御: `plen < 8` 时跳过。去重: `pos_ms > s_media_position + 250 || pos_ms < s_media_position` 才更新。

**存储**: 新增 `static volatile uint64_t s_media_position = 0`
**UI 通知**: 新增 `np_set_position(uint32_t pos_ms)` 单独回调，不复用 np_meta_t(position 是高频值，混入会触发全量 UI 刷新)

**复杂度**: 1/5 | **增量**: ~15 行 | **风险**: 低

### H.2 本地进度推进 (500ms tick)

**方案**: `esp_timer_create` 创建 500ms 周期定时器:
```c
if (s_media_playing && s_media_duration > 0 && s_media_position < s_media_duration) {
    s_media_position += 500;
    if (s_media_position > s_media_duration) s_media_position = s_media_duration;
}
```

**优先级**: 手机推送 SET_POSITION 直接覆盖，本地 tick 下次推进基于新值，天然优先。
**CPU**: 回调每 500ms 一次，单次 ~0.1us，可忽略。

**依赖**: 需 H.1 的 s_media_position + H.4 的 s_media_playing

### H.3 反向控制 NOTIFY receiver-control

**新增函数**:
```c
static void send_reverse_control(enum {RC_PAUSE, RC_RESUME, RC_PREV, RC_NEXT, RC_SEEK}, uint64_t seek_ms);
```

**二进制格式**:
- boolean: `[key_len:u8][key_bytes][0x00,0x01]`
- u64: `[key_len:u8][key_bytes][0x09][value:u64BE]`
- 通过 `send_encrypted_cmd(sock, CMD_NOTIFY, s_notify_seq++, body, len)` 发送

**键名**: key-pause / key-resume / key-prev / key-next / key-seek

**复杂度**: 2/5 | **增量**: ~60 行 | **风险**: 低(栈上构造，无堆分配)

### H.4 mark_reverse_control_ready

**修改位置**:
- 新增全局: `static volatile bool s_reverse_control_ready = false`
- 标记: miplay.c:2939 (GET_MEDIA_INFO 处理) 设 true
- 重置: miplay.c:2358 (disconnect_cleanup) 设 false
- 检查: H.3 send_reverse_control 入口

**复杂度**: 1/5 | **增量**: ~5 行 | **风险**: 无

### H.5 SET_MEDIA_STATE 解析

**方案**: 4 种编码逐步尝试:
1. 4 字节 BE: `state = (payload[0]<<24)|...|payload[3]`
2. 5 字节带前缀: payload[0]==0 时从 [1..4] 取
3. JSON: strstr `"state"/"playState"/"play_state"/"mState"`，解析冒号后数值
4. state: 2=playing, 3=paused, 其他忽略

**存储**: 新增 `static volatile bool s_media_playing = false`
**UI 通知**: 新增 `np_set_playing_state(bool)`

**复杂度**: 2/5 | **增量**: ~20 行 | **风险**: 低

### H.6 SET_VOLUME (已实现)

miplay.c:2982 已完整实现 SET_VOLUME 解析(u32 BE → s_volume_percent + NOTIFY volume)。仅需:
1. 删除 :3154 中冗余的 CMD_SET_VOLUME 条件
2. 新增 `miplay_notify_volume_change()` 入口供旋钮回调

**复杂度**: 1/5 | **增量**: ~10 行

### P1 依赖拓扑

```
H.4 (ready flag) ← H.3 (reverse control)
H.1 (position)   ← H.2 (tick) ← H.5 (playing state)
H.6              ← 独立
```

建议: **H.4 → H.1 → H.5 → H.2 → H.3 → H.6**

---

## 附录 I: 封面管线优化与 DLNA 适配方案

### I.1 封面归一化

**修改位置**: dlna.c decode_base64_cover() 或 fetch_album_art_async() 入口

**方案**: PSRAM 原地操作(前移覆盖法，零额外分配):
1. 原地剔除 `\r\n\t ` (前移覆盖)
2. 检查前缀: http/https → 跳过(走 HTTP 下载路径)
3. 跳过 `data:...;base64,` 前缀
4. 校验 base64(长度%4==0, 字符合法)

**不需要拼 data: 前缀** — 我们直接解码，格式检测仅用于日志。

**复杂度**: 2/5 | **内存**: 零增量 | **增量**: ~50 行

### I.2 封面尺寸 48→80

**方案**: 改 `JPEG_MAX_COVER`(dlna.c:1407) 和 `MAX_COVER`(dlna.c:1525) 为 80

**内存**: 80x80x2=12.8KB PSRAM (当前 4.6KB, +8.2KB)
**屏幕**: ST7735 160x128, 80x80 占 50% 宽度，视觉合理
**CPU**: 双线性缩放 6400 次插值 vs 2304 次，绝对耗时 ~20ms
**不建议 96x96**: 18.4KB + 接近屏幕宽度

**复杂度**: 1/5 | **增量**: 3 行改常量

### I.3 trackId 续传 (防封面闪烁)

**方案**: np_meta_t 增加 `uint32_t track_hash`，djb2 哈希 `title+artist+album`

**np_submit 语义**:
- track_hash 变化 → 全量替换(清旧封面+下载新封面)
- track_hash 相同且 has_cover==0 → merge(保留旧 cover_url)

**复杂度**: 2/5 | **增量**: ~60 行

### I.4 LatestRequestGate (已等价)

现有 `s_album_art_gen` 代次机制与 FusionPlay LatestRequestGate **功能等价**。不需要改。

### I.5 DLNA RenderingControl (已实现)

custom_dlna.c 已有: GetVolume/SetVolume/GetMute/SetMute + GENA LastChange 推送。**零工作量**。

### I.6 DLNA DIDL-Lite 解析增强

**方案**: cb_set_metadata 增加:
1. `extract_tag("upnp:album")` → m.album (简单)
2. 从 `<res` 标签提取 protocolInfo 属性 → 采样率/位深/声道 (中等)

**复杂度**: 2/5 | **增量**: ~40 行

### I.7 DLNA 多源仲裁

**方案**: 简化版 — cb_set_uri 入口加守卫: 若 NP_SRC_MIPLAY 正在播放，仅存储 URI 不自动播放。Play 命令才真正抢占。np_set_source 在 cb_play 中调用。

**复杂度**: 2/5 | **增量**: ~30 行

### I.8 DLNA GENA 事件订阅 (已实现)

custom_dlna.c 已有: SUBSCRIBE/UNSUBSCRIBE + gena_notify + gena_task 异步推送。**零工作量**。

---

## 附录 J: ESP32 约束、测试策略与实施路线图

### J.1 ESP32 特有约束

| 约束 | 现状 | 影响 |
|------|------|------|
| 内部 SRAM | ~300KB(碎片化后更少) | JSON 解析不能用内部 heap |
| PSRAM | 8MB, 可用 ~8MB | 封面/元数据缓冲用 PSRAM |
| 单文件 | miplay.c 3526 行 | 加 200 行后 ~3700 行，暂不需拆 |
| cJSON | ESP-IDF 自带但**不推荐** | 32KB JSON ~48KB 分配，碎片化风险 |
| 实时性 | select 500ms 超时 | strstr 32KB ~0.1ms，不阻塞 |

**结论**: 保持 strstr 手动解析，不引入 cJSON。对嵌套 JSON 用 strstr 定位子串。

### J.2 测试策略

**可自动化 (gcc host 端)**:
- 0x18 嵌套 JSON 解析: `{"mediaInfo":{"mTitle":"xxx"}}` 验证提取
- base64 去空白: 含 `\r\n` 的 base64 验证解码正确
- receiver-control TLV: 验证 key-pause/key-seek 二进制格式
- NOTIFY 序列号递增: 连续调用无重复

**需手机投屏**:
- 切歌 3 次 → 标题/歌手/封面刷新
- 进度条推进 (500ms tick)
- 旋钮控制暂停/播放/上下曲
- 30 分钟 heap 监控不下降

**回归**:
- DLNA 投屏网易云/喜马拉雅不受影响
- 心跳帧仍正常解密
- heap 对比: 修改前后 esp_get_free_heap_size()

### J.3 风险评估

| 风险 | 级别 | 缓解 |
|------|------|------|
| 0x18 payload 嵌套格式差异 | 高 | 先抓日志确认格式再编码 |
| 媒体 EOF 不 stop 判断错误 | 高 | generation 检查已有保护(L2667) |
| 反向控制 NOTIFY 序列号重复 | 中 | 当前单线程调用(旋钮)，风险低 |
| base64 封面内存峰值 | 中 | PSRAM 8MB 余量充足 |
| JSON 剥壳嵌套引号边界 | 中 | 多种嵌套格式测试覆盖 |

### J.4 实施路线图

**第 1 轮: 封面修复 (预计 2h)**
- G.1 base64 去空白 (~15 行)
- G.5 cover_keys 补字段 (1 行)
- I.1 封面归一化 (~50 行)
- I.2 封面尺寸 48→80 (3 行)
- 里程碑: 投屏含封面歌曲 → 封面正确显示

**第 2 轮: 切歌刷新 (预计 3h)**
- G.3 JSON 嵌套剥壳 (~40 行)
- G.2 提取 parse_set_media_info + 0x18 复用 (~50 行)
- G.4 媒体 EOF generation 感知 (~15 行)
- I.3 trackId 续传 (~60 行)
- 里程碑: 连续切歌 → 标题/歌手/封面即时刷新

**第 3 轮: 进度与反向控制 (预计 4h)**
- H.4 reverse_control_ready 标志 (~5 行)
- H.1 SET_POSITION 解析 (~15 行)
- H.5 SET_MEDIA_STATE 解析 (~20 行)
- H.2 本地 500ms 进度推进 (~20 行)
- H.3 反向控制 NOTIFY (~60 行)
- H.6 SET_VOLUME 清理 (~10 行)
- 里程碑: 进度条推进 + 旋钮控制手机

**第 4 轮: DLNA 完善 + 回归 (预计 3h)**
- I.6 DIDL-Lite 解析增强 (~40 行)
- I.7 多源仲裁 (~30 行)
- Host 端单元测试 (~100 行)
- 集成测试 + 回归测试
- 里程碑: 全部测试通过，heap 无回退

**总计: ~530 行新增代码 + ~100 行测试，预计 12 小时**

---

## 附录 K: ESP32-S3FN16R8 内存分配深度分析

### K.1 硬件内存拓扑

```
┌──────────────────────────────────────────────────────────────────┐
│                    ESP32-S3FN16R8 内存布局                        │
├─────────────────────┬──────────────┬─────────────────────────────┤
│ 区域                │ 大小         │ 用途                        │
├─────────────────────┼──────────────┼─────────────────────────────┤
│ Internal SRAM       │ ~320 KB      │ BSS/DMA/小对象/task栈       │
│   ├─ .data + .bss   │ ~60-80 KB    │ 静态全局变量                 │
│   ├─ FreeRTOS 内核   │ ~15 KB       │ TCB/就绪表/定时器           │
│   ├─ WiFi/LwIP      │ ~50-70 KB    │ 管理帧/ARP/DHCP/控制块     │
│   ├─ DMA SRAM       │ ~8 KB        │ I2S GDMA 描述符+缓冲       │
│   ├─ Reserved pool  │ 32 KB        │ SPIRAM_MALLOC_RESERVE       │
│   └─ 剩余可用       │ ~80-100 KB   │ 小 malloc + task 栈         │
├─────────────────────┼──────────────┼─────────────────────────────┤
│ PSRAM (Octal SPI)   │ 8 MB         │ 合并入堆(malloc可返回PSRAM) │
│   ├─ GMF 音频缓冲   │ ~560 KB      │ HTTP reader 512K + TX 32K   │
│   ├─ Task 栈        │ ~60 KB       │ 7个大任务的栈               │
│   ├─ 封面下载缓冲   │ ~560 KB      │ HTTP(s) 下载峰值            │
│   ├─ now_playing    │ ~50 KB       │ cover URL 48K + 结构        │
│   ├─ LVGL + UI      │ ~150-200 KB  │ 帧缓冲 + 字体 + 对象        │
│   └─ 剩余可用       │ ~6.5 MB      │ 余量充足                    │
├─────────────────────┼──────────────┼─────────────────────────────┤
│ Flash               │ 16 MB        │ QIO 80MHz                   │
│   ├─ bootloader     │ 20 KB        │                             │
│   ├─ partition table │ 4 KB        │                             │
│   ├─ nvs            │ 24 KB        │ WiFi 配置                   │
│   ├─ phy_init       │ 4 KB         │                             │
│   ├─ factory (app)  │ 7 MB         │ 固件+资源                   │
│   └─ 未使用         │ ~9 MB        │ 可加 SPIFFS/LittleFS        │
└─────────────────────┴──────────────┴─────────────────────────────┘
```

### K.2 Internal SRAM 详细预算

Internal SRAM 是 ESP32-S3 最稀缺的资源。WiFi 驱动、DMA 描述符、FreeRTOS TCB **必须**驻留此处。

**当前 Internal SRAM BSS 静态分配 (已确认):**

| 变量 | 大小 | 文件:行 | 备注 |
|------|------|---------|------|
| `s_cur_title[128]` | 128 B | dlna.c | DLNA 当前标题 |
| `s_cur_artist[128]` | 128 B | dlna.c | DLNA 当前歌手 |
| `s_media_title[128]` | 128 B | miplay.c:197 | MiPlay 标题 |
| `s_media_artist[64]` | 64 B | miplay.c:198 | MiPlay 歌手 |
| `s_media_album[64]` | 64 B | miplay.c:199 | MiPlay 专辑 |
| `s_media_cover_url[1024]` | 1024 B | miplay.c:203 | MiPlay 封面引用 |
| `s_media_duration` | 4 B | miplay.c:200 | MiPlay 时长 |
| `s_encrypt_buf[2089]` | 2089 B | miplay.c:1143 | AES 加密缓冲(静态避免碎片) |
| `s_envelope_buf[2057]` | 2057 B | miplay.c:1144 | SafetyData 信封(静态) |
| `s_album_art_tcb` | 80-100 B | dlna.c | 静态 TCB (StaticTask_t) |
| `s_tcp_tcb` | 80-100 B | miplay.c | TCP 任务 TCB |
| `s_rtsp_tcb` | 80-100 B | miplay.c | RTSP 任务 TCB |
| `s_media_tcb` | 80-100 B | miplay.c | Media 任务 TCB |
| 其他 BSS (mutex/queue/event) | ~500 B | 分散 | FreeRTOS 内核对象 |
| **BSS 合计** | **~6.6 KB** | | |

**Internal SRAM Task 栈 (动态分配 xTaskCreate):**

| Task | 栈大小 | 核心 | 备注 |
|------|--------|------|------|
| `ui_update` | 4096 B | CPU0 | LVGL UI 更新 |
| `delayed_stop_notify` | 3072 B | CPU1 | 延迟停止通知 |
| `miplay_scan` | 3072 B | CPU1 | mDNS 扫描 |
| `miplay_lan` | 4096 B | CPU0 | LAN 发现 |
| `mdns_ann` | 4096 B | CPU0 | mDNS 广播 |
| `image_drain` | 3072 B | -- | 图像排水 |
| `system_event` | 3072 B | -- | 系统事件(ESP 默认) |
| `main` | 3584 B | -- | app_main 栈 |
| WiFi task | ~4096 B | CPU0 | ESP WiFi 驱动 |
| `tiT` (LwIP) | ~4096 B | -- | LwIP TCPIP 线程 |
| `esp_timer` | 4096 B | -- | 高精度定时器 |
| NVS task | 2048 B | -- | NVS 操作 |
| **Task 栈合计** | **~44 KB** | | |

**Internal SRAM 动态 malloc (小对象，<=1024B 受 ALWAYSINTERNAL 保护):**

| 类别 | 估算 | 备注 |
|------|------|------|
| URI 字符串(strdup) | ~1-2 KB | 5-6 个 URI 各 ~200B |
| FreeRTOS 运行时分配 | ~3-5 KB | 队列/信号量/事件组内部 |
| LwIP 内部 pbuf | ~8-15 KB | TCP 窗口 29200×2 + PCB |
| WiFi 管理帧 | ~5-10 KB | 扫描/关联/密钥 |
| NVS 缓存 | ~2 KB | NVS page cache |
| mDNS 内部 | ~2-3 KB | 服务列表/应答缓冲 |
| HTTPD 会话 | ~3-4 KB | custom_dlna HTTP server |
| **动态合计** | **~25-40 KB** | |

**Internal SRAM 总账:**

```
可用总量:            ~320 KB
已占用:
  BSS 静态:          -6.6 KB
  Task 栈(动态):     -44 KB
  动态小对象:        -25~40 KB
  WiFi/LwIP:         -50~70 KB (含溢出到 PSRAM 的)
  FreeRTOS 内核:     -15 KB
  DMA SRAM:          -8 KB
  Reserved pool:     -32 KB (SPIRAM_MALLOC_RESERVE)
  ─────────────────────────
  已占用:            -181~216 KB
  剩余可用:          ~104~139 KB ← 余量尚可但非充裕
```

> `✶ Insight ─────────────────────────────────────`
> **为什么 ALWAYSINTERNAL=1024 很关键**: FreeRTOS 的 semaphore/event group 内部结构
> 通常 20-80 字节，如果堆分配器把它们送到 PSRAM，CPU 访问需要经过 SPI 总线，
> 在中断上下文中会崩溃（PSRAM 不可在 ISR 中访问）。1024 阈值确保所有内核同步
> 原语留在快速内部 SRAM。
>
> **SPIRAM_MALLOC_RESERVE_INTERNAL=32768 的作用**: 为 WiFi 驱动的运行时分配预留
> 空间。WiFi 扫描时临时分配 ~10-15KB 管理帧，如果没有保留池，会因内部 SRAM
> 耗尽返回 NULL，导致 WiFi 断联。
> `─────────────────────────────────────────────────`

### K.3 PSRAM 详细预算

PSRAM 通过 `CONFIG_SPIRAM_USE_MALLOC=y` 合并入系统堆，`malloc()` 对 >1024B 的
分配自动导向 PSRAM。

**PSRAM 大块分配清单:**

| 组件 | 分配量 | 生命周期 | 核心路径 |
|------|--------|----------|----------|
| GMF HTTP reader 输出缓冲 | **512 KB** | 常驻 | 音频流 |
| GMF codec_dev TX 输出 | 32 KB | 常驻 | 音频输出 |
| GMF HTTP reader task 栈 | 8 KB | 常驻 | 网络读取 |
| GMF dlna_audio pipeline 栈 | 16 KB | 常驻 | 解码+输出 |
| GMF miplay_audio pipeline 栈 | 16 KB | 常驻 | 解码+输出 |
| album_art worker 栈 | 8 KB | 按需(生命周期=下载+解码) | 封面 |
| miplay_tcp task 栈 | 6 KB | 常驻(MiPlay 激活时) | 控制协议 |
| miplay RTSP task 栈 | 8 KB | 按需(投屏期间) | RTSP |
| miplay media task 栈 | 16 KB | 按需(投屏期间) | 媒体流 |
| now_playing cover URL | **48 KB** | 按需(有封面时) | 元数据 |
| album art HTTP 下载缓冲 | **~512 KB** | 临时(下载期间峰值) | 封面 |
| album art base64 解码输出 | ~36 KB | 临时 | 封面 |
| album art 缩放输出 | 4.5 KB | 临时 | 封面 |
| lodepng 内部分配 | ~100-200 KB | 临时(PNG 解码时) | 封面 |
| MiPlay RTP 接收缓冲 | 4 KB | 按需 | 媒体流 |
| LVGL 帧缓冲 | ~150 KB | 常驻 | 显示 |
| LVGL 对象+字体堆 | 32 KB | 常驻 | 显示 |
| CJK 字体数据 | ~200-300 KB | 常驻(Flash mmap) | 显示 |
| mDNS 内部分配 | ~10-20 KB | 常驻 | 发现 |
| **常驻合计** | **~1.37 MB** | | |
| **峰值临时** | **~900 KB** | | |
| **PSRAM 总峰值** | **~2.27 MB** | | |
| **PSRAM 剩余** | **~5.73 MB** | | ← 余量非常充足 |

> `✶ Insight ─────────────────────────────────────`
> **封面下载是 PSRAM 峰值杀手**: `simple_http_get` 初始分配 256KB，每次 realloc
> +128KB，一张 500KB 的 JPEG 下载过程中会产生 ~768KB 的临时分配（256+128×4）。
> 加上 lodepng 解码一张 1000×1000 PNG 需要 ~4MB 原始像素缓冲（RGBA），
> 峰值可达 ~5MB。好在 PSRAM 有 8MB，但仍需注意。
>
> **为什么 lodepng 重定向到 PSRAM**: 原始 PNG 解码产生 width×height×4 字节的
> RGBA 输出，1000×1000 PNG = 4MB。如果 lodepng 默认用内部 SRAM，320KB 直接爆。
> `─────────────────────────────────────────────────`

### K.4 DMA SRAM 约束分析

ESP32-S3 的 DMA 描述符和 DMA 访问的缓冲必须在 Internal SRAM 中（DMA 不可穿越
PSRAM 的 SPI 总线）。

| DMA 用途 | 大小 | 状态 |
|----------|------|------|
| I2S GDMA TX 描述符 | ~512 B | esp_codec_dev 管理 |
| I2S GDMA TX 数据缓冲 | ~16-32 KB | 在 codec_dev_tx_out_buf 内 |
| SPI DMA (显示) | ~4 KB | tft_display 驱动 |
| GDMA AES (已释放) | ~0 B | **软件 AES 替代后不再占用** |
| **合计** | **~20-36 KB** | |

> `✶ Insight ─────────────────────────────────────`
> **软件 AES 的真正收益**: 之前硬件 GDMA AES 需要 ~4KB DMA 描述符缓冲 + 共享
> DMA 通道仲裁。切换纯软件 FIPS-197 AES-128-CBC 后，不仅释放了 DMA 通道，
> 还消除了 GDMA 中断与 I2S DMA 的竞争——之前偶发的 I2S 欠载（audio underrun）
> 有一部分就是 DMA 通道仲裁导致的。
> `─────────────────────────────────────────────────`

### K.5 堆碎片化风险评估

ESP32-S3 的堆管理器按 first-fit 分配，长时间运行后大小交替的 malloc/free 会
产生碎片。本项目的碎片化风险来源：

**高风险（已缓解）:**

| 风险 | 缓解措施 | 状态 |
|------|----------|------|
| MiPlay 任务反复创建/销毁 | `xTaskCreateStatic` + PSRAM 栈，永不 free | ✓ 已缓解 |
| 加密缓冲 malloc/free 交替 | `s_encrypt_buf`/`s_envelope_buf` 静态化 | ✓ 已缓解 |
| 封面下载 realloc 扩张 | 一次性 `heap_caps_malloc(MALLOC_CAP_SPIRAM)` | ✓ 已缓解 |
| lodepng 频繁小分配 | 重定向到 PSRAM，不影响内部 SRAM | ✓ 已缓解 |

**中等风险（需关注）:**

| 风险 | 原因 | 建议 |
|------|------|------|
| URI strdup/free 抖动 | 每次切歌 5-6 个 URI 交替分配释放 | 用固定缓冲区替代 strdup |
| cJSON 解析器分配 | 如果引入 cJSON，每次解析 ~2-5KB 小分配 | **使用 strstr 替代 cJSON**（已决策） |
| HTTP client 内部缓冲 | esp_http_client 每次请求分配 ~2-4KB | 持久化 client handle 避免反复创建 |
| LwIP pbuf 碎片 | 大量小 TCP 段累积 | 已设 TCP_WND=29200 缓解 |

**低风险:**

| 风险 | 原因 |
|------|------|
| LVGL 对象分配 | 32KB 专用池，自管理，不碰系统堆 |
| NVS 操作 | 固定 page 大小，无碎片风险 |
| mDNS 分配 | 已重定向到 PSRAM |

### K.6 提议的 ~530 行新代码内存影响评估

逐项评估方案中每项改动的内存开销：

**Round 1: 封面 base64 + WebP (预计 ~120 行)**

| 改动 | Internal SRAM | PSRAM | 备注 |
|------|---------------|-------|------|
| `cover_decode_and_show()` 函数 | 0 (代码段在 Flash) | 0 | Flash mmap |
| base64 whitespace stripping | 0 | 0 | 原地过滤 |
| WebP 分支 `WebPGetInfo`+`WebPDecodeRGBA` | 0 | ~100-200 KB | lodepng 已有此模式 |
| 抽象缩放函数 | 0 | 0 | 代码复用，不增加内存 |
| **净增** | **0** | **0-200 KB 临时** | 解码完即释放 |

**Round 2: 切歌刷新 (预计 ~165 行)**

| 改动 | Internal SRAM | PSRAM | 备注 |
|------|---------------|-------|------|
| `parse_set_media_info()` 函数 | 0 (Flash) | 0 | 代码段 |
| JSON 嵌套剥壳循环 | ~256 B 栈上临时 | 0 | 在现有函数栈帧内 |
| `apply_media_info()` np_update | 0 | 0 | 写入现有 BSS |
| generation 感知 EOF | 0 | 0 | 一个 int 比较 |
| **净增** | **~0** | **0** | 几乎零内存开销 |

**Round 3: 进度 + 反向控制 (预计 ~130 行)**

| 改动 | Internal SRAM | PSRAM | 备注 |
|------|---------------|-------|------|
| `reverse_control_ready` 标志 | 1 B (BSS) | 0 | |
| SET_POSITION 解析 | 0 | 0 | strstr 无分配 |
| 500ms 进度 timer | ~100 B | 0 | esp_timer 句柄 |
| NOTIFY 反向控制报文 | ~200 B 栈 | 0 | 构建在栈上发送 |
| **净增** | **~300 B** | **0** | |

**Round 4: DLNA 完善 (预计 ~70 行)**

| 改动 | Internal SRAM | PSRAM | 备注 |
|------|---------------|-------|------|
| DIDL-Lite 解析增强 | 0 | 0 | strstr 扩展 |
| 多源仲裁逻辑 | 0 | 0 | 条件判断 |
| **净增** | **0** | **0** | |

**总内存影响汇总:**

```
新增代码段 (Flash):    ~530 行 ≈ 4-6 KB → 存 Flash mmap，不占 SRAM
新增 Internal SRAM:    ~300 B (BSS 标志 + timer)
新增 PSRAM 常驻:       0 (所有解码缓冲临时分配，用完即释放)
新增 PSRAM 峰值临时:   ~200 KB (WebP 解码，与现有 PNG/JPEG 同模式)
──────────────────────────────────────────────────
结论: 内存影响可忽略不计
```

> `✶ Insight ─────────────────────────────────────`
> **为什么 strstr 方案比 cJSON 省内存**: cJSON 每解析一个 JSON 节点分配一个
> `cJSON` 结构体(32 字节)。一个典型 MiPlay mediaInfo JSON 含 ~15 个字段 →
> ~480 字节内部 SRAM 分配 + 树遍历的递归栈帧(~200 字节/层 × 5 层 = 1KB)。
> 而 strstr 只在栈上做字符串匹配，零堆分配。对 ESP32-S3 而言，这个选择
> 省了 ~1.5KB 内部 SRAM + 消除了碎片化风险。
>
> **Flash mmap 的好处**: ESP32-S3 的 XIP (Execute-In-Place) 从 Flash 直接执行
> 代码，不需要把代码加载到 SRAM。530 行 C 代码编译后约 4-6KB 的 .text 段，
> 全部在 Flash 上运行，对 SRAM 零影响。
> `─────────────────────────────────────────────────`

### K.7 关键内存瓶颈与对策

**瓶颈 1: Internal SRAM ~100KB 余量**

- 现状: WiFi 峰值（扫描+关联）可临时消耗额外 ~15KB
- 风险: 如果同时有多个网络连接 + HTTPD 响应 + mDNS 查询，可能逼近极限
- 对策:
  1. 已有的 `SPIRAM_MALLOC_RESERVE_INTERNAL=32768` 保护
  2. 不引入任何新的 Internal SRAM 常驻分配（方案已遵守）
  3. 所有新缓冲走 PSRAM（方案已遵守）
  4. 如未来需要更多余量，可将 `s_media_cover_url[1024]` 移到 PSRAM

**瓶颈 2: 封面下载 PSRAM 峰值 ~5MB**

- 现状: HTTP 下载 512KB + lodepng 解码 4MB = 峰值 ~5MB
- 风险: 如果此时 GMF 音频缓冲也在满载(512KB+32KB)，总峰值 ~5.5MB
- 对策:
  1. 下载完成后立即释放下载缓冲，再进入解码
  2. 不同时下载两张封面（album_art_queue 深度=1，xQueueOverwrite 覆盖）
  3. WebP 解码用 `WebPDecodeRGBA` 直接输出目标尺寸，不先解全尺寸再缩放
  4. 可选: 限制封面下载最大尺寸（如 2MB），超出跳过

**瓶颈 3: LVGL 字体 Flash 占用**

- 现状: CJK 字体（Simsun 16）可能占 ~200-300KB Flash
- 风险: 7MB app 分区已使用约 3-4MB，字体增加后仍在 7MB 限制内
- 对策: Flash 16MB 且 app 分区 7MB，余量 ~3MB，无需优化

**瓶颈 4: 两套音频管线共存时的 PSRAM 消耗**

- 现状: DLNA 和 MiPlay 各有独立的 GMF pipeline，但只有一条活跃
- 风险: 切换源时旧管线的缓冲还未释放、新管线已开始分配
- 对策:
  1. `on_miplay_media_start` 先调 `s_media_cb(false)` 停止旧管线
  2. 旧管线 `pipeline_stop` + `pipeline_destroy` 释放全部缓冲
  3. 新管线 `pipeline_create` + `pipeline_run` 再分配
  4. 交替过程 <100ms，PSRAM 余量充足，不会 OOM

### K.8 内存监控建议

开发阶段建议在 `ui_update` 任务中添加周期性内存水位日志：

```c
// 每 30 秒打印一次内存水位 (DEBUG 用)
if (++mem_tick % 30000 == 0) {
    multi_heap_info_t int_info, ext_info;
    heap_caps_get_info(&int_info, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&ext_info, MALLOC_CAP_SPIRAM);
    ESP_LOGI("MEM", "INT free=%lu/%lu  PSRAM free=%lu/%lu  "
             "int_min_free=%lu",
             int_info.free_bytes, int_info.total_bytes,
             ext_info.free_bytes, ext_info.total_bytes,
             heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}
```

关键指标:
- `int_min_free` < 50KB → 内部 SRAM 余量告警，排查是否有不必要的内部分配
- `ext_info.free_bytes` < 2MB → PSRAM 余量偏低，检查封面解码是否泄漏
- `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` < 16KB → 碎片化严重

### K.9 结论

| 维度 | 评估 | 置信度 |
|------|------|--------|
| Internal SRAM 余量 | ~100-140 KB，安全 | 高 |
| PSRAM 余量 | ~5.7 MB，非常充足 | 高 |
| DMA SRAM | 软件 AES 已释放，无压力 | 确定 |
| 堆碎片化风险 | 低，已有多项缓解措施 | 高 |
| 新代码内存影响 | ~300B 内部 + 0 常驻 PSRAM，可忽略 | 确定 |
| 封面峰值压力 | ~5MB/8MB PSRAM，可控 | 中(取决于源端封面大小) |

**总体判断: ~530 行方案对 S3FN16R8 内存零压力。** 所有新缓冲分配遵循
"PSRAM 临时分配 + 用完释放" 模式，不增加 Internal SRAM 常驻负担。
唯一需要关注的是极端情况下封面解码的 PSRAM 峰值，通过限制下载大小或使用
WebP `WebPDecode` 直接缩放可完全消除。

---

## 附录 L: 代码级 Bug 精确定位（逐行走查）

> 基于 miplay.c 0x12 解析逻辑 (:3066-3149)、media EOF (:1899-1903)、
> disconnect_cleanup (:2358-2372)、0x18 ALONE (:2548-2551)、
> dlna.c base64 解码 (:1274-1308)、now_playing.c np_submit (:76-96) 的逐行走查。
> 与 FusionPlay Rust `protocol.rs:emit_media_info` + `media_info_value` 对照。

### L.1 BUG-1: JSON 嵌套壳未剥 — P0 致命 (切歌不刷新的主因)

**位置**: `miplay.c:3071-3123`

**现象**: MiPlay 切歌后标题/歌手不更新

**根因**: HyperOS 发送的 0x12 payload 结构是嵌套的:
```json
{"mediaInfo":{"mTitle":"新歌","mArtist":"新歌手","mArt":"..."}}
// 或
{"metadata":"{\"mTitle\":\"新歌\",\"mArtist\":\"新歌手\"}"}
```
ESP32 直接在外层 JSON 上做 `strstr("\"mTitle\"")`，找到后 `strchr(t, ':')`
跳到值。但嵌套壳内的字段实际在第二个 `{` 之后，当前代码**有可能命中外层无关
字段或解析位置偏移**。当 metadata 是字符串化的 JSON 时，strstr 完全找不到字段。

**FusionPlay 做法** (`protocol.rs:2167-2181`):
```rust
fn media_info_value(body: &[u8]) -> Option<Value> {
    let value = parse_json_payload(body)?;
    for key in ["mediaInfo", "mediaInfoEx", "metadata"] {  // ← 三层剥壳
        match value.get(key) {
            Some(Value::Object(nested)) => return Some(Value::Object(nested.clone())),
            Some(Value::String(nested)) => {  // metadata 可能是字符串化的 JSON
                if let Ok(parsed) = serde_json::from_str(nested) {
                    return Some(parsed);
                }
            }
            _ => {}
        }
    }
    Some(value)  // 无壳则用原值
}
```

**修复方向**: 在 `miplay.c:3071` 解析前，先检测并跳过嵌套壳:
```c
// 剥壳: 跳过 "mediaInfo":{ / "mediaInfoEx":{ / "metadata":{
const char *inner = p_str;
static const char *shells[] = {"\"mediaInfo\"", "\"mediaInfoEx\"", "\"metadata\""};
for (int shell = 0; shell < 3; shell++) {
    const char *found = strstr(inner, shells[shell]);
    if (found) {
        const char *colon = strchr(found, ':');
        if (colon) {
            colon++;
            while (*colon == ' ' || *colon == '"') colon++;
            if (*colon == '{') inner = colon;  // 壳内是 object
            // 如果是 string 化的 JSON，需要找到对应闭引号后解码
        }
    }
}
p_str = inner;
```

**预估改动**: ~30 行

### L.2 BUG-2: cover_keys 缺失关键字段 — P1 重要 (封面不显示原因之一)

**位置**: `miplay.c:3077`

**当前代码**:
```c
static const char *cover_keys[] = {
    "\"mCoverUrl\"", "\"mArt\"", "\"artwork\"", "\"cover\"", NULL
};
```

**FusionPlay 对照** (`protocol.rs:2234-2236`):
```rust
let artwork = json_string_field(&value,
    &["mArt", "mCoverUrl", "artwork", "artworkUrl", "coverUrl"])
```

**差异**: ESP32 缺少 `"artworkUrl"` 和 `"coverUrl"` 两个字段名。
HyperOS 某些版本（尤其 1.1.6+）使用 `artworkUrl` 而非 `mArt`。
Kotlin 端 (`XiaomiPlaybackEvent.kt:93`) 也额外尝试 `artwork_url`:
```kotlin
artworkUrl = string("artwork_url") ?: string("artwork")
```

**修复**: 加上缺失字段:
```c
static const char *cover_keys[] = {
    "\"mCoverUrl\"", "\"mArt\"", "\"artwork\"", "\"artworkUrl\"",
    "\"coverUrl\"", "\"cover\"", NULL
};
```

**预估改动**: 1 行

### L.3 BUG-3: 0x18 ALONE_SET_MEDIA_INFO 空壳 — P1 重要 (切歌丢失元数据)

**位置**: `miplay.c:2548-2551`

**当前代码**:
```c
case 0x18: /* ALONE_SET_MEDIA_INFO */
    ESP_LOGI(TAG, "AloneSetMediaInfo seq=%u", seq);
    send_encrypted_cmd(client_sock, cmd + 1, seq, NULL, 0);  // ← 只回 ACK
    break;
```

**问题**: HyperOS 在某些场景下通过 0x18 (outer_type=0x04) 发送元数据更新，
payload 结构与 0x12 相同。当前代码完全忽略 payload。

**FusionPlay 对照** (`protocol.rs:1192`): 0x12 和 0x18 都调用
`emit_media_info(&frame.body, &events)`。

**修复方向**: 复用 0x12 的解析逻辑，提取公共函数 `parse_media_info()`:
```c
case 0x18: /* ALONE_SET_MEDIA_INFO */
    ESP_LOGI(TAG, "AloneSetMediaInfo seq=%u len=%lu", seq, (unsigned long)plen);
    if (plen > 0) {
        parse_media_info(client_sock, payload, plen);  // 复用 0x12 逻辑
    }
    send_encrypted_cmd(client_sock, cmd + 1, seq, NULL, 0);
    break;
```

**预估改动**: ~10 行 (含提取公共函数的重构)

### L.4 BUG-4: media EOF 无 generation 感知 — P1 重要 (切歌管线错误停止)

**位置**: `miplay.c:1899-1903`

**当前代码**:
```c
if (s_media_generation != generation)
    ESP_LOGI(TAG, "[MEDIA] Replaced by newer session (gen %lu -> %lu)", ...);
m_cleanup:
    if (s_media_cb) s_media_cb(false);  // ← 无条件停止管线！
    free(rtp_buf);
    close(media_sock);
```

**问题**: 当 HyperOS 切歌时，旧 media task 的 socket 收到 EOF 退出循环。
此时 `s_media_generation` 已更新（新 session），但旧 task 仍执行
`s_media_cb(false)` **无条件停止管线**——而新 session 的管线可能已启动。

**FusionPlay 对照** (`media.rs`): 三层错误分支:
1. generation-replaced → 静默退出（不通知 UI，不触发 stop 回调）
2. 正常 EOF → 通知停止
3. 错误 → 通知停止 + 日志

**修复方向**:
```c
m_cleanup:
    if (s_media_generation != generation) {
        ESP_LOGI(TAG, "[MEDIA] Stale session cleanup, skip stop callback");
    } else {
        if (s_media_cb) s_media_cb(false);
    }
    free(rtp_buf);
    close(media_sock);
    ...
```

**预估改动**: 5 行

### L.5 BUG-5: disconnect_cleanup 不清媒体缓冲 — P2 中等

**位置**: `miplay.c:2358-2372`

**当前代码只清除密钥和回调，不清除**:
- `s_media_title[128]`
- `s_media_artist[64]`
- `s_media_album[64]`
- `s_media_cover_url[1024]`
- `s_media_duration`

**问题**: 手机断连后重连，旧元数据残留。GetMediaInfo(0x14) 回读返回过时信息。

**修复方向**: 在 disconnect_cleanup 中清零:
```c
memset(s_media_title, 0, sizeof(s_media_title));
memset(s_media_artist, 0, sizeof(s_media_artist));
memset(s_media_album, 0, sizeof(s_media_album));
memset(s_media_cover_url, 0, sizeof(s_media_cover_url));
s_media_duration = 0;
```

**预估改动**: 5 行

### L.6 BUG-6: base64 解码前未剥离空白 — P1 重要 (封面 base64 永不解码)

**位置**: `dlna.c:1287-1297`

**当前代码**:
```c
size_t encoded_len = strlen(encoded);
// ...
int ret = mbedtls_base64_decode(decoded, capacity, &decoded_len,
                                (const unsigned char *)encoded, encoded_len);
```

**问题**: `mbedtls_base64_decode` 遇到空白字符（`\n`, `\r`, ` `）返回错误。
小米妙播的 mArt 字段是 **RFC 2045 行折叠 base64**——每 76 字符一个 `\n`。
当前代码不剥离，decode 直接失败 → 封面永远不显示。

**FusionPlay 对照** (`XiaomiPlaybackSnapshotReducer.kt:29`):
```kotlin
val encoded = source.filterNot(Char::isWhitespace)  // ← 先去所有空白
```

**修复方向**: 在 `decode_base64_cover` 内，base64_decode 之前:
```c
// 原地剥离空白（encoded 指向 source 内部，需要就地压缩）
size_t clean_len = 0;
char *clean = heap_caps_malloc(encoded_len + 1, MALLOC_CAP_SPIRAM);
if (!clean) { heap_caps_free(decoded); return -1; }
for (size_t i = 0; i < encoded_len; i++) {
    if (encoded[i] != ' ' && encoded[i] != '\n' &&
        encoded[i] != '\r' && encoded[i] != '\t')
        clean[clean_len++] = encoded[i];
}
clean[clean_len] = '\0';
encoded = clean;  // 后续用 clean 替代
encoded_len = clean_len;
// ... decode ...
heap_caps_free(clean);  // 解码完成后释放
```

**预估改动**: ~15 行

### L.7 BUG-7: s_media_cover_url[1024] 容量不足 — P2 中等

**位置**: `miplay.c:203`

**当前**: `static char s_media_cover_url[1024]`

**问题**: base64 编码的封面可达 ~50-100KB。1024 字节只够存短 HTTP URL。
当前缓解: `miplay.c:3114-3121` 用 PSRAM `heap_caps_malloc` 存完整 base64
到 `pending_cover`，绕过此限制。但 GetMediaInfo(0x14) 回读仍用截断的
`s_media_cover_url`。

**修复方向**: GetMediaInfo 回读时优先用 `np_get_cover_url()` 获取完整数据。

**预估改动**: ~10 行

### L.8 BUG-8: duration 不处理引号包裹的数字 — P3 低

**位置**: `miplay.c:3125-3129`

**当前代码**:
```c
s_media_duration = (uint32_t)atol(d);
```

**问题**: 如果 JSON 中 `"duration":"180000"` (字符串数字)，`atol` 从 `"` 开始
返回 0。FusionPlay (`protocol.rs:2160-2163`) 同时处理数字和字符串数字。

**修复**: 跳过 `:` 后的引号:
```c
if (d) { d = strchr(d, ':'); if (d) { d++; while (*d == ' ') d++;
    if (*d == '"') d++;  // ← 跳过开引号
    s_media_duration = (uint32_t)atol(d); }}
```

**预估改动**: 1 行

### L.9 Bug 汇总表

| 编号 | 严重度 | 文件:行 | 描述 | 预估改动 |
|------|--------|---------|------|----------|
| BUG-1 | **P0** | miplay.c:3071 | JSON 嵌套壳未剥，strstr 错位 | ~30 行 |
| BUG-2 | **P1** | miplay.c:3077 | cover_keys 缺 artworkUrl/coverUrl | 1 行 |
| BUG-3 | **P1** | miplay.c:2548 | 0x18 ALONE_SET_MEDIA_INFO 空壳 | ~10 行 |
| BUG-4 | **P1** | miplay.c:1903 | media EOF 无 generation 感知 | 5 行 |
| BUG-5 | P2 | miplay.c:2358 | disconnect 不清媒体缓冲 | 5 行 |
| BUG-6 | **P1** | dlna.c:1287 | base64 不剥离空白，行折叠 base64 永远失败 | ~15 行 |
| BUG-7 | P2 | miplay.c:203 | cover_url 缓冲 1024B 不够 base64 | ~10 行 |
| BUG-8 | P3 | miplay.c:3128 | duration 不处理引号数字 | 1 行 |

**总修复量**: ~80 行，覆盖切歌不刷新 (BUG-1/3/4) + 封面不显示 (BUG-2/6) 两大核心问题。

### L.10 Bug 与症状的因果链

```
症状 1: MiPlay 切歌标题/歌手不刷新
  ├─ BUG-1 (P0): JSON 嵌套壳 → strstr 解析错位 → 新标题未提取
  ├─ BUG-3 (P1): 0x18 空壳 → 部分切歌路径元数据丢失
  └─ BUG-4 (P1): media EOF 无 generation → 旧 task 停掉新管线

症状 2: MiPlay 封面自始至终不显示
  ├─ BUG-6 (P1): base64 行折叠 → mbedtls 解码失败 → 封面加载返回 -1
  ├─ BUG-2 (P1): cover_keys 缺 artworkUrl → 部分源封面字段未匹配
  └─ BUG-7 (P2): cover_url 缓冲截断 → GetMediaInfo 回读不完整
```

### L.11 修复优先级排序

**第 1 批 (必须一起修，否则互相掩盖)**:
1. BUG-1 — JSON 剥壳 (~30 行)
2. BUG-2 — cover_keys 补全 (1 行)
3. BUG-6 — base64 空白剥离 (~15 行)

**第 2 批 (独立可测)**:
4. BUG-4 — generation 感知 EOF (5 行)
5. BUG-3 — 0x18 复用 0x12 解析 (~10 行)

**第 3 批 (清理)**:
6. BUG-5 — disconnect 清缓冲 (5 行)
7. BUG-8 — duration 引号 (1 行)
8. BUG-7 — cover_url 回读增强 (~10 行)

---

## 附录 M: NightPlayer 内存优化对照分析

> 参考项目: [night-player-fake-pod-nano](https://gitee.com/ZYFDroid/night-player-fake-pod-nano)
> 硬件相同: ESP32-S3 + 8MB Octal PSRAM + 16MB Flash + I2S DAC + LVGL UI
> 功能复杂度相当: 5000+ 曲目库、封面解码、播放列表、shuffle、记忆续播、
> 定时关机、低电量保护、四页面滑动 UI、控制中心

### M.1 内存效率对比

| 维度 | NightPlayer | 本项目 (DLNA+MiPlay) | 差距分析 |
|------|-------------|---------------------|----------|
| Internal SRAM BSS | ~65 KB | ~6.6 KB (协议) + ~44 KB (栈) | NP 更大(音频静态缓冲)但可控 |
| Internal SRAM 余量 | 30-110 KB | ~100-140 KB | 相当 |
| PSRAM 常驻 | ~500 KB | ~1.37 MB | **我们多 870KB** |
| PSRAM 峰值 | ~4 MB (扫描期) | ~2.3 MB (封面) | NP 峰值更高但场景不同 |
| 任务数 | 10 | 15+ | **我们多 5 个任务** |
| 框架开销 | 无 (直接 Helix+I2S) | GMF ~560KB | **GMF 是大头** |

### M.2 NightPlayer 的核心优化策略

**策略 1: 音频路径全静态 BSS，零堆分配**

```c
// NightPlayer: 所有音频缓冲编译时确定，不碰堆
static int16_t read_buf[8192];        // 16 KB — MP3 读缓冲
static int16_t out_buf[2304];         // 4.6 KB — Helix 单帧输出
static int16_t pcm_buf[2048];         // 4 KB — int16→int32 转换
static int32_t silence_flush[6*240*2]; // 11.5 KB — DMA 静音预填充
```

本项目 GMF 用 `heap_caps_malloc` 分配 512KB HTTP reader 缓冲 + 32KB codec TX
缓冲，都是堆分配。虽然在 PSRAM 但增加了碎片化风险。

**可借鉴**: I2S DMA silence preload 模式值得采纳——切采样率时先填静音再开通道，
避免爆音。

**策略 2: PSRAM-First 两级降级分配**

```c
// NightPlayer: cover_alloc() 统一模式
uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (buf == NULL) {
    buf = malloc(size);  // 内部 SRAM 兜底
}
```

本项目直接 `heap_caps_malloc(MALLOC_CAP_SPIRAM)` 不检查返回值或不降级。
应统一采用两级降级模式。

**策略 3: 栈水位实测定栈大小**

NightPlayer 用 `performance_monitor` 每 20 秒采样所有任务的
`uxTaskGetStackHighWaterMark()`，实测峰值后加 1-2KB margin。

本项目任务栈大小是经验值，未做系统性 HWM 审计。

**建议**: 在 `ui_update` 任务中加入 HWM 采样，运行一轮完整投屏流程后记录峰值。

**策略 4: Flash mmap 替代堆加载**

NightPlayer 4MB `pldata` 分区通过 `mmap` 映射为只读内存访问，播放列表
零堆分配。本项目 CJK 字体已在 Flash mmap（`CONFIG_LV_FONT_SIMSUN_16_CJK=y`），
做法一致。

**策略 5: 任务通知替代队列**

NightPlayer 封面加载用 `xTaskNotifyGive` / `ulTaskNotifyTake` 单值通知，
比队列更轻量（零拷贝、无内存分配）。本项目封面用 `xQueueOverwrite` 也接近，
但 `strdup(url)` 仍有一次堆分配。

**策略 6: 非阻塞命令发送**

```c
// NightPlayer: UI 线程永远不阻塞
if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
    ESP_LOGW(TAG, "command queue full, drop cmd=%d", (int)cmd);
}
```

本项目 DLNA 回调从 HTTPD 线程调用 `np_submit`，如果 now_playing 回调里
`lvgl_port_lock` 等待时间长，会阻塞 HTTPD 线程。应检查是否有此风险。

**策略 7: 任务固定核心亲和性**

| 核心 | NightPlayer | 本项目 |
|------|-------------|--------|
| CPU0 | LVGL(8K), 按键(4K), deep_worker(8K), player_ctrl(4K) | LVGL(4K), miplay_scan(3K), miplay_lan(4K), mDNS(4K), WiFi |
| CPU1 | mp3_play(8K), mp3_out(4K), cover_load(6K) | album_art(8K), miplay_tcp(6K), RTSP(8K), media(16K), GMF×2 |

NightPlayer 把网络/WiFi 固定在 CPU0，音频固定在 CPU1。本项目也是
WiFi(CPU0)+音频(CPU1)，但 MiPlay 任务分散在两个核心，跨核通信开销更高。

### M.3 GMF 框架的内存代价

本项目最大的内存差异来自 GMF (GStreamer-like Media Framework)：

| GMF 组件 | PSRAM 消耗 | NightPlayer 等价物 | 消耗 |
|----------|-----------|-------------------|------|
| HTTP reader 输出缓冲 | **512 KB** | 无 (本地文件) | 0 |
| HTTP reader 任务栈 | 8 KB | 无 | 0 |
| dlna_audio pipeline 栈 | 16 KB | mp3_play 栈 | 8 KB |
| codec_dev TX 输出缓冲 | 32 KB | 无 (直接 i2s_channel_write) | 0 |
| miplay_audio pipeline 栈 | 16 KB | 无 | 0 |
| **合计** | **584 KB** | | **8 KB** |

GMF 的 512KB HTTP reader 缓冲是为网络抖动设计的——这在 DLNA 流媒体场景
下是**必要的**。NightPlayer 不需要因为它读本地 SD 卡。

**结论**: 560KB GMF 开销不可削减，但可以优化:
1. 两个音频管线 (DLNA + MiPlay) 不会同时活跃，可共享一个 pipeline 实例
2. HTTP reader 512KB 缓冲是 `sdkconfig` 常量，可在低内存场景降到 256KB

### M.4 可立即采纳的优化

| 编号 | 优化项 | 来源 | 改动量 | 收益 |
|------|--------|------|--------|------|
| O.1 | PSRAM-First 两级降级分配 | NightPlayer cover_alloc | ~20 行 | OOM 容错 |
| O.2 | HWM 栈水位审计日志 | NightPlayer performance_monitor | ~30 行 | 栈安全 |
| O.3 | 非阻盖 np_submit + 日志 | NightPlayer command queue | ~5 行 | HTTPD 不阻塞 |
| O.4 | Silence preload 切采样率 | NightPlayer audio.c | ~15 行 | 切歌无爆音 |
| O.5 | Release 关闭调试日志 | NightPlayer sdkconfig | 1 行 | 省 ~2KB 栈 |

**O.1 PSRAM-First 两级降级**: 对 `simple_http_get`、`decode_base64_cover`、
lodepng 分配统一加 `malloc()` 降级分支。

**O.2 HWM 审计**: 在 `ui_update` 中每 30 秒打印关键任务 HWM:
```c
TaskHandle_t h = xTaskGetHandle("miplay_tcp");
if (h) ESP_LOGI("HWM", "miplay_tcp=%lu", uxTaskGetStackHighWaterMark(h));
```

**O.4 Silence preload**: `miplay_pipeline_start()` 中切采样率后，
在 `i2s_channel_enable` 之前调 `i2s_channel_preload_data()` 填充静音。
