/*
 * Unit test for the dependency-free SQLite transaction primitives
 * (src/sms_tx.[ch]).
 *
 * The chan_quectel SMS layer keeps a single sqlite3 connection (the module
 * global `smsdb`) shared by every thread. A transaction on that connection is
 * only safe when the recursive per-connection mutex (sqlite3_db_mutex) is held
 * across the whole BEGIN..COMMIT window; otherwise concurrent operations
 * interleave and a second BEGIN fails with
 * "cannot start a transaction within a transaction" (the nested-BEGIN bug that
 * shows up as pairs of different thread-ids in messages).
 *
 * The fix is the SMS_TX_SCOPE macro: it acquires the per-connection mutex
 * BEFORE BEGIN and releases it AFTER COMMIT (rolling back on a failed COMMIT),
 * so the whole window is serialized. This test drives the production source
 * (src/sms_tx.c) against a real SQLite database and maps SMS_TX_LOG to printf.
 * It returns a non-zero exit code on any fault so CTest catches it.
 *
 * Covered:
 *   - concurrent transactions: several pthreads on ONE connection each wrap
 *     their work in SMS_TX_SCOPE -> never a nested-BEGIN (the macro holds the
 *     mutex across the window; without it the raw begin/commit races).
 *   - failed COMMIT -> ROLLBACK, connection not half-open, next BEGIN ok (defect B)
 *   - failed BEGIN -> no half-open state left behind
 *   - writer contention / SQLITE_BUSY -> no leak (autocommit restored)
 *   - COMMIT error (deferred FK) -> no leak (autocommit restored)
 *   - multipart out-of-order + duplicate seqorder -> count never exceeds parts
 *   - SRR unknown -> sms_tx_column_int_or_null returns -1 for NULL (not 0)
 *   - 4-thread stress (sim1 AT, sim2 AT, web, retry) -> 0 nested-BEGIN
 *   - forced failed BEGIN through SMS_TX_SCOPE -> autocommit, next begin ok
 *   - forced failed COMMIT through SMS_TX_SCOPE (deferred FK) -> ROLLBACK,
 *     write rolled back, autocommit, next begin ok
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

/* Map the transaction logging hook to printf for the unit test. Production
 * (smsdb.c) maps it to ast_log instead. */
#define SMS_TX_LOG(fmt, ...)                    \
    do {                                        \
        printf("  [tx] " fmt, __VA_ARGS__);     \
    } while (0)

#include "sms_tx.h"

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

/* -------------------------------------------------------------------------
 * helpers
 * ------------------------------------------------------------------------ */

static sqlite3* open_db(void)
{
    sqlite3* db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
        return NULL;
    }
    return db;
}

static void create_schema(sqlite3* db)
{
    /* mt_out-like table: id, addr, mr, srr_status (nullable = unknown), ... */
    const char* sql =
        "CREATE TABLE mt_out ("
        "  id TEXT,"
        "  addr TEXT,"
        "  mr INTEGER,"
        "  srr_status INTEGER,"
        "  created_at INTEGER"
        ");";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        faults++;
        printf("FAIL test setup: CREATE TABLE failed: %s\n", sqlite3_errmsg(db));
    }
}

/* Table used for the multipart emulation: a UNIQUE(id, seqorder) makes a
 * duplicate seqorder detectable, and the count of inserted rows must never
 * exceed the number of distinct parts. */
static void create_parts_table(sqlite3* db)
{
    const char* sql =
        "CREATE TABLE parts ("
        "  id TEXT,"
        "  ref INTEGER,"
        "  seqorder INTEGER,"
        "  UNIQUE(id, seqorder)"
        ");";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        faults++;
        printf("FAIL test setup: CREATE parts failed: %s\n", sqlite3_errmsg(db));
    }
}

/* Tables exercising a COMMIT-time foreign-key violation: `child.x` references
 * parent(id). FK enforcement is OFF by default, so it is enabled, and every FK
 * constraint is deferred to COMMIT time via PRAGMA defer_foreign_keys=ON (this
 * pragma must be applied AFTER the schema exists to take effect; SQLite does not
 * accept an inline DEFERRED keyword in the FK clause). Then BEGIN + an INSERT
 * with no matching parent succeeds, and COMMIT fails with
 * SQLITE_CONSTRAINT_FOREIGNKEY. This deterministically exercises the
 * failed-COMMIT path (unlike a real SQLITE_BUSY, which releases locks on COMMIT
 * and cannot be forced deterministically in a single process). */
