/*
   SMSDB-level test for the unified SMS history pagination (src/sms_paging.[ch]).

   Reproducible and committed: drives the real production pagination module
   (src/sms_paging.c) against an in-memory SQLite database whose schema matches
   the tables created by smsdb.c (mt_queue = MT SIM->SIP, mt_out = MO SIP->SIM),
   with fully controlled created_at values so ordering assertions are exact.

   Covered:
     - per-SIM isolation (filter by device, empty string == all devices)
     - MT/MO direction tagging and field mapping (mid/from for MT, id/dst for MO)
     - newest-first order by created_at
     - count/page metadata and page slicing (LIMIT/OFFSET), page beyond end
     - empty-but-valid device (no rows) is NOT an error
     - argument validation and real DB errors return -1 (the signal the mini-web
       turns into an HTTP 500 instead of a false successful empty page)
*/

#include <stdio.h>
#include <string.h>

#include <sqlite3.h>

#include "sms_paging.h"

static int ok;
static int faults;

#define CHECK(cond, ...)                                              \
    do {                                                             \
        if (cond) {                                                  \
            ok++;                                                    \
        } else {                                                     \
            faults++;                                                \
            printf("FAIL " __FILE__ ":%d: ", __LINE__);             \
            printf(__VA_ARGS__);                                     \
            printf("\n");                                            \
        }                                                            \
    } while (0)

/*! max rows a test page may collect */
#define MAX_ROWS 64

struct row_collector {
    struct sms_paging_row rows[MAX_ROWS];
    int n;
};

static int collect_row(void* ctx, const struct sms_paging_row* row)
{
    struct row_collector* const c = ctx;

    if (c->n < MAX_ROWS) {
        c->rows[c->n++] = *row;
    }
    return 0;
}

static sqlite3* open_db(void)
{
    sqlite3* db = NULL;

    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        return NULL;
    }
    return db;
}

static void exec_sql(sqlite3* db, const char* sql)
{
    /* fixed literals only, no user input reaches these statements */
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        faults++;
        printf("FAIL test setup: sqlite3_exec failed: %s\n", sql);
    }
}

/*! Create the exact mt_queue/mt_out schema used by smsdb.c (db_init). */
static void create_schema(sqlite3* db)
{
    exec_sql(db, "CREATE TABLE IF NOT EXISTS mt_queue (device TEXT NOT NULL, mid TEXT PRIMARY KEY,"
                 " from_num TEXT NOT NULL, msg TEXT NOT NULL, ts TEXT, state TEXT NOT NULL DEFAULT 'new',"
                 " attempts INTEGER NOT NULL DEFAULT 0, next_retry_at INTEGER NOT NULL DEFAULT 0,"
                 " last_result TEXT, created_at INTEGER NOT NULL DEFAULT (strftime('%s')),"
                 " sent_at INTEGER, delivered_at INTEGER)");
    exec_sql(db, "CREATE TABLE IF NOT EXISTS mt_out (id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 " device TEXT NOT NULL, dst TEXT NOT NULL, msg TEXT NOT NULL, ts TEXT,"
                 " state TEXT NOT NULL DEFAULT 'submitted_to_modem', srr_status INTEGER, mr INTEGER,"
                 " imdn_id TEXT, created_at INTEGER NOT NULL DEFAULT (strftime('%s')), updated_at INTEGER)");
}

static void seed_mt(sqlite3* db, const char* device, const char* mid, const char* from_num, long created_at)
{
    char sql[512];

    snprintf(sql, sizeof(sql),
             "INSERT INTO mt_queue (device, mid, from_num, msg, created_at) VALUES ('%s','%s','%s','msg-%s',%ld)",
             device, mid, from_num, mid, created_at);
    exec_sql(db, sql);
}

