/**
 * @file http_server.c
 * @brief Smart Door Lock HTTP Server Implementation
 *
 * RESTful API:
 *   GET  /              - Web UI
 *   GET  /api/status    - System status (JSON)
 *   GET  /api/users     - List users (JSON)
 *   POST /api/users     - Add user (JSON body: {name, role})
 *   DELETE /api/users/{id} - Delete user
 *   POST /api/door/unlock  - Remote unlock
 *   POST /api/door/lock    - Remote lock
 *   GET  /api/logs      - Access logs (JSON)
 *   POST /api/factory-reset - Factory reset
 */

#include "http_server.h"
#include "user_management.h"
#include "door_lock.h"
#include "wifi_manager.h"
#include "face_recognition.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "HTTP_SERVER";
static httpd_handle_t s_server = NULL;
extern const uint8_t http_server_index_html_start[] asm("_binary_index_html_start");
extern const uint8_t http_server_index_html_end[] asm("_binary_index_html_end");
__attribute__((weak)) void fingerprint_empty_library_via_http(void);

/* ============================================================
 * Helper: send JSON response
 * ============================================================ */

static esp_err_t send_json(httpd_req_t *req, int status_code,
                            cJSON *json)
{
    char *json_str = cJSON_PrintUnformatted(json);
    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (status_code != 200) {
        char status_str[8];
        snprintf(status_str, sizeof(status_str), "%d", status_code);
        httpd_resp_set_status(req, status_str);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);
    free(json_str);
    return ret;
}

static esp_err_t send_error(httpd_req_t *req, int code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", false);
    cJSON_AddStringToObject(root, "error", msg);
    esp_err_t ret = send_json(req, code, root);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t send_ok(httpd_req_t *req, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    if (msg) cJSON_AddStringToObject(root, "message", msg);
    esp_err_t ret = send_json(req, 200, root);
    cJSON_Delete(root);
    return ret;
}

/* Read request body */
static int read_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    int total = req->content_len;
    if (total <= 0 || total >= (int)buf_size) return -1;

    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, buf + received, total - received);
        if (ret <= 0) return -1;
        received += ret;
    }
    buf[received] = '\0';
    return received;
}

/* ============================================================
 * Web UI handler
 * ============================================================ */

static esp_err_t handler_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, (const char *)http_server_index_html_start,
                           http_server_index_html_end - http_server_index_html_start);
}

/* ============================================================
 * API: GET /api/status
 * ============================================================ */

static esp_err_t handler_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    /* Door state */
    door_state_t door_state = door_lock_get_state();
    cJSON_AddBoolToObject(root, "door_locked",
                          door_state == DOOR_STATE_LOCKED);
    cJSON_AddStringToObject(root, "door_state",
                            door_state == DOOR_STATE_LOCKED   ? "locked" :
                            door_state == DOOR_STATE_UNLOCKED ? "unlocked" : "unknown");

    /* User count */
    cJSON_AddNumberToObject(root, "user_count", user_mgmt_get_user_count());

    /* Security state */
    sdl_security_state_t sec;
    user_mgmt_get_security_state(&sec);
    cJSON_AddBoolToObject(root, "locked_out", sec.is_locked_out);
    cJSON_AddNumberToObject(root, "failed_attempts", sec.failed_attempts);
    cJSON_AddNumberToObject(root, "total_accesses", sec.total_accesses);
    cJSON_AddNumberToObject(root, "total_denials", sec.total_denials);

    /* WiFi */
    wifi_mgr_status_t wifi_status;
    wifi_mgr_get_status(&wifi_status);
    cJSON_AddBoolToObject(root, "wifi_connected",
                          wifi_status.state == WIFI_STATE_CONNECTED ||
                          wifi_status.state == WIFI_STATE_AP_STARTED);
    cJSON_AddStringToObject(root, "ip_addr", wifi_status.ip_addr);

    /* Face recognition */
    cJSON_AddBoolToObject(root, "camera_ready", face_recog_is_camera_ready());
    cJSON_AddNumberToObject(root, "enrolled_faces",
                            face_recog_get_enrolled_count());

    /* Uptime */
    cJSON_AddNumberToObject(root, "uptime_s",
                            (int)(esp_timer_get_time() / 1000000));

    esp_err_t ret = send_json(req, 200, root);
    cJSON_Delete(root);
    return ret;
}

/* ============================================================
 * API: GET /api/users
 * ============================================================ */