static void create_deferred_fk_db(sqlite3* db)
{
    const char* sql =
        "CREATE TABLE parent(id INTEGER PRIMARY KEY);"
        "CREATE TABLE child(x INTEGER, FOREIGN KEY(x) REFERENCES parent(id));";
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) {
        faults++;
        printf("FAIL test setup: CREATE fk tables failed: %s\n", sqlite3_errmsg(db));
        return;
    }
    /* Enable FK enforcement (OFF by default) and defer all FK checks to COMMIT. */
    sqlite3_exec(db, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA defer_foreign_keys=ON", NULL, NULL, NULL);
}

/* -------------------------------------------------------------------------
 * case 1: concurrent transactions on ONE connection -> no nested-BEGIN
 * ------------------------------------------------------------------------ */

/* Each part/work unit runs inside SMS_TX_SCOPE, which holds the per-connection
 * mutex across the whole BEGIN..COMMIT window. Without that mutex the raw
 * begin/commit would race and produce nested-BEGINs. */
static int run_scope(sqlite3* db, const char* tag)
{
    SMS_TX_SCOPE(t, db, tag);
    sqlite3_exec(db, "INSERT INTO mt_out(id, addr) VALUES('c','x')", NULL, NULL, NULL);
    return 0;
}

struct concurrent_ctx {
    sqlite3* db;
    int iters;
    long nested_begins; /* incremented whenever a scope's BEGIN returned non-OK */
};

#define CONC_ITERS 200

static void* concurrent_worker(void* p)
{
    struct concurrent_ctx* const c = p;
    for (int i = 0; i < c->iters; i++) {
        if (run_scope(c->db, "c") != 0) {
            c->nested_begins++;
        }
    }
    return NULL;
}

static void test_concurrent_no_nested_begin(void)
{
    sqlite3* const db = open_db();
    create_schema(db);

    const int nthreads = 4;
    struct concurrent_ctx ctxs[nthreads];
    pthread_t th[nthreads];

    for (int i = 0; i < nthreads; i++) {
        ctxs[i].db = db;
        ctxs[i].iters = CONC_ITERS;
        ctxs[i].nested_begins = 0;
        pthread_create(&th[i], NULL, concurrent_worker, &ctxs[i]);
    }
    for (int i = 0; i < nthreads; i++) {
        pthread_join(th[i], NULL);
    }

    long total_nested = 0;
    for (int i = 0; i < nthreads; i++) {
        total_nested += ctxs[i].nested_begins;
    }

    CHECK(total_nested == 0,
        "concurrent begin/commit: nested-BEGIN count = %ld, expected 0", total_nested);

    sqlite3_close(db);
}

/* -------------------------------------------------------------------------
 * case 2: failed COMMIT -> ROLLBACK, not half-open, next BEGIN ok (defect B)
 * ------------------------------------------------------------------------ */

static void test_failed_commit_rollback(void)
{
    sqlite3* const db = open_db();
    create_deferred_fk_db(db);

    const int bres = sms_tx_begin(db);
    CHECK(bres == SQLITE_OK, "failed-commit: BEGIN ok, got %d", bres);

    /* insert with no matching parent: succeeds at INSERT, fails at COMMIT */
    sqlite3_exec(db, "INSERT INTO child(x) VALUES(1)", NULL, NULL, NULL);

    const int cres = sms_tx_commit(db, bres);
    CHECK(cres != SQLITE_OK, "failed-commit: COMMIT failed as expected, got %d", cres);

    /* the connection must not be left half-open: sms_tx_commit rolled it back to
     * autocommit after the failed COMMIT */
    CHECK(sqlite3_get_autocommit(db) != 0,
        "failed-commit: connection restored to autocommit (no half-open) after failed COMMIT");

    /* a fresh BEGIN must succeed -> proves the ROLLBACK recovered cleanly */
    const int b2 = sms_tx_begin(db);
    CHECK(b2 == SQLITE_OK, "failed-commit: next BEGIN ok after rollback, got %d", b2);
    sms_tx_commit(db, b2);

    sqlite3_close(db);
}

/* -------------------------------------------------------------------------
 * case 3: failed BEGIN -> no half-open state left behind
 * ------------------------------------------------------------------------ */

