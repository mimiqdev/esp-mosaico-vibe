// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Magpie usage configuration.
 *
 * The values here are provided by the caller (Kconfig defaults merged with NVS
 * overrides in magpie_config_load()). No module may read Kconfig or NVS on its
 * own so the data path stays swappable between a direct gateway, a relay or a
 * test double.
 */

#define MAGPIE_BASE_URL_MAX 160
#define MAGPIE_TOKEN_MAX 128
#define MAGPIE_FILTER_MAX 96
#define MAGPIE_PROVIDER_COUNT_MAX 24
#define MAGPIE_WINDOW_COUNT_MAX 6
#define MAGPIE_NAME_MAX 40
#define MAGPIE_SERIES_POINTS 7

typedef struct {
    /* Base gateway URL, e.g. "http://omarchy:3425" or "https://relay/". A
     * trailing slash is optional. */
    char base_url[MAGPIE_BASE_URL_MAX];
    /* Magpie gateway key sent as "Authorization: Bearer <token>". Empty for a
     * loopback/relay endpoint that does not require one. Never log this. */
    char auth_token[MAGPIE_TOKEN_MAX];
    /* Comma-separated provider prefix filter, e.g. "office/". Empty = all. */
    char provider_filter[MAGPIE_FILTER_MAX];
    /* Poll period in seconds. */
    uint32_t refresh_interval_s;
    /* Age after which the snapshot is reported as stale. */
    uint32_t stale_after_s;
    /* Per-request network timeout. */
    uint32_t http_timeout_ms;
} magpie_config_t;

/** Fill *config with the compiled-in Kconfig defaults. */
void magpie_config_defaults(magpie_config_t *config);

/**
 * Load the effective configuration: Kconfig defaults overridden by any values
 * present in the NVS namespace "magpie".
 *
 * Falls back to defaults (and logs) when NVS is unavailable. Never logs the
 * token.
 */
esp_err_t magpie_config_load(magpie_config_t *config);

/**
 * Persist the runtime-overridable fields (base URL, token, filter, intervals)
 * into NVS so a relay or token rotation needs no rebuild.
 */
esp_err_t magpie_config_save(const magpie_config_t *config);