static esp_err_t handler_get_users(httpd_req_t *req)
{
    sdl_user_t users[SDL_USER_MAX_COUNT];
    uint8_t count = 0;
    user_mgmt_get_all_users(users, &count, SDL_USER_MAX_COUNT);

    cJSON *root  = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();

    for (int i = 0; i < count; i++) {
        cJSON *u = cJSON_CreateObject();
        cJSON_AddNumberToObject(u, "id",           users[i].id);
        cJSON_AddStringToObject(u, "name",         users[i].name);
        cJSON_AddNumberToObject(u, "role",         users[i].role);
        cJSON_AddNumberToObject(u, "status",       users[i].status);
        cJSON_AddNumberToObject(u, "auth_methods", users[i].auth_methods);
        cJSON_AddBoolToObject(u,   "has_face",     users[i].has_face);
        cJSON_AddNumberToObject(u, "fingerprint_id", users[i].fingerprint_id);
        cJSON_AddNumberToObject(u, "access_count", users[i].access_count);
        cJSON_AddNumberToObject(u, "last_access",  (double)users[i].last_access);
        cJSON_AddNumberToObject(u, "created_at",   (double)users[i].created_at);
        cJSON_AddItemToArray(array, u);
    }

    cJSON_AddItemToObject(root, "users", array);
    cJSON_AddNumberToObject(root, "count", count);

    esp_err_t ret = send_json(req, 200, root);
    cJSON_Delete(root);
    return ret;
}

/* ============================================================
 * API: POST /api/users
 * ============================================================ */

static esp_err_t handler_add_user(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_error(req, 400, "Invalid request body");
    }

    cJSON *json = cJSON_Parse(body);
    if (json == NULL) {
        return send_error(req, 400, "Invalid JSON");
    }

    cJSON *name_j = cJSON_GetObjectItem(json, "name");
    cJSON *role_j = cJSON_GetObjectItem(json, "role");

    if (!cJSON_IsString(name_j) || strlen(name_j->valuestring) == 0) {
        cJSON_Delete(json);
        return send_error(req, 400, "Missing or invalid 'name' field");
    }

    sdl_user_t new_user = {0};
    strncpy(new_user.name, name_j->valuestring, SDL_USER_NAME_MAX_LEN - 1);
    new_user.role   = cJSON_IsNumber(role_j) ? (sdl_user_role_t)role_j->valueint
                                              : SDL_USER_ROLE_USER;
    new_user.status = SDL_USER_STATUS_ACTIVE;

    cJSON_Delete(json);

    esp_err_t ret = user_mgmt_add_user(&new_user);
    if (ret != ESP_OK) {
        return send_error(req, 500,
                          ret == ESP_ERR_NO_MEM ? "User limit reached" :
                          "Failed to add user");
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", true);
    cJSON_AddStringToObject(resp, "message", "User added");
    cJSON_AddNumberToObject(resp, "user_id", new_user.id);
    esp_err_t r = send_json(req, 200, resp);
    cJSON_Delete(resp);
    return r;
}

/* ============================================================
 * API: DELETE /api/users/{id}
 * ============================================================ */

static esp_err_t handler_delete_user(httpd_req_t *req)
{
    /* Extract user ID from URI: /api/users/{id} */
    const char *uri = req->uri;
    const char *prefix = "/api/users/";
    if (strncmp(uri, prefix, strlen(prefix)) != 0) {
        return send_error(req, 400, "Invalid URI");
    }

    int user_id = atoi(uri + strlen(prefix));
    if (user_id <= 0) {
        return send_error(req, 400, "Invalid user ID");
    }

    esp_err_t ret = user_mgmt_delete_user((uint8_t)user_id);
    if (ret == ESP_ERR_NOT_FOUND) {
        return send_error(req, 404, "User not found");
    }
    if (ret != ESP_OK) {
        return send_error(req, 500, "Failed to delete user");
    }

    /* Also delete face data */
    face_recog_delete_user((uint8_t)user_id);

    return send_ok(req, "User deleted");
}

/* ============================================================
 * API: POST /api/door/unlock
 * ============================================================ */

static esp_err_t handler_door_unlock(httpd_req_t *req)
{
    if (user_mgmt_is_locked_out()) {
        return send_error(req, 403, "System is locked out");
    }

    esp_err_t ret = door_lock_unlock(DOOR_UNLOCK_SOURCE_REMOTE, 0);
    if (ret != ESP_OK) {
        return send_error(req, 500, "Failed to unlock door");
    }

    user_mgmt_record_access(0, SDL_AUTH_METHOD_FACE, true, 100);
    return send_ok(req, "Door unlocked remotely");
}

