#include "call_paging.h"
#include "call_log.h"
#include "web_util.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct rows { struct call_page_row row[40]; int n; int stop_after; };

/** SQLite scalar для fault injection: запрос проходит prepare, но возвращает ошибку непосредственно при sqlite3_step. */
static void sqlite_step_failure(sqlite3_context* ctx, int argc, sqlite3_value** argv)
{
    (void)argc; (void)argv;
    sqlite3_result_error(ctx, "intentional step error", -1);
}

/** Сохраняет реальные callback-копии production pager; ограничение массива ловит неожиданный лишний ряд. */
static int collect(void* opaque, const struct call_page_row* row)
{
    struct rows* out = opaque;
    assert(out->n < (int)(sizeof(out->row) / sizeof(out->row[0])));
    out->row[out->n++] = *row;
    return out->stop_after > 0 && out->n >= out->stop_after;
}

/** Открывает настоящий SQLite in-memory DB и создаёт схему call_log, совместимую с durable production rows. */
static sqlite3* open_db(void)
{
    sqlite3* db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "CREATE TABLE call_log(id INTEGER PRIMARY KEY AUTOINCREMENT,device TEXT NOT NULL,number TEXT NOT NULL,started_at TEXT NOT NULL,direction TEXT NOT NULL,call_idx INTEGER NOT NULL DEFAULT -1,answered INTEGER NOT NULL DEFAULT 0,finished INTEGER NOT NULL DEFAULT 0,ended_at TEXT)", NULL, NULL, NULL) == SQLITE_OK);
    return db;
}

/** Вставляет bind-параметризованную durable row, включая NULL, long TEXT и все lifecycle поля; исключает тестовый SQL-инъекционный шум. */
static void insert_row(sqlite3* db, long long id, const char* device, const char* number,
    const char* started, const char* direction, int answered, int finished, const char* ended)
{
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "INSERT INTO call_log(id,device,number,started_at,direction,answered,finished,ended_at) VALUES(?,?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 1, id) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 2, device, -1, SQLITE_TRANSIENT) == SQLITE_OK);
    if (number) assert(sqlite3_bind_text(st, 3, number, -1, SQLITE_TRANSIENT) == SQLITE_OK); else assert(sqlite3_bind_null(st, 3) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 4, started, -1, SQLITE_TRANSIENT) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 5, direction, -1, SQLITE_TRANSIENT) == SQLITE_OK);
    assert(sqlite3_bind_int(st, 6, answered) == SQLITE_OK && sqlite3_bind_int(st, 7, finished) == SQLITE_OK);
    if (ended) assert(sqlite3_bind_text(st, 8, ended, -1, SQLITE_TRANSIENT) == SQLITE_OK); else assert(sqlite3_bind_null(st, 8) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_DONE);
    assert(sqlite3_finalize(st) == SQLITE_OK);
}

/** Сценарий payload: exact hostile SIM string не должен матчить похожие/SQL-подобные имена; проверяются все восемь выбранных столбцов, NULL, truncation и callback lifetime copy. */
static void test_exact_device_and_full_payload(void)
{
    sqlite3* db = open_db(); struct rows out = {0};
    char long_number[CALL_PAGE_TEXT_LEN + 50];
    memset(long_number, '7', sizeof(long_number) - 1); long_number[sizeof(long_number) - 1] = '\0';
    insert_row(db, 7, "sim1' OR 1=1 --", long_number, "2025-02-03T04:05:06Z", "missed", 0, 1, NULL);
    insert_row(db, 8, "sim1X", "+other", "2025-02-03T04:05:06Z", "incoming", 1, 0, "later");
    insert_row(db, 9, "sim1' OR 1=1 --", "+15551234", "2025-02-03T04:05:07Z", "outgoing", 1, 1, "2025-02-03T04:06:00Z");
    int total = -1;
    assert(call_paging_count(db, "sim1' OR 1=1 --", &total) == 0 && total == 2);
    assert(call_paging_page(db, "sim1' OR 1=1 --", 30, 0, &out, collect) == 0 && out.n == 2);
    assert(out.row[0].id == 9 && strcmp(out.row[0].device, "sim1' OR 1=1 --") == 0);
    assert(strcmp(out.row[0].number, "+15551234") == 0 && strcmp(out.row[0].started_at, "2025-02-03T04:05:07Z") == 0);
    assert(strcmp(out.row[0].direction, "outgoing") == 0 && out.row[0].answered == 1 && out.row[0].finished == 1);
    assert(strcmp(out.row[0].ended_at, "2025-02-03T04:06:00Z") == 0);
    assert(out.row[1].id == 7 && strlen(out.row[1].number) == CALL_PAGE_TEXT_LEN - 1);
    assert(strcmp(out.row[1].number, long_number) != 0 && strcmp(out.row[1].direction, "missed") == 0);
    assert(out.row[1].answered == 0 && out.row[1].finished == 1 && out.row[1].ended_at[0] == '\0');
    assert(call_paging_count(db, "sim1X", &total) == 0 && total == 1);
    sqlite3_close(db);
}

