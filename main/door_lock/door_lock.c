/**
 * @file door_lock.c
 * @brief Smart Door Lock - Door Lock Control Implementation
 *
 * Controls the physical door lock via GPIO output.
 * Supports relay-based locks and servo-based locks.
 * Provides visual (LED) and audio (buzzer) feedback.
 */

#include "door_lock.h"
#include "event_manager.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "DOOR_LOCK";

/* Buzzer PWM configuration */
#define BUZZER_LEDC_TIMER       LEDC_TIMER_1
#define BUZZER_LEDC_CHANNEL     LEDC_CHANNEL_1
#define BUZZER_FREQ_HZ          2000
#define BUZZER_DUTY_CYCLE       4096    /* 50% duty for 13-bit resolution */

/* Beep patterns (duration in ms) */
#define BEEP_SHORT              100
#define BEEP_LONG               500
#define BEEP_PAUSE              100

/* Internal state */
static struct {
    bool                 initialized;
    door_lock_config_t   config;
    door_state_t         state;
    door_state_cb_t      callback;
    TimerHandle_t        auto_lock_timer;
    bool                 buzzer_pwm_init;
} s_lock = {
    .initialized    = false,
    .state          = DOOR_STATE_LOCKED,
    .callback       = NULL,
    .auto_lock_timer = NULL,
    .buzzer_pwm_init = false,
};

/* ============================================================
 * Private functions
 * ============================================================ */

static void set_lock_gpio(bool unlock)
{
    int level = unlock ? s_lock.config.active_level :
                         !s_lock.config.active_level;
    gpio_set_level(s_lock.config.lock_gpio, level);
    ESP_LOGD(TAG, "Lock GPIO %d set to %d (unlock=%d)",
             s_lock.config.lock_gpio, level, unlock);
}

static void auto_lock_timer_cb(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "Auto-lock timer expired, locking door");
    door_lock_lock();
}

static void notify_state_change(door_state_t new_state,
                                  door_unlock_source_t source,
                                  uint8_t user_id)
{
    s_lock.state = new_state;

    if (s_lock.callback) {
        s_lock.callback(new_state, source, user_id);
    }

    /* Post event */
    sdl_door_event_data_t evt_data = {
        .source    = (uint8_t)source,
        .user_id   = user_id,
        .timestamp = (uint32_t)(esp_timer_get_time() / 1000000),
    };

    if (new_state == DOOR_STATE_UNLOCKED) {
        event_manager_post_door(SDL_DOOR_EVENT_UNLOCKED, &evt_data);
    } else if (new_state == DOOR_STATE_LOCKED) {
        event_manager_post_door(SDL_DOOR_EVENT_LOCKED, &evt_data);
    } else if (new_state == DOOR_STATE_ALARM) {
        event_manager_post_door(SDL_DOOR_EVENT_ALARM, &evt_data);
    }
}

static void buzzer_beep_task(void *arg)
{
    /* Beep pattern: [on_ms, off_ms, on_ms, off_ms, ...], terminated by 0 */
    uint32_t *pattern = (uint32_t *)arg;
    int i = 0;
    bool on = true;

    while (pattern[i] != 0) {
        door_lock_set_buzzer(on);
        vTaskDelay(pdMS_TO_TICKS(pattern[i]));
        on = !on;
        i++;
    }
    door_lock_set_buzzer(false);
    free(pattern);
    vTaskDelete(NULL);
}

static void play_beep_pattern(const uint32_t *pattern, int count)
{
    if (s_lock.config.buzzer_gpio < 0) return;

    uint32_t *pat_copy = malloc((count + 1) * sizeof(uint32_t));
    if (pat_copy == NULL) return;

    memcpy(pat_copy, pattern, count * sizeof(uint32_t));
    pat_copy[count] = 0; /* Terminator */

    xTaskCreate(buzzer_beep_task, "buzzer", 1024, pat_copy,
                tskIDLE_PRIORITY + 1, NULL);
}

/* ============================================================
 * Public API implementation
 * ============================================================ */

