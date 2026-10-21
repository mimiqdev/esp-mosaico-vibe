// SPDX-License-Identifier: Apache-2.0

#include "magpie_ui_state.h"

#include <stdio.h>
#include <string.h>

#include "magpie_format.h"
#include "magpie_parse.h"

#define SECONDS_PER_MINUTE 60
#define SECONDS_PER_HOUR 3600
#define SECONDS_PER_DAY 86400

/* Sender placeholders that should not reach the screen as a "plan". */
static const char *const kUnusablePlans[] = {"", "-", "?", "none", "null",
                                             "unknown", "n/a"};

static void write_text(char *out, size_t size, const char *text)
{
    if (!out || size == 0) {
        return;
    }
    size_t i = 0;
    for (; i + 1 < size && text && text[i]; ++i) {
        out[i] = text[i];
    }
    out[i] = '\0';
}

void magpie_state_init(magpie_state_t *state)
{
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    magpie_index_clear(&state->index);
    for (uint8_t slot = 0; slot < MAGPIE_SLOTS_MAX; ++slot) {
        magpie_sub_clear(&state->subs[slot], slot);
    }
    state->transport = MAGPIE_TRANSPORT_NONE;
    state->pages_visible = 1;
}

void magpie_state_set_index(magpie_state_t *state, const magpie_index_t *index)
{
    if (!state || !index) {
        return;
    }
    state->index = *index;
    if (state->index.count == 0) {
        state->pages_visible = 1;
    } else if (state->index.count > MAGPIE_SLOTS_MAX) {
        state->pages_visible = MAGPIE_SLOTS_MAX;
    } else {
        state->pages_visible = state->index.count;
    }
    /* Forget payloads for slots the sender no longer reports so a shrinking
     * list cannot show a stale page. */
    for (uint8_t slot = state->pages_visible; slot < MAGPIE_SLOTS_MAX; ++slot) {
        if (state->subs[slot].loaded) {
            magpie_sub_clear(&state->subs[slot], slot);
        }
    }
    uint8_t loaded = 0;
    for (uint8_t slot = 0; slot < MAGPIE_SLOTS_MAX; ++slot) {
        if (state->subs[slot].loaded) {
            ++loaded;
        }
    }
    state->sub_count = loaded;
    magpie_state_set_page(state, state->page);
}

bool magpie_state_set_sub(magpie_state_t *state, const magpie_sub_t *sub)
{
    if (!state || !sub) {
        return false;
    }
    if (sub->index >= state->pages_visible) {
        return false;
    }
    const bool was_loaded = state->subs[sub->index].loaded;
    state->subs[sub->index] = *sub;
    if (!was_loaded) {
        ++state->sub_count;
    }
    return true;
}

void magpie_state_mark_data(magpie_state_t *state, uint32_t age_s,
                            magpie_transport_t transport)
{
    if (!state) {
        return;
    }
    state->age_s = age_s;
    state->ever_connected = true;
    if (transport != MAGPIE_TRANSPORT_NONE) {
        state->transport = transport;
    }
}

void magpie_state_set_page(magpie_state_t *state, uint8_t page)
{
    if (!state) {
        return;
    }
    const uint32_t limit = state->pages_visible > 1 ? state->pages_visible - 1 : 0;
    state->page = (uint8_t)(page > limit ? limit : page);
}

void magpie_state_tick(magpie_state_t *state, uint32_t age_s)
{
    if (!state) {
        return;
    }
    state->age_s = age_s;
    if (state->transport == MAGPIE_TRANSPORT_NONE) {
        /* Nothing has ever arrived: stay in CONNECTING rather than claiming a
         * link that has not produced data. */
        return;
    }
}

magpie_link_t magpie_state_link(const magpie_state_t *state)
{
    if (!state || state->transport == MAGPIE_TRANSPORT_NONE) {
        return MAGPIE_LINK_CONNECTING;
    }
    if (state->age_s >= MAGPIE_STALE_AFTER_S * 3) {
        return MAGPIE_LINK_OFFLINE;
    }
    if (state->age_s >= MAGPIE_STALE_AFTER_S) {
        return MAGPIE_LINK_STALE;
    }
    return MAGPIE_LINK_LIVE;
}

const char *magpie_link_name(magpie_link_t link)
{
    switch (link) {
    case MAGPIE_LINK_LIVE:
        return "LIVE";
    case MAGPIE_LINK_STALE:
        return "STALE";
    case MAGPIE_LINK_OFFLINE:
        return "OFFLINE";
    case MAGPIE_LINK_CONNECTING:
    default:
        return "CONNECTING";
    }
}