static void test_failed_begin(void)
{
    sqlite3* const db = open_db();
    create_schema(db);

    /* open a transaction by hand, then try to BEGIN again -> nested, fails */
    if (sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL) != SQLITE_OK) {
        printf("FAIL test setup: manual BEGIN failed\n");
        sqlite3_close(db);
        return;
    }

    const int bres = sms_tx_begin(db);
    CHECK(bres != SQLITE_OK, "failed-begin: nested BEGIN fails as expected, got %d", bres);

    /* clean up the manually-open transaction, then prove the connection is
     * usable again (our failed BEGIN introduced nothing half-open) */
    sqlite3_exec(db, "ROLLBACK TRANSACTION", NULL, NULL, NULL);

    const int b2 = sms_tx_begin(db);
    CHECK(b2 == SQLITE_OK, "failed-begin: BEGIN ok after cleaning up, got %d", b2);
    sqlite3_exec(db, "INSERT INTO mt_out(id, addr) VALUES('z','y')", NULL, NULL, NULL);
    const int c2 = sms_tx_commit(db, b2);
    CHECK(c2 == SQLITE_OK, "failed-begin: COMMIT ok after cleaning up, got %d", c2);

    sqlite3_close(db);
}

/* -------------------------------------------------------------------------
 * case 4: writer contention / SQLITE_BUSY -> no leak
 * ------------------------------------------------------------------------ */

static void test_contention_no_leak(void)
{
    /* Two connections to the same in-memory DB are not shared, so use a temp
     * file DB to model real writer contention between two threads. */
    char path[] = "/tmp/ctq_sms_tx_content_XXXXXX";
    if (mkstemp(path) < 0) {
        printf("FAIL contention: mkstemp failed\n");
        return;
    }

    sqlite3* a = NULL;
    sqlite3* b = NULL;
    if (sqlite3_open(path, &a) != SQLITE_OK || sqlite3_open(path, &b) != SQLITE_OK) {
        printf("FAIL contention: open failed\n");
        sqlite3_close(a);
        sqlite3_close(b);
        unlink(path);
        return;
    }

    const char* create =
        "CREATE TABLE mt_out(id TEXT, addr TEXT, mr INTEGER, srr_status INTEGER, created_at INTEGER);";
    sqlite3_exec(a, create, NULL, NULL, NULL);

    /* B takes and holds a write lock (BEGIN IMMEDIATE + write). */
    if (sqlite3_exec(b, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        printf("FAIL contention: B BEGIN IMMEDIATE failed\n");
        goto contention_cleanup;
    }
    sqlite3_exec(b, "INSERT INTO mt_out(id, addr) VALUES('b','x')", NULL, NULL, NULL);

    /* A starts a deferred transaction, then tries to write -> SQLITE_BUSY
     * because B holds the write lock. This is the contention path. */
    const int ab = sms_tx_begin(a);
    if (ab != SQLITE_OK) {
        printf("FAIL contention: A BEGIN failed under contention: %d\n", ab);
        goto contention_cleanup;
    }
    int busy = 0;
    if (sqlite3_exec(a, "INSERT INTO mt_out(id, addr) VALUES('a','x')", NULL, NULL, NULL) == SQLITE_BUSY) {
        busy = 1;
    }
    CHECK(busy == 1, "contention: second writer got SQLITE_BUSY as expected");

    /* A rolls its transaction back (releasing its locks); B then commits.
     * Neither connection is ever left in a half-open transaction. */
    const int ac = sms_tx_commit(a, ab);
    CHECK(ac == SQLITE_OK, "contention: A commit/rollback clean, got %d", ac);
    CHECK(sqlite3_get_autocommit(a) != 0, "contention: A restored to autocommit (no leak)");

    sqlite3_exec(b, "COMMIT", NULL, NULL, NULL);
    CHECK(sqlite3_get_autocommit(b) != 0, "contention: B restored to autocommit (no leak)");

contention_cleanup:
    sqlite3_close(a);
    sqlite3_close(b);
    unlink(path);
}

/* Deterministic COMMIT error (deferred FK violation) -> no leak, asserted via
 * the autocommit state. This is the same guarantee a real SQLITE_BUSY-on-COMMIT
 * must satisfy. */
static void test_commit_failure_no_leak(void)
{
    sqlite3* const db = open_db();
    create_deferred_fk_db(db);

    const int bres = sms_tx_begin(db);
    CHECK(bres == SQLITE_OK, "commit-failure: BEGIN ok, got %d", bres);

    sqlite3_exec(db, "INSERT INTO child(x) VALUES(1)", NULL, NULL, NULL);
    const int cres = sms_tx_commit(db, bres);

    CHECK(cres != SQLITE_OK, "commit-failure: COMMIT error propagated, got %d", cres);
    CHECK(sqlite3_get_autocommit(db) != 0,
        "commit-failure: connection restored to autocommit (no leak) after failed COMMIT");

    sqlite3_close(db);
}

/* -------------------------------------------------------------------------
 * case 5: multipart out-of-order + duplicate seqorder -> count never exceeds
 * ------------------------------------------------------------------------ */

/* Emulate the per-part DB write pattern: each part is inserted in its own
 * SMS_TX_SCOPE transaction (as production does), in shuffled order, with one
 * duplicate seqorder. The UNIQUE(id, seqorder) rejects the duplicate, so the
 * final row count equals the number of distinct parts. */
static int put_part(sqlite3* db, const char* id, int ref, int order)
{
    SMS_TX_SCOPE(t, db, id);
    char sql[128];
    snprintf(sql, sizeof(sql),
        "INSERT INTO parts(id, ref, seqorder) VALUES('%s', %d, %d)", id, ref, order);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    return 0;
}

static int count_rows(sqlite3* db)
{
    long n = 0;
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM parts", -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        n = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return (int)n;
}

static void test_multipart_out_of_order(void)
{
    sqlite3* const db = open_db();
    create_parts_table(db);

    /* 5 parts of one message, inserted out of order, plus one duplicate of
     * seqorder 2 for the same id. Distinct parts = 5. */
    put_part(db, "msg1", 100, 3);
    put_part(db, "msg1", 100, 1);
    put_part(db, "msg1", 100, 5);
    put_part(db, "msg1", 100, 2);
    put_part(db, "msg1", 100, 4);
    put_part(db, "msg1", 100, 2); /* duplicate seqorder -> rejected */

    const int n = count_rows(db);
    CHECK(n == 5, "multipart: distinct part count = %d, expected 5 (never exceeds parts)", n);

    sqlite3_close(db);
}

/* -------------------------------------------------------------------------
 * case 6: SRR unknown -> sms_tx_column_int_or_null returns -1 for NULL
 * ------------------------------------------------------------------------ */

static void test_srr_null(void)
{
    sqlite3* const db = open_db();
    create_schema(db);

    /* mt_out(id, addr, mr, srr_status, created_at): row 'a' has NULL mr and
     * NULL srr_status (unknown), row 'b' has both known. */
    sqlite3_exec(db, "INSERT INTO mt_out(id, addr, mr, srr_status, created_at) "
                     "VALUES('a','1',NULL,NULL,0)", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO mt_out(id, addr, mr, srr_status, created_at) "
                     "VALUES('b','2',0,7,0)", NULL, NULL, NULL);

    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT srr_status, mr FROM mt_out ORDER BY id", -1, &stmt, NULL)
            != SQLITE_OK) {
        printf("FAIL srr: prepare failed\n");
        sqlite3_close(db);
        return;
    }

    int srr_a = -99, mr_a = -99, srr_b = -99, mr_b = -99;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        srr_a = sms_tx_column_int_or_null(stmt, 0, -1);
        mr_a  = sms_tx_column_int_or_null(stmt, 1, -1);
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        srr_b = sms_tx_column_int_or_null(stmt, 0, -1);
        mr_b  = sms_tx_column_int_or_null(stmt, 1, -1);
    }
    sqlite3_finalize(stmt);

    CHECK(srr_a == -1, "srr: NULL srr_status renders as -1 (unknown), got %d", srr_a);
    CHECK(mr_a == -1, "srr: NULL mr renders as -1, got %d", mr_a);
    CHECK(srr_b == 7, "srr: known srr_status preserved, got %d", srr_b);
    CHECK(mr_b == 0, "srr: known mr == 0 preserved (not turned into -1), got %d", mr_b);

    sqlite3_close(db);
}

