/**
 * @file wifi_manager.c
 * @brief Smart Door Lock WiFi Manager Implementation
 *
 * Manages WiFi connectivity in both AP and STA modes.
 * Handles reconnection, event callbacks, and status reporting.
 */

#include "wifi_manager.h"
#include "event_manager.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "WIFI_MGR";

/* Event group bits */
#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1

/* Internal state */
static struct {
    bool                 initialized;
    wifi_mgr_config_t    config;
    wifi_mgr_state_t     state;
    wifi_mgr_event_cb_t  callback;
    EventGroupHandle_t   event_group;
    esp_netif_t         *netif_sta;
    esp_netif_t         *netif_ap;
    int                  retry_count;
    char                 ip_addr[WIFI_IP_MAX_LEN];
} s_wifi = {
    .initialized = false,
    .state       = WIFI_STATE_DISCONNECTED,
    .callback    = NULL,
    .event_group = NULL,
    .netif_sta   = NULL,
    .netif_ap    = NULL,
    .retry_count = 0,
};

/* ============================================================
 * Private: Event handlers
 * ============================================================ */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started, connecting...");
                s_wifi.state = WIFI_STATE_CONNECTING;
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *disc =
                    (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "WiFi disconnected (reason: %d)", disc->reason);
                s_wifi.state = WIFI_STATE_DISCONNECTED;
                memset(s_wifi.ip_addr, 0, sizeof(s_wifi.ip_addr));

                if (s_wifi.retry_count < s_wifi.config.max_retry) {
                    s_wifi.retry_count++;
                    ESP_LOGI(TAG, "Retrying WiFi connection (%d/%d)...",
                             s_wifi.retry_count, s_wifi.config.max_retry);
                    esp_wifi_connect();
                } else {
                    ESP_LOGE(TAG, "WiFi connection failed after %d retries",
                             s_wifi.config.max_retry);
                    xEventGroupSetBits(s_wifi.event_group, WIFI_FAIL_BIT);
                    s_wifi.state = WIFI_STATE_ERROR;
                }

                if (s_wifi.callback) {
                    s_wifi.callback(WIFI_STATE_DISCONNECTED, NULL);
                }
                event_manager_post_system(SDL_SYSTEM_EVENT_WIFI_DISCONNECTED, NULL);
                break;
            }

            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "WiFi AP started (SSID: %s)", s_wifi.config.ssid);
                s_wifi.state = WIFI_STATE_AP_STARTED;
                if (s_wifi.callback) {
                    s_wifi.callback(WIFI_STATE_AP_STARTED, "192.168.4.1");
                }
                break;

            case WIFI_EVENT_AP_STOP:
                ESP_LOGI(TAG, "WiFi AP stopped");
                s_wifi.state = WIFI_STATE_DISCONNECTED;
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *conn =
                    (wifi_event_ap_staconnected_t *)event_data;
                ESP_LOGI(TAG, "Client connected to AP: " MACSTR,
                         MAC2STR(conn->mac));
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *disc =
                    (wifi_event_ap_stadisconnected_t *)event_data;
                ESP_LOGI(TAG, "Client disconnected from AP: " MACSTR,
                         MAC2STR(disc->mac));
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            snprintf(s_wifi.ip_addr, sizeof(s_wifi.ip_addr),
                     IPSTR, IP2STR(&event->ip_info.ip));

            ESP_LOGI(TAG, "WiFi connected! IP: %s", s_wifi.ip_addr);
            s_wifi.retry_count = 0;
            s_wifi.state = WIFI_STATE_CONNECTED;

            xEventGroupSetBits(s_wifi.event_group, WIFI_CONNECTED_BIT);

            if (s_wifi.callback) {
                s_wifi.callback(WIFI_STATE_CONNECTED, s_wifi.ip_addr);
            }
            event_manager_post_system(SDL_SYSTEM_EVENT_WIFI_CONNECTED, NULL);
        }
    }
}

/* ============================================================
 * Public API implementation
 * ============================================================ */

esp_err_t wifi_mgr_init(const wifi_mgr_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Initializing WiFi manager (mode=%d, SSID=%s)",
             config->mode, config->ssid);

    memcpy(&s_wifi.config, config, sizeof(wifi_mgr_config_t));

    /* Initialize TCP/IP stack */
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Create event group */
    s_wifi.event_group = xEventGroupCreate();
    if (s_wifi.event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Initialize WiFi with default config */
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&wifi_init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register event handlers */
    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL, NULL);
    if (ret != ESP_OK) return ret;

    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL, NULL);
    if (ret != ESP_OK) return ret;

    /* Create network interfaces based on mode */
    if (config->mode == WIFI_MGR_MODE_STA || config->mode == WIFI_MGR_MODE_APSTA) {
        s_wifi.netif_sta = esp_netif_create_default_wifi_sta();
    }
    if (config->mode == WIFI_MGR_MODE_AP || config->mode == WIFI_MGR_MODE_APSTA) {
        s_wifi.netif_ap = esp_netif_create_default_wifi_ap();
    }

    s_wifi.initialized = true;
    ESP_LOGI(TAG, "WiFi manager initialized");
    return ESP_OK;
}