void magpie_format_countdown(int64_t reset_epoch_s, int64_t now_epoch_s,
                             char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    if (reset_epoch_s <= 0) {
        write_text(out, size, "--");
        return;
    }
    int64_t delta = reset_epoch_s - now_epoch_s;
    if (delta <= 0) {
        write_text(out, size, "now");
        return;
    }
    if (delta >= SECONDS_PER_DAY) {
        snprintf(out, size, "%lldd %lldh", (long long)(delta / SECONDS_PER_DAY),
                 (long long)((delta % SECONDS_PER_DAY) / SECONDS_PER_HOUR));
    } else if (delta >= SECONDS_PER_HOUR) {
        snprintf(out, size, "%lldh %02lldm", (long long)(delta / SECONDS_PER_HOUR),
                 (long long)((delta % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE));
    } else if (delta >= SECONDS_PER_MINUTE) {
        snprintf(out, size, "%lldm", (long long)(delta / SECONDS_PER_MINUTE));
    } else {
        write_text(out, size, "<1m");
    }
}

void magpie_format_plan(const char *plan, char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    if (!plan) {
        out[0] = '\0';
        return;
    }
    for (size_t i = 0; i < sizeof(kUnusablePlans) / sizeof(kUnusablePlans[0]);
         ++i) {
        if (strcmp(plan, kUnusablePlans[i]) == 0) {
            out[0] = '\0';
            return;
        }
    }
    write_text(out, size, plan);
    magpie_upper(out, out, size);
}

static void format_window_label(const magpie_win_t *window, char *out,
                                size_t size)
{
    if (!out || size == 0) {
        return;
    }
    if (window->label[0] == '\0') {
        write_text(out, size, "usage");
        return;
    }
    /* The sender sends compact labels ("5h", "7d", "cred"); expand the two
     * that read badly on a page title. */
    if (strcmp(window->label, "5h") == 0) {
        write_text(out, size, "5-HOUR WINDOW");
    } else if (strcmp(window->label, "7d") == 0) {
        write_text(out, size, "7-DAY WINDOW");
    } else if (strcmp(window->label, "24h") == 0) {
        write_text(out, size, "24-HOUR WINDOW");
    } else if (strcmp(window->label, "30d") == 0) {
        write_text(out, size, "30-DAY WINDOW");
    } else if (strcmp(window->label, "cred") == 0) {
        write_text(out, size, "CREDITS USED");
    } else if (strcmp(window->label, "od") == 0) {
        write_text(out, size, "ON-DEMAND");
    } else {
        char upper[MAGPIE_WIN_LABEL_MAX];
        magpie_upper(window->label, upper, sizeof(upper));
        write_text(out, size, upper);
    }
}

bool magpie_page_text(const magpie_state_t *state, uint8_t slot,
                      magpie_page_text_t *out)
{
    if (!state || !out || slot >= MAGPIE_SLOTS_MAX) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    const magpie_sub_t *sub = &state->subs[slot];
    const bool in_range = slot < state->pages_visible;
    if (!in_range) {
        out->empty = true;
        write_text(out->name, sizeof(out->name), "No subscription");
        return false;
    }
    if (!sub->loaded) {
        out->empty = true;
        write_text(out->name, sizeof(out->name), "Loading...");
        return false;
    }
    write_text(out->name, sizeof(out->name),
               sub->name[0] ? sub->name : sub->provider);
    magpie_format_plan(sub->plan, out->plan, sizeof(out->plan));
    out->error = sub->error;
    out->window_count = sub->window_count;
    out->empty = sub->window_count == 0 && !sub->error;
    for (uint8_t i = 0; i < sub->window_count && i < MAGPIE_WIN_MAX; ++i) {
        const magpie_win_t *window = &sub->windows[i];
        format_window_label(window, out->window_label[i],
                            sizeof(out->window_label[i]));
        snprintf(out->window_used[i], sizeof(out->window_used[i]), "%u%%",
                 (unsigned)window->used_percent);
        out->window_used_percent[i] = window->used_percent;
        magpie_format_countdown(window->reset_epoch_s, state->index.sender_epoch_s,
                                out->window_reset[i],
                                sizeof(out->window_reset[i]));
    }
    return true;
}

void magpie_header_text(const magpie_state_t *state, magpie_header_text_t *out)
{
    if (!state || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    const magpie_link_t link = magpie_state_link(state);
    write_text(out->status, sizeof(out->status), magpie_link_name(link));
    out->stale = link != MAGPIE_LINK_LIVE;
    switch (state->transport) {
    case MAGPIE_TRANSPORT_BLE:
        write_text(out->transport, sizeof(out->transport), "BLE");
        break;
    case MAGPIE_TRANSPORT_WIFI:
        write_text(out->transport, sizeof(out->transport), "WIFI");
        break;
    case MAGPIE_TRANSPORT_NONE:
    default:
        out->transport[0] = '\0';
        break;
    }

    if (state->index.summary.valid && state->transport != MAGPIE_TRANSPORT_NONE) {
        char countdown[24];
        magpie_format_countdown(state->index.summary.reset_epoch_s,
                                state->index.sender_epoch_s, countdown,
                                sizeof(countdown));
        /* The name is already bounded by the decoder; the buffer keeps room
         * for a full name, window, percent and countdown. */
        snprintf(out->tightest, sizeof(out->tightest), "%s %s %u%% - %s",
                 state->index.summary.name, state->index.summary.window,
                 (unsigned)state->index.summary.used_percent, countdown);
    } else if (state->transport == MAGPIE_TRANSPORT_NONE) {
        write_text(out->tightest, sizeof(out->tightest),
                   "waiting for first snapshot");
    } else {
        write_text(out->tightest, sizeof(out->tightest), "no quota data");
    }

    if (state->pages_visible > 1) {
        snprintf(out->counter, sizeof(out->counter), "%u/%u",
                 (unsigned)(state->page + 1), (unsigned)state->pages_visible);
    } else {
        out->counter[0] = '\0';
    }
}
