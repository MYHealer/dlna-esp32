#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Transport state strings (returned by get_transport_state) */
#define DLNA_STATE_STOPPED        "STOPPED"
#define DLNA_STATE_PLAYING        "PLAYING"
#define DLNA_STATE_PAUSED         "PAUSED_PLAYBACK"
#define DLNA_STATE_TRANSITIONING  "TRANSITIONING"
#define DLNA_STATE_NO_MEDIA       "NO_MEDIA_PRESENT"

/**
 * Callbacks: the application provides these to bridge audio control.
 * Return values:
 *   get_transport_state -> one of DLNA_STATE_* strings
 *   get_uri             -> current track URI string (persistent)
 *   get_position_sec    -> current playback position in seconds
 *   get_duration_sec    -> current track duration in seconds
 *   get_volume          -> current volume 0-100
 *   get_mute            -> 0 = unmuted, 1 = muted
 */
typedef struct {
    /* Required callbacks */
    const char* (*get_transport_state)(void);
    const char* (*get_uri)(void);
    int         (*get_position_sec)(void);
    int         (*get_duration_sec)(void);
    int         (*get_position_ms)(void);   /* optional: ms precision for lyrics sync */
    int         (*get_volume)(void);
    int         (*get_mute)(void);

    /* Action callbacks */
    void (*on_set_uri)(const char *uri);
    void (*on_set_next_uri)(const char *uri, const char *metadata);
    void (*on_set_metadata)(const char *metadata);
    void (*on_play)(void);
    void (*on_pause)(void);
    void (*on_stop)(void);
    void (*on_seek)(int seconds);
    void (*on_set_volume)(int vol);
    void (*on_set_mute)(int mute);
    void (*on_next)(void);
    void (*on_previous)(void);

    /* Configuration */
    const char *friendly_name;  /* e.g. "ESP32-S3 Music Player" */
    const char *uuid;           /* e.g. "8db0797ar-f01a-4949-8f59-51188b18180b" */
    int         port;           /* HTTP server port, e.g. 8080 */
} custom_dlna_config_t;

/**
 * @brief Initialize the custom DLNA protocol stack.
 *        Starts HTTP server + SSDP listener.
 */
esp_err_t custom_dlna_init(const custom_dlna_config_t *config);

/**
 * @brief Notify GENA subscribers of transport state change.
 *        Call this from HTTPD task or other safe contexts.
 *        NOT safe from esp_audio callbacks — use _async version instead.
 */
void custom_dlna_notify_transport_state(void);

/**
 * @brief Async transport state notify — safe from esp_audio callbacks.
 *        Queues notification to gena_task for deferred execution.
 */
void custom_dlna_notify_transport_state_async(void);

/**
 * @brief Notify GENA subscribers of volume/mute change.
 */
void custom_dlna_notify_rcs(void);

/**
 * @brief Update current URI and metadata (call after cb_next/cb_previous).
 */
void custom_dlna_update_uri(const char *uri, const char *metadata);

#ifdef __cplusplus
}
#endif