/* -------------------------------------------------------------------------
 * case 7: 4-thread stress (sim1 AT, sim2 AT, web, retry) -> 0 nested-BEGIN
 * ------------------------------------------------------------------------ */

#define STRESS_ITERS 100

struct stress_ctx {
    sqlite3* db;
    const char* label;
    long nested_begins;
};

static void* stress_worker(void* p)
{
    struct stress_ctx* const c = p;
    for (int i = 0; i < STRESS_ITERS; i++) {
        if (run_scope(c->db, c->label) != 0) {
            c->nested_begins++;
        }
    }
    return NULL;
}

static void test_four_thread_stress(void)
{
    sqlite3* const db = open_db();
    create_schema(db);

    const char* labels[] = {"sim1 AT", "sim2 AT", "web", "retry"};
    const int nthreads = 4;
    struct stress_ctx ctxs[nthreads];
    pthread_t th[nthreads];

    for (int i = 0; i < nthreads; i++) {
        ctxs[i].db = db;
        ctxs[i].label = labels[i];
        ctxs[i].nested_begins = 0;
        pthread_create(&th[i], NULL, stress_worker, &ctxs[i]);
    }
    for (int i = 0; i < nthreads; i++) {
        pthread_join(th[i], NULL);
    }

    long total_nested = 0;
    for (int i = 0; i < nthreads; i++) {
        total_nested += ctxs[i].nested_begins;
    }

    CHECK(total_nested == 0,
        "4-thread stress: nested-BEGIN count = %ld, expected 0", total_nested);

    sqlite3_close(db);
}

