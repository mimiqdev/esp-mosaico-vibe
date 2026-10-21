// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Minimal read-only JSON scanner shared by the device build and the host tests.
 *
 * It is not a general-purpose JSON library: it walks an already validated
 * document and exposes just enough navigation for magpie_parse.c to pull known
 * fields out of Magpie's responses. Keeping it dependency free avoids adding a
 * JSON component for three fixed payload shapes, and lets the exact same code
 * run under the host tests.
 *
 * Navigation contract:
 *
 *   mjson_enter()        consume '{' or '[', depth++
 *   mjson_next_key()     object only: advance to the next member's value
 *   mjson_next_element() array only: advance to the next element value
 *   mjson_skip()         consume the value at the cursor
 *   mjson_leave()        consume the rest of the container and its terminator
 *
 * Every mjson_enter() must be paired with exactly one mjson_leave().
 */

#define MAGPIE_JSON_DEPTH_MAX 16

typedef struct {
    const char *p;
    const char *end;
    unsigned depth;
} mjson_t;

/** Initialise a scanner over [text, text+len). */
void mjson_init(mjson_t *json, const char *text, size_t len);

/**
 * Validate that the document is a well-formed JSON value with nothing but
 * whitespace after it. Does not disturb the cursor.
 */
bool mjson_validate(const mjson_t *json);

/** True when the next value is an object ('{'), false otherwise. */
bool mjson_at_object(const mjson_t *json);

/** Consume the '{' or '[' at the cursor and enter that container. */
bool mjson_enter(mjson_t *json);

/** Consume everything up to and including the container's closing byte. */
bool mjson_leave(mjson_t *json);

/** Consume one value (object, array, string, number, literal). */
bool mjson_skip(mjson_t *json);

/**
 * Advance to the next member value of the current object. Returns false at the
 * object's closing brace or on error. With out_key non-NULL the member name is
 * copied (truncated to key_size) before the value is visited.
 */
bool mjson_next_key(mjson_t *json, char *out_key, size_t key_size);

/** Advance to the next element of the current array. */
bool mjson_next_element(mjson_t *json);

/* Value readers. The cursor holds the value before and after the call. */
bool mjson_read_string(mjson_t *json, char *out, size_t out_size);
bool mjson_read_double(mjson_t *json, double *out);
bool mjson_read_int64(mjson_t *json, int64_t *out);
/** Reads JSON true/false. Strings "true"/"false" and 1/0 also count. */
bool mjson_read_bool(mjson_t *json, bool *out);

/**
 * Lenient numeric read: JSON null, booleans and non-numeric values read as 0
 * and still consume the value, so a caller can walk a whole object without
 * branching on types. Always leaves the cursor after the value.
 */
void mjson_read_number_lenient(mjson_t *json, double *out);
