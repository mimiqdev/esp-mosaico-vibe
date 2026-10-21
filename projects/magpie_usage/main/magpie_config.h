// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Magpie usage display configuration.
 *
 * Values are Kconfig defaults (menu "Magpie usage display") merged with NVS
 * overrides in magpie_config_load(). No transport or UI module reads Kconfig or
 * NVS on its own, so the sources stay swappable and testable.
 *
 * Nothing here is a Magpie gateway key: the device only talks to the local
 * companion, which owns the loopback Magpie credentials. The optional token
 * below is the companion's display bearer token.
 */

#define MAGPIE_HOST_MAX 64
#define MAGPIE_TOKEN_MAX 96
#define MAGPIE_PATH_MAX 64

typedef enum {
    /* BLE first, Wi-Fi after MAGPIE_BLE_FAILS_BEFORE_WIFI failed connects. */
    MAGPIE_TRANSPORT_AUTO = 0,
    MAGPIE_TRANSPORT_BLE_ONLY,
    MAGPIE_TRANSPORT_WIFI_ONLY,
} magpie_transport_mode_t;

typedef struct {
    magpie_transport_mode_t mode;

    /* BLE central */
    char ble_name_prefix[24];     /* advertised local name to prefer */
    uint16_t ble_scan_ms;         /* one scan window */
    uint16_t ble_mtu;             /* requested ATT MTU */
    uint8_t ble_fails_before_wifi;/* consecutive connect failures */

    /* Wi-Fi HTTP fallback */
    char wifi_host[MAGPIE_HOST_MAX]; /* companion LAN host; empty = disabled */
    uint16_t wifi_port;
    char wifi_token[MAGPIE_TOKEN_MAX]; /* optional bearer token, never logged */
    uint32_t wifi_timeout_ms;
    bool wifi_start_radio;        /* bring up STA from NVS sysmeta credentials */

    /* Refresh cadence */
    uint32_t refresh_interval_s;  /* pull period while BLE-connected */
    uint32_t idle_interval_s;     /* pull period when nothing changed */
    uint32_t stale_after_s;       /* STALE threshold (60 s) */
} magpie_config_t;

/** Fill *config with the compiled-in Kconfig defaults. */
void magpie_config_defaults(magpie_config_t *config);

/**
 * Load the effective configuration: Kconfig defaults overridden by values in
 * the NVS namespace "magpie" of the application NVS partition.
 *
 * Falls back to defaults (and logs) when NVS is unavailable. Never logs the
 * token.
 */
esp_err_t magpie_config_load(magpie_config_t *config);

/** Human-readable transport mode for logs. */
const char *magpie_transport_mode_name(magpie_transport_mode_t mode);
