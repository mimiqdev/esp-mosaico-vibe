// SPDX-License-Identifier: Apache-2.0

/*
 * Host test harness for the portable Magpie sources.
 *
 * Deliberately a plain C program instead of a framework: the host runner in
 * run_tests.py already builds these exact device sources with ASan/UBSan, and
 * keeping the harness dependency free means the tests can run anywhere a C
 * compiler exists.
 *
 * argv[1] is the fixtures directory populated by tools/make_fixtures.py.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "magpie_format.h"
#include "magpie_json.h"
#include "magpie_model.h"
#include "magpie_parse.h"

static unsigned g_failures;
static unsigned g_checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(cond)) {                                                       \
            ++g_failures;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                    \
    } while (0)

#define CHECK_STR(actual, expected)                                          \
    do {                                                                     \
        ++g_checks;                                                          \
        if (strcmp((actual), (expected)) != 0) {                             \
            ++g_failures;                                                    \
            fprintf(stderr, "FAIL %s:%d: expected \"%s\", got \"%s\"\n",      \
                    __FILE__, __LINE__, (expected), (actual));               \
        }                                                                    \
    } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                              \
    do {                                                                     \
        ++g_checks;                                                          \
        const double _a = (double)(actual);                                   \
        const double _e = (double)(expected);                                 \
        const double _t = (tolerance);                                        \
        const double _d = _a > _e ? _a - _e : _e - _a;                        \
        if (_d > _t) {                                                       \
            ++g_failures;                                                    \
            fprintf(stderr, "FAIL %s:%d: expected %g +-%g, got %g\n",         \
                    __FILE__, __LINE__, _e, _t, _a);                         \
        }                                                                    \
    } while (0)

/* ---- fixture loading ------------------------------------------------ */

static const char *g_fixture_dir;

static char *load_fixture(const char *name, size_t *out_len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_fixture_dir, name);
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "FAIL cannot open fixture %s\n", path);
        ++g_failures;
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
    *out_len = read;
    return buffer;
}

static magpie_snapshot_t *load_snapshot(int64_t now, const char *filter)
{
    magpie_snapshot_t *snapshot = malloc(sizeof(*snapshot));
    if (!snapshot) {
        return NULL;
    }
    magpie_snapshot_clear(snapshot);
    size_t len = 0;
    char *health = load_fixture("health.json", &len);
    if (health) {
        CHECK(magpie_parse_health(health, len, snapshot) == MAGPIE_OK);
        free(health);
    }
    char *quotas = load_fixture("quotas.json", &len);
    if (quotas) {
        CHECK(magpie_parse_quotas(quotas, len, now, filter, snapshot) == MAGPIE_OK);
        free(quotas);
    }
    char *usage = load_fixture("usage.json", &len);
    if (usage) {
        CHECK(magpie_parse_usage(usage, len, snapshot) == MAGPIE_OK);
        free(usage);
    }
    char *requests = load_fixture("requests.json", &len);
    if (requests) {
        CHECK(magpie_parse_requests(requests, len, snapshot) == MAGPIE_OK);
        free(requests);
    }
    return snapshot;
}

/* ---- ISO-8601 -------------------------------------------------------- */

