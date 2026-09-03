# MiPlay Companion Socket 问题报告

## 项目背景

ESP32-S3 实现小米妙播(MiPlay)投屏接收端。音频播放正常，但**元数据(曲名/封面)和播放状态(暂停同步)不工作**。

## 协议架构（逆向 FusionPlay Rust 源码确认）

MiPlay 使用两个 TCP 连接到端口 8899：

```
主连接 (Main Connection):
  手机 → ESP32: SafetyAuth握手 → OPEN → RTSP → 音频流(RTP/TS/AAC)
  手机 → ESP32: SET_POSITION(0x0056) 每秒一次，8字节大端序位置
  手机 → ESP32: HEARTBEAT(0x001A) 每5秒一次
  手机 → ESP32: CMD_SET_MEDIA_INFO(0x0012) 仅在OPEN阶段发一次

伴侣连接 (Companion Socket):
  手机 → ESP32: 独立TCP连接，独立SafetyAuth握手（不同四元组→不同AES密钥）
  手机 → ESP32: ALONE_SET_MEDIA_INFO(外层类型0x04, 子命令0x18) — JSON元数据
  手机 → ESP32: ALONE_SET_STATE(外层类型0x04, 子命令0x14) — 播放/暂停状态
  处理完后关闭
```

**关键引用**: FusionPlay `protocol.rs:1141`:
> "HyperOS uses this 0x04xx family on a companion socket while the ordinary OPEN/media stream remains on the source session."

## 当前问题

### 问题1: Companion Socket 未被正确处理

**症状**: 串口日志中从未出现 `[COMPANION] Listen socket readable` 或 `Companion accepted`。

**已确认的事实**:
- 主连接的 `select()` 包含 `s_listen_sock`（监听套接字）
- 当新连接到达时，`select()` 应返回并触发 companion 处理逻辑
- 实际日志只显示一个连接：`=== Client: 192.168.1.179:XXXXX ===`
- 元数据通过主连接的 `CMD_SET_MEDIA_INFO`(0x0012) 传了一次（初始投屏时）
- 切歌后无新元数据到达

**可能原因**:
1. Companion socket 到达时被主连接的 `handle_client` 的 `break` 逻辑丢弃（已修复为 spawn task）
2. Companion socket 从未连接（手机侧行为）
3. Companion socket 连接但数据被主连接消费（FD 重叠）

### 问题2: 切歌后元数据不更新

**症状**: 第一首歌的曲名正确显示，切歌后曲名不变。

**根因分析**:
- `CMD_SET_MEDIA_INFO`(0x0012) 只在 OPEN 阶段发送一次
- 切歌时手机不发新的 `CMD_SET_MEDIA_INFO`
- 切歌的元数据更新依赖 companion socket 的 `ALONE_SET_MEDIA_INFO`(0x0418)
- Companion socket 未到达 → 元数据不更新

**日志证据**:
```
[901550] miplay: [MEDIA-INFO] cmd=0x0012 seq=34 payload={"mTitle":"","mDuration":0,...}
[905859] miplay: [MEDIA-INFO] cmd=0x0012 seq=42 payload={"mTitle":"不眠之夜","mDuration":138109,...}
# 之后只有 POSITION(0x0056) 和 HEARTBEAT(0x001A)，无新元数据
```

## 已实现的修复（当前代码）

### 1. Companion Task (`miplay_companion_task`)
```c
// 独立 FreeRTOS 任务处理 companion socket
static void miplay_companion_task(void *arg) {
    // 保存主连接加密状态
    // s_in_companion = true  (跳过主连接帧处理)
    // handle_client(comp_sock)  (复用完整 SafetyAuth + ALONE 流程)
    // 恢复主连接加密状态
    // s_in_companion = false
}
```

### 2. 加密互斥 (`s_crypto_mutex`)
```c
// 保护 send_encrypted_cmd / send_encrypted_envelope / safety_decrypt
static SemaphoreHandle_t s_crypto_mutex;

// send_encrypted_cmd 中:
if (s_crypto_mutex) xSemaphoreTake(s_crypto_mutex, portMAX_DELAY);
safety_encrypt(..., s_aes_key, s_encrypt_iv, s_encrypt_buf, ...);
if (s_crypto_mutex) xSemaphoreGive(s_crypto_mutex);
```

### 3. Listen Socket Handler
```c
if (FD_ISSET(s_listen_sock, &rfds)) {
    ESP_LOGI(TAG, "[COMPANION] Listen socket readable, accepting...");
    int comp_sock = accept(s_listen_sock, ...);
    // 检查 s_in_companion 防止重复
    // 分配 PSRAM 栈，spawn companion task
    continue;  // 主连接 select 循环继续
}
```

### 4. ALONE 命令处理（已有，在 switch(outer_type==0x04) 中）
```c
case 0x18: // ALONE_SET_MEDIA_INFO
    // 解析 JSON: mTitle, mArtist, mCoverUrl, mDuration
    // np_set_source(NP_SRC_MIPLAY)
    // np_set_meta() → 触发 UI 更新回调
    // np_set_cover_url() → 触发封面下载
    break;
case 0x14: // ALONE_SET_STATE
    // 解析 4字节大端(2=playing, 3=paused) 或 JSON
    // miplay_notify_play_state()
    break;
```

