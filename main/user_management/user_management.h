#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Smart Door Lock User Management
 * Handles user enrollment, deletion, and authentication records
 * ============================================================ */

#define SDL_USER_MAX_COUNT          10      /* Maximum number of users */
#define SDL_USER_NAME_MAX_LEN       32      /* Maximum user name length */
#define SDL_USER_FACE_FEAT_SIZE     512     /* Face feature vector size (bytes) */
#define SDL_USER_ACCESS_LOG_MAX     50      /* Maximum access log entries */
#define SDL_NFC_UID_MAX_LEN         10      /* Maximum ISO14443A UID length */

/* Authentication method flags */
#define SDL_AUTH_METHOD_FACE        (1 << 0)
#define SDL_AUTH_METHOD_FINGERPRINT (1 << 1)
#define SDL_AUTH_METHOD_PASSWORD    (1 << 2)
#define SDL_AUTH_METHOD_NFC         (1 << 3)
#define SDL_AUTH_METHOD_BOTH        (SDL_AUTH_METHOD_FACE | SDL_AUTH_METHOD_FINGERPRINT)
#define SDL_AUTH_METHOD_ALL         (SDL_AUTH_METHOD_FACE | SDL_AUTH_METHOD_FINGERPRINT | \
                                     SDL_AUTH_METHOD_PASSWORD | SDL_AUTH_METHOD_NFC)

/* User role */
typedef enum {
    SDL_USER_ROLE_ADMIN = 0,    /* Administrator - full access */
    SDL_USER_ROLE_USER,          /* Regular user - door access only */
    SDL_USER_ROLE_GUEST,         /* Guest - limited time access */
} sdl_user_role_t;

/* User status */
typedef enum {
    SDL_USER_STATUS_ACTIVE = 0,  /* User is active */
    SDL_USER_STATUS_DISABLED,    /* User is disabled */
    SDL_USER_STATUS_EXPIRED,     /* User access has expired */
} sdl_user_status_t;

/* User information structure */
typedef struct {
    uint8_t          id;                            /* User ID (1-based, 0=invalid) */
    char             name[SDL_USER_NAME_MAX_LEN];   /* User display name */
    sdl_user_role_t  role;                          /* User role */
    sdl_user_status_t status;                       /* User status */
    uint8_t          auth_methods;                  /* Enabled auth methods bitmask */
    uint8_t          fingerprint_id;                /* Fingerprint template ID in sensor */
    bool             has_face;                      /* Whether face is enrolled */
    float            face_features[128];            /* Face feature vector */
    bool             has_nfc_card;                  /* Whether an NFC card is bound */
    uint8_t          nfc_uid[SDL_NFC_UID_MAX_LEN];   /* NFC card UID */
    uint8_t          nfc_uid_len;                    /* NFC UID length */
    time_t           created_at;                    /* Creation timestamp */
    time_t           last_access;                   /* Last successful access timestamp */
    time_t           access_expire;                 /* Access expiry (0=never) */
    uint32_t         access_count;                  /* Total access count */
} sdl_user_t;

/* Access log entry */
typedef struct {
    uint8_t  user_id;           /* User ID (0=unknown) */
    uint8_t  auth_method;       /* Authentication method used */
    bool     success;           /* Whether access was granted */
    time_t   timestamp;         /* Event timestamp */
    uint8_t  confidence;        /* Recognition confidence (0-100) */
} sdl_access_log_t;

/* System security state */
typedef struct {
    uint8_t  failed_attempts;   /* Consecutive failed attempts */
    bool     is_locked_out;     /* Whether system is in lockout */
    time_t   lockout_until;     /* Lockout expiry timestamp */
    uint32_t total_accesses;    /* Total access attempts */
    uint32_t total_denials;     /* Total access denials */
} sdl_security_state_t;

/**
 * @brief Initialize user management module
 * @return ESP_OK on success
 */
esp_err_t user_mgmt_init(void);

/**
 * @brief Add a new user
 * @param user Pointer to user structure (id will be assigned)
 * @return ESP_OK on success, ESP_ERR_NO_MEM if max users reached
 */
