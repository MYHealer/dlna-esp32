/*
 * MiPlay 小米妙播 ESP32-S3 适配
 *
 * 基于 tv-miplay-receiver-interface.md 逆向：
 * - 帧格式: 9字节大端序 '$' + cmd(u16) + seq(u16) + len(u32) + payload
 * - 握手: Challenge(0x0028/0x0029) → Version(0x0036/0x0037) → SafetyInfo(0x1400/1401) → SafetyAuth(0x1402/1403)
 * - mDNS: _miplay_lan._tcp + _mi-connect._udp
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"  /* vTaskDeleteWithCaps — WithCaps 任务自删除必须用这个,
                                      * 普通 vTaskDelete(NULL) 会永久泄漏 PSRAM 栈+TCB
                                      * (每次投歌 ~188KB, N 次后 PSRAM 耗尽随机崩溃) */
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include <sys/time.h>
#include "esp_netif.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "mbedtls/aes.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_wifi_types.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sys.h"
#include <fcntl.h>
#include "miplay.h"
#include "now_playing.h"

static const char *TAG = "miplay";

/* ── mDNS 服务名（ESP-IDF 不自动加下划线，必须手动包含 _）── */
#define MIPLAY_LYRA_SERVICE     "_lyra-mdns"
#define MIPLAY_LYRA_PROTO       "_udp"
#define MIPLAY_MICON_SERVICE    "_mi-connect"
#define MIPLAY_MICON_PROTO      "_udp"
#define MIPLAY_LAN_SERVICE      "_miplay_lan"
#define MIPLAY_LAN_PROTO        "_tcp"

/* ── TCP 端口 ── */
#define MIPLAY_CONTROL_PORT     8899
#define MIPLAY_COAP_PORT        56666

/* ── MiLink fields ── */
#define MIPLAY_DEV              "2"    /* Rust: dev=2 required for HyperOS */
#define MIPLAY_SEC              "2"
#define MIPLAY_VERSION          "196608"

/* ── MiPlay 命令帧常量（9字节大端序）── */
#define MIPLAY_FRAME_MAGIC      0x24
#define MIPLAY_FRAME_HDR_LEN    9

/* ── 命令 ID ── */
#define CMD_OPEN_DEVICE         0x0000
#define CMD_PAUSE               0x0004
#define CMD_RESUME              0x0006
#define CMD_SET_VOLUME          0x000C
#define CMD_GET_VOLUME          0x000E
#define CMD_GET_MEDIA_INFO      0x0014
#define CMD_GET_MEDIA_INFO_ACK  0x0015   /* 对齐参考: 媒体信息查询应答 */
#define CMD_GET_STATE           0x001C
#define CMD_HEARTBEAT           0x001A
#define CMD_HEARTBEAT_ACK       0x001B
#define CMD_GET_DEVICE_INFO     0x001E
#define CMD_GET_DEVICE_INFO_ACK 0x001F
#define CMD_SAFETY_CHALLENGE    0x0028
#define CMD_SAFETY_ACK          0x0029
#define CMD_NOTIFY              0x0022
#define CMD_GET_MIRROR_MODE     0x0034
#define CMD_GET_MIRROR_MODE_ACK 0x0035
#define CMD_NATIVE_VERSION      0x0036
#define CMD_NATIVE_VERSION_ACK  0x0037
#define CMD_SET_PLAY_SOURCE     0x0040
#define CMD_SET_POSITION        0x0056
#define CMD_SET_MEDIA_INFO      0x0012
#define CMD_SET_MEDIA_INFO_ACK  0x0013
#define CMD_SET_MEDIA_STATE     0x005E
#define CMD_SET_MEDIA_STATE_ACK 0x005F
#define CMD_SET_LOCAL_DEV_INFO  0x0058
#define CMD_SET_LOCAL_DEV_ACK   0x0059
#define CMD_SAFETY_INFO         0x1400
#define CMD_SAFETY_INFO_ACK     0x1401
#define CMD_SAFETY_AUTH         0x1402
#define CMD_SAFETY_AUTH_ACK     0x1403

/* ── SafetyData v1 常量 ── */
#define SAFETY_DATA_HDR_LEN     9
#define SAFETY_DATA_VERSION     1
#define SAFETY_DATA_FLAGS       0xE0  /* encryption | padding | integrity */
#define SAFETY_VALUE_TYPE       30
#define AES_BLOCK_LEN           16
#define CRC32_MPEG2_POLY        0x04C11DB7

/* ── 版本字符串 ── */
#define RECEIVER_VERSION        "2.1.5071614"
#define CHALLENGE_SEQ           0

/* ── 连接状态回调（MiPlay 连接时通知 DLNA 暂停）── */
static miplay_connected_cb_t s_connected_cb = NULL;

/* ── TCP 监听 ── */
static TaskHandle_t s_tcp_task = NULL;
static TaskHandle_t s_rtsp_task = NULL;
static volatile bool s_running = false;
static volatile bool s_connected = false;  /* SafetyAuth 握手完成后 true，全部断开后 false */
static int s_listen_sock = -1;

/* 接收缓冲区
 * MIPLAY_MAX_FRAME_PAYLOAD 对齐参考 esp_miplay-main:128 (64KB)：
 * HyperOS 换曲时会推送整帧内嵌 base64 WebP 封面的 SetMediaInfo（实测 ~27KB），
 * 上限 2048 会把它整帧丢弃("Frame too large")并引发帧同步错乱、封面永远缺失。
 * 注意：上限放开不等于常驻缓冲扩大——RX 常驻仍 4KB，大帧临时走 PSRAM 缓冲
 * （见 handle_client 大帧搬运逻辑），用完即释放，避免历史教训"扩缓冲断联"。 */
#define MIPLAY_MAX_FRAME_PAYLOAD    (64U * 1024U)
#define MIPLAY_RX_BUF_LEN          4096
#define MIPLAY_MEDIA_INFO_BUFFER_SIZE 8192
#define MIPLAY_COVER_SOURCE_MAX     49152   /* 48KB, 足够 data:URI base64 封面 */

/* PSRAM 堆分配宏：内部 SRAM 碎片化是断联根因，所有大缓冲走 PSRAM */
#define MIPLAY_MALLOC(sz)  heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define MIPLAY_CALLOC(n, sz) heap_caps_calloc((n), (sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define MIPLAY_FREE(p)     heap_caps_free(p)

/* 对齐参考 miplay_psram_prefer_alloc(): PSRAM 优先，失败回退内部 SRAM。
 * 元数据/封面解析不能因为 PSRAM 暂时不足就整块丢失。 */
static void *miplay_psram_prefer_alloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ESP_LOGW(TAG, "PSRAM alloc %u failed, fallback internal", (unsigned)size);
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    if (!ptr) ESP_LOGE(TAG, "alloc %u failed entirely", (unsigned)size);
    return ptr;
}

/* RTSP 任务栈大小（从 PSRAM 分配 stack+TCB） */
#define MIPLAY_RTSP_STACK_SIZE  (48 * 1024)

static EXT_RAM_BSS_ATTR char s_device_id[16];
static EXT_RAM_BSS_ATTR char s_inst_name[48];
static EXT_RAM_BSS_ATTR char s_idhash[16];
static EXT_RAM_BSS_ATTR char s_appsdata[160];
static EXT_RAM_BSS_ATTR char s_mac_b64[16];
static EXT_RAM_BSS_ATTR char s_uuid[40];
static EXT_RAM_BSS_ATTR char s_lyra_appdata[96];
static uint8_t s_mac[6];

/* ── MiPlay LAN (UDP 5355) 发现应答 ── */
#define MIPLAY_LAN_DISCOVERY_PORT 5355
#define MIPLAY_LAN_MDNS_GROUP     "224.0.0.251"
static EXT_RAM_BSS_ATTR uint8_t s_lan_response[512];       /* unicast 应答（带 question） */
static size_t s_lan_response_len;
static EXT_RAM_BSS_ATTR uint8_t s_lan_announce[512];       /* multicast 宣告（无 question, 0x8001） */
static size_t s_lan_announce_len;
static EXT_RAM_BSS_ATTR char s_lan_hostname[32];
static TaskHandle_t s_lan_task = NULL;
static int s_lan_sock = -1;

static const char b64_tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* ── 握手状态机 ── */
typedef enum {
    STATE_VERSION_EXCHANGE,      /* 等待手机版本 → 发送版本+挑战 */
    STATE_SAFETY_VERIFIED,       /* HMAC-SHA1 验证通过 */
    STATE_CRYPTO_NEGOTIATED,     /* 加密协商完成 */
    STATE_SAFETY_INFO_EXCHANGED, /* SafetyInfo 交换完成 */
    STATE_MUTUAL_AUTH,           /* SafetyAuth 互相认证 */
    STATE_ESTABLISHED,           /* 握手完成 */
} miplay_state_t;

/* ── SafetyAuth 会话密钥材料 ── */
static uint8_t s_aes_key[16];
static uint8_t s_encrypt_iv[16];  /* writer cipher IV (Rust writer_loop) */
static uint8_t s_decrypt_iv[16];  /* reader cipher IV (Rust main loop) */
static char    s_auth_key[33];       /* 完整 32 hex chars，用于 HMAC */
static uint8_t s_auth_msg[33];      /* 32 hex chars + NUL */
static volatile bool s_has_session_key = false;

/* ── SetMirrorKey 媒体流加密密钥（Rust StreamKeys） ── */
static uint8_t s_stream_key[16];    /* streamKey: 16 ASCII bytes → AES key */
static uint8_t s_stream_iv[16];     /* streamIV: 16 ASCII bytes → initial IV */
static volatile bool s_has_stream_key = false;
static char s_mirror_auth_key[33];  /* authKey: 16 ASCII bytes from SetMirrorKey, for RTSP OPTIONS auth */

/* ── 音量状态（控制会话写，音频输出读）── */
static volatile uint32_t s_volume_percent = 50;
static miplay_vol_changed_cb_t s_vol_changed_cb = NULL;

void miplay_set_vol_changed_cb(miplay_vol_changed_cb_t cb)
{
    s_vol_changed_cb = cb;
}

/* ── GMF ring buffer（media_receive_task → io_miplay → aud_dec）── */
static RingbufHandle_t s_ts_ringbuf = NULL;
static miplay_media_cb_t s_media_cb = NULL;

void miplay_set_ts_ringbuf(RingbufHandle_t rb)
{
    s_ts_ringbuf = rb;
}

void miplay_set_media_cb(miplay_media_cb_t cb)
{
    s_media_cb = cb;
}

/* ── 反向播放状态/位置回调（对齐参考 PLAYER_STATE/POSITION 事件）── */
static miplay_play_state_cb_t s_play_state_cb = NULL;
static miplay_position_cb_t  s_position_cb   = NULL;

void miplay_set_play_state_cb(miplay_play_state_cb_t cb)
{
    s_play_state_cb = cb;
}

void miplay_set_position_cb(miplay_position_cb_t cb)
{
    s_position_cb = cb;
}

/* ── 活跃控制会话（供 receiver-control 等外部 API 使用）── */
static volatile int s_active_client_sock = -1;
static volatile int s_notify_seq = 8;
/* 媒体流收包总数，供 RTSP keepalive 诊断日志判断音频是否在流动 */
static volatile uint32_t s_media_pkt_total = 0;

/* 发送互斥锁：保护 send_encrypted_cmd 的共享缓冲区(s_encrypt_buf)、
 * CBC IV 状态(s_encrypt_iv)和序号(s_notify_seq)不被并发破坏。
 * 参考 esp_miplay-main s_send_mux，20 处 Take/Give。 */
static SemaphoreHandle_t s_send_mux = NULL;
/* ── 媒体会话 generation（每次 OPEN 递增，旧 RTSP/media task 自退出）── */
static volatile uint32_t s_media_generation = 0;

/* ── 媒体元数据（SetMediaInfo 接收，GetMediaInfo 返回）── */
static EXT_RAM_BSS_ATTR char s_media_id[64];        /* 对齐参考 */
static EXT_RAM_BSS_ATTR char s_media_audio_id[64];  /* 对齐参考 */
static EXT_RAM_BSS_ATTR char s_media_title[128];
static EXT_RAM_BSS_ATTR char s_media_artist[128];  /* 对齐参考: 64 → 128 */
static EXT_RAM_BSS_ATTR char s_media_album[128];   /* 对齐参考: 64 → 128 */
static uint32_t s_media_duration;  /* ms */
static uint32_t s_media_position;  /* ms */
static int      s_media_status;    /* 0=stopped, 2=playing, 3=paused */
/* cover 源：http URL 或 data:...;base64 引用。完整 base64 可能大，经 np_set_cover_url 交给 now_playing 动态 PSRAM，
 * 本地缓冲只存引用/前缀供 GetMediaInfo 回读。 */
static EXT_RAM_BSS_ATTR char s_media_cover_url[MIPLAY_COVER_SOURCE_MAX];
static volatile bool s_has_mirror_auth_key = false;

/* ── 基础工具 ── */
static void b64_encode_3(const unsigned char in[3], char *out)
{
    unsigned int v = (in[0] << 16) | (in[1] << 8) | in[2];
    out[0] = b64_tab[(v >> 18) & 63];
    out[1] = b64_tab[(v >> 12) & 63];
    out[2] = b64_tab[(v >> 6) & 63];
    out[3] = b64_tab[v & 63];
    out[4] = 0;
}

static void hex_to_lower(const uint8_t *src, size_t len, char *dst)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        dst[i * 2]     = hex[(src[i] >> 4) & 0x0F];
        dst[i * 2 + 1] = hex[src[i] & 0x0F];
    }
    dst[len * 2] = '\0';
}

static uint32_t get_my_ipv4(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n && esp_netif_get_ip_info(n, &ip) == ESP_OK) {
        return ip.ip.addr;
    }
    return 0x7F000001UL;
}

static void dump_hex(const uint8_t *data, int len, const char *label)
{
    char hex[160];
    int h = 0;
    for (int i = 0; i < len && i < 48; i++)
        h += snprintf(hex + h, sizeof(hex) - h, "%02X ", data[i]);
    ESP_LOGI(TAG, "[%s] %dB: %s", label, len, hex);
}

