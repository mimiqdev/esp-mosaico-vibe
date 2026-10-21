// SPDX-License-Identifier: Apache-2.0

#include "magpie_parse.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "magpie_json.h"

#define SECONDS_PER_DAY 86400LL
#define UNIX_EPOCH_YEAR 1970
#define UNUSED_PROVIDER_CAP MAGPIE_PROVIDERS_MAX

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i + 1 < dst_size && src[i]; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void copy_sanitized(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }
    size_t i = 0;
    for (; i + 1 < dst_size && src && src[i]; ++i) {
        const unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 0x20 && c < 0x7f) ? (char)c : ' ';
    }
    dst[i] = '\0';
}

static float clamp_percent(double value)
{
    if (!(value >= 0.0)) { /* also catches NaN */
        return 0.0f;
    }
    if (value > 100.0) {
        return 100.0f;
    }
    return (float)value;
}

int64_t magpie_days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153u * (month + (month > 2 ? -3 : 9)) + 2u) / 5u + day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097LL + (int64_t)doe - 719468LL;
}

/* Parse a run of digits with a width limit. */
static bool read_digits(const char **cursor, const char *end, unsigned count,
                        int *out)
{
    int value = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (*cursor >= end || **cursor < '0' || **cursor > '9') {
            return false;
        }
        value = value * 10 + (**cursor - '0');
        ++*cursor;
    }
    *out = value;
    return true;
}

bool magpie_parse_iso8601(const char *text, int64_t *out_epoch)
{
    if (!text || !out_epoch) {
        return false;
    }
    const char *cursor = text;
    const char *end = text + strlen(text);
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!read_digits(&cursor, end, 4, &year) || cursor >= end || *cursor != '-') {
        return false;
    }
    ++cursor;
    if (!read_digits(&cursor, end, 2, &month) || cursor >= end || *cursor != '-') {
        return false;
    }
    ++cursor;
    if (!read_digits(&cursor, end, 2, &day)) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return false;
    }
    int64_t seconds = magpie_days_from_civil(year, (unsigned)month, (unsigned)day) *
                      SECONDS_PER_DAY;
    if (cursor < end && (*cursor == 'T' || *cursor == ' ')) {
        ++cursor;
        if (!read_digits(&cursor, end, 2, &hour) || cursor >= end || *cursor != ':') {
            return false;
        }
        ++cursor;
        if (!read_digits(&cursor, end, 2, &minute)) {
            return false;
        }
        if (cursor < end && *cursor == ':') {
            ++cursor;
            if (!read_digits(&cursor, end, 2, &second)) {
                return false;
            }
            if (cursor < end && *cursor == '.') {
                ++cursor;
                while (cursor < end && *cursor >= '0' && *cursor <= '9') {
                    ++cursor;
                }
            }
        }
        seconds += hour * 3600LL + minute * 60LL + second;
    }
    /* Timezone: 'Z', or +/-hh:mm. A bare timestamp is treated as UTC. */
    if (cursor < end) {
        int offset_minutes = 0;
        if (*cursor == 'Z' || *cursor == 'z') {
            ++cursor;
        } else if (*cursor == '+' || *cursor == '-') {
            const int sign = *cursor == '-' ? -1 : 1;
            ++cursor;
            int off_hour = 0, off_minute = 0;
            if (!read_digits(&cursor, end, 2, &off_hour)) {
                return false;
            }
            if (cursor < end && *cursor == ':') {
                ++cursor;
            }
            if (cursor < end && *cursor >= '0' && *cursor <= '9') {
                if (!read_digits(&cursor, end, 2, &off_minute)) {
                    return false;
                }
            }
            offset_minutes = sign * (off_hour * 60 + off_minute);
        }
        if (cursor != end) {
            return false;
        }
        seconds -= offset_minutes * 60LL;
    }
    *out_epoch = seconds;
    return true;
}