static void test_iso8601(void)
{
    int64_t epoch = 0;

    /* 2026-01-01T00:00:00Z */
    CHECK(magpie_parse_iso8601("2026-01-01T00:00:00Z", &epoch));
    CHECK(epoch == 1767225600LL);

    /* An explicit +08:00 offset is converted back to UTC. */
    int64_t offset = 0;
    int64_t utc_same = 0;
    CHECK(magpie_parse_iso8601("2026-10-21T12:29:00+08:00", &offset));
    CHECK(magpie_parse_iso8601("2026-10-21T04:29:00Z", &utc_same));
    CHECK(offset == utc_same);

    /* A negative offset moves the other way. */
    int64_t behind = 0;
    int64_t behind_utc = 0;
    CHECK(magpie_parse_iso8601("2026-10-21T04:29:00-05:00", &behind));
    CHECK(magpie_parse_iso8601("2026-10-21T09:29:00Z", &behind_utc));
    CHECK(behind == behind_utc);

    int64_t utc = 0;
    int64_t shanghai = 0;
    CHECK(magpie_parse_iso8601("2026-10-21T12:29:00Z", &utc));
    CHECK(magpie_parse_iso8601("2026-10-21T12:29:00+00:00", &shanghai));
    CHECK(utc == shanghai);

    /* Fractional seconds, as Magpie emits for some providers. */
    int64_t with_fraction = 0;
    CHECK(magpie_parse_iso8601("2026-10-21T04:00:26.052876Z", &with_fraction));
    int64_t without_fraction = 0;
    CHECK(magpie_parse_iso8601("2026-10-21T04:00:26Z", &without_fraction));
    CHECK(with_fraction == without_fraction);

    /* Date only, treated as midnight UTC: 2026-10-21 is day 20747. */
    int64_t date_only = 0;
    CHECK(magpie_parse_iso8601("2026-10-21", &date_only));
    CHECK(date_only == 20747LL * 86400LL);
    CHECK(date_only == 1792540800LL);

    /* Rejections. */
    CHECK(!magpie_parse_iso8601("", &epoch));
    CHECK(!magpie_parse_iso8601("not a date", &epoch));
    CHECK(!magpie_parse_iso8601("2026-13-01T00:00:00Z", &epoch));
    CHECK(!magpie_parse_iso8601("2026-10-32T00:00:00Z", &epoch));

    /* Civil-date conversion against known epochs. */
    CHECK(magpie_days_from_civil(1970, 1, 1) == 0);
    CHECK(magpie_days_from_civil(2026, 1, 1) == 20454);
    CHECK(magpie_days_from_civil(2026, 10, 21) == 20747);
    CHECK(magpie_days_from_civil(2000, 3, 1) == 11017);
}

/* ---- JSON scanner ---------------------------------------------------- */

