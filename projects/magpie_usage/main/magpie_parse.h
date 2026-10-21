// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "magpie_model.h"

/*
 * Magpie response parsers.
 *
 * Pure C, no ESP-IDF and no I/O: each function turns one Magpie JSON document
 * into the portable model. The device feeds them HTTP bodies; the host tests
 * feed them fixtures captured from a real gateway (tests/host/fixtures).
 *
 * Return values are esp_err_t-compatible but declared as plain int so the host
 * build needs no ESP-IDF headers.
 */

#define MAGPIE_OK 0
#define MAGPIE_ERR_INVALID_ARG 0x102
#define MAGPIE_ERR_INVALID_RESPONSE 0x10c

/**
 * Parse "/v1/magpie/quotas".
 *
 * @param body            JSON document
 * @param len             length in bytes
 * @param now_epoch       current Unix time, used to derive reset countdowns
 * @param provider_filter optional comma-separated prefix list (e.g. "office/");
 *                        NULL or "" keeps every provider
 * @param out             snapshot receiving providers and the worst window
 *
 * Providers that report an error (signed out) are kept so the UI can show them.
 */
int magpie_parse_quotas(const char *body, size_t len, int64_t now_epoch,
                        const char *provider_filter, magpie_snapshot_t *out);

/** Parse "/v1/magpie/usage" into out->usage. */
int magpie_parse_usage(const char *body, size_t len, magpie_snapshot_t *out);

/** Parse "/v1/magpie/usage/requests?limit=N" into out->latest. */
int magpie_parse_requests(const char *body, size_t len, magpie_snapshot_t *out);

/** Parse the "/" health document into out->gateway_version. */
int magpie_parse_health(const char *body, size_t len, magpie_snapshot_t *out);

/** Reset a snapshot to its empty state before a refresh cycle. */
void magpie_snapshot_clear(magpie_snapshot_t *snapshot);

/** Count providers that report an error, for the status line. */
uint8_t magpie_snapshot_error_count(const magpie_snapshot_t *snapshot);

/**
 * Parse an ISO-8601 timestamp ("2026-10-21T12:29:00+08:00", "...Z", with
 * optional fractional seconds) into Unix seconds. Returns false when the text
 * is not a recognisable timestamp.
 */
bool magpie_parse_iso8601(const char *text, int64_t *out_epoch);

/**
 * Days in a civil date (Howard Hinnant's algorithm), exposed for the test of
 * magpie_parse_iso8601.
 */
int64_t magpie_days_from_civil(int year, unsigned month, unsigned day);
