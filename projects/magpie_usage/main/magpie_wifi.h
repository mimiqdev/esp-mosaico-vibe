// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "magpie_config.h"
#include "magpie_source.h"

/*
 * Wi-Fi fallback: HTTP GET against the companion's read-only display server.
 *
 *   GET http://<host>:<port>/v1/display/index
 *   GET http://<host>:<port>/v1/display/sub/<slot>
 *
 * Configuration comes from magpie_config (Kconfig defaults merged with NVS),
 * never from this module. The bearer token is optional and never logged.
 *
 * This source never contacts Magpie itself: Magpie requires caller keys off
 * loopback, and those must not exist on the device.
 */

typedef struct {
    char host[MAGPIE_HOST_MAX];       /* companion LAN host; empty disables */
    uint16_t port;
    char token[MAGPIE_TOKEN_MAX];     /* optional display bearer token */
    uint32_t timeout_ms;
} magpie_wifi_config_t;

/** Source state; owned privately by magpie_wifi.c. */
typedef struct magpie_wifi_source magpie_wifi_source_t;

/**
 * Initialise the source.
 *
 * When @p start_radio is true and the sysmeta Wi-Fi credentials exist, the STA
 * interface is started here (the first pull then waits for an IP). Otherwise
 * the source assumes something else already brought Wi-Fi up.
 */
esp_err_t magpie_wifi_source_init(const magpie_wifi_config_t *config,
                                  bool start_radio,
                                  magpie_source_t *out_source);

/** Stop waiting on Wi-Fi state (does not stop the driver). */
void magpie_wifi_source_deinit(magpie_source_t *source);

/** True when the configured host is empty. */
bool magpie_wifi_source_configured(void);

/** True once the STA has an IP address. */
bool magpie_wifi_source_has_ip(void);