static void test_json_scanner(void)
{
    const char *doc = "{\"a\":1,\"b\":[true,false,null],\"c\":{\"d\":\"x\\u00e9y\"},"
                      "\"e\":\"quote\\\"inside\"}";
    mjson_t json;
    mjson_init(&json, doc, strlen(doc));
    CHECK(mjson_validate(&json));

    mjson_t broken;
    const char *bad = "{\"a\":\"unterminated}";
    mjson_init(&broken, bad, strlen(bad));
    CHECK(!mjson_validate(&broken));

    mjson_t trailing;
    const char *junk = "{\"a\":1} trailing";
    mjson_init(&trailing, junk, strlen(junk));
    CHECK(!mjson_validate(&trailing));

    mjson_t empty;
    mjson_init(&empty, "{}", 2);
    CHECK(mjson_validate(&empty));
    CHECK(mjson_enter(&empty));
    char key[MAGPIE_LABEL_MAX];
    CHECK(!mjson_next_key(&empty, key, sizeof(key)));
    CHECK(mjson_leave(&empty));

    /* Walk the object and read each member. */
    mjson_t walk;
    mjson_init(&walk, doc, strlen(doc));
    CHECK(mjson_enter(&walk));
    char text[MAGPIE_TEXT_MAX];
    CHECK(mjson_next_key(&walk, key, sizeof(key)));
    CHECK_STR(key, "a");
    double number = 0;
    CHECK(mjson_read_double(&walk, &number));
    CHECK(number == 1.0);
    CHECK(mjson_next_key(&walk, key, sizeof(key)));
    CHECK_STR(key, "b");
    CHECK(mjson_enter(&walk));
    CHECK(mjson_next_element(&walk));
    CHECK(mjson_skip(&walk));
    CHECK(mjson_next_element(&walk));
    CHECK(mjson_skip(&walk));
    CHECK(mjson_next_element(&walk));
    CHECK(mjson_skip(&walk));
    CHECK(!mjson_next_element(&walk));
    CHECK(mjson_leave(&walk));
    CHECK(mjson_next_key(&walk, key, sizeof(key)));
    CHECK_STR(key, "c");
    CHECK(mjson_enter(&walk));
    CHECK(mjson_next_key(&walk, key, sizeof(key)));
    CHECK_STR(key, "d");
    CHECK(mjson_read_string(&walk, text, sizeof(text)));
    /* The \u escape is not ASCII-printable after decoding to 0xE9. */
    CHECK_STR(text, "x?y");
    CHECK(mjson_leave(&walk));
    CHECK(mjson_next_key(&walk, key, sizeof(key)));
    CHECK_STR(key, "e");
    CHECK(mjson_read_string(&walk, text, sizeof(text)));
    CHECK_STR(text, "quote\"inside");
    CHECK(!mjson_next_key(&walk, key, sizeof(key)));
    CHECK(mjson_leave(&walk));

    /* Lenient numbers: null/boolean do not fail the walk. */
    mjson_t lenient;
    const char *mixed = "{\"n\":null,\"b\":true,\"v\":4.5}";
    mjson_init(&lenient, mixed, strlen(mixed));
    CHECK(mjson_enter(&lenient));
    double value = -1;
    CHECK(mjson_next_key(&lenient, key, sizeof(key)));
    mjson_read_number_lenient(&lenient, &value);
    CHECK(value == 0.0);
    CHECK(mjson_next_key(&lenient, key, sizeof(key)));
    mjson_read_number_lenient(&lenient, &value);
    CHECK(value == 0.0);
    CHECK(mjson_next_key(&lenient, key, sizeof(key)));
    mjson_read_number_lenient(&lenient, &value);
    CHECK(value == 4.5);
    CHECK(mjson_leave(&lenient));

    /* Overflowing truncation must still terminate. */
    char tiny[4];
    mjson_t truncate;
    const char *long_text = "{\"k\":\"abcdefghij\"}";
    mjson_init(&truncate, long_text, strlen(long_text));
    CHECK(mjson_enter(&truncate));
    CHECK(mjson_next_key(&truncate, key, sizeof(key)));
    CHECK(mjson_read_string(&truncate, tiny, sizeof(tiny)));
    CHECK_STR(tiny, "abc");
    CHECK(mjson_leave(&truncate));
}

/* ---- quotas ---------------------------------------------------------- */

static void test_quotas_fixture(void)
{
    /* 2026-10-21T11:30:00+08:00 */
    const int64_t now = 1792543800LL;
    magpie_snapshot_t *snapshot = load_snapshot(now, NULL);
    if (!snapshot) {
        return;
    }

    CHECK(snapshot->provider_count == 7);
    CHECK(snapshot->provider_total == 7);
    CHECK_STR(snapshot->gateway_version, "0.1.1268");

    /* kimi-code-cn reports a 0% weekly window, the most constrained row. */
    CHECK(snapshot->has_quota);
    CHECK_STR(snapshot->quota_provider, "office/kimi-code-cn");
    CHECK_STR(snapshot->quota_window, "7 days");
    CHECK_NEAR(snapshot->quota_remaining, 0.0, 0.001);

    /* The claude row with 2% remaining on the 5-hour window must win over the
     * claude row with 16%: duplicate providers stay separate rows. */
    unsigned claude_rows = 0;
    for (uint8_t i = 0; i < snapshot->provider_count; ++i) {
        const magpie_provider_t *provider = &snapshot->providers[i];
        CHECK(provider->window_count > 0);
        if (strcmp(provider->provider, "office/claude") == 0) {
            ++claude_rows;
            CHECK(provider->worst_remaining <= 16.0f + 0.001f);
        }
        if (strcmp(provider->provider, "office/cursor") == 0) {
            CHECK_STR(provider->plan, "Team");
            /* Cursor's On-demand window (27.65%) is the worst of its five. */
            CHECK_NEAR(provider->worst_remaining, 27.6504, 0.01);
            CHECK(provider->window_count == MAGPIE_WINDOWS_MAX);
        }
    }
    CHECK(claude_rows == 2);

    /* A fractional, Z-suffixed reset parses; a null resetsAt does not. */
    for (uint8_t i = 0; i < snapshot->provider_count; ++i) {
        const magpie_provider_t *provider = &snapshot->providers[i];
        if (strcmp(provider->provider, "office/workbuddy") == 0) {
            CHECK_STR(provider->provider, "office/workbuddy");
            CHECK(provider->worst_reset_in_s == 0);
            for (uint8_t w = 0; w < provider->window_count; ++w) {
                CHECK(!provider->windows[w].has_reset);
            }
        }
        if (strcmp(provider->provider, "office/cursor") == 0) {
            for (uint8_t w = 0; w < provider->window_count; ++w) {
                CHECK(provider->windows[w].has_reset);
                CHECK(provider->windows[w].reset_in_s > 0);
            }
        }
    }

    /* This capture has no signed-out provider; the error fields stay empty and
     * nothing is counted as SIGNED OUT. */
    CHECK(magpie_snapshot_error_count(snapshot) == 0);
    for (uint8_t i = 0; i < snapshot->provider_count; ++i) {
        CHECK(snapshot->providers[i].error[0] == '\0');
    }

    free(snapshot);
}

