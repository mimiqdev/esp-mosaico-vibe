// SPDX-License-Identifier: Apache-2.0

#include "magpie_parse.h"

#include <string.h>

#include "magpie_json.h"

/* Reads a string, guaranteeing NUL termination inside the field. */
static bool read_text(mjson_t *json, char *out, size_t size, size_t max)
{
    if (!mjson_read_string(json, out, size)) {
        out[0] = '\0';
        return false;
    }
    if (max < size) {
        out[max - 1] = '\0';
    }
    return true;
}

static int read_int(mjson_t *json, int fallback)
{
    double value = 0;
    mjson_read_number_lenient(json, &value);
    if (value < -2147483648.0 || value > 2147483647.0) {
        return fallback;
    }
    return (int)value;
}

static void read_sub_window(mjson_t *json, magpie_win_t *window)
{
    char key[16];
    memset(window, 0, sizeof(*window));
    if (!mjson_enter(json)) {
        return;
    }
    while (mjson_next_key(json, key, sizeof(key))) {
        if (strcmp(key, "n") == 0) {
            read_text(json, window->label, sizeof(window->label),
                      MAGPIE_WIN_LABEL_MAX);
        } else if (strcmp(key, "u") == 0) {
            int used = read_int(json, 0);
            window->used_percent = (uint8_t)(used < 0 ? 0 : (used > 100 ? 100 : used));
        } else if (strcmp(key, "r") == 0) {
            window->reset_epoch_s = (int64_t)read_int(json, 0);
            if (window->reset_epoch_s < 0) {
                window->reset_epoch_s = 0;
            }
        } else if (!mjson_skip(json)) {
            break;
        }
    }
    (void)mjson_leave(json);
}

static void read_summary(mjson_t *json, magpie_summary_t *summary)
{
    char key[16];
    memset(summary, 0, sizeof(*summary));
    if (!mjson_at_object(json) || !mjson_enter(json)) {
        (void)mjson_skip(json);
        return;
    }
    while (mjson_next_key(json, key, sizeof(key))) {
        if (strcmp(key, "i") == 0) {
            summary->slot = read_int(json, -1);
            summary->valid = summary->slot >= 0;
        } else if (strcmp(key, "n") == 0) {
            read_text(json, summary->name, sizeof(summary->name),
                      MAGPIE_NAME_MAX);
        } else if (strcmp(key, "w") == 0) {
            read_text(json, summary->window, sizeof(summary->window),
                      MAGPIE_WIN_LABEL_MAX);
        } else if (strcmp(key, "u") == 0) {
            int used = read_int(json, 0);
            summary->used_percent =
                (uint8_t)(used < 0 ? 0 : (used > 100 ? 100 : used));
        } else if (strcmp(key, "r") == 0) {
            summary->reset_epoch_s = (int64_t)read_int(json, 0);
        } else if (!mjson_skip(json)) {
            break;
        }
    }
    (void)mjson_leave(json);
}

void magpie_index_clear(magpie_index_t *index)
{
    if (!index) {
        return;
    }
    memset(index, 0, sizeof(*index));
    index->summary.slot = -1;
}

