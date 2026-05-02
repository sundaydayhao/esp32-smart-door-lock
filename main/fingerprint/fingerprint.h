#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Smart Door Lock Fingerprint Module
 * Supports AS608/R307/R503 fingerprint sensors via UART
 * ============================================================ */

/* AS608 Protocol Constants */
#define FP_HEADER               0xEF01      /* Packet header */
#define FP_ADDR_DEFAULT         0xFFFFFFFF  /* Default device address */

/* Packet identifiers */
#define FP_PID_COMMAND          0x01        /* Command packet */
#define FP_PID_DATA             0x02        /* Data packet */
#define FP_PID_ACK              0x07        /* Acknowledge packet */
#define FP_PID_END_DATA         0x08        /* End of data packet */

/* Instruction codes */
#define FP_CMD_GET_IMAGE        0x01        /* Collect finger image */
#define FP_CMD_IMG_TO_TZ1       0x02        /* Generate char file from image to buffer 1 */
#define FP_CMD_IMG_TO_TZ2       0x03        /* Generate char file from image to buffer 2 */
#define FP_CMD_SEARCH           0x04        /* Search fingerprint library */
#define FP_CMD_REG_MODEL        0x05        /* Combine char files and generate template */
#define FP_CMD_STORE            0x06        /* Store template */
#define FP_CMD_LOAD_CHAR        0x07        /* Load template from library */
#define FP_CMD_UP_CHAR          0x08        /* Upload char file */
#define FP_CMD_DOWN_CHAR        0x09        /* Download char file */
#define FP_CMD_UP_IMAGE         0x0A        /* Upload image */
#define FP_CMD_DOWN_IMAGE       0x0B        /* Download image */
#define FP_CMD_DELETE_CHAR      0x0C        /* Delete template */
#define FP_CMD_EMPTY            0x0D        /* Empty library */
#define FP_CMD_SET_SYS_PARA     0x0E        /* Set system parameter */
#define FP_CMD_READ_SYS_PARA    0x0F        /* Read system parameter */
#define FP_CMD_SET_PWD          0x12        /* Set password */
#define FP_CMD_VERIFY_PWD       0x13        /* Verify password */
#define FP_CMD_HIGH_SPEED_SRCH  0x1B        /* High speed search */
#define FP_CMD_GEN_BIN_IMAGE    0x29        /* Generate binary image */
#define FP_CMD_VALID_TMPL_NUM   0x1D        /* Read valid template number */
#define FP_CMD_HAND_ID_INIT     0x40        /* Handshake */
#define FP_CMD_CHECK_SENSOR     0x36        /* Check sensor */
#define FP_CMD_GET_ALGO_VER     0x39        /* Get algorithm library version */
#define FP_CMD_GET_FW_VER       0x3A        /* Get firmware version */

/* Confirmation codes (response status) */
#define FP_OK                   0x00        /* Command execution complete */
#define FP_ERR_RECV_PKG         0x01        /* Error receiving data package */
#define FP_ERR_NO_FINGER        0x02        /* No finger on sensor */
#define FP_ERR_ENROLL_FAIL      0x03        /* Failed to enroll finger */
#define FP_ERR_OVER_DISTOR      0x04        /* Failed to generate character file (too dry/wet) */
#define FP_ERR_FEATURE_FAIL     0x06        /* Failed to generate character file (disorderly) */
#define FP_ERR_NO_MATCH         0x08        /* Fingers do not match */
#define FP_ERR_NOT_FOUND        0x09        /* No matching finger in library */
#define FP_ERR_COMBINE_FAIL     0x0A        /* Failed to combine character files */
#define FP_ERR_BAD_LOCATION     0x0B        /* Addressing PageID is beyond the finger library */
#define FP_ERR_LOAD_TMPL        0x0C        /* Error reading template from library */
#define FP_ERR_UPLOAD_FEAT      0x0D        /* Error uploading template */
#define FP_ERR_PACKETRESP       0x0E        /* Module can't receive following data packages */
#define FP_ERR_UPLOAD_IMG       0x0F        /* Error uploading image */
#define FP_ERR_DEL_TMPL         0x10        /* Failed to delete template */
#define FP_ERR_CLR_LIB          0x11        /* Failed to clear finger library */
#define FP_ERR_WRONG_PWD        0x13        /* Wrong password */
#define FP_ERR_INVALID_IMG      0x15        /* Failed to generate image (lack of valid primary image) */
#define FP_ERR_FLASH_ERR        0x18        /* Error writing flash */
#define FP_ERR_NO_DEF_ERR       0x19        /* No definition error */
#define FP_ERR_INVALID_REG      0x1A        /* Invalid register number */
#define FP_ERR_INCORRECT_CFG    0x1B        /* Incorrect configuration of register */
#define FP_ERR_WRONG_NOTEPAD    0x1C        /* Wrong notepad page number */
#define FP_ERR_COMM_PORT        0x1D        /* Failed to operate communication port */

/* Fingerprint library capacity */
#define FP_LIBRARY_SIZE         127         /* Maximum templates in library */

/* Buffer IDs */
#define FP_BUFFER_1             0x01
#define FP_BUFFER_2             0x02