static void test_quotas_filter(void)
{
    const int64_t now = 1792543800LL;
    magpie_snapshot_t *office = load_snapshot(now, "office/");
    if (!office) {
        return;
    }
    CHECK(office->provider_count == 6);
    for (uint8_t i = 0; i < office->provider_count; ++i) {
        CHECK(strncmp(office->providers[i].provider, "office/", 7) == 0);
    }
    free(office);

    magpie_snapshot_t *copilot = load_snapshot(now, "copilot");
    if (!copilot) {
        return;
    }
    CHECK(copilot->provider_count == 1);
    CHECK_STR(copilot->providers[0].provider, "copilot");
    free(copilot);

    /* A filter that matches nothing leaves no quota and keeps the default. */
    magpie_snapshot_t *none = load_snapshot(now, "zzz/");
    if (!none) {
        return;
    }
    CHECK(none->provider_count == 0);
    CHECK(none->provider_total == 0);
    CHECK(!none->has_quota);
    CHECK_NEAR(none->quota_remaining, 100.0, 0.001);
    free(none);
}

static void test_quotas_edge_cases(void)
{
    magpie_snapshot_t snapshot;
    magpie_snapshot_clear(&snapshot);

    /* Unlimited windows never become the constraint. */
    const char *unlimited =
        "{\"data\":[{\"provider\":\"a\",\"name\":\"A\",\"windows\":["
        "{\"name\":\"Limit\",\"remaining\":100,\"unlimited\":true},"
        "{\"name\":\"Month\",\"remaining\":40}]}]}";
    CHECK(magpie_parse_quotas(unlimited, strlen(unlimited), 0, NULL, &snapshot) ==
          MAGPIE_OK);
    CHECK(snapshot.provider_count == 1);
    CHECK(snapshot.providers[0].window_count == 2);
    CHECK_NEAR(snapshot.providers[0].worst_remaining, 40.0, 0.001);
    CHECK_STR(snapshot.quota_window, "Month");

    /* A provider whose only window is unlimited reports 100% and is kept. */
    magpie_snapshot_clear(&snapshot);
    const char *only_unlimited =
        "{\"data\":[{\"provider\":\"k\",\"windows\":["
        "{\"name\":\"Limit\",\"remaining\":100,\"unlimited\":true}]}]}";
    CHECK(magpie_parse_quotas(only_unlimited, strlen(only_unlimited), 0, NULL,
                              &snapshot) == MAGPIE_OK);
    CHECK(snapshot.providers[0].unlimited);
    CHECK_NEAR(snapshot.providers[0].worst_remaining, 100.0, 0.001);
    CHECK(!snapshot.has_quota);

    /* Used-only windows derive remaining so the row does not read 100%. */
    magpie_snapshot_clear(&snapshot);
    const char *used_only =
        "{\"data\":[{\"provider\":\"u\",\"windows\":["
        "{\"name\":\"5 hours\",\"used\":84}]}]}";
    CHECK(magpie_parse_quotas(used_only, strlen(used_only), 0, NULL, &snapshot) ==
          MAGPIE_OK);
    CHECK_NEAR(snapshot.providers[0].windows[0].remaining_percent, 16.0, 0.001);

    /* Out-of-range percentages are clamped. */
    magpie_snapshot_clear(&snapshot);
    const char *clamped =
        "{\"data\":[{\"provider\":\"c\",\"windows\":["
        "{\"name\":\"a\",\"remaining\":150},"
        "{\"name\":\"b\",\"remaining\":-20}]}]}";
    CHECK(magpie_parse_quotas(clamped, strlen(clamped), 0, NULL, &snapshot) ==
          MAGPIE_OK);
    CHECK_NEAR(snapshot.providers[0].windows[0].remaining_percent, 100.0, 0.001);
    CHECK_NEAR(snapshot.providers[0].windows[1].remaining_percent, 0.0, 0.001);
    CHECK_NEAR(snapshot.providers[0].worst_remaining, 0.0, 0.001);

    /* Provider cap: extra rows are counted but not stored. */
    magpie_snapshot_clear(&snapshot);
    char big[8192];
    size_t used = (size_t)snprintf(big, sizeof(big), "{\"data\":[");
    for (int i = 0; i < 20; ++i) {
        used += (size_t)snprintf(big + used, sizeof(big) - used,
                                 "%s{\"provider\":\"p%d\",\"windows\":["
                                 "{\"name\":\"5 hours\",\"remaining\":%d}]}",
                                 i ? "," : "", i, 100 - i);
    }
    snprintf(big + used, sizeof(big) - used, "]}");
    CHECK(magpie_parse_quotas(big, strlen(big), 0, NULL, &snapshot) == MAGPIE_OK);
    CHECK(snapshot.provider_total == 20);
    CHECK(snapshot.provider_count == MAGPIE_PROVIDERS_MAX);
    CHECK_NEAR(snapshot.quota_remaining, 81.0, 0.001);

    /* Malformed and empty documents are rejected without touching the model. */
    magpie_snapshot_clear(&snapshot);
    CHECK(magpie_parse_quotas("not json", 8, 0, NULL, &snapshot) ==
          MAGPIE_ERR_INVALID_RESPONSE);
    CHECK(magpie_parse_quotas("{}", 2, 0, NULL, &snapshot) ==
          MAGPIE_ERR_INVALID_RESPONSE);
    CHECK(magpie_parse_quotas(NULL, 0, 0, NULL, &snapshot) ==
          MAGPIE_ERR_INVALID_ARG);

    /* An empty data array is a valid response with nothing to show. */
    magpie_snapshot_clear(&snapshot);
    CHECK(magpie_parse_quotas("{\"data\":[]}", 11, 0, NULL, &snapshot) ==
          MAGPIE_OK);
    CHECK(snapshot.provider_count == 0);
    CHECK(!snapshot.has_quota);
}