static bool provider_matches_filter(const char *provider,
                                    const char *provider_filter)
{
    if (!provider_filter || provider_filter[0] == '\0') {
        return true;
    }
    const char *cursor = provider_filter;
    while (*cursor) {
        while (*cursor == ',' || *cursor == ' ') {
            ++cursor;
        }
        const char *start = cursor;
        while (*cursor && *cursor != ',') {
            ++cursor;
        }
        size_t len = (size_t)(cursor - start);
        while (len && start[len - 1] == ' ') {
            --len;
        }
        if (len && strncmp(provider, start, len) == 0) {
            return true;
        }
    }
    return false;
}

/* Reset countdown from an ISO timestamp, clamped to int32 seconds. */
static bool reset_countdown(const char *timestamp, int64_t now_epoch,
                            int32_t *out_seconds)
{
    int64_t epoch = 0;
    *out_seconds = 0;
    if (!magpie_parse_iso8601(timestamp, &epoch)) {
        return false;
    }
    int64_t delta = epoch - now_epoch;
    if (delta < 0) {
        delta = 0;
    }
    if (delta > INT32_MAX) {
        delta = INT32_MAX;
    }
    *out_seconds = (int32_t)delta;
    return true;
}

/* Parse one window object; the cursor sits on '{'. */
static void parse_window(mjson_t *json, int64_t now_epoch, magpie_window_t *window)
{
    char key[MAGPIE_LABEL_MAX];
    char resets_at[MAGPIE_TEXT_MAX];
    double remaining = 0;
    double used = 0;
    bool has_remaining = false;
    bool unlimited = false;
    resets_at[0] = '\0';

    if (!mjson_enter(json)) {
        return;
    }
    while (mjson_next_key(json, key, sizeof(key))) {
        if (strcmp(key, "name") == 0) {
            if (!mjson_read_string(json, window->name, sizeof(window->name))) {
                (void)mjson_skip(json);
            }
        } else if (strcmp(key, "remaining") == 0) {
            double value = 0;
            mjson_read_number_lenient(json, &value);
            remaining = value;
            has_remaining = true;
        } else if (strcmp(key, "used") == 0) {
            mjson_read_number_lenient(json, &used);
        } else if (strcmp(key, "unlimited") == 0) {
            bool flag = false;
            if (!mjson_read_bool(json, &flag)) {
                (void)mjson_skip(json);
            }
            unlimited = flag;
        } else if (strcmp(key, "resetsAt") == 0) {
            if (!mjson_read_string(json, resets_at, sizeof(resets_at))) {
                (void)mjson_skip(json);
                resets_at[0] = '\0';
            }
        } else if (!mjson_skip(json)) {
            break;
        }
    }
    (void)mjson_leave(json);

    /* Magpie reports percentages. Some providers only publish used; derive it
     * so those rows still render instead of showing a silent 100%. */
    if (!has_remaining && used > 0) {
        remaining = 100.0 - used;
    }
    window->remaining_percent = clamp_percent(remaining);
    window->unlimited = unlimited;
    window->has_reset = reset_countdown(resets_at, now_epoch, &window->reset_in_s);
}

/* Worst (lowest remaining) usable window; unlimited windows never win. */
static const magpie_window_t *worst_window(const magpie_provider_t *provider)
{
    const magpie_window_t *worst = NULL;
    for (uint8_t i = 0; i < provider->window_count; ++i) {
        const magpie_window_t *window = &provider->windows[i];
        if (window->unlimited) {
            continue;
        }
        if (!worst || window->remaining_percent < worst->remaining_percent) {
            worst = window;
        }
    }
    return worst;
}

