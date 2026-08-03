#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "pb_nvs";
static const char *NS = "pocketbyte";
static bool inited = false;

// This is what happens when a language doesn't have generics... :(
#define NVS_SET(ns, key, val, type, func) do { \
        nvs_handle_t _h; \
        if (nvs_open(ns, NVS_READWRITE, &_h) == ESP_OK) { \
            nvs_set_##func(_h, key, val); \
            nvs_commit(_h); \
            nvs_close(_h); \
        } \
    } while(0)

#define NVS_GET(ns, key, out, type, func) do { \
        nvs_handle_t _h; \
        if (nvs_open(ns, NVS_READONLY, &_h) == ESP_OK) { \
            nvs_get_##func(_h, key, out); \
            nvs_close(_h); \
        } \
    } while(0)

void pb_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI(TAG, "Erasing NVS for new version");
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Erase failed");
            return;
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init failed");
        return;
    }
    inited = true;
    ESP_LOGI(TAG, "Init ok");
}

void pb_nvs_deinit(void)
{
    nvs_flash_deinit();
    inited = false;
}

void pb_nvs_save_u32(const char *key, uint32_t value)
{
    if (!inited) {
        return;
    }
    NVS_SET(NS, key, value, uint32_t, u32);
}

uint32_t pb_nvs_load_u32(const char *key, uint32_t default_value)
{
    if (!inited) {
        return default_value;
    }
    uint32_t v = default_value;
    NVS_GET(NS, key, &v, uint32_t, u32);
    return v;
}

void pb_nvs_save_i32(const char *key, int32_t value)
{
    if (!inited) {
        return;
    }
    NVS_SET(NS, key, value, int32_t, i32);
}

int32_t pb_nvs_load_i32(const char *key, int32_t default_value)
{
    if (!inited) {
        return default_value;
    }
    int32_t v = default_value;
    NVS_GET(NS, key, &v, int32_t, i32);
    return v;
}

void pb_nvs_save_str(const char *key, const char *value)
{
    if (!inited) {
        return;
    }
    NVS_SET(NS, key, value, const char *, str);
}

esp_err_t pb_nvs_load_str(const char *key, char *buf, size_t *len)
{
    if (!inited) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NS, NVS_READONLY, &h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_get_str(h, key, buf, len);
    nvs_close(h);
    return ret;
}

void pb_nvs_save_blob(const char *key, const void *data, size_t len)
{
    if (!inited) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, key, data, len);
    nvs_commit(h);
    nvs_close(h);
}

esp_err_t pb_nvs_load_blob(const char *key, void *buf, size_t *len)
{
    if (!inited) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NS, NVS_READONLY, &h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_get_blob(h, key, buf, len);
    nvs_close(h);
    return ret;
}

void pb_nvs_erase_key(const char *key)
{
    if (!inited) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

void pb_nvs_erase_all(void)
{
    if (!inited) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
}
