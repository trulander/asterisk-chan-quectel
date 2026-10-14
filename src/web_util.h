/*
 * web_util.h
 *
 * Pure helper functions for the read-only mini HTTP page (RSSI bar scale,
 * page/int/query parsing and pagination math). No Asterisk or driver
 * dependencies, so the standalone test harness (test/web_util_test.c) can
 * compile this module directly.
 */

#ifndef CHAN_QUECTEL_WEB_UTIL_H_INCLUDED
#define CHAN_QUECTEL_WEB_UTIL_H_INCLUDED

#include <stddef.h>

/*!
 * \brief Map a GSM/UMTS RSSI index (0..31, as reported by AT+CSQ/^RSSI) to a
 * 0..4 "accessible bars" scale.
 * \param rssi raw RSSI index (0..31, 99 = unknown, negative = not measured)
 * \return number of bars: 0 for unknown/99/0 and any value outside 1..31,
 *         1 for 1-9, 2 for 10-14, 3 for 15-19, 4 for 20-31.
 */
int web_util_rssi_bars(int rssi);

/*!
 * \brief Parse a NUL-terminated base-10 integer (strict: whole string must
 * be a valid number, no trailing garbage).
 * \param s input string
 * \param out receives the parsed value on success
 * \return 0 on success, -1 on empty/invalid/overflow input
 */
int web_util_parse_int(const char* s, int* out);

/*!
 * \brief Clamp a 1-based page number into [1, max_page].
 * \param page requested page (may be < 1 or > max_page)
 * \param max_page last valid page (>= 1)
 * \return clamped page
 */
int web_util_clamp_page(int page, int max_page);

/*!
 * \brief Compute validated pagination metadata for a result set.
 * \param total total number of items (>= 0)
 * \param page_size items per page (> 0, e.g. 10)
 * \param page in/out: requested 1-based page, clamped into [1, pages] on return
 * \param pages out: total number of pages (>= 1)
 * \param offset out: 0-based row offset for the selected page
 * \return 0 on success, -1 on invalid arguments
 */
int web_util_page_meta(int total, int page_size, int* page, int* pages, int* offset);

/*!
 * \brief Extract the value of the first query parameter named \a name from a
 * query string ("a=b&c=d", with or without a leading '?').
 * \param query query string (may be NULL/empty)
 * \param name parameter name to look for
 * \param out receives the value (NUL-terminated) when found
 * \param out_len size of \a out
 * \return 0 and fills \a out when found, -1 otherwise
 */
int web_util_query_param(const char* query, const char* name, char* out, size_t out_len);

#endif /* CHAN_QUECTEL_WEB_UTIL_H_INCLUDED */