#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Smart Door Lock Display Module
 * Supports SSD1306 OLED (128x64) via I2C
 * ============================================================ */

/* Display dimensions */
#define DISPLAY_WIDTH           128
#define DISPLAY_HEIGHT          64

/* SSD1306 I2C address */
#define SSD1306_I2C_ADDR        0x3C

/* Display configuration */
typedef struct {
    int  i2c_port;      /* I2C port number */
    int  sda_pin;       /* SDA pin */
    int  scl_pin;       /* SCL pin */
    int  i2c_freq_hz;   /* I2C frequency (default 400000) */
    int  rst_pin;       /* Reset pin (-1 if not used) */
} display_config_t;

/* Display screen types */
typedef enum {
    DISPLAY_SCREEN_IDLE = 0,        /* Idle/standby screen */
    DISPLAY_SCREEN_SCANNING,        /* Scanning for face/finger */
    DISPLAY_SCREEN_FACE_SCAN,       /* Face scanning in progress */
    DISPLAY_SCREEN_FINGER_SCAN,     /* Fingerprint scanning */
    DISPLAY_SCREEN_ACCESS_GRANTED,  /* Access granted */
    DISPLAY_SCREEN_ACCESS_DENIED,   /* Access denied */
    DISPLAY_SCREEN_ENROLLING,       /* Enrollment in progress */
    DISPLAY_SCREEN_LOCKED_OUT,      /* System locked out */
    DISPLAY_SCREEN_WIFI_CONNECTING, /* WiFi connecting */
    DISPLAY_SCREEN_WIFI_CONNECTED,  /* WiFi connected */
    DISPLAY_SCREEN_CUSTOM,          /* Custom text screen */
} display_screen_t;

/**
 * @brief Initialize display module
 * @param config Display configuration
 * @return ESP_OK on success
 */
esp_err_t display_init(const display_config_t *config);

/**
 * @brief Deinitialize display module
 * @return ESP_OK on success
 */
esp_err_t display_deinit(void);

/**
 * @brief Clear the display
 */
void display_clear(void);

/**
 * @brief Show a predefined screen
 * @param screen Screen type to show
 * @param param Optional parameter (e.g., user name, progress)
 */
void display_show_screen(display_screen_t screen, const char *param);

/**
 * @brief Show custom text on display
 * @param line1 First line text (NULL to skip)
 * @param line2 Second line text (NULL to skip)
 * @param line3 Third line text (NULL to skip)
 * @param line4 Fourth line text (NULL to skip)
 */
void display_show_text(const char *line1, const char *line2,
                        const char *line3, const char *line4);

/**
 * @brief Show enrollment progress
 * @param progress Progress percentage (0-100)
 * @param step_text Current step description
 */
void display_show_enroll_progress(uint8_t progress, const char *step_text);

/**
 * @brief Show access granted with user name
 * @param user_name User name to display
 * @param auth_method "Face" or "Fingerprint"
 */
void display_show_access_granted(const char *user_name,
                                  const char *auth_method);

/**
 * @brief Show access denied
 * @param reason Reason for denial
 */
void display_show_access_denied(const char *reason);

/**
 * @brief Show lockout screen with countdown
 * @param seconds_remaining Seconds until lockout expires
 */
void display_show_lockout(uint32_t seconds_remaining);

/**
 * @brief Show system status
 * @param user_count Number of enrolled users
 * @param wifi_connected Whether WiFi is connected
 * @param ip_addr IP address string (NULL if not connected)
 */
void display_show_status(uint8_t user_count, bool wifi_connected,
                          const char *ip_addr);

/**
 * @brief Update display (flush buffer to screen)
 */
void display_update(void);

/**
 * @brief Set display brightness
 * @param brightness 0-255
 */
void display_set_brightness(uint8_t brightness);

/**
 * @brief Check if display is initialized
 * @return true if initialized
 */
bool display_is_ready(void);

#ifdef __cplusplus
}
#endif