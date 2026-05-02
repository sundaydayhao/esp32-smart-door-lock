/**
 * @file display.c
 * @brief Smart Door Lock Display Module Implementation
 *
 * Drives a 128x64 SSD1306 OLED display via I2C.
 * Provides high-level screen management for the door lock UI.
 */

#include "display.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "DISPLAY";

/* SSD1306 Commands */
#define SSD1306_CMD_DISPLAY_OFF         0xAE
#define SSD1306_CMD_DISPLAY_ON          0xAF
#define SSD1306_CMD_SET_CONTRAST        0x81
#define SSD1306_CMD_ENTIRE_DISPLAY_ON   0xA4
#define SSD1306_CMD_NORMAL_DISPLAY      0xA6
#define SSD1306_CMD_SET_MEM_ADDR_MODE   0x20
#define SSD1306_CMD_SET_COL_ADDR        0x21
#define SSD1306_CMD_SET_PAGE_ADDR       0x22
#define SSD1306_CMD_SET_DISP_START_LINE 0x40
#define SSD1306_CMD_SET_SEG_REMAP       0xA1
#define SSD1306_CMD_SET_MUX_RATIO       0xA8
#define SSD1306_CMD_SET_COM_OUT_DIR     0xC8
#define SSD1306_CMD_SET_DISP_OFFSET     0xD3
#define SSD1306_CMD_SET_COM_PIN_CFG     0xDA
#define SSD1306_CMD_SET_DISP_CLK_DIV    0xD5
#define SSD1306_CMD_SET_PRECHARGE       0xD9
#define SSD1306_CMD_SET_VCOM_DESEL      0xDB
#define SSD1306_CMD_CHARGE_PUMP         0x8D

/* I2C timing */
#define I2C_TIMEOUT_MS          100

/* Display buffer: 128 * 64 / 8 = 1024 bytes */
#define DISPLAY_BUF_SIZE        (DISPLAY_WIDTH * DISPLAY_HEIGHT / 8)

