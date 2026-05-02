#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Smart Door Lock - Door Lock Control Module
 * Controls the physical door lock via GPIO (relay/servo)
 * ============================================================ */

/* Door lock state */
typedef enum {
    DOOR_STATE_LOCKED = 0,      /* Door is locked */
    DOOR_STATE_UNLOCKED,        /* Door is unlocked */
    DOOR_STATE_LOCKING,         /* Door is in process of locking */
    DOOR_STATE_UNLOCKING,       /* Door is in process of unlocking */
    DOOR_STATE_ALARM,           /* Alarm state (forced entry detected) */
    DOOR_STATE_ERROR,           /* Error state */
} door_state_t;

/* Unlock source */
typedef enum {
    DOOR_UNLOCK_SOURCE_FACE = 0,    /* Unlocked by face recognition */
    DOOR_UNLOCK_SOURCE_FINGER,      /* Unlocked by fingerprint */
    DOOR_UNLOCK_SOURCE_REMOTE,      /* Unlocked remotely via WiFi */
    DOOR_UNLOCK_SOURCE_KEYPAD,      /* Unlocked by matrix keypad password */
    DOOR_UNLOCK_SOURCE_NFC,         /* Unlocked by NFC card */
    DOOR_UNLOCK_SOURCE_MANUAL,      /* Unlocked manually (admin) */
    DOOR_UNLOCK_SOURCE_SCHEDULE,    /* Unlocked by schedule */
} door_unlock_source_t;

/* Door lock configuration */
typedef struct {
    int      lock_gpio;             /* GPIO pin for lock control */
    int      active_level;          /* GPIO level when unlocked (0 or 1) */
    int      buzzer_gpio;           /* GPIO pin for buzzer (-1 if not used) */
    int      led_green_gpio;        /* GPIO pin for green LED (-1 if not used) */
    int      led_red_gpio;          /* GPIO pin for red LED (-1 if not used) */
    uint32_t unlock_duration_ms;    /* Auto-lock delay after unlock (ms) */
} door_lock_config_t;

/* Door lock callback type */
typedef void (*door_state_cb_t)(door_state_t new_state,
                                 door_unlock_source_t source,
                                 uint8_t user_id);

/**
 * @brief Initialize door lock module
 * @param config Door lock configuration
 * @return ESP_OK on success
 */
esp_err_t door_lock_init(const door_lock_config_t *config);

/**
 * @brief Deinitialize door lock module
 * @return ESP_OK on success
 */
esp_err_t door_lock_deinit(void);

/**
 * @brief Unlock the door
 * @param source Source of unlock request
 * @param user_id User ID requesting unlock (0 for remote/manual)
 * @return ESP_OK on success
 */
esp_err_t door_lock_unlock(door_unlock_source_t source, uint8_t user_id);

/**
 * @brief Lock the door immediately
 * @return ESP_OK on success
 */
esp_err_t door_lock_lock(void);

/**
 * @brief Get current door state
 * @return Current door state
 */
door_state_t door_lock_get_state(void);

/**
 * @brief Check if door is currently unlocked
 * @return true if unlocked
 */
bool door_lock_is_unlocked(void);

/**
 * @brief Register a state change callback
 * @param callback Callback function
 */
void door_lock_register_callback(door_state_cb_t callback);

/**
 * @brief Trigger access granted indication (green LED + beep)
 */
void door_lock_indicate_granted(void);

/**
 * @brief Trigger access denied indication (red LED + beep pattern)
 */
void door_lock_indicate_denied(void);

/**
 * @brief Trigger alarm indication (rapid beeping + red LED)
 */
void door_lock_indicate_alarm(void);

/**
 * @brief Trigger system ready indication
 */
void door_lock_indicate_ready(void);

/**
 * @brief Set buzzer state
 * @param on true to turn on, false to turn off
 */
void door_lock_set_buzzer(bool on);

/**
 * @brief Set LED state
 * @param green true to turn on green LED
 * @param red true to turn on red LED
 */
void door_lock_set_leds(bool green, bool red);

/**
 * @brief Get unlock duration setting
 * @return Unlock duration in milliseconds
 */
uint32_t door_lock_get_unlock_duration(void);

/**
 * @brief Set unlock duration
 * @param duration_ms Duration in milliseconds
 */
void door_lock_set_unlock_duration(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
