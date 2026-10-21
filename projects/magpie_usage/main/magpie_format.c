// SPDX-License-Identifier: Apache-2.0

#include "magpie_format.h"

#include <stdio.h>
#include <string.h>

#define SECONDS_PER_MINUTE 60
#define SECONDS_PER_HOUR 3600
#define SECONDS_PER_DAY 86400
#define MINUTES_PER_HOUR 60
#define HOURS_PER_DAY 24

static void write_text(char *out, size_t size, const char *text)
{
    if (!out || size == 0) {
        return;
    }
    size_t i = 0;
    for (; i + 1 < size && text[i]; ++i) {
        out[i] = text[i];
    }
    out[i] = '\0';
}

void magpie_format_percent(float percent, bool known, char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    if (!known) {
        write_text(out, size, "--");
        return;
    }
    if (percent < 0.0f) {
        percent = 0.0f;
    }
    if (percent > 100.0f) {
        percent = 100.0f;
    }
    /* Integer percent is readable at desk distance; keep one decimal only for
     * the low single digits where 1% granularity is misleading. */
    if (percent < 10.0f) {
        snprintf(out, size, "%.1f%%", (double)percent);
    } else {
        snprintf(out, size, "%.0f%%", (double)percent);
    }
}

void magpie_format_count(uint64_t value, char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    if (value < 1000ULL) {
        snprintf(out, size, "%llu", (unsigned long long)value);
    } else if (value < 1000000ULL) {
        const unsigned long long tenths = value / 100ULL;
        snprintf(out, size, "%llu.%lluK", (unsigned long long)(tenths / 10),
                 (unsigned long long)(tenths % 10));
    } else if (value < 1000000000ULL) {
        const unsigned long long hundredths = value / 10000ULL;
        snprintf(out, size, "%llu.%02lluM",
                 (unsigned long long)(hundredths / 100),
                 (unsigned long long)(hundredths % 100));
    } else {
        const unsigned long long hundredths = value / 10000000ULL;
        snprintf(out, size, "%llu.%02lluB",
                 (unsigned long long)(hundredths / 100),
                 (unsigned long long)(hundredths % 100));
    }
}

void magpie_format_duration(int32_t seconds, bool known, char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    if (!known) {
        write_text(out, size, "--");
        return;
    }
    if (seconds <= 0) {
        write_text(out, size, "now");
        return;
    }
    if (seconds >= SECONDS_PER_DAY) {
        const int32_t days = seconds / SECONDS_PER_DAY;
        const int32_t hours = (seconds % SECONDS_PER_DAY) / SECONDS_PER_HOUR;
        snprintf(out, size, "%dd %dh", (int)days, (int)hours);
        return;
    }
    if (seconds >= SECONDS_PER_HOUR) {
        const int32_t hours = seconds / SECONDS_PER_HOUR;
        const int32_t minutes = (seconds % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE;
        snprintf(out, size, "%dh %02dm", (int)hours, (int)minutes);
        return;
    }
    const int32_t minutes = seconds / SECONDS_PER_MINUTE;
    if (minutes < 1) {
        write_text(out, size, "<1m");
        return;
    }
    snprintf(out, size, "%dm", (int)minutes);
}

void magpie_format_cost(uint64_t micros, char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    if (micros == 0) {
        write_text(out, size, "$0");
    } else if (micros < 10000ULL) {
        /* Below one cent, show three decimals instead of rounding to $0.00. */
        snprintf(out, size, "$%llu.%03llu",
                 (unsigned long long)(micros / 1000000ULL),
                 (unsigned long long)((micros / 1000ULL) % 1000ULL));
    } else {
        const unsigned long long cents = micros / 10000ULL;
        snprintf(out, size, "$%llu.%02llu", cents / 100ULL, cents % 100ULL);
    }
}

void magpie_format_age(uint32_t seconds, bool known, char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    if (!known) {
        write_text(out, size, "never");
        return;
    }
    if (seconds < 5) {
        write_text(out, size, "just now");
    } else if (seconds < SECONDS_PER_MINUTE) {
        snprintf(out, size, "%us ago", (unsigned)seconds);
    } else if (seconds < SECONDS_PER_HOUR) {
        snprintf(out, size, "%um ago", (unsigned)(seconds / SECONDS_PER_MINUTE));
    } else if (seconds < SECONDS_PER_DAY) {
        snprintf(out, size, "%uh ago", (unsigned)(seconds / SECONDS_PER_HOUR));
    } else {
        snprintf(out, size, "%ud ago", (unsigned)(seconds / SECONDS_PER_DAY));
    }
}

void magpie_format_latency(uint32_t milliseconds, char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    if (milliseconds < 1000) {
        snprintf(out, size, "%ums", (unsigned)milliseconds);
    } else if (milliseconds < 60000) {
        snprintf(out, size, "%.1fs", (double)milliseconds / 1000.0);
    } else {
        snprintf(out, size, "%um %02us",
                 (unsigned)(milliseconds / 60000),
                 (unsigned)((milliseconds / 1000) % 60));
    }
}

void magpie_short_provider(const char *provider, char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    if (!provider) {
        out[0] = '\0';
        return;
    }
    const char *slash = strrchr(provider, '/');
    write_text(out, size, slash ? slash + 1 : provider);
}

void magpie_upper(const char *input, char *out, size_t size)
{
    if (!out || size == 0) {
        return;
    }
    if (!input) {
        out[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; i + 1 < size && input[i]; ++i) {
        const char c = input[i];
        out[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    out[i] = '\0';
}
