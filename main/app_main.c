/**
 * @file app_main.c
 * @brief Smart Door Lock - Main Application Entry Point
 *
 * System Architecture:
 *   - FreeRTOS task: face_task    - Continuous face detection/recognition
 *   - FreeRTOS task: finger_task  - Fingerprint sensor polling
 *   - FreeRTOS task: display_task - Display update management
 *   - Event-driven: door control, WiFi, HTTP server
 *
 * Authentication Flow:
 *   1. Face task detects face -> runs recognition -> posts auth event
 *   2. Finger task detects touch -> runs verification -> posts auth event
 *   3. Auth event handler: check lockout -> verify user -> unlock door
 *   4. Door auto-locks after configured duration
 *
 * Hardware (ESP32-S3):
 *   - OV2640 camera (face recognition)
 *   - AS608 fingerprint sensor (UART1)
 *   - SSD1306 OLED display (I2C)
 *   - Relay/solenoid lock (GPIO38)
 *   - Buzzer (GPIO39), Green LED (GPIO40), Red LED (GPIO41)
 */

#include "app_main.h"
#include "event_manager.h"
#include "face_recognition.h"
#include "fingerprint.h"
#include "keypad.h"
#include "door_lock.h"
#include "user_management.h"
#include "display.h"
#include "wifi_manager.h"
#include "http_server.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include <string.h>
#include <time.h>

static const char *TAG = "APP_MAIN";

#define KEYPAD_INPUT_MAX_LEN  16
#define AUTH_SUPPRESS_WINDOW_MS 3000

/* ============================================================
 * Task handles and synchronization
 * ============================================================ */

static TaskHandle_t  s_face_task_handle   = NULL;
static TaskHandle_t  s_finger_task_handle = NULL;
static TaskHandle_t  s_display_task_handle = NULL;
static SemaphoreHandle_t s_auth_mutex     = NULL;

/* System state */
static struct {
    bool     face_module_ok;
    bool     finger_module_ok;
    bool     keypad_ok;
    bool     display_ok;
    bool     wifi_ok;
    bool     enrolling;
    uint8_t  enroll_user_id;
    uint8_t  enroll_method;  /* 0=face, 1=finger */
    char     keypad_input[KEYPAD_INPUT_MAX_LEN + 1];
    uint8_t  keypad_input_len;
    TickType_t auth_suppress_until;
} s_app = {0};

/* ============================================================
 * Hardware configuration (from Kconfig)
 * ============================================================ */

static const face_camera_config_t CAMERA_CONFIG = {
    .pin_pwdn    = CONFIG_SDL_CAMERA_PIN_PWDN,
    .pin_reset   = CONFIG_SDL_CAMERA_PIN_RESET,
    .pin_xclk    = CONFIG_SDL_CAMERA_PIN_XCLK,
    .pin_siod    = CONFIG_SDL_CAMERA_PIN_SIOD,
    .pin_sioc    = CONFIG_SDL_CAMERA_PIN_SIOC,
    .pin_d7      = CONFIG_SDL_CAMERA_PIN_D7,
    .pin_d6      = CONFIG_SDL_CAMERA_PIN_D6,
    .pin_d5      = CONFIG_SDL_CAMERA_PIN_D5,
    .pin_d4      = CONFIG_SDL_CAMERA_PIN_D4,
    .pin_d3      = CONFIG_SDL_CAMERA_PIN_D3,
    .pin_d2      = CONFIG_SDL_CAMERA_PIN_D2,
    .pin_d1      = CONFIG_SDL_CAMERA_PIN_D1,
    .pin_d0      = CONFIG_SDL_CAMERA_PIN_D0,
    .pin_vsync   = CONFIG_SDL_CAMERA_PIN_VSYNC,
    .pin_href    = CONFIG_SDL_CAMERA_PIN_HREF,
    .pin_pclk    = CONFIG_SDL_CAMERA_PIN_PCLK,
    .xclk_freq_hz = 20000000,
};

