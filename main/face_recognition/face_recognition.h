#pragma once

#include "esp_err.h"
#include "esp_camera.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Smart Door Lock Face Recognition Module
 * Uses ESP32-S3 camera with face detection and recognition
 * Based on ESP-WHO framework concepts
 * ============================================================ */

/* Face recognition parameters */
#define FACE_FEAT_LEN           128         /* Feature vector length */
#define FACE_MAX_ENROLLED       10          /* Max enrolled faces */
#define FACE_DETECT_THRESHOLD   0.5f        /* Face detection confidence threshold */
#define FACE_RECOG_THRESHOLD    0.7f        /* Face recognition similarity threshold */
#define FACE_ENROLL_FRAMES      5           /* Number of frames to average for enrollment */

/* Face detection result */
typedef struct {
    int     x;              /* Bounding box left */
    int     y;              /* Bounding box top */
    int     w;              /* Bounding box width */
    int     h;              /* Bounding box height */
    float   confidence;     /* Detection confidence (0.0-1.0) */
    float   landmarks[10];  /* 5 facial landmarks (x,y pairs) */
} face_box_t;

/* Face recognition result */
typedef struct {
    bool    face_detected;      /* Whether a face was detected */
    bool    face_recognized;    /* Whether the face was recognized */
    int     user_id;            /* Recognized user ID (-1 if not recognized) */
    float   similarity;         /* Similarity score (0.0-1.0) */
    face_box_t box;             /* Face bounding box */
    float   features[FACE_FEAT_LEN]; /* Extracted face features */
} face_result_t;

/* Enrollment state */
typedef enum {
    FACE_ENROLL_IDLE = 0,       /* Not enrolling */
    FACE_ENROLL_COLLECTING,     /* Collecting face samples */
    FACE_ENROLL_PROCESSING,     /* Processing collected samples */
    FACE_ENROLL_DONE,           /* Enrollment complete */
    FACE_ENROLL_FAILED,         /* Enrollment failed */
} face_enroll_state_t;

/* Camera configuration */
typedef struct {
    int pin_pwdn;       /* Power down pin (-1 if not used) */
    int pin_reset;      /* Reset pin (-1 if not used) */
    int pin_xclk;       /* External clock pin */
    int pin_siod;       /* SCCB data pin */
    int pin_sioc;       /* SCCB clock pin */
    int pin_d7;         /* Data pin D7 */
    int pin_d6;         /* Data pin D6 */
    int pin_d5;         /* Data pin D5 */
    int pin_d4;         /* Data pin D4 */
    int pin_d3;         /* Data pin D3 */
    int pin_d2;         /* Data pin D2 */
    int pin_d1;         /* Data pin D1 */
    int pin_d0;         /* Data pin D0 */
    int pin_vsync;      /* VSYNC pin */
    int pin_href;       /* HREF pin */
    int pin_pclk;       /* Pixel clock pin */
    int xclk_freq_hz;   /* XCLK frequency (default 20MHz) */
} face_camera_config_t;

/* Enrolled face database entry */
typedef struct {
    uint8_t  user_id;                       /* Associated user ID */
    float    features[FACE_FEAT_LEN];       /* Face feature vector */
    bool     valid;                         /* Whether this entry is valid */
} face_db_entry_t;

/**
 * @brief Initialize face recognition module
 * @param cam_config Camera pin configuration
 * @return ESP_OK on success
 */
esp_err_t face_recog_init(const face_camera_config_t *cam_config);

/**
 * @brief Deinitialize face recognition module
 * @return ESP_OK on success
 */
esp_err_t face_recog_deinit(void);

/**
 * @brief Capture a frame and run face detection + recognition
 * @param result Output recognition result
 * @return ESP_OK on success (even if no face found)
 */
esp_err_t face_recog_run(face_result_t *result);

/**
 * @brief Start face enrollment for a user
 * @param user_id User ID to enroll
 * @return ESP_OK on success
 */
esp_err_t face_recog_enroll_start(uint8_t user_id);

/**
 * @brief Process one enrollment frame
 * Call repeatedly until state is FACE_ENROLL_DONE or FACE_ENROLL_FAILED
 * @param state Output enrollment state
 * @param progress Output progress (0-100)
 * @return ESP_OK on success
 */
esp_err_t face_recog_enroll_step(face_enroll_state_t *state, uint8_t *progress);

/**
 * @brief Cancel ongoing enrollment
 */
void face_recog_enroll_cancel(void);

/**
 * @brief Delete enrolled face for a user
 * @param user_id User ID
 * @return ESP_OK on success
 */
esp_err_t face_recog_delete_user(uint8_t user_id);

/**
 * @brief Load face features from user management into local DB
 * @param user_id User ID
 * @param features Feature vector
 * @param feat_len Feature vector length
 * @return ESP_OK on success
 */
esp_err_t face_recog_load_features(uint8_t user_id, const float *features,
                                    size_t feat_len);

/**
 * @brief Get the number of enrolled faces
 * @return Number of enrolled faces
 */
int face_recog_get_enrolled_count(void);

/**
 * @brief Capture a raw camera frame
 * @return Camera frame buffer (must be returned with esp_camera_fb_return)
 */
camera_fb_t *face_recog_capture_frame(void);

/**
 * @brief Check if camera is initialized
 * @return true if camera is ready
 */
bool face_recog_is_camera_ready(void);

/**
 * @brief Set recognition threshold
 * @param threshold Similarity threshold (0.0-1.0)
 */
void face_recog_set_threshold(float threshold);

#ifdef __cplusplus
}
#endif