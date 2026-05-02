/**
 * @file event_manager.c
 * @brief Smart Door Lock Event Manager Implementation
 *
 * Provides a centralized event bus for inter-module communication
 * using ESP-IDF's event loop library.
 */

#include "event_manager.h"
#include "esp_log.h"
#include "esp_event.h"

static const char *TAG = "EVENT_MGR";

/* Define event bases */
ESP_EVENT_DEFINE_BASE(SDL_DOOR_EVENTS);
ESP_EVENT_DEFINE_BASE(SDL_AUTH_EVENTS);
ESP_EVENT_DEFINE_BASE(SDL_SYSTEM_EVENTS);

/* Default event loop handle */
static esp_event_loop_handle_t s_event_loop = NULL;

esp_err_t event_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing event manager");

    /* Create default event loop if not already created */
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create default event loop: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Event manager initialized successfully");
    return ESP_OK;
}

esp_err_t event_manager_post_door(sdl_door_event_id_t event_id,
                                   const sdl_door_event_data_t *data)
{
    esp_err_t ret = esp_event_post(SDL_DOOR_EVENTS,
                                    (int32_t)event_id,
                                    (void *)data,
                                    data ? sizeof(sdl_door_event_data_t) : 0,
                                    pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to post door event %d: %s",
                 event_id, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t event_manager_post_auth(sdl_auth_event_id_t event_id,
                                   const sdl_auth_result_t *data)
{
    esp_err_t ret = esp_event_post(SDL_AUTH_EVENTS,
                                    (int32_t)event_id,
                                    (void *)data,
                                    data ? sizeof(sdl_auth_result_t) : 0,
                                    pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to post auth event %d: %s",
                 event_id, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t event_manager_post_system(sdl_system_event_id_t event_id,
                                     const void *data)
{
    esp_err_t ret = esp_event_post(SDL_SYSTEM_EVENTS,
                                    (int32_t)event_id,
                                    (void *)data,
                                    0,
                                    pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to post system event %d: %s",
                 event_id, esp_err_to_name(ret));
    }
    return ret;
}