static const fp_config_t FINGERPRINT_CONFIG = {
    .uart_num    = CONFIG_SDL_FINGERPRINT_UART_NUM,
    .uart_tx_pin = CONFIG_SDL_FINGERPRINT_UART_TX,
    .uart_rx_pin = CONFIG_SDL_FINGERPRINT_UART_RX,
    .baud_rate   = 57600,
    .device_addr = FP_ADDR_DEFAULT,
    .password    = 0x00000000,
};

static const door_lock_config_t DOOR_LOCK_CONFIG = {
    .lock_gpio        = CONFIG_SDL_DOOR_LOCK_GPIO,
    .active_level     = CONFIG_SDL_DOOR_LOCK_ACTIVE_LEVEL,
    .buzzer_gpio      = CONFIG_SDL_BUZZER_GPIO,
    .led_green_gpio   = CONFIG_SDL_LED_GREEN_GPIO,
    .led_red_gpio     = CONFIG_SDL_LED_RED_GPIO,
    .unlock_duration_ms = CONFIG_SDL_DOOR_UNLOCK_DURATION_MS,
};

static const display_config_t DISPLAY_CONFIG = {
    .i2c_port   = 0,
    .sda_pin    = CONFIG_SDL_DISPLAY_SDA_PIN,
    .scl_pin    = CONFIG_SDL_DISPLAY_SCL_PIN,
    .i2c_freq_hz = 400000,
    .rst_pin    = -1,
};

#if CONFIG_SDL_KEYPAD_ENABLE
static const keypad_config_t KEYPAD_CONFIG = {
    .row_gpios = {
        CONFIG_SDL_KEYPAD_ROW0_GPIO,
        CONFIG_SDL_KEYPAD_ROW1_GPIO,
        CONFIG_SDL_KEYPAD_ROW2_GPIO,
        CONFIG_SDL_KEYPAD_ROW3_GPIO,
    },
    .col_gpios = {
        CONFIG_SDL_KEYPAD_COL0_GPIO,
        CONFIG_SDL_KEYPAD_COL1_GPIO,
        CONFIG_SDL_KEYPAD_COL2_GPIO,
    },
    .scan_interval_ms = 20,
    .debounce_ms      = 40,
};
#endif

/* ============================================================
 * Authentication handler
 * ============================================================ */

static void handle_auth_failure(uint8_t auth_method);

static const char *auth_method_to_string(uint8_t auth_method)
{
    switch (auth_method) {
        case SDL_AUTH_METHOD_FACE:
            return "Face";
        case SDL_AUTH_METHOD_FINGERPRINT:
            return "Fingerprint";
        case SDL_AUTH_METHOD_PASSWORD:
            return "PIN";
        default:
            return "Unknown";
    }
}

static bool auth_result_is_suppressed(uint8_t auth_method)
{
    TickType_t now = xTaskGetTickCount();

    if (s_app.auth_suppress_until != 0 &&
        (int32_t)(s_app.auth_suppress_until - now) > 0) {
        ESP_LOGI(TAG, "Ignoring %s auth result during suppress window",
                 auth_method_to_string(auth_method));
        return true;
    }

    return false;
}

static void auth_start_suppress_window(void)
{
    s_app.auth_suppress_until = xTaskGetTickCount() +
                                pdMS_TO_TICKS(AUTH_SUPPRESS_WINDOW_MS);
}

static sdl_auth_event_id_t get_auth_success_event(uint8_t auth_method)
{
    switch (auth_method) {
        case SDL_AUTH_METHOD_FACE:
            return SDL_AUTH_EVENT_FACE_SUCCESS;
        case SDL_AUTH_METHOD_FINGERPRINT:
            return SDL_AUTH_EVENT_FINGER_SUCCESS;
        case SDL_AUTH_METHOD_PASSWORD:
            return SDL_AUTH_EVENT_PASSWORD_SUCCESS;
        default:
            return SDL_AUTH_EVENT_FACE_SUCCESS;
    }
}

