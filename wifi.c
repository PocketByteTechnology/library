#include "wifi.h"
#include "sd.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pb_wifi";

#define WIFI_JSON_PATH MOUNT_POINT"/wifi.json"
#define MAX_RETRY 5
#define CONNECTED_BIT BIT0
#define FAIL_BIT BIT1

static char wifi_ssid[128];
static char wifi_pass[128];
static EventGroupHandle_t wifi_event_group;
static esp_netif_t *wifi_netif;
static int num_retries;
static bool is_connected;
static bool wifi_started;
static bool init_done;
static pb_wifi_event_cb_t event_cb;
static void *event_cb_arg;

static void parse_wifi_json(void)
{
    // Wi-Fi credentials are in wifi.json on the SD card
    // You *could* store them in NVS instead, but keeping them on the SD card allows you to
    // modify them from your computer, which can be really handy
    FILE *file = fopen(WIFI_JSON_PATH, "r");
    if (file == NULL) {
        ESP_LOGW(TAG, "No %s found, use pb_wifi_set_credentials()", WIFI_JSON_PATH);
        return;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *buf = malloc(size + 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate buffer for wifi.json");
        fclose(file);
        return;
    }

    size_t read_bytes = fread(buf, 1, size, file);
    buf[read_bytes] = '\0';
    fclose(file);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse %s", WIFI_JSON_PATH);
        return;
    }

    cJSON *ssid = cJSON_GetObjectItem(root, "ssid");
    if (cJSON_IsString(ssid) && ssid->valuestring) {
        strlcpy(wifi_ssid, ssid->valuestring, sizeof(wifi_ssid));
    }
    cJSON *pass = cJSON_GetObjectItem(root, "password");
    if (cJSON_IsString(pass) && pass->valuestring) {
        strlcpy(wifi_pass, pass->valuestring, sizeof(wifi_pass));
    }

    cJSON_Delete(root);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        is_connected = false;
        if (num_retries < MAX_RETRY) {
            esp_wifi_connect();
            num_retries++;
            ESP_LOGI(TAG, "Retrying to connect to the access point");
        } else {
            xEventGroupSetBits(wifi_event_group, FAIL_BIT);
            ESP_LOGE(TAG, "Failed to connect to the access point");
        }
        if (event_cb) {
            event_cb(PB_WIFI_EVENT_DISCONNECTED, event_cb_arg);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        num_retries = 0;
        is_connected = true;
        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        if (event_cb) {
            event_cb(PB_WIFI_EVENT_GOT_IP, event_cb_arg);
        }
    }
}

esp_err_t pb_wifi_init(void)
{
    if (init_done) {
        return ESP_OK;
    }

    parse_wifi_json();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    wifi_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL,
                                                        &instance_got_ip));

    wifi_event_group = xEventGroupCreate();
    init_done = true;
    ESP_LOGI(TAG, "Wi-Fi initialized");
    return ESP_OK;
}

esp_err_t pb_wifi_connect(void)
{
    if (!init_done) {
        esp_err_t ret = pb_wifi_init();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    xEventGroupClearBits(wifi_event_group, CONNECTED_BIT | FAIL_BIT);
    num_retries = 0;

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            .sae_h2e_identifier = "",
        },
    };
    strlcpy((char *)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, wifi_pass, sizeof(wifi_config.sta.password));

    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!wifi_started) {
        ret = esp_wifi_start();
        if (ret != ESP_OK) {
            return ret;
        }
        wifi_started = true;
    }
    return ESP_OK;
}

esp_err_t pb_wifi_connect_blocking(int timeout_ms)
{
    esp_err_t ret = pb_wifi_connect();
    if (ret != ESP_OK) {
        return ret;
    }

    if (is_connected) {
        return ESP_OK;
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT | FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY);
    if (bits & CONNECTED_BIT) {
        return ESP_OK;
    }
    return ESP_ERR_WIFI_NOT_CONNECT;
}

esp_err_t pb_wifi_disconnect(void)
{
    is_connected = false;
    if (!wifi_started) {
        return ESP_OK;
    }
    return esp_wifi_disconnect();
}

bool pb_wifi_is_connected(void)
{
    return is_connected;
}

esp_err_t pb_wifi_get_ip(char *buf, size_t len)
{
    if (wifi_netif == NULL || !is_connected) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    esp_netif_ip_info_t ip;
    esp_err_t ret = esp_netif_get_ip_info(wifi_netif, &ip);
    if (ret != ESP_OK) {
        return ret;
    }
    esp_ip4addr_ntoa(&ip.ip, buf, len);
    return ESP_OK;
}