/* ── 设备身份标识 ── */
static void init_device_identity(void)
{
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    memcpy(s_mac, mac, 6);

    /* PC 原型: device_id = mac[2..5] 大写hex（后4字节） */
    snprintf(s_device_id, sizeof(s_device_id), "%02X%02X%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);
    /* inst_name: ESP32-MIPLAY(idHash前3字符)
     * Rust: idm_identity → SHA256(stable_seed) → base64url → 前3字符 = short_id
     *       idHash = base64_standard(short_id.as_bytes())  ← 关键！
     *       例: short_id="Fn_" → idHash=base64("Fn_")="Rm5f"
     * stable_seed = device_id_hex = MAC 全大写hex */
    {
        uint8_t sha256_hash[32];
        mbedtls_sha256((const unsigned char *)s_device_id, strlen(s_device_id), sha256_hash, 0);
        /* 1) base64url 前3字符 = short_id */
        static const char b64url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        uint32_t v = ((uint32_t)sha256_hash[0] << 16) |
                     ((uint32_t)sha256_hash[1] << 8) |
                     sha256_hash[2];
        char short_id[4];
        short_id[0] = b64url[(v >> 18) & 63];
        short_id[1] = b64url[(v >> 12) & 63];
        short_id[2] = b64url[(v >> 6) & 63];
        short_id[3] = 0;
        /* 2) idHash = base64_standard(short_id 的 UTF-8 字节)
         *    Rust: STANDARD.encode(short_id.as_bytes())
         *    直接把3个ASCII字符当3字节做标准Base64，不做位索引 */
        static const char b64std[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        uint32_t sv = ((uint32_t)(uint8_t)short_id[0] << 16) |
                      ((uint32_t)(uint8_t)short_id[1] << 8) |
                      (uint32_t)(uint8_t)short_id[2];
        s_idhash[0] = b64std[(sv >> 18) & 63];
        s_idhash[1] = b64std[(sv >> 12) & 63];
        s_idhash[2] = b64std[(sv >> 6) & 63];
        s_idhash[3] = b64std[sv & 63];
        s_idhash[4] = 0;
    }
    /* Rust: instance_suffix = did_hash[..10] (base64url) */
    {
        uint8_t sha256_hash[32];
        mbedtls_sha256((const unsigned char *)s_device_id, strlen(s_device_id), sha256_hash, 0);
        static const char b64url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        char suffix[11];
        for (int i = 0; i < 10; i++) {
            int byte_idx = (i * 6) / 8;
            int bit_offset = (i * 6) % 8;
            uint32_t val = ((uint32_t)sha256_hash[byte_idx] << 16);
            if (byte_idx + 1 < 32) val |= ((uint32_t)sha256_hash[byte_idx + 1] << 8);
            if (byte_idx + 2 < 32) val |= sha256_hash[byte_idx + 2];
            suffix[i] = b64url[(val >> (18 - bit_offset)) & 63];
        }
        suffix[10] = 0;
        snprintf(s_inst_name, sizeof(s_inst_name), "ESP32-MIPLAY(%s)", suffix);
    }

    /* UUID */
    {
        unsigned char md5[16];
        unsigned char uuid_src[16];
        const char salt[] = "miplay-esp32-dlna-uuid";
        unsigned char hash_input[6 + sizeof(salt)];
        memcpy(hash_input, mac, 6);
        memcpy(hash_input + 6, salt, sizeof(salt));
        mbedtls_md5(hash_input, sizeof(hash_input), md5);
        memcpy(uuid_src, md5, 16);
        uuid_src[6] = (uuid_src[6] & 0x0F) | 0x40;
        uuid_src[8] = (uuid_src[8] & 0x3F) | 0x80;
        snprintf(s_uuid, sizeof(s_uuid),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 uuid_src[0], uuid_src[1], uuid_src[2], uuid_src[3],
                 uuid_src[4], uuid_src[5], uuid_src[6], uuid_src[7],
                 uuid_src[8], uuid_src[9], uuid_src[10], uuid_src[11],
                 uuid_src[12], uuid_src[13], uuid_src[14], uuid_src[15]);
    }

    /* appsData: 与 Rust/Python 一致的硬编码值
     * 解码: 81 00 04 04 83 22 C3
     *   0x0483=1155 (Lyra 能力位入口), 0x22C3=8899 (控制端口)
     * Rust 注释: 使用 PC 端自算描述符会导致 HyperOS 设置 hasLyraBymiplay=true
     * 并拒绝将接收端加入音频 picker，所以必须用这个固定值 */
    memcpy(s_appsdata, "gQAEBIMiww==", 13);

    /* mac base64 */
    { b64_encode_3(mac, s_mac_b64); b64_encode_3(mac+3, s_mac_b64+4); s_mac_b64[8]=0; }

    /* lyra AppData — 与 PC 原型 build_app_data_binary 逐字节一致:
     * [0x00,0x40,0x15]
     * + inst_bytes = device_id 最后8hex 解码为4字节（= MAC 后4字节，即 s_mac[2..6]）
     * + [0x00,0x05,0x19,0x24]
     * + [0x10,0x01,0x03,0x0a,0x03,0x01,0xda,0xae,0x01,0x01,0x80,0x02]
     * + len(name)(1字节) + name
     * + [0x25,0x01,0x03]
     */
    {
        uint8_t appdata[64];
        int adoff = 0;
        const char *dev_name = "ESP32-MIPLAY";
        int name_len = strlen(dev_name);
        appdata[adoff++] = 0x00; appdata[adoff++] = 0x40; appdata[adoff++] = 0x15;
        appdata[adoff++] = mac[2]; appdata[adoff++] = mac[3];
        appdata[adoff++] = mac[4]; appdata[adoff++] = mac[5];
        static const uint8_t fixed[] = {
            0x00,0x05,0x19,0x24,
            0x10,0x01,0x03,0x0a,0x03,0x01,0xda,0xae,0x01,0x01,0x80,0x02,
        };
        memcpy(appdata + adoff, fixed, sizeof(fixed)); adoff += sizeof(fixed);
        appdata[adoff++] = (uint8_t)name_len;   /* 1字节长度（PC 原型） */
        memcpy(appdata + adoff, dev_name, name_len); adoff += name_len;
        appdata[adoff++] = 0x25; appdata[adoff++] = 0x01; appdata[adoff++] = 0x03;
        /* base64 编码 */
        int bi = 0;
        for (int i = 0; i + 2 < adoff; i += 3) {
            uint32_t v = (appdata[i]<<16)|(appdata[i+1]<<8)|appdata[i+2];
            s_lyra_appdata[bi++]=b64_tab[(v>>18)&63]; s_lyra_appdata[bi++]=b64_tab[(v>>12)&63];
            s_lyra_appdata[bi++]=b64_tab[(v>>6)&63];  s_lyra_appdata[bi++]=b64_tab[v&63];
        }
        int rem = adoff % 3;
        if (rem == 1) { uint32_t v=appdata[adoff-1]<<16; s_lyra_appdata[bi++]=b64_tab[(v>>18)&63]; s_lyra_appdata[bi++]=b64_tab[(v>>12)&63]; s_lyra_appdata[bi++]='='; s_lyra_appdata[bi++]='='; }
        else if (rem == 2) { uint32_t v=(appdata[adoff-2]<<16)|(appdata[adoff-1]<<8); s_lyra_appdata[bi++]=b64_tab[(v>>18)&63]; s_lyra_appdata[bi++]=b64_tab[(v>>12)&63]; s_lyra_appdata[bi++]=b64_tab[(v>>6)&63]; s_lyra_appdata[bi++]='='; }
        s_lyra_appdata[bi] = 0;
        ESP_LOGI(TAG, "lyra AppData (%d bytes): %s", bi, s_lyra_appdata);
    }

    ESP_LOGI(TAG, "identity: id=%s inst=%s idHash=%s", s_device_id, s_inst_name, s_idhash);
}

/* ── mDNS 注册 ── */
static uint32_t s_mdns_ip = 0;
static esp_err_t register_mdns_services(void)
{
    /* 设置 mDNS hostname 为 device_id，确保 A 记录存在
     * Rust: SRV target = instance_name → 需要 A 记录解析到 ESP32 IP */
    mdns_hostname_set(s_device_id);

    /* _mi-connect._udp — Rust discovery.rs build_mi_connect_txt
     * flags=CgE= (0x0a,0x01), dev=2, appsData=gQAEBIMiww== */
    mdns_txt_item_t micon_txt[] = {
        {"version", MIPLAY_VERSION}, {"apps","[5]"}, {"flags","CgE="},
        {"name","ESP32-MIPLAY"}, {"idHash",s_idhash}, {"dev",MIPLAY_DEV},
        {"sec",MIPLAY_SEC}, {"appsData",s_appsdata}, {"mac",s_mac_b64},
    };
    esp_err_t err = mdns_service_add_for_host(s_inst_name, MIPLAY_MICON_SERVICE, MIPLAY_MICON_PROTO,
        s_device_id, MIPLAY_COAP_PORT, micon_txt, sizeof(micon_txt)/sizeof(micon_txt[0]));
    if (err != ESP_OK) ESP_LOGW(TAG, "_mi-connect failed: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "Registered _mi-connect._udp dev=%s flags=CgE=", MIPLAY_DEV);

    /* _lyra-mdns._udp — Rust discovery.rs build_txt_data */
    {
        uint8_t channel = 0;
        char debug_info[128]; char ts_str[24]; char ch_str[4];
        s_mdns_ip = get_my_ipv4();
        /* Rust: encode_xiaomi_debug_ip — 中间地址段数字转标点
         * 数字 d → chr('#'+d)
         * 例: 192.168.110.38 → 192.$)+.$$.38 */
        {
            uint8_t ip[4] = { s_mdns_ip&0xFF, (s_mdns_ip>>8)&0xFF,
                              (s_mdns_ip>>16)&0xFF, (s_mdns_ip>>24)&0xFF };
            char enc1[8], enc2[8];
            for (int seg = 0; seg < 2; seg++) {
                char tmp[4]; char *out = seg==0 ? enc1 : enc2;
                uint8_t val = seg==0 ? ip[1] : ip[2];
                snprintf(tmp, sizeof(tmp), "%u", val);
                int oi = 0;
                for (int ci = 0; tmp[ci]; ci++) {
                    char c = tmp[ci];
                    if (c >= '0' && c <= '9') out[oi++] = '#' + (c - '0');
                    else out[oi++] = c;
                }
                out[oi] = 0;
            }
            snprintf(debug_info, sizeof(debug_info), "{msg:reply, ifname:STA, v4:%u.%s.%s.%u}",
                     (unsigned)ip[0], enc1, enc2, (unsigned)ip[3]);
        }
        /* TS = Unix epoch milliseconds（FusionPlay 用 SystemTime::now()） */
        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        long long ts_ms = (long long)tv_now.tv_sec * 1000LL + tv_now.tv_usec / 1000;
        snprintf(ts_str, sizeof(ts_str), "%lld", ts_ms);
        snprintf(ch_str, sizeof(ch_str), "%d", channel);

        mdns_txt_item_t lyra_txt[] = {
            {"AppData",s_lyra_appdata},{"MediumType","8192"},{"CH",ch_str},
            {"DebugInfo",debug_info},{"TS",ts_str},
        };
        err = mdns_service_add_for_host(s_device_id, MIPLAY_LYRA_SERVICE, MIPLAY_LYRA_PROTO,
            s_device_id, 5353, lyra_txt, sizeof(lyra_txt)/sizeof(lyra_txt[0]));
        if (err != ESP_OK) ESP_LOGW(TAG, "_lyra-mdns failed: %s", esp_err_to_name(err));
        else ESP_LOGI(TAG, "Registered _lyra-mdns._udp CH=%d", channel);
    }

    /* _miplay_lan 已弃用（Rust discovery.rs: "已弃用的 _miplay_lan 路由"）
     * 不注册，避免与 _mi-connect 产生重复 picker 行 */

    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * MiPlay LAN (UDP 5355) 发现应答
 * ── 手机 SystemUI 不走 mDNS 5353，而是向 UDP 5355 发 _miplay_lan._tcp
 *    PTR 查询，需要返回 legacy unicast DNS 响应（含 PTR+SRV+TXT+A）
 * ══════════════════════════════════════════════════════════════════════ */

static const uint8_t s_service_qname[] = {
    0x0b, '_','m','i','p','l','a','y','_','l','a','n',
    0x04, '_','t','c','p',
    0x05, 'l','o','c','a','l',
    0x00
};
#define SERVICE_QNAME_LEN sizeof(s_service_qname)

static inline void dns_push_u16(uint8_t *p, size_t *o, uint16_t v)
{
    p[(*o)++] = (uint8_t)(v >> 8);
    p[(*o)++] = (uint8_t)(v & 0xFF);
}

static inline void dns_push_u32(uint8_t *p, size_t *o, uint32_t v)
{
    p[(*o)++] = (uint8_t)((v >> 24) & 0xFF);
    p[(*o)++] = (uint8_t)((v >> 16) & 0xFF);
    p[(*o)++] = (uint8_t)((v >> 8) & 0xFF);
    p[(*o)++] = (uint8_t)(v & 0xFF);
}

static inline void dns_push_label(uint8_t *p, size_t *o, const char *label)
{
    size_t len = strlen(label);
    if (len > 63) len = 63;
    p[(*o)++] = (uint8_t)len;
    memcpy(p + *o, label, len);
    *o += len;
}

static inline void dns_push_ptr(uint8_t *p, size_t *o, uint16_t offset)
{
    uint16_t v = (uint16_t)(0xC000 | (offset & 0x3FFF));
    p[(*o)++] = (uint8_t)(v >> 8);
    p[(*o)++] = (uint8_t)(v & 0xFF);
}

/* 构建 _miplay_lan._tcp DNS 响应（支持 unicast/announcement 两种模式）
 * Rust lan.rs: is_unicast ? qdcount=1/class=1/ttl=10 : qdcount=0/class=0x8001/ttl=60 */
static size_t build_lan_packet(uint8_t *p, bool is_unicast)
{
    size_t o = 0;
    uint32_t ip = get_my_ipv4();
    uint16_t record_class = is_unicast ? 1 : 0x8001;
    uint32_t ttl = is_unicast ? 10 : 60;

    /* ── Header (12 bytes) ── */
    dns_push_u16(p, &o, 0x0000);   /* TX ID (query 时会被覆盖) */
    dns_push_u16(p, &o, 0x8400);   /* Authoritative response */
    dns_push_u16(p, &o, is_unicast ? 1 : 0);  /* QDCOUNT */
    dns_push_u16(p, &o, 1);        /* ANCOUNT=1 (PTR) */
    dns_push_u16(p, &o, 0);        /* NSCOUNT */
    dns_push_u16(p, &o, 3);        /* ARCOUNT (SRV+TXT+A) */

    /* ── Question section（仅 unicast）── */
    uint16_t service_off = (uint16_t)o;
    memcpy(p + o, s_service_qname, SERVICE_QNAME_LEN);
    o += SERVICE_QNAME_LEN;
    dns_push_u16(p, &o, 12);  /* QTYPE=PTR */
    dns_push_u16(p, &o, 1);   /* QCLASS=IN */
    if (is_unicast) {
        /* 回显 question（去掉 QU bit） */
        memcpy(p + o, s_service_qname, SERVICE_QNAME_LEN);
        o += SERVICE_QNAME_LEN;
        dns_push_u16(p, &o, 12);
        dns_push_u16(p, &o, 1);
    }

    /* ── Answer: PTR ── */
    dns_push_u32(p, &o, ttl);
    uint16_t instance_len = (uint16_t)strlen(s_inst_name);
    dns_push_u16(p, &o, (uint16_t)(instance_len + 3));
    uint16_t instance_off = (uint16_t)o;
    dns_push_label(p, &o, s_inst_name);
    dns_push_ptr(p, &o, service_off);

    /* ── Additional: SRV ── */
    dns_push_ptr(p, &o, instance_off);
    dns_push_u16(p, &o, 33);   /* TYPE=SRV */
    dns_push_u16(p, &o, record_class);
    dns_push_u32(p, &o, ttl);
    size_t srv_len_pos = o;
    dns_push_u16(p, &o, 0);
    size_t srv_rdata = o;
    dns_push_u16(p, &o, 0);    /* Priority */
    dns_push_u16(p, &o, 0);    /* Weight */
    dns_push_u16(p, &o, MIPLAY_CONTROL_PORT);
    uint16_t host_off = (uint16_t)o;
    dns_push_label(p, &o, s_lan_hostname);
    p[o++] = 0x05; memcpy(p + o, "local", 5); o += 5;
    p[o++] = 0x00;
    uint16_t srv_len = (uint16_t)(o - srv_rdata);
    p[srv_len_pos] = (uint8_t)(srv_len >> 8);
    p[srv_len_pos + 1] = (uint8_t)(srv_len & 0xFF);

    /* ── Additional: TXT ── */
    dns_push_ptr(p, &o, instance_off);
    dns_push_u16(p, &o, 16);   /* TYPE=TXT */
    dns_push_u16(p, &o, record_class);
    dns_push_u32(p, &o, ttl);
    char appdata_json[256];
    char idhash_lan[8];
    for (int i = 0; s_idhash[i] && i < 4; i++) {
        idhash_lan[i] = (s_idhash[i] == '+') ? '-' :
                        (s_idhash[i] == '/') ? '_' : s_idhash[i];
    }
    idhash_lan[3] = '\0';
    int stable_id = (int)(((uint32_t)s_mac[2] << 24) |
                          ((uint32_t)s_mac[3] << 16) |
                          ((uint32_t)s_mac[4] << 8)  |
                          s_mac[5]) & 0x7FFFFFFF;
    snprintf(appdata_json, sizeof(appdata_json),
             "{\"supportLyra\":true,"
             "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
             "\"ID\":%d,"
             "\"type\":3,"
             "\"idhash\":\"%s\","
             "\"extraAbility\":0}",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5],
             stable_id, idhash_lan);
    char txt_rec[300];
    int tl = snprintf(txt_rec, sizeof(txt_rec), "appdata=%s", appdata_json);
    if (tl > 255) tl = 255;
    dns_push_u16(p, &o, (uint16_t)(tl + 1));
    p[o++] = (uint8_t)tl;
    memcpy(p + o, txt_rec, (size_t)tl);
    o += (size_t)tl;

    /* ── Additional: A ── */
    dns_push_ptr(p, &o, host_off);
    dns_push_u16(p, &o, 1);    /* TYPE=A */
    dns_push_u16(p, &o, record_class);
    dns_push_u32(p, &o, ttl);
    dns_push_u16(p, &o, 4);
    p[o++] = (uint8_t)(ip & 0xFF);
    p[o++] = (uint8_t)((ip >> 8) & 0xFF);
    p[o++] = (uint8_t)((ip >> 16) & 0xFF);
    p[o++] = (uint8_t)((ip >> 24) & 0xFF);

    return o;
}

static void build_miplay_lan_response(void)
{
    snprintf(s_lan_hostname, sizeof(s_lan_hostname),
             "miplay-%02X%02X%02X", s_mac[3], s_mac[4], s_mac[5]);

    /* announcement: qdcount=0, class=0x8001, ttl=60 */
    s_lan_announce_len = build_lan_packet(s_lan_announce, false);
    /* unicast reply: qdcount=1, class=1, ttl=10 */
    s_lan_response_len = build_lan_packet(s_lan_response, true);

    ESP_LOGI(TAG, "MiPlay LAN built: announce=%u reply=%u bytes, host=%s",
             (unsigned)s_lan_announce_len, (unsigned)s_lan_response_len,
             s_lan_hostname);
}

static void miplay_lan_task(void *arg)
{
    (void)arg;
    s_lan_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_lan_sock < 0) {
        ESP_LOGE(TAG, "LAN UDP socket failed: %d", errno);
        s_lan_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    int reuse = 1;
    setsockopt(s_lan_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(MIPLAY_LAN_DISCOVERY_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_lan_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "LAN bind failed: %d", errno);
        close(s_lan_sock); s_lan_sock = -1;
        s_lan_task = NULL;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    /* 加入 224.0.0.251 组播组 */
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MIPLAY_LAN_MDNS_GROUP);
    mreq.imr_interface.s_addr = get_my_ipv4();
    setsockopt(s_lan_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    /* 100ms 超时，避免阻塞退出 */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(s_lan_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "LAN discovery UDP %d listening", MIPLAY_LAN_DISCOVERY_PORT);

    /* ── 启动时发3次组播宣告（Rust lan.rs announcement 模式）── */
    struct sockaddr_in mcast_dest = {
        .sin_family = AF_INET,
        .sin_port = htons(MIPLAY_LAN_DISCOVERY_PORT),
        .sin_addr.s_addr = inet_addr(MIPLAY_LAN_MDNS_GROUP),
    };
    for (int round = 1; round <= 3 && s_running; round++) {
        sendto(s_lan_sock, s_lan_announce, s_lan_announce_len, 0,
               (struct sockaddr *)&mcast_dest, sizeof(mcast_dest));
        ESP_LOGI(TAG, "LAN announcement %d/3 sent (%u bytes)", round,
                 (unsigned)s_lan_announce_len);
        vTaskDelay(pdMS_TO_TICKS(180));
    }

    while (s_running) {
        uint8_t rx_buf[512];
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        int n = recvfrom(s_lan_sock, rx_buf, sizeof(rx_buf), 0,
                         (struct sockaddr *)&src, &srclen);
        if (n <= 0) continue;
        if (n < (int)SERVICE_QNAME_LEN) {
            ESP_LOGI(TAG, "LAN recv %d bytes from %u.%u.%u.%u:%u (too short)",
                     n, (unsigned)(src.sin_addr.s_addr & 0xFF),
                     (unsigned)((src.sin_addr.s_addr >> 8) & 0xFF),
                     (unsigned)((src.sin_addr.s_addr >> 16) & 0xFF),
                     (unsigned)((src.sin_addr.s_addr >> 24) & 0xFF),
                     (unsigned)ntohs(src.sin_port));
            continue;
        }

        /* 检查报文是否含 _miplay_lan._tcp 服务名 */
        int found = 0;
        for (int i = 0; i + SERVICE_QNAME_LEN <= n; i++) {
            if (memcmp(rx_buf + i, s_service_qname, SERVICE_QNAME_LEN) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) continue;

        /* 组装应答：覆盖 TX ID（堆分配避免栈溢出） */
        uint8_t *reply = MIPLAY_MALLOC(s_lan_response_len);
        if (!reply) continue;
        memcpy(reply, s_lan_response, s_lan_response_len);
        reply[0] = rx_buf[0];
        reply[1] = rx_buf[1];

        sendto(s_lan_sock, reply, s_lan_response_len, 0,
               (struct sockaddr *)&src, srclen);
        MIPLAY_FREE(reply);
        ESP_LOGI(TAG, "LAN → _miplay_lan reply %u.%u.%u.%u (%u bytes)",
                 (unsigned)(src.sin_addr.s_addr & 0xFF),
                 (unsigned)((src.sin_addr.s_addr >> 8) & 0xFF),
                 (unsigned)((src.sin_addr.s_addr >> 16) & 0xFF),
                 (unsigned)((src.sin_addr.s_addr >> 24) & 0xFF),
                 (unsigned)s_lan_response_len);
    }
    close(s_lan_sock);
    s_lan_sock = -1;
    s_lan_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

/* ══════════════════════════════════════════════════════════════════════
 * MiPlay 命令帧编解码 — 9字节大端序
 * ══════════════════════════════════════════════════════════════════════ */

/* 发送命令帧: 0x24 + cmd(u16 BE) + seq(u16 BE) + len(u32 BE) + payload */
static int miplay_send_cmd(int sock, uint16_t cmd, uint16_t seq,
                           const uint8_t *payload, uint32_t payload_len)
{
    uint8_t hdr[MIPLAY_FRAME_HDR_LEN];
    hdr[0] = MIPLAY_FRAME_MAGIC;
    hdr[1] = (cmd >> 8) & 0xFF; hdr[2] = cmd & 0xFF;
    hdr[3] = (seq >> 8) & 0xFF; hdr[4] = seq & 0xFF;
    hdr[5] = (payload_len >> 24) & 0xFF; hdr[6] = (payload_len >> 16) & 0xFF;
    hdr[7] = (payload_len >> 8) & 0xFF;  hdr[8] = payload_len & 0xFF;

    int ret = send(sock, hdr, MIPLAY_FRAME_HDR_LEN, 0);
    if (ret < 0) return ret;
    if (payload_len > 0 && payload) {
        ret = send(sock, payload, payload_len, 0);
        if (ret < 0) return ret;
    }
    ESP_LOGI(TAG, "TX cmd=0x%04X seq=%u len=%lu", cmd, seq, (unsigned long)payload_len);
    return MIPLAY_FRAME_HDR_LEN + (int)payload_len;
}

/* ══════════════════════════════════════════════════════════════════════
 * CRC-32/MPEG-2（SafetyData 完整性校验）
 * ══════════════════════════════════════════════════════════════════════ */

/* MiPlay CRC-32 变体：字节交换查找表，无最终 XOR
 * 参考: miplay_integrity_table_entry() + miplay_integrity() */
static uint32_t miplay_crc32_table_entry(uint8_t index)
{
    uint32_t value = (uint32_t)index << 24;
    for (int i = 0; i < 8; i++)
        value = (value & 0x80000000) ? (value << 1) ^ 0x04C11DB7 : (value << 1);
    return __builtin_bswap32(value);
}

static uint32_t safety_integrity(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        uint8_t idx = (uint8_t)(crc & 0xFF) ^ data[i];
        crc = miplay_crc32_table_entry(idx) ^ (crc >> 8);
    }
    return crc;  /* 无最终 XOR */
}

/* ══════════════════════════════════════════════════════════════════════
 * AES-128-CBC 手动实现（SafetyData v1 使用无填充 CBC + 零填充）
 *
 * ⚠ 本实现使用纯软件 AES 内核（下方 sw_aes_*），不调用 mbedtls_aes_*。
 *    原因：CONFIG_MBEDTLS_HARDWARE_AES=y 时 ESP-IDF 把 mbedtls_aes_crypt_ecb
 *    宏映射到 esp_aes_crypt_ecb（硬件 GDMA 加速），每帧需 heap_caps_malloc
 *    MALLOC_CAP_DMA 内部缓冲。媒体(GMF)播放后内部 DMA SRAM 被占，控制线程
 *    decrypt 申请失败 → 所有加密控制帧(含元数据 0x12/0x0418)解密失败被丢弃。
 *    纯软件内核零 malloc、零 DMA，彻底摆脱该依赖。
 * ══════════════════════════════════════════════════════════════════════ */

/* ── 纯软件 AES-128（FIPS-197，公开域内核，栈分配零堆） ── */
static const uint8_t sw_aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static const uint8_t sw_aes_rsbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d };

static const uint8_t sw_aes_rcon[11] = { 0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };

/* 轮密钥扩展：nr=10 (AES-128)，rk 需 11*16=176 字节 */
static void sw_aes_key_expand(const uint8_t *key, uint32_t rk[44])
{
    uint32_t i;
    for (i = 0; i < 4; i++) {
        rk[i] = ((uint32_t)key[4*i] << 24) | ((uint32_t)key[4*i+1] << 16) |
                ((uint32_t)key[4*i+2] << 8) |  (uint32_t)key[4*i+3];
    }
    for (i = 4; i < 44; i++) {
        uint32_t t = rk[i - 1];
        if (i % 4 == 0) {
            t = (t << 8) | (t >> 24);  /* RotWord */
            t = (uint32_t)sw_aes_sbox[(t >> 24) & 0xFF] << 24 |
                (uint32_t)sw_aes_sbox[(t >> 16) & 0xFF] << 16 |
                (uint32_t)sw_aes_sbox[(t >> 8)  & 0xFF] << 8  |
                (uint32_t)sw_aes_sbox[(t      ) & 0xFF];
            t ^= (uint32_t)sw_aes_rcon[i / 4] << 24;
        }
        rk[i] = rk[i - 4] ^ t;
    }
}

/* GF(2^8) 有限域乘法：a * b（多项式模 0x11B） */
static uint8_t sw_gfmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    while (b) {
        if (b & 1) p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1B;
        b >>= 1;
    }
    return p;
}

/* MixColumns / InvMixColumns：用循环矩阵第一行系数 c 乘一列（列主序 x0..x3） */
static uint32_t sw_aes_mixcol(uint32_t x, const uint8_t c[4])
{
    uint8_t x0 = (x >> 24) & 0xFF, x1 = (x >> 16) & 0xFF;
    uint8_t x2 = (x >> 8) & 0xFF,  x3 = x & 0xFF;
    uint8_t r0 = sw_gfmul(x0,c[0]) ^ sw_gfmul(x1,c[1]) ^ sw_gfmul(x2,c[2]) ^ sw_gfmul(x3,c[3]);
    uint8_t r1 = sw_gfmul(x0,c[3]) ^ sw_gfmul(x1,c[0]) ^ sw_gfmul(x2,c[1]) ^ sw_gfmul(x3,c[2]);
    uint8_t r2 = sw_gfmul(x0,c[2]) ^ sw_gfmul(x1,c[3]) ^ sw_gfmul(x2,c[0]) ^ sw_gfmul(x3,c[1]);
    uint8_t r3 = sw_gfmul(x0,c[1]) ^ sw_gfmul(x1,c[2]) ^ sw_gfmul(x2,c[3]) ^ sw_gfmul(x3,c[0]);
    return ((uint32_t)r0 << 24) | ((uint32_t)r1 << 16) | ((uint32_t)r2 << 8) | r3;
}

static void sw_aes_encrypt_block(const uint8_t in[16], uint8_t out[16], const uint32_t rk[44])
{
    uint32_t s[4];
    int i;
    for (i = 0; i < 4; i++) {
        s[i] = ((uint32_t)in[4*i] << 24) | ((uint32_t)in[4*i+1] << 16) |
               ((uint32_t)in[4*i+2] << 8) |  (uint32_t)in[4*i+3];
        s[i] ^= rk[i];
    }
    for (int round = 1; round <= 10; round++) {
        /* SubBytes + ShiftRows */
        uint32_t t[4];
        for (i = 0; i < 4; i++) {
            uint8_t s0 = sw_aes_sbox[(s[i] >> 24) & 0xFF];
            uint8_t s1 = sw_aes_sbox[(s[i] >> 16) & 0xFF];
            uint8_t s2 = sw_aes_sbox[(s[i] >> 8)  & 0xFF];
            uint8_t s3 = sw_aes_sbox[s[i] & 0xFF];
            t[i] = ((uint32_t)s0 << 24) | ((uint32_t)s1 << 16) | ((uint32_t)s2 << 8) | s3;
        }
        uint8_t m[4][4];
        for (i = 0; i < 4; i++) {
            m[0][i] = (t[i] >> 24) & 0xFF;
            m[1][i] = (t[i] >> 16) & 0xFF;
            m[2][i] = (t[i] >> 8)  & 0xFF;
            m[3][i] = t[i] & 0xFF;
        }
        uint8_t sh[4][4];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                sh[r][c] = m[r][(c + r) & 3];
        for (i = 0; i < 4; i++) {
            uint32_t v = 0;
            for (int r = 0; r < 4; r++) v = (v << 8) | sh[r][i];
            t[i] = v;
        }
        if (round < 10) {  /* MixColumns */
            static const uint8_t mc[4] = { 2, 3, 1, 1 };
            for (i = 0; i < 4; i++) t[i] = sw_aes_mixcol(t[i], mc);
        }
        for (i = 0; i < 4; i++) s[i] = t[i] ^ rk[round * 4 + i];
    }
    for (i = 0; i < 4; i++) {
        out[4*i]   = (s[i] >> 24) & 0xFF;
        out[4*i+1] = (s[i] >> 16) & 0xFF;
        out[4*i+2] = (s[i] >> 8)  & 0xFF;
        out[4*i+3] =  s[i]        & 0xFF;
    }
}

/* 解密块：InvShiftRows → InvSubBytes → (InvMixColumns) → AddRoundKey
 * 用反向轮：先对 s 做逆 S-box 前的逆行移，再逐轮 InvMixColumns */
static void sw_aes_decrypt_block(const uint8_t in[16], uint8_t out[16], const uint32_t rk[44])
{
    uint32_t s[4];
    int i;
    for (i = 0; i < 4; i++) {
        s[i] = ((uint32_t)in[4*i] << 24) | ((uint32_t)in[4*i+1] << 16) |
               ((uint32_t)in[4*i+2] << 8) |  (uint32_t)in[4*i+3];
        s[i] ^= rk[40 + i];
    }
    for (int round = 9; round >= 0; round--) {
        uint32_t t[4];
        /* InvShiftRows: 展开 state（m[r][c] = state[r][c]） */
        uint8_t m[4][4];
        for (i = 0; i < 4; i++) {
            m[0][i] = (s[i] >> 24) & 0xFF;
            m[1][i] = (s[i] >> 16) & 0xFF;
            m[2][i] = (s[i] >> 8)  & 0xFF;
            m[3][i] = s[i] & 0xFF;
        }
        /* 逆 ShiftRows = 加密行左移 r 的逆 = 行右移 r：新 state[r][c]=旧 state[r][(c - r) & 3] */
        uint8_t ish[4][4];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                ish[r][c] = m[r][(c + 4 - r) & 3];
        for (i = 0; i < 4; i++) {   /* InvSubBytes 按列重组到 t */
            uint32_t v = 0;
            for (int r = 0; r < 4; r++) v = (v << 8) | sw_aes_rsbox[ish[r][i]];
            t[i] = v;
        }
        for (i = 0; i < 4; i++) t[i] ^= rk[round * 4 + i];  /* AddRoundKey(正序调度)在 InvMixColumns 之前 */
        if (round > 0) {  /* InvMixColumns 应用于解密轮 9..1，末轮 round=0 不做 */
            static const uint8_t imc[4] = { 14, 11, 13, 9 };
            for (i = 0; i < 4; i++) t[i] = sw_aes_mixcol(t[i], imc);
        }
        for (i = 0; i < 4; i++) s[i] = t[i];
    }
    for (i = 0; i < 4; i++) {
        out[4*i]   = (s[i] >> 24) & 0xFF;
        out[4*i+1] = (s[i] >> 16) & 0xFF;
        out[4*i+2] = (s[i] >> 8)  & 0xFF;
        out[4*i+3] =  s[i]        & 0xFF;
    }
}

/* AES-CBC 加密（无内置填充，调用方负责对齐；纯软件内核，零 DMA） */
static bool aes_cbc_encrypt(const uint8_t *key, uint8_t *iv,
                            const uint8_t *input, uint8_t *output, size_t len)
{
    if (len % AES_BLOCK_LEN != 0) return false;
    uint32_t rk[44];
    sw_aes_key_expand(key, rk);
    uint8_t iv_copy[AES_BLOCK_LEN];
    memcpy(iv_copy, iv, AES_BLOCK_LEN);
    for (size_t i = 0; i < len; i += AES_BLOCK_LEN) {
        uint8_t block[AES_BLOCK_LEN];
        for (int j = 0; j < AES_BLOCK_LEN; j++)
            block[j] = input[i + j] ^ iv_copy[j];
        sw_aes_encrypt_block(block, output + i, rk);
        memcpy(iv_copy, output + i, AES_BLOCK_LEN);
    }
    memcpy(iv, iv_copy, AES_BLOCK_LEN);
    return true;
}

/* AES-CBC 解密（纯软件内核，零 DMA） */
static bool aes_cbc_decrypt(const uint8_t *key, uint8_t *iv,
                            const uint8_t *input, uint8_t *output, size_t len)
{
    if (len % AES_BLOCK_LEN != 0) return false;
    uint32_t rk[44];
    sw_aes_key_expand(key, rk);
    uint8_t iv_copy[AES_BLOCK_LEN];
    memcpy(iv_copy, iv, AES_BLOCK_LEN);
    for (size_t i = 0; i < len; i += AES_BLOCK_LEN) {
        uint8_t block[AES_BLOCK_LEN];
        sw_aes_decrypt_block(input + i, block, rk);
        for (int j = 0; j < AES_BLOCK_LEN; j++)
            output[i + j] = block[j] ^ iv_copy[j];
        memcpy(iv_copy, input + i, AES_BLOCK_LEN);
    }
    memcpy(iv, iv_copy, AES_BLOCK_LEN);
    return true;
}

/* ══════════════════════════════════════════════════════════════════════
 * SafetyData v1 编解码
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * 加密 plaintext → SafetyData v1 容器
 * 返回写入 out 的字节数，失败返回 0
 */
static int safety_encrypt(const uint8_t *plaintext, size_t pt_len,
                          const uint8_t *key, uint8_t *iv,
                          uint8_t *out, size_t out_max)
{
    /* 零填充: 1~16 字节 */
    int pad_len = AES_BLOCK_LEN - (pt_len % AES_BLOCK_LEN);
    size_t padded_len = pt_len + pad_len;
    if (SAFETY_DATA_HDR_LEN + padded_len > out_max) return 0;

    uint8_t *padded = MIPLAY_CALLOC(1, padded_len);
    if (!padded) return 0;
    memcpy(padded, plaintext, pt_len);
    /* 剩余已为零 */

    uint8_t *ciphertext = out + SAFETY_DATA_HDR_LEN;
    if (!aes_cbc_encrypt(key, iv, padded, ciphertext, padded_len)) {
        MIPLAY_FREE(padded);
        return 0;
    }
    MIPLAY_FREE(padded);

    /* 构造9字节头 */
    uint32_t crc = safety_integrity(ciphertext, padded_len);
    out[0] = 0x00; out[1] = 0x07;  /* headerLenMinusTwo = 7 (9字节头-2) */
    out[2] = SAFETY_DATA_VERSION;
    out[3] = SAFETY_DATA_FLAGS;
    out[4] = (uint8_t)pad_len;
    out[5] = (crc >> 24) & 0xFF; out[6] = (crc >> 16) & 0xFF;
    out[7] = (crc >> 8) & 0xFF;  out[8] = crc & 0xFF;

    return SAFETY_DATA_HDR_LEN + (int)padded_len;
}

/*
 * 解密 SafetyData v1 容器 → plaintext
 * 返回 plaintext 长度，失败返回 -1
 */
static int safety_decrypt(const uint8_t *data, size_t data_len,
                          const uint8_t *key, uint8_t *iv,
                          uint8_t *out, size_t out_max)
{
    if (data_len < SAFETY_DATA_HDR_LEN) return -1;
    /* 固定9字节头: [0x00, 0x07, version, flags, padLen, CRC(4)] */
    if (data[0] != 0x00 || data[1] != 0x07) {
        ESP_LOGW(TAG, "safety_decrypt: bad magic %02X %02X", data[0], data[1]);
        return -1;
    }
    if (data[2] != SAFETY_DATA_VERSION) {
        ESP_LOGW(TAG, "safety_decrypt: unknown version %d", data[2]);
        return -1;
    }
    uint8_t flags = data[3];
    if (!(flags & 0x80)) {
        ESP_LOGW(TAG, "safety_decrypt: not encrypted (flags=0x%02X)", flags);
        return -1;
    }

    int pad_len = (flags & 0x40) ? data[4] : 0;
    uint32_t expected_crc = ((uint32_t)data[5]<<24)|((uint32_t)data[6]<<16)|
                            ((uint32_t)data[7]<<8)|data[8];
    size_t ct_len = data_len - SAFETY_DATA_HDR_LEN;
    if (ct_len == 0 || ct_len % AES_BLOCK_LEN != 0) return -1;
    if (safety_integrity(data + SAFETY_DATA_HDR_LEN, ct_len) != expected_crc) return -1;

    size_t pt_len = ct_len - pad_len;
    if (pt_len > out_max) {
        ESP_LOGW(TAG, "safety_decrypt: pt_len %d > out_max %d", (int)pt_len, (int)out_max);
        return -1;
    }

    /* Debug: print key, IV, ciphertext */
    {
        ESP_LOGI(TAG, "SD key[0..7]: %02X %02X %02X %02X %02X %02X %02X %02X",
                 key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7]);
        ESP_LOGI(TAG, "SD iv[0..7]:  %02X %02X %02X %02X %02X %02X %02X %02X",
                 iv[0], iv[1], iv[2], iv[3], iv[4], iv[5], iv[6], iv[7]);
        ESP_LOGI(TAG, "SD ct[0..15]: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                 data[9], data[10], data[11], data[12], data[13], data[14], data[15], data[16],
                 data[17], data[18], data[19], data[20], data[21], data[22], data[23], data[24]);
        ESP_LOGI(TAG, "SD ct_len=%d pad=%d pt_len=%d", (int)ct_len, pad_len, (int)pt_len);
    }

    uint8_t *decrypted = MIPLAY_MALLOC(ct_len);
    if (!decrypted) return -1;
    if (!aes_cbc_decrypt(key, iv, data + SAFETY_DATA_HDR_LEN, decrypted, ct_len)) {
        ESP_LOGW(TAG, "safety_decrypt: aes_cbc_decrypt failed");
        MIPLAY_FREE(decrypted);
        return -1;
    }

    /* Debug: print first 32 bytes of decrypted */
    {
        char hex[68];
        hex_to_lower(decrypted, (int)pt_len > 32 ? 32 : (int)pt_len, hex);
        ESP_LOGI(TAG, "SD dec[0..31]: %s", hex);
    }

    /* 验证零填充 */
    for (size_t i = pt_len; i < ct_len; i++) {
        if (decrypted[i] != 0) {
            ESP_LOGW(TAG, "safety_decrypt: bad pad at [%d]=0x%02X", (int)i, decrypted[i]);
            MIPLAY_FREE(decrypted); return -1;
        }
    }
    memcpy(out, decrypted, pt_len);
    MIPLAY_FREE(decrypted);
    return (int)pt_len;
}

/* ══════════════════════════════════════════════════════════════════════
 * SafetyEnvelope 编解码（OPack 子集）
 * tagLen(1) + tag("cmd"/"ack") + valueType(1) + payloadLen(4 BE) + payload
 * ══════════════════════════════════════════════════════════════════════ */

static int safety_envelope_encode(bool is_ack, uint8_t value_type,
                                  const uint8_t *payload, uint32_t payload_len,
                                  uint8_t *out, size_t out_max)
{
    const uint8_t *tag = is_ack ? (const uint8_t *)"ack" : (const uint8_t *)"cmd";
    int tag_len = 3;
    /* wrap_payload: keyLen(1) + key(3) + valueType(4 LE) + dataLen(1) + data */
    size_t total = 1 + tag_len + 4 + 1 + payload_len;
    if (total > out_max) return 0;
    size_t off = 0;
    out[off++] = (uint8_t)tag_len;
    memcpy(out + off, tag, tag_len); off += tag_len;
    /* value_type as 4-byte little-endian (constant 30 = 0x1E) */
    out[off++] = value_type; out[off++] = 0x00;
    out[off++] = 0x00;       out[off++] = 0x00;
    /* data length as 1 byte */
    out[off++] = (uint8_t)payload_len;
    memcpy(out + off, payload, payload_len);
    return (int)(off + payload_len);
}

/* 发送加密命令（安全通道建立后使用）
 * PC 原型语义(send_encrypted): outer=0x00 帧, payload 直接包 SafetyData，
 * 不套 SafetyEnvelope（envelope 仅在 0x14 Safety 阶段使用）。
 * 全部堆分配，避免大栈缓冲区。 */
/* 静态加密缓冲区：移入 PSRAM，释放内部 SRAM（参考项目做法） */
static EXT_RAM_BSS_ATTR uint8_t s_encrypt_buf[2064 + SAFETY_DATA_HDR_LEN + AES_BLOCK_LEN];
static EXT_RAM_BSS_ATTR uint8_t s_envelope_buf[9 + 2048];

static int send_encrypted_cmd(int sock, uint16_t cmd, uint16_t seq,
                               const uint8_t *payload, uint32_t payload_len)
{
    if (payload_len > MIPLAY_MAX_FRAME_PAYLOAD) return -1;
    if (s_send_mux) xSemaphoreTakeRecursive(s_send_mux, portMAX_DELAY);
    uint32_t sd_cap = sizeof(s_encrypt_buf);
    int sd_len = safety_encrypt(payload, payload_len, s_aes_key, s_encrypt_iv,
                                s_encrypt_buf, sd_cap);
    int ret = -1;
    if (sd_len > 0) ret = miplay_send_cmd(sock, cmd, seq, s_encrypt_buf, (uint32_t)sd_len);
    if (s_send_mux) xSemaphoreGiveRecursive(s_send_mux);
    return ret;
}

/* 原子分配序号 + 加密发送（对齐参考 send_encrypted_cmd_auto_seq）。
 * 防止 UI 线程旋钮反控与控制任务 notify 并发时序号和 IV 交错。 */
static int send_encrypted_cmd_auto_seq(int sock, uint16_t cmd,
                                       const uint8_t *payload, uint32_t payload_len,
                                       uint16_t *seq_out)
{
    if (seq_out) *seq_out = 0;
    if (s_send_mux) xSemaphoreTakeRecursive(s_send_mux, portMAX_DELAY);
    int seq = s_notify_seq;
    if (seq < 8 || seq > 0xFFFF) seq = 8;
    s_notify_seq = (seq == 0xFFFF) ? 8 : seq + 1;
    uint32_t sd_cap = sizeof(s_encrypt_buf);
    int sd_len = safety_encrypt(payload, payload_len, s_aes_key, s_encrypt_iv,
                                s_encrypt_buf, sd_cap);
    int ret = -1;
    if (sd_len > 0) ret = miplay_send_cmd(sock, cmd, (uint16_t)seq, s_encrypt_buf, (uint32_t)sd_len);
    if (s_send_mux) xSemaphoreGiveRecursive(s_send_mux);
    if (seq_out) *seq_out = (uint16_t)seq;
    return ret;
}

/* ── 空 mediaInfoEx NOTIFY（引导封面）──
 * 对齐参考 esp_miplay-main miplay_notify_empty_media_info_ex: HyperOS 在消费
 * 一条加密的空 mediaInfoEx NOTIFY 时安装 reverse-control / skip-metadata，
 * 此后手机推送的 SetMediaInfo 才会携带 mCoverUrl/mArt。字节与参考
 * miplay_build_media_info_ex 一致：0x0B+"mediaInfoEx"+0x16+[len]+空 mTitle。 */
static const uint8_t s_empty_media_info_ex[29] = {
    0x0B, 'm', 'e', 'd', 'i', 'a', 'I', 'n', 'f', 'o', 'E', 'x',
    0x16, 0x00, 0x00, 0x00, 0x0C, 0x06,
    'm', 'T', 'i', 't', 'l', 'e',
    0x14, 0x00, 0x00, 0x00, 0x00,
};
static int miplay_notify_empty_media_info_ex(int sock, const char *reason)
{
    uint16_t mi_seq = 0;
    int ret = send_encrypted_cmd_auto_seq(sock, CMD_NOTIFY, s_empty_media_info_ex,
                                          sizeof(s_empty_media_info_ex), &mi_seq);
    ESP_LOGI(TAG, "-> NOTIFY empty mediaInfoEx (%s) sock=%d seq=%u ret=%d",
             reason ? reason : "?", sock, mi_seq, ret);
    return ret;
}

/* Safety 阶段(0x14 帧)专用发送：envelope 明文（不加密），如 SafetyInfoAck */
static int send_plain_envelope(int sock, uint16_t cmd16, uint16_t seq,
                               const uint8_t *payload, uint32_t payload_len)
{
    if (payload_len > MIPLAY_MAX_FRAME_PAYLOAD) return -1;
    uint8_t *envelope = MIPLAY_MALLOC(1 + 3 + 4 + 1 + payload_len);
    if (!envelope) return -1;
    int elen = safety_envelope_encode(true, SAFETY_VALUE_TYPE,
                                      payload, payload_len,
                                      envelope, 1 + 3 + 4 + 1 + payload_len);
    if (elen <= 0) { MIPLAY_FREE(envelope); return -1; }
    int r = miplay_send_cmd(sock, cmd16, seq, envelope, (uint32_t)elen);
    MIPLAY_FREE(envelope);
    return r;
}

/* Safety 阶段(0x14 帧)专用发送：envelope + SafetyData 加密，
 * 如 SafetyAuth challenge("cmd") 与 SafetyAuthAck("ack") */
static int send_encrypted_envelope(int sock, uint16_t cmd16, uint16_t seq,
                                   bool is_ack,
                                   const uint8_t *payload, uint32_t payload_len)
{
    if (payload_len > MIPLAY_MAX_FRAME_PAYLOAD) return -1;
    int elen = safety_envelope_encode(is_ack, SAFETY_VALUE_TYPE,
                                      payload, payload_len,
                                      s_envelope_buf, sizeof(s_envelope_buf));
    if (elen <= 0) return -1;
    uint32_t sd_cap = sizeof(s_encrypt_buf);
    int sd_len = safety_encrypt(s_envelope_buf, (size_t)elen, s_aes_key, s_encrypt_iv,
                                s_encrypt_buf, sd_cap);
    if (sd_len <= 0) return -1;
    return miplay_send_cmd(sock, cmd16, seq, s_encrypt_buf, (uint32_t)sd_len);
}

/* ══════════════════════════════════════════════════════════════════════
 * HMAC-SHA1 / HMAC-SHA256 手动实现
 * ══════════════════════════════════════════════════════════════════════ */

static void hmac_sha1(const uint8_t *key, size_t key_len,
                      const uint8_t *msg, size_t msg_len,
                      uint8_t out[20])
{
    uint8_t k_pad[64];
    uint8_t o_key_pad[64], i_key_pad[64];

    /* 如果 key > 64 字节，先 hash */
    uint8_t k_hash[20];
    if (key_len > 64) {
        mbedtls_sha1(key, key_len, k_hash);
        key = k_hash; key_len = 20;
    }

    memset(k_pad, 0, 64);
    memcpy(k_pad, key, key_len);
    for (int i = 0; i < 64; i++) {
        o_key_pad[i] = k_pad[i] ^ 0x5C;
        i_key_pad[i] = k_pad[i] ^ 0x36;
    }

    /* inner = SHA1(i_key_pad || msg) */
    mbedtls_sha1_context ctx;
    uint8_t inner[20];
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts(&ctx);
    mbedtls_sha1_update(&ctx, i_key_pad, 64);
    mbedtls_sha1_update(&ctx, msg, msg_len);
    mbedtls_sha1_finish(&ctx, inner);
    mbedtls_sha1_free(&ctx);

    /* outer = SHA1(o_key_pad || inner) */
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts(&ctx);
    mbedtls_sha1_update(&ctx, o_key_pad, 64);
    mbedtls_sha1_update(&ctx, inner, 20);
    mbedtls_sha1_finish(&ctx, out);
    mbedtls_sha1_free(&ctx);
}

static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *msg, size_t msg_len,
                        uint8_t out[32])
{
    uint8_t k_pad[64];
    uint8_t o_key_pad[64], i_key_pad[64];

    uint8_t k_hash[32];
    if (key_len > 64) {
        mbedtls_sha256(key, key_len, k_hash, 0);
        key = k_hash; key_len = 32;
    }

    memset(k_pad, 0, 64);
    memcpy(k_pad, key, key_len);
    for (int i = 0; i < 64; i++) {
        o_key_pad[i] = k_pad[i] ^ 0x5C;
        i_key_pad[i] = k_pad[i] ^ 0x36;
    }

    uint8_t inner[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, i_key_pad, 64);
    mbedtls_sha256_update(&ctx, msg, msg_len);
    mbedtls_sha256_finish(&ctx, inner);
    mbedtls_sha256_free(&ctx);

    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, o_key_pad, 64);
    mbedtls_sha256_update(&ctx, inner, 32);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

/* ══════════════════════════════════════════════════════════════════════
 * SafetyAuth 会话密钥派生
 * authKey = MD5(数字转字母后的四元组)
 * 四元组顺序(P C原型): localIP+localPort+peerIP+peerPort
 * （手机视角 remote 在前，故接收端必须本端在前、对端在后，否则密钥不一致）
 * AES key = authKey[0:16] ASCII, AES IV = authKey[0:16] ASCII（各自独立演进）
 */
static void derive_session_key(uint32_t peer_ip, uint16_t peer_port,
                               uint32_t local_ip, uint16_t local_port)
{
    char buf[64];
    int off = 0;
    /* localIP+localPort+peerIP+peerPeer
     * s_addr 是网络字节序(大端)，按字节读取即可得到正确 IP 顺序 */
    const uint8_t *lip = (const uint8_t *)&local_ip;
    off += snprintf(buf + off, sizeof(buf) - off, "%u.%u.%u.%u%u",
                    lip[0], lip[1], lip[2], lip[3], local_port);
    const uint8_t *pip = (const uint8_t *)&peer_ip;
    off += snprintf(buf + off, sizeof(buf) - off, "%u.%u.%u.%u%u",
                    pip[0], pip[1], pip[2], pip[3], peer_port);
    /* 数字→字母变换 */
    for (int i = 0; i < off; i++) {
        if (buf[i] >= '0' && buf[i] <= '9')
            buf[i] = 'a' + (buf[i] - '0');
    }
    /* MD5 → 32 hex char authKey */
    uint8_t md5[16];
    mbedtls_md5((uint8_t *)buf, off, md5);
    char auth_key[33];
    hex_to_lower(md5, 16, auth_key);

    /* AES key = authKey[0:16] ASCII bytes (type 1)
     * AES IV  = authKey[0:16] ASCII bytes (S12 observed: type 1, NOT type 2)
     * 参考: SelectObservedS12InboundSafetyIvMaterial → FirstHalfMaterialType */
    memcpy(s_aes_key, auth_key, 16);
    memcpy(s_encrypt_iv, auth_key, 16);
    memcpy(s_decrypt_iv, auth_key, 16);
    memcpy(s_auth_key, auth_key, 32);          /* 完整 authKey 用于 HMAC */
    s_auth_key[32] = '\0';
    s_has_session_key = true;

    ESP_LOGI(TAG, "Session key: authKey=%s", auth_key);
    ESP_LOGI(TAG, "  aes_key=%.16s encrypt_iv=%.16s decrypt_iv=%.16s", auth_key, auth_key, auth_key);
}

/* 生成随机 authMsg (esp_random → 32 hex chars) */
static void generate_auth_msg(void)
{
    uint8_t rand_bytes[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = esp_random();
        memcpy(rand_bytes + i, &r, 4);
    }
    hex_to_lower(rand_bytes, 16, (char *)s_auth_msg);
}

/* 生成 16-17 位十进制挑战串 */
static void generate_challenge(char *out, size_t out_max)
{
    uint64_t r = ((uint64_t)esp_random() << 32) | esp_random();
    /* 取 16 位十进制 */
    uint64_t val = r % 10000000000000000ULL;  /* 10^16 */
    if (val < 1000000000000000ULL) {
        /* 不足 16 位时补前导 1 使其变成 17 位 */
        snprintf(out, out_max, "1%015llu", (unsigned long long)val);
    } else {
        snprintf(out, out_max, "%016llu", (unsigned long long)val);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * 设备信息响应（OPack 编码的 key/value 对）
 * ══════════════════════════════════════════════════════════════════════ */

static int build_device_info_payload(uint8_t *out, size_t out_max)
{
    /* PC 原型 build_device_info(): 3字节 body 长度 + 重复
     * [keyLen(1), key, 0x0C, valLen(2 BE), value]
     * body 在堆上构建（避免大栈数组）。 */
    struct { const char *key; const char *val; } fields[] = {
        {"alonePlayCapacity", "0"},
        {"canAlonePlayCtrl", "0"},
        {"canHeadsetCtrl", "0"},
        {"canRevCtrl", "1"},
        {"channel", ""},
        {"deviceId", s_device_id},          /* 8 hex 字符 = MAC 后4字节 */
        {"deviceType", "2"},                /* 对齐参考 esp_miplay-main: MIPLAY_DEV="2"(电视)，
                                               dev=2 required for HyperOS 下放 base64 WebP 封面。
                                               旧值"3"(Windows PC)让手机只推空 cover。 */
        {"model", "Android TV"},
        {"needAblum", "1"},
        {"needLrc", "1"},
        {"needPos", "1"},
        {"romVersion", ""},
        {"support", "audio"},
    };
    int num_fields = sizeof(fields) / sizeof(fields[0]);

    size_t body_cap = 512;
    uint8_t *body = MIPLAY_MALLOC(body_cap);
    if (!body) return -1;
    int body_off = 0;
    for (int i = 0; i < num_fields; i++) {
        size_t klen = strlen(fields[i].key);
        size_t vlen = strlen(fields[i].val);
        if (body_off + 1 + (int)klen + 1 + 2 + (int)vlen > (int)body_cap) {
            MIPLAY_FREE(body);
            return -1;
        }
        body[body_off++] = (uint8_t)klen;
        memcpy(body + body_off, fields[i].key, klen); body_off += klen;
        body[body_off++] = 0x0C;
        body[body_off++] = (vlen >> 8) & 0xFF;
        body[body_off++] = vlen & 0xFF;
        memcpy(body + body_off, fields[i].val, vlen); body_off += vlen;
    }

    /* 3字节魔术前缀（与 Rust MiPCAudio 一致） */
    int total = 3 + body_off;
    if (total > (int)out_max) {
        MIPLAY_FREE(body);
        return -1;
    }
    out[0] = 0x00;
    out[1] = 0x01;
    out[2] = 0x55;
    memcpy(out + 3, body, body_off);
    MIPLAY_FREE(body);
    return total;
}

/* ══════════════════════════════════════════════════════════════════════
 * RTSP 客户端（WFD 音频流接收）
 * ── OPEN(wfd://host:port) 后回连手机 RTSP 服务器，完成 WFD 会话协商
 * ══════════════════════════════════════════════════════════════════════ */

#define RTSP_BUF_SIZE  2048

typedef struct {
    char host[64];
    int  port;
    int  client_sock;   /* 控制通道 socket，RTSP 期间需监听心跳 */
    uint32_t generation; /* media generation 快照 */
} rtsp_task_arg_t;

/* 媒体接收任务参数（独立于 RTSP 协商，主控制循环可继续处理心跳） */
typedef struct {
    int media_sock;     /* multi socket 或 rtsp_sock */
    int client_sock;    /* 控制通道 socket（心跳） */
    int rtsp_sock;      /* RTSP socket（协商完可关闭） */
    uint32_t generation; /* media generation 快照 */
} media_task_arg_t;

/* 数据通道（Rust: image + multi 两个额外 TCP 连接） */
static int s_image_sock = -1;   /* image drain socket（丢弃） */
static int s_multi_sock = -1;   /* multi socket（接收媒体数据） */
static uint16_t s_image_port = 0;
static uint16_t s_multi_port = 0;

/* Rust: spawn_image_drain — 读取并丢弃 image 数据 */
static void image_drain_task(void *arg)
{
    int sock = (int)(intptr_t)arg;
    /* 设置 1 秒超时（对齐参考），防止 socket 关闭后长时间阻塞 */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[1024];  /* 降小缓冲区，只需丢弃数据 */
    uint32_t idle = 0, drained = 0;
    /* 对齐参考: 空闲（EAGAIN）不等于连接断开。手机在 image 通道上可能
     * 长时间不发数据，但通道必须保持打开，否则手机认定会话失败并断开 RTSP。
     * 只有对端 FIN(n=0) 或真实错误才退出。 */
    while (s_running) {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n > 0) { drained += (uint32_t)n; idle = 0; continue; }
        if (n == 0) break;                                  /* 对端 FIN */
        if (errno == EAGAIN || errno == EWOULDBLOCK) {      /* 空闲，继续等 */
            idle++;
            if (idle % 10 == 1) {
                ESP_LOGD(TAG, "[RTSP] image_drain idle %lus (drained=%lu)",
                         (unsigned long)idle, (unsigned long)drained);
            }
            continue;
        }
        ESP_LOGW(TAG, "[RTSP] image_drain recv err errno=%d", errno);
        break;
    }
    close(sock);
    ESP_LOGI(TAG, "[RTSP] image_drain done (drained=%lu, idle=%lus)",
             (unsigned long)drained, (unsigned long)idle);
    vTaskDeleteWithCaps(NULL);
}

/* 读取一个完整的 RTSP 消息（请求或响应）
 * 使用持久缓冲区：同 TCP 段中的后续消息不会丢失
 * 返回 1=成功, 0=关闭, -1=错误
 * headers 以 NUL 结尾，body 可能为空字符串 */
static char *s_rtsp_buf = NULL;
static size_t s_rtsp_buf_used = 0;

static void rtsp_read_msg_reset(void)
{
    MIPLAY_FREE(s_rtsp_buf);
    s_rtsp_buf = NULL;
    s_rtsp_buf_used = 0;
}

/* 对齐参考: 在 header 块内查找字段值，遇到空行(消息头结束)立即停止。
 * 不能扫到 body 或流水线化的下一个消息，否则 OPTIONS 会继承
 * GET_PARAMETER 的 Content-Length 并吞掉下一个请求。 */
static const char *rtsp_find_header_value(const char *headers, const char *name)
{
    size_t name_len = strlen(name);
    const char *line = headers;
    while (line && *line) {
        const char *line_end = strstr(line, "\r\n");
        if (!line_end) break;
        if (line_end == line) break;
        if ((size_t)(line_end - line) > name_len &&
            strncasecmp(line, name, name_len) == 0) {
            const char *colon = line + name_len;
            while (colon < line_end && (*colon == ' ' || *colon == '\t')) colon++;
            if (colon < line_end && *colon == ':') {
                const char *value = colon + 1;
                while (value < line_end && (*value == ' ' || *value == '\t')) value++;
                return value;
            }
        }
        line = line_end + 2;
    }
    return NULL;
}

/* 对齐参考: 判断缓冲区是否是合法的 RTSP 起始行（请求方法或响应行）。
 * 用于跳过 interleaved RTP 数据和残留的非 RTSP body 片段。 */
static int rtsp_is_message_start(const char *p, size_t n)
{
    static const char *const methods[] = {
        "OPTIONS ", "GET_PARAMETER ", "SET_PARAMETER ",
        "SETUP ", "PLAY ", "TEARDOWN ", "PAUSE ",
        "TIME_OFFSET ", "VIDEO_LATENCY ",
    };
    if (!p || n == 0) return 0;
    if (n >= 7 && memcmp(p, "RTSP/1.", 7) == 0) return 1;
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        size_t m = strlen(methods[i]);
        if (n >= m && memcmp(p, methods[i], m) == 0) return 1;
    }
    return 0;
}

/* 在缓冲区中定位下一个合法 RTSP 消息起点（\r\n 之后）。 */
static int rtsp_find_message_start(const char *buf, size_t n)
{
    if (!buf || n == 0) return -1;
    if (rtsp_is_message_start(buf, n)) return 0;
    for (size_t i = 0; i + 2 < n; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            rtsp_is_message_start(buf + i + 2, n - i - 2)) {
            return (int)(i + 2);
        }
    }
    return -1;
}

