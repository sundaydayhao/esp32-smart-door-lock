#pragma once

#include "esp_err.h"
#include "esp_netif.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Smart Door Lock WiFi Manager
 * Supports both AP mode and STA mode
 * ============================================================ */

#define WIFI_SSID_MAX_LEN       32
#define WIFI_PASS_MAX_LEN       64
#define WIFI_IP_MAX_LEN         16

/* WiFi operating mode */
typedef enum {
    WIFI_MGR_MODE_AP = 0,   /* Access Point mode */
    WIFI_MGR_MODE_STA,      /* Station mode (connect to router) */
    WIFI_MGR_MODE_APSTA,    /* Both AP and STA */
} wifi_mgr_mode_t;

/* WiFi connection state */
typedef enum {
    WIFI_STATE_DISCONNECTED = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_AP_STARTED,
    WIFI_STATE_ERROR,
} wifi_mgr_state_t;

/* WiFi manager configuration */
typedef struct {
    wifi_mgr_mode_t mode;
    char            ssid[WIFI_SSID_MAX_LEN];
    char            password[WIFI_PASS_MAX_LEN];
    uint8_t         max_retry;
    uint8_t         ap_max_conn;    /* Max AP connections (AP mode) */
    uint8_t         ap_channel;     /* AP channel (AP mode) */
} wifi_mgr_config_t;

/* WiFi status info */
typedef struct {
    wifi_mgr_state_t state;
    char             ip_addr[WIFI_IP_MAX_LEN];
    char             ssid[WIFI_SSID_MAX_LEN];
    int8_t           rssi;
    bool             has_ip;
} wifi_mgr_status_t;

/* WiFi event callback */
typedef void (*wifi_mgr_event_cb_t)(wifi_mgr_state_t state,
                                     const char *ip_addr);

/**
 * @brief Initialize WiFi manager
 * @param config WiFi configuration
 * @return ESP_OK on success
 */
esp_err_t wifi_mgr_init(const wifi_mgr_config_t *config);

/**
 * @brief Deinitialize WiFi manager
 * @return ESP_OK on success
 */
esp_err_t wifi_mgr_deinit(void);

/**
 * @brief Start WiFi (connect or start AP)
 * @return ESP_OK on success
 */
esp_err_t wifi_mgr_start(void);

/**
 * @brief Stop WiFi
 * @return ESP_OK on success
 */
esp_err_t wifi_mgr_stop(void);

/**
 * @brief Get current WiFi status
 * @param status Output status structure
 * @return ESP_OK on success
 */
esp_err_t wifi_mgr_get_status(wifi_mgr_status_t *status);

/**
 * @brief Check if WiFi is connected (STA mode) or AP is running
 * @return true if connected/running
 */
bool wifi_mgr_is_connected(void);

/**
 * @brief Get IP address string
 * @param buf Output buffer
 * @param buf_size Buffer size
 * @return ESP_OK on success
 */
esp_err_t wifi_mgr_get_ip(char *buf, size_t buf_size);

/**
 * @brief Register event callback
 * @param callback Callback function
 */
void wifi_mgr_register_callback(wifi_mgr_event_cb_t callback);

/**
 * @brief Update WiFi credentials (STA mode)
 * @param ssid New SSID
 * @param password New password
 * @return ESP_OK on success
 */
esp_err_t wifi_mgr_update_credentials(const char *ssid, const char *password);

#ifdef __cplusplus
}
#endif
