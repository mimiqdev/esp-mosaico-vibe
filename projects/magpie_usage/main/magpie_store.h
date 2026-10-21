// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "magpie_config.h"
#include "magpie_model.h"
#include "magpie_source.h"
#include "magpie_ui_state.h"

/*
 * Snapshot store and transport arbiter.
 *
 * The store is the single owner of the current snapshot: the refresh task
 * writes it under the mutex and the UI task copies it. That mutex is the only
 * shared mutable state in the application.
 *
 * Arbiter rules:
 *   - BLE first. After MAGPIE_DISPLAY_BLE_FAILS_BEFORE_WIFI consecutive
 *     failures the store prefers Wi-Fi, and it keeps retrying BLE on an
 *     exponential backoff (5 s .. 120 s) so a returning sender is picked up
 *     again automatically (magpie_store_ble_attempt_due()).
 *   - A successful BLE pull resets the preference and the backoff.
 *   - Wi-Fi only / BLE only modes come from Kconfig or NVS.
 */

#define MAGPIE_BACKOFF_MIN_MS 5000
#define MAGPIE_BACKOFF_MAX_MS 120000

typedef struct {
    magpie_config_t config;
    SemaphoreHandle_t lock;
    magpie_state_t state;

    /* Transport objects; only the refresh task drives them. */
    magpie_source_t ble_source;
    magpie_source_t wifi_source;
    bool ble_ready;
    bool wifi_ready;
    bool refresh_ble;   /* build the BLE source */
    bool refresh_wifi;  /* build the Wi-Fi source */
    bool prefer_wifi;
    uint8_t ble_failures;
    uint32_t backoff_ms;
    uint32_t ble_next_attempt_ms; /* app uptime in ms */

    /* Pull scratch, task-owned. */
    char index_buf[MAGPIE_INDEX_BYTES_MAX + 1];
    char sub_buf[MAGPIE_SUB_BYTES_MAX + 1];
    magpie_index_t scratch_index;
    magpie_sub_t scratch_subs[MAGPIE_SLOTS_MAX];
    uint8_t scratch_loaded;
} magpie_store_t;

/** Create the store; does not start any transport. */
esp_err_t magpie_store_init(magpie_store_t *store,
                            const magpie_config_t *config);

/** Register transports built by main.c (either may be NULL). */
void magpie_store_attach(magpie_store_t *store, magpie_source_t *ble,
                         magpie_source_t *wifi);

/** Copy the snapshot for rendering. Returns false before the first snapshot. */
bool magpie_store_snapshot(magpie_store_t *store, magpie_state_t *out);

/** Record the visible page so the copied snapshot keeps the selection. */
void magpie_store_set_page(magpie_store_t *store, uint8_t page);

/** True when a refresh should be attempted now (transport + backoff). */
bool magpie_store_refresh_due(magpie_store_t *store);

/**
 * Run one refresh with the arbiter's current choice.
 *
 * Returns the transport that produced the snapshot, or NONE on failure.
 * Called only from the refresh task.
 */
magpie_transport_t magpie_store_refresh(magpie_store_t *store);

/** Age the snapshot by elapsed_s; called from the refresh task. */
void magpie_store_tick(magpie_store_t *store, uint32_t elapsed_s);

/** True when the last snapshot came from BLE and the link still notifies. */
bool magpie_store_prefers_wifi(const magpie_store_t *store);

/** Milliseconds to wait before the next refresh. */
uint32_t magpie_store_next_delay_ms(magpie_store_t *store);
