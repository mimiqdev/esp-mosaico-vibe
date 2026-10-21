// SPDX-License-Identifier: Apache-2.0

#include "magpie_pull.h"

#include <string.h>

#include "magpie_parse.h"

bool magpie_pull_once(magpie_source_t *source, const magpie_pull_ctx_t *ctx,
                      magpie_pull_result_t *result)
{
    if (result) {
        memset(result, 0, sizeof(*result));
        result->last_error = MAGPIE_SOURCE_OK;
    }
    if (!source || !ctx || !result || !ctx->index_buf || !ctx->sub_buf ||
        !ctx->index || !ctx->subs || !ctx->loaded_count) {
        return false;
    }
    if (!source->open || !source->close || !source->read_index ||
        !source->read_sub) {
        return false;
    }

    if (source->open(source) != MAGPIE_SOURCE_OK) {
        result->last_error = MAGPIE_SOURCE_ERR_LINK;
        return false;
    }

    const int index_len = source->read_index(source, ctx->index_buf,
                                             ctx->index_cap);
    if (index_len < 0) {
        result->last_error = index_len;
        source->close(source);
        return false;
    }
    magpie_index_t index;
    if (magpie_decode_index(ctx->index_buf, (size_t)index_len, &index) !=
        MAGPIE_OK) {
        result->last_error = MAGPIE_SOURCE_ERR_PAYLOAD;
        source->close(source);
        return false;
    }
    *ctx->index = index;
    result->index_ok = true;

    *ctx->loaded_count = 0;
    const uint8_t wanted =
        index.count < MAGPIE_SLOTS_MAX ? index.count : MAGPIE_SLOTS_MAX;
    for (uint8_t slot = 0; slot < wanted; ++slot) {
        const int len = source->read_sub(source, slot, ctx->sub_buf,
                                         ctx->sub_cap);
        if (len < 0) {
            if (result->last_error == MAGPIE_SOURCE_OK) {
                result->last_error = len;
            }
            ++result->subs_failed;
            /* Drop the slot so a page never shows data from an older cycle
             * after the sender reported a different layout. */
            magpie_sub_clear(&ctx->subs[slot], slot);
            continue;
        }
        magpie_sub_t sub;
        if (magpie_decode_sub(ctx->sub_buf, (size_t)len, slot, &sub) !=
            MAGPIE_OK) {
            if (result->last_error == MAGPIE_SOURCE_OK) {
                result->last_error = MAGPIE_SOURCE_ERR_PAYLOAD;
            }
            ++result->subs_failed;
            magpie_sub_clear(&ctx->subs[slot], slot);
            continue;
        }
        ctx->subs[slot] = sub;
        ++result->subs_ok;
        ++*ctx->loaded_count;
    }
    /* Slots beyond the reported count are cleared so shrinking lists do not
     * leave a stale page behind. */
    for (uint8_t slot = wanted; slot < MAGPIE_SLOTS_MAX; ++slot) {
        magpie_sub_clear(&ctx->subs[slot], slot);
    }

    source->close(source);
    return true;
}
