#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PB_WIFI_EVENT_GOT_IP,
    PB_WIFI_EVENT_DISCONNECTED,
} pb_wifi_event_t;

typedef void (*pb_wifi_event_cb_t)(pb_wifi_event_t event, void *user_data);

esp_err_t pb_wifi_init(void);
esp_err_t pb_wifi_connect(void);
esp_err_t pb_wifi_connect_blocking(int timeout_ms);
esp_err_t pb_wifi_disconnect(void);
bool pb_wifi_is_connected(void);
esp_err_t pb_wifi_get_ip(char *buf, size_t len);
esp_err_t pb_wifi_set_credentials(const char *ssid, const char *password);
void pb_wifi_set_event_callback(pb_wifi_event_cb_t cb, void *user_data);

typedef enum {
    PB_HTTP_GET,
    PB_HTTP_POST,
    PB_HTTP_PUT,
    PB_HTTP_DELETE,
    PB_HTTP_PATCH,
    PB_HTTP_HEAD,
} pb_http_method_t;

typedef struct {
    pb_http_method_t method;
    const char *url;
    const char **headers;
    const char *body;
    int timeout_ms;
} pb_http_request_t;

esp_err_t pb_http_request(const pb_http_request_t *req, char **resp_out, size_t *resp_len_out, int *status_out);
esp_err_t pb_http_download(const char *url, const char *path, int *status_out);

#ifdef __cplusplus
}
#endif