/* Parse one provider object; the cursor sits on '{'. */
static void parse_provider(mjson_t *json, int64_t now_epoch,
                           magpie_provider_t *provider)
{
    char key[MAGPIE_LABEL_MAX];
    memset(provider, 0, sizeof(*provider));
    provider->worst_remaining = 100.0f;

    if (!mjson_enter(json)) {
        return;
    }
    while (mjson_next_key(json, key, sizeof(key))) {
        if (strcmp(key, "provider") == 0) {
            if (!mjson_read_string(json, provider->provider,
                                   sizeof(provider->provider))) {
                (void)mjson_skip(json);
            }
        } else if (strcmp(key, "name") == 0) {
            if (!mjson_read_string(json, provider->name, sizeof(provider->name))) {
                (void)mjson_skip(json);
            }
        } else if (strcmp(key, "plan") == 0) {
            if (!mjson_read_string(json, provider->plan, sizeof(provider->plan))) {
                (void)mjson_skip(json);
                provider->plan[0] = '\0';
            }
        } else if (strcmp(key, "error") == 0) {
            if (!mjson_read_string(json, provider->error,
                                   sizeof(provider->error))) {
                (void)mjson_skip(json);
                provider->error[0] = '\0';
            }
        } else if (strcmp(key, "windows") == 0) {
            if (mjson_enter(json)) {
                while (mjson_next_element(json)) {
                    if (provider->window_count >= MAGPIE_WINDOWS_MAX) {
                        if (!mjson_skip(json)) {
                            break;
                        }
                        continue;
                    }
                    magpie_window_t *window =
                        &provider->windows[provider->window_count];
                    parse_window(json, now_epoch, window);
                    provider->window_count++;
                }
                (void)mjson_leave(json);
            } else if (!mjson_skip(json)) {
                break;
            }
        } else if (!mjson_skip(json)) {
            break;
        }
    }
    (void)mjson_leave(json);

    const magpie_window_t *worst = worst_window(provider);
    if (worst) {
        provider->worst_remaining = worst->remaining_percent;
        provider->worst_reset_in_s = worst->reset_in_s;
        provider->unlimited = false;
    } else {
        provider->worst_remaining = 100.0f;
        provider->worst_reset_in_s = 0;
        /* A provider whose only windows are unlimited is not a constraint. */
        bool all_unlimited = provider->window_count > 0;
        for (uint8_t i = 0; i < provider->window_count; ++i) {
            if (!provider->windows[i].unlimited) {
                all_unlimited = false;
                break;
            }
        }
        provider->unlimited = all_unlimited;
    }
}

void magpie_snapshot_clear(magpie_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->quota_remaining = 100.0f;
}

/* Track the single most constrained window across every provider. */
static void update_quota(magpie_snapshot_t *out, const magpie_provider_t *provider)
{
    const magpie_window_t *worst = worst_window(provider);
    if (!worst) {
        return;
    }
    if (!out->has_quota || worst->remaining_percent < out->quota_remaining) {
        out->has_quota = true;
        copy_text(out->quota_provider, sizeof(out->quota_provider),
                  provider->provider[0] ? provider->provider : provider->name);
        copy_text(out->quota_window, sizeof(out->quota_window), worst->name);
        out->quota_remaining = worst->remaining_percent;
        out->quota_reset_in_s = worst->reset_in_s;
        out->quota_has_reset = worst->has_reset;
    }
}

int magpie_parse_quotas(const char *body, size_t len, int64_t now_epoch,
                        const char *provider_filter, magpie_snapshot_t *out)
{
    if (!body || !out) {
        return MAGPIE_ERR_INVALID_ARG;
    }
    mjson_t json;
    mjson_init(&json, body, len);
    if (!mjson_validate(&json)) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }
    out->provider_count = 0;
    out->provider_total = 0;
    out->has_quota = false;
    out->quota_remaining = 100.0f;

    char key[MAGPIE_LABEL_MAX];
    if (!mjson_enter(&json)) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }
    bool found_data = false;
    while (mjson_next_key(&json, key, sizeof(key))) {
        if (strcmp(key, "data") != 0 || !mjson_enter(&json)) {
            if (!mjson_skip(&json)) {
                break;
            }
            continue;
        }
        found_data = true;
        while (mjson_next_element(&json)) {
            magpie_provider_t provider;
            parse_provider(&json, now_epoch, &provider);
            if (!provider_matches_filter(provider.provider, provider_filter)) {
                continue;
            }
            out->provider_total++;
            update_quota(out, &provider);
            if (out->provider_count < MAGPIE_PROVIDERS_MAX) {
                out->providers[out->provider_count++] = provider;
            }
        }
        (void)mjson_leave(&json);
    }
    (void)mjson_leave(&json);
    return found_data ? MAGPIE_OK : MAGPIE_ERR_INVALID_RESPONSE;
}