static int rtsp_read_msg(int sock, char *headers, size_t hdr_max,
                          char *body, size_t body_max, int *body_len)
{
    if (!s_rtsp_buf) {
        s_rtsp_buf = MIPLAY_MALLOC(RTSP_BUF_SIZE);
        if (!s_rtsp_buf) return -1;
        s_rtsp_buf_used = 0;
    }
    headers[0] = 0;
    body[0] = 0;
    *body_len = 0;

    while (s_rtsp_buf_used < RTSP_BUF_SIZE - 1) {
        /* 确保 null 终止（strstr 需要） */
        s_rtsp_buf[s_rtsp_buf_used] = 0;

        /* 跳过 interleaved RTP 帧 */
        while (s_rtsp_buf_used >= 4 && (uint8_t)s_rtsp_buf[0] == 0x24) {
            uint16_t rtp_len = ((uint16_t)(uint8_t)s_rtsp_buf[2] << 8) | (uint8_t)s_rtsp_buf[3];
            if (s_rtsp_buf_used < (size_t)(4 + rtp_len)) break;  /* 数据不够 */
            ESP_LOGD(TAG, "[RTSP] skip interleaved len=%u", rtp_len);
            size_t skip = 4 + rtp_len;
            memmove(s_rtsp_buf, s_rtsp_buf + skip, s_rtsp_buf_used - skip);
            s_rtsp_buf_used -= skip;
            s_rtsp_buf[s_rtsp_buf_used] = 0;
        }
        if ((uint8_t)s_rtsp_buf[0] == 0x24) {
            /* 数据不够，继续读 */
            int n = recv(sock, s_rtsp_buf + s_rtsp_buf_used,
                         RTSP_BUF_SIZE - 1 - s_rtsp_buf_used, 0);
            if (n <= 0) return n == 0 ? 0 : -1;
            s_rtsp_buf_used += n;
            continue;
        }

        /* 对齐参考: 缓冲区开头不是合法 RTSP 起始行时定位下一个消息起点。
         * 手机 GET_PARAMETER 的 body 残留(如 "session_enable\r\n")不是合法
         * 起始行，没有 \r\n\r\n 时旧解析器会一直等手机关闭。 */
        if (s_rtsp_buf_used > 0 &&
            !rtsp_is_message_start(s_rtsp_buf, s_rtsp_buf_used)) {
            int next = rtsp_find_message_start(s_rtsp_buf, s_rtsp_buf_used);
            if (next > 0) {
                memmove(s_rtsp_buf, s_rtsp_buf + next, s_rtsp_buf_used - (size_t)next);
                s_rtsp_buf_used -= (size_t)next;
                s_rtsp_buf[s_rtsp_buf_used] = 0;
                continue;
            }
            if (strstr(s_rtsp_buf, "\r\n")) {
                ESP_LOGW(TAG, "[RTSP] drop leftover non-RTSP %u bytes: %.40s",
                         (unsigned)s_rtsp_buf_used, s_rtsp_buf);
                s_rtsp_buf_used = 0;
                s_rtsp_buf[0] = 0;
                continue;
            }
        }

        /* 查找 RTSP 消息结尾 \r\n\r\n */
        char *hdr_end = strstr(s_rtsp_buf, "\r\n\r\n");
        if (!hdr_end) {
            /* 没有完整消息头，继续读 */
            int n = recv(sock, s_rtsp_buf + s_rtsp_buf_used,
                         RTSP_BUF_SIZE - 1 - s_rtsp_buf_used, 0);
            if (n <= 0) {
                /* 区分三种退出原因：对端 FIN(n=0)、超时(EAGAIN)、真实错误。
                 * 不要笼统报"手机断开"——手机不会无故关闭，多数是我们解析/时序问题。 */
                if (n == 0) {
                    ESP_LOGW(TAG, "[RTSP-recv] peer FIN (n=0) used=%u",
                             (unsigned)s_rtsp_buf_used);
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    ESP_LOGD(TAG, "[RTSP-recv] timeout used=%u", (unsigned)s_rtsp_buf_used);
                } else {
                    ESP_LOGW(TAG, "[RTSP-recv] ERR n=%d errno=%d used=%u",
                             n, errno, (unsigned)s_rtsp_buf_used);
                }
                if (s_rtsp_buf_used > 0) {
                    /* 打印缓冲区内容帮助诊断 */
                    char hex[128];
                    int hlen = 0;
                    for (size_t i = 0; i < s_rtsp_buf_used && i < 40; i++)
                        hlen += snprintf(hex + hlen, sizeof(hex) - hlen, "%02X ", (uint8_t)s_rtsp_buf[i]);
                    ESP_LOGW(TAG, "[RTSP-recv] buf[%u]: %s", (unsigned)s_rtsp_buf_used, hex);
                    if (s_rtsp_buf_used >= 4 && (uint8_t)s_rtsp_buf[0] == 0x24) {
                        uint16_t rl = ((uint16_t)(uint8_t)s_rtsp_buf[2] << 8) | (uint8_t)s_rtsp_buf[3];
                        ESP_LOGW(TAG, "[RTSP-recv] interleaved partial: need=%u have=%u",
                                 (unsigned)(4 + rl), (unsigned)s_rtsp_buf_used);
                    }
                }
                return n == 0 ? 0 : -1;
            }
            s_rtsp_buf_used += n;
            continue;
        }

        size_t hdr_len = (size_t)(hdr_end - s_rtsp_buf) + 4;
        if (hdr_len >= hdr_max) {
            /* 对齐参考: 截断会破坏 Content-Length 查找前提，直接报错 */
            ESP_LOGW(TAG, "[RTSP] header too large (%u)", (unsigned)hdr_len);
            errno = EMSGSIZE;
            return -1;
        }
        memcpy(headers, s_rtsp_buf, hdr_len);
        headers[hdr_len] = 0;

        /* 检查 Content-Length（只看当前消息头，不扫 body / 下一帧）。
         * 对齐参考: 无 Content-Length 时用下一个消息起点推断 body 长度，
         * 否则会把下一个 RTSP 请求误当作当前消息的 body 吃掉。 */
        int content_len = 0;
        const char *cl = rtsp_find_header_value(headers, "Content-Length");
        if (cl) {
            content_len = atoi(cl);
        } else if (s_rtsp_buf_used > hdr_len) {
            size_t rest = s_rtsp_buf_used - hdr_len;
            if (!rtsp_is_message_start(s_rtsp_buf + hdr_len, rest)) {
                int next = rtsp_find_message_start(s_rtsp_buf + hdr_len, rest);
                content_len = (next > 0) ? next : (int)rest;
                ESP_LOGD(TAG, "[RTSP] no Content-Length, take body=%d", content_len);
            }
        }
        if (content_len < 0 || hdr_len + (size_t)content_len >= RTSP_BUF_SIZE) {
            ESP_LOGW(TAG, "[RTSP] invalid Content-Length=%d (hdr=%u)",
                     content_len, (unsigned)hdr_len);
            errno = EMSGSIZE;
            return -1;
        }

        /* 等待完整 body — 先检查 buffer，数据够就不 recv */
        size_t body_have = s_rtsp_buf_used - hdr_len;
        while ((int)body_have < content_len && s_rtsp_buf_used < RTSP_BUF_SIZE - 1) {
            size_t need = (size_t)content_len - body_have;
            size_t space = RTSP_BUF_SIZE - 1 - s_rtsp_buf_used;
            if (need > space) need = space;
            int n2 = recv(sock, s_rtsp_buf + s_rtsp_buf_used, need, 0);
            if (n2 <= 0) break;
            s_rtsp_buf_used += n2;
            s_rtsp_buf[s_rtsp_buf_used] = 0;
            body_have = s_rtsp_buf_used - hdr_len;
        }

        size_t copy = (size_t)content_len < body_max - 1 ? (size_t)content_len : body_max - 1;
        body_have = s_rtsp_buf_used - hdr_len;
        if (copy > body_have) copy = body_have;
        memcpy(body, s_rtsp_buf + hdr_len, copy);
        body[copy] = 0;
        *body_len = (int)copy;

        /* 对齐参考: 按 hdr_len + content_len 完整消费本条消息。
         * content_len 已由 rtsp_find_message_start 精确推断（无 Content-Length 时
         * 取到下一个消息起点为止），不会跨消息误吞下一个请求，
         * 因此这里必须真实消费 body，不能留 0 — 否则 body 残留会让下一次
         * 解析命中 "drop leftover" 分支，把紧随的 keepalive 请求一起丢掉。 */
        size_t total = hdr_len + (size_t)content_len;
        if (total > s_rtsp_buf_used) total = s_rtsp_buf_used;
        memmove(s_rtsp_buf, s_rtsp_buf + total, s_rtsp_buf_used - total);
        s_rtsp_buf_used -= total;
        s_rtsp_buf[s_rtsp_buf_used] = 0;
        return 1;
    }
    return -1;
}

