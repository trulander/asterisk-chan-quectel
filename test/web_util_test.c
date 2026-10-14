/*
 * web_util_test.c
 *
 * Standalone unit test for the pure mini-web helpers (src/web_util.c):
 * RSSI bar thresholding, strict integer parsing, page clamping/pagination
 * math and query-parameter extraction.
 *
 * Build & run (no Asterisk dependencies):
 *   gcc -Wall -Wextra -I src test/web_util_test.c src/web_util.c -o /tmp/web_util_test
 *   /tmp/web_util_test
 *
 * Prints one line per check; exits non-zero when any check fails.
 */

#include <stdio.h>
#include <string.h>

#include "web_util.h"

static int ok = 0;
static int faults = 0;

static void check_bars(int rssi, int expected)
{
    const int got = web_util_rssi_bars(rssi);
    if (got == expected) {
        ok++;
        printf("web_util_rssi_bars(%d) = %d  OK\n", rssi, got);
    } else {
        faults++;
        printf("web_util_rssi_bars(%d) = %d (expected %d)  FAIL\n", rssi, got, expected);
    }
}

static void test_rssi_bars(void)
{
    /* unknown / no data -> 0 bars */
    check_bars(-5, 0);
    check_bars(0, 0);
    check_bars(99, 0);
    check_bars(32, 0);
    check_bars(100, 0);
    check_bars(255, 0);
    /* 1-9 -> 1 */
    check_bars(1, 1);
    check_bars(8, 1);
    check_bars(9, 1);
    /* 10-14 -> 2 */
    check_bars(10, 2);
    check_bars(12, 2);
    check_bars(14, 2);
    /* 15-19 -> 3 */
    check_bars(15, 3);
    check_bars(17, 3);
    check_bars(19, 3);
    /* 20-31 -> 4 */
    check_bars(20, 4);
    check_bars(25, 4);
    check_bars(31, 4);
}

static void check_parse(const char* in, int expected_rc, int expected_val)
{
    int val = -12345;
    const int rc = web_util_parse_int(in, &val);
    if (rc == expected_rc && (rc != 0 || val == expected_val)) {
        ok++;
        printf("web_util_parse_int(\"%s\") rc=%d val=%d  OK\n", in, rc, val);
    } else {
        faults++;
        printf("web_util_parse_int(\"%s\") rc=%d val=%d (expected rc=%d val=%d)  FAIL\n", in, rc, val, expected_rc, expected_val);
    }
}

static void test_parse_int(void)
{
    check_parse("1", 0, 1);
    check_parse("0", 0, 0);
    check_parse("42", 0, 42);
    check_parse("2147483647", 0, 2147483647);
    check_parse("-3", 0, -3);
    /* invalid input */
    check_parse("", -1, 0);
    check_parse("abc", -1, 0);
    check_parse("12x", -1, 0);
    check_parse(" 2", -1, 0);
    check_parse("2 ", -1, 0);
    check_parse("2147483648", -1, 0); /* overflow */
    check_parse("-2147483649", -1, 0);
}

static void check_clamp(int page, int max_page, int expected)
{
    const int got = web_util_clamp_page(page, max_page);
    if (got == expected) {
        ok++;
        printf("web_util_clamp_page(%d, %d) = %d  OK\n", page, max_page, got);
    } else {
        faults++;
        printf("web_util_clamp_page(%d, %d) = %d (expected %d)  FAIL\n", page, max_page, got, expected);
    }
}

static void test_clamp_page(void)
{
    check_clamp(1, 5, 1);
    check_clamp(3, 5, 3);
    check_clamp(5, 5, 5);
    check_clamp(6, 5, 5);
    check_clamp(0, 5, 1);
    check_clamp(-2, 5, 1);
    check_clamp(10, 1, 1);
    check_clamp(4, 0, 1); /* degenerate max_page -> 1 */
}

static void check_page_meta(int total, int page_size, int in_page, int exp_page, int exp_pages, int exp_offset)
{
    int page = in_page;
    int pages = 0;
    int offset = -1;
    const int rc = web_util_page_meta(total, page_size, &page, &pages, &offset);
    if (rc == 0 && page == exp_page && pages == exp_pages && offset == exp_offset) {
        ok++;
        printf("web_util_page_meta(total=%d, size=%d, page=%d) -> page=%d pages=%d offset=%d  OK\n", total, page_size, in_page, page, pages,
               offset);
    } else {
        faults++;
        printf("web_util_page_meta(total=%d, size=%d, page=%d) -> rc=%d page=%d pages=%d offset=%d (expected page=%d pages=%d offset=%d)  FAIL\n",
               total, page_size, in_page, rc, page, pages, offset, exp_page, exp_pages, exp_offset);
    }
}

static void test_page_meta(void)
{
    check_page_meta(25, 10, 1, 1, 3, 0);
    check_page_meta(25, 10, 2, 2, 3, 10);
    check_page_meta(25, 10, 3, 3, 3, 20);
    check_page_meta(25, 10, 4, 3, 3, 20);  /* clamped to last page */
    check_page_meta(25, 10, 0, 1, 3, 0);   /* underflow -> first page */
    check_page_meta(10, 10, 2, 1, 1, 0);   /* exactly one page */
    check_page_meta(0, 10, 5, 1, 1, 0);    /* empty -> one empty page */
    check_page_meta(1, 10, 1, 1, 1, 0);
}

static void check_query_param(const char* query, const char* name, int expected_rc, const char* expected_val)
{
    char out[32];
    const int rc = web_util_query_param(query, name, out, sizeof(out));
    if (rc == expected_rc && (rc != 0 || strcmp(out, expected_val) == 0)) {
        ok++;
        printf("web_util_query_param(\"%s\", \"%s\") rc=%d val=\"%s\"  OK\n", query ? query : "", name, rc, out);
    } else {
        faults++;
        printf("web_util_query_param(\"%s\", \"%s\") rc=%d val=\"%s\" (expected rc=%d val=\"%s\")  FAIL\n", query ? query : "", name, rc, out,
               expected_rc, expected_val ? expected_val : "");
    }
}

static void test_query_param(void)
{
    check_query_param("page=2", "page", 0, "2");
    check_query_param("?page=3", "page", 0, "3");
    check_query_param("a=1&page=4", "page", 0, "4");
    check_query_param("page=2&x=zz", "page", 0, "2");
    check_query_param("page=", "page", 0, "");
    check_query_param("page=10", "nopage", -1, "");
    check_query_param("nopage=1", "page", -1, "");
    check_query_param("", "page", -1, "");
    check_query_param(NULL, "page", -1, "");
    check_query_param("page=22&x", "page", 0, "22");
}

int main(void)
{
    test_rssi_bars();
    test_parse_int();
    test_clamp_page();
    test_page_meta();
    test_query_param();

    printf("\ndone %d tests: %d OK %d FAILS\n", ok + faults, ok, faults);
    return faults ? 1 : 0;
}