esp_err_t wifi_mgr_deinit(void)
{
    if (!s_wifi.initialized) return ESP_ERR_INVALID_STATE;

    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_wifi.event_group) {
        vEventGroupDelete(s_wifi.event_group);
        s_wifi.event_group = NULL;
    }

    s_wifi.initialized = false;
    return ESP_OK;
}

esp_err_t wifi_mgr_start(void)
{
    if (!s_wifi.initialized) return ESP_ERR_INVALID_STATE;

    esp_err_t ret;

    if (s_wifi.config.mode == WIFI_MGR_MODE_AP) {
        /* Configure AP mode */
        ret = esp_wifi_set_mode(WIFI_MODE_AP);
        if (ret != ESP_OK) return ret;

        wifi_config_t ap_config = {
            .ap = {
                .max_connection = s_wifi.config.ap_max_conn > 0 ?
                                  s_wifi.config.ap_max_conn : 4,
                .channel        = s_wifi.config.ap_channel > 0 ?
                                  s_wifi.config.ap_channel : 1,
                .authmode       = WIFI_AUTH_WPA2_PSK,
            },
        };
        strncpy((char *)ap_config.ap.ssid, s_wifi.config.ssid,
                sizeof(ap_config.ap.ssid) - 1);
        strncpy((char *)ap_config.ap.password, s_wifi.config.password,
                sizeof(ap_config.ap.password) - 1);
        ap_config.ap.ssid_len = strlen(s_wifi.config.ssid);

        if (strlen(s_wifi.config.password) == 0) {
            ap_config.ap.authmode = WIFI_AUTH_OPEN;
        }

        ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        if (ret != ESP_OK) return ret;

    } else if (s_wifi.config.mode == WIFI_MGR_MODE_STA) {
        /* Configure STA mode */
        ret = esp_wifi_set_mode(WIFI_MODE_STA);
        if (ret != ESP_OK) return ret;

        wifi_config_t sta_config = {
            .sta = {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
                .pmf_cfg = {
                    .capable  = true,
                    .required = false,
                },
            },
        };
        strncpy((char *)sta_config.sta.ssid, s_wifi.config.ssid,
                sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, s_wifi.config.password,
                sizeof(sta_config.sta.password) - 1);

        ret = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        if (ret != ESP_OK) return ret;

    } else {
        /* APSTA mode */
        ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (ret != ESP_OK) return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* For STA mode, wait for connection */
    if (s_wifi.config.mode == WIFI_MGR_MODE_STA) {
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi.event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, pdFALSE,
            pdMS_TO_TICKS(10000) /* 10 second timeout */
        );

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Connected to WiFi SSID: %s", s_wifi.config.ssid);
        } else if (bits & WIFI_FAIL_BIT) {
            ESP_LOGW(TAG, "Failed to connect to WiFi SSID: %s",
                     s_wifi.config.ssid);
        } else {
            ESP_LOGW(TAG, "WiFi connection timeout");
        }
    }

    return ESP_OK;
}

esp_err_t wifi_mgr_stop(void)
{
    if (!s_wifi.initialized) return ESP_ERR_INVALID_STATE;
    return esp_wifi_stop();
}

esp_err_t wifi_mgr_get_status(wifi_mgr_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;

    status->state   = s_wifi.state;
    status->has_ip  = (s_wifi.state == WIFI_STATE_CONNECTED);
    strncpy(status->ip_addr, s_wifi.ip_addr, sizeof(status->ip_addr) - 1);
    strncpy(status->ssid, s_wifi.config.ssid, sizeof(status->ssid) - 1);

    if (s_wifi.state == WIFI_STATE_CONNECTED) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            status->rssi = ap_info.rssi;
        }
    }

    return ESP_OK;
}

bool wifi_mgr_is_connected(void)
{
    return (s_wifi.state == WIFI_STATE_CONNECTED ||
            s_wifi.state == WIFI_STATE_AP_STARTED);
}

esp_err_t wifi_mgr_get_ip(char *buf, size_t buf_size)
{
    if (buf == NULL) return ESP_ERR_INVALID_ARG;

    if (s_wifi.state == WIFI_STATE_CONNECTED) {
        strncpy(buf, s_wifi.ip_addr, buf_size - 1);
        buf[buf_size - 1] = '\0';
        return ESP_OK;
    } else if (s_wifi.state == WIFI_STATE_AP_STARTED) {
        strncpy(buf, "192.168.4.1", buf_size - 1);
        buf[buf_size - 1] = '\0';
        return ESP_OK;
    }

    return ESP_ERR_INVALID_STATE;
}

void wifi_mgr_register_callback(wifi_mgr_event_cb_t callback)
{
    s_wifi.callback = callback;
}

esp_err_t wifi_mgr_update_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) return ESP_ERR_INVALID_ARG;

    strncpy(s_wifi.config.ssid, ssid, WIFI_SSID_MAX_LEN - 1);
    strncpy(s_wifi.config.password, password, WIFI_PASS_MAX_LEN - 1);

    /* Reconnect with new credentials */
    esp_wifi_disconnect();
    s_wifi.retry_count = 0;

    wifi_config_t sta_config = {};
    strncpy((char *)sta_config.sta.ssid, ssid,
            sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password,
            sizeof(sta_config.sta.password) - 1);

    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    esp_wifi_connect();

    ESP_LOGI(TAG, "WiFi credentials updated, reconnecting...");
    return ESP_OK;
}
