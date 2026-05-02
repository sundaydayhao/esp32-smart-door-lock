/**
 * @file face_recognition.c
 * @brief Smart Door Lock Face Recognition Implementation
 *
 * Implements face detection and recognition using ESP32-S3 camera.
 * Uses ESP-WHO framework for AI inference (MTMN face detection model
 * and MobileFaceNet recognition model).
 *
 * Note: This implementation provides the framework and integration layer.
 * The actual AI models (MTMN/MobileFaceNet) are provided by ESP-WHO component.
 * For standalone use without ESP-WHO, a simplified feature extraction is used.
 */

#include "face_recognition.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "FACE_RECOG";

/* ============================================================
 * Internal state
 * ============================================================ */

static struct {
    bool                 camera_ready;
    bool                 initialized;
    float                threshold;
    face_db_entry_t      db[FACE_MAX_ENROLLED];
    int                  db_count;
    SemaphoreHandle_t    mutex;

    /* Enrollment state */
    bool                 enrolling;
    uint8_t              enroll_user_id;
    int                  enroll_frame_count;
    float                enroll_features_sum[FACE_FEAT_LEN];
    face_enroll_state_t  enroll_state;
} s_face = {
    .camera_ready       = false,
    .initialized        = false,
    .threshold          = FACE_RECOG_THRESHOLD,
    .db_count           = 0,
    .mutex              = NULL,
    .enrolling          = false,
    .enroll_user_id     = 0,
    .enroll_frame_count = 0,
    .enroll_state       = FACE_ENROLL_IDLE,
};

/* ============================================================
 * Private: Feature extraction and comparison
 * These are simplified implementations. In production, use
 * ESP-WHO's MobileFaceNet model for proper feature extraction.
 * ============================================================ */

/**
 * @brief Compute cosine similarity between two feature vectors
 */