int magpie_decode_index(const char *body, size_t len, magpie_index_t *out)
{
    if (!body || !out || len == 0) {
        return MAGPIE_ERR_INVALID_ARG;
    }
    if (len > MAGPIE_INDEX_BYTES_MAX * 2) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }
    mjson_t json;
    mjson_init(&json, body, len);
    if (!mjson_validate(&json) || !mjson_at_object(&json)) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }

    magpie_index_t index;
    magpie_index_clear(&index);
    char key[16];
    if (!mjson_enter(&json)) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }
    while (mjson_next_key(&json, key, sizeof(key))) {
        if (strcmp(key, "v") == 0) {
            index.version = (uint8_t)read_int(&json, 0);
        } else if (strcmp(key, "t") == 0) {
            index.sender_epoch_s = (int64_t)read_int(&json, 0);
        } else if (strcmp(key, "c") == 0) {
            int count = read_int(&json, 0);
            index.count = (uint8_t)(count < 0 ? 0
                                             : (count > MAGPIE_SLOTS_MAX
                                                    ? MAGPIE_SLOTS_MAX
                                                    : count));
        } else if (strcmp(key, "lo") == 0) {
            read_summary(&json, &index.summary);
        } else if (strcmp(key, "ids") == 0) {
            if (!mjson_enter(&json)) {
                if (!mjson_skip(&json)) {
                    break;
                }
                continue;
            }
            while (mjson_next_element(&json)) {
                const int id = read_int(&json, -1);
                if (id < 0 || id > 255) {
                    if (!mjson_skip(&json)) {
                        break;
                    }
                    continue;
                }
                if (index.id_count < MAGPIE_SLOTS_MAX) {
                    index.ids[index.id_count++] = (uint8_t)id;
                }
            }
            (void)mjson_leave(&json);
        } else if (!mjson_skip(&json)) {
            break;
        }
    }
    (void)mjson_leave(&json);

    if (index.version != MAGPIE_PROTOCOL_VERSION) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }
    /* The count is authoritative; a truncated ids array only limits which
     * slots the transports will fetch. */
    if (index.id_count == 0 && index.count > 0) {
        for (uint8_t i = 0; i < index.count && i < MAGPIE_SLOTS_MAX; ++i) {
            index.ids[index.id_count++] = i;
        }
    }
    *out = index;
    return MAGPIE_OK;
}

void magpie_sub_clear(magpie_sub_t *sub, uint8_t slot)
{
    if (!sub) {
        return;
    }
    memset(sub, 0, sizeof(*sub));
    sub->index = slot;
}

int magpie_decode_sub(const char *body, size_t len, int expected_slot,
                      magpie_sub_t *out)
{
    if (!body || !out || len == 0) {
        return MAGPIE_ERR_INVALID_ARG;
    }
    if (len > MAGPIE_SUB_BYTES_MAX) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }
    mjson_t json;
    mjson_init(&json, body, len);
    if (!mjson_validate(&json) || !mjson_at_object(&json)) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }

    magpie_sub_t sub;
    memset(&sub, 0, sizeof(sub));
    sub.index = expected_slot >= 0 ? (uint8_t)expected_slot : 0;
    int slot = expected_slot;

    char key[16];
    if (!mjson_enter(&json)) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }
    while (mjson_next_key(&json, key, sizeof(key))) {
        if (strcmp(key, "i") == 0) {
            slot = read_int(&json, -1);
            if (slot >= 0 && slot <= 255) {
                sub.index = (uint8_t)slot;
            }
        } else if (strcmp(key, "p") == 0) {
            read_text(&json, sub.provider, sizeof(sub.provider),
                      MAGPIE_PROVIDER_MAX);
        } else if (strcmp(key, "n") == 0) {
            read_text(&json, sub.name, sizeof(sub.name), MAGPIE_NAME_MAX);
        } else if (strcmp(key, "pl") == 0) {
            read_text(&json, sub.plan, sizeof(sub.plan), MAGPIE_PLAN_MAX);
        } else if (strcmp(key, "e") == 0) {
            sub.error = read_int(&json, 0) != 0;
        } else if (strcmp(key, "w") == 0) {
            if (!mjson_enter(&json)) {
                if (!mjson_skip(&json)) {
                    break;
                }
                continue;
            }
            while (mjson_next_element(&json)) {
                if (sub.window_count >= MAGPIE_WIN_MAX) {
                    if (!mjson_skip(&json)) {
                        break;
                    }
                    continue;
                }
                read_sub_window(&json, &sub.windows[sub.window_count]);
                sub.window_count++;
            }
            (void)mjson_leave(&json);
        } else if (!mjson_skip(&json)) {
            break;
        }
    }
    (void)mjson_leave(&json);

    if (expected_slot >= 0 && slot != expected_slot) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }
    if (sub.name[0] == '\0') {
        /* A payload without a display name is unusable; fall back to the
         * provider id so the page is never blank. */
        if (sub.provider[0] == '\0') {
            return MAGPIE_ERR_INVALID_RESPONSE;
        }
        strncpy(sub.name, sub.provider, sizeof(sub.name) - 1);
    }
    sub.loaded = true;
    *out = sub;
    return MAGPIE_OK;
}

bool magpie_payload_fits(size_t len, size_t limit)
{
    return len > 0 && len <= limit;
}