/* ---- usage ----------------------------------------------------------- */

static void test_usage_fixture(void)
{
    const int64_t now = 1792543800LL;
    magpie_snapshot_t *snapshot = load_snapshot(now, NULL);
    if (!snapshot) {
        return;
    }
    const magpie_usage_t *usage = &snapshot->usage;

    /* Aggregate block from the captured 30-day response. */
    CHECK(usage->calls == 158);
    CHECK(usage->errors == 2);
    CHECK(usage->input == 269086);
    CHECK(usage->output == 78416);
    CHECK(usage->reasoning == 26547);
    CHECK(usage->cache_read == 6087681);
    CHECK(usage->cache_write == 0);

    /* Sum of every component, matching the gateway's own token accounting. */
    CHECK(usage->tokens_total ==
          usage->input + usage->output + usage->reasoning + usage->cache_read +
              usage->cache_write);

    /* Today is the last bucket; 7d is the sum of the kept buckets. */
    CHECK(usage->series_count == 7);
    CHECK(usage->tokens_today == 5113992ULL);
    CHECK(usage->tokens_today == usage->series_tokens[6]);

    uint64_t expected_7d = 0;
    for (uint8_t i = 0; i < usage->series_count; ++i) {
        expected_7d += usage->series_tokens[i];
    }
    CHECK(usage->tokens_7d == expected_7d);
    CHECK(usage->tokens_7d == 6461730ULL);
    CHECK(usage->tokens_7d > usage->tokens_today);
    /* The capture keeps only seven daily buckets, so the 7-day sum covers the
     * whole captured series and can equal the aggregate total. */
    CHECK(usage->tokens_7d <= usage->tokens_total);

    /* Oct 20 is the quiet day inside the kept window. */
    CHECK(usage->series_tokens[5] == 1020649ULL);
    CHECK(usage->series_tokens[2] == 0);

    CHECK(usage->calls_today == 79);
    CHECK(usage->errors_today == 0);

    /* ttft_ms is a total; the average is ttft_ms / timed. */
    CHECK(usage->timed_calls == 156);
    CHECK(usage->average_ttft_ms == usage->ttft_ms_total / usage->timed_calls);
    CHECK(usage->average_ttft_ms == 3133);
    CHECK(usage->cost_micros == 216919);

    /* Latest request comes from the requests document. */
    CHECK_STR(snapshot->latest.model, "deepseek-v4.1-flash");
    CHECK(snapshot->latest.status == 200);
    CHECK(snapshot->latest.latency_ms == 3116);
    CHECK(!snapshot->latest.failed);

    free(snapshot);
}

