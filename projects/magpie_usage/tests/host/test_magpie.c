// SPDX-License-Identifier: Apache-2.0

/*
 * Host test harness for the portable Magpie display sources.
 *
 * Deliberately a plain C program instead of a framework: the host runner in
 * run_tests.py already builds these exact device sources with ASan/UBSan, and
 * keeping the harness dependency free means the tests can run anywhere a C
 * compiler exists.
 *
 * argv[1] is the fixtures directory; payload fixtures are generated from the
 * captured Magpie document by tools/make_payloads.py, so the decoder is tested
 * against the bytes the companion really sends.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "magpie_format.h"
#include "magpie_json.h"
#include "magpie_model.h"
#include "magpie_parse.h"
#include "magpie_pull.h"
#include "magpie_source.h"
#include "magpie_ui_state.h"

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
    if (out_len) {
        *out_len = read;
    }
    return buffer;
}

/* ---- JSON scanner ---------------------------------------------------- */

static void test_json_scanner(void)
{
    const char *doc = "{\"a\":1,\"b\":[true,false,null],\"c\":{\"d\":\"x\\u00e9y\"},"
                      "\"e\":\"quote\\\"inside\",\"n\":{\"deep\":[1,{\"k\":2}]}}";
    mjson_t json;
    mjson_init(&json, doc, strlen(doc));
    CHECK(mjson_validate(&json));
    CHECK(mjson_at_object(&json));

    mjson_t broken;
    const char *bad = "{\"a\":\"unterminated}";
    mjson_init(&broken, bad, strlen(bad));
    CHECK(!mjson_validate(&broken));

    mjson_t trailing;
    const char *junk = "{\"a\":1} trailing";
    mjson_init(&trailing, junk, strlen(junk));
    CHECK(!mjson_validate(&trailing));

    /* Skipping a nested value must consume exactly that value. */
    mjson_t skip;
    const char *nested = "{\"skip\":{\"x\":[1,2,{\"y\":3}]},\"after\":7}";
    mjson_init(&skip, nested, strlen(nested));
    char key[MAGPIE_WIN_LABEL_MAX];
    CHECK(mjson_enter(&skip));
    CHECK(mjson_next_key(&skip, key, sizeof(key)));
    CHECK_STR(key, "skip");
    CHECK(mjson_skip(&skip));
    CHECK(mjson_next_key(&skip, key, sizeof(key)));
    CHECK_STR(key, "after");
    double value = 0;
    CHECK(mjson_read_double(&skip, &value));
    CHECK(value == 7.0);
    CHECK(mjson_leave(&skip));

    /* Walking every member of the document works with nesting. */
    mjson_t walk;
    mjson_init(&walk, doc, strlen(doc));
    CHECK(mjson_enter(&walk));
    unsigned members = 0;
    while (mjson_next_key(&walk, key, sizeof(key))) {
        ++members;
        CHECK(mjson_skip(&walk));
    }
    CHECK(members == 5);
    CHECK(mjson_leave(&walk));

    /* Overlong strings truncate and terminate. */
    mjson_t truncate;
    const char *long_text = "{\"k\":\"abcdefghij\"}";
    mjson_init(&truncate, long_text, strlen(long_text));
    CHECK(mjson_enter(&truncate));
    CHECK(mjson_next_key(&truncate, key, sizeof(key)));
    char tiny[4];
    CHECK(mjson_read_string(&truncate, tiny, sizeof(tiny)));
    CHECK_STR(tiny, "abc");
    CHECK(mjson_leave(&truncate));
}

/* ---- INDEX payload --------------------------------------------------- */

static void test_index_fixture(void)
{
    size_t len = 0;
    char *body = load_fixture("payloads/index.json", &len);
    if (!body) {
        return;
    }
    magpie_index_t index;
    CHECK(magpie_decode_index(body, len, &index) == MAGPIE_OK);
    CHECK(index.version == MAGPIE_PROTOCOL_VERSION);
    CHECK(index.sender_epoch_s == 1792543800LL);
    CHECK(index.count == 7);
    CHECK(index.id_count == 7);
    for (uint8_t i = 0; i < index.id_count; ++i) {
        CHECK(index.ids[i] == i);
    }
    /* Kimi's weekly window is fully used, so it is the glance summary. */
    CHECK(index.summary.valid);
    CHECK(index.summary.slot == 0);
    CHECK_STR(index.summary.name, "Kimi Code (China)");
    CHECK_STR(index.summary.window, "7d");
    CHECK(index.summary.used_percent == 100);
    CHECK(index.summary.reset_epoch_s == 1792573226LL);
    CHECK(len <= MAGPIE_INDEX_BYTES_MAX);
    free(body);
}

