// SPDX-License-Identifier: Apache-2.0

#include "magpie_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "magpie_wifi";

/* Credentials written by `python mosaico.py iris test recovery-wifi`. */
#define SYSMETA_PARTITION "sysmeta"
#define WIFI_NAMESPACE "wifi"
#define WIFI_KEY_SSID "ssid"
#define WIFI_KEY_PASSWORD "password"
#define SSID_MAX 33
#define PASSWORD_MAX 65

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define WIFI_MAX_RETRIES 8

#define HTTP_BUFFER_SIZE 512
#define HTTP_TX_BUFFER_SIZE 256

/* One HTTP fallback per device; the instance lives here. */
struct magpie_wifi_source {
    magpie_wifi_config_t config;
    bool connected;
};

static struct magpie_wifi_source s_wifi;

static EventGroupHandle_t s_wifi_events;
static bool s_radio_started;
static bool s_has_ip;
static uint8_t s_retries;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id,
                               void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_has_ip = false;
        if (s_wifi_events && s_retries < WIFI_MAX_RETRIES) {
            ++s_retries;
            ESP_LOGW(TAG, "disconnected; retry %u/%u", (unsigned)s_retries,
                     (unsigned)WIFI_MAX_RETRIES);
            (void)esp_wifi_connect();
        } else if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Wi-Fi ready: ip=" IPSTR, IP2STR(&event->ip_info.ip));
        s_retries = 0;
        s_has_ip = true;
        if (s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
    }
}

static esp_err_t credentials_load(char *ssid, char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(SYSMETA_PARTITION, WIFI_NAMESPACE,
                                            NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t ssid_size = SSID_MAX;
    size_t password_size = PASSWORD_MAX;
    err = nvs_get_str(handle, WIFI_KEY_SSID, ssid, &ssid_size);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, WIFI_KEY_PASSWORD, password, &password_size);
    }
    nvs_close(handle);
    return err;
}

/**
 * Bring up the STA interface from the sysmeta credentials.
 *
 * This deliberately reuses the repository's provisioning path instead of
 * inventing one: the same ssid/password entries that
 * `python mosaico.py iris test recovery-wifi` writes. Nothing is compiled in.
 */
static esp_err_t wifi_start_sta(void)
{
    if (s_radio_started) {
        return ESP_OK;
    }
    char ssid[SSID_MAX] = {0};
    char password[PASSWORD_MAX] = {0};
    const esp_err_t load_err = credentials_load(ssid, password);
    if (load_err != ESP_OK || ssid[0] == '\0') {
        ESP_LOGW(TAG,
                 "no Wi-Fi credentials in sysmeta/%s (%s); Wi-Fi stays down",
                 WIFI_NAMESPACE, esp_err_to_name(load_err));
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    const esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        return loop_err;
    }
    if (!esp_netif_create_default_wifi_sta()) {
        return ESP_ERR_NO_MEM;
    }
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler,
                            NULL, NULL),
                        TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler,
                            NULL, NULL),
                        TAG, "ip events");

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    config.sta.threshold.authmode =
        password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* Credentials already live in sysmeta; keep them out of the Wi-Fi NVS. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                        "volatile storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "sta mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG,
                        "sta config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    /* BLE and Wi-Fi share the radio; minimum modem sleep keeps coexistence
     * working without stalling the display refresh. */
    (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    s_radio_started = true;
    return ESP_OK;
}