static void test_usage_edge_cases(void)
{
    magpie_snapshot_t snapshot;
    magpie_snapshot_clear(&snapshot);

    /* Empty response: every aggregate reads 0, no series, no division by zero. */
    CHECK(magpie_parse_usage("{}", 2, &snapshot) == MAGPIE_OK);
    CHECK(snapshot.usage.calls == 0);
    CHECK(snapshot.usage.tokens_total == 0);
    CHECK(snapshot.usage.tokens_today == 0);
    CHECK(snapshot.usage.tokens_7d == 0);
    CHECK(snapshot.usage.average_ttft_ms == 0);
    CHECK(snapshot.usage.series_count == 0);

    /* A series longer than the kept window keeps only the newest buckets. */
    char doc[4096];
    size_t used = (size_t)snprintf(doc, sizeof(doc), "{\"series\":[");
    for (int i = 0; i < 30; ++i) {
        used += (size_t)snprintf(doc + used, sizeof(doc) - used,
                                 "%s{\"input\":%d,\"output\":1,\"calls\":%d}",
                                 i ? "," : "", (i + 1) * 10, i);
    }
    snprintf(doc + used, sizeof(doc) - used, "]}");
    CHECK(magpie_parse_usage(doc, strlen(doc), &snapshot) == MAGPIE_OK);
    CHECK(snapshot.usage.series_count == MAGPIE_SERIES_MAX);
    /* Today (i == 29) has input 300 + output 1. */
    CHECK(snapshot.usage.tokens_today == 301);
    CHECK(snapshot.usage.calls_today == 29);
    /* Buckets 23..29 keep inputs 240..300, each with one output token. */
    CHECK(snapshot.usage.tokens_7d ==
          (241 + 251 + 261 + 271 + 281 + 291 + 301));

    /* "errors" larger than "calls" is clamped for a sane success rate. */
    magpie_snapshot_clear(&snapshot);
    const char *odd = "{\"series\":[{\"input\":1,\"calls\":2,\"errors\":9}]}";
    CHECK(magpie_parse_usage(odd, strlen(odd), &snapshot) == MAGPIE_OK);
    CHECK(snapshot.usage.errors_today == 2);

    CHECK(magpie_parse_usage("[]", 2, &snapshot) == MAGPIE_ERR_INVALID_RESPONSE);
    CHECK(magpie_parse_usage(NULL, 0, &snapshot) == MAGPIE_ERR_INVALID_ARG);
}

