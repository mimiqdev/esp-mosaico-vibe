// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "magpie_config.h"
#include "magpie_source.h"

/*
 * BLE central transport (NimBLE).
 *
 * Profile (documented in docs/protocol.md and produced by the omarchy
 * companion):
 *
 *   service 7a3d0001-9c4e-4b8a-9f31-6d6167706965
 *   INDEX   7a3d0002-...  read + notify  -> INDEX payload
 *   SUB     7a3d0003-...  write + read   -> one SUB payload per slot
 *
 * One "read a SUB" is: write the one-byte slot id, then read the
 * characteristic. The central subscribes to INDEX so the store can refresh as
 * soon as the sender has new data, and a periodic read remains the safety net.
 *
 * Only the central role is enabled (see sdkconfig.defaults) to keep RAM use
 * low next to Wi-Fi.
 */

typedef struct {
    char name_prefix[24];
    uint16_t scan_ms;
    uint16_t mtu;
} magpie_ble_config_t;

/** Central/source state; owned privately by magpie_ble.c. */
typedef struct magpie_ble_source magpie_ble_source_t;

/**
 * Initialise the NimBLE host and build the source vtable.
 *
 * The host is started once; repeated init calls are rejected. The first
 * open() performs the scan/connect/discover sequence.
 */
esp_err_t magpie_ble_source_init(const magpie_ble_config_t *config,
                                 magpie_source_t *out_source);

/** Release the connection and the source vtable. */
void magpie_ble_source_deinit(magpie_source_t *source);

/** True when a peer is connected and discovered. */
bool magpie_ble_source_connected(void);

/**
 * Value the last INDEX notification carried, if any.
 *
 * The store calls this after wait_changed() to avoid a redundant read.
 */
int magpie_ble_source_take_notification(char *buf, size_t cap);
