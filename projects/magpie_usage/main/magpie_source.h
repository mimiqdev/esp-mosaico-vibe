// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "magpie_model.h"

/*
 * Transport interface for device-pull snapshots.
 *
 * The device does two request/response operations per refresh, whatever the
 * transport:
 *
 *   1. read the INDEX payload (count + glance summary + slot ids),
 *   2. read one SUB payload per slot.
 *
 * BLE implements this as "write the slot to the SUB characteristic, then read
 * it"; Wi-Fi implements it as "GET /v1/display/sub/<slot>". Neither transport
 * knows about the UI or about decoding.
 *
 * No ESP-IDF includes: the PC simulator and the host tests provide their own
 * fake source, and the pull cycle is shared verbatim.
 */

/** Result codes; negative values are transport failures. */
#define MAGPIE_SOURCE_OK 0
#define MAGPIE_SOURCE_ERR_TIMEOUT (-1)
#define MAGPIE_SOURCE_ERR_LINK (-2)
#define MAGPIE_SOURCE_ERR_PAYLOAD (-3)
#define MAGPIE_SOURCE_ERR_ARG (-4)

typedef struct magpie_source magpie_source_t;

struct magpie_source {
    /** Short name for logs and the header ("ble", "wifi"). */
    const char *name;

    /**
     * Bring the transport up (connect, subscribe, resolve host).
     * A successful open must be followed by close().
     */
    int (*open)(magpie_source_t *self);

    /** Release the transport. Safe to call when open failed. */
    void (*close)(magpie_source_t *self);

    /** Read the INDEX payload into buf. Returns the byte count or an error. */
    int (*read_index)(magpie_source_t *self, char *buf, size_t cap);

    /** Read the SUB payload for slot into buf. */
    int (*read_sub)(magpie_source_t *self, uint8_t slot, char *buf, size_t cap);

    /**
     * Optional: wait up to timeout_ms for a change notification from the
     * sender. Returns true when new data may be available. Implementations
     * may sleep the full timeout and return false.
     */
    bool (*wait_changed)(magpie_source_t *self, uint32_t timeout_ms);

    /** Implementation-owned state. */
    void *impl;
};

/** True when the byte count is a plausible payload size. */
static inline bool magpie_source_len_ok(int len, size_t cap)
{
    return len > 0 && (size_t)len <= cap;
}