static void test_index_edge_cases(void)
{
    magpie_index_t index;
    magpie_index_clear(&index);
    CHECK(index.count == 0);
    CHECK(!index.summary.valid);

    /* Wrong protocol version is rejected. */
    const char *v2 = "{\"v\":2,\"t\":1,\"c\":1,\"lo\":{},\"ids\":[0]}";
    CHECK(magpie_decode_index(v2, strlen(v2), &index) ==
          MAGPIE_ERR_INVALID_RESPONSE);

    /* Malformed and non-object documents are rejected. */
    CHECK(magpie_decode_index("not json", 8, &index) ==
          MAGPIE_ERR_INVALID_RESPONSE);
    CHECK(magpie_decode_index("[]", 2, &index) ==
          MAGPIE_ERR_INVALID_RESPONSE);
    CHECK(magpie_decode_index(NULL, 0, &index) == MAGPIE_ERR_INVALID_ARG);

    /* A missing ids array falls back to 0..c-1. */
    const char *no_ids = "{\"v\":1,\"t\":5,\"c\":3,\"lo\":{}}";
    CHECK(magpie_decode_index(no_ids, strlen(no_ids), &index) == MAGPIE_OK);
    CHECK(index.count == 3);
    CHECK(index.id_count == 3);
    CHECK(index.ids[2] == 2);
    CHECK(!index.summary.valid);

    /* The count is capped at the number of pages the firmware can hold. */
    char big[512];
    snprintf(big, sizeof(big), "{\"v\":1,\"t\":1,\"c\":99,\"lo\":{},\"ids\":[]}");
    CHECK(magpie_decode_index(big, strlen(big), &index) == MAGPIE_OK);
    CHECK(index.count == MAGPIE_SLOTS_MAX);

    /* Out-of-range percents are clamped, unknown reset stays 0. */
    const char *odd =
        "{\"v\":1,\"t\":1,\"c\":1,\"lo\":{\"i\":0,\"n\":\"X\",\"w\":\"7d\","
        "\"u\":420,\"r\":0},\"ids\":[0]}";
    CHECK(magpie_decode_index(odd, strlen(odd), &index) == MAGPIE_OK);
    CHECK(index.summary.used_percent == 100);
    CHECK(index.summary.reset_epoch_s == 0);

    /* Oversized payloads are rejected before parsing. */
    char oversized[MAGPIE_INDEX_BYTES_MAX * 2 + 8];
    memset(oversized, ' ', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    oversized[0] = '{';
    oversized[1] = '}';
    CHECK(magpie_decode_index(oversized, strlen(oversized), &index) ==
          MAGPIE_ERR_INVALID_RESPONSE);
}

/* ---- SUB payload ----------------------------------------------------- */

static void test_sub_fixture(void)
{
    magpie_index_t index;
    size_t index_len = 0;
    char *index_body = load_fixture("payloads/index.json", &index_len);
    if (!index_body) {
        return;
    }
    CHECK(magpie_decode_index(index_body, index_len, &index) == MAGPIE_OK);
    free(index_body);

    for (uint8_t slot = 0; slot < index.count; ++slot) {
        char name[32];
        snprintf(name, sizeof(name), "payloads/sub-%u.json", (unsigned)slot);
        size_t len = 0;
        char *body = load_fixture(name, &len);
        if (!body) {
            return;
        }
        CHECK(len <= MAGPIE_SUB_BYTES_MAX);
        magpie_sub_t sub;
        CHECK(magpie_decode_sub(body, len, slot, &sub) == MAGPIE_OK);
        CHECK(sub.index == slot);
        CHECK(sub.loaded);
        CHECK(sub.name[0] != '\0');
        CHECK(sub.provider[0] != '\0');
        CHECK(sub.window_count <= MAGPIE_WIN_MAX);
        for (uint8_t w = 0; w < sub.window_count; ++w) {
            CHECK(sub.windows[w].label[0] != '\0');
            CHECK(sub.windows[w].used_percent <= 100);
        }
        /* Slot 0 is Kimi: weekly window fully used, then the 5-hour one. */
        if (slot == 0) {
            CHECK_STR(sub.provider, "kimi-code-cn");
            CHECK_STR(sub.name, "Kimi Code (China)");
            CHECK(!sub.error);
            CHECK(sub.window_count == 2);
            CHECK_STR(sub.windows[0].label, "7d");
            CHECK(sub.windows[0].used_percent == 100);
            CHECK(sub.windows[0].reset_epoch_s == 1792573226LL);
            CHECK_STR(sub.windows[1].label, "5h");
            CHECK(sub.windows[1].used_percent == 7);
        }
        /* The two Claude accounts keep distinct names when deduplicated. */
        if (slot == 1) {
            CHECK_STR(sub.provider, "claude");
            CHECK_STR(sub.name, "Claude Code (1)");
            CHECK_STR(sub.plan, "pro");
        }
        if (slot == 2) {
            CHECK_STR(sub.name, "Claude Code (2)");
        }
        /* A subscription with no reset time keeps 0 rather than garbage. */
        if (slot == 5) {
            CHECK_STR(sub.provider, "workbuddy");
            CHECK(sub.windows[0].reset_epoch_s == 0);
        }
        free(body);
    }
}

static void test_sub_edge_cases(void)
{
    magpie_sub_t sub;

    /* The slot must match the request; a mismatched reply is rejected. */
    const char *payload =
        "{\"i\":2,\"p\":\"claude\",\"n\":\"Claude\",\"pl\":\"Max\",\"e\":0,"
        "\"w\":[{\"n\":\"5h\",\"u\":84,\"r\":1792549000}]}";
    CHECK(magpie_decode_sub(payload, strlen(payload), 2, &sub) == MAGPIE_OK);
    CHECK(sub.index == 2);
    CHECK_STR(sub.name, "Claude");
    CHECK(sub.window_count == 1);
    CHECK(sub.windows[0].used_percent == 84);
    CHECK(magpie_decode_sub(payload, strlen(payload), 3, &sub) ==
          MAGPIE_ERR_INVALID_RESPONSE);
    /* -1 accepts any slot. */
    CHECK(magpie_decode_sub(payload, strlen(payload), -1, &sub) == MAGPIE_OK);

    /* Error/empty states survive decoding. */
    const char *error_payload =
        "{\"i\":0,\"p\":\"minimax\",\"n\":\"MiniMax\",\"pl\":\"\",\"e\":1,"
        "\"w\":[]}";
    CHECK(magpie_decode_sub(error_payload, strlen(error_payload), 0, &sub) ==
          MAGPIE_OK);
    CHECK(sub.error);
    CHECK(sub.window_count == 0);
    CHECK_STR(sub.plan, "");

    /* Extra windows are dropped, not overflowed. */
    const char *many =
        "{\"i\":0,\"p\":\"p\",\"n\":\"N\",\"pl\":\"x\",\"e\":0,\"w\":["
        "{\"n\":\"a\",\"u\":1},{\"n\":\"b\",\"u\":2},{\"n\":\"c\",\"u\":3},"
        "{\"n\":\"d\",\"u\":4}]}";
    CHECK(magpie_decode_sub(many, strlen(many), 0, &sub) == MAGPIE_OK);
    CHECK(sub.window_count == MAGPIE_WIN_MAX);
    CHECK(sub.windows[2].used_percent == 3);

    /* A payload without a name falls back to the provider id. */
    const char *no_name = "{\"i\":0,\"p\":\"grok\",\"w\":[]}";
    CHECK(magpie_decode_sub(no_name, strlen(no_name), 0, &sub) == MAGPIE_OK);
    CHECK_STR(sub.name, "grok");

    /* Neither name nor provider is unusable. */
    const char *empty = "{\"i\":0,\"w\":[]}";
    CHECK(magpie_decode_sub(empty, strlen(empty), 0, &sub) ==
          MAGPIE_ERR_INVALID_RESPONSE);

    /* Bad documents. */
    CHECK(magpie_decode_sub("nope", 4, 0, &sub) == MAGPIE_ERR_INVALID_RESPONSE);
    CHECK(magpie_decode_sub("[]", 2, 0, &sub) == MAGPIE_ERR_INVALID_RESPONSE);
    CHECK(magpie_decode_sub(NULL, 0, 0, &sub) == MAGPIE_ERR_INVALID_ARG);

    /* Used percent above 100 is clamped. */
    const char *clamped =
        "{\"i\":0,\"p\":\"p\",\"n\":\"N\",\"w\":[{\"n\":\"7d\",\"u\":250}]}";
    CHECK(magpie_decode_sub(clamped, strlen(clamped), 0, &sub) == MAGPIE_OK);
    CHECK(sub.windows[0].used_percent == 100);
}

/* ---- pull cycle ------------------------------------------------------ */

typedef struct {
    const char *index_body;
    const char *sub_bodies[MAGPIE_SLOTS_MAX];
    int index_result;
    int sub_fail_slot;      /* slot whose read fails; -1 for none */
    unsigned opens;
    unsigned closes;
    unsigned index_reads;
    unsigned sub_reads;
} fake_source_t;

static int fake_open(magpie_source_t *self)
{
    fake_source_t *fake = self->impl;
    ++fake->opens;
    return MAGPIE_SOURCE_OK;
}

static void fake_close(magpie_source_t *self)
{
    fake_source_t *fake = self->impl;
    ++fake->closes;
}

static int fake_read_index(magpie_source_t *self, char *buf, size_t cap)
{
    fake_source_t *fake = self->impl;
    ++fake->index_reads;
    if (fake->index_result != MAGPIE_SOURCE_OK) {
        return fake->index_result;
    }
    const size_t len = strlen(fake->index_body);
    if (len + 1 > cap) {
        return MAGPIE_SOURCE_ERR_PAYLOAD;
    }
    memcpy(buf, fake->index_body, len + 1);
    return (int)len;
}

static int fake_read_sub(magpie_source_t *self, uint8_t slot, char *buf,
                         size_t cap)
{
    fake_source_t *fake = self->impl;
    ++fake->sub_reads;
    if ((int)slot == fake->sub_fail_slot) {
        return MAGPIE_SOURCE_ERR_TIMEOUT;
    }
    if (slot >= MAGPIE_SLOTS_MAX || !fake->sub_bodies[slot]) {
        return MAGPIE_SOURCE_ERR_LINK;
    }
    const size_t len = strlen(fake->sub_bodies[slot]);
    if (len + 1 > cap) {
        return MAGPIE_SOURCE_ERR_PAYLOAD;
    }
    memcpy(buf, fake->sub_bodies[slot], len + 1);
    return (int)len;
}

static void load_pull_ctx(char *index_buf, char *sub_buf, magpie_index_t *index,
                          magpie_sub_t *subs, uint8_t *loaded)
{
    for (uint8_t slot = 0; slot < MAGPIE_SLOTS_MAX; ++slot) {
        magpie_sub_clear(&subs[slot], slot);
    }
    magpie_index_clear(index);
    *loaded = 0;
    (void)index_buf;
    (void)sub_buf;
}

static void test_pull_cycle(void)
{
    char *index_body = load_fixture("payloads/index.json", NULL);
    if (!index_body) {
        return;
    }
    fake_source_t fake;
    memset(&fake, 0, sizeof(fake));
    fake.index_body = index_body;
    fake.sub_fail_slot = -1;
    for (uint8_t slot = 0; slot < 7; ++slot) {
        char name[32];
        snprintf(name, sizeof(name), "payloads/sub-%u.json", (unsigned)slot);
        fake.sub_bodies[slot] = load_fixture(name, NULL);
    }

    magpie_source_t source;
    memset(&source, 0, sizeof(source));
    source.name = "fake";
    source.open = fake_open;
    source.close = fake_close;
    source.read_index = fake_read_index;
    source.read_sub = fake_read_sub;
    source.impl = &fake;

    char index_buf[MAGPIE_INDEX_BYTES_MAX + 1];
    char sub_buf[MAGPIE_SUB_BYTES_MAX + 1];
    magpie_index_t index;
    static magpie_sub_t subs[MAGPIE_SLOTS_MAX];
    uint8_t loaded = 0;
    load_pull_ctx(index_buf, sub_buf, &index, subs, &loaded);
    magpie_pull_ctx_t ctx = {
        .index_buf = index_buf,
        .sub_buf = sub_buf,
        .index_cap = sizeof(index_buf),
        .sub_cap = sizeof(sub_buf),
        .index = &index,
        .subs = subs,
        .loaded_count = &loaded,
    };
    magpie_pull_result_t result;

    CHECK(magpie_pull_once(&source, &ctx, &result));
    CHECK(result.index_ok);
    CHECK(result.subs_ok == 7);
    CHECK(result.subs_failed == 0);
    CHECK(result.last_error == MAGPIE_SOURCE_OK);
    CHECK(loaded == 7);
    CHECK(fake.opens == 1);
    CHECK(fake.closes == 1);
    CHECK(index.count == 7);
    CHECK_STR(subs[0].name, "Kimi Code (China)");

    /* One failing slot keeps the other pages. */
    fake.sub_fail_slot = 3;
    CHECK(magpie_pull_once(&source, &ctx, &result));
    CHECK(result.subs_ok == 6);
    CHECK(result.subs_failed == 1);
    CHECK(result.last_error == MAGPIE_SOURCE_ERR_TIMEOUT);
    CHECK(loaded == 6);
    CHECK(!subs[3].loaded);
    CHECK(subs[4].loaded);

    /* A failing INDEX aborts the cycle without touching the pages. */
    fake.index_result = MAGPIE_SOURCE_ERR_TIMEOUT;
    magpie_sub_t before = subs[4];
    CHECK(!magpie_pull_once(&source, &ctx, &result));
    CHECK(!result.index_ok);
    CHECK(result.last_error == MAGPIE_SOURCE_ERR_TIMEOUT);
    CHECK(memcmp(&subs[4], &before, sizeof(before)) == 0);
    CHECK(fake.closes == 3); /* closed even on failure */
    fake.index_result = MAGPIE_SOURCE_OK;

    /* A shorter list clears the slots that fell off. */
    const char *two = "{\"v\":1,\"t\":7,\"c\":2,\"lo\":{},\"ids\":[0,1]}";
    fake.index_body = two;
    CHECK(magpie_pull_once(&source, &ctx, &result));
    CHECK(index.count == 2);
    CHECK(loaded == 2);
    CHECK(!subs[2].loaded);

    for (uint8_t slot = 0; slot < 7; ++slot) {
        free((void *)fake.sub_bodies[slot]);
    }
    free(index_body);
}

/* ---- UI state -------------------------------------------------------- */

/* Like load_fixture() but a missing file is expected (fewer pages than
 * MAGPIE_SLOTS_MAX); it returns NULL without counting a failure. */
static char *load_fixture_optional(const char *name, size_t *out_len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_fixture_dir, name);
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    fclose(file);
    return load_fixture(name, out_len);
}