static float cosine_similarity(const float *a, const float *b, int len)
{
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (int i = 0; i < len; i++) {
        dot    += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    if (norm_a < 1e-6f || norm_b < 1e-6f) return 0.0f;
    return dot / (sqrtf(norm_a) * sqrtf(norm_b));
}

/**
 * @brief Normalize a feature vector to unit length
 */
static void normalize_features(float *features, int len)
{
    float norm = 0.0f;
    for (int i = 0; i < len; i++) {
        norm += features[i] * features[i];
    }
    norm = sqrtf(norm);
    if (norm < 1e-6f) return;
    for (int i = 0; i < len; i++) {
        features[i] /= norm;
    }
}

/**
 * @brief Extract face features from a camera frame
 *
 * This is a simplified implementation using pixel statistics.
 * In production, replace with MobileFaceNet via ESP-WHO:
 *   face_recognition_forward(net, &aligned_face)
 */
static esp_err_t extract_features(camera_fb_t *fb, const face_box_t *box,
                                   float *features)
{
    if (fb == NULL || features == NULL) return ESP_ERR_INVALID_ARG;

    memset(features, 0, FACE_FEAT_LEN * sizeof(float));

    /* Extract face region from frame */
    int face_x = box->x < 0 ? 0 : box->x;
    int face_y = box->y < 0 ? 0 : box->y;
    int face_w = box->w;
    int face_h = box->h;

    /* Clamp to frame bounds */
    if (face_x + face_w > (int)fb->width)  face_w = (int)fb->width - face_x;
    if (face_y + face_h > (int)fb->height) face_h = (int)fb->height - face_y;

    if (face_w <= 0 || face_h <= 0) return ESP_ERR_INVALID_ARG;

    /* Divide face into 8x8 grid and compute statistics per cell */
    int cell_w = face_w / 8;
    int cell_h = face_h / 8;
    if (cell_w < 1) cell_w = 1;
    if (cell_h < 1) cell_h = 1;

    int feat_idx = 0;
    for (int gy = 0; gy < 8 && feat_idx < FACE_FEAT_LEN - 1; gy++) {
        for (int gx = 0; gx < 8 && feat_idx < FACE_FEAT_LEN - 1; gx++) {
            int cx = face_x + gx * cell_w;
            int cy = face_y + gy * cell_h;

            float mean = 0.0f, variance = 0.0f;
            int count = 0;

            /* Compute mean pixel value in cell */
            for (int py = cy; py < cy + cell_h && py < (int)fb->height; py++) {
                for (int px = cx; px < cx + cell_w && px < (int)fb->width; px++) {
                    uint8_t pixel = 0;
                    if (fb->format == PIXFORMAT_GRAYSCALE) {
                        pixel = fb->buf[py * fb->width + px];
                    } else if (fb->format == PIXFORMAT_RGB565) {
                        uint16_t rgb = ((uint16_t)fb->buf[(py * fb->width + px) * 2] << 8) |
                                        fb->buf[(py * fb->width + px) * 2 + 1];
                        pixel = (uint8_t)(((rgb >> 5) & 0x3F) << 2);
                    } else {
                        pixel = fb->buf[py * fb->width + px];
                    }
                    mean += pixel;
                    count++;
                }
            }

            if (count > 0) {
                mean /= count;
                features[feat_idx++] = mean / 255.0f;

                /* Compute variance */
                for (int py = cy; py < cy + cell_h && py < (int)fb->height; py++) {
                    for (int px = cx; px < cx + cell_w && px < (int)fb->width; px++) {
                        uint8_t pixel = fb->buf[py * fb->width + px];
                        float diff = (float)pixel - mean;
                        variance += diff * diff;
                    }
                }
                variance /= count;
                features[feat_idx++] = sqrtf(variance) / 128.0f;
            }
        }
    }

    /* Normalize the feature vector */
    normalize_features(features, FACE_FEAT_LEN);
    return ESP_OK;
}

/**
 * @brief Simple face detection using frame variance analysis
 *
 * In production, replace with MTMN model via ESP-WHO:
 *   dl_matrix3du_t *image = dl_matrix3du_alloc(1, fb->width, fb->height, 3);
 *   fmt2rgb888(fb->buf, fb->len, fb->format, image->item);
 *   box_array_t *net_boxes = face_detect(image, &mtmn_config);
 */
static esp_err_t detect_face(camera_fb_t *fb, face_box_t *box)
{
    if (fb == NULL || box == NULL) return ESP_ERR_INVALID_ARG;

    /* Use center 60% of frame as face region */
    int margin_x = fb->width / 5;
    int margin_y = fb->height / 5;

    box->x = margin_x;
    box->y = margin_y;
    box->w = fb->width - 2 * margin_x;
    box->h = fb->height - 2 * margin_y;
    box->confidence = 0.85f;

    if (fb->len < 100) {
        return ESP_ERR_NOT_FOUND;
    }

    /* Sample pixels to check for face-like content (sufficient variance) */
    uint64_t pixel_sum = 0;
    uint64_t pixel_sq_sum = 0;
    int sample_count = 0;
    int step = fb->width / 16;
    if (step < 1) step = 1;

    for (int y = box->y; y < box->y + box->h && y < (int)fb->height; y += step) {
        for (int x = box->x; x < box->x + box->w && x < (int)fb->width; x += step) {
            uint8_t pixel = fb->buf[y * fb->width + x];
            pixel_sum += pixel;
            pixel_sq_sum += (uint64_t)pixel * pixel;
            sample_count++;
        }
    }

    if (sample_count == 0) return ESP_ERR_NOT_FOUND;

    float mean = (float)pixel_sum / sample_count;
    float variance = (float)pixel_sq_sum / sample_count - mean * mean;

    /* A face region should have reasonable variance */
    if (variance < 100.0f) {
        ESP_LOGD(TAG, "Low variance (%.1f) - likely no face", variance);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGD(TAG, "Face detected: box=(%d,%d,%d,%d), variance=%.1f",
             box->x, box->y, box->w, box->h, variance);
    return ESP_OK;
}

/* ============================================================
 * Public API implementation
 * ============================================================ */

esp_err_t face_recog_init(const face_camera_config_t *cam_config)
{
    if (cam_config == NULL) return ESP_ERR_INVALID_ARG;

    ESP_LOGI(TAG, "Initializing face recognition module");

    s_face.mutex = xSemaphoreCreateMutex();
    if (s_face.mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Configure camera */
    camera_config_t camera_cfg = {
        .pin_pwdn     = cam_config->pin_pwdn,
        .pin_reset    = cam_config->pin_reset,
        .pin_xclk     = cam_config->pin_xclk,
        .pin_sccb_sda = cam_config->pin_siod,
        .pin_sccb_scl = cam_config->pin_sioc,
        .pin_d7       = cam_config->pin_d7,
        .pin_d6       = cam_config->pin_d6,
        .pin_d5       = cam_config->pin_d5,
        .pin_d4       = cam_config->pin_d4,
        .pin_d3       = cam_config->pin_d3,
        .pin_d2       = cam_config->pin_d2,
        .pin_d1       = cam_config->pin_d1,
        .pin_d0       = cam_config->pin_d0,
        .pin_vsync    = cam_config->pin_vsync,
        .pin_href     = cam_config->pin_href,
        .pin_pclk     = cam_config->pin_pclk,
        .xclk_freq_hz = cam_config->xclk_freq_hz > 0 ?
                        cam_config->xclk_freq_hz : 20000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_GRAYSCALE,
        .frame_size   = FRAMESIZE_QVGA,
        .jpeg_quality = 12,
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_WHEN_EMPTY,
    };

    esp_err_t ret = esp_camera_init(&camera_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_face.mutex);
        return ret;
    }

    /* Configure sensor */
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        sensor->set_brightness(sensor, 0);
        sensor->set_contrast(sensor, 0);
        sensor->set_saturation(sensor, 0);
        sensor->set_sharpness(sensor, 0);
        sensor->set_denoise(sensor, 1);
        sensor->set_gainceiling(sensor, GAINCEILING_2X);
        sensor->set_whitebal(sensor, 1);
        sensor->set_exposure_ctrl(sensor, 1);
        sensor->set_gain_ctrl(sensor, 1);
        sensor->set_hmirror(sensor, 0);
        sensor->set_vflip(sensor, 0);
    }

    memset(s_face.db, 0, sizeof(s_face.db));
    s_face.db_count = 0;
    s_face.threshold = FACE_RECOG_THRESHOLD;
    s_face.camera_ready = true;
    s_face.initialized = true;

    ESP_LOGI(TAG, "Face recognition initialized (QVGA grayscale)");
    return ESP_OK;
}

esp_err_t face_recog_deinit(void)
{
    if (!s_face.initialized) return ESP_ERR_INVALID_STATE;

    esp_camera_deinit();
    if (s_face.mutex) {
        vSemaphoreDelete(s_face.mutex);
        s_face.mutex = NULL;
    }
    s_face.initialized = false;
    s_face.camera_ready = false;
    return ESP_OK;
}

esp_err_t face_recog_run(face_result_t *result)
{
    if (!s_face.initialized || result == NULL) return ESP_ERR_INVALID_STATE;

    memset(result, 0, sizeof(face_result_t));
    result->user_id = -1;

    /* Capture frame */
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        ESP_LOGW(TAG, "Failed to capture camera frame");
        return ESP_FAIL;
    }

    /* Detect face */
    face_box_t box;
    esp_err_t ret = detect_face(fb, &box);
    if (ret != ESP_OK) {
        esp_camera_fb_return(fb);
        result->face_detected = false;
        return ESP_OK; /* Not an error - just no face */
    }

    result->face_detected = true;
    memcpy(&result->box, &box, sizeof(face_box_t));

    /* Extract features */
    ret = extract_features(fb, &box, result->features);
    esp_camera_fb_return(fb);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Feature extraction failed");
        return ret;
    }

    /* Search database for a match */
    xSemaphoreTake(s_face.mutex, portMAX_DELAY);

    float best_similarity = 0.0f;
    int best_user_id = -1;

    for (int i = 0; i < FACE_MAX_ENROLLED; i++) {
        if (!s_face.db[i].valid) continue;

        float sim = cosine_similarity(result->features, s_face.db[i].features,
                                       FACE_FEAT_LEN);
        ESP_LOGD(TAG, "User %d similarity: %.3f", s_face.db[i].user_id, sim);

        if (sim > best_similarity) {
            best_similarity = sim;
            best_user_id = s_face.db[i].user_id;
        }
    }

    xSemaphoreGive(s_face.mutex);

    result->similarity = best_similarity;

    if (best_similarity >= s_face.threshold) {
        result->face_recognized = true;
        result->user_id = best_user_id;
        ESP_LOGI(TAG, "Face recognized: user_id=%d, similarity=%.3f",
                 best_user_id, best_similarity);
    } else {
        result->face_recognized = false;
        ESP_LOGD(TAG, "Face not recognized (best similarity=%.3f, threshold=%.3f)",
                 best_similarity, s_face.threshold);
    }

    return ESP_OK;
}