/* 5x7 font (ASCII 32-127) */
static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' 32 */
    {0x00,0x00,0x5F,0x00,0x00}, /* '!' 33 */
    {0x00,0x07,0x00,0x07,0x00}, /* '"' 34 */
    {0x14,0x7F,0x14,0x7F,0x14}, /* '#' 35 */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* '$' 36 */
    {0x23,0x13,0x08,0x64,0x62}, /* '%' 37 */
    {0x36,0x49,0x55,0x22,0x50}, /* '&' 38 */
    {0x00,0x05,0x03,0x00,0x00}, /* ''' 39 */
    {0x00,0x1C,0x22,0x41,0x00}, /* '(' 40 */
    {0x00,0x41,0x22,0x1C,0x00}, /* ')' 41 */
    {0x14,0x08,0x3E,0x08,0x14}, /* '*' 42 */
    {0x08,0x08,0x3E,0x08,0x08}, /* '+' 43 */
    {0x00,0x50,0x30,0x00,0x00}, /* ',' 44 */
    {0x08,0x08,0x08,0x08,0x08}, /* '-' 45 */
    {0x00,0x60,0x60,0x00,0x00}, /* '.' 46 */
    {0x20,0x10,0x08,0x04,0x02}, /* '/' 47 */
    {0x3E,0x51,0x49,0x45,0x3E}, /* '0' 48 */
    {0x00,0x42,0x7F,0x40,0x00}, /* '1' 49 */
    {0x42,0x61,0x51,0x49,0x46}, /* '2' 50 */
    {0x21,0x41,0x45,0x4B,0x31}, /* '3' 51 */
    {0x18,0x14,0x12,0x7F,0x10}, /* '4' 52 */
    {0x27,0x45,0x45,0x45,0x39}, /* '5' 53 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* '6' 54 */
    {0x01,0x71,0x09,0x05,0x03}, /* '7' 55 */
    {0x36,0x49,0x49,0x49,0x36}, /* '8' 56 */
    {0x06,0x49,0x49,0x29,0x1E}, /* '9' 57 */
    {0x00,0x36,0x36,0x00,0x00}, /* ':' 58 */
    {0x00,0x56,0x36,0x00,0x00}, /* ';' 59 */
    {0x08,0x14,0x22,0x41,0x00}, /* '<' 60 */
    {0x14,0x14,0x14,0x14,0x14}, /* '=' 61 */
    {0x00,0x41,0x22,0x14,0x08}, /* '>' 62 */
    {0x02,0x01,0x51,0x09,0x06}, /* '?' 63 */
    {0x32,0x49,0x79,0x41,0x3E}, /* '@' 64 */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 'A' 65 */
    {0x7F,0x49,0x49,0x49,0x36}, /* 'B' 66 */
    {0x3E,0x41,0x41,0x41,0x22}, /* 'C' 67 */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 'D' 68 */
    {0x7F,0x49,0x49,0x49,0x41}, /* 'E' 69 */
    {0x7F,0x09,0x09,0x09,0x01}, /* 'F' 70 */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 'G' 71 */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 'H' 72 */
    {0x00,0x41,0x7F,0x41,0x00}, /* 'I' 73 */
    {0x20,0x40,0x41,0x3F,0x01}, /* 'J' 74 */
    {0x7F,0x08,0x14,0x22,0x41}, /* 'K' 75 */
    {0x7F,0x40,0x40,0x40,0x40}, /* 'L' 76 */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 'M' 77 */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 'N' 78 */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 'O' 79 */
    {0x7F,0x09,0x09,0x09,0x06}, /* 'P' 80 */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 'Q' 81 */
    {0x7F,0x09,0x19,0x29,0x46}, /* 'R' 82 */
    {0x46,0x49,0x49,0x49,0x31}, /* 'S' 83 */
    {0x01,0x01,0x7F,0x01,0x01}, /* 'T' 84 */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 'U' 85 */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 'V' 86 */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 'W' 87 */
    {0x63,0x14,0x08,0x14,0x63}, /* 'X' 88 */
    {0x07,0x08,0x70,0x08,0x07}, /* 'Y' 89 */
    {0x61,0x51,0x49,0x45,0x43}, /* 'Z' 90 */
    {0x00,0x7F,0x41,0x41,0x00}, /* '[' 91 */
    {0x02,0x04,0x08,0x10,0x20}, /* '\' 92 */
    {0x00,0x41,0x41,0x7F,0x00}, /* ']' 93 */
    {0x04,0x02,0x01,0x02,0x04}, /* '^' 94 */
    {0x40,0x40,0x40,0x40,0x40}, /* '_' 95 */
    {0x00,0x01,0x02,0x04,0x00}, /* '`' 96 */
    {0x20,0x54,0x54,0x54,0x78}, /* 'a' 97 */
    {0x7F,0x48,0x44,0x44,0x38}, /* 'b' 98 */
    {0x38,0x44,0x44,0x44,0x20}, /* 'c' 99 */
    {0x38,0x44,0x44,0x48,0x7F}, /* 'd' 100 */
    {0x38,0x54,0x54,0x54,0x18}, /* 'e' 101 */
    {0x08,0x7E,0x09,0x01,0x02}, /* 'f' 102 */
    {0x0C,0x52,0x52,0x52,0x3E}, /* 'g' 103 */
    {0x7F,0x08,0x04,0x04,0x78}, /* 'h' 104 */
    {0x00,0x44,0x7D,0x40,0x00}, /* 'i' 105 */
    {0x20,0x40,0x44,0x3D,0x00}, /* 'j' 106 */
    {0x7F,0x10,0x28,0x44,0x00}, /* 'k' 107 */
    {0x00,0x41,0x7F,0x40,0x00}, /* 'l' 108 */
    {0x7C,0x04,0x18,0x04,0x78}, /* 'm' 109 */
    {0x7C,0x08,0x04,0x04,0x78}, /* 'n' 110 */
    {0x38,0x44,0x44,0x44,0x38}, /* 'o' 111 */
    {0x7C,0x14,0x14,0x14,0x08}, /* 'p' 112 */
    {0x08,0x14,0x14,0x18,0x7C}, /* 'q' 113 */
    {0x7C,0x08,0x04,0x04,0x08}, /* 'r' 114 */
    {0x48,0x54,0x54,0x54,0x20}, /* 's' 115 */
    {0x04,0x3F,0x44,0x40,0x20}, /* 't' 116 */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 'u' 117 */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 'v' 118 */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 'w' 119 */
    {0x44,0x28,0x10,0x28,0x44}, /* 'x' 120 */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 'y' 121 */
    {0x44,0x64,0x54,0x4C,0x44}, /* 'z' 122 */
    {0x00,0x08,0x36,0x41,0x00}, /* '{' 123 */
    {0x00,0x00,0x7F,0x00,0x00}, /* '|' 124 */
    {0x00,0x41,0x36,0x08,0x00}, /* '}' 125 */
    {0x10,0x08,0x08,0x10,0x08}, /* '~' 126 */
};

