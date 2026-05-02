/**
 * @file fingerprint.c
 * @brief Smart Door Lock Fingerprint Module Implementation
 *
 * Implements AS608/R307/R503 fingerprint sensor communication
 * over UART using the standard AS608 protocol.
 *
 * Protocol packet format:
 *   [Header:2][Address:4][PID:1][Length:2][Data:N][Checksum:2]
 */

#include "fingerprint.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "FINGERPRINT";

/* UART buffer sizes */
#define UART_BUF_SIZE           1024
#define UART_RX_TIMEOUT_MS      2000
#define UART_CMD_TIMEOUT_MS     5000

/* Packet structure sizes */
#define PKT_HEADER_SIZE         2
#define PKT_ADDR_SIZE           4
#define PKT_PID_SIZE            1
#define PKT_LEN_SIZE            2
#define PKT_CHKSUM_SIZE         2
#define PKT_OVERHEAD            (PKT_HEADER_SIZE + PKT_ADDR_SIZE + PKT_PID_SIZE + PKT_LEN_SIZE + PKT_CHKSUM_SIZE)

/* Maximum packet data size */
#define PKT_MAX_DATA_SIZE       256
#define PKT_MAX_SIZE            (PKT_OVERHEAD + PKT_MAX_DATA_SIZE)

/* Module state */
static struct {
    int              uart_num;
    uint32_t         device_addr;
    uint32_t         password;
    fp_state_t       state;
    bool             initialized;
    SemaphoreHandle_t mutex;
} s_fp = {
    .uart_num    = -1,
    .device_addr = FP_ADDR_DEFAULT,
    .password    = 0x00000000,
    .state       = FP_STATE_IDLE,
    .initialized = false,
    .mutex       = NULL,
};

/* ============================================================
 * Private: Packet building and parsing
 * ============================================================ */

/**
 * @brief Build a command packet
 * @param buf Output buffer
 * @param buf_size Buffer size
 * @param pid Packet identifier
 * @param data Command data
 * @param data_len Data length
 * @return Total packet length
 */
static int build_packet(uint8_t *buf, size_t buf_size, uint8_t pid,
                         const uint8_t *data, uint16_t data_len)
{
    if (buf_size < (size_t)(PKT_OVERHEAD + data_len)) {
        return -1;
    }

    uint16_t length = data_len + PKT_CHKSUM_SIZE;
    int idx = 0;

    /* Header */
    buf[idx++] = (FP_HEADER >> 8) & 0xFF;
    buf[idx++] = FP_HEADER & 0xFF;

    /* Address */
    buf[idx++] = (s_fp.device_addr >> 24) & 0xFF;
    buf[idx++] = (s_fp.device_addr >> 16) & 0xFF;
    buf[idx++] = (s_fp.device_addr >> 8) & 0xFF;
    buf[idx++] = s_fp.device_addr & 0xFF;

    /* Packet ID */
    buf[idx++] = pid;

    /* Length (data + checksum) */
    buf[idx++] = (length >> 8) & 0xFF;
    buf[idx++] = length & 0xFF;

    /* Data */
    memcpy(&buf[idx], data, data_len);
    idx += data_len;

    /* Checksum: sum of PID + Length + Data */
    uint16_t checksum = pid + (length >> 8) + (length & 0xFF);
    for (int i = 0; i < data_len; i++) {
        checksum += data[i];
    }
    buf[idx++] = (checksum >> 8) & 0xFF;
    buf[idx++] = checksum & 0xFF;

    return idx;
}

/**
 * @brief Send a command and receive response
 * @param cmd Command byte
 * @param params Additional parameters
 * @param params_len Parameters length
 * @param resp_data Output response data buffer
 * @param resp_len Output response data length
 * @return Confirmation code (FP_OK on success)
 */
