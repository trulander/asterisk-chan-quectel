#define _XOPEN_SOURCE 700
#include "sms_delete_flow.h"
#include "sms_delete_state.h"
#include "sms_incoming_schema.h"
#include "sms_multipart.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); ++failures; } } while (0)

typedef struct sqlite_flow {
    sqlite3* db;
    const char* key;
    const char* message;
    const char* sender;
    const char* timestamp;
    const char* device;
    const char* storage;
    int index;
    int order;
    int parts;
    long long rowid;
    char* assembled;
    int complete;
    int verify;
    int fail_commit;
    int fail_queue_commit;
    int queued;
    int deleted;
    int deferred;
    int mid_generations;
    char mid[64];
} sqlite_flow_t;

static int sql_count(sqlite3* db, const char* sql)
{
    sqlite3_stmt* stmt = NULL;
    int value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) value = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

static int begin(sqlite3* db) { return sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1; }
static int commit(sqlite3* db)
{
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK) return 0;
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
}

static long long rowid_for_key(sqlite3* db, const char* key)
{
    sqlite3_stmt* stmt = NULL;
    long long rowid = 0;
    if (sqlite3_prepare_v2(db, "SELECT rowid FROM incoming_msg WHERE key=?", -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        rowid = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return rowid;
}

static int row_state(sqlite_flow_t* flow, long long rowid, sms_delete_state_t* state)
{
    sqlite3_stmt* stmt = NULL;
    int result = -1;
    if (sqlite3_prepare_v2(flow->db, "SELECT delete_status,delete_attempts FROM incoming_msg WHERE rowid=?", -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_bind_int64(stmt, 1, rowid) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        state->status = (sms_delete_status_t)sqlite3_column_int(stmt, 0);
        state->attempts = (unsigned)sqlite3_column_int(stmt, 1);
        result = 0;
    }
    sqlite3_finalize(stmt);
    return result;
}

static int persist_state(sqlite_flow_t* flow, sms_delete_state_t state, int retry_at)
{
    if (begin(flow->db)) return -1;
    if (sms_incoming_delete_state_update(flow->db, flow->rowid, flow->device, flow->storage, flow->index,
                                         state.status, state.attempts, retry_at)) {
        sqlite3_exec(flow->db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return commit(flow->db);
}

static int save_cell(void* opaque, sms_flow_result_t* result)
{
    sqlite_flow_t* flow = opaque;
    if (begin(flow->db)) return -1;
    char* assembled = NULL;
    long long rowid = 0;
    const int count = sms_multipart_put(flow->db, flow->key, flow->order, flow->parts, 3600, flow->message,
                                        flow->device, flow->storage, flow->index, 1, &assembled, &rowid);
    if (count <= 0) {
        sqlite3_free(assembled);
        sqlite3_exec(flow->db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    if (flow->fail_commit) {
        if (sqlite3_exec(flow->db, "INSERT INTO deferred_child(x) VALUES(999)", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_free(assembled);
            sqlite3_exec(flow->db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
    }
    if (commit(flow->db)) {
        sqlite3_free(assembled);
        return -1;
    }
    sqlite3_free(flow->assembled);
    flow->assembled = assembled;
    flow->rowid = rowid;
    flow->complete = count == flow->parts;
    result->complete = flow->complete;
    result->durable_rowid = rowid;
    return 0;
}

static int queue_complete(void* opaque, const sms_flow_result_t* result)
{
    sqlite_flow_t* flow = opaque;
    (void)result;
    if (!flow->assembled || (flow->parts == 3 && strcmp(flow->assembled, "ABC"))) return -1;
    int queued = 0;
    char existing_mid[64];
    if (sms_incoming_group_queue_state(flow->db, flow->key, &queued, existing_mid, sizeof(existing_mid))) return -1;
    if (queued) {
        memcpy(flow->mid, existing_mid, strlen(existing_mid) + 1);
        return 0;
    }
    snprintf(flow->mid, sizeof(flow->mid), "mt-%d", ++flow->mid_generations);
    if (begin(flow->db)) return -1;
    int inserted = 0;
    char effective_mid[64];
    if (sms_incoming_mt_queue_group(flow->db, flow->key, flow->device, flow->mid, flow->sender,
                                   flow->assembled, flow->timestamp, effective_mid, sizeof(effective_mid), &inserted)) {
        sqlite3_exec(flow->db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    if (flow->fail_queue_commit && sqlite3_exec(flow->db, "INSERT INTO deferred_child(x) VALUES(999)", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(flow->db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    if (commit(flow->db)) return -1;
    memcpy(flow->mid, effective_mid, strlen(effective_mid) + 1);
    flow->queued += inserted;
    /* MT row and marker share one committed transaction before CMGD is allowed. */
    if (!sqlite3_get_autocommit(flow->db) || (inserted && sql_count(flow->db, "SELECT COUNT(*) FROM mt_queue") < 1)) return -1;
    return 0;
}

static int delete_cell(void* opaque, const sms_flow_result_t* result)
{
    sqlite_flow_t* flow = opaque;
    (void)result;
    sms_delete_state_t state;
    if (row_state(flow, flow->rowid, &state)) return -1;
    if (flow->verify) {
        if (state.status == SMS_DELETE_VERIFYING || state.status == SMS_DELETE_VERIFYING_UNCOUNTED) {
            if (sms_delete_state_apply(&state, SMS_DELETE_EVENT_VERIFY_PRESENT_SAVED)) return -1;
        }
        if (state.status == SMS_DELETE_STOPPED) return persist_state(flow, state, 0);
    }
    if (state.status == SMS_DELETE_PENDING && sms_delete_state_apply(&state, SMS_DELETE_EVENT_DELETE_QUEUED)) return -1;
    if (persist_state(flow, state, 0)) return -1;
    ++flow->deleted;
    return 0;
}

static int mark_absent(void* opaque)
{
    sqlite_flow_t* flow = opaque;
    sms_delete_state_t state;
    if (row_state(flow, flow->rowid, &state)) return -1;
    if (state.status != SMS_DELETE_VERIFYING && state.status != SMS_DELETE_VERIFYING_UNCOUNTED &&
        sms_delete_state_apply(&state, SMS_DELETE_EVENT_VERIFY_START)) return -1;
    if (sms_delete_state_apply(&state, SMS_DELETE_EVENT_VERIFY_ABSENT)) return -1;
    if (begin(flow->db)) return -1;
    if (sms_incoming_delete_state_update(flow->db, flow->rowid, flow->device, flow->storage, flow->index,
                                         state.status, state.attempts, 0) || sms_incoming_gc_completed_groups(flow->db)) {
        sqlite3_exec(flow->db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return commit(flow->db);
}

static void defer_retry(void* opaque) { ++((sqlite_flow_t*)opaque)->deferred; }
static const sms_flow_ops_t ops = {save_cell, queue_complete, delete_cell, mark_absent, defer_retry};

static int open_database(const char* path, sqlite3** db)
{
    if (sqlite3_open(path, db) != SQLITE_OK) return -1;
    if (sms_incoming_schema_ensure(*db)) return -1;
    if (sqlite3_exec(*db,
        "CREATE TABLE IF NOT EXISTS mt_queue(device TEXT NOT NULL,mid TEXT PRIMARY KEY,from_num TEXT NOT NULL,msg TEXT NOT NULL,ts TEXT,state TEXT NOT NULL DEFAULT 'new');"
        "CREATE TABLE IF NOT EXISTS deferred_parent(id INTEGER PRIMARY KEY);"
        "CREATE TABLE IF NOT EXISTS deferred_child(x INTEGER REFERENCES deferred_parent(id) DEFERRABLE INITIALLY DEFERRED);"
        "PRAGMA foreign_keys=ON;", NULL, NULL, NULL) != SQLITE_OK) return -1;
    return sqlite3_exec(*db, "DELETE FROM incoming_msg;DELETE FROM mt_queue;DELETE FROM deferred_child;", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

static sms_flow_request_t make_request(sqlite_flow_t* flow, int complete)
{
    sms_flow_request_t request = {1, SMS_FLOW_SIM_CELL, flow->device, flow->storage, flow->index, complete};
    return request;
}

static void test_multipart_retained_until_mt_queue_and_all_absent(const char* path)
{
    sqlite_flow_t flow = {.key="sim-a/+123/42/3", .sender="+123", .timestamp="2026-01-01T00:00:00Z",
                          .device="sim-a", .storage="SM", .parts=3, .mid="mt-multipart"};
    CHECK(open_database(path, &flow.db) == 0, "open SQLite database for production flow test");
    const int orders[] = {1, 3, 2};
    const int indices[] = {10, 30, 20};
    const char* texts[] = {"A", "C", "B"};
    long long rowids[3] = {0};
    for (int i = 0; i < 3; ++i) {
        flow.order = orders[i];
        flow.index = indices[i];
        flow.message = texts[i];
        sms_flow_request_t request = make_request(&flow, 0);
        sms_flow_result_t result;
        CHECK(sms_delete_flow_process(&request, SMS_FLOW_CELL_PRESENT, &ops, &flow, &result) == 0,
              "production flow durably saves each out-of-order SIM segment");
        rowids[i] = result.durable_rowid;
        CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM incoming_msg") == i + 1, "segments remain in incoming_msg");
        if (i < 2) {
            CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM mt_queue") == 0, "incomplete SMS is not delivered");
            CHECK(flow.queued == 0, "no MT queue before all parts assemble");
        }
    }
    CHECK(flow.queued == 1 && sql_count(flow.db, "SELECT COUNT(*) FROM mt_queue") == 1, "complete SMS has exactly one durable MT row");
    CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM incoming_msg WHERE mt_queued=1 AND mt_mid='mt-1'") == 3,
          "queue marker and stable MID are committed for all retained parts");
    CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM incoming_msg") == 3, "queue durability alone does not GC SIM segments");
    sqlite3_stmt* msg_stmt = NULL;
    CHECK(sqlite3_prepare_v2(flow.db, "SELECT msg FROM mt_queue WHERE mid='mt-1'", -1, &msg_stmt, NULL) == SQLITE_OK &&
              sqlite3_step(msg_stmt) == SQLITE_ROW && !strcmp((const char*)sqlite3_column_text(msg_stmt, 0), "ABC"),
          "out-of-order production assembly is queued in sequence order");
    sqlite3_finalize(msg_stmt);

    /* CMGD failures followed by fresh reads of the same retained group must
     * reuse its MID, not create a new MT row on each retry. */
    flow.rowid = rowids[2];
    flow.index = indices[2];
    flow.order = 2;
    flow.message = "B";
    flow.verify = 1;
    for (unsigned retry = 1; retry <= 2; ++retry) {
        sms_delete_state_t state;
        CHECK(row_state(&flow, flow.rowid, &state) == 0 && sms_delete_state_apply(&state, SMS_DELETE_EVENT_CMGD_ERROR) == 0,
              "CMGD failure enters verification before a fresh present read");
        CHECK(persist_state(&flow, state, 0) == 0, "persist CMGD error before reread");
        CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_VERIFY_START) == 0 && persist_state(&flow, state, 0) == 0,
              "fresh verification starts from confirmed CMGD failure");
        sms_flow_request_t request = make_request(&flow, 1);
        sms_flow_result_t result;
        CHECK(sms_delete_flow_process(&request, SMS_FLOW_CELL_PRESENT, &ops, &flow, &result) == 0,
              "fresh present reread re-saves and retries the retained group");
        CHECK(row_state(&flow, flow.rowid, &state) == 0 && state.attempts == retry,
              "only confirmed CMGD error plus fresh durable presence increments attempts");
        CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM mt_queue") == 1 && flow.queued == 1 && flow.mid_generations == 1,
              "retries do not create unbounded duplicate MT rows or MIDs");
    }
    flow.verify = 0;

    for (int i = 0; i < 3; ++i) {
        flow.rowid = rowids[i];
        flow.index = indices[i];
        sms_flow_request_t request = make_request(&flow, 1);
        sms_flow_result_t result;
        CHECK(sms_delete_flow_process(&request, SMS_FLOW_CELL_ABSENT, &ops, &flow, &result) == 0,
              "fresh absent read confirms one corresponding SIM cell");
        CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM incoming_msg") == (i == 2 ? 0 : 3),
              "GC waits for every segment cell to be confirmed absent");
    }
    sqlite3_free(flow.assembled);
    sqlite3_close(flow.db);
}

static void test_queue_and_marker_commit_atomically(const char* path)
{
    sqlite_flow_t flow = {.key="sim-a/+123/8/1/SM/8", .sender="+123", .timestamp="2026-01-01T00:00:00Z",
                          .device="sim-a", .storage="SM", .index=8, .order=1, .parts=1, .message="full", .fail_queue_commit=1};
    CHECK(open_database(path, &flow.db) == 0, "open DB for MT queue/group marker commit failure");
    sms_flow_request_t request = make_request(&flow, 1);
    sms_flow_result_t result;
    CHECK(sms_delete_flow_process(&request, SMS_FLOW_CELL_PRESENT, &ops, &flow, &result) < 0,
          "failed MT queue/group marker transaction aborts the flow");
    CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM mt_queue") == 0, "failed marker commit rolls back MT queue row too");
    CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM incoming_msg WHERE mt_queued=1 OR mt_mid IS NOT NULL") == 0,
          "failed marker commit leaves no false durable group marker");
    CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM incoming_msg WHERE message='full'") == 1,
          "queue transaction failure retains the already durable source SMS for retry");
    CHECK(flow.deleted == 0 && flow.deferred == 1, "failed queue transaction never issues CMGD");
    sqlite3_free(flow.assembled);
    sqlite3_close(flow.db);
}

static void test_commit_failure_never_queues_or_deletes(const char* path)
{
    sqlite_flow_t flow = {.key="sim-a/+123/7/1/SM/8", .sender="+123", .timestamp="2026-01-01T00:00:00Z",
                          .device="sim-a", .storage="SM", .index=8, .order=1, .parts=1, .message="full", .mid="mt-failed", .fail_commit=1};
    CHECK(open_database(path, &flow.db) == 0, "open SQLite DB for forced commit failure");
    sms_flow_request_t request = make_request(&flow, 1);
    sms_flow_result_t result;
    CHECK(sms_delete_flow_process(&request, SMS_FLOW_CELL_PRESENT, &ops, &flow, &result) < 0, "deferred constraint forces segment COMMIT failure");
    CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM incoming_msg") == 0, "failed COMMIT leaves no partial segment row");
    CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM mt_queue WHERE mid='mt-failed'") == 0 && flow.queued == 0, "failed COMMIT cannot partially deliver");
    CHECK(flow.deleted == 0 && flow.deferred == 1, "failed COMMIT schedules retry and never queues CMGD");
    sqlite3_free(flow.assembled);
    sqlite3_close(flow.db);
}

static void test_blocked_due_job_does_not_starve_other_devices(const char* path)
{
    sqlite3* db = NULL;
    CHECK(open_database(path, &db) == 0, "open DB for due-job fairness test");
    CHECK(sqlite3_exec(db,
        "INSERT INTO incoming_msg(key,seqorder,message,source_device,source_storage,source_index,delete_status,delete_attempts,retry_at) VALUES"
        "('blocked',1,'b','missing-device','SM',4,4,2,0),('active',1,'a','active-device','SM',5,4,3,0)", NULL, NULL, NULL) == SQLITE_OK,
        "insert a blocked first job and an actionable second job");
    long long rowid = 0;
    int index = -1;
    char device[32];
    CHECK(sms_incoming_delete_job_pick(db, 0, &rowid, device, sizeof(device), &index) == 1 && !strcmp(device, "missing-device"),
          "picker initially returns the oldest due row");
    CHECK(begin(db) == 0 && sms_incoming_delete_retry_defer(db, rowid, 100) == 0 && commit(db) == 0,
          "temporarily non-actionable row is deferred without changing status or attempts");
    CHECK(sms_incoming_delete_job_pick(db, 0, &rowid, device, sizeof(device), &index) == 1 &&
              !strcmp(device, "active-device") && index == 5,
          "deferred row no longer starves the next actionable SIM job");
    CHECK(sql_count(db, "SELECT COUNT(*) FROM incoming_msg WHERE key='blocked' AND delete_status=4 AND delete_attempts=2 AND retry_at=100") == 1,
          "defer changes only retry time and issues no deletion action");
    sqlite3_close(db);
}

static int deny_incoming_select(void* context, int action, const char* arg1, const char* arg2, const char* dbname, const char* trigger)
{
    (void)context; (void)arg2; (void)dbname; (void)trigger;
    return action == SQLITE_READ && arg1 && !strcmp(arg1, "incoming_msg") ? SQLITE_DENY : SQLITE_OK;
}

static void test_group_marker_failures_binding_and_restart(const char* path)
{
    sqlite_flow_t flow = {.key="key-' OR 1=1 --", .sender="+1555", .timestamp="t", .device="dev-a",
                          .storage="SM", .index=17, .order=1, .parts=1, .message="payload"};
    CHECK(open_database(path, &flow.db) == 0, "open DB for group marker adversarial cases");
    char* assembled = NULL;
    long long rowid = 0;
    CHECK(begin(flow.db) == 0 && sms_multipart_put(flow.db, flow.key, 1, 1, 3600, flow.message, flow.device,
          flow.storage, flow.index, 1, &assembled, &rowid) == 1 && commit(flow.db) == 0,
          "seed isolated group with a SQL-looking bound key");
    sqlite3_free(assembled);
    int queued = -1, inserted = -1;
    char mid[64];
    CHECK(sms_incoming_group_queue_state(flow.db, "no such group", &queued, mid, sizeof(mid)) != 0,
          "missing group lookup is an explicit error, not an empty successful group");
    CHECK(sqlite3_set_authorizer(flow.db, deny_incoming_select, NULL) == SQLITE_OK,
          "install marker lookup failure injection");
    CHECK(sms_incoming_group_queue_state(flow.db, flow.key, &queued, mid, sizeof(mid)) != 0,
          "SQLite lookup error propagates from group marker query");
    sqlite3_set_authorizer(flow.db, NULL, NULL);

    CHECK(begin(flow.db) == 0, "begin queue/marker transaction");
    CHECK(sms_incoming_mt_queue_group(flow.db, flow.key, flow.device, "mid-1", flow.sender, "payload", "t",
          mid, sizeof(mid), &inserted) == 0 && inserted == 1 && !strcmp(mid, "mid-1"),
          "first group queue inserts a bound MT row and records its marker");
    CHECK(commit(flow.db) == 0, "commit durable queue and marker together");
    sqlite3_close(flow.db);

    CHECK(sqlite3_open(path, &flow.db) == SQLITE_OK, "reopen persisted DB as a fresh process");
    CHECK(sms_incoming_group_queue_state(flow.db, flow.key, &queued, mid, sizeof(mid)) == 0 && queued == 1 &&
          !strcmp(mid, "mid-1"), "restart reloads exact stable group MID");
    CHECK(begin(flow.db) == 0 && sms_incoming_mt_queue_group(flow.db, flow.key, "dev-b", "mid-2", "+other",
          "different", "t2", mid, sizeof(mid), &inserted) == 0 && inserted == 0 && !strcmp(mid, "mid-1") &&
          commit(flow.db) == 0, "repeat queue after restart reuses marker without inserting candidate MID");
    CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM mt_queue") == 1 &&
          sql_count(flow.db, "SELECT COUNT(*) FROM mt_queue WHERE mid='mid-1' AND device='dev-a' AND msg='payload'") == 1,
          "retry is idempotent and preserves original row fields");
    CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM incoming_msg WHERE key=\"key-' OR 1=1 --\" AND mt_mid='mid-1'") == 1 &&
          sql_count(flow.db, "SELECT COUNT(*) FROM incoming_msg WHERE mt_mid='mid-1'") == 1,
          "bound group key cannot mark unrelated groups");
    CHECK(sms_incoming_group_queue_state(flow.db, flow.key, &queued, mid, 1) != 0,
          "undersized MID buffer fails without writing beyond caller capacity");

    /* A second group exercises failures between queue INSERT and marker UPDATE.
     * The caller's transaction must roll both effects back on every error. */
    CHECK(sqlite3_exec(flow.db, "INSERT INTO incoming_msg(key,seqorder,message,source_device,source_storage,source_index) VALUES('second',1,'two','dev-a','SM',18);"
          "CREATE TRIGGER reject_marker BEFORE UPDATE OF mt_queued ON incoming_msg WHEN NEW.key='second' BEGIN SELECT RAISE(ABORT,'marker failure'); END;",
          NULL, NULL, NULL) == SQLITE_OK, "seed second group and marker-write failure trigger");
    CHECK(begin(flow.db) == 0 && sms_incoming_mt_queue_group(flow.db, "second", "dev-a", "mid-marker-fail", "x", "two", "t",
          mid, sizeof(mid), &inserted) != 0, "marker write error propagates after attempted MT insert");
    sqlite3_exec(flow.db, "ROLLBACK", NULL, NULL, NULL);
    CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM mt_queue WHERE mid='mid-marker-fail'") == 0 &&
          sql_count(flow.db, "SELECT COUNT(*) FROM incoming_msg WHERE key='second' AND mt_queued=0 AND mt_mid IS NULL") == 1,
          "marker write failure leaves neither queued row nor false marker after rollback");
    sqlite3_exec(flow.db, "DROP TRIGGER reject_marker", NULL, NULL, NULL);
    CHECK(sqlite3_exec(flow.db, "CREATE TRIGGER reject_queue BEFORE INSERT ON mt_queue WHEN NEW.mid='mid-insert-fail' BEGIN SELECT RAISE(ABORT,'queue failure'); END;",
          NULL, NULL, NULL) == SQLITE_OK, "install queue-insert failure trigger");
    CHECK(begin(flow.db) == 0 && sms_incoming_mt_queue_group(flow.db, "second", "dev-a", "mid-insert-fail", "x", "two", "t",
          mid, sizeof(mid), &inserted) != 0, "queue insertion failure propagates before marker update");
    sqlite3_exec(flow.db, "ROLLBACK", NULL, NULL, NULL);
    CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM incoming_msg WHERE key='second' AND mt_queued=1") == 0 &&
          sql_count(flow.db, "SELECT COUNT(*) FROM mt_queue WHERE mid='mid-insert-fail'") == 0,
          "queue insertion failure leaves group unmarked and no queue row");
    sqlite3_close(flow.db);
}

static void test_retry_defer_boundaries_isolation_and_order(const char* path)
{
    sqlite3* db = NULL;
    CHECK(open_database(path, &db) == 0, "open DB for defer boundary/isolation tests");
    CHECK(sqlite3_exec(db,
        "INSERT INTO incoming_msg(key,seqorder,message,source_device,source_storage,source_index,delete_status,delete_attempts,retry_at) VALUES"
        "('null-device',1,'x',NULL,'SM',45,4,8,0),('empty-device',1,'x','','SM',46,4,9,0),"
        "('blank-device',1,'x','   ','SM',47,4,10,0),('null-index',1,'x','d4','SM',NULL,4,11,0),"
        "('negative-index',1,'x','d5','SM',-1,4,12,0),('other-storage',1,'x','d3','ME',44,4,7,0),"
        "('first',1,'a','d1','SM',11,4,2,5),('second',1,'b','d2','SM',22,5,4,5),"
        "('third',1,'c','d1','SM',33,6,1,6)", NULL, NULL, NULL) == SQLITE_OK,
        "insert malformed legacy rows ahead of valid jobs, plus non-SM and future jobs");
    long long first = 0, picked = 0; int index = -1; char device[32];
    CHECK(sms_incoming_delete_job_pick(db, 4, &picked, device, sizeof(device), &index) == 0,
          "retry_at=5 is not due at now=4, and malformed/non-SM rows are filtered");
    CHECK(sms_incoming_delete_job_pick(db, 5, &picked, device, sizeof(device), &index) == 1 &&
          !strcmp(device, "d1") && index == 11,
          "at now=5 picker skips earlier malformed NULL/empty/blank device and NULL/negative index plus non-SM rows");
    first = picked;
    CHECK(begin(db) == 0 && sms_incoming_delete_retry_defer(db, first, 10) == 0 && commit(db) == 0,
          "defer first due job by retry time only");
    CHECK(sql_count(db, "SELECT COUNT(*) FROM incoming_msg WHERE key='first' AND delete_status=4 AND delete_attempts=2 AND retry_at=10") == 1,
          "defer preserves status and attempt count exactly");
    CHECK(sms_incoming_delete_job_pick(db, 5, &picked, device, sizeof(device), &index) == 1 &&
          !strcmp(device, "d2") && index == 22, "next due device is selected instead of starvation");
    CHECK(begin(db) == 0 && sms_incoming_delete_retry_defer(db, picked, 10) == 0 && commit(db) == 0,
          "defer second independently blocked job");
    const long long third = rowid_for_key(db, "third");
    CHECK(third > 0 && sms_incoming_delete_retry_defer(db, third, 100) == 0,
          "keep third device's future retry outside the current fairness window");
    CHECK(sms_incoming_delete_job_pick(db, 5, &picked, device, sizeof(device), &index) == 0,
          "all deferred rows disappear until their due boundary");
    CHECK(sms_incoming_delete_job_pick(db, 10, &picked, device, sizeof(device), &index) == 1 &&
          !strcmp(device, "d1") && index == 11, "multiple devices resume in stable rowid order at due boundary");
    CHECK(sms_incoming_delete_retry_defer(db, -1, 100) != 0,
          "invalid rowid is rejected without mutating a real pending job");
    CHECK(sms_incoming_delete_retry_defer(db, rowid_for_key(db, "other-storage"), 100) != 0,
          "non-SM row cannot be deferred as a SIM delete job");
    CHECK(sql_count(db, "SELECT COUNT(*) FROM incoming_msg WHERE key='third' AND retry_at=100 AND delete_status=6 AND delete_attempts=1") == 1,
          "defer changes only retry time, preserving future row status and attempts");
    sqlite3_close(db);
}

static void test_restart_picker_recovers_cmgd_sent(const char* path)
{
    sqlite_flow_t flow = {.key="sim-a/+123/9/1/SM/9", .sender="+123", .timestamp="2026-01-01T00:00:00Z",
                          .device="sim-a", .storage="SM", .index=9, .order=1, .parts=1, .message="full", .mid="mt-restart"};
    CHECK(open_database(path, &flow.db) == 0, "open SQLite DB for restart recovery");
    sms_flow_request_t request = make_request(&flow, 1);
    sms_flow_result_t result;
    CHECK(sms_delete_flow_process(&request, SMS_FLOW_CELL_PRESENT, &ops, &flow, &result) == 0, "initial save and delete enqueue");
    flow.rowid = result.durable_rowid;
    sms_delete_state_t state;
    CHECK(row_state(&flow, flow.rowid, &state) == 0 && state.status == SMS_DELETE_CMGD_SENT, "CMGD_SENT is durable before response");

    CHECK(sqlite3_exec(flow.db, "CREATE TRIGGER reject_complete BEFORE UPDATE OF delete_status ON incoming_msg WHEN NEW.delete_status=2 BEGIN SELECT RAISE(ABORT,'injected completion write failure'); END;",
                       NULL, NULL, NULL) == SQLITE_OK, "install injected state-write failure");
    CHECK(row_state(&flow, flow.rowid, &state) == 0 && sms_delete_state_apply(&state, SMS_DELETE_EVENT_CMGD_OK) == 0,
          "successful modem response transitions in-memory state");
    CHECK(begin(flow.db) == 0, "begin failed CMGD_OK state update");
    CHECK(sms_incoming_delete_state_update(flow.db, flow.rowid, flow.device, flow.storage, flow.index, state.status, state.attempts, 0) != 0,
          "injected DB failure prevents durable CMGD_OK update");
    sqlite3_exec(flow.db, "ROLLBACK", NULL, NULL, NULL);

    sqlite3_close(flow.db);
    flow.db = NULL;
    CHECK(sqlite3_open(path, &flow.db) == SQLITE_OK, "reopen DB as after process restart");
    long long picked_rowid = 0;
    int picked_index = -1;
    char picked_device[32];
    CHECK(sms_incoming_delete_job_pick(flow.db, 2147483647, &picked_rowid, picked_device, sizeof(picked_device), &picked_index) == 1,
          "periodic picker includes ambiguous CMGD_SENT after DB state-write failure");
    CHECK(picked_rowid == flow.rowid && picked_index == flow.index && !strcmp(picked_device, flow.device),
          "restart recovery preserves row/device/index correlation");

    CHECK(sqlite3_exec(flow.db, "DROP TRIGGER reject_complete", NULL, NULL, NULL) == SQLITE_OK, "remove injected write failure");
    sms_delete_state_t recovered;
    CHECK(row_state(&flow, flow.rowid, &recovered) == 0 && recovered.status == SMS_DELETE_CMGD_SENT,
          "failed CMGD_OK persistence leaves recoverable CMGD_SENT state");
    CHECK(sms_delete_state_apply(&recovered, SMS_DELETE_EVENT_VERIFY_START) == 0 && recovered.status == SMS_DELETE_VERIFYING_UNCOUNTED,
          "restart recovery verifies current cell without counting an unconfirmed failure");
    CHECK(persist_state(&flow, recovered, 0) == 0, "persist restart verification state");
    request = make_request(&flow, 1);
    CHECK(sms_delete_flow_process(&request, SMS_FLOW_CELL_ABSENT, &ops, &flow, &result) == 0,
          "fresh current-cell absent result completes recovery");
    CHECK(sql_count(flow.db, "SELECT COUNT(*) FROM incoming_msg") == 0, "confirmed absent row is garbage collected after durable MT queue");
    sqlite3_free(flow.assembled);
    sqlite3_close(flow.db);
}

int main(void)
{
    char path[] = "/tmp/sms-delete-flow-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "create temporary SQLite path");
    if (fd >= 0) close(fd);
    test_multipart_retained_until_mt_queue_and_all_absent(path);
    test_queue_and_marker_commit_atomically(path);
    test_commit_failure_never_queues_or_deletes(path);
    test_blocked_due_job_does_not_starve_other_devices(path);
    test_group_marker_failures_binding_and_restart(path);
    test_retry_defer_boundaries_isolation_and_order(path);
    test_restart_picker_recovers_cmgd_sent(path);
    unlink(path);
    if (failures) {
        fprintf(stderr, "%d SQLite-backed flow test(s) failed\n", failures);
        return 1;
    }
    puts("SQLite-backed SMS delete flow tests passed");
    return 0;
}
