// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Presentation helpers shared by the device UI and the host tests.
 *
 * Portable C with no ESP-IDF dependency, so the exact strings the device shows
 * can be asserted in tests instead of being validated only by eye on hardware.
 * All writers always NUL-terminate and never write past `size`.
 */

/** "84%" / "3.7%" (one decimal under 10%) / "--" for unknown. */
void magpie_format_percent(float percent, bool known, char *out, size_t size);

/** Compact token counts: "512", "12.3K", "1.25M", "3.40B". */
void magpie_format_count(uint64_t value, char *out, size_t size);

/** "2d 8h", "3h 05m", "42m", "now", "--" for unknown. */
void magpie_format_duration(int32_t seconds, bool known, char *out, size_t size);

/** "$1.23" / "$0.004" / "$12.34" from micro-dollars. */
void magpie_format_cost(uint64_t micros, char *out, size_t size);

/** "12s ago" / "4m ago" / "2h ago" / "never". */
void magpie_format_age(uint32_t seconds, bool known, char *out, size_t size);

/** "333ms" / "8.3s" / "2m 05s". */
void magpie_format_latency(uint32_t milliseconds, char *out, size_t size);

/** Shorten a provider id for the device: "office/claude" -> "claude". */
void magpie_short_provider(const char *provider, char *out, size_t size);

/** Uppercase ASCII in place-safe form, used for small status labels. */
void magpie_upper(const char *input, char *out, size_t size);