static void seed_mo(sqlite3* db, const char* device, const char* dst, long created_at)
{
    char sql[512];

    snprintf(sql, sizeof(sql),
             "INSERT INTO mt_out (device, dst, msg, created_at) VALUES ('%s','%s','msg-%s',%ld)",
             device, dst, dst, created_at);
    exec_sql(db, sql);
}

/*! MT row with the full delivery/retry column set (attempts/next_retry_at/
    last_result/sent_at/delivered_at). */
static void seed_mt_ext(sqlite3* db, const char* device, const char* mid, const char* from_num, long created_at, int attempts, long next_retry_at,
                        const char* last_result, long sent_at, long delivered_at)
{
    char sql[1024];

    snprintf(sql, sizeof(sql),
             "INSERT INTO mt_queue (device, mid, from_num, msg, created_at, state, attempts, next_retry_at, last_result, sent_at, delivered_at) "
             "VALUES ('%s','%s','%s','msg-%s',%ld,'delivered',%d,%ld,'%s',%ld,%ld)",
             device, mid, from_num, mid, created_at, attempts, next_retry_at, last_result, sent_at, delivered_at);
    exec_sql(db, sql);
}

/*! MO row with delivery status columns (srr_status/mr); pass -1 for NULL. */
static void seed_mo_ext(sqlite3* db, const char* device, const char* dst, long created_at, int srr_status, int mr)
{
    char sql[512];

    if (srr_status < 0 && mr < 0) {
        snprintf(sql, sizeof(sql),
                 "INSERT INTO mt_out (device, dst, msg, created_at) VALUES ('%s','%s','msg-%s',%ld)",
                 device, dst, dst, created_at);
    } else if (srr_status < 0) {
        snprintf(sql, sizeof(sql),
                 "INSERT INTO mt_out (device, dst, msg, created_at, mr) VALUES ('%s','%s','msg-%s',%ld,%d)",
                 device, dst, dst, created_at, mr);
    } else if (mr < 0) {
        snprintf(sql, sizeof(sql),
                 "INSERT INTO mt_out (device, dst, msg, created_at, srr_status) VALUES ('%s','%s','msg-%s',%ld,%d)",
                 device, dst, dst, created_at, srr_status);
    } else {
        snprintf(sql, sizeof(sql),
                 "INSERT INTO mt_out (device, dst, msg, created_at, srr_status, mr) VALUES ('%s','%s','msg-%s',%ld,%d,%d)",
                 device, dst, dst, created_at, srr_status, mr);
    }
    exec_sql(db, sql);
}

/*! Key that uniquely identifies one merged row for order/membership checks:
    direction ':' ref_id (mid for MT, "id" text for MO). */
static void row_key(const struct sms_paging_row* row, char* out, size_t len)
{
    snprintf(out, len, "%s:%s", row->direction, row->ref_id);
}

/*! Assert collector holds exactly the expected keys, in order; empty = none. */
static void check_rows(const char* what, const struct row_collector* c, const char* const* keys, int nkeys)
{
    int i;

    CHECK(c->n == nkeys, "%s: got %d rows, expected %d", what, c->n, nkeys);
    for (i = 0; i < c->n && i < nkeys && i < MAX_ROWS; i++) {
        char key[256];

        row_key(&c->rows[i], key, sizeof(key));
        CHECK(strcmp(key, keys[i]) == 0, "%s[%d]: got key \"%s\", expected \"%s\"", what, i, key, keys[i]);
    }
}

