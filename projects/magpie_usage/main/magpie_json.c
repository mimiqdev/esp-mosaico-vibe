// SPDX-License-Identifier: Apache-2.0

#include "magpie_json.h"

#include <stdlib.h>
#include <string.h>

#define MJSON_LEAVE_GUARD 4096

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool at_end(const mjson_t *json)
{
    return json->p >= json->end;
}

static void skip_space(mjson_t *json)
{
    while (json->p < json->end && is_space(*json->p)) {
        ++json->p;
    }
}

static bool skip_string(mjson_t *json)
{
    if (at_end(json) || *json->p != '"') {
        return false;
    }
    ++json->p;
    while (!at_end(json)) {
        const char c = *json->p++;
        if (c == '\\') {
            if (at_end(json)) {
                return false;
            }
            ++json->p;
        } else if (c == '"') {
            return true;
        }
    }
    return false;
}

static bool skip_literal(mjson_t *json, const char *literal)
{
    const size_t len = strlen(literal);
    if ((size_t)(json->end - json->p) < len ||
        memcmp(json->p, literal, len) != 0) {
        return false;
    }
    json->p += len;
    return true;
}

static bool skip_number(mjson_t *json)
{
    const char *start = json->p;
    if (!at_end(json) && (*json->p == '-' || *json->p == '+')) {
        ++json->p;
    }
    while (!at_end(json) && ((*json->p >= '0' && *json->p <= '9') ||
                             *json->p == '.' || *json->p == 'e' ||
                             *json->p == 'E')) {
        ++json->p;
    }
    return json->p > start;
}

static bool skip_scalar(mjson_t *json)
{
    switch (*json->p) {
    case '"':
        return skip_string(json);
    case 't':
        return skip_literal(json, "true");
    case 'f':
        return skip_literal(json, "false");
    case 'n':
        return skip_literal(json, "null");
    default:
        return skip_number(json);
    }
}

void mjson_init(mjson_t *json, const char *text, size_t len)
{
    json->p = text;
    json->end = text + len;
    json->depth = 0;
}

/* Consumes one complete value, including nested containers. */
static bool skip_value(mjson_t *json)
{
    skip_space(json);
    if (at_end(json)) {
        return false;
    }
    const char open = *json->p;
    if (open != '{' && open != '[') {
        return skip_scalar(json);
    }
    if (json->depth >= MAGPIE_JSON_DEPTH_MAX) {
        return false;
    }
    ++json->depth;
    ++json->p;
    const bool is_object = open == '{';
    const char close = is_object ? '}' : ']';
    skip_space(json);
    if (!at_end(json) && *json->p == close) {
        ++json->p;
        --json->depth;
        return true;
    }
    for (;;) {
        skip_space(json);
        if (at_end(json)) {
            return false;
        }
        if (is_object) {
            if (!skip_string(json)) {
                return false;
            }
            skip_space(json);
            if (at_end(json) || *json->p != ':') {
                return false;
            }
            ++json->p;
        }
        if (!skip_value(json)) {
            return false;
        }
        skip_space(json);
        if (at_end(json)) {
            return false;
        }
        if (*json->p == ',') {
            ++json->p;
            continue;
        }
        if (*json->p == close) {
            ++json->p;
            --json->depth;
            return true;
        }
        return false;
    }
}

bool mjson_skip(mjson_t *json)
{
    return skip_value(json);
}

bool mjson_at_object(const mjson_t *json)
{
    mjson_t probe = *json;
    skip_space(&probe);
    return !at_end(&probe) && *probe.p == '{';
}

bool mjson_enter(mjson_t *json)
{
    skip_space(json);
    if (at_end(json)) {
        return false;
    }
    if (*json->p != '{' && *json->p != '[') {
        return false;
    }
    if (json->depth >= MAGPIE_JSON_DEPTH_MAX) {
        return false;
    }
    ++json->depth;
    ++json->p;
    return true;
}

bool mjson_leave(mjson_t *json)
{
    unsigned guard = 0;
    for (;;) {
        skip_space(json);
        if (at_end(json)) {
            return false;
        }
        const char c = *json->p;
        if (c == '}' || c == ']') {
            ++json->p;
            if (json->depth) {
                --json->depth;
            }
            return true;
        }
        /* Tolerate a partially walked object or array: separators and the
         * member colon are consumed here, values by skip_value(). */
        if (c == ',' || c == ':') {
            ++json->p;
            continue;
        }
        if (!mjson_skip(json)) {
            return false;
        }
        if (++guard > MJSON_LEAVE_GUARD) {
            return false;
        }
    }
}

static bool at_container_end(const mjson_t *json)
{
    return at_end(json) || *json->p == '}' || *json->p == ']';
}