/* -------------------------------------------------------------------------
 * case 8: forced failed BEGIN through SMS_TX_SCOPE -> autocommit, next begin ok
 * ------------------------------------------------------------------------ */

/* Thin wrapper so the macro's failed-BEGIN path (which returns -1 from the
 * enclosing function) can be exercised and its return value asserted. */
static int try_scope(sqlite3* db, const char* tag)
{
    SMS_TX_SCOPE(t, db, tag);
    return 0;
}

static void test_failed_begin_via_scope(void)
{
    sqlite3* const db = open_db();
    create_schema(db);

    /* Force the macro's BEGIN to fail: a transaction is already open, so the
     * macro's BEGIN is nested and returns an error. */
    CHECK(sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, NULL) == SQLITE_OK,
        "failed-begin-via-scope: manual BEGIN ok");

    int rc = try_scope(db, "x");
    CHECK(rc == -1, "failed-begin-via-scope: macro returns -1 on failed BEGIN, got %d", rc);

    /* The macro's failed BEGIN opened nothing: rolling back the manual
     * transaction restores autocommit and a fresh BEGIN works. The mutex stays
     * usable (the P2 double-unlock is gone, so nothing is corrupted). */
    CHECK(sqlite3_exec(db, "ROLLBACK TRANSACTION", NULL, NULL, NULL) == SQLITE_OK,
        "failed-begin-via-scope: manual ROLLBACK ok");
    CHECK(sqlite3_get_autocommit(db) != 0,
        "failed-begin-via-scope: connection in autocommit after failed BEGIN");
    int b2 = sms_tx_begin(db);
    CHECK(b2 == SQLITE_OK, "failed-begin-via-scope: next BEGIN ok, got %d", b2);
    sms_tx_commit(db, b2);

    sqlite3_close(db);
}

/* -------------------------------------------------------------------------
 * case 9: forced failed COMMIT through SMS_TX_SCOPE (deferred FK) -> ROLLBACK
 * ------------------------------------------------------------------------ */

/* Wrapper that opens a scope, writes a row that violates the deferred FK, and
 * lets the macro COMMIT at scope exit, so the failed-COMMIT path is exercised. */
static int commit_scope(sqlite3* db, const char* tag)
{
    SMS_TX_SCOPE(t, db, tag);
    sqlite3_exec(db, "INSERT INTO child(x) VALUES(1)", NULL, NULL, NULL);
    return 0;
}

static void test_failed_commit_via_scope(void)
{
    sqlite3* const db = open_db();
    create_deferred_fk_db(db);

    commit_scope(db, "x");

    /* The macro's COMMIT failed (deferred FK) and rolled back: the connection is
     * back to autocommit (not half-open), the write did not persist, and a fresh
     * BEGIN works. */
    CHECK(sqlite3_get_autocommit(db) != 0,
        "failed-commit-via-scope: connection back to autocommit after ROLLBACK");

    long persisted = -1;
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM child", -1, &stmt, NULL) == SQLITE_OK
            && sqlite3_step(stmt) == SQLITE_ROW) {
        persisted = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    CHECK(persisted == 0,
        "failed-commit-via-scope: violated write was rolled back (rows=%ld)", persisted);

    int b2 = sms_tx_begin(db);
    CHECK(b2 == SQLITE_OK, "failed-commit-via-scope: next BEGIN ok, got %d", b2);
    sms_tx_commit(db, b2);

    sqlite3_close(db);
}

/* -------------------------------------------------------------------------
 * driver
 * ------------------------------------------------------------------------ */

int main(void)
{
    test_concurrent_no_nested_begin();
    test_failed_commit_rollback();
    test_failed_begin();
    test_contention_no_leak();
    test_commit_failure_no_leak();
    test_multipart_out_of_order();
    test_srr_null();
    test_four_thread_stress();
    test_failed_begin_via_scope();
    test_failed_commit_via_scope();

    printf("\n%s: done %d checks: %d OK %d FAILS\n",
        "sms_tx_test", ok + faults, ok, faults);
    return faults ? 1 : 0;
}