/* ---- requests -------------------------------------------------------- */

static void test_requests_edge_cases(void)
{
    magpie_snapshot_t snapshot;
    magpie_snapshot_clear(&snapshot);

    /* No rows: the UI shows a dash instead of stale data. */
    CHECK(magpie_parse_requests("{\"rows\":[]}", 11, &snapshot) == MAGPIE_OK);
    CHECK_STR(snapshot.latest.model, "");
    CHECK(snapshot.latest.status == 0);

    /* Server error status marks the request failed. */
    const char *failed =
        "{\"rows\":[{\"served\":\"m\",\"status\":500,\"ms\":10}]}";
    CHECK(magpie_parse_requests(failed, strlen(failed), &snapshot) == MAGPIE_OK);
    CHECK(snapshot.latest.failed);

    /* A non-ASCII model name is sanitised for the ASCII UI font. */
    const char *utf8 =
        "{\"rows\":[{\"served\":\"mo\\u00e9l\",\"status\":200}]}";
    CHECK(magpie_parse_requests(utf8, strlen(utf8), &snapshot) == MAGPIE_OK);
    CHECK_STR(snapshot.latest.model, "mo?l");

    /* An explicit error flag is enough to mark failure at status 200. */
    const char *flagged =
        "{\"rows\":[{\"model\":\"m\",\"status\":200,\"err\":1}]}";
    CHECK(magpie_parse_requests(flagged, strlen(flagged), &snapshot) == MAGPIE_OK);
    CHECK(snapshot.latest.failed);
    CHECK_STR(snapshot.latest.model, "m");
}

/* ---- health ---------------------------------------------------------- */

static void test_health(void)
{
    magpie_snapshot_t snapshot;
    magpie_snapshot_clear(&snapshot);

    /* A non-Magpie endpoint is rejected: this is what stops the device from
     * rendering a captive-portal or proxy page as if it were usage data. */
    const char *other = "{\"name\":\"something-else\",\"version\":\"1\"}";
    CHECK(magpie_parse_health(other, strlen(other), &snapshot) ==
          MAGPIE_ERR_INVALID_RESPONSE);

    const char *magpie = "{\"name\":\"magpie\",\"version\":\"0.1.1268\"}";
    CHECK(magpie_parse_health(magpie, strlen(magpie), &snapshot) == MAGPIE_OK);
    CHECK_STR(snapshot.gateway_version, "0.1.1268");
}

/* ---- formatting ------------------------------------------------------ */

static void check_format_percent(float value, bool known, const char *expected)
{
    char text[16];
    magpie_format_percent(value, known, text, sizeof(text));
    CHECK_STR(text, expected);
}

static void check_format_count(uint64_t value, const char *expected)
{
    char text[24];
    magpie_format_count(value, text, sizeof(text));
    CHECK_STR(text, expected);
}

static void check_format_duration(int32_t seconds, bool known, const char *expected)
{
    char text[24];
    magpie_format_duration(seconds, known, text, sizeof(text));
    CHECK_STR(text, expected);
}