bool mjson_next_key(mjson_t *json, char *out_key, size_t key_size)
{
    skip_space(json);
    if (at_container_end(json)) {
        return false;
    }
    if (*json->p == ',') {
        ++json->p;
    }
    skip_space(json);
    if (at_end(json) || *json->p != '"') {
        return false;
    }
    if (!mjson_read_string(json, out_key, key_size)) {
        return false;
    }
    skip_space(json);
    if (at_end(json) || *json->p != ':') {
        return false;
    }
    ++json->p;
    return true;
}

bool mjson_next_element(mjson_t *json)
{
    skip_space(json);
    if (at_container_end(json)) {
        return false;
    }
    if (*json->p == ',') {
        ++json->p;
        skip_space(json);
    }
    return !at_container_end(json);
}

bool mjson_read_string(mjson_t *json, char *out, size_t out_size)
{
    skip_space(json);
    if (at_end(json) || *json->p != '"') {
        return false;
    }
    ++json->p;
    size_t written = 0;
    while (!at_end(json)) {
        char c = *json->p;
        if (c == '"') {
            ++json->p;
            if (out_size) {
                out[written < out_size ? written : out_size - 1] = '\0';
            }
            return true;
        }
        if (c == '\\') {
            ++json->p;
            if (at_end(json)) {
                return false;
            }
            switch (*json->p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                /* The UI is ASCII-only, so a non-printable escape becomes '?'. */
                ++json->p;
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    if (at_end(json)) {
                        return false;
                    }
                    const char h = *json->p;
                    unsigned digit;
                    if (h >= '0' && h <= '9') {
                        digit = (unsigned)(h - '0');
                    } else if (h >= 'a' && h <= 'f') {
                        digit = (unsigned)(h - 'a' + 10);
                    } else if (h >= 'A' && h <= 'F') {
                        digit = (unsigned)(h - 'A' + 10);
                    } else {
                        return false;
                    }
                    code = code * 16 + digit;
                    ++json->p;
                }
                c = (code >= 0x20 && code < 0x7f) ? (char)code : '?';
                if (out_size && written + 1 < out_size) {
                    out[written++] = c;
                }
                continue;
            }
            default: c = *json->p; break;
            }
        }
        if (out_size && written + 1 < out_size) {
            out[written++] = c;
        }
        ++json->p;
    }
    return false;
}

bool mjson_read_double(mjson_t *json, double *out)
{
    skip_space(json);
    if (at_end(json)) {
        return false;
    }
    if (*json->p == '"' || *json->p == '{' || *json->p == '[' ||
        !(*json->p == '-' || *json->p == '+' || *json->p == '.' ||
          (*json->p >= '0' && *json->p <= '9'))) {
        return false;
    }
    const char *start = json->p;
    if (!skip_number(json)) {
        return false;
    }
    char scratch[32];
    const size_t len = (size_t)(json->p - start);
    if (len == 0 || len >= sizeof(scratch)) {
        return false;
    }
    memcpy(scratch, start, len);
    scratch[len] = '\0';
    char *endptr = NULL;
    const double value = strtod(scratch, &endptr);
    if (endptr != scratch + len) {
        return false;
    }
    *out = value;
    return true;
}

bool mjson_read_int64(mjson_t *json, int64_t *out)
{
    double value = 0;
    if (!mjson_read_double(json, &value)) {
        return false;
    }
    *out = (int64_t)value;
    return true;
}

bool mjson_read_bool(mjson_t *json, bool *out)
{
    skip_space(json);
    if (at_end(json)) {
        return false;
    }
    if (*json->p == 't') {
        if (!skip_literal(json, "true")) {
            return false;
        }
        *out = true;
        return true;
    }
    if (*json->p == 'f') {
        if (!skip_literal(json, "false")) {
            return false;
        }
        *out = false;
        return true;
    }
    char word[8];
    if (*json->p == '"') {
        if (!mjson_read_string(json, word, sizeof(word))) {
            return false;
        }
        if (strcmp(word, "true") == 0 || strcmp(word, "1") == 0) {
            *out = true;
            return true;
        }
        *out = false;
        return true;
    }
    double number = 0;
    if (!mjson_read_double(json, &number)) {
        return false;
    }
    *out = number != 0;
    return true;
}

void mjson_read_number_lenient(mjson_t *json, double *out)
{
    *out = 0;
    skip_space(json);
    if (at_end(json)) {
        return;
    }
    const char c = *json->p;
    if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
        (void)mjson_read_double(json, out);
        return;
    }
    (void)mjson_skip(json);
}

bool mjson_validate(const mjson_t *json)
{
    mjson_t probe = *json;
    if (!skip_value(&probe)) {
        return false;
    }
    skip_space(&probe);
    return probe.p == probe.end;
}