/* Fingerprint module state */
typedef enum {
    FP_STATE_IDLE = 0,          /* Idle, waiting for command */
    FP_STATE_SCANNING,          /* Scanning finger */
    FP_STATE_ENROLLING,         /* Enrolling new finger */
    FP_STATE_VERIFYING,         /* Verifying finger */
    FP_STATE_ERROR,             /* Error state */
} fp_state_t;

/* Enrollment step */
typedef enum {
    FP_ENROLL_STEP_1 = 0,       /* First scan */
    FP_ENROLL_STEP_2,           /* Second scan */
    FP_ENROLL_STEP_DONE,        /* Enrollment complete */
} fp_enroll_step_t;

/* System parameters */
typedef struct {
    uint16_t status_reg;        /* Status register */
    uint16_t system_id;         /* System identifier */
    uint16_t library_size;      /* Finger library size */
    uint16_t security_level;    /* Security level (1-5) */
    uint32_t device_addr;       /* Device address */
    uint16_t data_packet_size;  /* Data packet size */
    uint16_t baud_rate;         /* Baud rate setting */
} fp_sys_params_t;

/* Search result */
typedef struct {
    uint16_t page_id;           /* Template page ID (location in library) */
    uint16_t match_score;       /* Match confidence score */
} fp_search_result_t;

/* Fingerprint module configuration */
typedef struct {
    int      uart_num;          /* UART port number */
    int      uart_tx_pin;       /* UART TX pin */
    int      uart_rx_pin;       /* UART RX pin */
    uint32_t baud_rate;         /* UART baud rate (default 57600) */
    uint32_t device_addr;       /* Device address (default 0xFFFFFFFF) */
    uint32_t password;          /* Device password (default 0x00000000) */
} fp_config_t;

/**
 * @brief Initialize fingerprint sensor module
 * @param config Pointer to configuration structure
 * @return ESP_OK on success
 */
esp_err_t fingerprint_init(const fp_config_t *config);

/**
 * @brief Deinitialize fingerprint sensor module
 * @return ESP_OK on success
 */
esp_err_t fingerprint_deinit(void);

/**
 * @brief Verify password with sensor
 * @return ESP_OK if password correct
 */
esp_err_t fingerprint_verify_password(void);

/**
 * @brief Read system parameters from sensor
 * @param params Output system parameters
 * @return ESP_OK on success
 */
esp_err_t fingerprint_read_params(fp_sys_params_t *params);

/**
 * @brief Get number of stored templates
 * @param count Output template count
 * @return ESP_OK on success
 */
esp_err_t fingerprint_get_template_count(uint16_t *count);

/**
 * @brief Capture finger image
 * @return ESP_OK if image captured, FP_ERR_NO_FINGER if no finger
 */
esp_err_t fingerprint_get_image(void);

/**
 * @brief Convert image to feature template in buffer
 * @param buffer_id Buffer ID (FP_BUFFER_1 or FP_BUFFER_2)
 * @return ESP_OK on success
 */
esp_err_t fingerprint_image_to_template(uint8_t buffer_id);

/**
 * @brief Create model from two buffers
 * @return ESP_OK on success
 */
esp_err_t fingerprint_create_model(void);

/**
 * @brief Store model to library
 * @param page_id Location in library (0 to library_size-1)
 * @param buffer_id Buffer to store from
 * @return ESP_OK on success
 */
esp_err_t fingerprint_store_model(uint16_t page_id, uint8_t buffer_id);

/**
 * @brief Delete template from library
 * @param page_id Template location
 * @param count Number of templates to delete
 * @return ESP_OK on success
 */
esp_err_t fingerprint_delete_model(uint16_t page_id, uint16_t count);

/**
 * @brief Empty the entire fingerprint library
 * @return ESP_OK on success
 */
esp_err_t fingerprint_empty_library(void);

/**
 * @brief Search fingerprint library for a match
 * @param buffer_id Buffer containing the feature to search
 * @param start_page Start page for search
 * @param page_count Number of pages to search
 * @param result Output search result
 * @return ESP_OK if match found, ESP_ERR_NOT_FOUND if no match
 */
esp_err_t fingerprint_search(uint8_t buffer_id, uint16_t start_page,
                              uint16_t page_count, fp_search_result_t *result);

/**
 * @brief High-level: Enroll a new fingerprint
 * Performs the complete enrollment process (2 scans required)
 * @param page_id Location to store the template
 * @param step Current enrollment step (updated by function)
 * @return ESP_OK when enrollment complete, ESP_ERR_NOT_FINISHED if more steps needed
 */
esp_err_t fingerprint_enroll(uint16_t page_id, fp_enroll_step_t *step);

/**
 * @brief High-level: Verify a fingerprint against the library
 * Captures image and searches the entire library
 * @param result Output search result (page_id and score)
 * @return ESP_OK if match found, ESP_ERR_NOT_FOUND if no match
 */
esp_err_t fingerprint_verify(fp_search_result_t *result);

/**
 * @brief Get current module state
 * @return Current state
 */
fp_state_t fingerprint_get_state(void);

/**
 * @brief Check if finger is present on sensor
 * @return true if finger detected
 */
bool fingerprint_is_finger_present(void);

#ifdef __cplusplus
}
#endif