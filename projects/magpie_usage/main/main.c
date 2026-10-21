// SPDX-License-Identifier: Apache-2.0

#include "board_display.h"
#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_gsp_esp_lcd.h"
#include "freertos/event_groups.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#if CONFIG_BT_ENABLED
#include "magpie_ble.h"
#endif
#include "magpie_config.h"
#include "magpie_store.h"
#include "magpie_ui.h"
#include "magpie_wifi.h"
#include "nvs_flash.h"
#include "ui_bundle.h"

static const char *TAG = "magpie_usage";

#define REFRESH_TASK_STACK 6144
#define REFRESH_TASK_PRIORITY 4
#define UI_TICK_MS 500
#define UI_TASK_STACK 4096
#define UI_TASK_PRIORITY 5

/*
 * Single owner rules:
 *   - the refresh task is the only writer of magpie_store_t;
 *   - the UI task is the only caller of magpie_ui_* and therefore the only
 *     GSP consumer in this application;
 *   - the two communicate through magpie_store_snapshot() (mutex) plus an
 *     event bit, so neither blocks the other for long.
 */
typedef struct {
    magpie_store_t store;
    magpie_ui_t *ui;
    magpie_source_t ble_source;
    magpie_source_t wifi_source;
} app_ctx_t;

static app_ctx_t s_app;
static EventGroupHandle_t s_events;

#define EVENT_NEW_DATA BIT0
#define EVENT_STOP BIT1

/* ---- refresh task ---------------------------------------------------- */

static void refresh_task(void *param)
{
    app_ctx_t *app = param;
    uint32_t last_tick_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    for (;;) {
        if (xEventGroupWaitBits(s_events, EVENT_STOP, pdFALSE, pdFALSE, 0) &
            EVENT_STOP) {
            break;
        }

        const uint32_t now_ms =
            (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        const uint32_t elapsed_s = (now_ms - last_tick_ms) / 1000;
        if (elapsed_s) {
            last_tick_ms += elapsed_s * 1000;
            magpie_store_tick(&app->store, elapsed_s);
        }

        if (magpie_store_refresh_due(&app->store)) {
            const magpie_transport_t used = magpie_store_refresh(&app->store);
            if (used != MAGPIE_TRANSPORT_NONE) {
                xEventGroupSetBits(s_events, EVENT_NEW_DATA);
            }
        }

        uint32_t delay_ms = magpie_store_next_delay_ms(&app->store);
        if (delay_ms < UI_TICK_MS) {
            delay_ms = UI_TICK_MS;
        }
        /* Wake early when the transport reports a change notification. */
        magpie_source_t *active =
            magpie_store_prefers_wifi(&app->store) ? &app->wifi_source
                                                   : &app->ble_source;
        if (active->wait_changed && active->wait_changed(active, delay_ms)) {
            continue;
        }
    }
    vTaskDelete(NULL);
}

/* ---- UI task --------------------------------------------------------- */

static void ui_task(void *param)
{
    app_ctx_t *app = param;
    for (;;) {
        const EventBits_t bits =
            xEventGroupWaitBits(s_events, EVENT_NEW_DATA | EVENT_STOP, pdTRUE,
                                pdFALSE, pdMS_TO_TICKS(1000));
        if (bits & EVENT_STOP) {
            break;
        }
        magpie_state_t snapshot;
        if (bits & EVENT_NEW_DATA) {
            if (magpie_store_snapshot(&app->store, &snapshot)) {
                magpie_ui_show(app->ui, &snapshot, magpie_ui_page(app->ui));
            }
        } else {
            /* Keep the link age (LIVE -> STALE -> OFFLINE) current. */
            magpie_ui_refresh_status(app->ui);
        }
    }
    vTaskDelete(NULL);
}

/* ---- wiring --------------------------------------------------------- */

static void build_transports(app_ctx_t *app, const magpie_config_t *config)
{
#if CONFIG_BT_ENABLED
    magpie_ble_config_t ble_config = {0};
    strlcpy(ble_config.name_prefix, config->ble_name_prefix,
            sizeof(ble_config.name_prefix));
    ble_config.scan_ms = config->ble_scan_ms;
    ble_config.mtu = config->ble_mtu;
#endif

    const bool want_ble = config->mode != MAGPIE_TRANSPORT_WIFI_ONLY;
#if CONFIG_BT_ENABLED
    if (want_ble) {
        const esp_err_t err =
            magpie_ble_source_init(&ble_config, &app->ble_source);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "BLE central unavailable (%s)", esp_err_to_name(err));
        } else {
            magpie_store_attach(&app->store, &app->ble_source, NULL);
        }
    }