/* ============================================================
 * API: POST /api/door/lock
 * ============================================================ */

static esp_err_t handler_door_lock(httpd_req_t *req)
{
    esp_err_t ret = door_lock_lock();
    if (ret != ESP_OK) {
        return send_error(req, 500, "Failed to lock door");
    }
    return send_ok(req, "Door locked");
}

/* ============================================================
 * API: GET /api/logs
 * ============================================================ */

static esp_err_t handler_get_logs(httpd_req_t *req)
{
    sdl_access_log_t logs[SDL_USER_ACCESS_LOG_MAX];
    uint8_t count = 0;
    user_mgmt_get_access_log(logs, &count, SDL_USER_ACCESS_LOG_MAX);

    cJSON *root  = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();

    for (int i = 0; i < count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "user_id",     logs[i].user_id);
        cJSON_AddNumberToObject(entry, "auth_method", logs[i].auth_method);
        cJSON_AddBoolToObject(entry,   "success",     logs[i].success);
        cJSON_AddNumberToObject(entry, "timestamp",   (double)logs[i].timestamp);
        cJSON_AddNumberToObject(entry, "confidence",  logs[i].confidence);
        cJSON_AddItemToArray(array, entry);
    }

    cJSON_AddItemToObject(root, "logs", array);
    cJSON_AddNumberToObject(root, "count", count);

    esp_err_t ret = send_json(req, 200, root);
    cJSON_Delete(root);
    return ret;
}

/* ============================================================
 * API: POST /api/factory-reset
 * ============================================================ */

static esp_err_t handler_factory_reset(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Factory reset requested via HTTP");

    user_mgmt_factory_reset();
    fingerprint_empty_library_via_http();  /* Handled in app_main */

    return send_ok(req, "Factory reset complete. Restart device.");
}

/* Stub - actual fingerprint clear is done in app_main via event */
__attribute__((weak)) void fingerprint_empty_library_via_http(void) {}

/* ============================================================
 * OPTIONS handler for CORS preflight
 * ============================================================ */

static esp_err_t handler_options(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods",
                       "GET, POST, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers",
                       "Content-Type");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* ============================================================
 * URI handler table
 * ============================================================ */

static const httpd_uri_t uri_handlers[] = {
    { .uri = "/",                  .method = HTTP_GET,    .handler = handler_index        },
    { .uri = "/api/status",        .method = HTTP_GET,    .handler = handler_status       },
    { .uri = "/api/users",         .method = HTTP_GET,    .handler = handler_get_users    },
    { .uri = "/api/users",         .method = HTTP_POST,   .handler = handler_add_user     },
    { .uri = "/api/users/*",       .method = HTTP_DELETE, .handler = handler_delete_user  },
    { .uri = "/api/door/unlock",   .method = HTTP_POST,   .handler = handler_door_unlock  },
    { .uri = "/api/door/lock",     .method = HTTP_POST,   .handler = handler_door_lock    },
    { .uri = "/api/logs",          .method = HTTP_GET,    .handler = handler_get_logs     },
    { .uri = "/api/factory-reset", .method = HTTP_POST,   .handler = handler_factory_reset},
    { .uri = "/*",                 .method = HTTP_OPTIONS,.handler = handler_options       },
};

/* ============================================================
 * Public API
 * ============================================================ */

esp_err_t http_server_start(const http_server_config_t *config)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "HTTP server already running");
        return ESP_OK;
    }

    httpd_config_t httpd_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_cfg.server_port      = config ? config->port : 80;
    httpd_cfg.max_open_sockets = config ? config->max_clients : 4;
    httpd_cfg.uri_match_fn     = httpd_uri_match_wildcard;
    httpd_cfg.stack_size       = 8192;
    httpd_cfg.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", httpd_cfg.server_port);

    esp_err_t ret = httpd_start(&s_server, &httpd_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register URI handlers */
    for (int i = 0; i < sizeof(uri_handlers) / sizeof(uri_handlers[0]); i++) {
        ret = httpd_register_uri_handler(s_server, &uri_handlers[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register handler for %s",
                     uri_handlers[i].uri);
        }
    }

    ESP_LOGI(TAG, "HTTP server started successfully");
    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (s_server == NULL) return ESP_OK;

    esp_err_t ret = httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "HTTP server stopped");
    return ret;
}

bool http_server_is_running(void)
{
    return (s_server != NULL);
}