static void build_state_from_fixtures(magpie_state_t *state)
{
    magpie_state_init(state);
    size_t len = 0;
    char *body = load_fixture("payloads/index.json", &len);
    if (!body) {
        return;
    }
    magpie_index_t index;
    if (magpie_decode_index(body, len, &index) == MAGPIE_OK) {
        magpie_state_set_index(state, &index);
    }
    free(body);
    for (uint8_t slot = 0; slot < MAGPIE_SLOTS_MAX; ++slot) {
        char name[32];
        snprintf(name, sizeof(name), "payloads/sub-%u.json", (unsigned)slot);
        char *sub_body = load_fixture_optional(name, &len);
        if (!sub_body) {
            continue;
        }
        magpie_sub_t sub;
        if (magpie_decode_sub(sub_body, len, slot, &sub) == MAGPIE_OK) {
            CHECK(magpie_state_set_sub(state, &sub));
        }
        free(sub_body);
    }
}

static void test_ui_state(void)
{
    magpie_state_t state;
    build_state_from_fixtures(&state);
    CHECK(state.pages_visible == 7);
    CHECK(state.sub_count == 7);

    /* Nothing has arrived yet: CONNECTING, not a fake LIVE. */
    magpie_header_text_t header;
    magpie_header_text(&state, &header);
    CHECK_STR(header.status, "CONNECTING");
    CHECK_STR(header.transport, "");
    CHECK_STR(header.tightest, "waiting for first snapshot");
    CHECK_STR(header.counter, "1/7");
    magpie_state_set_page(&state, 2);
    magpie_header_text(&state, &header);
    CHECK_STR(header.counter, "3/7");
    magpie_state_set_page(&state, 0);

    magpie_state_mark_data(&state, 3, MAGPIE_TRANSPORT_BLE);
    magpie_header_text(&state, &header);
    CHECK_STR(header.status, "LIVE");
    CHECK_STR(header.transport, "BLE");
    CHECK(!header.stale);
    CHECK(strstr(header.tightest, "Kimi Code (China)") == header.tightest);
    CHECK(strstr(header.tightest, "7d") != NULL);
    CHECK(strstr(header.tightest, "100%") != NULL);

    /* 60 s without data is STALE, 180 s is OFFLINE. */
    magpie_state_tick(&state, MAGPIE_STALE_AFTER_S);
    CHECK(magpie_state_link(&state) == MAGPIE_LINK_STALE);
    magpie_header_text(&state, &header);
    CHECK_STR(header.status, "STALE");
    CHECK(header.stale);
    magpie_state_tick(&state, MAGPIE_STALE_AFTER_S * 3);
    CHECK(magpie_state_link(&state) == MAGPIE_LINK_OFFLINE);
    magpie_state_tick(&state, MAGPIE_STALE_AFTER_S - 1);
    CHECK(magpie_state_link(&state) == MAGPIE_LINK_LIVE);
    /* A Wi-Fi snapshot replaces the transport hint. */
    magpie_state_mark_data(&state, 1, MAGPIE_TRANSPORT_WIFI);
    magpie_header_text(&state, &header);
    CHECK_STR(header.transport, "WIFI");

    /* Per-page text. */
    magpie_page_text_t page;
    CHECK(magpie_page_text(&state, 0, &page));
    CHECK_STR(page.name, "Kimi Code (China)");
    CHECK_STR(page.plan, "");
    CHECK(!page.error);
    CHECK(page.window_count == 2);
    CHECK_STR(page.window_label[0], "7-DAY WINDOW");
    CHECK_STR(page.window_used[0], "100%");
    CHECK(page.window_used_percent[0] == 100);
    CHECK_STR(page.window_reset[0], "8h 10m");
    CHECK_STR(page.window_label[1], "5-HOUR WINDOW");
    CHECK_STR(page.window_used[1], "7%");

    CHECK(magpie_page_text(&state, 1, &page));
    CHECK_STR(page.name, "Claude Code (1)");
    CHECK_STR(page.plan, "PRO");
    CHECK_STR(page.window_label[0], "5-HOUR WINDOW");
    CHECK_STR(page.window_used[0], "98%");
    CHECK_STR(page.window_reset[0], "6h 40m");

    /* The unknown reset time renders as "--". */
    CHECK(magpie_page_text(&state, 5, &page));
    CHECK_STR(page.name, "WorkBuddy");
    CHECK_STR(page.window_reset[0], "--");

    /* A page outside the reported count is empty. */
    CHECK(!magpie_page_text(&state, 9, &page));
    CHECK(page.empty);
    CHECK_STR(page.name, "No subscription");

    /* A page whose SUB never arrived reads as loading. */
    magpie_sub_t cleared;
    magpie_sub_clear(&cleared, 4);
    state.subs[4] = cleared;
    CHECK(!magpie_page_text(&state, 4, &page));
    CHECK(page.empty);
    CHECK_STR(page.name, "Loading...");

    /* Signed-out providers show their error flag. */
    magpie_sub_t errored;
    magpie_sub_clear(&errored, 4);
    errored.loaded = true;
    errored.error = true;
    strcpy(errored.name, "MiniMax");
    strcpy(errored.provider, "minimax");
    state.subs[4] = errored;
    CHECK(magpie_page_text(&state, 4, &page));
    CHECK(page.error);
    CHECK(page.window_count == 0);

    /* A shrinking index hides pages and forgets their payloads. */
    magpie_index_t smaller;
    magpie_index_clear(&smaller);
    smaller.version = 1;
    smaller.count = 3;
    smaller.id_count = 3;
    smaller.ids[0] = 0;
    smaller.ids[1] = 1;
    smaller.ids[2] = 2;
    magpie_state_set_index(&state, &smaller);
    CHECK(state.pages_visible == 3);
    CHECK(state.sub_count == 3);
    CHECK(!state.subs[6].loaded);
    /* A SUB for a hidden slot is refused. */
    magpie_sub_t late;
    magpie_sub_clear(&late, 6);
    late.loaded = true;
    strcpy(late.name, "Late");
    strcpy(late.provider, "late");
    CHECK(!magpie_state_set_sub(&state, &late));
}