static void test_seed_and_basics(void)
{
    sqlite3* const db = open_db();
    struct row_collector c;
    const char* sim1_all[] = { "mt:m5", "mo:2", "mt:m3", "mo:1", "mt:m1_1000" };
    const char* sim2_all[] = { "mt:s2mt3", "mo:3", "mt:s2mt" };
    const char* all8[] = { "mt:m5", "mt:s2mt3", "mo:2", "mo:3", "mt:m3", "mo:1", "mt:s2mt", "mt:m1_1000" };

    create_schema(db);
    seed_mt(db, "sim1", "m1_1000", "+49100", 1000);
    seed_mo(db, "sim1", "+49900", 1100);            /* mt_out id 1 */
    seed_mt(db, "sim1", "m3", "+49120", 1200);
    seed_mo(db, "sim1", "+49930", 1300);            /* mt_out id 2 */
    seed_mt(db, "sim1", "m5", "+49140", 1400);
    seed_mt(db, "sim2", "s2mt", "+49200", 1050);
    seed_mo(db, "sim2", "+49925", 1250);            /* mt_out id 3 */
    seed_mt(db, "sim2", "s2mt3", "+49230", 1350);

    /* counts -------------------------------------------------------------- */
    {
        int total = -1;
        CHECK(sms_paging_count(db, "sim1", &total) == 0 && total == 5, "count(sim1)=%d, expected 5", total);
        CHECK(sms_paging_count(db, "sim2", &total) == 0 && total == 3, "count(sim2)=%d, expected 3", total);
        CHECK(sms_paging_count(db, NULL, &total) == 0 && total == 8, "count(all)=%d, expected 8", total);
        CHECK(sms_paging_count(db, "", &total) == 0 && total == 8, "count(\"\")=%d, expected 8 (empty == all)", total);
    }

    /* whole-device pages, newest-first ------------------------------------ */
    memset(&c, 0, sizeof(c));
    CHECK(sms_paging_page(db, "sim1", 10, 0, &c, collect_row) == 0, "page(sim1) rc");
    check_rows("page sim1 (all 5)", &c, sim1_all, 5);

    memset(&c, 0, sizeof(c));
    CHECK(sms_paging_page(db, "sim2", 10, 0, &c, collect_row) == 0, "page(sim2) rc");
    check_rows("page sim2 (all 3)", &c, sim2_all, 3);

    memset(&c, 0, sizeof(c));
    CHECK(sms_paging_page(db, NULL, 10, 0, &c, collect_row) == 0, "page(all) rc");
    check_rows("page all (8, newest first)", &c, all8, 8);

    /* field mapping: MT carries mid/from, MO carries id/dst ----------------- */
    {
        struct sms_paging_row first;

        memset(&c, 0, sizeof(c));
        sms_paging_page(db, "sim1", 10, 0, &c, collect_row);
        CHECK(c.n == 5, "page sim1 count for mapping");
        first = c.rows[1]; /* the MO row "mo:2" (created_at 1300) */
        CHECK(strcmp(first.direction, "mo") == 0 && strcmp(first.device, "sim1") == 0, "MO direction/device");
        CHECK(strcmp(first.ref_id, "2") == 0 && strcmp(first.peer, "+49930") == 0, "MO ref_id=id, peer=dst");
        CHECK(first.id == 2, "MO id=%ld, expected 2", first.id);
        CHECK(strcmp(first.state, "submitted_to_modem") == 0, "MO state default");

        first = c.rows[0]; /* MT row "mt:m5" (created_at 1400) */
        CHECK(strcmp(first.direction, "mt") == 0 && strcmp(first.device, "sim1") == 0, "MT direction/device");
        CHECK(strcmp(first.ref_id, "m5") == 0 && strcmp(first.peer, "+49140") == 0, "MT ref_id=mid, peer=from_num");
        CHECK(strcmp(first.state, "new") == 0, "MT state default");
    }

    /* pagination metadata: page_size / offset slices ------------------------ */
    {
        const char* p1[] = { "mt:m5", "mo:2", "mt:m3" };
        const char* p2[] = { "mo:1", "mt:m1_1000" };

        memset(&c, 0, sizeof(c));
        CHECK(sms_paging_page(db, "sim1", 3, 0, &c, collect_row) == 0, "page(sim1,size3,off0) rc");
        check_rows("page sim1 size=3 off=0", &c, p1, 3);

        memset(&c, 0, sizeof(c));
        CHECK(sms_paging_page(db, "sim1", 3, 3, &c, collect_row) == 0, "page(sim1,size3,off3) rc");
        check_rows("page sim1 size=3 off=3", &c, p2, 2);

        memset(&c, 0, sizeof(c));
        CHECK(sms_paging_page(db, "sim1", 3, 6, &c, collect_row) == 0, "page(sim1,size3,off6) rc");
        check_rows("page sim1 beyond end (empty)", &c, NULL, 0);

        memset(&c, 0, sizeof(c));
        CHECK(sms_paging_page(db, NULL, 4, 0, &c, collect_row) == 0, "page(all,size4,off0) rc");
        CHECK(c.n == 4, "page all size=4 cols 4, got %d", c.n);
        CHECK(strcmp(c.rows[3].device, "sim2") == 0, "page all 4th row is s2mt (sim2)");
    }

    sqlite3_close(db);
}