static bool wifi_wait_connected(uint32_t timeout_ms)
{
    if (!s_wifi_events) {
        return false;
    }
    if (s_has_ip) {
        return true;
    }
    const EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool magpie_wifi_source_configured(void)
{
    return s_wifi.config.host[0] != '\0' && s_wifi.config.port != 0;
}

bool magpie_wifi_source_has_ip(void)
{
    return s_has_ip;
}

/**
 * Read one path into buf.
 *
 * The response is streamed into the caller's buffer with a hard cap, so an
 * unexpected large document is rejected instead of overflowing: payloads are
 * at most 180 bytes and the transports never need a large allocation.
 */
static int http_get(const magpie_wifi_source_t *self, const char *path,
                    char *buf, size_t cap)
{
    char url[MAGPIE_HOST_MAX + 32 + 24];
    snprintf(url, sizeof(url), "http://%s:%u%s", self->config.host,
             (unsigned)self->config.port, path);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = (int)self->config.timeout_ms,
        .buffer_size = HTTP_BUFFER_SIZE,
        .buffer_size_tx = HTTP_TX_BUFFER_SIZE,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return MAGPIE_SOURCE_ERR_LINK;
    }
    if (self->config.token[0]) {
        char header[MAGPIE_TOKEN_MAX + 16];
        snprintf(header, sizeof(header), "Bearer %s", self->config.token);
        esp_http_client_set_header(client, "Authorization", header);
    }
    esp_http_client_set_header(client, "Accept", "application/json");

    int result = MAGPIE_SOURCE_OK;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: open failed (%s)", path, esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return MAGPIE_SOURCE_ERR_LINK;
    }
    const int content_length = esp_http_client_fetch_headers(client);
    if (content_length >= 0 && (size_t)content_length >= cap) {
        ESP_LOGW(TAG, "%s is %d bytes, above the %u-byte payload limit", path,
                 content_length, (unsigned)cap);
        result = MAGPIE_SOURCE_ERR_PAYLOAD;
    }

    size_t used = 0;
    while (result == MAGPIE_SOURCE_OK && used + 1 < cap) {
        const int read =
            esp_http_client_read(client, buf + used, (int)(cap - 1 - used));
        if (read < 0) {
            result = MAGPIE_SOURCE_ERR_LINK;
            break;
        }
        if (read == 0) {
            break;
        }
        used += (size_t)read;
    }
    buf[used] = '\0';
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (result != MAGPIE_SOURCE_OK) {
        return result;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "%s returned HTTP %d", path, status);
        return status == 401 || status == 403 ? MAGPIE_SOURCE_ERR_LINK
                                             : MAGPIE_SOURCE_ERR_PAYLOAD;
    }
    if (used == 0) {
        return MAGPIE_SOURCE_ERR_PAYLOAD;
    }
    return (int)used;
}

static int wifi_open(magpie_source_t *source)
{
    magpie_wifi_source_t *self = source->impl;
    if (!magpie_wifi_source_configured()) {
        return MAGPIE_SOURCE_ERR_ARG;
    }
    if (!wifi_wait_connected(WIFI_CONNECT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "no IP address; Wi-Fi pull skipped");
        return MAGPIE_SOURCE_ERR_LINK;
    }
    self->connected = true;
    return MAGPIE_SOURCE_OK;
}

static void wifi_close(magpie_source_t *source)
{
    magpie_wifi_source_t *self = source->impl;
    if (self) {
        self->connected = false;
    }
}

static int wifi_read_index(magpie_source_t *source, char *buf, size_t cap)
{
    magpie_wifi_source_t *self = source->impl;
    if (!self || !self->connected) {
        return MAGPIE_SOURCE_ERR_LINK;
    }
    return http_get(self, "/v1/display/index", buf, cap);
}

static int wifi_read_sub(magpie_source_t *source, uint8_t slot, char *buf,
                         size_t cap)
{
    magpie_wifi_source_t *self = source->impl;
    if (!self || !self->connected) {
        return MAGPIE_SOURCE_ERR_LINK;
    }
    char path[48];
    snprintf(path, sizeof(path), "/v1/display/sub/%u", (unsigned)slot);
    return http_get(self, path, buf, cap);
}

static bool wifi_wait_changed(magpie_source_t *source, uint32_t timeout_ms)
{
    (void)source;
    /* HTTP has no push channel; the periodic read is the safety net. */
    vTaskDelay(pdMS_TO_TICKS(timeout_ms));
    return false;
}

esp_err_t magpie_wifi_source_init(const magpie_wifi_config_t *config,
                                  bool start_radio,
                                  magpie_source_t *out_source)
{
    ESP_RETURN_ON_FALSE(config && out_source, ESP_ERR_INVALID_ARG, TAG,
                        "null argument");
    magpie_wifi_source_t *self = &s_wifi;
    memset(self, 0, sizeof(*self));
    self->config = *config;
    self->config.host[MAGPIE_HOST_MAX - 1] = '\0';
    self->config.token[MAGPIE_TOKEN_MAX - 1] = '\0';

    if (start_radio) {
        const esp_err_t err = wifi_start_sta();
        if (err != ESP_OK) {
            /* Keep the source usable in case another component owns Wi-Fi. */
            ESP_LOGW(TAG, "STA bring-up unavailable (%s)", esp_err_to_name(err));
        }
    }

    memset(out_source, 0, sizeof(*out_source));
    out_source->name = "wifi";
    out_source->open = wifi_open;
    out_source->close = wifi_close;
    out_source->read_index = wifi_read_index;
    out_source->read_sub = wifi_read_sub;
    out_source->wait_changed = wifi_wait_changed;
    out_source->impl = self;
    return ESP_OK;
}

void magpie_wifi_source_deinit(magpie_source_t *source)
{
    s_wifi.connected = false;
    if (source) {
        source->impl = NULL;
        source->open = NULL;
        source->close = NULL;
        source->read_index = NULL;
        source->read_sub = NULL;
        source->wait_changed = NULL;
    }
}