static void test_countdown_format(void)
{
    char text[24];
    magpie_format_countdown(0, 1000, text, sizeof(text));
    CHECK_STR(text, "--");
    magpie_format_countdown(-5, 1000, text, sizeof(text));
    CHECK_STR(text, "--");
    magpie_format_countdown(1000, 1000, text, sizeof(text));
    CHECK_STR(text, "now");
    magpie_format_countdown(1000 + 30, 1000, text, sizeof(text));
    CHECK_STR(text, "<1m");
    magpie_format_countdown(1000 + 42 * 60, 1000, text, sizeof(text));
    CHECK_STR(text, "42m");
    magpie_format_countdown(1000 + 3 * 3600 + 5 * 60, 1000, text,
                            sizeof(text));
    CHECK_STR(text, "3h 05m");
    magpie_format_countdown(1000 + 2 * 86400 + 8 * 3600, 1000, text,
                            sizeof(text));
    CHECK_STR(text, "2d 8h");

    magpie_format_plan("pro", text, sizeof(text));
    CHECK_STR(text, "PRO");
    magpie_format_plan("", text, sizeof(text));
    CHECK_STR(text, "");
    magpie_format_plan(NULL, text, sizeof(text));
    CHECK_STR(text, "");
    magpie_format_plan("none", text, sizeof(text));
    CHECK_STR(text, "");
    magpie_format_plan("Team", text, sizeof(text));
    CHECK_STR(text, "TEAM");
}

