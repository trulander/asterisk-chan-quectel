/*
 * web_util.c
 *
 * Pure helper functions for the read-only mini HTTP page. Kept free of
 * Asterisk/driver dependencies so test/web_util_test.c can build it directly.
 */

#include <errno.h>  /* errno, ERANGE */
#include <limits.h> /* INT_MIN, INT_MAX */
#include <stdlib.h> /* strtol */
#include <string.h> /* strlen, strncpy */

#include "web_util.h"

int web_util_rssi_bars(int rssi)
{
    /* 0, 99, negative and anything outside the valid 1..31 range -> no data */
    if (rssi < 1 || rssi > 31) {
        return 0;
    }
    if (rssi >= 20) {
        return 4;
    }
    if (rssi >= 15) {
        return 3;
    }
    if (rssi >= 10) {
        return 2;
    }
    return 1;
}

int web_util_parse_int(const char* s, int* out)
{
    const char* p;
    char* end = NULL;
    long v;

    if (!s || !*s || !out) {
        return -1;
    }

    /* reject leading whitespace/junk: allow an optional sign followed by digits */
    p = s;
    if (*p == '+' || *p == '-') {
        p++;
    }
    if (!*p || *p < '0' || *p > '9') {
        return -1;
    }

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno == ERANGE || end == s || *end != '\0') {
        return -1;
    }
    if (v < INT_MIN || v > INT_MAX) {
        return -1;
    }

    *out = (int)v;
    return 0;
}

int web_util_clamp_page(int page, int max_page)
{
    if (max_page < 1) {
        max_page = 1;
    }
    if (page < 1) {
        return 1;
    }
    if (page > max_page) {
        return max_page;
    }
    return page;
}

int web_util_page_meta(int total, int page_size, int* page, int* pages, int* offset)
{
    int p;
    int n;

    if (total < 0 || page_size <= 0 || !page || !pages || !offset) {
        return -1;
    }

    n = total > 0 ? (total + page_size - 1) / page_size : 1;
    p = web_util_clamp_page(*page, n);

    *page   = p;
    *pages  = n;
    *offset = (p - 1) * page_size;
    return 0;
}

int web_util_query_param(const char* query, const char* name, char* out, size_t out_len)
{
    const char* p;
    size_t name_len;
    size_t value_len;

    if (!name || !*name || !out || out_len == 0) {
        return -1;
    }

    out[0] = '\0';

    if (!query) {
        return -1;
    }
    /* a leading '?' is part of a query string, skip it */
    if (*query == '?') {
        query++;
    }

    name_len = strlen(name);
    for (p = query; *p;) {
        const char* const amp = strchr(p, '&');
        const char* const end = amp ? amp : p + strlen(p);
        const char* const eq  = memchr(p, '=', (size_t)(end - p));

        if (eq && (size_t)(eq - p) == name_len && strncmp(p, name, name_len) == 0) {
            value_len = (size_t)(end - (eq + 1));
            if (value_len >= out_len) {
                value_len = out_len - 1;
            }
            memcpy(out, eq + 1, value_len);
            out[value_len] = '\0';
            return 0;
        }

        if (!amp) {
            break;
        }
        p = amp + 1;
    }

    return -1;
}