esp_err_t door_lock_init(const door_lock_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Initializing door lock (GPIO %d, active=%d)",
             config->lock_gpio, config->active_level);

    memcpy(&s_lock.config, config, sizeof(door_lock_config_t));

    /* Configure lock GPIO */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << config->lock_gpio),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure lock GPIO: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure LED GPIOs */
    if (config->led_green_gpio >= 0) {
        gpio_config_t led_conf = {
            .pin_bit_mask = (1ULL << config->led_green_gpio),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&led_conf);
        gpio_set_level(config->led_green_gpio, 0);
    }

    if (config->led_red_gpio >= 0) {
        gpio_config_t led_conf = {
            .pin_bit_mask = (1ULL << config->led_red_gpio),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&led_conf);
        gpio_set_level(config->led_red_gpio, 0);
    }

    /* Configure buzzer with LEDC PWM */
    if (config->buzzer_gpio >= 0) {
        ledc_timer_config_t ledc_timer = {
            .speed_mode      = LEDC_LOW_SPEED_MODE,
            .timer_num       = BUZZER_LEDC_TIMER,
            .duty_resolution = LEDC_TIMER_13_BIT,
            .freq_hz         = BUZZER_FREQ_HZ,
            .clk_cfg         = LEDC_AUTO_CLK,
        };
        ret = ledc_timer_config(&ledc_timer);
        if (ret == ESP_OK) {
            ledc_channel_config_t ledc_channel = {
                .speed_mode = LEDC_LOW_SPEED_MODE,
                .channel    = BUZZER_LEDC_CHANNEL,
                .timer_sel  = BUZZER_LEDC_TIMER,
                .intr_type  = LEDC_INTR_DISABLE,
                .gpio_num   = config->buzzer_gpio,
                .duty       = 0,
                .hpoint     = 0,
            };
            ret = ledc_channel_config(&ledc_channel);
            if (ret == ESP_OK) {
                s_lock.buzzer_pwm_init = true;
            }
        }
        if (!s_lock.buzzer_pwm_init) {
            ESP_LOGW(TAG, "Buzzer PWM init failed, using GPIO mode");
            gpio_config_t buz_conf = {
                .pin_bit_mask = (1ULL << config->buzzer_gpio),
                .mode         = GPIO_MODE_OUTPUT,
                .pull_up_en   = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type    = GPIO_INTR_DISABLE,
            };
            gpio_config(&buz_conf);
            gpio_set_level(config->buzzer_gpio, 0);
        }
    }

    /* Create auto-lock timer */
    s_lock.auto_lock_timer = xTimerCreate(
        "auto_lock",
        pdMS_TO_TICKS(config->unlock_duration_ms),
        pdFALSE,    /* One-shot */
        NULL,
        auto_lock_timer_cb
    );
    if (s_lock.auto_lock_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create auto-lock timer");
        return ESP_ERR_NO_MEM;
    }

    /* Ensure door starts locked */
    set_lock_gpio(false);
    s_lock.state = DOOR_STATE_LOCKED;
    s_lock.initialized = true;

    ESP_LOGI(TAG, "Door lock initialized (unlock duration: %d ms)",
             config->unlock_duration_ms);

    /* Startup indication */
    door_lock_indicate_ready();

    return ESP_OK;
}

esp_err_t door_lock_deinit(void)
{
    if (!s_lock.initialized) return ESP_ERR_INVALID_STATE;

    /* Ensure door is locked */
    set_lock_gpio(false);

    if (s_lock.auto_lock_timer) {
        xTimerDelete(s_lock.auto_lock_timer, portMAX_DELAY);
        s_lock.auto_lock_timer = NULL;
    }

    s_lock.initialized = false;
    return ESP_OK;
}