static void test_extended_columns(void)
{
    sqlite3* const db = open_db();
    struct row_collector c;
    const char* sim1_all[] = { "mt:me5", "mo:2", "mo:1", "mt:me4", "mt:me3" };

    create_schema(db);
    /* MT with full delivery/retry columns */
    seed_mt_ext(db, "sim1", "me3", "+49130", 1310, 1, 1400, "202 accepted (queued)", 1320, 1330);
    /* MT that was never retried (defaults: attempts 0, next_retry_at 0) */
    seed_mt(db, "sim1", "me4", "+49130", 1410);
    /* MT with no report fields (sent/delivered 0, empty last_result) */
    seed_mt_ext(db, "sim1", "me5", "+49130", 1500, 2, 0, "", 0, 0);
    /* MO with srr_status + mr */
    seed_mo_ext(db, "sim1", "+49900", 1420, 0, 0x34);
    /* MO with no status report yet (srr_status/mr NULL -> -1 sentinels) */
    seed_mo(db, "sim1", "+49901", 1430);

    memset(&c, 0, sizeof(c));
    CHECK(sms_paging_page(db, "sim1", 10, 0, &c, collect_row) == 0, "ext page rc");
    check_rows("page ext (newest first)", &c, sim1_all, 5);

    /* MO row with a real status report: own columns filled, MT columns are
       the -1 sentinels so the renderer shows an em-dash. */
    {
        const struct sms_paging_row* row = NULL;
        for (int i = 0; i < c.n; i++) {
            if (strcmp(c.rows[i].direction, "mo") == 0 && strcmp(c.rows[i].ref_id, "1") == 0) {
                row = &c.rows[i];
            }
        }
        CHECK(row != NULL, "ext: MO row found");
        if (row) {
            CHECK(row->srr_status == 0 && row->mr == 0x34, "ext: MO srr_status=%ld mr=%ld, expected 0 / 0x34", row->srr_status, row->mr);
            CHECK(row->attempts == -1 && row->next_retry_at == -1 && row->sent_at == -1 && row->delivered_at == -1,
                  "ext: MO MT-column sentinels (%ld/%ld/%ld/%ld)", row->attempts, row->next_retry_at, row->sent_at, row->delivered_at);
            CHECK(strlen(row->last_result) == 0, "ext: MO last_result empty");
        }
    }

    /* MO row with no status report: srr_status/mr fall back to the -1 sentinel. */
    {
        const struct sms_paging_row* row = NULL;
        for (int i = 0; i < c.n; i++) {
            if (strcmp(c.rows[i].direction, "mo") == 0 && strcmp(c.rows[i].ref_id, "2") == 0) {
                row = &c.rows[i];
            }
        }
        CHECK(row != NULL, "ext: MO(no report) row found");
        if (row) {
            CHECK(row->srr_status == -1 && row->mr == -1, "ext: MO(no report) srr_status=%ld mr=%ld, expected -1", row->srr_status, row->mr);
        }
    }

    /* MT rows carry their own columns; the MO-only columns are -1. */
    {
        const struct sms_paging_row* row = NULL;
        for (int i = 0; i < c.n; i++) {
            if (strcmp(c.rows[i].direction, "mt") == 0 && strcmp(c.rows[i].ref_id, "me3") == 0) {
                row = &c.rows[i];
            }
        }
        CHECK(row != NULL, "ext: MT row found");
        if (row) {
            CHECK(row->attempts == 1 && row->last_result && strcmp(row->last_result, "202 accepted (queued)") == 0,
                  "ext: MT attempts/last_result");
            CHECK(row->next_retry_at == 1400 && row->sent_at == 1320 && row->delivered_at == 1330,
                  "ext: MT retry/sent/delivered (%ld/%ld/%ld)", row->next_retry_at, row->sent_at, row->delivered_at);
            CHECK(row->srr_status == -1 && row->mr == -1, "ext: MT MO-column sentinels (%ld/%ld)", row->srr_status, row->mr);
        }
    }

    /* MT with defaults: attempts 0, next_retry_at 0, empty last_result. */
    {
        const struct sms_paging_row* row = NULL;
        for (int i = 0; i < c.n; i++) {
            if (strcmp(c.rows[i].direction, "mt") == 0 && strcmp(c.rows[i].ref_id, "me4") == 0) {
                row = &c.rows[i];
            }
        }
        CHECK(row != NULL, "ext: MT(defaults) row found");
        if (row) {
            CHECK(row->attempts == 0 && row->next_retry_at == 0 && row->sent_at == 0 && row->delivered_at == 0 && strlen(row->last_result) == 0,
                  "ext: MT(defaults) attempts=%ld next=%ld last='%s'", row->attempts, row->next_retry_at, row->last_result);
        }
    }

    sqlite3_close(db);
}