/* Tokens recorded for one daily bucket. */
static uint64_t bucket_tokens(double input, double output, double reasoning,
                             double cache_read, double cache_write)
{
    double total = 0;
    const double parts[] = {input, output, reasoning, cache_read, cache_write};
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        if (parts[i] > 0) {
            total += parts[i];
        }
    }
    return (uint64_t)(total + 0.5);
}

typedef struct {
    double input, output, reasoning, cache_read, cache_write, calls, errors;
    uint64_t tokens;
} bucket_t;

static void parse_series_bucket(mjson_t *json, bucket_t *bucket)
{
    char key[MAGPIE_LABEL_MAX];
    memset(bucket, 0, sizeof(*bucket));
    if (!mjson_enter(json)) {
        return;
    }
    while (mjson_next_key(json, key, sizeof(key))) {
        double value = 0;
        if (strcmp(key, "input") == 0) {
            mjson_read_number_lenient(json, &value);
            bucket->input = value;
        } else if (strcmp(key, "output") == 0) {
            mjson_read_number_lenient(json, &value);
            bucket->output = value;
        } else if (strcmp(key, "reasoning") == 0) {
            mjson_read_number_lenient(json, &value);
            bucket->reasoning = value;
        } else if (strcmp(key, "cache_read") == 0) {
            mjson_read_number_lenient(json, &value);
            bucket->cache_read = value;
        } else if (strcmp(key, "cache_write") == 0) {
            mjson_read_number_lenient(json, &value);
            bucket->cache_write = value;
        } else if (strcmp(key, "calls") == 0) {
            mjson_read_number_lenient(json, &value);
            bucket->calls = value;
        } else if (strcmp(key, "errors") == 0) {
            mjson_read_number_lenient(json, &value);
            bucket->errors = value;
        } else if (!mjson_skip(json)) {
            break;
        }
    }
    (void)mjson_leave(json);
    bucket->tokens = bucket_tokens(bucket->input, bucket->output,
                                   bucket->reasoning, bucket->cache_read,
                                   bucket->cache_write);
}