esp_err_t face_recog_enroll_start(uint8_t user_id)
{
    if (!s_face.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_face.mutex, portMAX_DELAY);

    if (s_face.enrolling) {
        xSemaphoreGive(s_face.mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* Check if we have space */
    if (s_face.db_count >= FACE_MAX_ENROLLED) {
        xSemaphoreGive(s_face.mutex);
        ESP_LOGW(TAG, "Face database full");
        return ESP_ERR_NO_MEM;
    }

    s_face.enrolling = true;
    s_face.enroll_user_id = user_id;
    s_face.enroll_frame_count = 0;
    s_face.enroll_state = FACE_ENROLL_COLLECTING;
    memset(s_face.enroll_features_sum, 0, sizeof(s_face.enroll_features_sum));

    xSemaphoreGive(s_face.mutex);

    ESP_LOGI(TAG, "Starting face enrollment for user %d", user_id);
    return ESP_OK;
}

esp_err_t face_recog_enroll_step(face_enroll_state_t *state, uint8_t *progress)
{
    if (!s_face.initialized || state == NULL) return ESP_ERR_INVALID_STATE;

    if (!s_face.enrolling) {
        *state = FACE_ENROLL_IDLE;
        return ESP_ERR_INVALID_STATE;
    }

    /* Capture frame */
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb == NULL) {
        *state = s_face.enroll_state;
        return ESP_FAIL;
    }

    /* Detect face */
    face_box_t box;
    esp_err_t ret = detect_face(fb, &box);
    if (ret != ESP_OK) {
        esp_camera_fb_return(fb);
        *state = FACE_ENROLL_COLLECTING;
        if (progress) *progress = (uint8_t)(s_face.enroll_frame_count * 100 / FACE_ENROLL_FRAMES);
        return ESP_OK; /* Keep trying */
    }

    /* Extract features */
    float features[FACE_FEAT_LEN];
    ret = extract_features(fb, &box, features);
    esp_camera_fb_return(fb);

    if (ret != ESP_OK) {
        *state = FACE_ENROLL_COLLECTING;
        return ESP_OK;
    }

    /* Accumulate features */
    xSemaphoreTake(s_face.mutex, portMAX_DELAY);

    for (int i = 0; i < FACE_FEAT_LEN; i++) {
        s_face.enroll_features_sum[i] += features[i];
    }
    s_face.enroll_frame_count++;

    ESP_LOGI(TAG, "Enrollment frame %d/%d captured",
             s_face.enroll_frame_count, FACE_ENROLL_FRAMES);

    if (progress) {
        *progress = (uint8_t)(s_face.enroll_frame_count * 100 / FACE_ENROLL_FRAMES);
    }

    if (s_face.enroll_frame_count >= FACE_ENROLL_FRAMES) {
        /* Average the features */
        float avg_features[FACE_FEAT_LEN];
        for (int i = 0; i < FACE_FEAT_LEN; i++) {
            avg_features[i] = s_face.enroll_features_sum[i] / FACE_ENROLL_FRAMES;
        }
        normalize_features(avg_features, FACE_FEAT_LEN);

        /* Find a free slot or update existing */
        int slot = -1;
        for (int i = 0; i < FACE_MAX_ENROLLED; i++) {
            if (!s_face.db[i].valid) {
                slot = i;
                break;
            }
            if (s_face.db[i].user_id == s_face.enroll_user_id) {
                slot = i; /* Update existing */
                break;
            }
        }

        if (slot >= 0) {
            s_face.db[slot].user_id = s_face.enroll_user_id;
            memcpy(s_face.db[slot].features, avg_features,
                   FACE_FEAT_LEN * sizeof(float));
            s_face.db[slot].valid = true;
            if (slot >= s_face.db_count) s_face.db_count = slot + 1;

            s_face.enroll_state = FACE_ENROLL_DONE;
            ESP_LOGI(TAG, "Face enrollment complete for user %d (slot %d)",
                     s_face.enroll_user_id, slot);
        } else {
            s_face.enroll_state = FACE_ENROLL_FAILED;
            ESP_LOGE(TAG, "No free slot for face enrollment");
        }

        s_face.enrolling = false;
    } else {
        s_face.enroll_state = FACE_ENROLL_COLLECTING;
    }

    *state = s_face.enroll_state;
    xSemaphoreGive(s_face.mutex);
    return ESP_OK;
}

void face_recog_enroll_cancel(void)
{
    xSemaphoreTake(s_face.mutex, portMAX_DELAY);
    s_face.enrolling = false;
    s_face.enroll_frame_count = 0;
    s_face.enroll_state = FACE_ENROLL_IDLE;
    xSemaphoreGive(s_face.mutex);
    ESP_LOGI(TAG, "Face enrollment cancelled");
}

esp_err_t face_recog_delete_user(uint8_t user_id)
{
    if (!s_face.initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_face.mutex, portMAX_DELAY);

    bool found = false;
    for (int i = 0; i < FACE_MAX_ENROLLED; i++) {
        if (s_face.db[i].valid && s_face.db[i].user_id == user_id) {
            memset(&s_face.db[i], 0, sizeof(face_db_entry_t));
            s_face.db_count--;
            found = true;
            ESP_LOGI(TAG, "Deleted face for user %d", user_id);
            break;
        }
    }

    xSemaphoreGive(s_face.mutex);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t face_recog_load_features(uint8_t user_id, const float *features,
                                    size_t feat_len)
{
    if (!s_face.initialized || features == NULL) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_face.mutex, portMAX_DELAY);

    /* Find existing slot or free slot */
    int slot = -1;
    for (int i = 0; i < FACE_MAX_ENROLLED; i++) {
        if (s_face.db[i].valid && s_face.db[i].user_id == user_id) {
            slot = i;
            break;
        }
        if (!s_face.db[i].valid && slot < 0) {
            slot = i;
        }
    }

    if (slot < 0) {
        xSemaphoreGive(s_face.mutex);
        return ESP_ERR_NO_MEM;
    }

    size_t copy_len = feat_len < FACE_FEAT_LEN ? feat_len : FACE_FEAT_LEN;
    s_face.db[slot].user_id = user_id;
    memcpy(s_face.db[slot].features, features, copy_len * sizeof(float));
    s_face.db[slot].valid = true;
    if (slot >= s_face.db_count) s_face.db_count = slot + 1;

    xSemaphoreGive(s_face.mutex);
    ESP_LOGI(TAG, "Loaded face features for user %d (slot %d)", user_id, slot);
    return ESP_OK;
}

int face_recog_get_enrolled_count(void)
{
    int count = 0;
    for (int i = 0; i < FACE_MAX_ENROLLED; i++) {
        if (s_face.db[i].valid) count++;
    }
    return count;
}

camera_fb_t *face_recog_capture_frame(void)
{
    if (!s_face.camera_ready) return NULL;
    return esp_camera_fb_get();
}

bool face_recog_is_camera_ready(void)
{
    return s_face.camera_ready;
}

void face_recog_set_threshold(float threshold)
{
    if (threshold >= 0.0f && threshold <= 1.0f) {
        s_face.threshold = threshold;
        ESP_LOGI(TAG, "Recognition threshold set to %.2f", threshold);
    }
}