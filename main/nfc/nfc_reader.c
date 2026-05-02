/**
 * @file nfc_reader.c
 * @brief PN532 NFC reader over I2C.
 */

#include "nfc_reader.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "NFC_READER";

#define PN532_PREAMBLE                  0x00
#define PN532_STARTCODE1                0x00
#define PN532_STARTCODE2                0xFF
#define PN532_POSTAMBLE                 0x00
#define PN532_HOST_TO_PN532             0xD4
#define PN532_PN532_TO_HOST             0xD5
#define PN532_I2C_READY                 0x01

#define PN532_CMD_GET_FIRMWARE_VERSION  0x02
#define PN532_CMD_SAM_CONFIGURATION     0x14
#define PN532_CMD_IN_LIST_PASSIVE       0x4A

#define PN532_FRAME_MAX                 80
#define PN532_ACK_FRAME_LEN             6
#define PN532_I2C_STATUS_LEN            1
#define PN532_I2C_TIMEOUT_MS            100
#define PN532_READY_POLL_MS             10

static const uint8_t PN532_ACK_FRAME[PN532_ACK_FRAME_LEN] = {
    0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00
};

static struct {
    bool initialized;
    bool ready;
    nfc_reader_config_t config;
} s_nfc = {0};

static esp_err_t pn532_i2c_write(const uint8_t *data, size_t len)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) return ESP_ERR_NO_MEM;

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_nfc.config.i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, (uint8_t *)data, len, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(s_nfc.config.i2c_port, cmd,
                                          pdMS_TO_TICKS(PN532_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t pn532_i2c_read(uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (cmd == NULL) return ESP_ERR_NO_MEM;

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_nfc.config.i2c_addr << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, data, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, data + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(s_nfc.config.i2c_port, cmd,
                                          pdMS_TO_TICKS(PN532_I2C_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t pn532_wait_ready(uint32_t timeout_ms)
{
    uint32_t elapsed = 0;

    while (elapsed <= timeout_ms) {
        uint8_t status = 0;
        esp_err_t ret = pn532_i2c_read(&status, sizeof(status));
        if (ret == ESP_OK && status == PN532_I2C_READY) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(PN532_READY_POLL_MS));
        elapsed += PN532_READY_POLL_MS;
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t pn532_read_ack(uint32_t timeout_ms)
{
    esp_err_t ret = pn532_wait_ready(timeout_ms);
    if (ret != ESP_OK) return ret;

    uint8_t ack[PN532_I2C_STATUS_LEN + PN532_ACK_FRAME_LEN] = {0};
    ret = pn532_i2c_read(ack, sizeof(ack));
    if (ret != ESP_OK) return ret;

    if (ack[0] != PN532_I2C_READY ||
        memcmp(&ack[PN532_I2C_STATUS_LEN], PN532_ACK_FRAME, PN532_ACK_FRAME_LEN) != 0) {
        ESP_LOGW(TAG, "Invalid PN532 ACK");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static uint8_t pn532_checksum(uint8_t sum)
{
    return (uint8_t)(~sum + 1);
}

static esp_err_t pn532_write_command(const uint8_t *cmd, size_t cmd_len)
{
    if (cmd == NULL || cmd_len == 0 || cmd_len > 64) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t frame[PN532_FRAME_MAX] = {0};
    uint8_t len = (uint8_t)(cmd_len + 1); /* TFI + command bytes */
    uint8_t sum = PN532_HOST_TO_PN532;
    size_t idx = 0;

    frame[idx++] = PN532_PREAMBLE;
    frame[idx++] = PN532_STARTCODE1;
    frame[idx++] = PN532_STARTCODE2;
    frame[idx++] = len;
    frame[idx++] = pn532_checksum(len);
    frame[idx++] = PN532_HOST_TO_PN532;

    for (size_t i = 0; i < cmd_len; i++) {
        frame[idx++] = cmd[i];
        sum = (uint8_t)(sum + cmd[i]);
    }

    frame[idx++] = pn532_checksum(sum);
    frame[idx++] = PN532_POSTAMBLE;

    return pn532_i2c_write(frame, idx);
}

static esp_err_t pn532_read_response(uint8_t *payload, size_t *payload_len,
                                      uint32_t timeout_ms)
{
    if (payload == NULL || payload_len == NULL || *payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = pn532_wait_ready(timeout_ms);
    if (ret != ESP_OK) return ret;

    uint8_t raw[PN532_I2C_STATUS_LEN + PN532_FRAME_MAX] = {0};
    ret = pn532_i2c_read(raw, sizeof(raw));
    if (ret != ESP_OK) return ret;

    if (raw[0] != PN532_I2C_READY) {
        return ESP_ERR_TIMEOUT;
    }

    const uint8_t *frame = &raw[PN532_I2C_STATUS_LEN];
    if (frame[0] != PN532_PREAMBLE ||
        frame[1] != PN532_STARTCODE1 ||
        frame[2] != PN532_STARTCODE2) {
        ESP_LOGW(TAG, "Invalid PN532 response preamble");
        return ESP_FAIL;
    }

    uint8_t len = frame[3];
    uint8_t lcs = frame[4];
    if ((uint8_t)(len + lcs) != 0 || len < 2) {
        ESP_LOGW(TAG, "Invalid PN532 response length");
        return ESP_FAIL;
    }

    if (frame[5] != PN532_PN532_TO_HOST) {
        ESP_LOGW(TAG, "Invalid PN532 response TFI 0x%02X", frame[5]);
        return ESP_FAIL;
    }

    uint8_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum = (uint8_t)(sum + frame[5 + i]);
    }
    sum = (uint8_t)(sum + frame[5 + len]);
    if (sum != 0) {
        ESP_LOGW(TAG, "Invalid PN532 response checksum");
        return ESP_FAIL;
    }

    size_t copy_len = len - 1; /* Exclude TFI, keep response code + data. */
    if (copy_len > *payload_len) {
        return ESP_ERR_NO_MEM;
    }

    memcpy(payload, &frame[6], copy_len);
    *payload_len = copy_len;
    return ESP_OK;
}

static esp_err_t pn532_transceive(const uint8_t *cmd, size_t cmd_len,
                                   uint8_t expected_response,
                                   uint8_t *response, size_t *response_len,
                                   uint32_t timeout_ms)
{
    esp_err_t ret = pn532_write_command(cmd, cmd_len);
    if (ret != ESP_OK) return ret;

    ret = pn532_read_ack(timeout_ms);
    if (ret != ESP_OK) return ret;

    ret = pn532_read_response(response, response_len, timeout_ms);
    if (ret != ESP_OK) return ret;

    if (*response_len == 0 || response[0] != expected_response) {
        ESP_LOGW(TAG, "Unexpected PN532 response 0x%02X, expected 0x%02X",
                 *response_len > 0 ? response[0] : 0, expected_response);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void pn532_reset(void)
{
    if (s_nfc.config.reset_gpio < 0) return;

    gpio_config_t reset_conf = {
        .pin_bit_mask = (1ULL << s_nfc.config.reset_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&reset_conf);

    gpio_set_level(s_nfc.config.reset_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(s_nfc.config.reset_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(80));
    gpio_set_level(s_nfc.config.reset_gpio, 1);
    vTaskDelay(pdMS_TO_TICKS(400));
}

static esp_err_t pn532_sam_configuration(void)
{
    uint8_t cmd[] = {
        PN532_CMD_SAM_CONFIGURATION,
        0x01, /* Normal mode */
        0x14, /* Timeout: 50 ms * 20 = 1 s */
        0x01, /* Use IRQ */
    };
    uint8_t resp[8] = {0};
    size_t resp_len = sizeof(resp);

    return pn532_transceive(cmd, sizeof(cmd), PN532_CMD_SAM_CONFIGURATION + 1,
                            resp, &resp_len, s_nfc.config.command_timeout_ms);
}

esp_err_t nfc_reader_init(const nfc_reader_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    if (s_nfc.initialized) return ESP_OK;

    memcpy(&s_nfc.config, config, sizeof(s_nfc.config));
    if (s_nfc.config.i2c_addr == 0) {
        s_nfc.config.i2c_addr = PN532_I2C_ADDR_DEFAULT;
    }
    if (s_nfc.config.i2c_freq_hz <= 0) {
        s_nfc.config.i2c_freq_hz = 400000;
    }
    if (s_nfc.config.command_timeout_ms == 0) {
        s_nfc.config.command_timeout_ms = 1000;
    }

    ESP_LOGI(TAG, "Initializing PN532 NFC reader (I2C%d SDA:%d SCL:%d addr:0x%02X)",
             s_nfc.config.i2c_port, s_nfc.config.sda_pin,
             s_nfc.config.scl_pin, s_nfc.config.i2c_addr);

    i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = s_nfc.config.sda_pin,
        .scl_io_num = s_nfc.config.scl_pin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = s_nfc.config.i2c_freq_hz,
    };

    esp_err_t ret = i2c_param_config(s_nfc.config.i2c_port, &i2c_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(s_nfc.config.i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_nfc.config.irq_gpio >= 0) {
        gpio_config_t irq_conf = {
            .pin_bit_mask = (1ULL << s_nfc.config.irq_gpio),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&irq_conf);
    }

    pn532_reset();

    nfc_firmware_t fw = {0};
    ret = nfc_reader_get_firmware(&fw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PN532 firmware read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = pn532_sam_configuration();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PN532 SAM configuration failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_nfc.initialized = true;
    s_nfc.ready = true;
    ESP_LOGI(TAG, "PN532 ready (IC=0x%02X, version=%d.%d, support=0x%02X)",
             fw.ic, fw.version, fw.revision, fw.support);
    return ESP_OK;
}

esp_err_t nfc_reader_deinit(void)
{
    if (!s_nfc.initialized) return ESP_ERR_INVALID_STATE;

    s_nfc.initialized = false;
    s_nfc.ready = false;
    return ESP_OK;
}

bool nfc_reader_is_ready(void)
{
    return s_nfc.initialized && s_nfc.ready;
}

esp_err_t nfc_reader_get_firmware(nfc_firmware_t *firmware)
{
    if (firmware == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t cmd[] = { PN532_CMD_GET_FIRMWARE_VERSION };
    uint8_t resp[8] = {0};
    size_t resp_len = sizeof(resp);

    esp_err_t ret = pn532_transceive(cmd, sizeof(cmd),
                                     PN532_CMD_GET_FIRMWARE_VERSION + 1,
                                     resp, &resp_len,
                                     s_nfc.config.command_timeout_ms);
    if (ret != ESP_OK) return ret;
    if (resp_len < 5) return ESP_FAIL;

    firmware->ic = resp[1];
    firmware->version = resp[2];
    firmware->revision = resp[3];
    firmware->support = resp[4];
    firmware->chip = ((uint32_t)firmware->ic << 24) |
                     ((uint32_t)firmware->version << 16) |
                     ((uint32_t)firmware->revision << 8) |
                     firmware->support;
    return ESP_OK;
}

esp_err_t nfc_reader_poll(nfc_card_t *card)
{
    if (!s_nfc.initialized || card == NULL) return ESP_ERR_INVALID_STATE;

    uint8_t cmd[] = {
        PN532_CMD_IN_LIST_PASSIVE,
        0x01, /* Max one target */
        0x00, /* 106 kbps type A */
    };
    uint8_t resp[32] = {0};
    size_t resp_len = sizeof(resp);

    esp_err_t ret = pn532_transceive(cmd, sizeof(cmd),
                                     PN532_CMD_IN_LIST_PASSIVE + 1,
                                     resp, &resp_len,
                                     s_nfc.config.command_timeout_ms);
    if (ret == ESP_ERR_TIMEOUT) {
        return ESP_ERR_NOT_FOUND;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    if (resp_len < 7 || resp[1] == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t uid_len = resp[6];
    if (uid_len == 0 || uid_len > NFC_UID_MAX_LEN || resp_len < (size_t)(7 + uid_len)) {
        return ESP_FAIL;
    }

    memset(card, 0, sizeof(*card));
    card->atqa = ((uint16_t)resp[3] << 8) | resp[4];
    card->sak = resp[5];
    card->uid_len = uid_len;
    memcpy(card->uid, &resp[7], uid_len);
    return ESP_OK;
}

bool nfc_uid_equal(const uint8_t *a, uint8_t a_len,
                   const uint8_t *b, uint8_t b_len)
{
    if (a == NULL || b == NULL || a_len != b_len) return false;
    return memcmp(a, b, a_len) == 0;
}

void nfc_uid_to_hex(const uint8_t *uid, uint8_t uid_len,
                    char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";

    if (out == NULL || out_size == 0) return;
    out[0] = '\0';

    if (uid == NULL || uid_len == 0) return;

    size_t needed = (size_t)uid_len * 2 + 1;
    if (out_size < needed) {
        return;
    }

    for (uint8_t i = 0; i < uid_len; i++) {
        out[i * 2] = hex[(uid[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex[uid[i] & 0x0F];
    }
    out[uid_len * 2] = '\0';
}
