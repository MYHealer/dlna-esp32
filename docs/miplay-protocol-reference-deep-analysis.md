# MiPlay 参考实现深度分析 — FusionPlay.MiPlaySdk

> 日期：2026-08-30
> 分析对象：`E:\Downloads\miplay\FusionPlay-main\FusionPlay-main\src\FusionPlay.MiPlaySdk\`（protocol.rs / media.rs / lib.rs / lyra.rs / discovery.rs）
> 用途：为 ESP32（`e:\ESP\dlna`）的 MiPlay 实现提供协议级参考；与现有实现做逐项对照，输出整改核对清单。
> 配套文档：`MiPlay-Bug-Report-33-lyrics-sync.md`（歌词/位置同步）、`MiPlay-Bug-Report-34-metadata-cover-review.md`（切歌元数据/封面评审）、`docs/miplay-companion-socket-problem-report.md`（companion 问题）。

---

## 1. 总体架构：一个连接 = 一个完整会话

参考实现的**核心设计**（protocol.rs:872-934）：

- 专用 listener 线程在 8899 端口非阻塞 accept；**每个新 TCP 连接 → 独立 session_id → 独立线程 `run_session()`**。
- 每会话独立 `ControlCipher`（reader 一个、writer 一个），密钥由本地/远端 socket 端点派生（`generate_auth_key(local, remote)`，protocol.rs:1076）。
- 会话之间只通过 `ControlHub` 共享最小化状态：
  - `sessions`（session_id → shutdown stream 句柄）
  - `control_routes`（session_id → {remote_ip, 发送通道, 通知序列号, reverse_control_ready, play_source_registered}）
  - `active`（当前活动媒体会话，含 `PlaybackGate`）
  - `stream_keys`（按 remote_ip 共享的流密钥）
  - `remote_effects`（反向控制"发出→等待手机回执"队列）

**与 ESP32 的本质差异**：参考实现**没有全局静态加密态**，主/伴两个连接天然并发、互不阻塞；ESP32 用共享静态 `s_aes_key/s_encrypt_iv/s_decrypt_iv` + companion 任务 save/restore（`s_in_companion` 期间主连接被冻结）。这是 #34 P0 的架构根源。

## 2. 帧格式与加密信封

帧头 9 字节（read_frame，protocol.rs:1783-1822）：

```
0x24('$') | outer_type:u8 | command:u8 | value_type:u16 BE(=sequence) | length:u32 BE
```

关键事实：

- **length 取全 4 字节**。参考注释明确：旧解析把 value_type 高位当保留零、只取 length 后 2 字节，当 `value_type=0x0100` 时会把有效会话误判断链（protocol.rs:1791-1797）。
- 加密 body 判定：`body.starts_with([0x00,0x07,0x01,0xe0])`。
- 信封格式：`[00 07 01 E0] + pad(1B) + integrity(4B BE) + ciphertext`。
- **integrityType=1** = 反字节序 CRC-32（查表多项式 0x04c11db7，起值 0xffffffff，**最终不异或**）。手机在解密前就校验 integrity（protocol.rs:1897-1900）：随机占位会产生手机静默关闭连接的症状。
- CBC IV 连锁：IV 初始 = key；每帧加密后 IV = 密文末 16 字节；解密先取 next_iv 再解。
- Padding：零填充（非 PKCS#7），长度记于信封 byte[4]。

**ESP32 对照**（miplay.c:2434-2438）：`cmd = buf[1..2]`（含 outer 高字节）、`seq = buf[3..5]`、`plen = buf[5..9]` 完整 4B BE —— 与参考 read_frame **逐字段一致 ✓**，无需改动（注意未来不要退回 2 字节 length 读取）。

## 3. 握手状态机（顺序即生命线）

完整时序（protocol.rs:1211-1348，注释中标注了"手机实测会翻脸"的细节）：

| 步骤 | 方向/命令 | 参考实现行为 |
|---|---|---|
| 1 | C→S `GET_VERSION` | 回 `GET_VERSION_ACK`（版本+`\0`） |
| 2 | C→S `AUTH_ACK`(0x29) | **此刻才发 NOTIFY 5/6/7**（mode / mediaInfoEx / state）。注释：AUTH_ACK 之前乐观发送会令手机在 SafetyAuth 后丢弃设备（protocol.rs:1218-1233） |
| 3 | C→S `SafetyInfo`（未加密） | 回 `SafetyInfoAck`（plain wrapper, key="ack"，含 aesIvType=2/aesKeyType=1/authAlgorithmType=4/integrityType=1）+ **立即发我们的 `SafetyAuth` challenge**（encrypted wrapper, key="cmd"） |
| 4 | C→S `SafetyAuth`（peer challenge） | 计算 `authMsgAck = HMAC-SHA256(auth_key, peer_authMsg)`，**暂不回复**，存入 pending |
| 5 | C→S `SafetyAuthAck`（对我的 challenge 的 ack） | 校验 HMAC 与期望一致 → **才回我们最终的 `SafetyAuthAck`**（wrapper key="ack"） |
| 6 | 安全通道建立 | 随后 `GET_DEVICE_INFO / SET_DEVICE_INFO / GET_MIRROR_MODE / SET_MIRROR_KEY / GET_MEDIA_INFO …` |

要点：**每次会话（含 companion）都走同一套握手**；`EncryptedWrapper` 必须保留外层 0x14 wrapper 标记（发送 0x00 会被手机在兼容性探测中拒绝，protocol.rs:1722-1728）。

**ESP32 对照**：与 4.4 系列已验证的流程一致；重点核对 NOTIFY 5/6/7 是否严格在 `AUTH_ACK`(0x29) 之后发送。

## 4. 各命令应答细节（对照表）

| 命令 | 参考实现应答（protocol.rs） | ESP32 现状 | 结论 |
|---|---|---|---|
| `GET_STATE`(0x16) | pre-OPEN 回 idle(0)；**OPEN 后回真实 playing(2)/paused(3)**（1440-1451）；注释：永远回 idle 会让 HyperOS 丢弃状态转换 | 固定回 `{0,0,0,0,0}`（miplay.c:2510） | ⚠️ 待改 |
| `SET_VOLUME` | **ACK 空体**（status-only），随后 NOTIFY type-7 `volume` 发布实际值（1589-1609，encode_set_volume_ack 返回空） | ACK 回 5 字节音量（miplay.c:3010-3015） | ⚠️ 待核对真机 |
| `GET_VOLUME` | ACK `[0, percent:u32 BE]`（2292-2297） | 5 字节同格式 | ✓ |
| `OPEN`（加密） | 回 `{0,0,0,0,0}`；必要时补发 NOTIFY(空 mediaInfoEx)；`begin_media_session` + `activate` + spawn RTSP（1469-1552） | 已实现 | ✓ |
| `GET_MEDIA_INFO`(0x14) | **不回 0x15**，回 unsolicited NOTIFY mediaInfoEx，安装反向控制回调（1637-1652） | 已实现（miplay.c:2958-2988） | ✓ |
| `SET_MEDIA_INFO` / `0x04xx ALONE` | 任何会话都 observe 到活动 source；ACK 保留 0x04 高字节 | 主/伴两路均处理 ✓ | ✓ |
| `HEART_BEAT`(0x1A) | 必须回 `0x1B` 同 seq；注释：反向控制通知后手机会立即发心跳，不回会造成 pause/next 挂起并拆路由（1427-1439） | 已实现 | ✓ |
| `GET_MIRROR_MODE` | 回 Mode 1（主动音频流路由）ACK（1383-1392） | 已实现 | ✓ |
| 反向控制(k-pause/k-next) | NOTIFY 发出后**等手机状态回执确认**（send_confirmed + remote_effects 队列） | 裸发，无确认 | 低优先 |

## 5. 播放/位置状态机（#33 B3/B4 的原版方案）

- **PlaybackGate**（media.rs:48-123）：三维暂停——`source_paused`（显式命令）与 `output_suspended`（系统持续）合并为 effective paused。
- **暂停 stale 帧防护**：`POSITION_RESUME_SETTLING_WINDOW = 1.5s`——暂停后 1.5s 内的 position 帧**不算** resume 证据（`accepts_weak_resume`）。
- **position 推进判定**：`advancing = pos > prev + 250ms`（protocol.rs:612-616）才作为弱恢复证据。
- **强暂停 vs 弱恢复**：显式 `PAUSE` / RTSP PAUSE → `observe_playback`（强门控）；`SET_MEDIA_STATE` 快照与 position 帧只是"恢复暗示"（observe_playback_snapshot：*"HyperOS replays cached paused frames while restoring a route… not local mute authority"*，protocol.rs:554-579）。
- **状态解码**（decode_playback_state, protocol.rs:2260-2283）：4B BE / `[0]+4B` / JSON key `setState|state|mediaState|playState`；2=播放，3=暂停 —— ESP32 解析与其一致 ✓。

**对 ESP32 的结论**：位置只能由发送方 `SET_POSITION` 帧驱动，本地时钟仅用于帧间插值；暂停必须冻结并留 1.5s 静默窗口。对应修复：`miplay_notify_position` 末尾刷新 `s_play_start_us`；`miplay_notify_play_state(false)` 清零 `s_play_start_us`。

## 6. 元数据 / 切歌（与"封面歌曲信息"直接相关）

- **歌词字段**：`mLrc` 及其别名 `lyrics_text / lyrics_uri / lyrics_url / lyrics / lyric` —— 按当前决定**留占位、暂不解析**。
- **切歌识别**：`observe_media_info`（protocol.rs:581-605）比较 `media_identity(body)` 变化 → `track_revision++` 并**清空 last_position**——即"换歌"由元数据 identity 判定，不是命令类型。
- **companion = 同一 source**：`active_source_session_id`（protocol.rs:739-745）按 **remote_ip 归并**；切歌元数据即使从 companion 路由先于新 OPEN 到达也会照常发布（protocol.rs:1615-1636）。
- **反向控制路由选择**：`preferred_control_route`（protocol.rs:290-305）按 `reverse_control_ready / play_source_registered / updated_at / is_active` 挑最优路由——通知不绑定"OPEN 会话"，而是绑定 best route。

## 7. 通知序列号管理

- NOTIFY 5/6/7 为未加密能力声明；**加密通知从 8 开始**（`FIRST_ENCRYPTED_NOTIFICATION_SEQUENCE`），每路由独立递增（protocol.rs:798, 1107-1113）。
- ESP32 全局 `s_notify_seq = 8` 起步 ✓（多连接并发语义略不同，低优先）。

## 8. 对 ESP32 的整改核对清单（按价值排序）

| # | 项 | 优先级 | 对应修改 |
|---|----|--------|----------|
| 1 | `GET_STATE` 回真实播放状态（2/3）而非固定空闲 | P1 | 对齐 protocol.rs:1440-1451 |
| 2 | `SET_VOLUME` ACK 改空体，值走 NOTIFY 发布 | P1 | 对齐 protocol.rs:1589-1609；需真机确认 |
| 3 | 位置模型：帧间插值 + 暂停冻结 + 1.5s 静默窗口 | P1 | `miplay_notify_position` / `miplay_notify_play_state`（#33 F2） |
| 4 | companion 运行期间仍应答 `0x1A` 心跳 | P1 | `s_in_companion` 分支加心跳豁免（#34 P0） |
| 5 | 帧头 length 4 字节读取保持（勿退化） | 提示 | 已对齐 ✓ |
| 6 | 反向控制回执确认机制 | 低优先 | 可选移植 send_confirmed |

## 9. 参考文件索引

- `src/FusionPlay.MiPlaySdk/src/protocol.rs` — 控制协议：会话/握手/命令/加密/CRC/hub
- `src/FusionPlay.MiPlaySdk/src/media.rs` — 媒体会话：RTSP 拉流、PlaybackGate、位置观察
- `src/FusionPlay.MiPlaySdk/src/lib.rs` — SDK 入口与事件
- `src/FusionPlay.MiPlaySdk/src/lyra.rs` — Lyra 相关（音频编解码侧）
- `src/FusionPlay.MiPlaySdk/src/discovery.rs` — mDNS 发现
- `tools/miplay-lyrics-sender-probe/` — 歌词发送端探针（验证 mLrc 传输链路）