### 5. 主连接帧跳过保护
```c
if (s_in_companion) {
    // 跳过帧处理，避免与 companion task 的加密缓冲区竞争
    buf_used -= total;
    memmove(buf, buf + total, buf_used);
    continue;
}
```

## 串口日志（最新一次投屏）

```
[55591] === Client: 192.168.1.179:47444 ===
[55618] -> DEVICE_ID(0x28): 47866723311293 seq=4
[55725] → SafetyAuth challenge sent
[55787] SafetyAuth challenge from phone
[55844] SafetyAuthAck from phone (verifying...)
[55876] Mutual SafetyAuth complete!
[55885] Sending deferred SafetyAuthAck seq=2
[55917] NP: Source changed → 2, epoch=1
[56317] GetMediaInfo seq=9 (title=)
[56542] GetMediaInfo seq=11 (title=)
[56992] GetMediaInfo seq=14 (title=)
[57620] GetMediaInfo seq=17 (title=)
# 无 "COMPANION" 日志
# 无 OPEN/RTSP 日志（日志截断或尚未到达）
```

## 分析结论与答复（v1, 2026-08-30，已核对 FusionPlay 源码 + 当前工作区代码）

### Q1: Companion socket 为什么没到达？手机是否真的会建立第二个 TCP 连接？

**会。** 参考实现多处确认 HyperOS 会用第二个 TCP 连接承载 `0x04xx` ALONE 命名空间命令与切歌元数据：

