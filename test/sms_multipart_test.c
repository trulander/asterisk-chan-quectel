#include <stdio.h>
#include <string.h>
#include <sqlite3.h>

#include "sms_multipart.h"
#include "sms_incoming_schema.h"
#include "sms_tx.h"

static int failures;
/* CHECK накапливает fixture/assertion ошибки и позволяет завершить все сценарии. */
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); ++failures; } \
} while (0)

/** Создать изолированную SQLite БД для проверки production schema helpers без Asterisk. */
static sqlite3* open_db(void)
{
    sqlite3* db = NULL;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) return NULL;
    const char* schema =
        "CREATE TABLE mt_queue (device TEXT NOT NULL, mid TEXT PRIMARY KEY, from_num TEXT NOT NULL, msg TEXT NOT NULL, ts TEXT, state TEXT NOT NULL DEFAULT 'new');"
        "CREATE TABLE parent(id INTEGER PRIMARY KEY);"
        "CREATE TABLE child(x INTEGER REFERENCES parent(id));"
        "PRAGMA foreign_keys=ON;";
    if (sms_incoming_schema_ensure(db) || sqlite3_exec(db, schema, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

/** Выполнить scalar COUNT-проверку результата в тестовой БД; -1 обозначает SQL error. */
static int count(sqlite3* db, const char* sql)
{
    sqlite3_stmt* stmt = NULL;
    int result = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) result = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

/** Создать очередь с указанным сообщением для проверки atomic queue/rollback. */
static int insert_queue(sqlite3* db, const char* msg)
{
    return sms_mt_queue_put(db, "sim-a", "one-sms", "+100", msg, "2026-01-01 00:00:00");
}

/** Проверить переход старой incoming_msg схемы к новой и безопасные defaults старых строк. */
static void test_legacy_schema_migration(void)
{
    sqlite3* db = NULL;
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK, "open legacy DB");
    CHECK(sqlite3_exec(db, "CREATE TABLE incoming_msg(key TEXT, seqorder INTEGER, expiration INTEGER, message TEXT, PRIMARY KEY(key,seqorder))",
                       NULL, NULL, NULL) == SQLITE_OK, "create legacy incoming_msg schema");
    CHECK(sms_incoming_schema_ensure(db) == 0, "migrate legacy incoming_msg schema");
    CHECK(sms_incoming_schema_ensure(db) == 0, "migration is idempotent");
    CHECK(count(db, "SELECT COUNT(*) FROM pragma_table_info('incoming_msg') WHERE name IN ('source_device','source_storage','source_index','delete_status','delete_attempts','retry_at','mt_queued','mt_mid')") == 8,
          "migration adds durable SIM deletion columns");
    sqlite3_close(db);
}

/** Регрессия повторного использования SIM index: update должен затронуть только точный rowid/identity. */
static void test_delete_state_update_is_rowid_scoped(void)
{
    sqlite3* db = open_db();
    CHECK(sqlite3_exec(db, "INSERT INTO incoming_msg(key,seqorder,message,source_device,source_storage,source_index) VALUES"
                          "('old-message',1,'old','sim-a','SM',7),('new-message',1,'new','sim-a','SM',7)",
                       NULL, NULL, NULL) == SQLITE_OK, "insert reused SIM index rows");
    sqlite3_int64 old_rowid = 0;
    sqlite3_int64 new_rowid = 0;
    sqlite3_stmt* stmt = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT rowid FROM incoming_msg ORDER BY key DESC", -1, &stmt, NULL) == SQLITE_OK, "prepare reused-index rowids");
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) old_rowid = sqlite3_column_int64(stmt, 0);
    if (stmt && sqlite3_step(stmt) == SQLITE_ROW) new_rowid = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    CHECK(old_rowid > 0 && new_rowid > 0 && old_rowid != new_rowid, "two messages have distinct rowids");
    CHECK(sms_incoming_delete_state_update(db, old_rowid, "sim-a", "SM", 7, 2, 1, 0) == 0, "update selected SIM cell row");
    CHECK(count(db, "SELECT COUNT(*) FROM incoming_msg WHERE delete_status=2 AND key='old-message'") == 1, "selected row updated");
    CHECK(count(db, "SELECT COUNT(*) FROM incoming_msg WHERE delete_status=0 AND key='new-message'") == 1, "reused slot row unchanged");
    CHECK(sms_incoming_delete_state_update(db, old_rowid, "sim-b", "SM", 7, 2, 1, 0) != 0, "source identity mismatch rejected");
    sqlite3_close(db);
}

