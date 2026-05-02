/**
 * @file keypad.c
 * @brief 4x3 matrix keypad scanner
 */

#include "keypad.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "KEYPAD";

static const char s_keymap[KEYPAD_ROWS][KEYPAD_COLS] = {
    { '1', '2', '3' },
    { '4', '5', '6' },
    { '7', '8', '9' },
    { '*', '0', '#' },
};

static struct {
    bool            initialized;
    keypad_config_t config;
    keypad_key_cb_t callback;
    void           *cb_ctx;
    TaskHandle_t    task_handle;
} s_keypad = {0};

static void set_all_rows(int level)
{
    for (int i = 0; i < KEYPAD_ROWS; i++) {
        gpio_set_level(s_keypad.config.row_gpios[i], level);
    }
}

static char scan_raw_key(void)
{
    char key = 0;

    for (int row = 0; row < KEYPAD_ROWS; row++) {
        set_all_rows(1);
        gpio_set_level(s_keypad.config.row_gpios[row], 0);
        esp_rom_delay_us(30);

        for (int col = 0; col < KEYPAD_COLS; col++) {
            if (gpio_get_level(s_keypad.config.col_gpios[col]) == 0) {
                key = s_keymap[row][col];
                break;
            }
        }

        if (key != 0) {
            break;
        }
    }

    set_all_rows(1);
    return key;
}

static void keypad_task(void *arg)
{
    (void)arg;

    char last_raw = 0;
    char stable_key = 0;
    TickType_t stable_since = xTaskGetTickCount();

    while (1) {
        char raw_key = scan_raw_key();
        TickType_t now = xTaskGetTickCount();

        if (raw_key != last_raw) {
            last_raw = raw_key;
            stable_since = now;
        } else if (raw_key != stable_key &&
                   (now - stable_since) >= pdMS_TO_TICKS(s_keypad.config.debounce_ms)) {
            stable_key = raw_key;
            if (stable_key != 0 && s_keypad.callback != NULL) {
                s_keypad.callback(stable_key, s_keypad.cb_ctx);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(s_keypad.config.scan_interval_ms));
    }
}

esp_err_t keypad_init(const keypad_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    if (s_keypad.initialized) return ESP_OK;

    memcpy(&s_keypad.config, config, sizeof(s_keypad.config));

    for (int i = 0; i < KEYPAD_ROWS; i++) {
        if (config->row_gpios[i] < 0) return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < KEYPAD_COLS; i++) {
        if (config->col_gpios[i] < 0) return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t row_conf = {
        .pin_bit_mask = 0,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < KEYPAD_ROWS; i++) {
        row_conf.pin_bit_mask |= (1ULL << config->row_gpios[i]);
    }

    esp_err_t ret = gpio_config(&row_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure row GPIOs: %s", esp_err_to_name(ret));
        return ret;
    }
    set_all_rows(1);

    gpio_config_t col_conf = {
        .pin_bit_mask = 0,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    for (int i = 0; i < KEYPAD_COLS; i++) {
        col_conf.pin_bit_mask |= (1ULL << config->col_gpios[i]);
    }

    ret = gpio_config(&col_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure column GPIOs: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_keypad.config.scan_interval_ms == 0) {
        s_keypad.config.scan_interval_ms = 20;
    }
    if (s_keypad.config.debounce_ms == 0) {
        s_keypad.config.debounce_ms = 40;
    }

    if (xTaskCreate(keypad_task, "keypad_task", 3072, NULL, 3,
                    &s_keypad.task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create keypad task");
        return ESP_ERR_NO_MEM;
    }

    s_keypad.initialized = true;
    ESP_LOGI(TAG, "Matrix keypad initialized");
    return ESP_OK;
}

esp_err_t keypad_deinit(void)
{
    if (!s_keypad.initialized) return ESP_ERR_INVALID_STATE;

    if (s_keypad.task_handle != NULL) {
        vTaskDelete(s_keypad.task_handle);
        s_keypad.task_handle = NULL;
    }

    memset(&s_keypad, 0, sizeof(s_keypad));
    return ESP_OK;
}

void keypad_register_callback(keypad_key_cb_t callback, void *ctx)
{
    s_keypad.callback = callback;
    s_keypad.cb_ctx = ctx;
}

bool keypad_is_ready(void)
{
    return s_keypad.initialized;
}