/* 确保全部发送（类似 Python sendall） */
static int send_all(int sock, const char *buf, int len)
{
    int sent = 0;
    while (sent < len) {
        int n = send(sock, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

/* 发送 RTSP 请求 */
static void rtsp_send_req(int sock, const char *method, const char *url,
                            const char *extra, int cseq)
{
    char req[384];
    int len = snprintf(req, sizeof(req),
                       "%s %s RTSP/1.0\r\n"
                       "User-Agent: stagefright/1.1 (Linux;Android 4.1)\r\n"
                       "CSeq: %d\r\n"
                       "%s"
                       "\r\n",
                       method, url, cseq, extra ? extra : "");
    send(sock, req, len, 0);
}

/* 发送 RTSP 响应（body 可为 NULL）
 * 与 Python v7 send_rtsp_response 完全一致 */
static void rtsp_send_resp(int sock, int cseq, const char *body)
{
    char resp[640];
    int len;
    if (body) {
        len = snprintf(resp, sizeof(resp),
                       "RTSP/1.0 200 OK\r\n"
                       "User-Agent: stagefright/1.1 (Linux;Android 4.1)\r\n"
                       "CSeq: %d\r\n"
                       "Content-Length: %d\r\n"
                       "\r\n%s",
                       cseq, (int)strlen(body), body);
    } else {
        len = snprintf(resp, sizeof(resp),
                       "RTSP/1.0 200 OK\r\n"
                       "User-Agent: stagefright/1.1 (Linux;Android 4.1)\r\n"
                       "CSeq: %d\r\n"
                       "\r\n",
                       cseq);
    }
    send(sock, resp, len, 0);
}

/* OPTIONS 响应：Public 在 headers 区，可选 auth 头部（Rust: keys 有值时） */
static void rtsp_send_options_resp(int sock, int cseq, const char *auth_ack)
{
    char resp[384];
    int len;
    if (auth_ack && auth_ack[0]) {
        len = snprintf(resp, sizeof(resp),
                       "RTSP/1.0 200 OK\r\n"
                       "User-Agent: stagefright/1.1 (Linux;Android 4.1)\r\n"
                       "CSeq: %d\r\n"
                       "Public: org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER\r\n"
                       "authKeyType:2\r\n"
                       "authAlgorithmVal:4\r\n"
                       "authMsgAck:%s\r\n"
                       "\r\n",
                       cseq, auth_ack);
    } else {
        len = snprintf(resp, sizeof(resp),
                       "RTSP/1.0 200 OK\r\n"
                       "User-Agent: stagefright/1.1 (Linux;Android 4.1)\r\n"
                       "CSeq: %d\r\n"
                       "Public: org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER\r\n"
                       "\r\n",
                       cseq);
    }
    send(sock, resp, len, 0);
}

/* 从 headers 提取 CSeq */
static int rtsp_get_cseq(const char *headers)
{
    const char *p = strstr(headers, "CSeq:");
    if (!p) p = strstr(headers, "cseq:");
    if (!p) return 0;
    return atoi(p + 5);
}

/* 从 headers 提取 Session */
static void rtsp_get_session(const char *headers, char *out, size_t max)
{
    out[0] = 0;
    const char *p = strstr(headers, "Session:");
    if (!p) p = strstr(headers, "session:");
    if (!p) return;
    p += 8;
    while (*p == ' ') p++;
    size_t i = 0;
    while (*p && *p != '\r' && *p != ';' && i < max - 1) {
        out[i++] = *p++;
    }
    out[i] = 0;
}

static void miplay_rtsp_run(const char *host, int port, int client_sock, uint32_t generation);

/* RTSP 独立任务（对齐参考 esp_miplay-main 栈大小） */
#define RTSP_TASK_STACK_SIZE  (48 * 1024)   /* 参考: 48KB */
#define MEDIA_TASK_STACK_SIZE (128 * 1024)  /* 参考: 128KB */

/**
 * @brief 创建 PSRAM 栈任务（对齐参考 miplay_create_task）。
 *        优先 PSRAM 栈，失败则回退内部 SRAM（半栈）。
 */
static BaseType_t miplay_create_task(TaskFunction_t fn, const char *name,
                                     uint32_t stack_bytes, void *arg,
                                     UBaseType_t prio, TaskHandle_t *handle,
                                     BaseType_t core)
{
    TaskHandle_t h = NULL;
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        fn, name, stack_bytes, arg, prio, &h, core,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret == pdPASS) {
        if (handle) *handle = h;
        return pdPASS;
    }
    ESP_LOGW(TAG, "%s: PSRAM stack %u failed, fallback internal", name, (unsigned)stack_bytes);
    h = NULL;
    ret = xTaskCreatePinnedToCore(fn, name, stack_bytes / 2, arg, prio, &h, core);
    if (ret == pdPASS && handle) *handle = h;
    return ret;
}

static void miplay_rtsp_task_wrapper(void *arg)
{
    rtsp_task_arg_t *a = (rtsp_task_arg_t *)arg;
    ESP_LOGI(TAG, "[RTSP-task] Starting: %s:%d sock=%d gen=%lu", a->host, a->port, a->client_sock, (unsigned long)a->generation);
    ESP_LOGI(TAG, "[RTSP-task] Heap: %lu free, PSRAM: %lu free",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    miplay_rtsp_run(a->host, a->port, a->client_sock, a->generation);
    if (s_media_generation != a->generation)
        ESP_LOGI(TAG, "[RTSP-task] Replaced by newer session (gen %lu -> %lu)",
                 (unsigned long)a->generation, (unsigned long)s_media_generation);
    /* 不关闭 client_sock — 主控制循环需要它处理心跳 */
    free(a);
    s_rtsp_task = NULL;
    ESP_LOGI(TAG, "[RTSP-task] Done");
    vTaskDeleteWithCaps(NULL);
}

/* ── TS demux + PES 解密辅助 ── */
#define TS_PKT_SIZE     188
#define TS_SYNC_BYTE    0x47
#define TARGET_PID      0x1100
#define PES_START_CODE  0x000001C0
#define ENCRYPT_PREFIX  256  /* wfd_content_SP_protection 中的加密前缀长度 */

/* PES private data 中提取16字节 AES IV（Rust: pes_private_data_iv） */
static bool pes_extract_iv(const uint8_t *pes, size_t pes_len, uint8_t iv[16])
{
    if (pes_len < 9) return false;
    uint8_t flags = pes[7];
    if (!(flags & 0x01)) return false;  /* PES_extension_flag 未设置 */

    /* 使用 PES_header_data_length 直接跳过后随字段（比手动检查标志位更健壮） */
    size_t cursor = 9 + pes[8];

    /* PES_extension_flag 后跟 extension flags byte，bit 7 = PES_private_data_flag */
    if (cursor + 17 > pes_len) return false;
    if (!(pes[cursor] & 0x80)) return false;
    memcpy(iv, pes + cursor + 1, 16);
    return true;
}

/* 检查 ADTS 帧头（Rust: is_adts） */
static bool is_adts(const uint8_t *data, size_t len)
{
    return len >= 7 && data[0] == 0xFF && (data[1] & 0xF6) == 0xF0;
}

/* TS 流内 PES 音频解密（就地修改 TS 缓冲区）
 * 扫描 TS 包，定位 PID=0x1100 的音频 PES，提取 IV，解密前 256 字节。
 * PES 可能跨多个 TS 包，用 dec_state 跟踪解密进度。 */
typedef struct {
    uint8_t iv[16];
    int remaining;      /* 当前 PES 剩余待解密字节 */
    int pes_pay_off;    /* PES payload 在当前 TS 包中的起始偏移 */
} pes_dec_state_t;

static void decrypt_ts_media(uint8_t *ts_buf, size_t ts_len)
{
    pes_dec_state_t st = { .remaining = 0, .pes_pay_off = 0 };
    for (size_t off = 0; off + TS_PKT_SIZE <= ts_len; off += TS_PKT_SIZE) {
        uint8_t *pkt = ts_buf + off;
        if (pkt[0] != TS_SYNC_BYTE) continue;
        uint16_t pid = ((uint16_t)(pkt[1] & 0x1F) << 8) | pkt[2];
        if (pid != TARGET_PID) continue;
        uint8_t afc = (pkt[3] >> 4) & 0x03;
        if (afc == 0 || afc == 2) continue;
        bool pusi = (pkt[1] & 0x40) != 0;
        int pay_off = 4;
        if (afc == 3) {
            int alen = pkt[4];
            pay_off = 5 + alen;
        }
        int pay_len = TS_PKT_SIZE - pay_off;
        if (pay_len <= 0) continue;
        if (pusi) {
            /* 新 PES 包开始 */
            uint8_t *pes = pkt + pay_off;
            if (pay_len < 9 || pes[0]!=0||pes[1]!=0||pes[2]!=1||pes[3]!=0xC0) {
                st.remaining = 0;
                continue;
            }
            uint16_t pes_len = ((uint16_t)pes[4] << 8) | pes[5];
            int hdr_len = 9 + pes[8];
            if (hdr_len > pay_len) hdr_len = pay_len;
            if (pes_extract_iv(pes, pay_len, st.iv)) {
                st.remaining = (pes_len > 0) ? (pes_len - hdr_len + 6) : ENCRYPT_PREFIX;
                if (st.remaining > ENCRYPT_PREFIX) st.remaining = ENCRYPT_PREFIX;
            } else {
                st.remaining = 0;
            }
            st.pes_pay_off = hdr_len;
        }
        if (st.remaining > 0) {
            int start = pusi ? st.pes_pay_off : 0;
            int avail = pay_len - start;
            if (avail <= 0) { st.remaining = 0; continue; }
            int n = (avail < st.remaining) ? avail : st.remaining;
            if (n % AES_BLOCK_LEN != 0) n -= n % AES_BLOCK_LEN;
            if (n > 0) {
                mbedtls_aes_context actx;
                mbedtls_aes_init(&actx);
                mbedtls_aes_setkey_dec(&actx, s_stream_key, 128);
                mbedtls_aes_crypt_cbc(&actx, MBEDTLS_AES_DECRYPT, n, st.iv,
                                      pkt + pay_off + start, pkt + pay_off + start);
                mbedtls_aes_free(&actx);
                st.remaining -= n;
            }
            if (st.remaining <= 0) st.remaining = 0;
        }
    }
}

/* ── 媒体接收任务（独立 FreeRTOS 任务，与主控制循环并行）
 * 只读 media_sock（RTP 数据），心跳由主控制循环处理
 * 实现：RTP → TS demux → PES 解密 → AAC 解码 → I2S */
static void media_receive_task(void *arg)
{
    media_task_arg_t *marg = (media_task_arg_t *)arg;
    int media_sock = marg->media_sock;
    int rtsp_sock_to_close = marg->rtsp_sock;  /* 媒体结束时关闭 */
    uint32_t generation = marg->generation;     /* generation 快照 */
    free(marg);

    ESP_LOGI(TAG, "[MEDIA] Task started (sock=%d)", media_sock);
    if (s_media_cb) s_media_cb(true);

    uint8_t *rtp_buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!rtp_buf) {
        ESP_LOGE(TAG, "[MEDIA] alloc failed");
        close(media_sock);
        vTaskDeleteWithCaps(NULL); return;
    }

    /* 对齐参考: SO_RCVBUF 增大接收窗口 + SO_RCVTIMEO 100ms */
    int media_rcvbuf = 32 * 1024;
    setsockopt(media_sock, SOL_SOCKET, SO_RCVBUF, &media_rcvbuf, sizeof(media_rcvbuf));
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };  /* 对齐参考: 100ms */
    setsockopt(media_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint32_t pkt_count = 0, rtp_total = 0, ts_total = 0;
    TickType_t media_start = xTaskGetTickCount();
    TickType_t last_log = media_start;
    uint32_t idle_ticks = 0;

    while (s_running && s_media_generation == generation) {
        /* ── 定期状态日志：每秒一次，便于确认媒体流是否真的在收包 ── */
        if (xTaskGetTickCount() - last_log >= pdMS_TO_TICKS(1000)) {
            ESP_LOGI(TAG, "[MEDIA] alive %lus pkts=%lu rtp=%luB ts=%luB idle=%lu ringbuf=%s",
                     (unsigned long)((xTaskGetTickCount() - media_start) / configTICK_RATE_HZ),
                     (unsigned long)pkt_count, (unsigned long)rtp_total,
                     (unsigned long)ts_total, (unsigned long)idle_ticks,
                     s_ts_ringbuf ? "yes" : "NO");
            last_log = xTaskGetTickCount();
        }
        /* ── 读取4字节 interleaved RTP 帧头：$ + channel + len(u16 BE) ── */
        uint8_t hdr[4];
        int got = 0;
        while (got < 4 && s_running) {
            int n = recv(media_sock, hdr + got, 4 - got, 0);
            if (n <= 0) {
                if (!s_running) break;
                if (n == 0) { ESP_LOGW(TAG, "[MEDIA] sock closed (n=0) pkts=%lu", (unsigned long)pkt_count); goto m_cleanup; }
                if (errno == EAGAIN || errno == EWOULDBLOCK) { idle_ticks++; continue; }
                ESP_LOGW(TAG, "[MEDIA] recv err errno=%d pkts=%lu", errno, (unsigned long)pkt_count);
                goto m_cleanup;
            }
            got += n;
        }
        if (!s_running) break;
        if (hdr[0] != 0x24) {
            ESP_LOGW(TAG, "[MEDIA] bad marker 0x%02X, skipping", hdr[0]);
            memmove(hdr, hdr + 1, 3);
            int n = recv(media_sock, hdr + 3, 1, 0);
            if (n <= 0) goto m_cleanup;
            if (hdr[0] != 0x24) continue;
        }

        uint16_t rtp_len = ((uint16_t)hdr[2] << 8) | hdr[3];
        if (rtp_len < 12 || rtp_len > 4096) {
            ESP_LOGW(TAG, "[MEDIA] bad RTP len: %u", rtp_len);
            continue;
        }

        got = 0;
        while (got < rtp_len && s_running) {
            int n = recv(media_sock, rtp_buf + got, rtp_len - got, 0);
            if (n > 0) { got += n; continue; }
            /* 对齐参考: 超时只是暂时无数据，半包仍留在内核缓冲，继续收。
             * 旧代码把 EAGAIN 当致命错误退出，导致媒体流刚起就结束。 */
            if (n == 0) { ESP_LOGW(TAG, "[MEDIA] body FIN pkts=%lu", (unsigned long)pkt_count); goto m_cleanup; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) { idle_ticks++; continue; }
            ESP_LOGW(TAG, "[MEDIA] body recv err errno=%d pkts=%lu", errno, (unsigned long)pkt_count);
            goto m_cleanup;
        }
        rtp_total += rtp_len;
        pkt_count++;
        s_media_pkt_total = pkt_count;
        if (pkt_count <= 3) {
            ESP_LOGI(TAG, "[MEDIA] RTP#%lu len=%u first=[%02X %02X %02X %02X]",
                     (unsigned long)pkt_count, rtp_len,
                     rtp_buf[0], rtp_buf[1], rtp_buf[2], rtp_buf[3]);
        }

        /* ── 剥离 RTP 头，提取 TS 载荷 ── */
        uint8_t *ts_data = rtp_buf;
        uint32_t ts_len = rtp_len;
        if ((rtp_buf[0] & 0xC0) == 0x80) {
            int hdr_len = 12 + (rtp_buf[0] & 0x0F) * 4;
            if (rtp_buf[0] & 0x10) {
                if (hdr_len + 4 <= (int)rtp_len) {
                    int ext_words = ((uint16_t)rtp_buf[hdr_len + 2] << 8) | rtp_buf[hdr_len + 3];
                    hdr_len += 4 + ext_words * 4;
                }
            }
            int end = rtp_len;
            if (rtp_buf[0] & 0x20) {
                end -= rtp_buf[rtp_len - 1];
            }
            if (hdr_len < end) {
                ts_data = rtp_buf + hdr_len;
                ts_len = end - hdr_len;
            }
        }

        /* ── 解密加密的 PES 音频数据（streamKey 已交换时）── */
        if (s_has_stream_key) {
            decrypt_ts_media(ts_data, ts_len);
        }

        /* ── 发送到 ring buffer，由 GMF 管线解码 ── */
        if (s_ts_ringbuf && ts_len > 0) {
            xRingbufferSend(s_ts_ringbuf, ts_data, ts_len, pdMS_TO_TICKS(100));
            ts_total += ts_len;
        }

        if (pkt_count % 100 == 1) {
            ESP_LOGI(TAG, "[MEDIA] pkts=%lu rtp=%luKB ts=%luKB",
                     (unsigned long)pkt_count, (unsigned long)(rtp_total / 1024),
                     (unsigned long)(ts_total / 1024));
        }
    }
    if (s_media_generation != generation)
        ESP_LOGI(TAG, "[MEDIA] Replaced by newer session (gen %lu -> %lu)",
                 (unsigned long)generation, (unsigned long)s_media_generation);
m_cleanup:
    /* generation 不匹配 = 旧 session 被新 session 替代，不应停止新管线 */
    if (s_media_generation != generation) {
        ESP_LOGI(TAG, "[MEDIA] Stale session cleanup, skip stop callback");
    } else {
        if (s_media_cb) s_media_cb(false);
    }
    free(rtp_buf);
    close(media_sock);
    /* media_sock 在 interleaved 回退路径下就是 rtsp_sock，必须避免双重关闭
     * 同一个 fd —— 否则 RTSP 控制连接被误关，keepalive 读到 EBADF 立即断连。 */
    if (rtsp_sock_to_close >= 0 && rtsp_sock_to_close != media_sock) {
        ESP_LOGW(TAG, "[MEDIA] closing rtsp_sock=%d (media_sock=%d)",
                 rtsp_sock_to_close, media_sock);
        close(rtsp_sock_to_close);
    }
    if (s_image_sock >= 0) { close(s_image_sock); s_image_sock = -1; }
    s_image_port = s_multi_port = 0;
    ESP_LOGI(TAG, "[MEDIA] Task ended, %lu pkts, %luKB rtp, %luKB ts",
             (unsigned long)pkt_count, (unsigned long)(rtp_total / 1024),
             (unsigned long)(ts_total / 1024));
    vTaskDeleteWithCaps(NULL);
}

/* 统一事件循环：控制通道 + RTSP + 媒体
 * Rust 用独立线程处理控制通道和 RTSP/媒体。ESP32 单线程用 select 多路复用。
 * client_sock 在此函数内不再被主循环读取（主循环 buf_used 已清空）。 */
static void miplay_rtsp_run(const char *host, int port, int client_sock, uint32_t generation)
{
    char *headers = NULL;
    char *body = NULL;
    int rtsp_sock = -1;
    char rtsp_host_local[64];
    int rtsp_port_local = 0;

    /* 清空 RTSP 持久缓冲区（防止旧数据污染新会话） */
    rtsp_read_msg_reset();

    /* RTSP 连接重试（手机有时连接后立即 RST，errno=0） */
    for (int attempt = 0; attempt < 3 && s_running; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "[RTSP] Retry attempt %d...", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        ESP_LOGI(TAG, "[RTSP] Connecting to %s:%d ...", host, port);

        rtsp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (rtsp_sock < 0) { ESP_LOGE(TAG, "[RTSP] socket failed"); goto rtsp_cleanup; }
        int flag = 1;
        setsockopt(rtsp_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        struct timeval tv = { .tv_sec = 2 };
        setsockopt(rtsp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in dest = {
            .sin_family = AF_INET,
            .sin_port = htons(port),
            .sin_addr.s_addr = inet_addr(host),
        };
        if (connect(rtsp_sock, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
            ESP_LOGW(TAG, "[RTSP] connect failed: %d", errno);
            close(rtsp_sock); rtsp_sock = -1;
            continue;
        }
        ESP_LOGI(TAG, "[RTSP] Connected!");
        rtsp_read_msg_reset();

        /* 试读第一条消息，如果立刻失败则重连 */
        char test_hdr[16];
        int tn = recv(rtsp_sock, test_hdr, 1, MSG_PEEK);
        if (tn <= 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "[RTSP] First peek failed: n=%d errno=%d, reconnecting", tn, errno);
            close(rtsp_sock); rtsp_sock = -1;
            continue;
        }
        break;  /* socket 正常，继续 */
    }
    if (rtsp_sock < 0) {
        ESP_LOGE(TAG, "[RTSP] All connection attempts failed");
        goto rtsp_cleanup;
    }

    /* 保存 host:port 用于后续数据通道连接 */
    strncpy(rtsp_host_local, host, sizeof(rtsp_host_local) - 1);
    rtsp_host_local[sizeof(rtsp_host_local) - 1] = 0;
    rtsp_port_local = port;

    headers = MIPLAY_MALLOC(RTSP_BUF_SIZE);
    body = MIPLAY_MALLOC(RTSP_BUF_SIZE);
    if (!headers || !body) {
        MIPLAY_FREE(headers); MIPLAY_FREE(body);
        close(rtsp_sock); rtsp_sock = -1;
        goto rtsp_cleanup;
    }
    int body_len;

    char session_id[32] = {0};
    char pres_url[160] = "rtsp://localhost/wfd1.0/streamid=0";
    int cseq = 1;
    int sent_options = 0;
    int rtsp_state = 0;  /* 0=handshake, 1=setup_sent, 2=playing */
    int cseq_of_options = 0;   /* 我们 OPTIONS 请求的 cseq */
    int cseq_of_getparam = 0;  /* 我们 GET_PARAMETER 请求的 cseq */
    int cseq_of_setup = 0;     /* 我们 SETUP 请求的 cseq */
    int cseq_of_play = 0;      /* 我们 PLAY 请求的 cseq */

    /* 阻塞超时（2秒，等手机发消息，与 Rust 500ms 接近） */
    struct timeval tv2 = { .tv_sec = 2 };
    setsockopt(rtsp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));

    /* 重置持久缓冲区（清除上次会话残留数据） */
    rtsp_read_msg_reset();

    while (s_running && s_media_generation == generation && rtsp_state < 2) {
        ESP_LOGI(TAG, "[RTSP] Waiting for msg (state=%d)...", rtsp_state);
        int ret = rtsp_read_msg(rtsp_sock, headers, RTSP_BUF_SIZE,
                                 body, RTSP_BUF_SIZE, &body_len);
        if (ret <= 0) {
            if (ret == 0) ESP_LOGI(TAG, "[RTSP] Disconnected");
            else ESP_LOGW(TAG, "[RTSP] Read error (errno=%d)", errno);
            break;
        }
        ESP_LOGI(TAG, "[RTSP] Got msg (%d bytes)", ret);

        int peer_cseq = rtsp_get_cseq(headers);
        int is_resp = (strncmp(headers, "RTSP/1.0 ", 9) == 0);

        if (is_resp) {
            /* ── 处理我们发出的请求的响应 ── */
            ESP_LOGI(TAG, "[RTSP] ← resp cseq=%d: %.120s", peer_cseq, headers);
            if (peer_cseq == cseq_of_options && strstr(headers, "200")) {
                ESP_LOGI(TAG, "[RTSP] ← OPTIONS resp 200");
            }
            else if (peer_cseq == cseq_of_getparam && strstr(headers, "200")) {
                ESP_LOGI(TAG, "[RTSP] ← GET_PARAMETER resp (body=%d bytes)", body_len);
                if (body_len > 0) ESP_LOGI(TAG, "[RTSP] GET_PARAMETER resp body: %.200s", body);
            }
            else if (peer_cseq == cseq_of_setup && strstr(headers, "200")) {
                /* SETUP 响应 — 提取 session，立即发 PLAY（与 Python v7 一致） */
                rtsp_get_session(headers, session_id, sizeof(session_id));
                ESP_LOGI(TAG, "[RTSP] ← SETUP resp 200, session=%s", session_id);
                char play_extra[64];
                snprintf(play_extra, sizeof(play_extra), "Session: %s\r\n", session_id);
                cseq_of_setup = 0;  /* 清除 */
                rtsp_send_req(rtsp_sock, "PLAY", pres_url, play_extra, cseq);
                ESP_LOGI(TAG, "[RTSP] → PLAY (session=%s, cseq=%d)", session_id, cseq);
                cseq_of_play = cseq;
                cseq++;
            }
            else if (peer_cseq == cseq_of_play && strstr(headers, "200")) {
                ESP_LOGI(TAG, "[RTSP] ← PLAY resp 200 === STREAM STARTED ===");
                rtsp_state = 2;
            }
            else if (strstr(headers, "200")) {
                ESP_LOGI(TAG, "[RTSP] ← resp 200 (cseq=%d, ignored)", peer_cseq);
            }
            continue;
        }

        /* ── 处理手机发来的 RTSP 请求 ── */
        char method[32] = {0};
        sscanf(headers, "%31s", method);
        ESP_LOGI(TAG, "[RTSP] ← %s (cseq=%d)", method, peer_cseq);

        if (strcmp(method, "OPTIONS") == 0) {
            ESP_LOGI(TAG, "[RTSP] OPTIONS headers(%d): %.200s", (int)strlen(headers), headers);
            /* 解析 wfd_timer_server_port */
            char *tsp = strstr(headers, "wfd_timer_server_port:");
            if (tsp) {
                uint32_t tip = 0; int tport = 0;
                sscanf(tsp, "wfd_timer_server_port:%lu:%d", &tip, &tport);
                ESP_LOGI(TAG, "[RTSP] Timer server: %lu.%lu.%lu.%u:%d",
                         (unsigned long)(tip & 0xFF), (unsigned long)((tip >> 8) & 0xFF),
                         (unsigned long)((tip >> 16) & 0xFF), (unsigned)((tip >> 24) & 0xFF), tport);
            }
            /* ── OPTIONS 响应：有 auth 时带 HMAC，无 auth 时不带 ──
             * FusionPlay: hmac_sha256_hex(keys.auth_key, challenge_bytes)
             * auth_key = 32-char hex string from control protocol key derivation */
            {
                char auth_ack_hex[65] = {0};
                char *phone_auth = strstr(headers, "authMsg:");
                if (phone_auth && s_has_session_key) {
                    phone_auth += 8;
                    while (*phone_auth == ' ' || *phone_auth == '\t' ||
                           *phone_auth == '\r' || *phone_auth == '\n') phone_auth++;
                    /* 截断尾部空白 */
                    int chal_len = strlen(phone_auth);
                    while (chal_len > 0 && (phone_auth[chal_len-1] == '\r' ||
                           phone_auth[chal_len-1] == '\n' || phone_auth[chal_len-1] == ' '))
                        chal_len--;
                    uint8_t hash[32];
                    hmac_sha256((const uint8_t *)s_auth_key, 32,
                                (const uint8_t *)phone_auth, chal_len, hash);
                    hex_to_lower(hash, 32, auth_ack_hex);
                    ESP_LOGI(TAG, "[RTSP] authMsg=%.*s → ack=%.16s...", chal_len, phone_auth, auth_ack_hex);
                }
                char resp[512];
                int rlen;
                if (auth_ack_hex[0]) {
                    rlen = snprintf(resp, sizeof(resp),
                        "RTSP/1.0 200 OK\r\n"
                        "User-Agent: stagefright/1.1 (Linux;Android 4.1)\r\n"
                        "CSeq: %d\r\n"
                        "Public: org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER\r\n"
                        "authKeyType:2\r\n"
                        "authAlgorithmVal:4\r\n"
                        "authMsgAck:%s\r\n"
                        "\r\n",
                        peer_cseq, auth_ack_hex);
                } else {
                    rlen = snprintf(resp, sizeof(resp),
                        "RTSP/1.0 200 OK\r\n"
                        "User-Agent: stagefright/1.1 (Linux;Android 4.1)\r\n"
                        "CSeq: %d\r\n"
                        "Public: org.wfa.wfd1.0, GET_PARAMETER, SET_PARAMETER\r\n"
                        "\r\n",
                        peer_cseq);
                }
                send(rtsp_sock, resp, rlen, 0);
            }
            ESP_LOGI(TAG, "[RTSP] → OPTIONS resp (cseq=%d)", peer_cseq);
            /* 发我们自己的 OPTIONS（带独立随机 challenge） */
            if (!sent_options) {
                char rtsp_chal[33] = {0};
                uint8_t rb[16];
                for (int i = 0; i < 16; i += 4) {
                    uint32_t r = esp_random();
                    memcpy(rb + i, &r, 4);
                }
                hex_to_lower(rb, 16, rtsp_chal);
                char extra[128];
                snprintf(extra, sizeof(extra),
                    "Require: org.wfa.wfd1.0\r\n"
                    "lib_version: audio-display-release2.1 2.1.5071614\r\n"
                    "authMsg:%s\r\n",
                    rtsp_chal);
                rtsp_send_req(rtsp_sock, "OPTIONS", "*", extra, cseq);
                ESP_LOGI(TAG, "[RTSP] → OPTIONS req (cseq=%d, authMsg=%.16s)", cseq, rtsp_chal);
                cseq_of_options = cseq;
                cseq++;
                sent_options = 1;
            }
        }
        else if (strcmp(method, "rtp_ports") == 0) {
            /* rtp_ports 有两种情况：
             * 1. 手机对我们 GET_PARAMETER 的响应 → 不回复
             * 2. 手机主动发的请求 → 回复 capabilities */
            if (peer_cseq == cseq_of_getparam) {
                ESP_LOGI(TAG, "[RTSP] rtp_ports is resp to our GET_PARAMETER (cseq=%d), ack", peer_cseq);
                cseq_of_getparam = 0;
            } else {
                const char *cap =
                "wfd_audio_codecs_v2: 15 3 3\r\n"
                "wfd_video_formats: none\r\n"
                "wfd_video_enctype: none\r\n"
                "wfd_video_gamuttype: none\r\n"
                "wfd_video_bitrate: none\r\n"
                "wfd_current_video_info: none\r\n"
                "wfd_client_rtp_ports: RTP/AVP/TCP;interleaved mode=play\r\n"
                "miplay_support_image: none\r\n"
                "wfd_standby_resume_capability: supported\r\n"
                "wfd_content_SP_protection: 4 1 256 3 1 1 0 0\r\n"
                "wfd_support_secure_win:enable\r\n"
                "device_info: -1 -1 -1 -1 -1 -1 -1\r\n";
                rtsp_send_resp(rtsp_sock, peer_cseq, cap);
                ESP_LOGI(TAG, "[RTSP] → rtp_ports resp (caps, cseq=%d)", peer_cseq);
            }
        }
        else if (strcmp(method, "GET_PARAMETER") == 0) {
            ESP_LOGI(TAG, "[RTSP] GET_PARAMETER body[%d]: %.80s", body_len, body);
            if (strstr(body, "wfd_audio_codecs") ||
                strstr(body, "wfd_client_rtp_ports") ||
                strstr(body, "session_enable") ||
                strstr(body, "wfd_support_secure_win")) {
                /* Rust/Python/备份版：先建数据 socket，再发响应
                 * 手机期望数据连接在收到 GET_PARAMETER 响应前就建立 */
                if (s_image_sock < 0) {
                    s_image_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
                    s_multi_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
                    if (s_image_sock >= 0 && s_multi_sock >= 0) {
                        struct timeval ctv = { .tv_sec = 3 };
                        setsockopt(s_image_sock, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));
                        setsockopt(s_multi_sock, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));
                        struct sockaddr_in daddr = {
                            .sin_family = AF_INET,
                            .sin_port = htons(rtsp_port_local),
                            .sin_addr.s_addr = inet_addr(rtsp_host_local),
                        };
                        if (connect(s_image_sock, (struct sockaddr *)&daddr, sizeof(daddr)) == 0 &&
                            connect(s_multi_sock, (struct sockaddr *)&daddr, sizeof(daddr)) == 0) {
                            struct sockaddr_in local_addr;
                            socklen_t addrlen = sizeof(local_addr);
                            getsockname(s_image_sock, (struct sockaddr *)&local_addr, &addrlen);
                            s_image_port = ntohs(local_addr.sin_port);
                            getsockname(s_multi_sock, (struct sockaddr *)&local_addr, &addrlen);
                            s_multi_port = ntohs(local_addr.sin_port);
                            ESP_LOGI(TAG, "[RTSP] Data sockets: image=%u multi=%u",
                                     (unsigned)s_image_port, (unsigned)s_multi_port);
                            /* img_drain: 对齐参考 12KB PSRAM 栈 */
                            miplay_create_task(image_drain_task, "img_drain",
                                               12 * 1024, (void*)(intptr_t)s_image_sock,
                                               3, NULL, 1);
                            s_image_sock = -1;
                        } else {
                            ESP_LOGW(TAG, "[RTSP] data socket connect failed (ok, using interleaved)");
                            close(s_image_sock); close(s_multi_sock);
                            s_image_sock = s_multi_sock = -1;
                        }
                    }
                }
                /* 建完 socket 后再发响应 */
                const char *cap =
                    "wfd_audio_codecs_v2: 15 3 3\r\n"
                    "wfd_video_formats: none\r\n"
                    "wfd_video_enctype: none\r\n"
                    "wfd_video_gamuttype: none\r\n"
                    "wfd_video_bitrate: none\r\n"
                    "wfd_current_video_info: none\r\n"
                    "wfd_client_rtp_ports: RTP/AVP/TCP;interleaved mode=play\r\n"
                    "miplay_support_image: none\r\n"
                    "wfd_standby_resume_capability: supported\r\n"
                    "wfd_content_SP_protection: 4 1 256 3 1 1 0 0\r\n"
                    "wfd_support_secure_win:enable\r\n"
                    "device_info: -1 -1 -1 -1 -1 -1 -1\r\n";
                rtsp_send_resp(rtsp_sock, peer_cseq, cap);
                ESP_LOGI(TAG, "[RTSP] → GET_PARAMETER resp (audio caps)");
            } else {
                rtsp_send_resp(rtsp_sock, peer_cseq, NULL);
            }
        }
        else if (strcmp(method, "SET_PARAMETER") == 0) {
            /* 解析 presentation_URL */
            char *purl = strstr(body, "wfd_presentation_URL:");
            if (purl) {
                purl += 21;  /* strlen("wfd_presentation_URL:") = 21 */
                while (*purl == ' ') purl++;
                int i = 0;
                while (purl[i] && purl[i] != ' ' && purl[i] != '\r' &&
                       purl[i] != '\n' && i < (int)sizeof(pres_url) - 1) {
                    pres_url[i] = purl[i];
                    i++;
                }
                pres_url[i] = 0;
                ESP_LOGI(TAG, "[RTSP] presentation_URL: %s", pres_url);
            }
            rtsp_send_resp(rtsp_sock, peer_cseq, NULL);

            /* 检查是否包含 SETUP trigger */
            if (strstr(body, "wfd_trigger_method: SETUP")) {
                ESP_LOGI(TAG, "[RTSP] Got SETUP trigger");
                char extra[128];
                if (s_image_port > 0 && s_multi_port > 0) {
                    snprintf(extra, sizeof(extra),
                             "Transport: RTP/AVP/TCP;interleaved=0-1\r\n"
                             "MultiPort: image_port=%u;multi_port=%u\r\n",
                             (unsigned)s_image_port, (unsigned)s_multi_port);
                } else {
                    snprintf(extra, sizeof(extra),
                             "Transport: RTP/AVP/TCP;interleaved=0-1\r\n");
                }
                rtsp_send_req(rtsp_sock, "SETUP", pres_url, extra, cseq);
                ESP_LOGI(TAG, "[RTSP] → SETUP (cseq=%d)", cseq);
                cseq_of_setup = cseq;
                cseq++;
                rtsp_state = 1;
            }
        }
        else if (strcmp(method, "PLAY") == 0) {
            /* 手机发 PLAY 请求 → 回200，启动媒体 */
            rtsp_send_resp(rtsp_sock, peer_cseq, NULL);
            ESP_LOGI(TAG, "[RTSP] → PLAY resp (STREAM STARTED!)");
            rtsp_state = 2;  /* 退出信令循环，启动媒体 */
        }
        else if (strcmp(method, "TEARDOWN") == 0) {
            rtsp_send_resp(rtsp_sock, peer_cseq, NULL);
            ESP_LOGI(TAG, "[RTSP] ← TEARDOWN, closing");
            break;
        }
        else {
            ESP_LOGW(TAG, "[RTSP] unhandled: %s (cseq=%d, body_len=%d)",
                     method, peer_cseq, body_len);
            if (body_len > 0) ESP_LOGW(TAG, "[RTSP] body: %.120s", body);
            rtsp_send_resp(rtsp_sock, peer_cseq, NULL);
        }
    }

    /* ── 媒体流接收阶段：启动独立任务，返回主控制循环处理心跳 ── */
    int media_sock = (s_multi_sock >= 0) ? s_multi_sock : rtsp_sock;
    if (rtsp_state >= 2) {
        media_task_arg_t *marg = malloc(sizeof(media_task_arg_t));
        if (marg) {
            marg->media_sock = media_sock;
            marg->client_sock = client_sock;
            marg->rtsp_sock = rtsp_sock;
            marg->generation = generation;
            ESP_LOGI(TAG, "[MEDIA] Launching media task (stack=%d)...", MEDIA_TASK_STACK_SIZE);
            ESP_LOGI(TAG, "[MEDIA] Heap: %lu free, PSRAM: %lu free, min_free: %lu",
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     (unsigned long)esp_get_minimum_free_heap_size());
            TaskHandle_t th = NULL;
            /* 对齐参考 128KB PSRAM 栈 */
            if (miplay_create_task(media_receive_task, "media_rx",
                                   MEDIA_TASK_STACK_SIZE, marg, 8, &th, 1) != pdPASS) {
                ESP_LOGE(TAG, "[MEDIA] Task creation failed");
                free(marg);
                th = NULL;
            }
            if (th) {
                ESP_LOGI(TAG, "[MEDIA] Task launched, continuing RTSP loop for keepalive");
                s_image_sock = -1; s_multi_sock = -1;
                /* 不 return！继续维护 RTSP 控制连接，响应手机的 GET_PARAMETER keepalive */
            } else {
                ESP_LOGE(TAG, "[MEDIA] Task creation failed (no PSRAM stack?)");
                free(marg);
            }
        }
    }

    /* ── PLAY 后继续维护 RTSP 连接（响应 GET_PARAMETER / TEARDOWN）── */
    if (rtsp_state >= 2) {
        ESP_LOGI(TAG, "[RTSP] Entering keepalive loop...");
        TickType_t loop_start = xTaskGetTickCount();
        uint32_t keepalive_count = 0;
        while (s_running) {
            int ret = rtsp_read_msg(rtsp_sock, headers, RTSP_BUF_SIZE,
                                     body, RTSP_BUF_SIZE, &body_len);
            if (ret <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;  /* 2秒超时，继续 */
                if (ret == 0) {
                    /* 对端发 FIN：可能是手机侧超时/异常，也可能是我们长期没响应 keepalive。
                     * 记录存活时长和已响应次数，便于判断是我们卡住还是手机侧主动退出。 */
                    ESP_LOGW(TAG, "[RTSP] sock closed by peer after %lu ms, keepalives_acked=%lu, media_pkts=%lu",
                             (unsigned long)((xTaskGetTickCount() - loop_start) * portTICK_PERIOD_MS),
                             (unsigned long)keepalive_count, (unsigned long)s_media_pkt_total);
                } else {
                    ESP_LOGW(TAG, "[RTSP] Read error ret=%d errno=%d after %lu ms, keepalives=%lu",
                             ret, errno,
                             (unsigned long)((xTaskGetTickCount() - loop_start) * portTICK_PERIOD_MS),
                             (unsigned long)keepalive_count);
                }
                break;
            }
            int peer_cseq = rtsp_get_cseq(headers);
            int is_resp = (strncmp(headers, "RTSP/1.0 ", 9) == 0);
            if (is_resp) {
                ESP_LOGI(TAG, "[RTSP] ← resp cseq=%d (keepalive)", peer_cseq);
                continue;
            }
            char method[32] = {0};
            sscanf(headers, "%31s", method);
            ESP_LOGI(TAG, "[RTSP] ← %s (cseq=%d, keepalive)", method, peer_cseq);
            if (strcmp(method, "GET_PARAMETER") == 0) {
                /* 手机发来的 keepalive — 回复空 body */
                rtsp_send_resp(rtsp_sock, peer_cseq, NULL);
                keepalive_count++;
                ESP_LOGI(TAG, "[RTSP] GET_PARAMETER ack cseq=%d (count=%lu)",
                         peer_cseq, (unsigned long)keepalive_count);
            } else if (strcmp(method, "TEARDOWN") == 0) {
                rtsp_send_resp(rtsp_sock, peer_cseq, NULL);
                ESP_LOGI(TAG, "[RTSP] ← TEARDOWN, stopping");
                s_running = false;
                break;
            } else {
                rtsp_send_resp(rtsp_sock, peer_cseq, NULL);
                keepalive_count++;
                ESP_LOGI(TAG, "[RTSP] %s ack cseq=%d", method, peer_cseq);
            }
        }
    }
    /* 任务创建失败或 RTSP 连接失败：清理资源并返回 */
rtsp_cleanup:
    MIPLAY_FREE(headers);
    MIPLAY_FREE(body);
    if (rtsp_sock >= 0) close(rtsp_sock);
    if (s_image_sock >= 0) { close(s_image_sock); s_image_sock = -1; }
    if (s_multi_sock >= 0) { close(s_multi_sock); s_multi_sock = -1; }
    s_image_port = s_multi_port = 0;
    ESP_LOGI(TAG, "[RTSP] Cleanup done");
    /* 250ms 错误恢复退避 — 防止快速重连冲击手机端 */
    vTaskDelay(pdMS_TO_TICKS(250));
}

/* ══════════════════════════════════════════════════════════════════════
 * SET_MEDIA_INFO 元数据解析（0x12 / 0x18 共用）
 *
 * 参考 FusionPlay protocol.rs media_info_value() 三层剥壳:
 *   {"mediaInfo":{...}} / {"mediaInfoEx":{...}} / {"metadata":"..."}
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * @brief 在 JSON 字符串中查找 "key":"value" 或 "key":number
 *        跳过嵌套层级，只匹配顶层键值对。
 * @return value 起始位置（跳过引号），或 NULL
 */
static const char *json_find_value(const char *json, const char *key, size_t *vlen)
{
    /* 构建搜索模式 "key" */
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ') p++;
    /* 值可能是 "string" 或 number/bool */
    if (*p == '"') {
        p++;  /* 跳过开引号 */
        const char *start = p;
        /* 找闭引号（跳过转义的 \"）*/
        while (*p && !(*p == '"' && *(p - 1) != '\\')) p++;
        *vlen = p - start;
        return start;
    } else {
        /* 数字/布尔值 */
        const char *start = p;
        while (*p && *p != ',' && *p != '}' && *p != ' ') p++;
        *vlen = p - start;
        return start;
    }
}

/**
 * @brief 给定 JSON 文本，定位 key 的字符串值，并**反转义**拷贝到 out。
 *
 * 与 json_find_value 的区别：返回的是反转义后的干净副本（处理 \" \\ \/ \uXXXX
 * 等），而非指向原缓冲的指针。Xiaomi mArt/data-URI 常把 URL 的 '/' 写成 '\/'，
 * 若不反转义，base64/URL 会混入反斜杠导致下游解码失败。
 *
 * @param json  JSON 文本（可在原始 TCP payload 上操作，无 NUL 需 len）
 * @param len   JSON 长度
 * @param key   键名（不含引号）
 * @param out   输出缓冲（PSRAM 传入更佳）
 * @param out_size
 * @return 反转义后字符串长度（不含结尾 NUL），未找到/失败返回 -1
 */
static int json_copy_unescaped(const char *json, size_t len, const char *key,
                               char *out, size_t out_size)
{
    /* 在限定 len 内搜 "key" 位置 */
    char pattern[40];
    int pl = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (pl <= 0 || (size_t)pl >= sizeof(pattern)) return -1;
    /* 逐字节搜（避免超 len） */
    const char *end = json + len;
    const char *p = json;
    while (p + pl <= end) {
        const char *hit = strstr(p, pattern);
        if (!hit || hit + pl > end) break;
        bool at_key = (hit == json) || strchr("{, \t\r\n", hit[-1]) != NULL;
        const char *after = hit + pl;
        while (after < end && (*after == ' ' || *after == '\t')) after++;
        if (at_key && after < end && *after == ':') {
            after++;
            while (after < end && (*after == ' ' || *after == '\t')) after++;
            if (after < end && *after == '"') {
                after++;  /* 跳过开引号 */
                size_t n = 0;
                while (after < end && *after != '"') {
                    char c = *after;
                    if (c == '\\' && after + 1 < end) {
                        after++;
                        char e = *after;
                        if (e == 'u' && after + 5 <= end) {
                            /* \uXXXX → UTF-8（仅支持 BMP 及可简单处理范围） */
                            unsigned cp = 0;
                            const char *hx = after + 1;
                            for (int i = 0; i < 4; i++) {
                                char h = hx[i];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                                else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                                else return -1;
                            }
                            if (cp < 0x80) {
                                if (n + 1 >= out_size) return -1;
                                out[n++] = (char)cp;
                            } else if (cp < 0x800) {
                                if (n + 2 >= out_size) return -1;
                                out[n++] = (char)(0xC0 | (cp >> 6));
                                out[n++] = (char)(0x80 | (cp & 0x3F));
                            } else if (cp < 0x10000) {
                                if (n + 3 >= out_size) return -1;
                                out[n++] = (char)(0xE0 | (cp >> 12));
                                out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                                out[n++] = (char)(0x80 | (cp & 0x3F));
                            } else {
                                return -1;
                            }
                            after += 5;
                        } else {
                            /* \b \f \n \r \t \\ \" \/ */
                            switch (e) {
                                case 'b': c = '\b'; break;
                                case 'f': c = '\f'; break;
                                case 'n': c = '\n'; break;
                                case 'r': c = '\r'; break;
                                case 't': c = '\t'; break;
                                default:  c = e;      break; /* \\ \" \/ → 原字符 */
                            }
                            if (n + 1 >= out_size) return -1;
                            out[n++] = c;
                        }
                    } else {
                        if (n + 1 >= out_size) return -1;
                        out[n++] = c;
                    }
                    after++;
                }
                if (after >= end || *after != '"') return -1;  /* 未闭合 */
                out[n] = '\0';
                return (int)n;
            }
        }
        p = hit + 1;
    }
    return -1;
}

/**
 * @brief 在 JSON 里找一个数字/布尔字段（可带引号或裸数字），返回 int64。
 */
static bool json_find_num(const char *json, size_t len, const char *key, int64_t *out)
{
    char pattern[40];
    int pl = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (pl <= 0 || (size_t)pl >= sizeof(pattern)) return false;
    const char *end = json + len;
    for (const char *p = json; p + pl <= end; ) {
        const char *hit = strstr(p, pattern);
        if (!hit || hit + pl > end) return false;
        bool at_key = (hit == json) || strchr("{, \t\r\n", hit[-1]) != NULL;
        const char *after = hit + pl;
        while (after < end && (*after == ' ' || *after == '\t')) after++;
        if (at_key && after < end && *after == ':') {
            after++;
            while (after < end && (*after == ' ' || *after == '\t')) after++;
            if (after < end && *after == '"') after++;  /* 引号数字 */
            char *ep = NULL;
            long long v = strtoll(after, &ep, 10);
            if (ep != after) { *out = v; return true; }
            return false;
        }
        p = hit + 1;
    }
    return false;
}

/**
 * @brief 剥去 JSON 嵌套壳，返回实际元数据对象的起始位置。
 *
 * HyperOS 的 0x12/0x18 payload 可能是：
 *   {"mediaInfo":{"mTitle":...}}           — object 壳
 *   {"mediaInfoEx":{"mTitle":...}}         — object 壳
 *   {"metadata":"{\"mTitle\":...}"}        — string 壳（字符串化的 JSON）
 *   {"mTitle":...}                         — 无壳
 *
 * 参考 FusionPlay protocol.rs media_info_value() 的三层尝试。
 *
 * @param payload 原始 payload（可能 NUL 终止也可能不）
 * @param plen    payload 长度
 * @param buf     输出缓冲区（当需要 unescape stringified JSON 时使用）
 * @param buf_len 缓冲区大小
 * @return 解析起始位置（= payload 或 = buf），NULL 表示解析失败
 */
static const char *strip_media_info_shell(const char *payload, size_t plen,
                                          char *buf, size_t buf_len)
{
    /* 确保 NUL 终止（payload 来自 TCP 帧，可能不含 NUL）*/
    if (!payload || plen == 0) return NULL;

    /* 尝试三个壳键 */
    static const char *shell_keys[] = {"mediaInfo", "mediaInfoEx", "metadata"};
    for (int i = 0; i < 3; i++) {
        char pattern[40];
        snprintf(pattern, sizeof(pattern), "\"%s\"", shell_keys[i]);
        const char *found = strstr(payload, pattern);
        if (!found) continue;
        const char *p = found + strlen(pattern);
        /* 跳到冒号后的值 */
        const char *end = payload + plen;
        while (p < end && *p == ' ') p++;
        if (p >= end || *p != ':') continue;
        p++;
        while (p < end && *p == ' ') p++;
        if (p >= end) continue;

        if (*p == '{') {
            /* object 壳：直接返回 { 位置 */
            return p;
        }
        if (*p == '"') {
            /* string 壳：metadata 的值是字符串化的 JSON，需要 unescape */
            p++;  /* 跳过开引号 */
            size_t out_pos = 0;
            while (p < end && *p != '"') {
                if (*p == '\\' && (p + 1) < end) {
                    p++;
                    if (*p == '"') {
                        if (out_pos < buf_len - 1) buf[out_pos++] = '"';
                    } else if (*p == '\\') {
                        if (out_pos < buf_len - 1) buf[out_pos++] = '\\';
                    } else if (*p == 'n') {
                        if (out_pos < buf_len - 1) buf[out_pos++] = '\n';
                    } else if (*p == 'r') {
                        if (out_pos < buf_len - 1) buf[out_pos++] = '\r';
                    } else if (*p == 't') {
                        if (out_pos < buf_len - 1) buf[out_pos++] = '\t';
                    } else if (*p == 'u' && (p + 4) < end) {
                        /* \uXXXX → 简单保留 ASCII 范围 */
                        char hex[5] = {p[1], p[2], p[3], p[4], 0};
                        unsigned cp = (unsigned)strtoul(hex, NULL, 16);
                        if (cp < 0x80 && out_pos < buf_len - 1) {
                            buf[out_pos++] = (char)cp;
                        }
                        p += 4;
                    } else {
                        if (out_pos < buf_len - 1) buf[out_pos++] = *p;
                    }
                } else {
                    if (out_pos < buf_len - 1) buf[out_pos++] = *p;
                }
                p++;
            }
            buf[out_pos] = '\0';
            if (out_pos > 0) return buf;
        }
    }
    /* 无壳：直接返回原 payload */
    return payload;
}

/**
 * @brief 解析 SET_MEDIA_INFO (0x12/0x18) 的 JSON payload，上报到 now_playing。
 *        0x12 和 0x18 共用此函数。
 *
 * @param payload JSON payload
 * @param plen    payload 长度
 * @return pending_cover（PSRAM 分配的完整 cover base64，调用方负责 free）
 */
/* 解析时先把各字段反转义拷到临时缓冲，再决定是否采用。
 * 小于返回 true 时 out 已是干净字符串。 */
static bool parse_field_unescaped(const char *json, size_t len,
                                  const char *const *aliases,
                                  char *out, size_t out_size)
{
    for (int k = 0; aliases[k]; k++) {
        /* 必须取到非空值才算命中：实测 HyperOS 大帧 SetMediaInfo 里
         * "mCoverUrl":"" 在前、"mArt":"UklGR..."(base64 WebP 封面) 在后，
         * 若空值短路返回 true，多别名 fallback 永远轮不到 mArt。
         * 调用方本就判 out[0]，此处收紧语义不改任何调用方行为。 */
        if (json_copy_unescaped(json, len, aliases[k], out, out_size) >= 0 && out[0])
            return true;
    }
    return false;
}

static char *parse_media_info(const uint8_t *payload, size_t plen)
{
    char *pending_cover = NULL;
    if (!payload || plen == 0) return NULL;

    /* 字段名列表（对齐参考 esp_miplay-main + FusionPlay protocol.rs） */
    static const char *title_keys[]  = {"mTitle", "title", "songName", "mSongName", NULL};
    static const char *artist_keys[] = {"mArtist", "artist", "singer", "artistName", NULL};
    static const char *album_keys[]  = {"mAlbum", "album", NULL};
    static const char *cover_keys[]  = {
        "mCoverUrl", "coverUrl", "mArt", "artwork", "artworkUrl",
        "albumArt", "albumArtURI", "albumArtUrl", "cover_url", "cover", "art",
        NULL
    };
    static const char *duration_keys[] = {"mDuration", "duration", "durationMs", "duration_ms", NULL};
    static const char *position_keys[] = {"mPosition", "position", "positionMs", "position_ms", NULL};
    static const char *status_keys[]   = {"status", "playStatus", "setState", "mediaState", "playState", NULL};

    /* 1. PSRAM 拷贝 payload + NUL 终止（对齐参考，带回退避免元数据整块丢失） */
    char *json = (char *)miplay_psram_prefer_alloc(plen + 1);
    if (!json) {
        ESP_LOGE(TAG, "parse_media_info: alloc %u failed", (unsigned)(plen + 1));
        return NULL;
    }
    memcpy(json, payload, plen);
    json[plen] = '\0';
    ESP_LOGI(TAG, "[META] payload=%uB head=%.120s", (unsigned)plen, json);

    const char *p_src = json;
    size_t p_len = plen;
    char *nested = NULL;

    /* 2. Shell 剥壳：mediaInfo / mediaInfoEx / metadata */
    static const char *shell_keys[] = {"mediaInfo", "mediaInfoEx", "metadata"};
    for (int i = 0; i < 3 && !nested; i++) {
        char pat[40];
        int pl = snprintf(pat, sizeof(pat), "\"%s\"", shell_keys[i]);
        if (pl <= 0 || (size_t)pl >= sizeof(pat)) continue;
        const char *hit = NULL;
        for (const char *q = p_src; (hit = strstr(q, pat)) && hit - p_src + pl <= (long)p_len; q = hit + 1) {
            bool at_key = (hit == p_src) || strchr("{, \t\r\n", hit[-1]) != NULL;
            if (!at_key) continue;
            const char *after = hit + pl;
            while (after < p_src + p_len && (*after == ' ' || *after == '\t')) after++;
            if (after >= p_src + p_len || *after != ':') continue;
            after++;
            while (after < p_src + p_len && (*after == ' ' || *after == '\t')) after++;
            if (after < p_src + p_len && *after == '"') {
                char *tmp = heap_caps_malloc(p_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (tmp) {
                    int r = json_copy_unescaped(p_src, p_len, shell_keys[i], tmp, p_len + 1);
                    if (r > 0 && (tmp[0] == '{' || tmp[0] == '[')) {
                        nested = tmp;
                        p_src = nested;
                        p_len = (size_t)r;
                        break;
                    }
                    heap_caps_free(tmp);
                }
            }
        }
    }

    /* 3. 各字段提取 */
    char buf[256];  /* 仅用于短字段：title/artist/album/duration/position/status */

    /* title */
    if (parse_field_unescaped(p_src, p_len, title_keys, buf, sizeof(buf)) && buf[0]) {
        strlcpy(s_media_title, buf, sizeof(s_media_title));
    }
    /* artist */
    if (parse_field_unescaped(p_src, p_len, artist_keys, buf, sizeof(buf)) && buf[0]) {
        strlcpy(s_media_artist, buf, sizeof(s_media_artist));
    }
    /* album */
    if (parse_field_unescaped(p_src, p_len, album_keys, buf, sizeof(buf)) && buf[0]) {
        strlcpy(s_media_album, buf, sizeof(s_media_album));
    }
    /* duration */
    s_media_duration = 0;
    for (int k = 0; duration_keys[k]; k++) {
        int64_t num = 0;
        char dtmp[40];
        if (json_copy_unescaped(p_src, p_len, duration_keys[k], dtmp, sizeof(dtmp)) >= 0 && dtmp[0]) {
            num = atoll(dtmp);
        } else if (json_find_num(p_src, p_len, duration_keys[k], &num)) {
        }
        if (num > 0) { s_media_duration = (uint32_t)num; break; }
    }
    /* position */
    s_media_position = 0;
    for (int k = 0; position_keys[k]; k++) {
        int64_t num = 0;
        char ptmp[40];
        if (json_copy_unescaped(p_src, p_len, position_keys[k], ptmp, sizeof(ptmp)) >= 0 && ptmp[0]) {
            num = atoll(ptmp);
        } else if (json_find_num(p_src, p_len, position_keys[k], &num)) {
        }
        if (num > 0) { s_media_position = (uint32_t)num; break; }
    }
    /* status */
    for (int k = 0; status_keys[k]; k++) {
        if (json_copy_unescaped(p_src, p_len, status_keys[k], buf, sizeof(buf)) >= 0 && buf[0]) {
            int64_t num = 0;
            if (buf[0] >= '0' && buf[0] <= '9') {
                num = atoll(buf);
            } else if (strcasecmp(buf, "playing") == 0) {
                num = 2;
            } else if (strcasecmp(buf, "paused") == 0) {
                num = 3;
            } else if (strcasecmp(buf, "stopped") == 0) {
                num = 0;
            }
            if (num >= 0) { s_media_status = (int)num; break; }
        }
    }

    /* cover — PSRAM 动态分配提取缓冲，避免静态 48KB 缓冲截断大 base64 封面。
     * 实测 "英雄之诗" mArt base64 ≈ 55KB，溢出 s_media_cover_url[48KB] 导致
     * json_copy_unescaped 丢失闭合引号、返回 -1、封面静默丢失。 */
    s_media_cover_url[0] = '\0';
    {
        char *cover_tmp = (char *)miplay_psram_prefer_alloc(p_len + 1);
        if (cover_tmp) {
            if (parse_field_unescaped(p_src, p_len, cover_keys,
                                      cover_tmp, p_len + 1) && cover_tmp[0]) {
                size_t bl = strlen(cover_tmp);
                pending_cover = (char *)miplay_psram_prefer_alloc(bl + 1);
                if (pending_cover) {
                    memcpy(pending_cover, cover_tmp, bl + 1);
                    ESP_LOGI(TAG, "[META] cover extracted len=%u head=%.60s",
                             (unsigned)bl, pending_cover);
                } else {
                    ESP_LOGW(TAG, "[META] cover alloc %u failed, cover dropped", (unsigned)(bl + 1));
                }
                /* GetMediaInfo 回读：截断存入静态缓冲 */
                strlcpy(s_media_cover_url, cover_tmp, sizeof(s_media_cover_url));
            } else {
                ESP_LOGI(TAG, "[META] no cover field matched (keys tried: mCoverUrl/coverUrl/mArt/...)");
                /* 诊断：打印 payload 所有 JSON key，确认 cover 类字段是否存在及确切名。
                 * 若手机真没发封面，key 清单里就看不到任何 cover/art 项。 */
                {
                    char keybuf[80];
                    const char *end = p_src + p_len;
                    for (const char *q = p_src; q < end && *q; ) {
                        if (*q != '"') { q++; continue; }
                        const char *eq = strchr(q + 1, '"');
                        if (!eq || eq >= end) break;
                        if (eq - q - 1 > (long)sizeof(keybuf) - 1) { q = eq + 1; continue; }
                        const char *colon = eq + 1;
                        while (colon < end && (*colon == ' ' || *colon == '\t')) colon++;
                        if (colon >= end || *colon != ':') { q = eq + 1; continue; }
                        size_t klen = (size_t)(eq - q - 1);
                        memcpy(keybuf, q + 1, klen);
                        keybuf[klen] = '\0';
                        const char *vs = colon + 1;
                        while (vs < end && (*vs == ' ' || *vs == '\t')) vs++;
                        char vbuf[48]; int vn = 0;
                        const char *vstop;
                        if (vs < end && *vs == '"') {
                            const char *v1 = vs + 1;
                            const char *vq = strchr(v1, '"');
                            vstop = (vq && vq < end) ? vq + 1 : end;
                            while (v1 < end && *v1 != '"' && vn < (int)sizeof(vbuf) - 1) vbuf[vn++] = *v1++;
                        } else {
                            vstop = vs;
                            while (vstop < end && *vstop != ',' && *vstop != '}') vstop++;
                            while (vs < vstop && vn < (int)sizeof(vbuf) - 1) vbuf[vn++] = *vs++;
                        }
                        vbuf[vn] = '\0';
                        ESP_LOGI(TAG, "  [key] \"%s\" = \"%.*s%s\"", keybuf,
                                 vn > 40 ? 40 : vn, vbuf, vn > 40 ? "..." : "");
                        const char *next = vstop;
                        while (next < end && *next != ',' && *next != '}') next++;
                        q = (next < end) ? next + 1 : end;
                    }
                }
                heap_caps_free(cover_tmp);
            }
        } else {
            ESP_LOGW(TAG, "[META] cover tmp alloc %u failed", (unsigned)(p_len + 1));
        }
    }

    MIPLAY_FREE(nested);
    MIPLAY_FREE(json);

    ESP_LOGI(TAG, "Media: title=%.32s artist=%.24s album=%.24s dur=%ums pos=%ums status=%d cover=%d",
             s_media_title, s_media_artist, s_media_album,
             (unsigned)s_media_duration, (unsigned)s_media_position,
             s_media_status, pending_cover ? 1 : 0);

    /* 上报元数据到 UI（统一走 now_playing，与 DLNA 同路径）。
     * 对齐参考: 只要任一字段有效就上报，不能要求 title 和 artist 同时非空。 */
    if (s_media_title[0] || s_media_artist[0] || s_media_album[0] || s_media_duration > 0) {
        np_meta_t m;
        memset(&m, 0, sizeof(m));
        strlcpy(m.title,  s_media_title,  sizeof(m.title));
        strlcpy(m.artist, s_media_artist, sizeof(m.artist));
        strlcpy(m.album,  s_media_album,  sizeof(m.album));
        m.duration_ms = s_media_duration;
        m.has_cover = (pending_cover != NULL);
        ESP_LOGI(TAG, "[META] np_submit title=%.24s artist=%.16s dur=%ums has_cover=%d",
                 m.title, m.artist, (unsigned)m.duration_ms, (int)m.has_cover);
        np_submit(NP_SRC_MIPLAY, &m);
    } else {
        ESP_LOGW(TAG, "[META] all fields empty, skip np_submit");
    }
    /* cover 源在 np_submit 之后交付，避免 np_set_source 清掉 */
    if (pending_cover) {
        ESP_LOGI(TAG, "[META] np_set_cover_url len=%u", (unsigned)strlen(pending_cover));
        np_set_cover_url(pending_cover);
        /* 不在此处 free，调用方负责释放（cover_ref） */
    }
    return pending_cover;
}

/* ══════════════════════════════════════════════════════════════════════
 * TCP 客户端处理
 * ══════════════════════════════════════════════════════════════════════ */

static void disconnect_cleanup(int client_sock)
{
    s_active_client_sock = -1;
    s_connected = false;
    close(client_sock);
    s_has_session_key = false;
    s_has_stream_key = false;
    s_has_mirror_auth_key = false;
    memset(s_aes_key, 0, sizeof(s_aes_key));
    memset(s_encrypt_iv, 0, sizeof(s_encrypt_iv));
    memset(s_decrypt_iv, 0, sizeof(s_decrypt_iv));
    memset(s_auth_key, 0, sizeof(s_auth_key));
    memset(s_auth_msg, 0, sizeof(s_auth_msg));
    /* 清除媒体元数据（防止重连后 GetMediaInfo 回读旧数据） */
    memset(s_media_title, 0, sizeof(s_media_title));
    memset(s_media_artist, 0, sizeof(s_media_artist));
    memset(s_media_album, 0, sizeof(s_media_album));
    memset(s_media_cover_url, 0, sizeof(s_media_cover_url));
    s_media_duration = 0;
    rtsp_read_msg_reset();
    if (s_connected_cb) s_connected_cb(false);
}

static void handle_client(int client_sock, struct sockaddr_in *client_addr)
{
    ESP_LOGI(TAG, "=== Client: %s:%d ===",
             inet_ntoa(client_addr->sin_addr), ntohs(client_addr->sin_port));

    miplay_state_t state = STATE_VERSION_EXCHANGE;
    uint16_t rx_seq = 0;
    uint16_t tx_seq = 0;
    (void)state;   /* 状态机用于调试/未来扩展 */
    (void)rx_seq;
    (void)tx_seq;

    /* SafetyAuth 流程状态（PC 原型语义） */
    bool auth_challenge_sent = false;   /* 我方 challenge 已随 SafetyInfoAck 发出 */
    s_notify_seq = 8;                   /* NOTIFY 序列号计数器（递增，避免重复） */
    s_active_client_sock = client_sock; /* 供外部 API 使用 */
    bool pending_ack_valid = false;     /* 对端 0x1402 的 ack 待其 0x1403 验证后补发 */
    char pending_ack_hex[65] = {0};
    uint16_t pending_ack_seq = 0;

    /* 获取 TCP 四元组用于密钥派生 */
    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    getsockname(client_sock, (struct sockaddr *)&local_addr, &addr_len);
    uint32_t peer_ip = client_addr->sin_addr.s_addr;
    uint16_t peer_port = ntohs(client_addr->sin_port);
    uint32_t local_ip = local_addr.sin_addr.s_addr;
    uint16_t local_port = ntohs(local_addr.sin_port);

    uint8_t *buf = heap_caps_malloc(MIPLAY_RX_BUF_LEN, MALLOC_CAP_SPIRAM);
    if (!buf) { close(client_sock); return; }
    int buf_cap = MIPLAY_RX_BUF_LEN;   /* 当前缓冲容量（大帧时临时扩容） */
    int buf_used = 0;
    char challenge[20];
    generate_challenge(challenge, sizeof(challenge));

    ESP_LOGI(TAG, "Endpoints: peer=%lu:%u local=%lu:%u challenge=%s",
             (unsigned long)peer_ip, peer_port, (unsigned long)local_ip, local_port, challenge);

    /* 握手顺序（基于验证通过的 PC 原型）：
     * [TV→Phone] 0x28 DEVICE_ID (14位数字ID)
     * [Phone→TV] 0x36 GET_VERSION
     * [TV→Phone] 0x37 VERSION_ACK "2.1.5071614\0"
     * [Phone→TV] 0x29 AUTH_ACK (32 hex chars)
     * [TV→Phone] 0x22 NOTIFY(5,6,7) 能力声明
     * [Phone→TV] 0x1400 SafetyInfo (未加密, outer=0x14)
     * [TV→Phone] 0x1401 SafetyInfoAck (plain wrapper) + 0x1402 SafetyAuth challenge (encrypted)
     * [Phone→TV] 0x1402 SafetyAuth (encrypted) + 0x1403 SafetyAuthAck (encrypted)
     * [TV→Phone] 0x1403 SafetyAuthAck (encrypted, JSON with \n\t indent)
     * → Security channel established
     */

    /* 发送 DEVICE_ID(0x28) — 数字设备ID, seq=4 */
    derive_session_key(peer_ip, peer_port, local_ip, local_port);
    generate_auth_msg();
    {
        /* 生成14位数字设备ID */
        uint8_t md5[16];
        char id_buf[32];
        int id_len = snprintf(id_buf, sizeof(id_buf), "%02X%02X%02X%02X%02X%02X",
                              s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
        mbedtls_md5((uint8_t *)id_buf, id_len, md5);
        uint64_t num_id = ((uint64_t)md5[0] << 56) | ((uint64_t)md5[1] << 48) |
                          ((uint64_t)md5[2] << 40) | ((uint64_t)md5[3] << 32) |
                          ((uint64_t)md5[4] << 24) | ((uint64_t)md5[5] << 16) |
                          ((uint64_t)md5[6] << 8)  | md5[7];
        num_id %= 100000000000000ULL; /* 10^14 */
        char did_str[16];
        snprintf(did_str, sizeof(did_str), "%014llu", (unsigned long long)num_id);
        miplay_send_cmd(client_sock, CMD_SAFETY_CHALLENGE, 4,
                        (const uint8_t *)did_str, strlen(did_str));
        ESP_LOGI(TAG, "-> DEVICE_ID(0x28): %s seq=4", did_str);
    }

    while (s_running) {
        /* select() 同时监听 client socket 和 listen socket。
         * 新连接到达时 listen 可读 → 立即退出 → accept 新连接（秒切）。
         * 超时 500ms（匹配逆向报告 RTSP 控制读超时）。 */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(client_sock, &rfds);
        FD_SET(s_listen_sock, &rfds);
        int maxfd = (client_sock > s_listen_sock) ? client_sock : s_listen_sock;
        struct timeval sel_tv = { .tv_usec = 500000 };
        int sr = select(maxfd + 1, &rfds, NULL, NULL, &sel_tv);
        if (sr < 0) { ESP_LOGW(TAG, "select error: %d", errno); break; }
        if (sr == 0) continue;  /* 500ms 超时，重试 */
        if (FD_ISSET(s_listen_sock, &rfds)) {
            ESP_LOGI(TAG, "New connection pending, exiting current session");
            break;  /* 新连接到达 → 退出 handle_client → accept 新连接 */
        }
        if (!FD_ISSET(client_sock, &rfds)) continue;

        int space = buf_cap - buf_used;
        if (space <= 0) {
            ESP_LOGW(TAG, "RX buffer full (%d bytes), discarding", buf_used);
            buf_used = 0;
            continue;
        }
        int n = recv(client_sock, buf + buf_used, space, 0);
        if (n <= 0) {
            if (n == 0) ESP_LOGI(TAG, "Client disconnected");
            else ESP_LOGW(TAG, "Recv error: %d", errno);
            break;
        }
        buf_used += n;

        /* 处理完整帧 */
        while (buf_used >= MIPLAY_FRAME_HDR_LEN) {
            /* 解析9字节大端序头 */
            if (buf[0] != MIPLAY_FRAME_MAGIC) {
                /* 扫描下一个 magic 字节位置，避免逐字节 memmove 死循环 */
                int skip = 1;
                while (skip < buf_used && buf[skip] != MIPLAY_FRAME_MAGIC) skip++;
                if (skip > 0) {
                    memmove(buf, buf + skip, buf_used - skip);
                    buf_used -= skip;
                }
                continue;
            }
            uint8_t outer_type = buf[1];   /* 帧外型: 0x00 普通 / 0x14 逻辑(Safety) */
            /* 验证 outer_type 合法性：过滤加密数据中的 false positive */
            if (outer_type != 0x00 && outer_type != 0x04 && outer_type != 0x14) {
                /* 0x24 在加密数据中误匹配，跳过这个字节 */
                memmove(buf, buf + 1, buf_used - 1);
                buf_used--;
                continue;
            }
            uint16_t cmd = ((uint16_t)buf[1] << 8) | buf[2];
            uint16_t seq = ((uint16_t)buf[3] << 8) | buf[4];
            uint32_t plen = ((uint32_t)buf[5]<<24)|((uint32_t)buf[6]<<16)|
                            ((uint32_t)buf[7]<<8)|buf[8];
            uint32_t total = MIPLAY_FRAME_HDR_LEN + plen;

            if (plen > MIPLAY_MAX_FRAME_PAYLOAD) {
                /* 对齐参考 esp_miplay-main: Frame too large 时只向前扫到下一个
                 * magic 字节（skip ≤ buf_used，界内安全）。切勿用天文数字 plen/total
                 * 做 memmove：plen 可达数十亿，(int)total 溢出为负后
                 * buf_used>=(int)total 恒真 → memmove(buf, buf+负数, 巨大) 越界写，
                 * 破坏 lwIP 内核内存导致 Guru Meditation panic（切歌偶发崩溃根因）。 */
                ESP_LOGW(TAG, "Frame too large (%lu), skip to next magic", (unsigned long)plen);
                int skip = 1;
                while (skip < buf_used && buf[skip] != MIPLAY_FRAME_MAGIC) skip++;
                memmove(buf, buf + skip, buf_used - skip);
                buf_used -= skip;
                continue;
            }
            if (buf_used < (int)total) {
                /* 大帧（如 27KB 封面 SetMediaInfo）超过常驻缓冲：临时扩容 PSRAM
                 * 缓冲继续收齐整帧。堆分配用完即释放，不常驻，规避历史教训
                 * "永久扩 RX 缓冲导致断联"。上限已在 plen 检查处卡死 64KB。 */
                if ((int)total > buf_cap) {
                    int new_cap = (int)total;
                    if (new_cap > (int)(MIPLAY_MAX_FRAME_PAYLOAD + MIPLAY_FRAME_HDR_LEN))
                        new_cap = (int)(MIPLAY_MAX_FRAME_PAYLOAD + MIPLAY_FRAME_HDR_LEN);
                    uint8_t *big = heap_caps_malloc(new_cap, MALLOC_CAP_SPIRAM);
                    if (!big) {
                        /* PSRAM 暂时不足：丢弃已收部分，按 skip-to-magic 自恢复 */
                        ESP_LOGW(TAG, "Big-frame alloc %d failed, resync", new_cap);
                        memmove(buf, buf + 1, buf_used - 1);
                        buf_used--;
                        continue;
                    }
                    memcpy(big, buf, buf_used);
                    heap_caps_free(buf);
                    buf = big;
                    buf_cap = new_cap;
                    ESP_LOGI(TAG, "RX buffer grown to %d for frame len=%lu", new_cap,
                             (unsigned long)plen);
                }
                break;  /* 等待更多数据 */
            }

            const uint8_t *payload = buf + MIPLAY_FRAME_HDR_LEN;
            ESP_LOGI(TAG, "RX cmd=0x%04X seq=%u len=%lu outer=0x%02X%s",
                     cmd, seq, (unsigned long)plen, outer_type,
                     (outer_type == 0x14) ? " [SAFETY]" : (outer_type == 0x04) ? " [ALONE]" : "");
            if (plen > 0 && plen <= 128) dump_hex(payload, (int)plen, "payload");

            rx_seq = seq;

            /* ── 统一解密（匹配 Rust read_frame：所有加密帧必须解密以更新 IV）── */
            uint8_t *dec_buf = NULL;
            int dec_len = 0;
            bool was_encrypted = false;
            if (s_has_session_key && plen >= 9 &&
                payload[0] == 0x00 && payload[1] == 0x07 &&
                payload[2] == 0x01 && payload[3] == 0xE0) {
                /* 输出缓冲按密文长度动态分配（9B 头 + AES 块余量）：
                 * 27KB 封面帧解密后 pt ~27KB，固定 1024 会被 safety_decrypt 拒收。 */
                dec_buf = heap_caps_malloc(plen + AES_BLOCK_LEN, MALLOC_CAP_SPIRAM);
                if (dec_buf) {
                    dec_len = safety_decrypt(payload, plen, s_aes_key, s_decrypt_iv,
                                             dec_buf, plen + AES_BLOCK_LEN);
                    if (dec_len >= 0) {
                        was_encrypted = true;
                        payload = dec_buf;
                        plen = (uint32_t)dec_len;
                    } else {
                        ESP_LOGW(TAG, "Unified decrypt failed for cmd=0x%04X", cmd);
                        dec_buf = NULL;
                    }
                }
            }

            /* ── AloneMediaPlayer 命名空间 (outer_type=0x04) ── */
            if (outer_type == 0x04) {
                uint8_t sub_cmd = cmd & 0xFF;
                switch (sub_cmd) {
                case 0x14: /* ALONE_SET_STATE */
                    ESP_LOGI(TAG, "AloneSetState seq=%u", seq);
                    send_encrypted_cmd(client_sock, cmd + 1, seq, NULL, 0);
                    break;
                case 0x16: /* ALONE_GET_STATE */
                    ESP_LOGI(TAG, "AloneGetState seq=%u", seq);
                    /* 回复空闲状态 [0,0,0,0,0] */
                    { uint8_t st[] = {0,0,0,0,0}; send_encrypted_cmd(client_sock, cmd+1, seq, st, sizeof(st)); }
                    break;
                case 0x18: /* ALONE_SET_MEDIA_INFO — 与 0x12 同结构，复用解析 */
                    ESP_LOGI(TAG, "AloneSetMediaInfo seq=%u len=%lu", seq, (unsigned long)plen);
                    if (plen > 0) {
                        char *cover_ref = parse_media_info(payload, plen);
                        if (cover_ref) heap_caps_free(cover_ref);
                    }
                    send_encrypted_cmd(client_sock, cmd + 1, seq, NULL, 0);
                    break;
                default:
                    ESP_LOGW(TAG, "AloneMedia unknown sub=0x%02X seq=%u", sub_cmd, seq);
                    break;
                }
                goto frame_done;
            }

            /* ── 命令分发 ── */
            switch (cmd) {
            case CMD_NATIVE_VERSION: {
                /* 手机版本 → 回复版本 */
                if (plen > 0) {
                    ESP_LOGI(TAG, "Phone version: %.*s", (int)(plen-1), payload);
                }
                /* 回复版本 */
                const char *ver = RECEIVER_VERSION;
                uint8_t ver_payload[16];
                int vlen = snprintf((char *)ver_payload, sizeof(ver_payload), "%s", ver) + 1;
                miplay_send_cmd(client_sock, CMD_NATIVE_VERSION_ACK, seq, ver_payload, vlen);
                ESP_LOGI(TAG, "→ VersionAck: %s", ver);
                state = STATE_VERSION_EXCHANGE;
                break;
            }

            case CMD_SAFETY_ACK: {
                /* AUTH_ACK(0x29): 手机在 VERSION_ACK 后发来的确认 */
                if (plen > 0) {
                    char auth_hex[33] = {0};
                    int copy_len = plen > 32 ? 32 : (int)plen;
                    memcpy(auth_hex, payload, copy_len);
                    ESP_LOGI(TAG, "AUTH_ACK: %s", auth_hex);
                }
                /* 发送 NOTIFY(5,6,7) 能力声明 — 必须在 SafetyInfo 之前 */
                /* NOTIFY 5: mode */
                {
                    uint8_t mode_body[] = {4, 'm', 'o', 'd', 'e', 3, 2};
                    miplay_send_cmd(client_sock, 0x0022, 5, mode_body, sizeof(mode_body));
                }
                /* NOTIFY 6: mediaInfoEx (精确29字节) */
                {
                    uint8_t mi_body[] = {
                        0x0b, 'm', 'e', 'd', 'i', 'a', 'I', 'n', 'f', 'o', 'E', 'x',
                        0x16, 0x00, 0x00, 0x00, 0x0c, 0x06,
                        'm', 'T', 'i', 't', 'l', 'e',
                        0x14, 0x00, 0x00, 0x00, 0x00,
                    };
                    miplay_send_cmd(client_sock, 0x0022, 6, mi_body, sizeof(mi_body));
                }
                /* NOTIFY 7: state */
                {
                    uint8_t state_body[] = {5, 's', 't', 'a', 't', 'e', 3, 0};
                    miplay_send_cmd(client_sock, 0x0022, 7, state_body, sizeof(state_body));
                }
                ESP_LOGI(TAG, "-> NOTIFY capabilities (5,6,7)");
                break;
            }

            case CMD_OPEN_DEVICE: {
                /* OPEN — payload 已由统一解密处理 */
                if (was_encrypted && plen > 0) {
                    /* WFD URL 格式: wfd://host:port?mirrorMode=1
                     * 解密后前16字节可能异常，搜索 "wfd://" 定位 */
                    const char *wfd_start = memmem(payload, plen, "wfd://", 6);
                    if (!wfd_start) {
                        ESP_LOGW(TAG, "OPEN: wfd:// not found in %d bytes", (int)plen);
                        break;
                    }
                    char wfd_url[160] = {0};
                    int url_len = (int)plen - (int)(wfd_start - (const char *)payload);
                    if (url_len > 159) url_len = 159;
                    memcpy(wfd_url, wfd_start, url_len);
                    wfd_url[url_len] = '\0';
                    ESP_LOGI(TAG, "OPEN body: %s", wfd_url);
                    /* 解析 host:port 启动 RTSP 客户端 */
                    char rtsp_host[64] = {0};
                    int rtsp_port = 0;
                    const char *u = strstr(wfd_url, "wfd://");
                    if (u) {
                        u += 6;
                        const char *colon = strchr(u, ':');
                        const char *q = strchr(u, '?');
                        if (colon && (!q || colon < q)) {
                            int hl = colon - u;
                            if (hl > 0 && hl < (int)sizeof(rtsp_host)) {
                                memcpy(rtsp_host, u, hl);
                                rtsp_port = atoi(colon + 1);
                            }
                        } else if (q) {
                            int hl = q - u;
                            if (hl > 0 && hl < (int)sizeof(rtsp_host)) {
                                memcpy(rtsp_host, u, hl);
                                rtsp_port = 8554;  /* WFD 默认端口 */
                            }
                        }
                    }
                    ESP_LOGI(TAG, "RTSP target: %s:%d", rtsp_host, rtsp_port);
                    /* 回复 OPEN_ACK (5 zero bytes) 加密（PC: send_encrypted 无 envelope） */
                    uint8_t ack_body[] = {0, 0, 0, 0, 0};
                    send_encrypted_cmd(client_sock, CMD_OPEN_DEVICE + 1, seq,
                                       ack_body, sizeof(ack_body));
                    ESP_LOGI(TAG, "-> OPEN_ACK");
                    /* 发送初始 NOTIFY mediaInfo (seq=8) — Rust: initial_media_info_notification */
                    {
                        uint8_t mi_body[] = {
                            0x0B, 'm', 'e', 'd', 'i', 'a', 'I', 'n', 'f', 'o', 'E', 'x',
                            0x16, 0x00, 0x00, 0x00, 0x0C, 0x06,
                            'm', 'T', 'i', 't', 'l', 'e',
                            0x14, 0x00, 0x00, 0x00, 0x00,
                        };
                        { uint16_t sq; send_encrypted_cmd_auto_seq(client_sock, CMD_NOTIFY,
                                       mi_body, sizeof(mi_body), &sq);
                          ESP_LOGI(TAG, "-> NOTIFY mediaInfoEx (seq=%u)", sq); }
                    }
                    /* 启动 RTSP 独立任务（PSRAM 静态栈，不依赖内部 SRAM） */
                    if (rtsp_port > 0) {
                        /* 递增 generation → 旧 RTSP/media task 下次读时自退出 */
                        uint32_t gen = ++s_media_generation;
                        ESP_LOGI(TAG, "[RTSP] media_generation -> %lu", (unsigned long)gen);
                        rtsp_task_arg_t *rtsp_arg = malloc(sizeof(rtsp_task_arg_t));
                        if (rtsp_arg) {
                            strncpy(rtsp_arg->host, rtsp_host, sizeof(rtsp_arg->host) - 1);
                            rtsp_arg->host[sizeof(rtsp_arg->host) - 1] = 0;
                            rtsp_arg->port = rtsp_port;
                            rtsp_arg->client_sock = client_sock;
                            rtsp_arg->generation = gen;
                            /* 等旧 RTSP task 退出后再创建 */
                            if (s_rtsp_task) {
                                for (int wt = 0; wt < 50 && eTaskGetState(s_rtsp_task) != eDeleted; wt++) {
                                    vTaskDelay(pdMS_TO_TICKS(10));
                                }
                                s_rtsp_task = NULL;
                            }
                            if (miplay_create_task(miplay_rtsp_task_wrapper, "miplay_rtsp",
                                                   RTSP_TASK_STACK_SIZE, rtsp_arg, 4,
                                                   &s_rtsp_task, 1) == pdPASS) {
                                ESP_LOGI(TAG, "[RTSP] Task launched (PSRAM), ctrl loop continues");
                                break;
                            } else {
                                ESP_LOGW(TAG, "[RTSP] Task creation failed");
                                free(rtsp_arg);
                            }
                        } else {
                            ESP_LOGW(TAG, "[RTSP] arg/stack alloc failed");
                            free(rtsp_arg);
                        }
                        /* fallback: inline 运行（仅在任务创建失败时） */
                        ESP_LOGW(TAG, "[RTSP] Inline fallback");
                        miplay_rtsp_run(rtsp_host, rtsp_port, client_sock, gen);
                        /* RTSP 结束后直接退出 handler（socket 已断开） */
                        free(buf);
                        close(client_sock);
                        ESP_LOGI(TAG, "[RTSP] inline done, exiting handler");
                        return;
                    }
                } else {
                    ESP_LOGI(TAG, "OpenDevice (pre-auth)");
                }
                break;
            }

            case CMD_HEARTBEAT: {
                /* 心跳 → 回复 ACK（安全通道后加密） */
                if (s_has_session_key) {
                    send_encrypted_cmd(client_sock, CMD_HEARTBEAT_ACK, seq, NULL, 0);
                } else {
                    miplay_send_cmd(client_sock, CMD_HEARTBEAT_ACK, seq, NULL, 0);
                }
                break;
            }

            default: {
                /* 处理 logical frame (0x14) 内的逻辑命令 */
                /* logical frame: cmd=0x14, payload 是内部 logical 帧
                 * 内部格式: tagLen(1) + tag("cmd"/"ack") + type(1) + innerLen(4 BE) + innerPayload
                 * 但手机也可能直接发0x1400/0x1402等 SafetyAuth 命令
                 */

                /* ── SafetyInfo (0x1400) ──
                 * PC 原型: 回 SafetyInfoAck(0x1401, envelope 明文) 后立即发
                 * 我方 SafetyAuth challenge(0x1402, envelope "cmd" 加密) */
                if (cmd == CMD_SAFETY_INFO && outer_type == 0x14) {
                    ESP_LOGI(TAG, "SafetyInfo offer received");
                    const char *selection_json = "{\n\t\"aesIvType\": \"2\",\n\t\"aesKeyType\": \"1\",\n\t\"authAlgorithmType\": \"4\",\n\t\"authKeyType\": \"1\",\n\t\"integrityType\": \"1\",\n\t\"result\": \"0\" \n} \n";
                    if (send_plain_envelope(client_sock, CMD_SAFETY_INFO_ACK, seq,
                                            (const uint8_t *)selection_json,
                                            strlen(selection_json)) > 0) {
                        ESP_LOGI(TAG, "→ SafetyInfoAck");
                    }
                    /* 发送我们自己的 challenge（authMsg 已在连接开始时生成） */
                    if (!auth_challenge_sent) {
                        char challenge_json[128];
                        int cj_len = snprintf(challenge_json, sizeof(challenge_json),
                                              "{\n\t\"authMsg\": \"%s\" \n} \n", s_auth_msg);
                        if (send_encrypted_envelope(client_sock, CMD_SAFETY_AUTH, 0,
                                                    false, (const uint8_t *)challenge_json,
                                                    cj_len) > 0) {
                            auth_challenge_sent = true;
                            ESP_LOGI(TAG, "→ SafetyAuth challenge sent");
                        }
                    }
                    state = STATE_SAFETY_INFO_EXCHANGED;
                    break;
                }

                /* ── SafetyInfoAck (0x1401) ── */
                if (cmd == CMD_SAFETY_INFO_ACK && outer_type == 0x14) {
                    ESP_LOGI(TAG, "SafetyInfoAck received (phone confirmed selection)");
                    state = STATE_SAFETY_INFO_EXCHANGED;
                    break;
                }

                /* ── SafetyAuth (0x1402, envelope 加密) ──
                 * 使用统一解密结果 dec_buf（避免重复解密导致 IV 不同步） */
                if (cmd == CMD_SAFETY_AUTH && outer_type == 0x14) {
                    ESP_LOGI(TAG, "SafetyAuth challenge from phone");
                    if (!dec_buf || dec_len < 0) {
                        ESP_LOGW(TAG, "SafetyAuth: no decrypted data");
                        break;
                    }
                    uint8_t *plaintext = dec_buf;
                    int pt_len = dec_len;
                    ESP_LOGI(TAG, "SA pt_len=%d bytes[0..3]=%02x %02x %02x %02x",
                             pt_len, plaintext[0], plaintext[1], plaintext[2], plaintext[3]);

                    /* 提取 authMsg — 始终搜索完整 plaintext（第一块可能解密异常，
                     * 但 authMsg 在偏移 16+ 处总能找到） */
                    char peer_auth_msg[33] = {0};
                    const char *am = memmem(plaintext, pt_len, "\"authMsg\"", 9);
                    if (am) {
                        am += 9;
                        while (*am == ':' || *am == ' ' || *am == '\t') am++;
                        if (*am == '"') {
                            am++;
                            const char *end = strchr(am, '"');
                            if (end && end - am <= 32) {
                                memcpy(peer_auth_msg, am, end - am);
                            }
                        }
                    } else {
                        ESP_LOGW(TAG, "SA: authMsg not found in %d bytes", pt_len);
                    }
                    ESP_LOGI(TAG, "Phone authMsg: %s", peer_auth_msg);

                    /* HMAC-SHA256(authKey_full_32B, peer_auth_msg) → 待发 ack */
                    uint8_t ack_hash[32];
                    hmac_sha256((uint8_t *)s_auth_key, 32, (uint8_t *)peer_auth_msg,
                                strlen(peer_auth_msg), ack_hash);
                    hex_to_lower(ack_hash, 32, pending_ack_hex);
                    pending_ack_seq = seq;
                    pending_ack_valid = true;
                    state = STATE_MUTUAL_AUTH;
                    break;
                }

                /* ── SafetyAuthAck (0x1403, envelope 加密) ──
                 * 使用统一解密结果 dec_buf */
                if (cmd == CMD_SAFETY_AUTH_ACK && outer_type == 0x14) {
                    ESP_LOGI(TAG, "SafetyAuthAck from phone (verifying...)");
                    if (!dec_buf || dec_len < 0) break;
                    uint8_t *plaintext = dec_buf;
                    int pt_len = dec_len;
                    /* 解析 envelope: tagLen(1)+tag(3)+valueType(4LE)+dataLen(1)+json */
                    uint8_t tag_len = plaintext[0];
                    if (tag_len != 3 || pt_len < 1 + tag_len + 4 + 1) { break; }
                    uint32_t json_len = plaintext[5 + tag_len];
                    if (json_len > (uint32_t)(pt_len - 6 - tag_len)) { break; }
                    const uint8_t *json = plaintext + 6 + tag_len;
                    char peer_ack[65] = {0};
                    const char *am2 = strstr((const char *)json, "\"authMsgAck\"");
                    if (am2) {
                        am2 += 13; /* skip "authMsgAck" with closing quote */
                        while (*am2 == ':' || *am2 == ' ' || *am2 == '\t') am2++;
                        if (*am2 == '"') {
                            am2++;
                            const char *end = strchr(am2, '"');
                            if (end && end - am2 <= 64) memcpy(peer_ack, am2, end - am2);
                        }
                    } else {
                        ESP_LOGW(TAG, "SAck: authMsgAck not found");
                    }
                    /* plaintext = dec_buf, 不要 free */

                    /* 验证 HMAC-SHA256(authKey_full_32B, s_auth_msg) == peer_ack */
                    ESP_LOGI(TAG, "SAck peer_ack=%s", peer_ack);
                    ESP_LOGI(TAG, "SAck s_auth_msg=%s", (char *)s_auth_msg);
                    uint8_t expected_hash[32];
                    hmac_sha256((uint8_t *)s_auth_key, 32, s_auth_msg, strlen((char *)s_auth_msg), expected_hash);
                    char expected_hex[65];
                    hex_to_lower(expected_hash, 32, expected_hex);
                    ESP_LOGI(TAG, "SAck expected=%s", expected_hex);
                    if (strcmp(peer_ack, expected_hex) != 0) {
                        ESP_LOGE(TAG, "SafetyAuthAck HMAC mismatch, disconnecting");
                        close(client_sock);
                        goto frame_done;  /* socket 已关闭，下次 recv 失败退出循环 */
                    }
                    ESP_LOGI(TAG, "Mutual SafetyAuth complete!");
                    /* 补发对端 authMsg 的 ack */
                    ESP_LOGI(TAG, "pending_ack_valid=%d", (int)pending_ack_valid);
                    if (pending_ack_valid) {
                        char ack_json[128];
                        int aj_len = snprintf(ack_json, sizeof(ack_json),
                                              "{\n\t\"authMsgAck\": \"%s\",\n\t\"result\": \"0\" \n} \n",
                                              pending_ack_hex);
                        ESP_LOGI(TAG, "Sending deferred SafetyAuthAck seq=%u json=%s", pending_ack_seq, ack_json);
                        int sr = send_encrypted_envelope(client_sock, CMD_SAFETY_AUTH_ACK,
                                                    pending_ack_seq, true,
                                                    (const uint8_t *)ack_json, aj_len);
                        ESP_LOGI(TAG, "send_encrypted_envelope returned %d", sr);
                        pending_ack_valid = false;
                    }
                    state = STATE_ESTABLISHED;
                    s_connected = true;
                    /* 对齐参考 esp_miplay-main (L4476 "SafetyAuth"): SafetyAuth 互验
                     * 成功后主动发一条加密空 mediaInfoEx NOTIFY，让 HyperOS 安装
                     * reverse-control / skip-metadata。此后手机推送的 SetMediaInfo 才会
                     * 携带 mCoverUrl/mArt。当前只在 GetMediaInfo 应答(3669)发一次，
                     * 手机若不主动查询则封面引导缺失 → 封面永久为空。 */
                    miplay_notify_empty_media_info_ex(client_sock, "SafetyAuth");
                    /* 通知上层：MiPlay 已连接，DLNA 应暂停 */
                    if (s_connected_cb) s_connected_cb(true);
                    break;
                }

                /* ── 设备信息请求 (0x001E) ── */
                if (cmd == CMD_GET_DEVICE_INFO) {
                    ESP_LOGI(TAG, "GetDeviceInfo received");
                    uint8_t *devinfo = MIPLAY_MALLOC(1024);
                    if (devinfo) {
                        int di_len = build_device_info_payload(devinfo, 1024);
                        if (di_len > 0) {
                            if (s_has_session_key) {
                                send_encrypted_cmd(client_sock, CMD_GET_DEVICE_INFO_ACK, seq,
                                                   devinfo, di_len);
                            } else {
                                miplay_send_cmd(client_sock, CMD_GET_DEVICE_INFO_ACK, seq,
                                                devinfo, di_len);
                            }
                            ESP_LOGI(TAG, "-> DeviceInfoAck (%d bytes)", di_len);
                        }
                        MIPLAY_FREE(devinfo);
                    }
                    break;
                }

                /* ── SetLocalDeviceInfo (0x0058) ── */
                if (cmd == CMD_SET_LOCAL_DEV_INFO) {
                    ESP_LOGI(TAG, "SetLocalDeviceInfo seq=%u len=%lu", seq, (unsigned long)plen);
                    if (s_has_session_key) {
                        send_encrypted_cmd(client_sock, CMD_SET_LOCAL_DEV_ACK, seq, NULL, 0);
                    } else {
                        miplay_send_cmd(client_sock, CMD_SET_LOCAL_DEV_ACK, seq, NULL, 0);
                    }
                    break;
                }

                /* ── GetMirrorMode (0x0034) ── */
                if (cmd == CMD_GET_MIRROR_MODE) {
                    ESP_LOGI(TAG, "GetMirrorMode seq=%u", seq);
                    /* FusionPlay: mode=1 = mobile-audio-streaming（HyperOS 发 SET_MEDIA_INFO + 装反向控制）
                     * mode=2 = 有音频但无移动流（HyperOS 不发元数据、不装控制） */
                    uint8_t mode_resp[] = {0x00, 0x00, 0x00, 0x00, 0x01};
                    if (s_has_session_key) {
                        send_encrypted_cmd(client_sock, CMD_GET_MIRROR_MODE_ACK, seq,
                                           mode_resp, sizeof(mode_resp));
                    } else {
                        miplay_send_cmd(client_sock, CMD_GET_MIRROR_MODE_ACK, seq,
                                        mode_resp, sizeof(mode_resp));
                    }
                    break;
                }

                /* ── GetVolume (0x000E) ── */
                if (cmd == CMD_GET_VOLUME) {
                    ESP_LOGI(TAG, "GetVolume seq=%u (current=%u%%)", seq, (unsigned)s_volume_percent);
                    uint8_t vol_body[5] = {0};
                    vol_body[1] = (uint8_t)((s_volume_percent >> 24) & 0xFF);
                    vol_body[2] = (uint8_t)((s_volume_percent >> 16) & 0xFF);
                    vol_body[3] = (uint8_t)((s_volume_percent >> 8) & 0xFF);
                    vol_body[4] = (uint8_t)(s_volume_percent & 0xFF);
                    send_encrypted_cmd(client_sock, CMD_GET_VOLUME + 1, seq,
                                       vol_body, sizeof(vol_body));
                    break;
                }

                /* ── GetState (0x001C) ── */
                if (cmd == CMD_GET_STATE) {
                    ESP_LOGI(TAG, "GetState seq=%u", seq);
                    /* PC: bytes([0]) + u32 BE(0) = [0,0,0,0,0] */
                    uint8_t state_body[] = {0x00, 0x00, 0x00, 0x00, 0x00};
                    send_encrypted_cmd(client_sock, CMD_GET_STATE + 1, seq,
                                       state_body, sizeof(state_body));
                    break;
                }

                /* ── GetMediaInfo (0x0014, outer=0x00) ──
                 * PC: 回 NOTIFY(0x22) + mediaInfoEx 二进制 TLV
                 * 格式: 0x0B + "mediaInfoEx" + 0x16 + len(4B BE) + subfields...
                 * 子字段: keyLen(1B) + key + 0x14 + valLen(1B) + value */
                if (cmd == CMD_GET_MEDIA_INFO && outer_type == 0x00) {
                    ESP_LOGI(TAG, "GetMediaInfo seq=%u (title=%.20s)", seq, s_media_title);
                    /* 与 FusionPlay 对齐：始终发 empty_media_info_notification (29字节)
                     * 手机消费此 NOTIFY 时安装 reverse-control callback */
                    miplay_notify_empty_media_info_ex(client_sock, "GetMediaInfo");
                    break;
                }

                /* ── SetPlaySource (0x0040) ── */
                if (cmd == CMD_SET_PLAY_SOURCE) {
                    ESP_LOGI(TAG, "SetPlaySource seq=%u", seq);
                    /* ACK */
                    send_encrypted_cmd(client_sock, CMD_SET_PLAY_SOURCE + 1, seq,
                                       NULL, 0);
                    break;
                }

                /* ── SetVolume (0x000C) — 解析音量值 + ACK 回传实际百分比 ── */
                if (cmd == CMD_SET_VOLUME) {
                    if (plen >= 4) {
                        uint32_t vol = ((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
                                       ((uint32_t)payload[2] << 8) | payload[3];
                        if (vol <= 100) {
                            s_volume_percent = vol;
                            ESP_LOGI(TAG, "Volume set to %u%%", (unsigned)vol);
                            if (s_vol_changed_cb) s_vol_changed_cb(vol);
                        }
                    }
                    uint8_t ack[5] = {0};
                    ack[1] = (uint8_t)((s_volume_percent >> 24) & 0xFF);
                    ack[2] = (uint8_t)((s_volume_percent >> 16) & 0xFF);
                    ack[3] = (uint8_t)((s_volume_percent >> 8) & 0xFF);
                    ack[4] = (uint8_t)(s_volume_percent & 0xFF);
                    send_encrypted_cmd(client_sock, CMD_SET_VOLUME + 1, seq, ack, sizeof(ack));
                    /* sender-volume 通知（二进制 TLV，12 字节）*/
                    uint8_t vol_notify[] = {
                        0x06,                                       /* key_len = 6 */
                        'v', 'o', 'l', 'u', 'm', 'e',             /* key = "volume" */
                        0x07,                                       /* value_type = 7 (u32 BE) */
                        0x00, 0x00, 0x00, (uint8_t)s_volume_percent /* percent BE */
                    };
                    send_encrypted_cmd_auto_seq(client_sock, CMD_NOTIFY,
                                       vol_notify, sizeof(vol_notify), NULL);
                    break;
                }

                /* ── Pause (0x0004) / Resume (0x0006) / SetPosition (0x0056) ── */
                if (cmd == CMD_PAUSE || cmd == CMD_RESUME || cmd == CMD_SET_POSITION) {
                    /* 对齐参考 4650-4677: PAUSE/RESUME 触发 PLAYER_STATE 事件,
                     * SET_POSITION 解析前 8 字节大端毫秒位置(opack_u64_be 语义)。 */
                    if (cmd == CMD_PAUSE) {
                        ESP_LOGI(TAG, "Media control cmd=0x0004 (pause) seq=%u", seq);
                        if (s_play_state_cb) s_play_state_cb(true);
                    } else if (cmd == CMD_RESUME) {
                        ESP_LOGI(TAG, "Media control cmd=0x0006 (resume) seq=%u", seq);
                        if (s_play_state_cb) s_play_state_cb(false);
                    } else {
                        int64_t pos_ms = 0;
                        if (plen >= 8) {
                            const uint8_t *p8 = payload;
                            for (int i = 0; i < 8; i++)
                                pos_ms = (pos_ms << 8) | (int64_t)p8[i];
                        }
                        ESP_LOGI(TAG, "Media control cmd=0x0056 (position) seq=%u pos=%lldms",
                                 seq, (long long)pos_ms);
                        if (s_position_cb) s_position_cb(pos_ms);
                    }
                    send_encrypted_cmd(client_sock, cmd + 1, seq, NULL, 0);
                    break;
                }

                /* ── SetMirrorKey (0x006C) ── */
                if (cmd == 0x006C) {
                    ESP_LOGI(TAG, "SetMirrorKey seq=%u payload=%.*s", seq, (int)plen, payload);
                    /* 提取 streamKey, streamIV, authKey */
                    const char *sk = strstr((const char *)payload, "\"streamKey\"");
                    const char *siv = strstr((const char *)payload, "\"streamIV\"");
                    const char *ak = strstr((const char *)payload, "\"authKey\"");
                    if (sk && siv) {
                        sk = strchr(sk, ':');
                        siv = strchr(siv, ':');
                        if (sk && siv) {
                            sk++; siv++;
                            while (*sk == ' ' || *sk == '"') sk++;
                            while (*siv == ' ' || *siv == '"') siv++;
                            memcpy(s_stream_key, sk, 16);
                            memcpy(s_stream_iv, siv, 16);
                            s_has_stream_key = true;
                            ESP_LOGI(TAG, "SetMirrorKey: streamKey=%.16s streamIV=%.16s",
                                     s_stream_key, s_stream_iv);
                        }
                    }
                    /* 提取 authKey（用于 RTSP OPTIONS 认证） */
                    if (ak) {
                        ak = strchr(ak, ':');
                        if (ak) {
                            ak++;
                            while (*ak == ' ' || *ak == '"') ak++;
                            memcpy(s_mirror_auth_key, ak, 16);
                            s_mirror_auth_key[16] = '\0';
                            s_has_mirror_auth_key = true;
                            ESP_LOGI(TAG, "SetMirrorKey: authKey=%.16s", s_mirror_auth_key);
                        }
                    }
                    if (!sk || !siv) {
                        ESP_LOGW(TAG, "SetMirrorKey: streamKey/streamIV not found in JSON");
                    }
                    uint8_t mk_body[] = {0x00};
                    send_encrypted_cmd(client_sock, 0x006D, seq, mk_body, sizeof(mk_body));
                    break;
                }

                /* ── Heartbeat (0x001A) ── */
                if (cmd == CMD_HEARTBEAT) {
                    send_encrypted_cmd(client_sock, CMD_HEARTBEAT_ACK, seq, NULL, 0);
                    ESP_LOGI(TAG, "HB→ACK seq=%u", seq);
                    break;
                }

                /* ── 入站 NOTIFY (0x0022) ──
                 * 对齐参考: mirror mode 2 下手机把媒体信息/播放状态作为 0x0022
                 * 通知异步下发（而非 SET_MEDIA_INFO）。缺此分支则标题和封面全丢。 */
                if (cmd == CMD_NOTIFY && plen > 0) {
                    ESP_LOGI(TAG, "[NOTIFY-in] seq=%u len=%lu payload=%.120s",
                             seq, (unsigned long)plen, payload);
                    /* 入站 NOTIFY 可能是 OPack 二进制 TLV 或 JSON。
                     * JSON 路径可直接解析；OPack 首字节是 key 长度（小值），
                     * 不是 '{'，parse_media_info 会自然失败并告警。 */
                    if (payload[0] == '{' || payload[0] == '[') {
                        char *cover_ref = parse_media_info(payload, plen);
                        if (cover_ref) heap_caps_free(cover_ref);
                    } else {
                        /* 诊断: dump 首 16 字节确认是否 OPack TLV(mediaInfoEx 二进制块)。
                         * OPack mediaInfoEx = 0x0B 'mediaInfoEx' 0x16 len(4B) {...}。
                         * 若手机经此通道发 base64 封面，首字节=len(小值)且后跟 ASCII label。 */
                        ESP_LOGW(TAG, "[NOTIFY-in] non-JSON payload first=%02X %02X %02X %02X %02X "
                                 "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X (opack parser not ported)",
                                 plen > 0 ? payload[0] : 0, plen > 1 ? payload[1] : 0,
                                 plen > 2 ? payload[2] : 0, plen > 3 ? payload[3] : 0,
                                 plen > 4 ? payload[4] : 0, plen > 5 ? payload[5] : 0,
                                 plen > 6 ? payload[6] : 0, plen > 7 ? payload[7] : 0,
                                 plen > 8 ? payload[8] : 0, plen > 9 ? payload[9] : 0,
                                 plen > 10 ? payload[10] : 0, plen > 11 ? payload[11] : 0,
                                 plen > 12 ? payload[12] : 0, plen > 13 ? payload[13] : 0,
                                 plen > 14 ? payload[14] : 0, plen > 15 ? payload[15] : 0);
                    }
                    /* 对齐参考: NOTIFY 是事件上报，不回复同序号 ACK。 */
                    break;
                }

                /* ── GetMediaInfo 应答 (0x0015) ──
                 * 对齐参考: 我方查询媒体信息后的应答，携带完整元数据。 */
                if (cmd == CMD_GET_MEDIA_INFO_ACK && plen > 0) {
                    ESP_LOGI(TAG, "[GET_MEDIA_INFO_ACK] seq=%u len=%lu payload=%.120s",
                             seq, (unsigned long)plen, payload);
                    if (payload[0] == '{' || payload[0] == '[') {
                        char *cover_ref = parse_media_info(payload, plen);
                        if (cover_ref) heap_caps_free(cover_ref);
                    } else {
                        /* 诊断: 参考在 0x0015 优先用 OPack 解析 mediaInfoEx(可能含 base64 cover)。
                         * 本地只认 JSON。dump 首 16 字节确认是否二进制块。 */
                        ESP_LOGW(TAG, "[GET_MEDIA_INFO_ACK] non-JSON first=%02X %02X %02X %02X %02X "
                                 "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X (opack not ported)",
                                 plen > 0 ? payload[0] : 0, plen > 1 ? payload[1] : 0,
                                 plen > 2 ? payload[2] : 0, plen > 3 ? payload[3] : 0,
                                 plen > 4 ? payload[4] : 0, plen > 5 ? payload[5] : 0,
                                 plen > 6 ? payload[6] : 0, plen > 7 ? payload[7] : 0,
                                 plen > 8 ? payload[8] : 0, plen > 9 ? payload[9] : 0,
                                 plen > 10 ? payload[10] : 0, plen > 11 ? payload[11] : 0,
                                 plen > 12 ? payload[12] : 0, plen > 13 ? payload[13] : 0,
                                 plen > 14 ? payload[14] : 0, plen > 15 ? payload[15] : 0);
                    }
                    break;
                }

                /* ── Media control commands during streaming ── */
                if (cmd == CMD_SET_MEDIA_INFO) {
                    ESP_LOGI(TAG, "[MEDIA-INFO] SetMediaInfo seq=%u payload=%.*s",
                             seq, (int)(plen > 200 ? 200 : plen), payload);
                    char *cover_ref = NULL;
                    if (plen > 0) {
                        cover_ref = parse_media_info(payload, plen);
                    }
                    if (cover_ref) heap_caps_free(cover_ref);
                    send_encrypted_cmd(client_sock, CMD_SET_MEDIA_INFO_ACK, seq, NULL, 0);
                    break;
                }
                /* ── SetPlaySource (0x0040) ──
                 * 对齐参考: 除了 ACK 还要解析 payload，它可能携带媒体信息。 */
                if (cmd == CMD_SET_PLAY_SOURCE) {
                    ESP_LOGI(TAG, "[SET_PLAY_SOURCE] seq=%u len=%lu payload=%.120s",
                             seq, (unsigned long)plen, payload);
                    if (plen > 0) {
                        char *cover_ref = parse_media_info(payload, plen);
                        if (cover_ref) heap_caps_free(cover_ref);
                    }
                    send_encrypted_cmd(client_sock, cmd + 1, seq, NULL, 0);
                    break;
                }
                if (cmd == CMD_SET_MEDIA_STATE || cmd == CMD_SET_POSITION ||
                    cmd == CMD_PAUSE || cmd == CMD_RESUME || cmd == CMD_SET_VOLUME) {
                    send_encrypted_cmd(client_sock, cmd + 1, seq, NULL, 0);
                    ESP_LOGI(TAG, "[MEDIA-ctrl] cmd=0x%04X → ACK", cmd);
                    break;
                }

                ESP_LOGW(TAG, "Unhandled cmd=0x%04X seq=%u len=%lu outer=0x%02X",
                         cmd, seq, (unsigned long)plen, outer_type);
                /* 尝试提取安全帧内命令（0x14XX → 内层0x00XX）*/
                if ((cmd & 0xFF00) == 0x1400 && dec_buf && dec_len > 0) {
                    uint16_t inner_cmd = cmd & 0x00FF;
                    ESP_LOGW(TAG, "Safety frame inner_cmd=0x%04X, trying dispatch", inner_cmd);
                    if (inner_cmd == CMD_SET_MEDIA_INFO) {
                        ESP_LOGI(TAG, "[MEDIA-INFO via safety] seq=%u payload=%.*s",
                                 seq, (int)(dec_len > 200 ? 200 : dec_len), dec_buf);
                        char *cover_ref = parse_media_info(dec_buf, dec_len);
                        if (cover_ref) heap_caps_free(cover_ref);
                        send_encrypted_cmd(client_sock, CMD_SET_MEDIA_INFO_ACK, seq, NULL, 0);
                    }
                }
                break;
            }
            } /* switch */
            frame_done:

            /* 移除已处理帧 */
            if ((int)total < buf_used)
                memmove(buf, buf + total, buf_used - total);
            heap_caps_free(dec_buf);
            dec_buf = NULL;
            buf_used -= total;
        } /* while buf_used >= 9 */
    } /* while s_running */

    free(buf);
    disconnect_cleanup(client_sock);
    ESP_LOGI(TAG, "=== Client handler done ===");
}

/* ── TCP 监听任务 ── */
static void miplay_tcp_task(void *arg)
{
    (void)arg;
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(MIPLAY_CONTROL_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_listen_sock < 0) {
        ESP_LOGE(TAG, "Socket failed: %d", errno);
        s_tcp_task = NULL; vTaskDeleteWithCaps(NULL); return;
    }
    int opt = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(s_listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed: %d", errno);
        close(s_listen_sock); s_listen_sock = -1;
        s_tcp_task = NULL; vTaskDeleteWithCaps(NULL); return;
    }
    if (listen(s_listen_sock, 5) < 0) {
        ESP_LOGE(TAG, "Listen failed: %d", errno);
        close(s_listen_sock); s_listen_sock = -1;
        s_tcp_task = NULL; vTaskDeleteWithCaps(NULL); return;
    }
    ESP_LOGI(TAG, "TCP %d listening for MiPlay", MIPLAY_CONTROL_PORT);
    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(s_listen_sock, (struct sockaddr *)&client_addr, &addr_len);
        if (client_sock < 0) { if (s_running) { ESP_LOGW(TAG, "Accept failed: %d", errno); vTaskDelay(pdMS_TO_TICKS(250)); } continue; }
        /* TCP_NODELAY: 立即发送小包，匹配 FusionPlay 行为 */
        int nodelay = 1;
        setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        handle_client(client_sock, &client_addr);
    }
    close(s_listen_sock); s_listen_sock = -1;
    s_tcp_task = NULL; vTaskDeleteWithCaps(NULL);
}

/* ── mDNS 扫描任务 ── */
static TaskHandle_t s_scan_task = NULL;
static void miplay_scan_task(void *arg)
{
    (void)arg;
    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (!s_running) break;
        mdns_result_t *results = NULL;
        if (mdns_query_ptr("_lyra-mdns", "_udp", 2000, 10, &results) != ESP_OK || !results)
            continue;
        mdns_result_t *r = results;
        while (r) {
            if (r->addr && r->txt_count > 0) {
                const char *appdata = NULL;
                for (int i = 0; i < r->txt_count; i++)
                    if (strcmp(r->txt[i].key, "AppData") == 0) { appdata = r->txt[i].value; break; }
                if (appdata && strlen(appdata) > 4) {
                    const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    int fb = 0;
                    for (int i = 0; i < 64; i++) { if (b64[i]==appdata[0]) { fb=i<<2; break; } }
                    for (int i = 0; i < 64; i++) { if (b64[i]==appdata[1]) { fb|=(i>>4)&3; break; } }
                    if (fb == 0x02) {
                        mdns_ip_addr_t *a = r->addr;
                        while (a) {
                            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                                ESP_LOGI(TAG, "Found phone: %s at %u.%u.%u.%u:%u",
                                         r->instance_name ? r->instance_name : "?",
                                         (unsigned)(a->addr.u_addr.ip4.addr&0xFF),
                                         (unsigned)((a->addr.u_addr.ip4.addr>>8)&0xFF),
                                         (unsigned)((a->addr.u_addr.ip4.addr>>16)&0xFF),
                                         (unsigned)((a->addr.u_addr.ip4.addr>>24)&0xFF),
                                         r->port);
                                break;
                            }
                            a = a->next;
                        }
                    }
                }
            }
            r = r->next;
        }
        mdns_query_results_free(results);
    }
    s_scan_task = NULL; vTaskDeleteWithCaps(NULL);
}

/* ── mDNS unsolicited announcement（startup_burst + periodic cache_refresh）
 *    FusionPlay 1.1.6: 注册服务后主动广播 PTR+SRV+TXT+A，cold cache 手机可立即发现。
 *    之后每 30 秒刷新一次，防止手机侧 mDNS 缓存过期。 ── */

#define MDNS_ANNOUNCE_INTERVAL_SEC  30
#define MDNS_MULTICAST_ADDR         "224.0.0.251"
#define MDNS_PORT                   5353

/* 构建单个 mDNS 服务的 unsolicited 响应包（ANCOUNT=1 PTR + ARCOUNT=3 SRV+TXT+A）*/
static size_t build_mdns_announce(uint8_t *p, size_t p_size,
                                   const uint8_t *svc_qname, size_t svc_qname_len,
                                   const char *instance, uint16_t port,
                                   const mdns_txt_item_t *txt, size_t txt_count,
                                   uint32_t ip)
{
    size_t o = 0;
    /* Header */
    dns_push_u16(p, &o, 0x0000);
    dns_push_u16(p, &o, 0x8400);
    dns_push_u16(p, &o, 0);    /* QDCOUNT */
    dns_push_u16(p, &o, 1);    /* ANCOUNT */
    dns_push_u16(p, &o, 0);
    dns_push_u16(p, &o, 3);    /* ARCOUNT */
    /* PTR answer */
    uint16_t svc_off = (uint16_t)o;
    memcpy(p + o, svc_qname, svc_qname_len);
    o += svc_qname_len;
    dns_push_u16(p, &o, 12);   /* PTR */
    dns_push_u16(p, &o, 0x8001);
    dns_push_u32(p, &o, 120);  /* TTL */
    size_t inst_len = strlen(instance);
    dns_push_u16(p, &o, (uint16_t)(inst_len + 3));
    uint16_t inst_off = (uint16_t)o;
    dns_push_label(p, &o, instance);
    dns_push_ptr(p, &o, svc_off);
    /* SRV */
    dns_push_ptr(p, &o, inst_off);
    dns_push_u16(p, &o, 33);
    dns_push_u16(p, &o, 0x8001);
    dns_push_u32(p, &o, 120);
    size_t srv_len_pos = o;
    dns_push_u16(p, &o, 0);
    size_t srv_start = o;
    dns_push_u16(p, &o, 0);
    dns_push_u16(p, &o, 0);
    dns_push_u16(p, &o, port);
    uint16_t host_off = (uint16_t)o;
    dns_push_label(p, &o, s_device_id);
    p[o++] = 0x05; memcpy(p + o, "local", 5); o += 5;
    p[o++] = 0x00;
    uint16_t srv_len = (uint16_t)(o - srv_start);
    p[srv_len_pos] = (uint8_t)(srv_len >> 8);
    p[srv_len_pos + 1] = (uint8_t)(srv_len & 0xFF);
    /* TXT — 每条 key=value 作为独立的 length-prefixed string */
    dns_push_ptr(p, &o, inst_off);
    dns_push_u16(p, &o, 16);
    dns_push_u16(p, &o, 0x8001);
    dns_push_u32(p, &o, 120);
    size_t txt_rdlen_pos = o;
    dns_push_u16(p, &o, 0);
    size_t txt_rdlen_start = o;
    for (size_t i = 0; i < txt_count; i++) {
        size_t kl = strlen(txt[i].key);
        const char *val = txt[i].value ? txt[i].value : "";
        size_t vl = strlen(val);
        uint8_t slen = (uint8_t)(kl + 1 + vl);
        if (o + 1 + slen > p_size - 64) break;
        p[o++] = slen;
        memcpy(p + o, txt[i].key, kl); o += kl;
        p[o++] = '=';
        memcpy(p + o, val, vl); o += vl;
    }
    uint16_t txt_rdlen = (uint16_t)(o - txt_rdlen_start);
    p[txt_rdlen_pos] = (uint8_t)(txt_rdlen >> 8);
    p[txt_rdlen_pos + 1] = (uint8_t)(txt_rdlen & 0xFF);
    /* A */
    dns_push_ptr(p, &o, host_off);
    dns_push_u16(p, &o, 1);
    dns_push_u16(p, &o, 0x8001);
    dns_push_u32(p, &o, 120);
    dns_push_u16(p, &o, 4);
    p[o++] = (uint8_t)(ip & 0xFF);
    p[o++] = (uint8_t)((ip >> 8) & 0xFF);
    p[o++] = (uint8_t)((ip >> 16) & 0xFF);
    p[o++] = (uint8_t)((ip >> 24) & 0xFF);
    return o;
}

static TaskHandle_t s_announce_task = NULL;

static void mdns_announce_task(void *arg)
{
    (void)arg;
    /* 延迟 2 秒等待 mDNS 组件完全就绪 */
    vTaskDelay(pdMS_TO_TICKS(2000));

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) { ESP_LOGE(TAG, "announce socket failed: %d", errno); s_announce_task = NULL; vTaskDeleteWithCaps(NULL); return; }
    int ttl = 255;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(MDNS_PORT),
        .sin_addr.s_addr = inet_addr(MDNS_MULTICAST_ADDR),
    };

    uint32_t ip = get_my_ipv4();
    uint8_t pkt[768];

    /* TXT for _mi-connect */
    mdns_txt_item_t micon_txt[] = {
        {"version", MIPLAY_VERSION}, {"apps","[5]"}, {"flags","CgE="},
        {"name","ESP32-MIPLAY"}, {"idHash",s_idhash}, {"dev",MIPLAY_DEV},
        {"sec",MIPLAY_SEC}, {"appsData",s_appsdata}, {"mac",s_mac_b64},
    };
    /* TXT for _lyra-mdns */
    struct timeval tv_now; gettimeofday(&tv_now, NULL);
    long long ts_ms = (long long)tv_now.tv_sec * 1000LL + tv_now.tv_usec / 1000;
    char ts_str[24]; snprintf(ts_str, sizeof(ts_str), "%lld", ts_ms);
    char debug_info[128];
    {
        uint8_t a[4] = { ip&0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF };
        char e1[8]={0}, e2[8]={0};
        char t[4]; int oi;
        snprintf(t, sizeof(t), "%u", a[1]); oi=0;
        for (int c=0; t[c]; c++) e1[oi++] = (t[c]>='0'&&t[c]<='9') ? '#'+(t[c]-'0') : t[c];
        snprintf(t, sizeof(t), "%u", a[2]); oi=0;
        for (int c=0; t[c]; c++) e2[oi++] = (t[c]>='0'&&t[c]<='9') ? '#'+(t[c]-'0') : t[c];
        snprintf(debug_info, sizeof(debug_info), "{msg:announcement, ifname:STA, v4:%u.%s.%s.%u}",
                 (unsigned)a[0], e1, e2, (unsigned)a[3]);
    }
    mdns_txt_item_t lyra_txt[] = {
        {"AppData",s_lyra_appdata},{"MediumType","8192"},{"CH","0"},
        {"DebugInfo",debug_info},{"TS",ts_str},
    };

    /* service QNAME 字节 */
    static const uint8_t lyra_qname[] = { 0x0a,'_','l','y','r','a','-','m','d','n','s', 0x04,'_','u','d','p', 0x05,'l','o','c','a','l', 0x00 };
    static const uint8_t micon_qname[] = { 0x0b,'_','m','i','-','c','o','n','n','e','c','t', 0x04,'_','u','d','p', 0x05,'l','o','c','a','l', 0x00 };

    /* startup_burst: 3 轮，180ms 间隔 */
    for (int round = 1; round <= 3 && s_running; round++) {
        size_t len1 = build_mdns_announce(pkt, sizeof(pkt), lyra_qname, sizeof(lyra_qname),
                                           s_device_id, 5353, lyra_txt, 5, ip);
        sendto(sock, pkt, len1, 0, (struct sockaddr *)&dest, sizeof(dest));
        size_t len2 = build_mdns_announce(pkt, sizeof(pkt), micon_qname, sizeof(micon_qname),
                                           s_inst_name, MIPLAY_COAP_PORT, micon_txt, 9, ip);
        sendto(sock, pkt, len2, 0, (struct sockaddr *)&dest, sizeof(dest));
        ESP_LOGI(TAG, "mDNS announce burst %d/3 (lyra=%uB micon=%uB)", round, (unsigned)len1, (unsigned)len2);
        if (round < 3) vTaskDelay(pdMS_TO_TICKS(180));
    }

    /* periodic cache_refresh */
    while (s_running) {
        for (int i = 0; i < MDNS_ANNOUNCE_INTERVAL_SEC * 10 && s_running; i++)
            vTaskDelay(pdMS_TO_TICKS(100));
        if (!s_running) break;

        /* 更新时间戳 */
        gettimeofday(&tv_now, NULL);
        ts_ms = (long long)tv_now.tv_sec * 1000LL + tv_now.tv_usec / 1000;
        snprintf(ts_str, sizeof(ts_str), "%lld", ts_ms);
        lyra_txt[4].value = ts_str;

        size_t len1 = build_mdns_announce(pkt, sizeof(pkt), lyra_qname, sizeof(lyra_qname),
                                           s_device_id, 5353, lyra_txt, 5, ip);
        sendto(sock, pkt, len1, 0, (struct sockaddr *)&dest, sizeof(dest));
        size_t len2 = build_mdns_announce(pkt, sizeof(pkt), micon_qname, sizeof(micon_qname),
                                           s_inst_name, MIPLAY_COAP_PORT, micon_txt, 9, ip);
        sendto(sock, pkt, len2, 0, (struct sockaddr *)&dest, sizeof(dest));
        ESP_LOGD(TAG, "mDNS announce refresh (lyra=%uB micon=%uB)", (unsigned)len1, (unsigned)len2);
    }
    close(sock);
    s_announce_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

/* ── 连接状态回调 ── */
void miplay_set_connected_cb(miplay_connected_cb_t cb)
{
    s_connected_cb = cb;
}

uint32_t miplay_get_volume(void)
{
    return s_volume_percent;
}

void miplay_send_receiver_control(const char *action, int64_t value)
{
    int sock = s_active_client_sock;
    ESP_LOGI(TAG, "[CTRL] send_receiver_control(%s) sock=%d has_key=%d", action, sock, (int)s_has_session_key);
    if (sock < 0 || !s_has_session_key) {
        ESP_LOGW(TAG, "[CTRL] BLOCKED: sock=%d has_key=%d", sock, (int)s_has_session_key);
        return;
    }

    /* 对齐 FusionPlay receiver_control_boolean 格式：
     * [key_len(1B)] [key_bytes] [0x00] [0x01]
     * key: "key-pause" / "key-resume" / "key-prev" / "key-next" */
    uint8_t body[20];
    int o = 0;
    char key[16];
    int klen = snprintf(key, sizeof(key), "key-%s", action);
    if (klen <= 0 || klen > 15) return;
    body[o++] = (uint8_t)klen;
    memcpy(body + o, key, klen); o += klen;
    body[o++] = 0x00;  /* boolean type */
    body[o++] = 0x01;  /* true */

    { uint16_t sq; send_encrypted_cmd_auto_seq(sock, CMD_NOTIFY, body, o, &sq);
      ESP_LOGI(TAG, "[CTRL] -> NOTIFY %s (seq=%u len=%d)", key, sq, o); }
}

/* ── 公共 API ── */
esp_err_t miplay_init(void)
{
    if (s_running) return ESP_OK;
    ESP_LOGI(TAG, "Initializing MiPlay (MiPlayForWindows compatible)");
    if (!s_send_mux) s_send_mux = xSemaphoreCreateRecursiveMutex();
    if (!s_send_mux) { ESP_LOGE(TAG, "s_send_mux create failed"); return ESP_ERR_NO_MEM; }
    init_device_identity();

    esp_err_t err = register_mdns_services();
    if (err != ESP_OK) return err;

    s_running = true;
    build_miplay_lan_response();
    /* 对齐参考: TCP 16KB, scan/lan/ann 12KB, 全部 PSRAM 栈 */
    if (miplay_create_task(miplay_tcp_task, "miplay_tcp", 16 * 1024,
                           NULL, 5, &s_tcp_task, 1) != pdPASS) {
        ESP_LOGE(TAG, "TCP task failed"); s_running = false; return ESP_FAIL;
    }
    miplay_create_task(miplay_scan_task, "miplay_scan", 12 * 1024, NULL, 3, &s_scan_task, 1);
    miplay_create_task(miplay_lan_task,  "miplay_lan",  12 * 1024, NULL, 3, &s_lan_task, 0);
    miplay_create_task(mdns_announce_task, "mdns_ann", 12 * 1024, NULL, 3, &s_announce_task, 0);
    ESP_LOGI(TAG, "MiPlay initialized (TCP+LAN+Scan)");
    ESP_LOGI(TAG, "TCP 8899 listening for MiPlay");
    return ESP_OK;
}

void miplay_stop(void)
{
    if (!s_running) return;
    s_running = false;
    if (s_lan_sock >= 0) { close(s_lan_sock); s_lan_sock = -1; }
    if (s_listen_sock >= 0) { close(s_listen_sock); s_listen_sock = -1; }
    for (int i = 0; i < 50 && (s_tcp_task != NULL || s_lan_task != NULL); i++) vTaskDelay(pdMS_TO_TICKS(100));
    mdns_service_remove(MIPLAY_MICON_SERVICE, MIPLAY_MICON_PROTO);
    mdns_service_remove(MIPLAY_LYRA_SERVICE, MIPLAY_LYRA_PROTO);
    if (s_send_mux) { vSemaphoreDelete(s_send_mux); s_send_mux = NULL; }
    rtsp_read_msg_reset();  /* 释放持久缓冲区 */
    ESP_LOGI(TAG, "MiPlay stopped");
}