/** Сохранить multipart в порядке 1,3,2; до третьего сегмента нет сборки, итоговый текст упорядочен. */
static void test_out_of_order_1_3_2(void)
{
    sqlite3* db = open_db();
    CHECK(db != NULL, "open in-memory DB");
    if (!db) return;

    char* assembled = NULL;
    long long rowid;
    int begin = sms_tx_begin(db);
    int parts = sms_multipart_put(db, "sim-a/+100/7/3", 1, 3, 3600, "first-", "sim-a", "SM", 4, 1, &assembled, &rowid);
    CHECK(parts == 1 && assembled == NULL, "first part remains incomplete");
    CHECK(sms_tx_commit(db, begin) == SQLITE_OK, "part 1 commit");
    CHECK(count(db, "SELECT COUNT(*) FROM mt_queue") == 0, "no delivery after first part");

    begin = sms_tx_begin(db);
    parts = sms_multipart_put(db, "sim-a/+100/7/3", 3, 3, 3600, "third", "sim-a", "SM", 6, 1, &assembled, &rowid);
    CHECK(parts == 2 && assembled == NULL, "parts 1,3 remain incomplete under count condition");
    CHECK(sms_tx_commit(db, begin) == SQLITE_OK, "part 3 commit");
    CHECK(count(db, "SELECT COUNT(*) FROM mt_queue") == 0, "no delivery before final missing part");

    begin = sms_tx_begin(db);
    parts = sms_multipart_put(db, "sim-a/+100/7/3", 2, 3, 3600, "second-", "sim-a", "SM", 5, 1, &assembled, &rowid);
    CHECK(parts == 3, "third stored part completes the count");
    CHECK(assembled && strcmp(assembled, "first-second-third") == 0, "parts assembled by seqorder");
    CHECK(count(db, "SELECT COUNT(*) FROM mt_queue") == 0, "helper does not queue an incomplete fragment");
    CHECK(insert_queue(db, assembled ? assembled : "") == 0, "one complete SMS inserted into MT queue");
    CHECK(sms_tx_commit(db, begin) == SQLITE_OK, "final part and MT queue commit");
    CHECK(count(db, "SELECT COUNT(*) FROM mt_queue") == 1, "exactly one MT queue row after complete SMS");
    CHECK(count(db, "SELECT COUNT(*) FROM incoming_msg") == 3, "SIM-backed part rows retained after assembly");
    CHECK(count(db, "SELECT COUNT(*) FROM incoming_msg WHERE source_storage='SM' AND source_index IN (4,5,6) AND delete_status=0 AND delete_attempts=0") == 3,
          "each retained part records its SIM storage/index and pending delete state");
    sqlite3_free(assembled);
    sqlite3_close(db);
}

/** Внести SQL/COMMIT ошибки и доказать, что они не дают ложного успешного queue/delete результата. */
static void test_database_and_commit_failures(void)
{
    sqlite3* db = open_db();
    CHECK(db != NULL, "open DB for failure tests");
    if (!db) return;

    sqlite3* broken_db = NULL;
    sqlite3_open(":memory:", &broken_db);
    char* assembled = NULL;
    CHECK(sms_multipart_put(broken_db, "missing-schema", 1, 1, 3600, "fragment", "sim-a", "SM", 9, 1, &assembled, NULL) < 0,
          "SQLite write failure is reported");
    CHECK(assembled == NULL && count(db, "SELECT COUNT(*) FROM mt_queue") == 0, "DB failure has no partial delivery");
    sqlite3_close(broken_db);

    sqlite3_exec(db, "PRAGMA defer_foreign_keys=ON", NULL, NULL, NULL);
    int begin = sms_tx_begin(db);
    CHECK(begin == SQLITE_OK, "begin forced COMMIT failure transaction");
    int inserted = sms_multipart_put(db, "commit-fail", 1, 1, 3600, "fragment", "sim-a", "SM", 10, 1, &assembled, NULL);
    CHECK(inserted == 1 && assembled && strcmp(assembled, "fragment") == 0, "part is staged before failed COMMIT");
    CHECK(sqlite3_exec(db, "INSERT INTO child(x) VALUES(99)", NULL, NULL, NULL) == SQLITE_OK, "stage deferred FK violation");
    CHECK(sms_tx_commit(db, begin) != SQLITE_OK, "COMMIT failure is observable");
    CHECK(count(db, "SELECT COUNT(*) FROM incoming_msg WHERE key='commit-fail'") == 0, "failed COMMIT rolls part back");
    CHECK(count(db, "SELECT COUNT(*) FROM mt_queue") == 0, "failed COMMIT cannot queue partial SMS");
    sqlite3_free(assembled);
    sqlite3_close(db);
}

/** Последовательно запустить migration, row identity, multipart order и DB failure regressions. */
int main(void)
{
    test_legacy_schema_migration();
    test_delete_state_update_is_rowid_scoped();
    test_out_of_order_1_3_2();
    test_database_and_commit_failures();
    if (failures) {
        fprintf(stderr, "%d sms multipart test(s) failed\n", failures);
        return 1;
    }
    puts("sms multipart tests passed");
    return 0;
}
