#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KEYPAD_ROWS  4
#define KEYPAD_COLS  3

typedef struct {
    int row_gpios[KEYPAD_ROWS];
    int col_gpios[KEYPAD_COLS];
    uint32_t scan_interval_ms;
    uint32_t debounce_ms;
} keypad_config_t;

typedef void (*keypad_key_cb_t)(char key, void *ctx);

esp_err_t keypad_init(const keypad_config_t *config);
esp_err_t keypad_deinit(void);
void keypad_register_callback(keypad_key_cb_t callback, void *ctx);
bool keypad_is_ready(void);

#ifdef __cplusplus
}
#endif
