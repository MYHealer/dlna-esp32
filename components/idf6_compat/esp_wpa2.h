/* esp_wpa2.h — IDF 6.x 兼容包装
 * IDF 6.x 将 esp_wpa2.h 合并到了 esp_eap_client.h
 * 旧 API 的兼容实现在 esp_wpa2_api_port.c 中，但缺少头文件声明 */
#pragma once
#include "esp_eap_client.h"

/* IDF 6.x 自带 esp_wpa2_api_port.c 提供这些兼容函数，但没有声明 */
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_wifi_sta_wpa2_ent_enable(void);
esp_err_t esp_wifi_sta_wpa2_ent_disable(void);
esp_err_t esp_wifi_sta_wpa2_ent_set_identity(const unsigned char *identity, int len);
esp_err_t esp_wifi_sta_wpa2_ent_set_username(const unsigned char *username, int len);
esp_err_t esp_wifi_sta_wpa2_ent_set_password(const unsigned char *password, int len);
esp_err_t esp_wifi_sta_wpa2_ent_set_new_password(const unsigned char *new_password, int len);
esp_err_t esp_wifi_sta_wpa2_ent_set_ca_cert(const unsigned char *ca_cert, int ca_cert_len);
esp_err_t esp_wifi_sta_wpa2_ent_set_cert_key(const unsigned char *client_cert, int client_cert_len,
                                              const unsigned char *private_key, int private_key_len,
                                              const unsigned char *private_key_passwd, int private_key_passwd_len);
esp_err_t esp_wifi_sta_wpa2_ent_set_disable_time_check(bool disable);
esp_err_t esp_wifi_sta_wpa2_ent_get_disable_time_check(bool *disable);
esp_err_t esp_wifi_sta_wpa2_ent_set_ttls_phase2_method(esp_eap_ttls_phase2_types type);
esp_err_t esp_wifi_sta_wpa2_set_suiteb_192bit_certification(bool enable);
esp_err_t esp_wifi_sta_wpa2_ent_set_pac_file(const unsigned char *pac_file, int pac_file_len);
esp_err_t esp_wifi_sta_wpa2_ent_set_fast_phase1_params(esp_eap_fast_config config);
esp_err_t esp_wifi_sta_wpa2_use_default_cert_bundle(bool use_default_bundle);

#ifdef __cplusplus
}
#endif
