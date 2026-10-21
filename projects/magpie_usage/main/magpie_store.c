// SPDX-License-Identifier: Apache-2.0

#include "magpie_store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "magpie_parse.h"
#include "magpie_pull.h"

static const char *TAG = "magpie_store";

static uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

esp_err_t magpie_store_init(magpie_store_t *store,
                            const magpie_config_t *config)
{
    if (!store || !config) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(store, 0, sizeof(*store));
    store->config = *config;
    store->lock = xSemaphoreCreateMutex();
    if (!store->lock) {
        return ESP_ERR_NO_MEM;
    }
    magpie_state_init(&store->state);
    store->backoff_ms = MAGPIE_BACKOFF_MIN_MS;
    return ESP_OK;
}

void magpie_store_attach(magpie_store_t *store, magpie_source_t *ble,
                         magpie_source_t *wifi)
{
    if (!store) {
        return;
    }
    if (ble && ble->open) {
        store->ble_source = *ble;
        store->ble_ready = true;
    }
    if (wifi && wifi->open) {
        store->wifi_source = *wifi;
        store->wifi_ready = true;
    }
    store->refresh_ble = store->ble_ready;
    store->refresh_wifi = store->wifi_ready;
}

bool magpie_store_snapshot(magpie_store_t *store, magpie_state_t *out)
{
    if (!store || !out || !store->lock) {
        return false;
    }
    xSemaphoreTake(store->lock, portMAX_DELAY);
    *out = store->state;
    const bool have_data = store->state.transport != MAGPIE_TRANSPORT_NONE;
    xSemaphoreGive(store->lock);
    return have_data;
}

void magpie_store_set_page(magpie_store_t *store, uint8_t page)
{
    if (!store || !store->lock) {
        return;
    }
    xSemaphoreTake(store->lock, portMAX_DELAY);
    magpie_state_set_page(&store->state, page);
    xSemaphoreGive(store->lock);
}

bool magpie_store_prefers_wifi(const magpie_store_t *store)
{
    return store && store->prefer_wifi;
}

static magpie_source_t *pick_source(magpie_store_t *store)
{
    const bool wifi_only = store->config.mode == MAGPIE_TRANSPORT_WIFI_ONLY;
    const bool ble_only = store->config.mode == MAGPIE_TRANSPORT_BLE_ONLY;

    if (wifi_only) {
        return store->refresh_wifi ? &store->wifi_source : NULL;
    }
    if (ble_only) {
        return store->refresh_ble ? &store->ble_source : NULL;
    }
    const bool ble_due =
        store->refresh_ble &&
        (int32_t)(uptime_ms() - store->ble_next_attempt_ms) >= 0;
    if (!store->prefer_wifi && ble_due) {
        return &store->ble_source;
    }
    if (store->refresh_wifi) {
        return &store->wifi_source;
    }
    return store->refresh_ble ? &store->ble_source : NULL;
}

bool magpie_store_refresh_due(magpie_store_t *store)
{
    if (!store) {
        return false;
    }
    return pick_source(store) != NULL;
}

magpie_transport_t magpie_store_refresh(magpie_store_t *store)
{
    if (!store || !store->lock) {
        return MAGPIE_TRANSPORT_NONE;
    }
    magpie_source_t *choice = pick_source(store);
    if (!choice) {
        return MAGPIE_TRANSPORT_NONE;
    }
    const bool is_ble = choice == &store->ble_source;
    const magpie_transport_t used =
        is_ble ? MAGPIE_TRANSPORT_BLE : MAGPIE_TRANSPORT_WIFI;

    magpie_pull_ctx_t ctx = {
        .index_buf = store->index_buf,
        .sub_buf = store->sub_buf,
        .index_cap = sizeof(store->index_buf),
        .sub_cap = sizeof(store->sub_buf),
        .index = &store->scratch_index,
        .subs = store->scratch_subs,
        .loaded_count = &store->scratch_loaded,
    };
    magpie_pull_result_t result;
    const bool ok = magpie_pull_once(choice, &ctx, &result);

    if (!ok) {
        ESP_LOGW(TAG, "%s pull failed (%d)", choice->name, result.last_error);
        if (is_ble) {
            ++store->ble_failures;
            store->backoff_ms *= 2;
            if (store->backoff_ms > MAGPIE_BACKOFF_MAX_MS) {
                store->backoff_ms = MAGPIE_BACKOFF_MAX_MS;
            }
            store->ble_next_attempt_ms = uptime_ms() + store->backoff_ms;
            if (store->ble_failures >= store->config.ble_fails_before_wifi &&
                store->wifi_ready && !store->prefer_wifi) {
                ESP_LOGW(TAG,
                         "%u BLE failures; using Wi-Fi until BLE recovers "
                         "(retry in %lums)",
                         (unsigned)store->ble_failures,
                         (unsigned long)store->backoff_ms);
                store->prefer_wifi = true;
            }
        }
        return MAGPIE_TRANSPORT_NONE;
    }

    if (is_ble) {
        if (store->prefer_wifi) {
            ESP_LOGI(TAG, "BLE recovered; preferring BLE again");
        }
        store->ble_failures = 0;
        store->prefer_wifi = false;
        store->backoff_ms = MAGPIE_BACKOFF_MIN_MS;
        store->ble_next_attempt_ms = 0;
    }

    xSemaphoreTake(store->lock, portMAX_DELAY);
    magpie_state_set_index(&store->state, &store->scratch_index);
    uint8_t accepted = 0;
    for (uint8_t slot = 0; slot < MAGPIE_SLOTS_MAX; ++slot) {
        if (!store->scratch_subs[slot].loaded) {
            continue;
        }
        if (magpie_state_set_sub(&store->state, &store->scratch_subs[slot])) {
            ++accepted;
        }
    }
    store->state.sub_count = accepted;
    magpie_state_mark_data(&store->state, 0, used);
    const uint8_t count = store->state.index.count;
    xSemaphoreGive(store->lock);

    ESP_LOGI(TAG, "%s snapshot: %u subscriptions, %u payloads%s",
             is_ble ? "BLE" : "Wi-Fi", (unsigned)count, (unsigned)accepted,
             result.subs_failed ? " (some slots failed)" : "");
    return used;
}

void magpie_store_tick(magpie_store_t *store, uint32_t elapsed_s)
{
    if (!store || !store->lock) {
        return;
    }
    xSemaphoreTake(store->lock, portMAX_DELAY);
    magpie_state_tick(&store->state, store->state.age_s + elapsed_s);
    xSemaphoreGive(store->lock);
}

uint32_t magpie_store_next_delay_ms(magpie_store_t *store)
{
    if (!store) {
        return 1000;
    }
    const uint32_t interval_ms = store->config.refresh_interval_s * 1000u;
    if (store->prefer_wifi && store->ble_ready) {
        /* While Wi-Fi serves the display, wake up when BLE is worth a retry. */
        const int32_t remaining =
            (int32_t)(store->ble_next_attempt_ms - uptime_ms());
        if (remaining <= 0) {
            return 0;
        }
        return (uint32_t)remaining < interval_ms ? (uint32_t)remaining
                                                : interval_ms;
    }
    return interval_ms;
}