esp_err_t pb_wifi_set_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(wifi_ssid, ssid, sizeof(wifi_ssid));
    strlcpy(wifi_pass, password, sizeof(wifi_pass));
    if (wifi_started) {
        return pb_wifi_connect();
    }
    return ESP_OK;
}

void pb_wifi_set_event_callback(pb_wifi_event_cb_t cb, void *user_data)
{
    event_cb = cb;
    event_cb_arg = user_data;
}

typedef struct {
    FILE *file;
    char *buf;
    size_t size;
    esp_err_t err;
} pb_http_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    pb_http_ctx_t *ctx = (pb_http_ctx_t *)evt->user_data;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (evt->data_len > 0) {
                if (ctx->file != NULL) {
                    size_t written = fwrite(evt->data, sizeof(char), evt->data_len, ctx->file);
                    if (written < evt->data_len) {
                        ESP_LOGE(TAG, "Flash write error. Might be full?");
                        ctx->err = ESP_FAIL;
                        return ESP_FAIL;
                    }
                    ctx->size += written;
                } else {
                    char *new_buf = realloc(ctx->buf, ctx->size + evt->data_len + 1);
                    if (new_buf == NULL) {
                        ESP_LOGE(TAG, "Failed to allocate memory for chunk");
                        ctx->err = ESP_ERR_NO_MEM;
                        return ESP_FAIL;
                    }
                    ctx->buf = new_buf;
                    memcpy(ctx->buf + ctx->size, evt->data, evt->data_len);
                    ctx->size += evt->data_len;
                    ctx->buf[ctx->size] = '\0';
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static esp_http_client_method_t esp_method(pb_http_method_t method)
{
    switch (method) {
        case PB_HTTP_GET:     return HTTP_METHOD_GET;
        case PB_HTTP_POST:    return HTTP_METHOD_POST;
        case PB_HTTP_PUT:     return HTTP_METHOD_PUT;
        case PB_HTTP_DELETE:  return HTTP_METHOD_DELETE;
        case PB_HTTP_PATCH:   return HTTP_METHOD_PATCH;
        case PB_HTTP_HEAD:    return HTTP_METHOD_HEAD;
    }
    return HTTP_METHOD_GET;
}

esp_err_t pb_http_request(const pb_http_request_t *req, char **resp_out, size_t *resp_len_out, int *status_out)
{
    if (req == NULL || req->url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (resp_out) {
        *resp_out = NULL;
    }
    if (resp_len_out) {
        *resp_len_out = 0;
    }

    pb_http_ctx_t ctx = {0};

    esp_http_client_config_t config = {
        .url = req->url,
        .method = esp_method(req->method),
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = req->timeout_ms > 0 ? req->timeout_ms : 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_HTTP_BASE;
    }

    if (req->headers) {
        for (int i = 0; req->headers[i]; i++) {
            const char *header = req->headers[i];
            const char *colon = strchr(header, ':');
            if (colon == NULL) {
                continue;
            }
            char *name = strndup(header, colon - header);
            if (name == NULL) {
                esp_http_client_cleanup(client);
                return ESP_ERR_NO_MEM;
            }
            const char *value = colon + 1;
            while (*value == ' ') {
                value++;
            }
            esp_http_client_set_header(client, name, value);
            free(name);
        }
    }

    if (req->body) {
        esp_http_client_set_post_field(client, req->body, strlen(req->body));
    }

    esp_err_t ret = esp_http_client_perform(client);
    if (status_out) {
        *status_out = ret == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    }
    esp_http_client_cleanup(client);

    if (ctx.err != ESP_OK) {
        ret = ctx.err;
    }

    if (resp_out && ctx.buf) {
        *resp_out = ctx.buf;
        ctx.buf = NULL;
        if (resp_len_out) {
            *resp_len_out = ctx.size;
        }
    }
    free(ctx.buf);

    return ret;
}

esp_err_t pb_http_download(const char *url, const char *path, int *status_out)
{
    if (url == NULL || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Stream large files (app binaries, images, etc.) straight to the SD card
    // Otherwise you get stuff like corruption (happened to me while making the app store)
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        return ESP_ERR_INVALID_STATE;
    }

    pb_http_ctx_t ctx = { .file = file };

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        fclose(file);
        return ESP_ERR_HTTP_BASE;
    }

    esp_err_t ret = esp_http_client_perform(client);
    if (status_out) {
        *status_out = ret == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    }
    esp_http_client_cleanup(client);
    fclose(file);

    if (ctx.err != ESP_OK) {
        return ctx.err;
    }
    return ret;
}
