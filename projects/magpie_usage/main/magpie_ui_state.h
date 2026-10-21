// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "magpie_model.h"

/*
 * Presentation helpers and the pure display state machine for the Magpie
 * usage pages.
 *
 * No ESP-IDF includes and no GSP calls: magpie_ui.c owns the GSP setters, this
 * layer owns "what should be on screen". That split lets the host tests and
 * the PC simulator assert every string and page transition without a display.
 */

/** Largest text this module produces (name + suffix, counters). */
#define MAGPIE_TEXT_MAX 40

/** Connection state as shown in the header. */
typedef enum {
    MAGPIE_LINK_CONNECTING = 0,
    MAGPIE_LINK_LIVE,
    MAGPIE_LINK_STALE,
    MAGPIE_LINK_OFFLINE,
} magpie_link_t;

/** Which transport produced the current data (header hint). */
typedef enum {
    MAGPIE_TRANSPORT_NONE = 0,
    MAGPIE_TRANSPORT_BLE,
    MAGPIE_TRANSPORT_WIFI,
} magpie_transport_t;

/**
 * Everything the pages need.
 *
 * Ownership: magpie_store owns the snapshot under a mutex; the UI copies it
 * with magpie_state_apply() on the UI task and then renders from the copy.
 */
typedef struct {
    magpie_index_t index;
    magpie_sub_t subs[MAGPIE_SLOTS_MAX];
    uint8_t sub_count;        /* slots with a loaded SUB payload */
    magpie_transport_t transport;
    bool ever_connected;
    uint32_t age_s;           /* seconds since the last accepted snapshot */
    uint32_t pages_visible;   /* pages actually shown, >= 1 */
    uint8_t page;             /* currently visible page */
} magpie_state_t;

/** Strings for one page, produced by magpie_page_text(). */
typedef struct {
    char name[MAGPIE_NAME_MAX + 8];
    char plan[MAGPIE_PLAN_MAX + 4];
    char window_label[MAGPIE_WIN_MAX][MAGPIE_WIN_LABEL_MAX + 12];
    char window_used[MAGPIE_WIN_MAX][8];
    char window_reset[MAGPIE_WIN_MAX][24];
    uint8_t window_used_percent[MAGPIE_WIN_MAX];
    uint8_t window_count;
    bool error;
    bool empty;
} magpie_page_text_t;

/** Header strings. */
typedef struct {
    char status[16];
    char transport[8];
    char tightest[96];
    char counter[16];
    bool stale;
} magpie_header_text_t;

/** Reset to the pre-connection state. */
void magpie_state_init(magpie_state_t *state);

/**
 * Install a decoded INDEX. Slots beyond the new count keep their old payload
 * but stop being rendered (pages_visible follows the count).
 */
void magpie_state_set_index(magpie_state_t *state, const magpie_index_t *index);

/** Install a decoded SUB; ignored when the slot is outside the current count. */
bool magpie_state_set_sub(magpie_state_t *state, const magpie_sub_t *sub);

/** Record an accepted snapshot time (monotonic seconds) and the transport. */
void magpie_state_mark_data(magpie_state_t *state, uint32_t age_s,
                            magpie_transport_t transport);

/** Set the visible page (clamped to pages_visible - 1). */
void magpie_state_set_page(magpie_state_t *state, uint8_t page);

/** Age the snapshot; call once per UI tick. */
void magpie_state_tick(magpie_state_t *state, uint32_t age_s);

/** Current link state derived from connectivity and age. */
magpie_link_t magpie_state_link(const magpie_state_t *state);

/** Fill one page's strings. Returns false when the slot has no data. */
bool magpie_page_text(const magpie_state_t *state, uint8_t slot,
                      magpie_page_text_t *out);

/** Fill the header strings. */
void magpie_header_text(const magpie_state_t *state, magpie_header_text_t *out);

/** Upper-case status label ("LIVE", "STALE", ...). */
const char *magpie_link_name(magpie_link_t link);

/* --- small formatters (also used by the host tests) ------------------ */

/** "<1m" / "42m" / "3h 05m" / "2d 8h" / "now"; 0 epoch means unknown. */
void magpie_format_countdown(int64_t reset_epoch_s, int64_t now_epoch_s,
                             char *out, size_t size);

/** "Untitled" -> "" ; also collapses the sender's ASCII placeholders. */
void magpie_format_plan(const char *plan, char *out, size_t size);
