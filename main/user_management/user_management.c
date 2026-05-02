/**
 * @file user_management.c
 * @brief Smart Door Lock User Management Implementation
 *
 * Manages user enrollment, authentication records, and security state.
 * User data is persisted in NVS (Non-Volatile Storage).
 */

#include "user_management.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "USER_MGMT";

/* NVS namespace */
#define NVS_NAMESPACE           "sdl_users"
#define NVS_KEY_USER_COUNT      "user_count"
#define NVS_KEY_USER_PREFIX     "user_"
#define NVS_KEY_LOG_COUNT       "log_count"
#define NVS_KEY_LOG_PREFIX      "log_"
#define NVS_KEY_SECURITY        "security"

/* Configuration */
#define MAX_USERS               CONFIG_SDL_MAX_USERS
#define MAX_FAILED_ATTEMPTS     CONFIG_SDL_MAX_FAILED_ATTEMPTS
#define LOCKOUT_DURATION_S      CONFIG_SDL_LOCKOUT_DURATION_S

/* Internal state */
static sdl_user_t           s_users[SDL_USER_MAX_COUNT];
static uint8_t              s_user_count = 0;
static sdl_access_log_t     s_access_log[SDL_USER_ACCESS_LOG_MAX];
static uint8_t              s_log_count = 0;
static uint8_t              s_log_head = 0;  /* Circular buffer head */
static sdl_security_state_t s_security_state = {0};
static SemaphoreHandle_t    s_mutex = NULL;
static bool                 s_initialized = false;

/* ============================================================
 * Private helper functions
 * ============================================================ */

static int find_user_index(uint8_t user_id)
{
    for (int i = 0; i < SDL_USER_MAX_COUNT; i++) {
        if (s_users[i].id == user_id && s_users[i].status != SDL_USER_STATUS_DISABLED) {
            return i;
        }
    }
    return -1;
}

static int find_free_slot(void)
{
    for (int i = 0; i < SDL_USER_MAX_COUNT; i++) {
        if (s_users[i].id == 0) {
            return i;
        }
    }
    return -1;
}

static uint8_t generate_user_id(void)
{
    /* Find the next available ID (1-based) */
    for (uint8_t id = 1; id <= SDL_USER_MAX_COUNT; id++) {
        bool found = false;
        for (int i = 0; i < SDL_USER_MAX_COUNT; i++) {
            if (s_users[i].id == id) {
                found = true;
                break;
            }
        }
        if (!found) {
            return id;
        }
    }
    return 0; /* No available ID */
}

static bool uid_matches(const uint8_t *a, uint8_t a_len,
                        const uint8_t *b, uint8_t b_len)
{
    if (a == NULL || b == NULL || a_len == 0 || a_len != b_len) {
        return false;
    }
    return memcmp(a, b, a_len) == 0;
}

/* ============================================================
 * NVS persistence functions
 * ============================================================ */

esp_err_t user_mgmt_save(void)
{
    nvs_handle_t handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Save user count */
    ret = nvs_set_u8(handle, NVS_KEY_USER_COUNT, s_user_count);
    if (ret != ESP_OK) goto cleanup;

    /* Save each user */
    for (int i = 0; i < SDL_USER_MAX_COUNT; i++) {
        if (s_users[i].id != 0) {
            char key[16];
            snprintf(key, sizeof(key), "%s%d", NVS_KEY_USER_PREFIX, i);
            ret = nvs_set_blob(handle, key, &s_users[i], sizeof(sdl_user_t));
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to save user %d: %s", i, esp_err_to_name(ret));
            }
        }
    }

    /* Save security state */
    ret = nvs_set_blob(handle, NVS_KEY_SECURITY, &s_security_state,
                       sizeof(sdl_security_state_t));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save security state: %s", esp_err_to_name(ret));
    }

    /* Save access log */
    ret = nvs_set_u8(handle, NVS_KEY_LOG_COUNT, s_log_count);
    if (ret == ESP_OK) {
        for (int i = 0; i < s_log_count && i < SDL_USER_ACCESS_LOG_MAX; i++) {
            char key[16];
            snprintf(key, sizeof(key), "%s%d", NVS_KEY_LOG_PREFIX, i);
            nvs_set_blob(handle, key, &s_access_log[i], sizeof(sdl_access_log_t));
        }
    }

    ret = nvs_commit(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "User data saved to NVS (%d users)", s_user_count);
    }