static sdl_auth_event_id_t get_auth_failure_event(uint8_t auth_method)
{
    switch (auth_method) {
        case SDL_AUTH_METHOD_FACE:
            return SDL_AUTH_EVENT_FACE_FAILED;
        case SDL_AUTH_METHOD_FINGERPRINT:
            return SDL_AUTH_EVENT_FINGER_FAILED;
        case SDL_AUTH_METHOD_PASSWORD:
            return SDL_AUTH_EVENT_PASSWORD_FAILED;
        default:
            return SDL_AUTH_EVENT_FACE_FAILED;
    }
}

#if CONFIG_SDL_KEYPAD_ENABLE
static void keypad_reset_input(bool refresh_display)
{
    memset(s_app.keypad_input, 0, sizeof(s_app.keypad_input));
    s_app.keypad_input_len = 0;

    if (refresh_display && s_app.display_ok) {
        display_show_text("Password Unlock", "Enter PIN", "*=Clear", "#=Unlock");
    }
}

static void keypad_show_input(void)
{
    if (!s_app.display_ok) return;

    char masked[KEYPAD_INPUT_MAX_LEN + 1] = {0};
    for (uint8_t i = 0; i < s_app.keypad_input_len; i++) {
        masked[i] = '*';
    }

    display_show_text("Password Unlock",
                      s_app.keypad_input_len > 0 ? masked : "Enter PIN",
                      "*=Clear",
                      "#=Unlock");
}

static void handle_password_auth_success(void)
{
    if (auth_result_is_suppressed(SDL_AUTH_METHOD_PASSWORD)) {
        return;
    }

    ESP_LOGI(TAG, "Password unlock GRANTED");

    user_mgmt_clear_failed_attempts();
    user_mgmt_record_access(0, SDL_AUTH_METHOD_PASSWORD, true, 100);

    if (s_app.display_ok) {
        display_show_access_granted("PIN OK", "Password");
    }

    auth_start_suppress_window();
    door_lock_unlock(DOOR_UNLOCK_SOURCE_KEYPAD, 0);

    sdl_auth_result_t auth_result = {
        .user_id     = 0,
        .auth_method = SDL_AUTH_METHOD_PASSWORD,
        .confidence  = 100,
    };
    strncpy(auth_result.user_name, "PIN", sizeof(auth_result.user_name) - 1);
    event_manager_post_auth(SDL_AUTH_EVENT_PASSWORD_SUCCESS, &auth_result);
}