/* Internal state */
static struct {
    bool     initialized;
    int      i2c_port;
    uint8_t  buf[DISPLAY_BUF_SIZE];
    bool     dirty;
} s_disp = {
    .initialized = false,
    .i2c_port    = -1,
    .dirty       = false,
};

/* ============================================================
 * Private: SSD1306 low-level driver
 * ============================================================ */

static esp_err_t ssd1306_write_cmd(uint8_t cmd)
{
    uint8_t data[2] = { 0x00, cmd }; /* 0x00 = control byte (command) */
    i2c_cmd_handle_t handle = i2c_cmd_link_create();
    i2c_master_start(handle);
    i2c_master_write_byte(handle, (SSD1306_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(handle, data, 2, true);
    i2c_master_stop(handle);
    esp_err_t ret = i2c_master_cmd_begin(s_disp.i2c_port, handle,
                                          pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(handle);
    return ret;
}

static esp_err_t ssd1306_write_data(const uint8_t *data, size_t len)
{
    i2c_cmd_handle_t handle = i2c_cmd_link_create();
    i2c_master_start(handle);
    i2c_master_write_byte(handle, (SSD1306_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(handle, 0x40, true); /* 0x40 = control byte (data) */
    i2c_master_write(handle, (uint8_t *)data, len, true);
    i2c_master_stop(handle);
    esp_err_t ret = i2c_master_cmd_begin(s_disp.i2c_port, handle,
                                          pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(handle);
    return ret;
}

static esp_err_t ssd1306_init_sequence(void)
{
    const uint8_t init_cmds[] = {
        SSD1306_CMD_DISPLAY_OFF,
        SSD1306_CMD_SET_DISP_CLK_DIV,  0x80,
        SSD1306_CMD_SET_MUX_RATIO,     0x3F,   /* 64 rows */
        SSD1306_CMD_SET_DISP_OFFSET,   0x00,
        SSD1306_CMD_SET_DISP_START_LINE,
        SSD1306_CMD_CHARGE_PUMP,       0x14,   /* Enable charge pump */
        SSD1306_CMD_SET_MEM_ADDR_MODE, 0x00,   /* Horizontal addressing */
        SSD1306_CMD_SET_SEG_REMAP,
        SSD1306_CMD_SET_COM_OUT_DIR,
        SSD1306_CMD_SET_COM_PIN_CFG,   0x12,
        SSD1306_CMD_SET_CONTRAST,      0xCF,
        SSD1306_CMD_SET_PRECHARGE,     0xF1,
        SSD1306_CMD_SET_VCOM_DESEL,    0x40,
        SSD1306_CMD_ENTIRE_DISPLAY_ON,
        SSD1306_CMD_NORMAL_DISPLAY,
        SSD1306_CMD_DISPLAY_ON,
    };

    for (size_t i = 0; i < sizeof(init_cmds); i++) {
        esp_err_t ret = ssd1306_write_cmd(init_cmds[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SSD1306 init cmd 0x%02X failed: %s",
                     init_cmds[i], esp_err_to_name(ret));
            return ret;
        }
    }
    return ESP_OK;
}

static void draw_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= DISPLAY_WIDTH || y < 0 || y >= DISPLAY_HEIGHT) return;
    int byte_idx = (y / 8) * DISPLAY_WIDTH + x;
    int bit_idx  = y % 8;
    if (on) {
        s_disp.buf[byte_idx] |= (1 << bit_idx);
    } else {
        s_disp.buf[byte_idx] &= ~(1 << bit_idx);
    }
    s_disp.dirty = true;
}

static void draw_char(int x, int y, char c)
{
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t line = glyph[col];
        for (int row = 0; row < 7; row++) {
            draw_pixel(x + col, y + row, (line >> row) & 1);
        }
    }
}

static void draw_string(int x, int y, const char *str)
{
    if (str == NULL) return;
    int cx = x;
    while (*str && cx < DISPLAY_WIDTH) {
        draw_char(cx, y, *str++);
        cx += 6; /* 5 pixels + 1 space */
    }
}

static void draw_string_centered(int y, const char *str)
{
    if (str == NULL) return;
    int len = strlen(str);
    int x = (DISPLAY_WIDTH - len * 6) / 2;
    if (x < 0) x = 0;
    draw_string(x, y, str);
}

static void draw_hline(int x, int y, int w)
{
    for (int i = 0; i < w; i++) {
        draw_pixel(x + i, y, true);
    }
}

static void draw_rect(int x, int y, int w, int h)
{
    draw_hline(x, y, w);
    draw_hline(x, y + h - 1, w);
    for (int i = 0; i < h; i++) {
        draw_pixel(x, y + i, true);
        draw_pixel(x + w - 1, y + i, true);
    }
}

static void draw_progress_bar(int x, int y, int w, int h, uint8_t percent)
{
    draw_rect(x, y, w, h);
    int fill = (w - 2) * percent / 100;
    for (int i = 0; i < fill; i++) {
        for (int j = 1; j < h - 1; j++) {
            draw_pixel(x + 1 + i, y + j, true);
        }
    }
}

/* ============================================================
 * Public API implementation
 * ============================================================ */

esp_err_t display_init(const display_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Initializing SSD1306 display (I2C%d, SDA:%d, SCL:%d)",
             config->i2c_port, config->sda_pin, config->scl_pin);

    /* Configure I2C master */
    i2c_config_t i2c_conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = config->sda_pin,
        .scl_io_num       = config->scl_pin,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = config->i2c_freq_hz > 0 ?
                            config->i2c_freq_hz : 400000,
    };

    esp_err_t ret = i2c_param_config(config->i2c_port, &i2c_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(config->i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_disp.i2c_port = config->i2c_port;

    /* Initialize SSD1306 */
    ret = ssd1306_init_sequence();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 initialization failed");
        return ret;
    }

    /* Clear display */
    memset(s_disp.buf, 0, DISPLAY_BUF_SIZE);
    s_disp.initialized = true;
    display_update();

    ESP_LOGI(TAG, "Display initialized successfully");
    return ESP_OK;
}

esp_err_t display_deinit(void)
{
    if (!s_disp.initialized) return ESP_ERR_INVALID_STATE;

    ssd1306_write_cmd(SSD1306_CMD_DISPLAY_OFF);
    i2c_driver_delete(s_disp.i2c_port);
    s_disp.initialized = false;
    return ESP_OK;
}

void display_clear(void)
{
    memset(s_disp.buf, 0, DISPLAY_BUF_SIZE);
    s_disp.dirty = true;
}

void display_update(void)
{
    if (!s_disp.initialized) return;

    /* Set column and page address to full display */
    ssd1306_write_cmd(SSD1306_CMD_SET_COL_ADDR);
    ssd1306_write_cmd(0);
    ssd1306_write_cmd(DISPLAY_WIDTH - 1);
    ssd1306_write_cmd(SSD1306_CMD_SET_PAGE_ADDR);
    ssd1306_write_cmd(0);
    ssd1306_write_cmd(7); /* 8 pages for 64 rows */

    /* Write buffer in chunks */
    const int chunk_size = 32;
    for (int i = 0; i < DISPLAY_BUF_SIZE; i += chunk_size) {
        int len = (i + chunk_size > DISPLAY_BUF_SIZE) ?
                  DISPLAY_BUF_SIZE - i : chunk_size;
        ssd1306_write_data(&s_disp.buf[i], len);
    }
    s_disp.dirty = false;
}

void display_show_screen(display_screen_t screen, const char *param)
{
    if (!s_disp.initialized) return;

    display_clear();

    switch (screen) {
        case DISPLAY_SCREEN_IDLE:
            draw_string_centered(0, "Smart Door Lock");
            draw_hline(0, 10, DISPLAY_WIDTH);
            draw_string_centered(18, "Face/Finger/NFC");
#if CONFIG_SDL_KEYPAD_ENABLE
            draw_string_centered(28, "or PIN unlock");
#else
            draw_string_centered(28, "to unlock");
#endif
            draw_hline(0, 52, DISPLAY_WIDTH);
            draw_string_centered(55, "Ready");
            break;

        case DISPLAY_SCREEN_FACE_SCAN:
            draw_string_centered(0, "Face Scan");
            draw_hline(0, 10, DISPLAY_WIDTH);
            /* Draw simple face icon */
            draw_rect(44, 14, 40, 32);
            draw_string_centered(20, "Look at");
            draw_string_centered(30, "camera...");
            break;

        case DISPLAY_SCREEN_FINGER_SCAN:
            draw_string_centered(0, "Fingerprint");
            draw_hline(0, 10, DISPLAY_WIDTH);
            draw_string_centered(20, "Place finger");
            draw_string_centered(30, "on sensor...");
            /* Draw fingerprint icon (simplified) */
            draw_rect(54, 42, 20, 20);
            break;

        case DISPLAY_SCREEN_ACCESS_GRANTED:
            draw_string_centered(5, "ACCESS GRANTED");
            draw_hline(0, 16, DISPLAY_WIDTH);
            if (param) {
                draw_string_centered(25, "Welcome,");
                draw_string_centered(36, param);
            } else {
                draw_string_centered(30, "Welcome!");
            }
            /* Draw checkmark */
            draw_string_centered(50, "[ UNLOCKED ]");
            break;

        case DISPLAY_SCREEN_ACCESS_DENIED:
            draw_string_centered(5, "ACCESS DENIED");
            draw_hline(0, 16, DISPLAY_WIDTH);
            if (param) {
                draw_string_centered(28, param);
            } else {
                draw_string_centered(28, "Not recognized");
            }
            draw_string_centered(50, "[ LOCKED ]");
            break;

        case DISPLAY_SCREEN_ENROLLING:
            draw_string_centered(0, "Enrollment");
            draw_hline(0, 10, DISPLAY_WIDTH);
            if (param) {
                draw_string_centered(18, param);
            }
            break;

        case DISPLAY_SCREEN_LOCKED_OUT:
            draw_string_centered(0, "!! LOCKED OUT !!");
            draw_hline(0, 10, DISPLAY_WIDTH);
            draw_string_centered(18, "Too many failed");
            draw_string_centered(28, "attempts");
            if (param) {
                draw_string_centered(42, "Wait:");
                draw_string_centered(52, param);
            }
            break;

        case DISPLAY_SCREEN_WIFI_CONNECTING:
            draw_string_centered(10, "WiFi");
            draw_string_centered(25, "Connecting...");
            break;

        case DISPLAY_SCREEN_WIFI_CONNECTED:
            draw_string_centered(5, "WiFi Connected");
            draw_hline(0, 16, DISPLAY_WIDTH);
            if (param) {
                draw_string_centered(28, "IP:");
                draw_string_centered(40, param);
            }
            break;

        default:
            if (param) {
                draw_string_centered(28, param);
            }
            break;
    }

    display_update();
}

void display_show_text(const char *line1, const char *line2,
                        const char *line3, const char *line4)
{
    if (!s_disp.initialized) return;

    display_clear();
    if (line1) draw_string(0, 0,  line1);
    if (line2) draw_string(0, 16, line2);
    if (line3) draw_string(0, 32, line3);
    if (line4) draw_string(0, 48, line4);
    display_update();
}

void display_show_enroll_progress(uint8_t progress, const char *step_text)
{
    if (!s_disp.initialized) return;

    display_clear();
    draw_string_centered(0, "Enrolling...");
    draw_hline(0, 10, DISPLAY_WIDTH);

    if (step_text) {
        draw_string_centered(18, step_text);
    }

    /* Progress bar */
    draw_progress_bar(4, 36, 120, 12, progress);

    /* Percentage text */
    char pct_str[8];
    snprintf(pct_str, sizeof(pct_str), "%d%%", progress);
    draw_string_centered(52, pct_str);

    display_update();
}

void display_show_access_granted(const char *user_name,
                                  const char *auth_method)
{
    if (!s_disp.initialized) return;

    display_clear();
    draw_string_centered(2, "ACCESS GRANTED");
    draw_hline(0, 12, DISPLAY_WIDTH);

    if (auth_method) {
        draw_string_centered(18, auth_method);
    }

    if (user_name) {
        draw_string_centered(30, "Welcome,");
        draw_string_centered(42, user_name);
    }

    display_update();
}

void display_show_access_denied(const char *reason)
{
    if (!s_disp.initialized) return;

    display_clear();
    draw_string_centered(2, "ACCESS DENIED");
    draw_hline(0, 12, DISPLAY_WIDTH);

    if (reason) {
        draw_string_centered(28, reason);
    } else {
        draw_string_centered(28, "Not recognized");
    }

    draw_string_centered(50, "Try again");
    display_update();
}

void display_show_lockout(uint32_t seconds_remaining)
{
    if (!s_disp.initialized) return;

    display_clear();
    draw_string_centered(0, "SYSTEM LOCKED");
    draw_hline(0, 10, DISPLAY_WIDTH);
    draw_string_centered(18, "Too many attempts");
    draw_string_centered(30, "Please wait:");

    char time_str[16];
    if (seconds_remaining >= 60) {
        snprintf(time_str, sizeof(time_str), "%lum %lus",
                 (unsigned long)(seconds_remaining / 60),
                 (unsigned long)(seconds_remaining % 60));
    } else {
        snprintf(time_str, sizeof(time_str), "%lu seconds",
                 (unsigned long)seconds_remaining);
    }
    draw_string_centered(44, time_str);
    display_update();
}

void display_show_status(uint8_t user_count, bool wifi_connected,
                          const char *ip_addr)
{
    if (!s_disp.initialized) return;

    display_clear();
    draw_string_centered(0, "System Status");
    draw_hline(0, 10, DISPLAY_WIDTH);

    char buf[24];
    snprintf(buf, sizeof(buf), "Users: %d", user_count);
    draw_string(0, 14, buf);

    if (wifi_connected) {
        draw_string(0, 26, "WiFi: Connected");
        if (ip_addr) {
            draw_string(0, 38, ip_addr);
        }
    } else {
        draw_string(0, 26, "WiFi: Offline");
    }

    display_update();
}

void display_set_brightness(uint8_t brightness)
{
    if (!s_disp.initialized) return;
    ssd1306_write_cmd(SSD1306_CMD_SET_CONTRAST);
    ssd1306_write_cmd(brightness);
}

bool display_is_ready(void)
{
    return s_disp.initialized;
}
