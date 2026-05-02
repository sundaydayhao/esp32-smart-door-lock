#pragma once

#include "esp_err.h"
#include "esp_event.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Smart Door Lock Event Manager
 * Centralized event bus for inter-module communication
 * ============================================================ */

/* Event base declarations */
ESP_EVENT_DECLARE_BASE(SDL_DOOR_EVENTS);
ESP_EVENT_DECLARE_BASE(SDL_AUTH_EVENTS);
ESP_EVENT_DECLARE_BASE(SDL_SYSTEM_EVENTS);

/* Door events */
typedef enum {
    SDL_DOOR_EVENT_UNLOCK_REQUEST = 0,  /* Request to unlock door */
    SDL_DOOR_EVENT_LOCK_REQUEST,         /* Request to lock door */
    SDL_DOOR_EVENT_UNLOCKED,             /* Door has been unlocked */
    SDL_DOOR_EVENT_LOCKED,               /* Door has been locked */
    SDL_DOOR_EVENT_ALARM,                /* Door alarm triggered */
} sdl_door_event_id_t;

/* Authentication events */
typedef enum {
    SDL_AUTH_EVENT_FACE_SUCCESS = 0,     /* Face recognition succeeded */
    SDL_AUTH_EVENT_FACE_FAILED,          /* Face recognition failed */
    SDL_AUTH_EVENT_FACE_ENROLL_START,    /* Face enrollment started */
    SDL_AUTH_EVENT_FACE_ENROLL_DONE,     /* Face enrollment completed */
    SDL_AUTH_EVENT_FINGER_SUCCESS,       /* Fingerprint recognition succeeded */
    SDL_AUTH_EVENT_FINGER_FAILED,        /* Fingerprint recognition failed */
    SDL_AUTH_EVENT_PASSWORD_SUCCESS,     /* Password verification succeeded */
    SDL_AUTH_EVENT_PASSWORD_FAILED,      /* Password verification failed */
    SDL_AUTH_EVENT_NFC_SUCCESS,          /* NFC card verification succeeded */
    SDL_AUTH_EVENT_NFC_FAILED,           /* NFC card verification failed */
    SDL_AUTH_EVENT_NFC_ENROLL_START,     /* NFC card enrollment started */
    SDL_AUTH_EVENT_NFC_ENROLL_DONE,      /* NFC card enrollment completed */
    SDL_AUTH_EVENT_FINGER_ENROLL_START,  /* Fingerprint enrollment started */
    SDL_AUTH_EVENT_FINGER_ENROLL_DONE,   /* Fingerprint enrollment completed */
    SDL_AUTH_EVENT_LOCKOUT,              /* System locked out due to failed attempts */
    SDL_AUTH_EVENT_LOCKOUT_CLEARED,      /* Lockout cleared */
} sdl_auth_event_id_t;

/* System events */
typedef enum {
    SDL_SYSTEM_EVENT_BOOT = 0,           /* System boot complete */
    SDL_SYSTEM_EVENT_WIFI_CONNECTED,     /* WiFi connected */
    SDL_SYSTEM_EVENT_WIFI_DISCONNECTED,  /* WiFi disconnected */
    SDL_SYSTEM_EVENT_OTA_START,          /* OTA update started */
    SDL_SYSTEM_EVENT_OTA_DONE,           /* OTA update completed */
    SDL_SYSTEM_EVENT_ERROR,              /* System error */
} sdl_system_event_id_t;

/* Authentication result data */
typedef struct {
    uint8_t  user_id;           /* Authenticated user ID */
    char     user_name[32];     /* Authenticated user name */
    uint8_t  auth_method;       /* SDL_AUTH_METHOD_* bit flag */
    uint8_t  confidence;        /* Confidence level (0-100) */
} sdl_auth_result_t;

/* Door event data */
typedef struct {
    uint8_t  source;            /* 0=face, 1=fingerprint, 2=remote, 3=manual */
    uint8_t  user_id;           /* User who triggered the event */
    uint32_t timestamp;         /* Event timestamp */
} sdl_door_event_data_t;

/**
 * @brief Initialize the event manager
 * @return ESP_OK on success
 */
esp_err_t event_manager_init(void);

/**
 * @brief Post a door event
 * @param event_id Door event ID
 * @param data Event data (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t event_manager_post_door(sdl_door_event_id_t event_id,
                                   const sdl_door_event_data_t *data);

/**
 * @brief Post an authentication event
 * @param event_id Auth event ID
 * @param data Auth result data (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t event_manager_post_auth(sdl_auth_event_id_t event_id,
                                   const sdl_auth_result_t *data);

/**
 * @brief Post a system event
 * @param event_id System event ID
 * @param data Event data (can be NULL)
 * @return ESP_OK on success
 */
esp_err_t event_manager_post_system(sdl_system_event_id_t event_id,
                                     const void *data);

#ifdef __cplusplus
}
#endif