/** Создаёт 61 строку sim1 и отдельную строку sim2: проверяет exact count, newest-first/tie-break, sim1 offsets 0/30/60 дают 30/30/1, следующий offset пуст, а чужая SIM изолирована. */
static void test_order_page_boundaries_and_counts(void)
{
    sqlite3* db = open_db(); struct rows out = {0}; int total = 0, i;
    for (i = 1; i <= 62; ++i) insert_row(db, i, i == 62 ? "sim2" : "sim1", "+1", i <= 3 ? "2025-01-02" : "2025-01-03", "incoming", i % 2, 1, NULL);
    assert(call_paging_count(db, "sim1", &total) == 0 && total == 61);
    assert(call_paging_page(db, "sim1", CALL_PAGE_SIZE, 0, &out, collect) == 0 && out.n == 30);
    assert(out.row[0].id == 61 && out.row[1].id == 60 && out.row[29].id == 32);
    memset(&out, 0, sizeof(out));
    assert(call_paging_page(db, "sim1", CALL_PAGE_SIZE, 30, &out, collect) == 0 && out.n == 30);
    assert(out.row[0].id == 31 && out.row[27].id == 4 && out.row[28].id == 3 && out.row[29].id == 2);
    memset(&out, 0, sizeof(out));
    assert(call_paging_page(db, "sim1", CALL_PAGE_SIZE, 60, &out, collect) == 0 && out.n == 1 && out.row[0].id == 1);
    memset(&out, 0, sizeof(out));
    assert(call_paging_page(db, "sim1", CALL_PAGE_SIZE, 90, &out, collect) == 0 && out.n == 0);
    assert(call_paging_count(db, "sim2", &total) == 0 && total == 1);
    memset(&out, 0, sizeof(out));
    assert(call_paging_page(db, "sim2", CALL_PAGE_SIZE, 0, &out, collect) == 0 && out.n == 1 && out.row[0].id == 62);
    assert(call_paging_count(db, "unknown", &total) == 0 && total == 0);
    memset(&out, 0, sizeof(out));
    assert(call_paging_page(db, "unknown", CALL_PAGE_SIZE, 0, &out, collect) == 0 && out.n == 0);
    sqlite3_close(db);
}

/** Сценарий production-used page metadata: первые/последние/empty и out-of-range requested pages clamp к валидному offset; invalid inputs отвергаются без частичного контракта. */
static void test_page_meta_clamping_and_invalid_args(void)
{
    int page, pages, offset;
    page = -9; assert(web_util_page_meta(31, CALL_PAGE_SIZE, &page, &pages, &offset) == 0 && page == 1 && pages == 2 && offset == 0);
    page = 99; assert(web_util_page_meta(31, CALL_PAGE_SIZE, &page, &pages, &offset) == 0 && page == 2 && pages == 2 && offset == 30);
    page = 7; assert(web_util_page_meta(0, CALL_PAGE_SIZE, &page, &pages, &offset) == 0 && page == 1 && pages == 1 && offset == 0);
    page = 1; assert(web_util_page_meta(30, CALL_PAGE_SIZE, &page, &pages, &offset) == 0 && pages == 1 && offset == 0);
    assert(web_util_page_meta(-1, CALL_PAGE_SIZE, &page, &pages, &offset) == -1);
    assert(web_util_page_meta(1, 0, &page, &pages, &offset) == -1);
    assert(web_util_page_meta(1, CALL_PAGE_SIZE, NULL, &pages, &offset) == -1);
    assert(call_paging_count(NULL, "sim1", &pages) == -1 && call_paging_count(NULL, "sim1", NULL) == -1);
    assert(call_paging_page(NULL, "sim1", 30, 0, NULL, collect) == -1);
    sqlite3* db = open_db(); struct rows out = {0};
    assert(call_paging_page(db, NULL, 30, 0, &out, collect) == -1);
    assert(call_paging_page(db, "sim1", 0, 0, &out, collect) == -1);
    assert(call_paging_page(db, "sim1", -1, 0, &out, collect) == -1);
    assert(call_paging_page(db, "sim1", 30, -1, &out, collect) == -1);
    assert(call_paging_page(db, "sim1", 30, 0, &out, NULL) == -1);
    assert(call_paging_count(db, NULL, &pages) == -1);
    sqlite3_close(db);
}