/* ---- shared formatters ---------------------------------------------- */

static void check_format_percent(float value, bool known, const char *expected)
{
    char text[16];
    magpie_format_percent(value, known, text, sizeof(text));
    CHECK_STR(text, expected);
}

static void test_format(void)
{
    check_format_percent(84.0f, true, "84%");
    check_format_percent(0.0f, true, "0.0%");
    check_format_percent(3.7f, true, "3.7%");
    check_format_percent(100.0f, true, "100%");
    check_format_percent(120.0f, true, "100%");
    check_format_percent(-5.0f, true, "0.0%");
    check_format_percent(50.0f, false, "--");

    char text[24];
    magpie_format_count(0, text, sizeof(text));
    CHECK_STR(text, "0");
    magpie_format_count(12345, text, sizeof(text));
    CHECK_STR(text, "12.3K");
    magpie_format_count(5172035, text, sizeof(text));
    CHECK_STR(text, "5.17M");

    magpie_upper("live", text, sizeof(text));
    CHECK_STR(text, "LIVE");
    magpie_short_provider("office/claude", text, sizeof(text));
    CHECK_STR(text, "claude");

    /* Every writer terminates even with a one-byte buffer. */
    char one = 'x';
    magpie_format_count(1234567, &one, 1);
    CHECK(one == '\0');
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <fixtures-dir>\n", argv[0]);
        return 2;
    }
    g_fixture_dir = argv[1];

    test_json_scanner();
    test_index_fixture();
    test_index_edge_cases();
    test_sub_fixture();
    test_sub_edge_cases();
    test_pull_cycle();
    test_ui_state();
    test_countdown_format();
    test_format();

    printf("%u checks, %u failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