static uint8_t send_command(uint8_t cmd, const uint8_t *params,
                              uint16_t params_len, uint8_t *resp_data,
                              uint16_t *resp_len)
{
    uint8_t pkt_buf[PKT_MAX_SIZE];
    uint8_t cmd_data[PKT_MAX_DATA_SIZE];

    /* Build command data: [CMD][PARAMS...] */
    cmd_data[0] = cmd;
    if (params && params_len > 0) {
        memcpy(&cmd_data[1], params, params_len);
    }

    /* Build packet */
    int pkt_len = build_packet(pkt_buf, sizeof(pkt_buf), FP_PID_COMMAND,
                                cmd_data, 1 + params_len);
    if (pkt_len < 0) {
        ESP_LOGE(TAG, "Failed to build packet");
        return 0xFF;
    }

    /* Flush RX buffer */
    uart_flush_input(s_fp.uart_num);

    /* Send packet */
    int sent = uart_write_bytes(s_fp.uart_num, (const char *)pkt_buf, pkt_len);
    if (sent != pkt_len) {
        ESP_LOGE(TAG, "UART write failed: sent %d of %d bytes", sent, pkt_len);
        return 0xFF;
    }

    /* Wait for response */
    uint8_t resp_buf[PKT_MAX_SIZE];
    int recv_len = uart_read_bytes(s_fp.uart_num, resp_buf, sizeof(resp_buf),
                                    pdMS_TO_TICKS(UART_RX_TIMEOUT_MS));
    if (recv_len < (int)PKT_OVERHEAD) {
        ESP_LOGW(TAG, "No response or too short (%d bytes)", recv_len);
        return 0xFF;
    }

    /* Validate header */
    if (resp_buf[0] != ((FP_HEADER >> 8) & 0xFF) ||
        resp_buf[1] != (FP_HEADER & 0xFF)) {
        ESP_LOGE(TAG, "Invalid response header: 0x%02X%02X",
                 resp_buf[0], resp_buf[1]);
        return 0xFF;
    }

    /* Validate PID */
    if (resp_buf[6] != FP_PID_ACK) {
        ESP_LOGE(TAG, "Unexpected PID: 0x%02X", resp_buf[6]);
        return 0xFF;
    }

    /* Extract length */
    uint16_t data_len = ((uint16_t)resp_buf[7] << 8) | resp_buf[8];
    data_len -= PKT_CHKSUM_SIZE; /* Subtract checksum from length */

    /* Extract confirmation code */
    uint8_t conf_code = resp_buf[9];

    /* Extract response data if requested */
    if (resp_data && resp_len && data_len > 1) {
        *resp_len = data_len - 1;
        memcpy(resp_data, &resp_buf[10], *resp_len);
    } else if (resp_len) {
        *resp_len = 0;
    }

    ESP_LOGD(TAG, "CMD 0x%02X -> conf_code 0x%02X", cmd, conf_code);
    return conf_code;
}

/* ============================================================
 * Public API implementation
 * ============================================================ */