static void keypad_event_callback(char key, void *ctx)
{
    (void)ctx;

    if (user_mgmt_is_locked_out()) {
        if (s_app.display_ok) {
            display_show_screen(DISPLAY_SCREEN_LOCKED_OUT, "Wait...");
        }
        return;
    }

    if (key >= '0' && key <= '9') {
        if (s_app.keypad_input_len < KEYPAD_INPUT_MAX_LEN) {
            s_app.keypad_input[s_app.keypad_input_len++] = key;
            s_app.keypad_input[s_app.keypad_input_len] = '\0';
            keypad_show_input();
        } else {
            ESP_LOGW(TAG, "Keypad input buffer full");
            if (s_app.display_ok) {
                display_show_access_denied("PIN too long");
            }
        }
        return;
    }

    if (key == '*') {
        keypad_reset_input(true);
        return;
    }

    if (key != '#') {
        ESP_LOGD(TAG, "Ignoring unsupported keypad key '%c'", key);
        return;
    }

    if (s_app.keypad_input_len == 0) {
        keypad_reset_input(true);
        return;
    }

    if (xSemaphoreTake(s_auth_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Auth mutex busy, dropping keypad submit");
        return;
    }

    if (strcmp(s_app.keypad_input, CONFIG_SDL_KEYPAD_PASSWORD) == 0) {
        handle_password_auth_success();
    } else {
        handle_auth_failure(SDL_AUTH_METHOD_PASSWORD);
    }

    xSemaphoreGive(s_auth_mutex);
    keypad_reset_input(false);
}
#endif

static void handle_auth_success(uint8_t user_id, uint8_t auth_method,
                                  uint8_t confidence)
{
    if (auth_result_is_suppressed(auth_method)) {
        return;
    }

    /* Get user info */
    sdl_user_t user;
    esp_err_t ret = user_mgmt_get_user(user_id, &user);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Auth success but user %d not found", user_id);
        return;
    }

    /* Check user status */
    if (user.status != SDL_USER_STATUS_ACTIVE) {
        ESP_LOGW(TAG, "User %d is not active (status=%d)", user_id, user.status);
        door_lock_indicate_denied();
        display_show_access_denied("Account disabled");
        user_mgmt_record_access(user_id, auth_method, false, confidence);
        return;
    }

    /* Check access expiry */
    if (user.access_expire > 0 && time(NULL) > user.access_expire) {
        ESP_LOGW(TAG, "User %d access has expired", user_id);
        door_lock_indicate_denied();
        display_show_access_denied("Access expired");
        user_mgmt_record_access(user_id, auth_method, false, confidence);
        return;
    }

    /* Grant access */
    ESP_LOGI(TAG, "Access GRANTED: user='%s' (ID=%d), method=%s, confidence=%d%%",
             user.name, user_id,
             auth_method == SDL_AUTH_METHOD_FACE ? "Face" : "Fingerprint",
             confidence);

    /* Clear failed attempts */
    user_mgmt_clear_failed_attempts();

    /* Record access */
    user_mgmt_record_access(user_id, auth_method, true, confidence);

    /* Show on display */
    const char *method_str = (auth_method == SDL_AUTH_METHOD_FACE) ?
                              "Face Recognition" : "Fingerprint";
    display_show_access_granted(user.name, method_str);

    /* Unlock door */
    door_unlock_source_t source = (auth_method == SDL_AUTH_METHOD_FACE) ?
                                   DOOR_UNLOCK_SOURCE_FACE :
                                   DOOR_UNLOCK_SOURCE_FINGER;
    auth_start_suppress_window();
    door_lock_unlock(source, user_id);

    /* Post auth event */
    sdl_auth_result_t auth_result = {
        .user_id     = user_id,
        .auth_method = auth_method,
        .confidence  = confidence,
    };
    strncpy(auth_result.user_name, user.name, sizeof(auth_result.user_name) - 1);
    event_manager_post_auth(get_auth_success_event(auth_method), &auth_result);
}

static void handle_auth_failure(uint8_t auth_method)
{
    if (auth_result_is_suppressed(auth_method)) {
        return;
    }

    ESP_LOGW(TAG, "Authentication FAILED (method=%d)", auth_method);

    /* Record failed attempt */
    bool lockout = user_mgmt_record_failed_attempt();
    user_mgmt_record_access(0, auth_method, false, 0);

    if (lockout) {
        ESP_LOGW(TAG, "LOCKOUT triggered!");
        display_show_screen(DISPLAY_SCREEN_LOCKED_OUT, "Too many attempts");
        door_lock_indicate_alarm();
        event_manager_post_auth(SDL_AUTH_EVENT_LOCKOUT, NULL);
    } else {
        display_show_access_denied(auth_method == SDL_AUTH_METHOD_PASSWORD ?
                                   "Wrong PIN" : "Not recognized");
        door_lock_indicate_denied();
        event_manager_post_auth(get_auth_failure_event(auth_method), NULL);
    }
}

/* ============================================================
 * Face recognition task
 * ============================================================ */