int magpie_parse_usage(const char *body, size_t len, magpie_snapshot_t *out)
{
    if (!body || !out) {
        return MAGPIE_ERR_INVALID_ARG;
    }
    mjson_t json;
    mjson_init(&json, body, len);
    if (!mjson_validate(&json)) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }
    magpie_usage_t *usage = &out->usage;
    memset(usage, 0, sizeof(*usage));

    /* Keep the tail of the series in a small ring so tokens_today and
     * tokens_7d do not need the whole 30-day array resident. */
    bucket_t tail[MAGPIE_SERIES_MAX];
    size_t tail_count = 0;
    size_t bucket_index = 0;

    char key[MAGPIE_LABEL_MAX];
    if (!mjson_enter(&json)) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }
    while (mjson_next_key(&json, key, sizeof(key))) {
        double value = 0;
        if (strcmp(key, "calls") == 0) {
            mjson_read_number_lenient(&json, &value);
            usage->calls = (uint32_t)(value > 0 ? value : 0);
        } else if (strcmp(key, "errors") == 0) {
            mjson_read_number_lenient(&json, &value);
            usage->errors = (uint32_t)(value > 0 ? value : 0);
        } else if (strcmp(key, "input") == 0) {
            mjson_read_number_lenient(&json, &value);
            usage->input = (uint64_t)(value > 0 ? value : 0);
        } else if (strcmp(key, "output") == 0) {
            mjson_read_number_lenient(&json, &value);
            usage->output = (uint64_t)(value > 0 ? value : 0);
        } else if (strcmp(key, "reasoning") == 0) {
            mjson_read_number_lenient(&json, &value);
            usage->reasoning = (uint64_t)(value > 0 ? value : 0);
        } else if (strcmp(key, "cache_read") == 0) {
            mjson_read_number_lenient(&json, &value);
            usage->cache_read = (uint64_t)(value > 0 ? value : 0);
        } else if (strcmp(key, "cache_write") == 0) {
            mjson_read_number_lenient(&json, &value);
            usage->cache_write = (uint64_t)(value > 0 ? value : 0);
        } else if (strcmp(key, "cost") == 0) {
            mjson_read_number_lenient(&json, &value);
            usage->cost_micros = (uint64_t)(value > 0 ? value * 1e6 : 0);
        } else if (strcmp(key, "timed") == 0) {
            mjson_read_number_lenient(&json, &value);
            usage->timed_calls = (uint32_t)(value > 0 ? value : 0);
        } else if (strcmp(key, "ttft_ms") == 0) {
            mjson_read_number_lenient(&json, &value);
            usage->ttft_ms_total = (uint32_t)(value > 0 ? value : 0);
        } else if (strcmp(key, "series") == 0) {
            if (!mjson_enter(&json)) {
                if (!mjson_skip(&json)) {
                    break;
                }
                continue;
            }
            while (mjson_next_element(&json)) {
                bucket_t bucket;
                parse_series_bucket(&json, &bucket);
                if (tail_count < MAGPIE_SERIES_MAX) {
                    tail[tail_count++] = bucket;
                } else {
                    memmove(&tail[0], &tail[1],
                            sizeof(tail[0]) * (MAGPIE_SERIES_MAX - 1));
                    tail[MAGPIE_SERIES_MAX - 1] = bucket;
                }
                ++bucket_index;
            }
            (void)mjson_leave(&json);
        } else if (!mjson_skip(&json)) {
            break;
        }
    }
    (void)mjson_leave(&json);

    usage->tokens_total = usage->input + usage->output + usage->reasoning +
                          usage->cache_read + usage->cache_write;
    usage->average_ttft_ms =
        usage->timed_calls ? usage->ttft_ms_total / usage->timed_calls : 0;
    usage->calls_today = 0;
    usage->errors_today = 0;
    usage->tokens_today = 0;
    usage->tokens_7d = 0;
    usage->series_count = (uint8_t)tail_count;
    for (size_t i = 0; i < tail_count; ++i) {
        usage->tokens_7d += tail[i].tokens;
        usage->series_tokens[i] = tail[i].tokens;
    }
    if (tail_count) {
        const bucket_t *today = &tail[tail_count - 1];
        usage->tokens_today = today->tokens;
        usage->calls_today = (uint32_t)(today->calls > 0 ? today->calls : 0);
        usage->errors_today = (uint32_t)(today->errors > 0 ? today->errors : 0);
    }
    if (usage->calls_today && usage->errors_today > usage->calls_today) {
        usage->errors_today = usage->calls_today;
    }
    return MAGPIE_OK;
}

