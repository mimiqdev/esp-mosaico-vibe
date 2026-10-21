// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <stdio.h>

#include "esp_gsp.h"
#include "magpie_ui_state.h"

/*
 * GSP binding for the Magpie usage pages.
 *
 * magpie_ui.c is the only file that talks to GSP: it pushes magpie_state_t
 * strings into the generated setters, owns the PageFlow, and forwards page
 * changes back to the caller so refresh scheduling can prioritise the page the
 * user is looking at.
 *
 * The same file is compiled into the device firmware and into the PC
 * simulator backend (pc/platform_pc.c), so the scene and the refresh logic run
 * unchanged in both.
 */

#define MAGPIE_UI_PAGE_MAX MAGPIE_SLOTS_MAX

typedef struct {
    esp_gsp_handle_t ui;
    magpie_state_t state;
    esp_gsp_err_t last_error;
    uint8_t page;             /* page the PageFlow is showing */
    uint8_t rendered_pages;   /* pages whose widgets are currently populated */
    bool dirty;               /* a render was requested but not applied */
    /* Copy of what is already on screen, so an unchanged page or header is
     * not re-sent: GSP queues setters with bounded capacity, and a full
     * re-render every tick would drop the newest updates. */
    magpie_page_text_t shown_page[MAGPIE_SLOTS_MAX];
    bool shown_page_valid[MAGPIE_SLOTS_MAX];
    bool shown_page_count[MAGPIE_SLOTS_MAX];  /* page is inside the live range */
    magpie_header_text_t shown_header;
    bool shown_header_valid;
    /* Pages still to (re)render. GSP queues setters with bounded capacity, so
     * a large snapshot is applied over several pump() calls instead of one
     * burst that would be dropped. */
    uint8_t pending_first;
    uint8_t pending_count;
    uint8_t pending_low;    /* first slot to actually repaint */
    uint8_t pending_high;   /* one past the last slot to repaint */
} magpie_ui_t;

/** Push the initial (empty) state and select page 0. */
esp_gsp_err_t magpie_ui_init(esp_gsp_handle_t ui, magpie_ui_t *out);

/** Pages each pump() call writes before waiting for the render task. */
#define MAGPIE_UI_PAGES_PER_PUMP 1
/** Upper bound for the per-page flush. */
#define MAGPIE_UI_FLUSH_TIMEOUT_MS 120

/** Release timers/subscriptions. Safe on a partially initialised handle. */
void magpie_ui_deinit(magpie_ui_t *self);

/**
 * Replace the displayed snapshot and re-render.
 *
 * `page` is the page to show; it is clamped to the visible range. Called from
 * the application task, never from a GSP callback.
 */
void magpie_ui_show(magpie_ui_t *self, const magpie_state_t *state,
                    uint8_t page);

/**
 * Apply up to MAGPIE_UI_PAGES_PER_PUMP pending pages.
 *
 * Call from the UI task until magpie_ui_dirty() is false; leaving a small gap
 * between calls lets the render task drain its update queue.
 */
void magpie_ui_pump(magpie_ui_t *self);

/** Re-render the header only (link age changes every second). */
void magpie_ui_refresh_status(magpie_ui_t *self);

/**
 * Adopt the page PageFlow is actually showing.
 *
 * The user swipes; the framework commits. Without this the counter and the
 * page dots would keep reporting the page the application last requested.
 * Returns true when the page changed.
 */
bool magpie_ui_sync_page(magpie_ui_t *self);

/** Current page, clamped to the visible range. */
uint8_t magpie_ui_page(const magpie_ui_t *self);

/** True when a render was requested since the last successful show(). */
bool magpie_ui_dirty(const magpie_ui_t *self);