cleanup:
    nvs_close(handle);
    return ret;
}

esp_err_t user_mgmt_load(void)
{
    nvs_handle_t handle;
    esp_err_t ret;

    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved user data found, starting fresh");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Load user count */
    uint8_t count = 0;
    ret = nvs_get_u8(handle, NVS_KEY_USER_COUNT, &count);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "No user count found in NVS");
        nvs_close(handle);
        return ESP_OK;
    }

    /* Load each user */
    memset(s_users, 0, sizeof(s_users));
    s_user_count = 0;

    for (int i = 0; i < SDL_USER_MAX_COUNT; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%s%d", NVS_KEY_USER_PREFIX, i);
        size_t size = sizeof(sdl_user_t);
        ret = nvs_get_blob(handle, key, &s_users[i], &size);
        if (ret == ESP_OK && s_users[i].id != 0) {
            s_user_count++;
        }
    }

    /* Load security state */
    size_t sec_size = sizeof(sdl_security_state_t);
    nvs_get_blob(handle, NVS_KEY_SECURITY, &s_security_state, &sec_size);

    /* Load access log */
    uint8_t log_count = 0;
    if (nvs_get_u8(handle, NVS_KEY_LOG_COUNT, &log_count) == ESP_OK) {
        s_log_count = log_count < SDL_USER_ACCESS_LOG_MAX ? log_count : SDL_USER_ACCESS_LOG_MAX;
        for (int i = 0; i < s_log_count; i++) {
            char key[16];
            snprintf(key, sizeof(key), "%s%d", NVS_KEY_LOG_PREFIX, i);
            size_t size = sizeof(sdl_access_log_t);
            nvs_get_blob(handle, key, &s_access_log[i], &size);
        }
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "Loaded %d users from NVS", s_user_count);
    return ESP_OK;
}

/* ============================================================
 * Public API implementation
 * ============================================================ */

esp_err_t user_mgmt_init(void)
{
    ESP_LOGI(TAG, "Initializing user management");

    /* Create mutex */
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs to be erased");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Clear internal state */
    memset(s_users, 0, sizeof(s_users));
    memset(s_access_log, 0, sizeof(s_access_log));
    memset(&s_security_state, 0, sizeof(s_security_state));
    s_user_count = 0;
    s_log_count = 0;
    s_log_head = 0;

    /* Load persisted data */
    ret = user_mgmt_load();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load user data, starting fresh");
    }

    s_initialized = true;
    ESP_LOGI(TAG, "User management initialized (%d users loaded)", s_user_count);
    return ESP_OK;
}