static void test_format(void)
{
    check_format_percent(84.0f, true, "84%");
    check_format_percent(0.0f, true, "0.0%");
    check_format_percent(3.7f, true, "3.7%");
    check_format_percent(9.96f, true, "10.0%");
    check_format_percent(100.0f, true, "100%");
    check_format_percent(120.0f, true, "100%");
    check_format_percent(-5.0f, true, "0.0%");
    check_format_percent(50.0f, false, "--");

    check_format_count(0, "0");
    check_format_count(512, "512");
    check_format_count(1000, "1.0K");
    check_format_count(12345, "12.3K");
    check_format_count(1250000, "1.25M");
    check_format_count(5172035, "5.17M");
    check_format_count(3400000000ULL, "3.40B");

    check_format_duration(0, true, "now");
    check_format_duration(-5, true, "now");
    check_format_duration(30, true, "<1m");
    check_format_duration(42 * 60, true, "42m");
    check_format_duration(3 * 3600 + 5 * 60, true, "3h 05m");
    check_format_duration(2 * 86400 + 8 * 3600, true, "2d 8h");
    check_format_duration(100, false, "--");

    char text[24];
    magpie_format_cost(0, text, sizeof(text));
    CHECK_STR(text, "$0");
    magpie_format_cost(1230000, text, sizeof(text));
    CHECK_STR(text, "$1.23");
    magpie_format_cost(4000, text, sizeof(text));
    CHECK_STR(text, "$0.004");
    magpie_format_cost(226035, text, sizeof(text));
    CHECK_STR(text, "$0.22");

    magpie_format_age(2, true, text, sizeof(text));
    CHECK_STR(text, "just now");
    magpie_format_age(12, true, text, sizeof(text));
    CHECK_STR(text, "12s ago");
    magpie_format_age(240, true, text, sizeof(text));
    CHECK_STR(text, "4m ago");
    magpie_format_age(7300, true, text, sizeof(text));
    CHECK_STR(text, "2h ago");
    magpie_format_age(0, false, text, sizeof(text));
    CHECK_STR(text, "never");

    magpie_format_latency(333, text, sizeof(text));
    CHECK_STR(text, "333ms");
    magpie_format_latency(8300, text, sizeof(text));
    CHECK_STR(text, "8.3s");
    magpie_format_latency(125000, text, sizeof(text));
    CHECK_STR(text, "2m 05s");

    magpie_short_provider("office/claude", text, sizeof(text));
    CHECK_STR(text, "claude");
    magpie_short_provider("copilot", text, sizeof(text));
    CHECK_STR(text, "copilot");
    magpie_short_provider(NULL, text, sizeof(text));
    CHECK_STR(text, "");

    magpie_upper("live", text, sizeof(text));
    CHECK_STR(text, "LIVE");
    magpie_upper(NULL, text, sizeof(text));
    CHECK_STR(text, "");

    /* Every writer must terminate exactly, even with a one-byte buffer. */
    char one = 'x';
    magpie_format_count(1234567, &one, 1);
    CHECK(one == '\0');
    magpie_format_percent(50, true, &one, 1);
    CHECK(one == '\0');
}

/* ---- end-to-end: fixtures -> model -> rendered strings ---------------- */

static void test_render_pipeline(void)
{
    const int64_t now = 1792543800LL;
    magpie_snapshot_t *snapshot = load_snapshot(now, "office/");
    if (!snapshot) {
        return;
    }

    char percent[16];
    char count[24];
    char provider[MAGPIE_TEXT_MAX];
    magpie_format_percent(snapshot->quota_remaining, snapshot->has_quota, percent,
                          sizeof(percent));
    CHECK_STR(percent, "0.0%");
    magpie_format_count(snapshot->usage.tokens_today, count, sizeof(count));
    CHECK_STR(count, "5.11M");
    magpie_short_provider(snapshot->quota_provider, provider, sizeof(provider));
    CHECK_STR(provider, "kimi-code-cn");
    free(snapshot);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <fixtures-dir>\n", argv[0]);
        return 2;
    }
    g_fixture_dir = argv[1];

    test_iso8601();
    test_json_scanner();
    test_quotas_fixture();
    test_quotas_filter();
    test_quotas_edge_cases();
    test_usage_fixture();
    test_usage_edge_cases();
    test_requests_edge_cases();
    test_health();
    test_format();
    test_render_pipeline();

    printf("%u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
