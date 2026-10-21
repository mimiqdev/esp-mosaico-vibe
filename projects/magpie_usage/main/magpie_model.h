// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Portable model for the device-pull display protocol (protocol v1).
 *
 * No ESP-IDF includes: the payload parser, the store and the UI rendering
 * strings are compiled natively for the host tests and the PC simulator, and
 * exactly the same sources run on the device.
 *
 * One page == one subscription. The sender (omarchy companion) derives the
 * payload; the device only decodes it, so every value here is already in the
 * shape the UI draws.
 */

/** Pages (subscriptions) the firmware can hold. 13 subscriptions today. */
#define MAGPIE_SLOTS_MAX 16
/** Quota windows per subscription kept for display. */
#define MAGPIE_WIN_MAX 3
#define MAGPIE_PROVIDER_MAX 24
#define MAGPIE_NAME_MAX 28
#define MAGPIE_PLAN_MAX 14
#define MAGPIE_WIN_LABEL_MAX 8

#define MAGPIE_PROTOCOL_VERSION 1
/** Wire limits shared with the companion (see docs/protocol.md). */
#define MAGPIE_SUB_BYTES_MAX 180
#define MAGPIE_INDEX_BYTES_MAX 180
/** A snapshot older than this many seconds renders as STALE. */
#define MAGPIE_STALE_AFTER_S 60
/** Days of history kept for the small activity strip on page 1. */

/** One quota window: short label, used percent and reset epoch. */
typedef struct {
    char label[MAGPIE_WIN_LABEL_MAX];
    uint8_t used_percent;   /* 0..100, integer as sent by the companion */
    int64_t reset_epoch_s;  /* 0 = unknown */
} magpie_win_t;

/** One subscription == one swipe page. */
typedef struct {
    uint8_t index;                      /* slot/id as sent by the companion */
    char provider[MAGPIE_PROVIDER_MAX];
    char name[MAGPIE_NAME_MAX];
    char plan[MAGPIE_PLAN_MAX];
    bool error;                         /* signed out / provider error */
    magpie_win_t windows[MAGPIE_WIN_MAX];
    uint8_t window_count;
    bool loaded;                        /* SUB payload received for this slot */
} magpie_sub_t;

/** The glance summary carried by INDEX: the tightest window overall. */
typedef struct {
    int valid;
    int slot;
    char name[MAGPIE_NAME_MAX];
    char window[MAGPIE_WIN_LABEL_MAX];
    uint8_t used_percent;
    int64_t reset_epoch_s;
} magpie_summary_t;

/** Decoded INDEX payload plus the slot ids it announced. */
typedef struct {
    uint8_t version;
    int64_t sender_epoch_s;
    uint8_t count;
    magpie_summary_t summary;
    uint8_t ids[MAGPIE_SLOTS_MAX];
    uint8_t id_count;
} magpie_index_t;
