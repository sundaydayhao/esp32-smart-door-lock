#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Smart Door Lock HTTP Server
 * RESTful API for remote management via WiFi
 * ============================================================ */

/* HTTP server configuration */
typedef struct {
    uint16_t port;          /* HTTP server port (default 80) */
    uint8_t  max_clients;   /* Maximum concurrent clients */
} http_server_config_t;

/**
 * @brief Initialize and start HTTP server
 * @param config Server configuration
 * @return ESP_OK on success
 */
esp_err_t http_server_start(const http_server_config_t *config);

/**
 * @brief Stop HTTP server
 * @return ESP_OK on success
 */
esp_err_t http_server_stop(void);

/**
 * @brief Check if HTTP server is running
 * @return true if running
 */
bool http_server_is_running(void);

#ifdef __cplusplus
}
#endif