esp_err_t user_mgmt_add_user(sdl_user_t *user);

/**
 * @brief Delete a user by ID
 * @param user_id User ID to delete
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if user not found
 */
esp_err_t user_mgmt_delete_user(uint8_t user_id);

/**
 * @brief Get user by ID
 * @param user_id User ID to find
 * @param user Output user structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t user_mgmt_get_user(uint8_t user_id, sdl_user_t *user);

/**
 * @brief Update user information
 * @param user Updated user structure
 * @return ESP_OK on success
 */
esp_err_t user_mgmt_update_user(const sdl_user_t *user);

/**
 * @brief Get all users
 * @param users Output array of users
 * @param count Output count of users
 * @param max_count Maximum number of users to return
 * @return ESP_OK on success
 */
esp_err_t user_mgmt_get_all_users(sdl_user_t *users, uint8_t *count,
                                   uint8_t max_count);

/**
 * @brief Find user by fingerprint ID
 * @param fingerprint_id Fingerprint template ID
 * @param user Output user structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t user_mgmt_find_by_fingerprint(uint8_t fingerprint_id,
                                         sdl_user_t *user);

/**
 * @brief Bind an NFC card UID to a user
 * @param user_id User ID
 * @param uid NFC UID bytes
 * @param uid_len NFC UID length
 * @return ESP_OK on success
 */
esp_err_t user_mgmt_bind_nfc_card(uint8_t user_id, const uint8_t *uid,
                                   uint8_t uid_len);

/**
 * @brief Remove the NFC card from a user
 * @param user_id User ID
 * @return ESP_OK on success
 */
esp_err_t user_mgmt_unbind_nfc_card(uint8_t user_id);

/**
 * @brief Find user by NFC UID
 * @param uid NFC UID bytes
 * @param uid_len NFC UID length
 * @param user Output user structure
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t user_mgmt_find_by_nfc_uid(const uint8_t *uid, uint8_t uid_len,
                                     sdl_user_t *user);

/**
 * @brief Update user's face features
 * @param user_id User ID
 * @param features Face feature vector
 * @param feature_size Size of feature vector
 * @return ESP_OK on success
 */
esp_err_t user_mgmt_update_face_features(uint8_t user_id,
                                          const float *features,
                                          size_t feature_size);

/**
 * @brief Record an access attempt
 * @param user_id User ID (0 for unknown)
 * @param auth_method Authentication method used
 * @param success Whether access was granted
 * @param confidence Recognition confidence
 * @return ESP_OK on success
 */
esp_err_t user_mgmt_record_access(uint8_t user_id, uint8_t auth_method,
                                   bool success, uint8_t confidence);

/**
 * @brief Get access log entries
 * @param logs Output log array
 * @param count Output count of entries
 * @param max_count Maximum entries to return
 * @return ESP_OK on success
 */
esp_err_t user_mgmt_get_access_log(sdl_access_log_t *logs, uint8_t *count,
                                    uint8_t max_count);

/**
 * @brief Get security state
 * @param state Output security state
 * @return ESP_OK on success
 */
esp_err_t user_mgmt_get_security_state(sdl_security_state_t *state);

/**
 * @brief Check if system is in lockout
 * @return true if locked out
 */
bool user_mgmt_is_locked_out(void);

/**
 * @brief Record a failed authentication attempt
 * @return true if lockout was triggered
 */
bool user_mgmt_record_failed_attempt(void);

/**
 * @brief Clear failed attempt counter
 */
void user_mgmt_clear_failed_attempts(void);

/**
 * @brief Get total number of enrolled users
 * @return Number of users
 */
uint8_t user_mgmt_get_user_count(void);

/**
 * @brief Save all user data to NVS
 * @return ESP_OK on success
 */
esp_err_t user_mgmt_save(void);

/**
 * @brief Load all user data from NVS
 * @return ESP_OK on success
 */
esp_err_t user_mgmt_load(void);

/**
 * @brief Factory reset - delete all users and logs
 * @return ESP_OK on success
 */
esp_err_t user_mgmt_factory_reset(void);

#ifdef __cplusplus
}
#endif
