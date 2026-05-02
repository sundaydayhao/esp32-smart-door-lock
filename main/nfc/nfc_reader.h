#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Smart Door Lock NFC Reader
 * Supports PN532 in I2C mode for ISO14443A UID card detection.
 * ============================================================ */

#define NFC_UID_MAX_LEN              10
#define PN532_I2C_ADDR_DEFAULT       0x24

typedef struct {
    int      i2c_port;          /* I2C port number */
    int      sda_pin;           /* Shared I2C SDA pin */
    int      scl_pin;           /* Shared I2C SCL pin */
    int      i2c_freq_hz;       /* I2C bus speed */
    uint8_t  i2c_addr;          /* PN532 7-bit I2C address */
    int      reset_gpio;        /* PN532 RSTPDN pin (-1 if not used) */
    int      irq_gpio;          /* PN532 IRQ pin (-1 if not used) */
    uint32_t command_timeout_ms;/* PN532 command timeout */
} nfc_reader_config_t;

typedef struct {
    uint8_t uid[NFC_UID_MAX_LEN];
    uint8_t uid_len;
    uint16_t atqa;
    uint8_t sak;
} nfc_card_t;

typedef struct {
    uint32_t chip;
    uint8_t  ic;
    uint8_t  version;
    uint8_t  revision;
    uint8_t  support;
} nfc_firmware_t;

esp_err_t nfc_reader_init(const nfc_reader_config_t *config);
esp_err_t nfc_reader_deinit(void);
bool nfc_reader_is_ready(void);

esp_err_t nfc_reader_get_firmware(nfc_firmware_t *firmware);
esp_err_t nfc_reader_poll(nfc_card_t *card);

bool nfc_uid_equal(const uint8_t *a, uint8_t a_len,
                   const uint8_t *b, uint8_t b_len);
void nfc_uid_to_hex(const uint8_t *uid, uint8_t uid_len,
                    char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