esp_err_t user_mgmt_add_user(sdl_user_t *user)
{
    if (!s_initialized || user == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_user_count >= MAX_USERS) {
        ESP_LOGW(TAG, "Maximum user count (%d) reached", MAX_USERS);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    int slot = find_free_slot();
    if (slot < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Assign user ID */
    user->id = generate_user_id();
    if (user->id == 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NO_MEM;
    }

    /* Set creation timestamp */
    user->created_at = time(NULL);
    user->access_count = 0;
    user->last_access = 0;

    /* Copy user to slot */
    memcpy(&s_users[slot], user, sizeof(sdl_user_t));
    s_user_count++;

    ESP_LOGI(TAG, "Added user '%s' with ID %d (slot %d)",
             user->name, user->id, slot);

    /* Save to NVS */
    user_mgmt_save();

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t user_mgmt_delete_user(uint8_t user_id)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int idx = find_user_index(user_id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Deleting user '%s' (ID %d)",
             s_users[idx].name, user_id);

    /* Clear the slot */
    memset(&s_users[idx], 0, sizeof(sdl_user_t));
    s_user_count--;

    /* Save to NVS */
    user_mgmt_save();

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t user_mgmt_get_user(uint8_t user_id, sdl_user_t *user)
{
    if (!s_initialized || user == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int idx = find_user_index(user_id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(user, &s_users[idx], sizeof(sdl_user_t));
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t user_mgmt_update_user(const sdl_user_t *user)
{
    if (!s_initialized || user == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int idx = find_user_index(user->id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    memcpy(&s_users[idx], user, sizeof(sdl_user_t));
    user_mgmt_save();

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t user_mgmt_get_all_users(sdl_user_t *users, uint8_t *count,
                                   uint8_t max_count)
{
    if (!s_initialized || users == NULL || count == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    *count = 0;
    for (int i = 0; i < SDL_USER_MAX_COUNT && *count < max_count; i++) {
        if (s_users[i].id != 0) {
            memcpy(&users[*count], &s_users[i], sizeof(sdl_user_t));
            (*count)++;
        }
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t user_mgmt_find_by_fingerprint(uint8_t fingerprint_id,
                                         sdl_user_t *user)
{
    if (!s_initialized || user == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (int i = 0; i < SDL_USER_MAX_COUNT; i++) {
        if (s_users[i].id != 0 &&
            s_users[i].fingerprint_id == fingerprint_id &&
            (s_users[i].auth_methods & SDL_AUTH_METHOD_FINGERPRINT)) {
            memcpy(user, &s_users[i], sizeof(sdl_user_t));
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t user_mgmt_bind_nfc_card(uint8_t user_id, const uint8_t *uid,
                                   uint8_t uid_len)
{
    if (!s_initialized || uid == NULL ||
        uid_len == 0 || uid_len > SDL_NFC_UID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int idx = find_user_index(user_id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    for (int i = 0; i < SDL_USER_MAX_COUNT; i++) {
        if (i == idx || s_users[i].id == 0 || !s_users[i].has_nfc_card) {
            continue;
        }
        if (uid_matches(s_users[i].nfc_uid, s_users[i].nfc_uid_len, uid, uid_len)) {
            ESP_LOGW(TAG, "NFC UID already bound to user %d", s_users[i].id);
            xSemaphoreGive(s_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }

    memset(s_users[idx].nfc_uid, 0, sizeof(s_users[idx].nfc_uid));
    memcpy(s_users[idx].nfc_uid, uid, uid_len);
    s_users[idx].nfc_uid_len = uid_len;
    s_users[idx].has_nfc_card = true;
    s_users[idx].auth_methods |= SDL_AUTH_METHOD_NFC;

    ESP_LOGI(TAG, "Bound NFC UID (%d bytes) to user '%s' (ID %d)",
             uid_len, s_users[idx].name, user_id);

    user_mgmt_save();
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t user_mgmt_unbind_nfc_card(uint8_t user_id)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int idx = find_user_index(user_id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    memset(s_users[idx].nfc_uid, 0, sizeof(s_users[idx].nfc_uid));
    s_users[idx].nfc_uid_len = 0;
    s_users[idx].has_nfc_card = false;
    s_users[idx].auth_methods &= ~SDL_AUTH_METHOD_NFC;

    ESP_LOGI(TAG, "Unbound NFC card from user '%s' (ID %d)",
             s_users[idx].name, user_id);

    user_mgmt_save();
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t user_mgmt_find_by_nfc_uid(const uint8_t *uid, uint8_t uid_len,
                                     sdl_user_t *user)
{
    if (!s_initialized || uid == NULL || user == NULL ||
        uid_len == 0 || uid_len > SDL_NFC_UID_MAX_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (int i = 0; i < SDL_USER_MAX_COUNT; i++) {
        if (s_users[i].id != 0 &&
            s_users[i].has_nfc_card &&
            (s_users[i].auth_methods & SDL_AUTH_METHOD_NFC) &&
            uid_matches(s_users[i].nfc_uid, s_users[i].nfc_uid_len, uid, uid_len)) {
            memcpy(user, &s_users[i], sizeof(sdl_user_t));
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t user_mgmt_update_face_features(uint8_t user_id,
                                          const float *features,
                                          size_t feature_size)
{
    if (!s_initialized || features == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int idx = find_user_index(user_id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    size_t copy_size = feature_size < 128 ? feature_size : 128;
    memcpy(s_users[idx].face_features, features, copy_size * sizeof(float));
    s_users[idx].has_face = true;
    s_users[idx].auth_methods |= SDL_AUTH_METHOD_FACE;

    user_mgmt_save();

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t user_mgmt_record_access(uint8_t user_id, uint8_t auth_method,
                                   bool success, uint8_t confidence)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* Add to circular log buffer */
    uint8_t log_idx = s_log_head % SDL_USER_ACCESS_LOG_MAX;
    s_access_log[log_idx].user_id = user_id;
    s_access_log[log_idx].auth_method = auth_method;
    s_access_log[log_idx].success = success;
    s_access_log[log_idx].timestamp = time(NULL);
    s_access_log[log_idx].confidence = confidence;

    s_log_head = (s_log_head + 1) % SDL_USER_ACCESS_LOG_MAX;
    if (s_log_count < SDL_USER_ACCESS_LOG_MAX) {
        s_log_count++;
    }

    /* Update user's last access time */
    if (success && user_id != 0) {
        int idx = find_user_index(user_id);
        if (idx >= 0) {
            s_users[idx].last_access = time(NULL);
            s_users[idx].access_count++;
        }
    }

    /* Update security state */
    s_security_state.total_accesses++;
    if (!success) {
        s_security_state.total_denials++;
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t user_mgmt_get_access_log(sdl_access_log_t *logs, uint8_t *count,
                                    uint8_t max_count)
{
    if (!s_initialized || logs == NULL || count == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    *count = s_log_count < max_count ? s_log_count : max_count;

    /* Return most recent entries first */
    for (int i = 0; i < *count; i++) {
        int src_idx = ((int)s_log_head - 1 - i + SDL_USER_ACCESS_LOG_MAX)
                      % SDL_USER_ACCESS_LOG_MAX;
        memcpy(&logs[i], &s_access_log[src_idx], sizeof(sdl_access_log_t));
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t user_mgmt_get_security_state(sdl_security_state_t *state)
{
    if (!s_initialized || state == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(state, &s_security_state, sizeof(sdl_security_state_t));
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool user_mgmt_is_locked_out(void)
{
    if (!s_initialized) return false;

    if (!s_security_state.is_locked_out) return false;

    /* Check if lockout has expired */
    time_t now = time(NULL);
    if (now >= s_security_state.lockout_until) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_security_state.is_locked_out = false;
        s_security_state.failed_attempts = 0;
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "Lockout expired, system unlocked");
        return false;
    }

    return true;
}

bool user_mgmt_record_failed_attempt(void)
{
    if (!s_initialized) return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_security_state.failed_attempts++;
    ESP_LOGW(TAG, "Failed attempt %d/%d",
             s_security_state.failed_attempts, MAX_FAILED_ATTEMPTS);

    bool lockout_triggered = false;
    if (s_security_state.failed_attempts >= MAX_FAILED_ATTEMPTS) {
        s_security_state.is_locked_out = true;
        s_security_state.lockout_until = time(NULL) + LOCKOUT_DURATION_S;
        lockout_triggered = true;
        ESP_LOGW(TAG, "LOCKOUT TRIGGERED! System locked for %d seconds",
                 LOCKOUT_DURATION_S);
    }

    xSemaphoreGive(s_mutex);
    return lockout_triggered;
}

void user_mgmt_clear_failed_attempts(void)
{
    if (!s_initialized) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_security_state.failed_attempts = 0;
    s_security_state.is_locked_out = false;
    xSemaphoreGive(s_mutex);
}

uint8_t user_mgmt_get_user_count(void)
{
    return s_user_count;
}

esp_err_t user_mgmt_factory_reset(void)
{
    ESP_LOGW(TAG, "Performing factory reset - all user data will be deleted!");

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    memset(s_users, 0, sizeof(s_users));
    memset(s_access_log, 0, sizeof(s_access_log));
    memset(&s_security_state, 0, sizeof(s_security_state));
    s_user_count = 0;
    s_log_count = 0;
    s_log_head = 0;

    xSemaphoreGive(s_mutex);

    /* Erase NVS namespace */
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }

    ESP_LOGI(TAG, "Factory reset complete");
    return ESP_OK;
}
