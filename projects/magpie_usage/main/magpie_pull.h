// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "magpie_model.h"
#include "magpie_source.h"

/*
 * Transport-agnostic pull cycle.
 *
 * Portable C with no ESP-IDF dependency: the same function performs a refresh
 * over BLE or Wi-Fi, and the host tests drive it with a fake source to check
 * ordering, partial failures and stale handling.
 */

/** Buffers the cycle needs; supplied by the caller so sizes are explicit. */
typedef struct {
    char *index_buf;
    char *sub_buf;
    size_t index_cap;
    size_t sub_cap;
    magpie_index_t *index;
    magpie_sub_t *subs;             /* MAGPIE_SLOTS_MAX entries */
    uint8_t *loaded_count;          /* slots whose SUB payload was accepted */
} magpie_pull_ctx_t;

/** Outcome of one pull cycle. */
typedef struct {
    bool index_ok;
    uint8_t subs_ok;
    uint8_t subs_failed;
    int last_error;                 /* first transport error, MAGPIE_SOURCE_* */
} magpie_pull_result_t;

/**
 * Perform one full refresh.
 *
 * The index is fetched first; when it fails nothing else is attempted and the
 * caller keeps the previous snapshot. A failing SUB slot does not discard the
 * others, so a single truncated read never blanks a page.
 *
 * Returns true when at least the INDEX payload was accepted.
 */
bool magpie_pull_once(magpie_source_t *source, const magpie_pull_ctx_t *ctx,
                      magpie_pull_result_t *result);