#else
    if (want_ble) {
        ESP_LOGW(TAG, "built without Bluetooth; the BLE transport is unavailable");
    }
#endif

    const bool want_wifi = config->mode != MAGPIE_TRANSPORT_BLE_ONLY &&
                           config->wifi_host[0] != '\0';
    if (want_wifi) {
        magpie_wifi_config_t wifi_config = {0};
        strlcpy(wifi_config.host, config->wifi_host, sizeof(wifi_config.host));
        wifi_config.port = config->wifi_port;
        strlcpy(wifi_config.token, config->wifi_token,
                sizeof(wifi_config.token));
        wifi_config.timeout_ms = config->wifi_timeout_ms;
        const esp_err_t err = magpie_wifi_source_init(
            &wifi_config, config->wifi_start_radio, &app->wifi_source);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi source unavailable (%s)", esp_err_to_name(err));
        } else {
            magpie_store_attach(&app->store, NULL, &app->wifi_source);
        }
    }
    if (config->mode == MAGPIE_TRANSPORT_WIFI_ONLY && config->wifi_host[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi only mode without a host; nothing can be pulled");
    }
}

static esp_err_t start_ui(app_ctx_t *app)
{
    esp_gsp_config_t app_config;
    const esp_err_t bundle_err = ui_bundle_open(&app_config);
    if (bundle_err != ESP_OK) {
        ESP_LOGE(TAG,
                 "GSP UI unavailable (0x%x); ESP-Iris Recovery RPC remains active",
                 bundle_err);
        return bundle_err;
    }

    esp_display_present_target_config_t display;
    ESP_RETURN_ON_ERROR(board_display_init(&display), TAG, "display init");

    esp_lcd_touch_handle_t touch = NULL;
    ESP_RETURN_ON_ERROR(board_touch_init(&touch), TAG, "touch init");

    esp_gsp_esp_lcd_config_t lcd = ESP_GSP_ESP_LCD_CONFIG_INIT();
    lcd.display = display;
    lcd.touch = touch;

    ESP_RETURN_ON_ERROR(iris_screen_mirror_init(), TAG, "screen mirror init");

    esp_gsp_handle_t ui;
    ESP_RETURN_ON_ERROR(esp_gsp_esp_lcd_start(&app_config, &lcd, &ui), TAG,
                        "GSP start");
    ESP_RETURN_ON_ERROR(iris_screen_mirror_attach(ui), TAG, "mirror attach");

    static magpie_ui_t ui_state;
    ESP_RETURN_ON_ERROR(magpie_ui_init(ui, &ui_state), TAG, "magpie UI init");
    app->ui = &ui_state;
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());
    /* sysmeta holds the Wi-Fi credentials; open it so the Wi-Fi source can
     * read the same provisioned network the rest of the repository uses. */
    esp_err_t sysmeta = nvs_flash_init_partition("sysmeta");
    if (sysmeta != ESP_OK && sysmeta != ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGW(TAG, "sysmeta NVS unavailable (%s)", esp_err_to_name(sysmeta));
    }

    /* Keep Recovery reachable even when the UI or the network is broken. */
    iris_ota_support_start();

    magpie_config_t config;
    ESP_ERROR_CHECK(magpie_config_load(&config));

    s_events = xEventGroupCreate();
    if (!s_events) {
        ESP_LOGE(TAG, "no memory for the event group");
        return;
    }

    ESP_ERROR_CHECK(magpie_store_init(&s_app.store, &config));
    build_transports(&s_app, &config);

    if (start_ui(&s_app) == ESP_OK) {
        ESP_LOGI(TAG, "Magpie usage display ready at %dx%d RGB565 "
                      "(transport=%s, refresh=%lus)",
                 BSP_LCD_H_RES, BSP_LCD_V_RES,
                 magpie_transport_mode_name(config.mode),
                 (unsigned long)config.refresh_interval_s);
        if (xTaskCreate(ui_task, "magpie_ui", UI_TASK_STACK, &s_app,
                        UI_TASK_PRIORITY, NULL) != pdPASS) {
            ESP_LOGE(TAG, "UI task creation failed");
        }
    }

    if (xTaskCreate(refresh_task, "magpie_pull", REFRESH_TASK_STACK, &s_app,
                    REFRESH_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "refresh task creation failed");
    }
}