esp_err_t fingerprint_init(const fp_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Initializing fingerprint sensor on UART%d (TX:%d, RX:%d)",
             config->uart_num, config->uart_tx_pin, config->uart_rx_pin);

    /* Create mutex */
    s_fp.mutex = xSemaphoreCreateMutex();
    if (s_fp.mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Configure UART */
    uart_config_t uart_config = {
        .baud_rate  = config->baud_rate > 0 ? config->baud_rate : 57600,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_driver_install(config->uart_num,
                                         UART_BUF_SIZE * 2,
                                         UART_BUF_SIZE * 2,
                                         0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_param_config(config->uart_num, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure UART: %s", esp_err_to_name(ret));
        uart_driver_delete(config->uart_num);
        return ret;
    }

    ret = uart_set_pin(config->uart_num,
                        config->uart_tx_pin,
                        config->uart_rx_pin,
                        UART_PIN_NO_CHANGE,
                        UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UART pins: %s", esp_err_to_name(ret));
        uart_driver_delete(config->uart_num);
        return ret;
    }

    s_fp.uart_num    = config->uart_num;
    s_fp.device_addr = config->device_addr ? config->device_addr : FP_ADDR_DEFAULT;
    s_fp.password    = config->password;
    s_fp.state       = FP_STATE_IDLE;

    /* Give sensor time to boot */
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Verify password */
    ret = fingerprint_verify_password();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Fingerprint sensor not responding or wrong password");
        uart_driver_delete(config->uart_num);
        return ret;
    }

    s_fp.initialized = true;
    ESP_LOGI(TAG, "Fingerprint sensor initialized successfully");
    return ESP_OK;
}

esp_err_t fingerprint_deinit(void)
{
    if (!s_fp.initialized) return ESP_ERR_INVALID_STATE;

    uart_driver_delete(s_fp.uart_num);
    if (s_fp.mutex) {
        vSemaphoreDelete(s_fp.mutex);
        s_fp.mutex = NULL;
    }
    s_fp.initialized = false;
    s_fp.uart_num = -1;
    return ESP_OK;
}

esp_err_t fingerprint_verify_password(void)
{
    uint8_t params[4] = {
        (s_fp.password >> 24) & 0xFF,
        (s_fp.password >> 16) & 0xFF,
        (s_fp.password >> 8) & 0xFF,
        s_fp.password & 0xFF,
    };

    uint8_t conf = send_command(FP_CMD_VERIFY_PWD, params, 4, NULL, NULL);
    if (conf == FP_OK) {
        ESP_LOGI(TAG, "Password verified successfully");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Password verification failed: 0x%02X", conf);
    return ESP_FAIL;
}

esp_err_t fingerprint_read_params(fp_sys_params_t *params)
{
    if (!s_fp.initialized || params == NULL) return ESP_ERR_INVALID_STATE;

    uint8_t resp[16];
    uint16_t resp_len = 0;
    uint8_t conf = send_command(FP_CMD_READ_SYS_PARA, NULL, 0, resp, &resp_len);

    if (conf != FP_OK || resp_len < 16) {
        return ESP_FAIL;
    }

    params->status_reg       = ((uint16_t)resp[0] << 8) | resp[1];
    params->system_id        = ((uint16_t)resp[2] << 8) | resp[3];
    params->library_size     = ((uint16_t)resp[4] << 8) | resp[5];
    params->security_level   = ((uint16_t)resp[6] << 8) | resp[7];
    params->device_addr      = ((uint32_t)resp[8] << 24) | ((uint32_t)resp[9] << 16) |
                               ((uint32_t)resp[10] << 8) | resp[11];
    params->data_packet_size = ((uint16_t)resp[12] << 8) | resp[13];
    params->baud_rate        = ((uint16_t)resp[14] << 8) | resp[15];

    ESP_LOGI(TAG, "Library size: %d, Security level: %d",
             params->library_size, params->security_level);
    return ESP_OK;
}

esp_err_t fingerprint_get_template_count(uint16_t *count)
{
    if (!s_fp.initialized || count == NULL) return ESP_ERR_INVALID_STATE;

    uint8_t resp[2];
    uint16_t resp_len = 0;
    uint8_t conf = send_command(FP_CMD_VALID_TMPL_NUM, NULL, 0, resp, &resp_len);

    if (conf != FP_OK || resp_len < 2) {
        return ESP_FAIL;
    }

    *count = ((uint16_t)resp[0] << 8) | resp[1];
    ESP_LOGI(TAG, "Template count: %d", *count);
    return ESP_OK;
}

esp_err_t fingerprint_get_image(void)
{
    if (!s_fp.initialized) return ESP_ERR_INVALID_STATE;

    uint8_t conf = send_command(FP_CMD_GET_IMAGE, NULL, 0, NULL, NULL);
    switch (conf) {
        case FP_OK:
            ESP_LOGD(TAG, "Finger image captured");
            return ESP_OK;
        case FP_ERR_NO_FINGER:
            return ESP_ERR_NOT_FOUND;
        default:
            ESP_LOGW(TAG, "Get image failed: 0x%02X", conf);
            return ESP_FAIL;
    }
}

esp_err_t fingerprint_image_to_template(uint8_t buffer_id)
{
    if (!s_fp.initialized) return ESP_ERR_INVALID_STATE;

    uint8_t cmd = (buffer_id == FP_BUFFER_1) ? FP_CMD_IMG_TO_TZ1 : FP_CMD_IMG_TO_TZ2;
    uint8_t params[1] = { buffer_id };
    uint8_t conf = send_command(cmd, params, 1, NULL, NULL);

    if (conf == FP_OK) {
        ESP_LOGD(TAG, "Image converted to template in buffer %d", buffer_id);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Image to template failed: 0x%02X", conf);
    return ESP_FAIL;
}

esp_err_t fingerprint_create_model(void)
{
    if (!s_fp.initialized) return ESP_ERR_INVALID_STATE;

    uint8_t conf = send_command(FP_CMD_REG_MODEL, NULL, 0, NULL, NULL);
    if (conf == FP_OK) {
        ESP_LOGI(TAG, "Model created from buffers");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Create model failed: 0x%02X", conf);
    return ESP_FAIL;
}

esp_err_t fingerprint_store_model(uint16_t page_id, uint8_t buffer_id)
{
    if (!s_fp.initialized) return ESP_ERR_INVALID_STATE;

    uint8_t params[3] = {
        buffer_id,
        (page_id >> 8) & 0xFF,
        page_id & 0xFF,
    };

    uint8_t conf = send_command(FP_CMD_STORE, params, 3, NULL, NULL);
    if (conf == FP_OK) {
        ESP_LOGI(TAG, "Template stored at page %d", page_id);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Store model failed: 0x%02X", conf);
    return ESP_FAIL;
}

esp_err_t fingerprint_delete_model(uint16_t page_id, uint16_t count)
{
    if (!s_fp.initialized) return ESP_ERR_INVALID_STATE;

    uint8_t params[4] = {
        (page_id >> 8) & 0xFF,
        page_id & 0xFF,
        (count >> 8) & 0xFF,
        count & 0xFF,
    };

    uint8_t conf = send_command(FP_CMD_DELETE_CHAR, params, 4, NULL, NULL);
    if (conf == FP_OK) {
        ESP_LOGI(TAG, "Deleted %d template(s) starting at page %d", count, page_id);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Delete model failed: 0x%02X", conf);
    return ESP_FAIL;
}

esp_err_t fingerprint_empty_library(void)
{
    if (!s_fp.initialized) return ESP_ERR_INVALID_STATE;

    uint8_t conf = send_command(FP_CMD_EMPTY, NULL, 0, NULL, NULL);
    if (conf == FP_OK) {
        ESP_LOGI(TAG, "Fingerprint library cleared");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Empty library failed: 0x%02X", conf);
    return ESP_FAIL;
}

esp_err_t fingerprint_search(uint8_t buffer_id, uint16_t start_page,
                              uint16_t page_count, fp_search_result_t *result)
{
    if (!s_fp.initialized || result == NULL) return ESP_ERR_INVALID_STATE;

    uint8_t params[5] = {
        buffer_id,
        (start_page >> 8) & 0xFF,
        start_page & 0xFF,
        (page_count >> 8) & 0xFF,
        page_count & 0xFF,
    };

    uint8_t resp[4];
    uint16_t resp_len = 0;
    uint8_t conf = send_command(FP_CMD_SEARCH, params, 5, resp, &resp_len);

    if (conf == FP_OK && resp_len >= 4) {
        result->page_id     = ((uint16_t)resp[0] << 8) | resp[1];
        result->match_score = ((uint16_t)resp[2] << 8) | resp[3];
        ESP_LOGI(TAG, "Match found: page=%d, score=%d",
                 result->page_id, result->match_score);
        return ESP_OK;
    }

    if (conf == FP_ERR_NOT_FOUND) {
        ESP_LOGD(TAG, "No matching fingerprint found");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGW(TAG, "Search failed: 0x%02X", conf);
    return ESP_FAIL;
}

esp_err_t fingerprint_enroll(uint16_t page_id, fp_enroll_step_t *step)
{
    if (!s_fp.initialized || step == NULL) return ESP_ERR_INVALID_STATE;

    esp_err_t ret;

    switch (*step) {
        case FP_ENROLL_STEP_1:
            ESP_LOGI(TAG, "Enrollment step 1: Place finger on sensor");
            s_fp.state = FP_STATE_ENROLLING;

            /* Capture first image */
            ret = fingerprint_get_image();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Step 1: No finger detected");
                return ret;
            }

            /* Convert to template in buffer 1 */
            ret = fingerprint_image_to_template(FP_BUFFER_1);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Step 1: Failed to process image");
                return ret;
            }

            *step = FP_ENROLL_STEP_2;
            ESP_LOGI(TAG, "Step 1 complete. Remove finger and place again.");
            return ESP_ERR_NOT_FINISHED;

        case FP_ENROLL_STEP_2:
            ESP_LOGI(TAG, "Enrollment step 2: Place same finger again");

            /* Capture second image */
            ret = fingerprint_get_image();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Step 2: No finger detected");
                return ret;
            }

            /* Convert to template in buffer 2 */
            ret = fingerprint_image_to_template(FP_BUFFER_2);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Step 2: Failed to process image");
                return ret;
            }

            /* Create model from both buffers */
            ret = fingerprint_create_model();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to create model - fingers may not match");
                *step = FP_ENROLL_STEP_1; /* Reset enrollment */
                return ret;
            }

            /* Store model */
            ret = fingerprint_store_model(page_id, FP_BUFFER_1);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to store model at page %d", page_id);
                *step = FP_ENROLL_STEP_1;
                return ret;
            }

            *step = FP_ENROLL_STEP_DONE;
            s_fp.state = FP_STATE_IDLE;
            ESP_LOGI(TAG, "Enrollment complete! Template stored at page %d", page_id);
            return ESP_OK;

        default:
            return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t fingerprint_verify(fp_search_result_t *result)
{
    if (!s_fp.initialized || result == NULL) return ESP_ERR_INVALID_STATE;

    s_fp.state = FP_STATE_VERIFYING;

    /* Capture image */
    esp_err_t ret = fingerprint_get_image();
    if (ret != ESP_OK) {
        s_fp.state = FP_STATE_IDLE;
        return ret;
    }

    /* Convert to template */
    ret = fingerprint_image_to_template(FP_BUFFER_1);
    if (ret != ESP_OK) {
        s_fp.state = FP_STATE_IDLE;
        return ret;
    }

    /* Search entire library */
    ret = fingerprint_search(FP_BUFFER_1, 0, FP_LIBRARY_SIZE, result);
    s_fp.state = FP_STATE_IDLE;
    return ret;
}

fp_state_t fingerprint_get_state(void)
{
    return s_fp.state;
}

bool fingerprint_is_finger_present(void)
{
    if (!s_fp.initialized) return false;

    uint8_t conf = send_command(FP_CMD_GET_IMAGE, NULL, 0, NULL, NULL);
    return (conf == FP_OK);
}