static void face_recognition_task(void *arg)
{
    ESP_LOGI(TAG, "Face recognition task started");

    face_result_t result;
    TickType_t last_detect_time = 0;
    const TickType_t DETECT_INTERVAL = pdMS_TO_TICKS(500); /* 2 FPS */
    const TickType_t COOLDOWN_MS     = pdMS_TO_TICKS(3000);

    while (1) {
        /* Check if we should be running */
        if (!s_app.face_module_ok || s_app.enrolling) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        /* Check lockout */
        if (user_mgmt_is_locked_out()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Rate limiting */
        TickType_t now = xTaskGetTickCount();
        if (now - last_detect_time < DETECT_INTERVAL) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        last_detect_time = now;

        /* Run face recognition */
        esp_err_t ret = face_recog_run(&result);
        if (ret != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!result.face_detected) {
            continue;
        }

        ESP_LOGD(TAG, "Face detected (recognized=%d, user=%d, sim=%.2f)",
                 result.face_recognized, result.user_id, result.similarity);

        if (result.face_recognized && result.user_id > 0) {
            /* Take auth mutex to prevent concurrent auth */
            if (xSemaphoreTake(s_auth_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                uint8_t confidence = (uint8_t)(result.similarity * 100);
                handle_auth_success((uint8_t)result.user_id,
                                     SDL_AUTH_METHOD_FACE, confidence);
                xSemaphoreGive(s_auth_mutex);

                /* Cooldown after successful auth */
                vTaskDelay(COOLDOWN_MS);
            }
        } else if (result.face_detected && !result.face_recognized) {
            /* Face detected but not recognized */
            if (user_mgmt_get_user_count() > 0) {
                if (xSemaphoreTake(s_auth_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    handle_auth_failure(SDL_AUTH_METHOD_FACE);
                    xSemaphoreGive(s_auth_mutex);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }
            }
        }
    }
}

/* ============================================================
 * Fingerprint task
 * ============================================================ */

static void fingerprint_task(void *arg)
{
    ESP_LOGI(TAG, "Fingerprint task started");

    fp_search_result_t fp_result;
    fp_enroll_step_t   enroll_step = FP_ENROLL_STEP_1;
    TickType_t         cooldown_until = 0;

    while (1) {
        if (!s_app.finger_module_ok) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* Check lockout */
        if (user_mgmt_is_locked_out()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* Enrollment mode */
        if (s_app.enrolling && s_app.enroll_method == 1) {
            esp_err_t ret = fingerprint_enroll(s_app.enroll_user_id, &enroll_step);

            if (ret == ESP_ERR_NOT_FINISHED) {
                /* Step 1 done, waiting for step 2 */
                display_show_enroll_progress(50, "Remove finger...");
                vTaskDelay(pdMS_TO_TICKS(2000));
                display_show_enroll_progress(50, "Place again...");
            } else if (ret == ESP_OK && enroll_step == FP_ENROLL_STEP_DONE) {
                /* Enrollment complete */
                ESP_LOGI(TAG, "Fingerprint enrolled for user %d at page %d",
                         s_app.enroll_user_id, s_app.enroll_user_id);

                /* Update user record */
                sdl_user_t user;
                if (user_mgmt_get_user(s_app.enroll_user_id, &user) == ESP_OK) {
                    user.fingerprint_id = s_app.enroll_user_id;
                    user.auth_methods  |= SDL_AUTH_METHOD_FINGERPRINT;
                    user_mgmt_update_user(&user);
                }

                display_show_enroll_progress(100, "Enrolled!");
                vTaskDelay(pdMS_TO_TICKS(2000));
                s_app.enrolling = false;
                enroll_step = FP_ENROLL_STEP_1;
                event_manager_post_auth(SDL_AUTH_EVENT_FINGER_ENROLL_DONE, NULL);
            } else if (ret == ESP_ERR_NOT_FOUND) {
                /* No finger - keep waiting */
                vTaskDelay(pdMS_TO_TICKS(200));
            } else {
                /* Error */
                ESP_LOGW(TAG, "Fingerprint enrollment error: %s",
                         esp_err_to_name(ret));
                display_show_enroll_progress(0, "Try again...");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            continue;
        }

        /* Cooldown check */
        if (xTaskGetTickCount() < cooldown_until) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Verification mode: check if finger is present */
        if (!fingerprint_is_finger_present()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Finger detected - run verification */
        ESP_LOGI(TAG, "Finger detected, verifying...");
        display_show_screen(DISPLAY_SCREEN_FINGER_SCAN, NULL);

        esp_err_t ret = fingerprint_verify(&fp_result);

        if (ret == ESP_OK) {
            /* Match found - find user */
            sdl_user_t user;
            esp_err_t user_ret = user_mgmt_find_by_fingerprint(
                (uint8_t)fp_result.page_id, &user);

            if (user_ret == ESP_OK) {
                uint8_t confidence = (uint8_t)(fp_result.match_score * 100 / 200);
                if (confidence > 100) confidence = 100;

                if (xSemaphoreTake(s_auth_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    handle_auth_success(user.id, SDL_AUTH_METHOD_FINGERPRINT,
                                         confidence);
                    xSemaphoreGive(s_auth_mutex);
                }
                cooldown_until = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
            } else {
                ESP_LOGW(TAG, "Fingerprint matched page %d but no user found",
                         fp_result.page_id);
                if (xSemaphoreTake(s_auth_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    handle_auth_failure(SDL_AUTH_METHOD_FINGERPRINT);
                    xSemaphoreGive(s_auth_mutex);
                }
                cooldown_until = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
            }
        } else if (ret == ESP_ERR_NOT_FOUND) {
            /* No match */
            if (user_mgmt_get_user_count() > 0) {
                if (xSemaphoreTake(s_auth_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    handle_auth_failure(SDL_AUTH_METHOD_FINGERPRINT);
                    xSemaphoreGive(s_auth_mutex);
                }
            }
            cooldown_until = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        }

        /* Wait for finger to be removed */
        while (fingerprint_is_finger_present()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/* ============================================================
 * Display task
 * ============================================================ */

static void display_task(void *arg)
{
    ESP_LOGI(TAG, "Display task started");

    TickType_t last_update = 0;
    bool       showing_result = false;
    TickType_t result_until   = 0;

    /* Show idle screen initially */
    display_show_screen(DISPLAY_SCREEN_IDLE, NULL);

    while (1) {
        TickType_t now = xTaskGetTickCount();

        /* Check if result screen should be cleared */
        if (showing_result && now >= result_until) {
            showing_result = false;
            display_show_screen(DISPLAY_SCREEN_IDLE, NULL);
        }

        /* Update lockout countdown if locked out */
        if (user_mgmt_is_locked_out()) {
            if (now - last_update >= pdMS_TO_TICKS(1000)) {
                last_update = now;
                /* Show lockout screen - simplified */
                display_show_screen(DISPLAY_SCREEN_LOCKED_OUT, "Wait...");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ============================================================
 * WiFi event callback
 * ============================================================ */

static void wifi_event_callback(wifi_mgr_state_t state, const char *ip_addr)
{
    switch (state) {
        case WIFI_STATE_CONNECTED:
            ESP_LOGI(TAG, "WiFi connected: %s", ip_addr ? ip_addr : "");
            display_show_screen(DISPLAY_SCREEN_WIFI_CONNECTED, ip_addr);
            vTaskDelay(pdMS_TO_TICKS(2000));
            display_show_screen(DISPLAY_SCREEN_IDLE, NULL);
            break;

        case WIFI_STATE_AP_STARTED:
            ESP_LOGI(TAG, "WiFi AP started");
            display_show_screen(DISPLAY_SCREEN_WIFI_CONNECTED, "192.168.4.1");
            vTaskDelay(pdMS_TO_TICKS(2000));
            display_show_screen(DISPLAY_SCREEN_IDLE, NULL);
            break;

        case WIFI_STATE_DISCONNECTED:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;

        default:
            break;
    }
}

/* ============================================================
 * Load enrolled users into face recognition DB
 * ============================================================ */

static void load_face_features(void)
{
    sdl_user_t users[SDL_USER_MAX_COUNT];
    uint8_t count = 0;
    user_mgmt_get_all_users(users, &count, SDL_USER_MAX_COUNT);

    int loaded = 0;
    for (int i = 0; i < count; i++) {
        if (users[i].has_face &&
            (users[i].auth_methods & SDL_AUTH_METHOD_FACE)) {
            esp_err_t ret = face_recog_load_features(
                users[i].id, users[i].face_features, 128);
            if (ret == ESP_OK) {
                loaded++;
                ESP_LOGI(TAG, "Loaded face features for user '%s' (ID=%d)",
                         users[i].name, users[i].id);
            }
        }
    }
    ESP_LOGI(TAG, "Loaded %d face feature sets", loaded);
}

/* ============================================================
 * Main application entry point
 * ============================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "  Smart Door Lock System v1.0");
    ESP_LOGI(TAG, "  ESP32-S3 | Face + Fingerprint Auth");
    ESP_LOGI(TAG, "===========================================");

    esp_err_t ret;

    /* Create auth mutex */
    s_auth_mutex = xSemaphoreCreateMutex();
    if (s_auth_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create auth mutex");
        esp_restart();
    }

    /* ---- Step 1: Event Manager ---- */
    ret = event_manager_init();
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[1/7] Event manager initialized");

    /* ---- Step 2: User Management ---- */
    ret = user_mgmt_init();
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[2/7] User management initialized (%d users)",
             user_mgmt_get_user_count());

    /* ---- Step 3: Display ---- */
    ret = display_init(&DISPLAY_CONFIG);
    if (ret == ESP_OK) {
        s_app.display_ok = true;
        display_show_text("Smart Door Lock", "Initializing...", NULL, NULL);
        ESP_LOGI(TAG, "[3/7] Display initialized");
    } else {
        ESP_LOGW(TAG, "[3/7] Display init failed (continuing without display)");
    }

    /* ---- Step 4: Door Lock ---- */
    ret = door_lock_init(&DOOR_LOCK_CONFIG);
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[4/7] Door lock initialized");

#if CONFIG_SDL_KEYPAD_ENABLE
    ret = keypad_init(&KEYPAD_CONFIG);
    if (ret == ESP_OK) {
        keypad_register_callback(keypad_event_callback, NULL);
        keypad_reset_input(false);
        s_app.keypad_ok = true;
        ESP_LOGI(TAG, "[4/7] Matrix keypad initialized");
    } else {
        ESP_LOGW(TAG, "[4/7] Matrix keypad init failed: %s",
                 esp_err_to_name(ret));
    }
#endif

    /* ---- Step 5: Face Recognition ---- */
    ret = face_recog_init(&CAMERA_CONFIG);
    if (ret == ESP_OK) {
        s_app.face_module_ok = true;
        /* Set recognition threshold from config */
        float threshold = (float)CONFIG_SDL_FACE_RECOGNITION_THRESHOLD / 100.0f;
        face_recog_set_threshold(threshold);
        /* Load enrolled face features */
        load_face_features();
        ESP_LOGI(TAG, "[5/7] Face recognition initialized (threshold=%.2f)",
                 threshold);
    } else {
        ESP_LOGW(TAG, "[5/7] Camera/face recognition init failed: %s",
                 esp_err_to_name(ret));
    }

    /* ---- Step 6: Fingerprint Sensor ---- */
    ret = fingerprint_init(&FINGERPRINT_CONFIG);
    if (ret == ESP_OK) {
        s_app.finger_module_ok = true;
        fp_sys_params_t fp_params;
        if (fingerprint_read_params(&fp_params) == ESP_OK) {
            ESP_LOGI(TAG, "[6/7] Fingerprint sensor: lib_size=%d, sec_level=%d",
                     fp_params.library_size, fp_params.security_level);
        }
    } else {
        ESP_LOGW(TAG, "[6/7] Fingerprint sensor init failed: %s",
                 esp_err_to_name(ret));
    }

    /* ---- Step 7: WiFi ---- */
    wifi_mgr_config_t wifi_cfg = {
#if CONFIG_SDL_WIFI_AP_MODE
        .mode        = WIFI_MGR_MODE_AP,
#else
        .mode        = WIFI_MGR_MODE_STA,
#endif
        .max_retry   = CONFIG_SDL_WIFI_MAX_RETRY,
        .ap_max_conn = 4,
        .ap_channel  = 1,
    };
    strncpy(wifi_cfg.ssid,     CONFIG_SDL_WIFI_SSID,     sizeof(wifi_cfg.ssid) - 1);
    strncpy(wifi_cfg.password, CONFIG_SDL_WIFI_PASSWORD, sizeof(wifi_cfg.password) - 1);

    if (s_app.display_ok) {
        display_show_screen(DISPLAY_SCREEN_WIFI_CONNECTING, NULL);
    }

    ret = wifi_mgr_init(&wifi_cfg);
    if (ret == ESP_OK) {
        wifi_mgr_register_callback(wifi_event_callback);
        ret = wifi_mgr_start();
        if (ret == ESP_OK) {
            s_app.wifi_ok = true;
            ESP_LOGI(TAG, "[7/7] WiFi started");

            /* Start HTTP server */
            http_server_config_t http_cfg = {
                .port        = 80,
                .max_clients = 4,
            };
            ret = http_server_start(&http_cfg);
            if (ret == ESP_OK) {
                char ip[WIFI_IP_MAX_LEN];
                wifi_mgr_get_ip(ip, sizeof(ip));
                ESP_LOGI(TAG, "HTTP server running at http://%s", ip);
            }
        }
    } else {
        ESP_LOGW(TAG, "[7/7] WiFi init failed");
    }

    /* ---- System ready ---- */
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "  System READY");
    ESP_LOGI(TAG, "  Face module:   %s", s_app.face_module_ok   ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  Finger module: %s", s_app.finger_module_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  Keypad:        %s", s_app.keypad_ok        ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  Display:       %s", s_app.display_ok       ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  WiFi:          %s", s_app.wifi_ok          ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  Users enrolled: %d", user_mgmt_get_user_count());
    ESP_LOGI(TAG, "===========================================");

    if (s_app.display_ok) {
        display_show_screen(DISPLAY_SCREEN_IDLE, NULL);
    }

    event_manager_post_system(SDL_SYSTEM_EVENT_BOOT, NULL);

    /* ---- Start FreeRTOS tasks ---- */

    /* Face recognition task - runs on Core 1 (AI core) */
    if (s_app.face_module_ok) {
        xTaskCreatePinnedToCore(
            face_recognition_task,
            "face_task",
            8192,           /* Stack size */
            NULL,
            5,              /* Priority */
            &s_face_task_handle,
            1               /* Core 1 */
        );
    }

    /* Fingerprint task - runs on Core 0 */
    if (s_app.finger_module_ok) {
        xTaskCreatePinnedToCore(
            fingerprint_task,
            "finger_task",
            4096,
            NULL,
            4,
            &s_finger_task_handle,
            0               /* Core 0 */
        );
    }

    /* Display task - runs on Core 0 */
    if (s_app.display_ok) {
        xTaskCreatePinnedToCore(
            display_task,
            "display_task",
            2048,
            NULL,
            2,
            &s_display_task_handle,
            0
        );
    }

    /* Main task becomes the watchdog/monitor */
    ESP_LOGI(TAG, "All tasks started, entering monitor loop");

    while (1) {
        /* Periodic status log */
        vTaskDelay(pdMS_TO_TICKS(30000)); /* Every 30 seconds */

        ESP_LOGI(TAG, "Status: door=%s, users=%d, wifi=%s",
                 door_lock_is_unlocked() ? "UNLOCKED" : "LOCKED",
                 user_mgmt_get_user_count(),
                 wifi_mgr_is_connected() ? "connected" : "offline");

        /* Check for lockout expiry and update display */
        if (user_mgmt_is_locked_out()) {
            ESP_LOGW(TAG, "System is in lockout state");
        }
    }
}
