// SPDX-License-Identifier: Apache-2.0

/*
 * PC simulator backend for the Magpie usage display.
 *
 * The simulator has no BLE and no companion, so this adapter plays the sender:
 * it loads the generated payload fixtures (tests/host/fixtures/payloads, built
 * from a captured Magpie document by tools/make_payloads.py), decodes them with
 * the same magpie_parse.c the device uses, and drives magpie_ui exactly like
 * the firmware's refresh task does.
 *
 * It also advances the snapshot age so LIVE -> STALE -> OFFLINE can be observed
 * in the simulator without waiting a minute per step.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gsp_sim_bridge.h"
#include "magpie_model.h"
#include "magpie_parse.h"
#include "magpie_ui.h"
#include "magpie_ui_state.h"

#ifndef MAGPIE_SIM_FIXTURES
#define MAGPIE_SIM_FIXTURES "tests/host/fixtures/payloads"
#endif

/* Seconds of simulated time per real second; 1 keeps the real cadence. */
#ifndef MAGPIE_SIM_TIME_SCALE
#define MAGPIE_SIM_TIME_SCALE 1
#endif

/* Stop ageing after this many simulated seconds (0 = never). */
#ifndef MAGPIE_SIM_STALE_AFTER_S
#define MAGPIE_SIM_STALE_AFTER_S 0
#endif

/* Maximum subscriptions to load from the fixtures (all by default). */
/* Timer period for the simulated refresh/age loop. */
#ifndef MAGPIE_SIM_TICK_MS
#define MAGPIE_SIM_TICK_MS 200
#endif

#ifndef MAGPIE_SIM_MAX_SUBS
#define MAGPIE_SIM_MAX_SUBS MAGPIE_SLOTS_MAX
#endif

static magpie_ui_t s_ui;
static magpie_state_t s_state;
static void *s_timer;
static unsigned s_tick;

static char *read_file(const char *path, size_t *out_len)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "magpie_sim: cannot open %s\n", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    const long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *buffer = malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    const size_t read = fread(buffer, 1, (size_t)size, file);
    fclose(file);
    buffer[read] = '\0';
    if (out_len) {
        *out_len = read;
    }
    return buffer;
}

/* Loads the payload fixtures into the display state. */
static bool load_fixtures(magpie_state_t *state)
{
    magpie_state_init(state);
    char path[512];
    size_t len = 0;
    snprintf(path, sizeof(path), "%s/index.json", MAGPIE_SIM_FIXTURES);
    char *body = read_file(path, &len);
    if (!body) {
        return false;
    }
    magpie_index_t index;
    const bool ok = magpie_decode_index(body, len, &index) == MAGPIE_OK;
    free(body);
    if (!ok) {
        fprintf(stderr, "magpie_sim: index fixture is not a v1 INDEX payload\n");
        return false;
    }
    if (index.count > MAGPIE_SIM_MAX_SUBS) {
        index.count = MAGPIE_SIM_MAX_SUBS;
        index.id_count = MAGPIE_SIM_MAX_SUBS;
        for (uint8_t i = 0; i < MAGPIE_SIM_MAX_SUBS; ++i) {
            index.ids[i] = i;
        }
    }
    magpie_state_set_index(state, &index);

    for (uint8_t slot = 0; slot < index.count; ++slot) {
        snprintf(path, sizeof(path), "%s/sub-%u.json", MAGPIE_SIM_FIXTURES,
                 (unsigned)slot);
        char *sub_body = read_file(path, &len);
        if (!sub_body) {
            continue;
        }
        magpie_sub_t sub;
        if (magpie_decode_sub(sub_body, len, slot, &sub) == MAGPIE_OK) {
            magpie_state_set_sub(state, &sub);
        }
        free(sub_body);
    }
    magpie_state_mark_data(state, 0, MAGPIE_TRANSPORT_BLE);
    fprintf(stderr, "magpie_sim: loaded %u subscriptions from %s\n",
            (unsigned)index.count, MAGPIE_SIM_FIXTURES);
    return true;
}

/*
 * sim_bridge delivers timer callbacks from inside the render poll and does not
 * tolerate setters submitted from another thread, so the simulator drives the
 * UI from one timer: a bounded amount of work per tick, exactly like the
 * firmware's UI task draining its own work in small slices.
 */
static void tick(esp_gsp_handle_t ui, void *ctx)
{
    (void)ui;
    (void)ctx;
    ++s_tick;
    /* Fill in one more page while a snapshot is still being applied. */
    magpie_ui_pump(&s_ui);
    /* Age once per simulated second and repaint the header/neighbourhood. */
    if (s_tick % (1000 / MAGPIE_SIM_TICK_MS)) {
        return;
    }
    uint32_t age = s_state.age_s + (uint32_t)MAGPIE_SIM_TIME_SCALE;
#if MAGPIE_SIM_STALE_AFTER_S > 0
    if (age > MAGPIE_SIM_STALE_AFTER_S) {
        age = MAGPIE_SIM_STALE_AFTER_S;
    }
#endif
    if (MAGPIE_SIM_TIME_SCALE) {
        magpie_state_tick(&s_state, age);
    }
    magpie_ui_show(&s_ui, &s_state, magpie_ui_page(&s_ui));
}

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    const esp_gsp_err_t err = magpie_ui_init(ui, &s_ui);
    if (err != ESP_GSP_OK) {
        return err;
    }
    if (!load_fixtures(&s_state)) {
        /* Keep the empty state: the UI then shows CONNECTING, which is also a
         * state worth previewing. */
        magpie_state_init(&s_state);
    }
#ifdef MAGPIE_UI_DEBUG
    fprintf(stderr, "magpie_sim: link=%s transport=%d pages=%u\n",
            magpie_link_name(magpie_state_link(&s_state)),
            (int)s_state.transport, (unsigned)s_state.pages_visible);
#endif
    magpie_ui_show(&s_ui, &s_state, 0);
    s_timer = esp_gsp_timer_create(ui, MAGPIE_SIM_TICK_MS, tick, NULL);
    if (!s_timer) {
        return ESP_GSP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    if (s_timer) {
        (void)esp_gsp_timer_delete(ui, s_timer);
        s_timer = NULL;
    }
    magpie_ui_deinit(&s_ui);
    fprintf(stderr, "magpie_sim: ticks=%u pages=%u subs=%u last_error=%d\n",
            s_tick, (unsigned)s_state.pages_visible,
            (unsigned)s_state.sub_count, (int)s_ui.last_error);
}