- [protocol.rs:1141](file:///E:/Downloads/miplay/FusionPlay-main/FusionPlay-main/src/FusionPlay.MiPlaySdk/src/protocol.rs#L1141)：*"HyperOS uses this 0x04xx family on a companion socket while the ordinary OPEN/media stream remains on the source session."*
- protocol.rs:1188：*"Track metadata is often sent on a companion socket before the replacement media OPEN reaches us."*
- 单测 `companion_control_sessions_share_the_active_phone_source`（protocol.rs:2454）验证了 companion 路由共享活动 source。

**但关键在下一句**：参考实现 **不依赖**“第二个连接必须来”。协议层把 `0x0012 / 0x0418 / NOTIFY mediaInfoEx` 在**任何连接**（主 / 伴）上都合并进同一个 source（protocol.rs:1615-1636）：*"Treat both sockets as one source so a track change cannot be filtered merely because its SET_MEDIA_INFO arrived on the non-OPEN session."* —— 元数据早于替代媒体流到达，且不能因来源连接不同而被过滤。

**日志中无 `[COMPANION]` 行的可解释原因（按概率排序）**：
1. **固件与源码不一致**：companion 支持是**未提交**的工作区改动（`git status` 显示 miplay.c 为 M），本次抓日志的固件很可能不含该逻辑；
2. 该次会话只停留在 GetMediaInfo 探测阶段（切歌未发生，手机未开 companion 连接）；
3. 手机在**主连接**上直接发 `0x04xx` 帧 —— ESP32 主连接 switch 里其实有 `outer_type==0x04` 分支（[miplay.c:2485](file:///e:/ESP/dlna/components/miplay/miplay.c#L2485)），若发生会打印 `AloneSetMediaInfo`，日志里没有，说明那次会话连 ALONE 帧都没发。

> 结论：先确认烧录固件 == 当前源码（含 uncommitted 改动），再下"companion 不工作"的定论。

### Q2: 如果 companion socket 确实存在，为什么 select() 没检测到？

FusionPlay 用**专用 accept-loop 线程**（protocol.rs:872-934：`miplay-control-listener` 循环 accept，每个连接 → 独立 session_id → 独立线程 `run_session`）。ESP32 把 accept 折进主会话 select（[miplay.c:2340-2399](file:///e:/ESP/dlna/components/miplay/miplay.c#L2340-L2399)）**本身可行**，但有三个硬前提：

1. 固件确实包含该逻辑（见 Q1）；
2. **companion 连接必须复走完整 SafetyAuth 握手** —— companion 任务（miplay.c:2189-2255）复用 `handle_client` 整套状态机（DEVICE_ID→VERSION→AUTH→SafetyAuth…）。若 HyperOS 对 companion 连接走简化握手或先发控制帧而**不发握手序列**，handle_client 会永久阻塞在等握手帧，表现为"companion 接受后什么都没发生"；
3. `s_in_companion` 期间主连接帧被整体丢弃（[miplay.c:2453-2458](file:///e:/ESP/dlna/components/miplay/miplay.c#L2453-L2458)），心跳无人应答 —— companion 会话一旦存活 >5s，主连接会被手机判死。

参考实现无此问题：每会话独立 `ControlCipher`（各自加解密），独立线程，互不阻塞、无共享静态态。

### Q3: 切歌时元数据更新的正确路径是什么？

参考结论（protocol.rs:1624-1635）：**两路都算，一路都不能丢** —— 切歌元数据既可能从 companion `0x0418` 来，也可能从主连接 `0x0012` 来；*"Xiaomi may publish the next track before its replacement OPEN/session state"*，所以元数据必须在新的媒体流 OPEN 之前就 forward 出去。

对 ESP32 的修正方向：
- **不要假设"0x0012 只在 OPEN 阶段发一次"**（本报告问题2 的假设不成立）；
- 主连接 `SET_MEDIA_INFO`(0x0012)、companion `0x0418`、`NOTIFY mediaInfoEx` 三处都必须把元数据喂进同一个 Now Playing 状态机（当前已统一走 `np_set_meta`，链接是对的）；
- 真正缺的是"新歌检测 → 清旧歌词 → 装载新歌词"的挂接（见 Bug Report #33 的 B2/F1）：`on_np_meta_changed`（[dlna.c:2122](file:///e:/ESP/dlna/main/dlna.c#L2122)）现在只改标题/歌手/时长。

### Q4: FusionPlay 的 companion 处理机制是什么？

结构（protocol.rs:872-1030）：

```
listener 专用线程（accept 循环，非阻塞）
  └─ 每个新 TCP 连接 → 独立 session_id → 独立线程 run_session()
       ├─ 每会话独立 ControlCipher（由 local/remote socket 端点派生 key）
       ├─ 0x04xx 命名空间在会话读取循环内、低字节匹配之前处理（1145，防 0x0404 误判为 PAUSE）
       ├─ ACK 保留 0x04 高字节（Outgoing::encrypted_namespaced）
       └─ ControlHub（按 remote IP 合并）：ALONE_SET_STATE/MEDIA_INFO/POSITION
            → observe 到 active_source_session_id(session_id, remote.ip())
            → companion 断开自动回退主会话（单测 2494）
```

GetState 返回活动 source 的当前播放状态（ESP32 现在固定回 `{0,0,0,0,0}` 空闲态，[miplay.c:2510](file:///e:/ESP/dlna/components/miplay/miplay.c#L2510) 是错的，应回 DLNA/MiPlay 当前状态）。

**对 ESP32 的启示**：最稳妥是改成"独立 accept 任务 + 每连接轻量会话"，用 `s_crypto_mutex` 保护而非整段保存/恢复静态全局加密态（当前 [miplay_companion_task](file:///e:/ESP/dlna/components/miplay/miplay.c#L2189-L2255) 的 save/restore 方案有握手阻塞 + 心跳断连两个风险）。

### 补充发现

- `canAlonePlayCtrl=0 / alonePlayCapacity=0`（[miplay.c:1193-1194](file:///e:/ESP/dlna/components/miplay/miplay.c#L1193-L1194)）：与参考 PC 构建声明一致（protocol.rs:2066-2067），但参考单测（protocol.rs:3059）证明**手机仍会**在 companion 路由上探测 standalone 命名空间 → 不能靠这两个 flag 规避，必须处理 `0x04xx`。
- ESP32 对 `GET_MEDIA_INFO` 已用 unsolicited `NOTIFY mediaInfoEx` 应答（[miplay.c:2958-2988](file:///e:/ESP/dlna/components/miplay/miplay.c#L2958-L2988)），与 FusionPlay（protocol.rs:1637-1652）对齐 ✓。
- 与 Bug Report #33 的关联：companion 元数据不通 → 切歌曲名/封面不变（本报告）；即使打通，**歌词装载（#33 B1/B2）与位置漂移/暂停不冻结（#33 B3/B4）仍是独立缺陷**。

### 建议修复集（与 #33 合并执行）

| 优先级 | 改动 | 对应缺陷 |
|---|---|---|
| P0-1 | 独立 accept-loop 任务 + 每连接轻量 ALONE 会话（不跑完整握手、不整段 save/restore 静态态，用 `s_crypto_mutex`）；GetState 回真实播放状态 | 本报告 Q2/Q4 |
| P0-2 | 主/伴任何连接来的元数据 → 同一 now-playing 状态机；`on_np_meta_changed` 做新歌检测 → 清旧歌词 + 装载新歌词 | 本报告 Q3 + #33 B1/B2 |
| P0-3 | 位置以手机 `SET_POSITION` 帧为准，暂停冻结、1.5s 静默窗口防 stale 帧 | #33 B3/B4/B5 |

## 参考文件

- `components/miplay/miplay.c` — 主实现 (~3600行)
- `E:\Downloads\miplay\FusionPlay-main\` — FusionPlay Rust 参考实现
  - `src\FusionPlay.MiPlaySdk\src\protocol.rs` — 协议常量和命令处理
- `components\now_playing\now_playing.c` — 元数据薄接口层
- `main\dlna.c` — UI 回调 + 封面/歌词处理

## 环境

- 芯片: ESP32-S3 N16R8 (PSRAM 8MB)
- IDF: v5.5.4 (master branch)
- 手机: HyperOS (小米/红米)
- 端口: TCP 8899 (控制), UDP 5353/5355 (mDNS), TCP 8554 (RTSP)