/** После finish/restart recovery проверяет pager на lifecycle rows, созданных настоящими call_log production helpers, без ручного INSERT-представления статусов. */
static void test_lifecycle_rows_after_restart(void)
{
    sqlite3* db = NULL; char missed[CALL_LOG_ID_LEN], outgoing[CALL_LOG_ID_LEN], active[CALL_LOG_ID_LEN];
    struct rows out = {0}; int total = 0;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "CREATE TABLE mt_queue(device TEXT NOT NULL,mid TEXT PRIMARY KEY,from_num TEXT NOT NULL,msg TEXT NOT NULL,ts TEXT,state TEXT NOT NULL DEFAULT 'new',attempts INTEGER DEFAULT 0,next_retry_at INTEGER DEFAULT 0,last_result TEXT,created_at INTEGER DEFAULT 1,sent_at INTEGER,delivered_at INTEGER)", NULL, NULL, NULL) == SQLITE_OK);
    assert(call_log_schema_ensure(db) == 0);
    assert(call_log_start(db, "sim1", 1, CALL_LOG_INCOMING, "+111", "2025-01-01T00:00:00Z", missed, sizeof(missed)) == 0);
    assert(call_log_finish(db, missed, 0, "2025-01-01T00:01:00Z") == 0);
    assert(call_log_start(db, "sim1", 2, CALL_LOG_OUTGOING, "+222", "2025-01-02T00:00:00Z", outgoing, sizeof(outgoing)) == 0);
    assert(call_log_finish(db, outgoing, 0, "2025-01-02T00:01:00Z") == 0);
    assert(call_log_start(db, "sim1", 3, CALL_LOG_INCOMING, "+333", "2025-01-03T00:00:00Z", active, sizeof(active)) == 0);
    assert(call_paging_count(db, "sim1", &total) == 0 && total == 3);
    assert(call_paging_page(db, "sim1", CALL_PAGE_SIZE, 0, &out, collect) == 0 && out.n == 3);
    assert(out.row[0].id > out.row[1].id && strcmp(out.row[0].number, "+333") == 0 && out.row[0].finished == 0);
    assert(call_log_recover(db, "2025-01-04T00:00:00Z") == 0);
    memset(&out, 0, sizeof(out));
    assert(call_paging_page(db, "sim1", CALL_PAGE_SIZE, 0, &out, collect) == 0 && out.n == 3);
    assert(strcmp(out.row[0].direction, "missed") == 0 && out.row[0].answered == 0 && out.row[0].finished == 1);
    assert(strcmp(out.row[1].direction, "outgoing") == 0 && out.row[1].finished == 1);
    assert(strcmp(out.row[2].direction, "missed") == 0 && out.row[2].finished == 1);
    sqlite3_close(db);
}

/** Callback возвращает неноль после первой строки: pager должен немедленно прекратить обход и трактовать это как успешный early-stop, не как SQL failure. */
static void test_callback_early_stop(void)
{
    sqlite3* db = open_db(); struct rows out = {0}; out.stop_after = 1;
    insert_row(db, 1, "sim1", "a", "t1", "incoming", 0, 0, NULL);
    insert_row(db, 2, "sim1", "b", "t2", "outgoing", 1, 1, "end");
    assert(call_paging_page(db, "sim1", 30, 0, &out, collect) == 0 && out.n == 1);
    sqlite3_close(db);
}

/** Создаёт SQLite function с runtime error, чтобы statement успешно prepare-ился, но production pager получил настоящий sqlite3_step error; отдельно missing/malformed schema обязаны возвращать -1. */
static void test_schema_and_step_errors(void)
{
    sqlite3* db = NULL; struct rows out = {0}; int total = 0;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(call_paging_count(db, "sim1", &total) == -1);
    assert(call_paging_page(db, "sim1", 30, 0, &out, collect) == -1);
    assert(sqlite3_exec(db, "CREATE TABLE call_log(id,device,number,started_at,direction,answered,finished)", NULL, NULL, NULL) == SQLITE_OK);
    assert(call_paging_count(db, "sim1", &total) == 0 && total == 0);
    assert(call_paging_page(db, "sim1", 30, 0, &out, collect) == -1); /* ended_at missing */
    sqlite3_close(db);

    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(sqlite3_create_function(db, "test_fail", 0, SQLITE_UTF8, NULL, sqlite_step_failure, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db, "CREATE TABLE source(id,device,started_at,direction,answered,finished,ended_at); INSERT INTO source VALUES(1,'sim1','t','incoming',0,0,NULL); CREATE VIEW call_log AS SELECT id,device,test_fail() AS number,started_at,direction,answered,finished,ended_at FROM source", NULL, NULL, NULL) == SQLITE_OK);
    assert(call_paging_page(db, "sim1", 30, 0, &out, collect) == -1);
    sqlite3_close(db);
}

/** Контракт production error body: проверяется точный общий literal и его byte length, чтобы truncation/сдвиг HTTP Content-Length не скрывался отдельной копией константы. */
static void test_json_error_body_contract(void)
{
    const char expected[] = "{\"error\":\"call database unavailable\"}";
    assert(strcmp(CALL_WEB_JSON_ERROR, expected) == 0);
    assert(sizeof(CALL_WEB_JSON_ERROR) - 1 == 37);
    assert(strlen(CALL_WEB_JSON_ERROR) == sizeof(CALL_WEB_JSON_ERROR) - 1);
}

/** Запускает сценарии SQL pager, page metadata и общий production JSON error body. */
int main(void)
{
    test_json_error_body_contract();
    test_exact_device_and_full_payload();
    test_order_page_boundaries_and_counts();
    test_page_meta_clamping_and_invalid_args();
    test_callback_early_stop();
    test_lifecycle_rows_after_restart();
    test_schema_and_step_errors();
    puts("call-paging: OK");
    return 0;
}