esp_err_t door_lock_unlock(door_unlock_source_t source, uint8_t user_id)
{
    if (!s_lock.initialized) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Unlocking door (source=%d, user=%d)", source, user_id);

    /* Stop any pending auto-lock timer */
    xTimerStop(s_lock.auto_lock_timer, 0);

    /* Unlock the door */
    set_lock_gpio(true);
    notify_state_change(DOOR_STATE_UNLOCKED, source, user_id);

    /* Visual/audio feedback */
    door_lock_indicate_granted();

    /* Start auto-lock timer */
    xTimerChangePeriod(s_lock.auto_lock_timer,
                        pdMS_TO_TICKS(s_lock.config.unlock_duration_ms),
                        0);
    xTimerStart(s_lock.auto_lock_timer, 0);

    ESP_LOGI(TAG, "Door unlocked, will auto-lock in %d ms",
             s_lock.config.unlock_duration_ms);
    return ESP_OK;
}

esp_err_t door_lock_lock(void)
{
    if (!s_lock.initialized) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "Locking door");

    /* Stop auto-lock timer */
    xTimerStop(s_lock.auto_lock_timer, 0);

    /* Lock the door */
    set_lock_gpio(false);
    notify_state_change(DOOR_STATE_LOCKED, DOOR_UNLOCK_SOURCE_MANUAL, 0);

    /* Turn off LEDs */
    door_lock_set_leds(false, false);

    return ESP_OK;
}

door_state_t door_lock_get_state(void)
{
    return s_lock.state;
}

bool door_lock_is_unlocked(void)
{
    return (s_lock.state == DOOR_STATE_UNLOCKED);
}

void door_lock_register_callback(door_state_cb_t callback)
{
    s_lock.callback = callback;
}

void door_lock_indicate_granted(void)
{
    /* Green LED on, red LED off */
    door_lock_set_leds(true, false);

    /* Single short beep */
    static const uint32_t pattern[] = { BEEP_SHORT };
    play_beep_pattern(pattern, 1);
}

void door_lock_indicate_denied(void)
{
    /* Red LED on, green LED off */
    door_lock_set_leds(false, true);

    /* Three short beeps */
    static const uint32_t pattern[] = {
        BEEP_SHORT, BEEP_PAUSE,
        BEEP_SHORT, BEEP_PAUSE,
        BEEP_SHORT
    };
    play_beep_pattern(pattern, 5);

    /* Turn off red LED after 2 seconds */
    vTaskDelay(pdMS_TO_TICKS(2000));
    door_lock_set_leds(false, false);
}

void door_lock_indicate_alarm(void)
{
    /* Rapid alternating LEDs + continuous beeping */
    door_lock_set_leds(false, true);

    static const uint32_t pattern[] = {
        200, 100, 200, 100, 200, 100,
        200, 100, 200, 100, 200
    };
    play_beep_pattern(pattern, 11);
}

void door_lock_indicate_ready(void)
{
    /* Two short beeps on startup */
    static const uint32_t pattern[] = {
        BEEP_SHORT, BEEP_PAUSE, BEEP_SHORT
    };
    play_beep_pattern(pattern, 3);

    /* Brief green LED flash */
    door_lock_set_leds(true, false);
    vTaskDelay(pdMS_TO_TICKS(500));
    door_lock_set_leds(false, false);
}

void door_lock_set_buzzer(bool on)
{
    if (s_lock.config.buzzer_gpio < 0) return;

    if (s_lock.buzzer_pwm_init) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL,
                      on ? BUZZER_DUTY_CYCLE : 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, BUZZER_LEDC_CHANNEL);
    } else {
        gpio_set_level(s_lock.config.buzzer_gpio, on ? 1 : 0);
    }
}

void door_lock_set_leds(bool green, bool red)
{
    if (s_lock.config.led_green_gpio >= 0) {
        gpio_set_level(s_lock.config.led_green_gpio, green ? 1 : 0);
    }
    if (s_lock.config.led_red_gpio >= 0) {
        gpio_set_level(s_lock.config.led_red_gpio, red ? 1 : 0);
    }
}

uint32_t door_lock_get_unlock_duration(void)
{
    return s_lock.config.unlock_duration_ms;
}

void door_lock_set_unlock_duration(uint32_t duration_ms)
{
    s_lock.config.unlock_duration_ms = duration_ms;
    ESP_LOGI(TAG, "Unlock duration set to %d ms", duration_ms);
}