static void test_empty_and_errors(void)
{
    sqlite3* const db = open_db();
    struct row_collector c;
    int total = -1;

    create_schema(db);

    /* a device with no rows is a legit empty result (not an error) ---------- */
    CHECK(sms_paging_count(db, "empty", &total) == 0 && total == 0, "count(empty)=%d, expected 0 success", total);
    memset(&c, 0, sizeof(c));
    CHECK(sms_paging_page(db, "empty", 10, 0, &c, collect_row) == 0, "page(empty) rc success");
    CHECK(c.n == 0, "page(empty) 0 rows, got %d", c.n);

    /* argument validation returns -1 ---------------------------------------- */
    CHECK(sms_paging_count(db, "empty", NULL) == -1, "count with NULL total -> -1");
    CHECK(sms_paging_count(NULL, "empty", &total) == -1, "count with NULL db -> -1");
    CHECK(sms_paging_page(db, NULL, 0, 0, &c, collect_row) == -1, "page with page_size 0 -> -1");
    CHECK(sms_paging_page(db, NULL, 10, -1, &c, collect_row) == -1, "page with offset -1 -> -1");
    CHECK(sms_paging_page(db, NULL, 10, 0, &c, NULL) == -1, "page with NULL cb -> -1");
    CHECK(sms_paging_page(NULL, NULL, 10, 0, &c, collect_row) == -1, "page with NULL db -> -1");

    /* a real DB error (missing table) surfaces as -1, never a phantom success - */
    {
        sqlite3* const broken = open_db(); /* no schema created */

        CHECK(sms_paging_count(broken, "sim1", &total) == -1, "count on missing table -> -1");
        memset(&c, 0, sizeof(c));
        CHECK(sms_paging_page(broken, "sim1", 10, 0, &c, collect_row) == -1, "page on missing table -> -1");
        sqlite3_close(broken);
    }

    sqlite3_close(db);
}

int main(void)
{
    test_seed_and_basics();
    test_extended_columns();
    test_empty_and_errors();

    printf("\n%s: done %d tests: %d OK %d FAILS\n", "sms_paging_test", ok + faults, ok, faults);
    return faults ? 1 : 0;
}