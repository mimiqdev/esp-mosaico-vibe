// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Portable Magpie usage model.
 *
 * This header is deliberately free of ESP-IDF includes so magpie_json.c,
 * magpie_time.c and magpie_parse.c can be compiled natively for host tests
 * (see tests/host). The device build and the host build share the same source.
 */

#define MAGPIE_TEXT_MAX 48
#define MAGPIE_LABEL_MAX 16
#define MAGPIE_PROVIDERS_MAX 12
#define MAGPIE_WINDOWS_MAX 4
#define MAGPIE_SERIES_MAX 8

/** One quota window of one provider, e.g. "5 hours" with 16% remaining. */
typedef struct {
    char name[MAGPIE_LABEL_MAX];
    float remaining_percent; /* 0..100 */
    int32_t reset_in_s;      /* seconds until resetsAt; 0 when unknown */
    bool unlimited;
    bool has_reset;
} magpie_window_t;

/** One provider row: subscription name, plan and its quota windows. */
typedef struct {
    char provider[MAGPIE_TEXT_MAX]; /* provider id, e.g. "office/claude" */
    char name[MAGPIE_TEXT_MAX];     /* display name, e.g. "Claude Code" */
    char plan[MAGPIE_LABEL_MAX];
    char error[MAGPIE_TEXT_MAX];    /* non-empty when the provider is signed out */
    magpie_window_t windows[MAGPIE_WINDOWS_MAX];
    uint8_t window_count;
    float worst_remaining; /* min over usable windows; 100 when none */
    int32_t worst_reset_in_s;
    bool unlimited;
} magpie_provider_t;

/** Aggregated usage totals plus the daily token series. */
typedef struct {
    uint32_t calls;
    uint32_t errors;
    uint64_t input;
    uint64_t output;
    uint64_t reasoning;
    uint64_t cache_read;
    uint64_t cache_write;
    uint64_t cost_micros;
    uint32_t ttft_ms_total;
    uint32_t timed_calls;
    /* Derived */
    uint64_t tokens_total;
    uint64_t tokens_today;
    uint64_t tokens_7d;
    uint32_t calls_today;
    uint32_t errors_today;
    uint32_t average_ttft_ms;
    /* Series tail kept for the activity page. */
    uint8_t series_count;
    uint64_t series_tokens[MAGPIE_SERIES_MAX];
} magpie_usage_t;

/** Latest request from /v1/magpie/usage/requests. */
typedef struct {
    char model[MAGPIE_TEXT_MAX];
    int32_t status;
    uint32_t latency_ms;
    bool failed;
} magpie_request_t;

/** Everything the UI needs; produced by the parser, consumed by the UI only. */
typedef struct {
    magpie_provider_t providers[MAGPIE_PROVIDERS_MAX];
    uint8_t provider_count;
    uint16_t provider_total; /* count before the display cap */

    /* Most constrained window across all providers (the glance value). */
    bool has_quota;
    char quota_provider[MAGPIE_TEXT_MAX];
    char quota_window[MAGPIE_LABEL_MAX];
    float quota_remaining;
    int32_t quota_reset_in_s;
    bool quota_has_reset;

    magpie_usage_t usage;
    magpie_request_t latest;

    char gateway_version[MAGPIE_LABEL_MAX];
} magpie_snapshot_t;
