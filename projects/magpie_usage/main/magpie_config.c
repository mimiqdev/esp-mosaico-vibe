// SPDX-License-Identifier: Apache-2.0

#include "magpie_config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "magpie_config";

#define MAGPIE_NVS_NAMESPACE "magpie"
#define MAGPIE_NVS_PARTITION "nvs"

#define KEY_HOST "wifi_host"
#define KEY_PORT "wifi_port"
#define KEY_TOKEN "wifi_token"
#define KEY_MODE "mode"
#define KEY_REFRESH "refresh_s"
#define KEY_BLE_NAME "ble_name"

const char *magpie_transport_mode_name(magpie_transport_mode_t mode)
{
    switch (mode) {
    case MAGPIE_TRANSPORT_BLE_ONLY:
        return "ble";
    case MAGPIE_TRANSPORT_WIFI_ONLY:
        return "wifi";
    case MAGPIE_TRANSPORT_AUTO:
    default:
        return "auto";
    }
}

void magpie_config_defaults(magpie_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    /* Choice symbols: sdkconfig.h defines only the selected one. */
#if defined(CONFIG_MAGPIE_DISPLAY_TRANSPORT_BLE)
    config->mode = MAGPIE_TRANSPORT_BLE_ONLY;
#elif defined(CONFIG_MAGPIE_DISPLAY_TRANSPORT_WIFI)
    config->mode = MAGPIE_TRANSPORT_WIFI_ONLY;
#else
    config->mode = MAGPIE_TRANSPORT_AUTO;
#endif
    strlcpy(config->ble_name_prefix, CONFIG_MAGPIE_DISPLAY_BLE_NAME,
            sizeof(config->ble_name_prefix));
    config->ble_scan_ms = CONFIG_MAGPIE_DISPLAY_BLE_SCAN_MS;
    config->ble_mtu = CONFIG_MAGPIE_DISPLAY_BLE_MTU;
    config->ble_fails_before_wifi = CONFIG_MAGPIE_DISPLAY_BLE_FAILS_BEFORE_WIFI;
    strlcpy(config->wifi_host, CONFIG_MAGPIE_DISPLAY_WIFI_HOST,
            sizeof(config->wifi_host));
    config->wifi_port = CONFIG_MAGPIE_DISPLAY_WIFI_PORT;
    strlcpy(config->wifi_token, CONFIG_MAGPIE_DISPLAY_WIFI_TOKEN,
            sizeof(config->wifi_token));
    config->wifi_timeout_ms = CONFIG_MAGPIE_DISPLAY_WIFI_TIMEOUT_MS;
    config->wifi_start_radio = CONFIG_MAGPIE_DISPLAY_WIFI_START_RADIO;
    config->refresh_interval_s = CONFIG_MAGPIE_DISPLAY_REFRESH_INTERVAL_S;
    config->idle_interval_s = CONFIG_MAGPIE_DISPLAY_IDLE_INTERVAL_S;
    config->stale_after_s = CONFIG_MAGPIE_DISPLAY_STALE_AFTER_S;
}

/* Read a string override, keeping the current value when it is absent.
 * NVS stores strings, so the read buffer is sized by the target field. */
static void read_override(nvs_handle_t handle, const char *key, char *out,
                          size_t size)
{
    size_t length = size;
    char scratch[MAGPIE_TOKEN_MAX];
    if (size > sizeof(scratch)) {
        /* Only the token field is this large; anything bigger is a bug. */
        size = sizeof(scratch);
        length = size;
    }
    if (nvs_get_str(handle, key, scratch, &length) == ESP_OK) {
        scratch[size - 1] = '\0';
        strlcpy(out, scratch, size);
    }
}

esp_err_t magpie_config_load(magpie_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    magpie_config_defaults(config);

    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(MAGPIE_NVS_PARTITION,
                                            MAGPIE_NVS_NAMESPACE,
                                            NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* A fresh device has no overrides; the build defaults are valid. */
        ESP_LOGI(TAG, "no NVS overrides (%s); using build defaults",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    read_override(handle, KEY_HOST, config->wifi_host,
                  sizeof(config->wifi_host));
    read_override(handle, KEY_TOKEN, config->wifi_token,
                  sizeof(config->wifi_token));
    read_override(handle, KEY_BLE_NAME, config->ble_name_prefix,
                  sizeof(config->ble_name_prefix));
    int32_t number = 0;
    if (nvs_get_i32(handle, KEY_PORT, &number) == ESP_OK && number > 0 &&
        number < 65536) {
        config->wifi_port = (uint16_t)number;
    }
    if (nvs_get_i32(handle, KEY_MODE, &number) == ESP_OK &&
        number >= MAGPIE_TRANSPORT_AUTO && number <= MAGPIE_TRANSPORT_WIFI_ONLY) {
        config->mode = (magpie_transport_mode_t)number;
    }
    if (nvs_get_i32(handle, KEY_REFRESH, &number) == ESP_OK && number >= 5 &&
        number <= 600) {
        config->refresh_interval_s = (uint32_t)number;
    }
    nvs_close(handle);

    ESP_LOGI(TAG, "transport=%s ble=%s wifi=%s:%u token=%s refresh=%lus",
             magpie_transport_mode_name(config->mode),
             config->ble_name_prefix,
             config->wifi_host[0] ? config->wifi_host : "(unset)",
             (unsigned)config->wifi_port,
             config->wifi_token[0] ? "set" : "none",
             (unsigned long)config->refresh_interval_s);
    return ESP_OK;
}