int magpie_parse_requests(const char *body, size_t len, magpie_snapshot_t *out)
{
    if (!body || !out) {
        return MAGPIE_ERR_INVALID_ARG;
    }
    mjson_t json;
    mjson_init(&json, body, len);
    if (!mjson_validate(&json)) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }
    magpie_request_t *latest = &out->latest;
    memset(latest, 0, sizeof(*latest));

    char key[MAGPIE_LABEL_MAX];
    if (!mjson_enter(&json)) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }
    while (mjson_next_key(&json, key, sizeof(key))) {
        if (strcmp(key, "rows") != 0 || !mjson_enter(&json)) {
            if (!mjson_skip(&json)) {
                break;
            }
            continue;
        }
        /* The newest request is the first row; read it and skip the rest. */
        if (mjson_next_element(&json)) {
            char field[MAGPIE_LABEL_MAX];
            char served[MAGPIE_TEXT_MAX];
            char model[MAGPIE_TEXT_MAX];
            bool has_error = false;
            served[0] = '\0';
            model[0] = '\0';
            if (mjson_enter(&json)) {
                while (mjson_next_key(&json, field, sizeof(field))) {
                    if (strcmp(field, "served") == 0) {
                        if (!mjson_read_string(&json, served, sizeof(served))) {
                            (void)mjson_skip(&json);
                        }
                    } else if (strcmp(field, "model") == 0) {
                        if (!mjson_read_string(&json, model, sizeof(model))) {
                            (void)mjson_skip(&json);
                        }
                    } else if (strcmp(field, "status") == 0) {
                        int64_t status = 0;
                        if (mjson_read_int64(&json, &status)) {
                            latest->status = (int32_t)status;
                        } else {
                            (void)mjson_skip(&json);
                        }
                    } else if (strcmp(field, "ms") == 0) {
                        double ms = 0;
                        mjson_read_number_lenient(&json, &ms);
                        latest->latency_ms = (uint32_t)(ms > 0 ? ms : 0);
                    } else if (strcmp(field, "err") == 0 ||
                               strcmp(field, "error") == 0) {
                        bool flag = false;
                        if (!mjson_read_bool(&json, &flag)) {
                            (void)mjson_skip(&json);
                        }
                        has_error = flag;
                    } else if (!mjson_skip(&json)) {
                        break;
                    }
                }
                (void)mjson_leave(&json);
            }
            copy_sanitized(latest->model, sizeof(latest->model),
                           served[0] ? served : model);
            latest->failed = has_error || latest->status < 200 ||
                             latest->status >= 400;
        }
        (void)mjson_leave(&json);
    }
    (void)mjson_leave(&json);
    return MAGPIE_OK;
}

int magpie_parse_health(const char *body, size_t len, magpie_snapshot_t *out)
{
    if (!body || !out) {
        return MAGPIE_ERR_INVALID_ARG;
    }
    mjson_t json;
    mjson_init(&json, body, len);
    if (!mjson_validate(&json)) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }
    out->gateway_version[0] = '\0';
    char key[MAGPIE_LABEL_MAX];
    char name[MAGPIE_LABEL_MAX];
    name[0] = '\0';
    bool is_magpie = false;
    if (!mjson_enter(&json)) {
        return MAGPIE_ERR_INVALID_RESPONSE;
    }
    while (mjson_next_key(&json, key, sizeof(key))) {
        if (strcmp(key, "name") == 0) {
            if (mjson_read_string(&json, name, sizeof(name))) {
                is_magpie = strcmp(name, "magpie") == 0;
            } else {
                (void)mjson_skip(&json);
            }
        } else if (strcmp(key, "version") == 0) {
            if (!mjson_read_string(&json, out->gateway_version,
                                   sizeof(out->gateway_version))) {
                (void)mjson_skip(&json);
                out->gateway_version[0] = '\0';
            }
        } else if (!mjson_skip(&json)) {
            break;
        }
    }
    (void)mjson_leave(&json);
    return is_magpie ? MAGPIE_OK : MAGPIE_ERR_INVALID_RESPONSE;
}

uint8_t magpie_snapshot_error_count(const magpie_snapshot_t *snapshot)
{
    if (!snapshot) {
        return 0;
    }
    uint8_t count = 0;
    for (uint8_t i = 0; i < snapshot->provider_count; ++i) {
        if (snapshot->providers[i].error[0]) {
            ++count;
        }
    }
    return count;
}
