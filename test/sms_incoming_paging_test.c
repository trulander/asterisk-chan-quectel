#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include "sms_incoming_paging.h"

static int checks, failures;
/* CHECK считает каждый результат и продолжает, чтобы показать полный набор регрессий за запуск. */
#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

struct collected {
    struct sms_incoming_page_row rows[16]; /*!< Inline copies строк страницы; row не сохраняется из pager. */
    int count; /*!< Число собранных строк, не превышает capacity массива. */
};
/** Скопировать callback row в ограниченный массив; указатели pager-owned не сохраняются. */
static int collect(void *ctx, const struct sms_incoming_page_row *row)
{
    struct collected *out = ctx;
    if (out->count >= (int)(sizeof(out->rows) / sizeof(out->rows[0]))) return 1;
    out->rows[out->count++] = *row;
    return 0;
}
/** Проверить документированный callback-stop: первая строка фиксируется и обход прекращается. */
static int stop_after_one(void *ctx, const struct sms_incoming_page_row *row)
{
    struct collected *out = ctx;
    out->rows[out->count++] = *row;
    return 1;
}
/** Выполнить фиксированный fixture SQL и сообщить setup failure с SQLite ошибкой. */
static int exec_ok(sqlite3 *db, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) fprintf(stderr, "SQL failed (%d): %s\n", rc, err ? err : "");
    sqlite3_free(err);
    return rc == SQLITE_OK;
}
/** Обнулить коллекцию перед независимой page проверкой. */
static void reset(struct collected *out) { memset(out, 0, sizeof(*out)); }
/** Упростить тестовый вызов production pager и очистить накопитель перед ним. */
static int page(sqlite3 *db, const char *device, int size, int offset, struct collected *out)
{
    reset(out);
    return sms_incoming_paging_page(db, device, size, offset, out, collect);
}
/** SQLite authorizer для гарантированного read-denied SQL failure. */
static int deny_incoming_read(void *ctx, int action, const char *arg1,
                              const char *arg2, const char *database, const char *trigger)
{
    (void)ctx; (void)arg2; (void)database; (void)trigger;
    return action == SQLITE_READ && arg1 && strcmp(arg1, "incoming_msg") == 0
        ? SQLITE_DENY : SQLITE_OK;
}
/** Получить scalar query result для read-only/no-mutation regression assertion. */
static int scalar(sqlite3 *db, const char *sql, sqlite3_int64 *value)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) *value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW;
}

static const char *schema =
    "CREATE TABLE incoming_msg("
    "key TEXT,seqorder INTEGER,expiration INTEGER,message TEXT,"
    "source_device TEXT,source_storage TEXT,source_index INTEGER,"
    "delete_status INTEGER,delete_attempts INTEGER,retry_at INTEGER,"
    "mt_queued INTEGER,mt_mid TEXT)";

/** Создать fixture, проверить поля/filter/order/page/error/immutability, затем проверить schema errors. */
int main(void)
{
    sqlite3 *db = NULL;
    struct collected got;
    int total = -1, before_total_changes;
    sqlite3_int64 before_rows = -1, after_rows = -1;
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    CHECK(exec_ok(db, schema));
    /* Равные expiration/key/seqorder намеренно проверяют разрешение равенства через rowid. */
    CHECK(exec_ok(db,
        "INSERT INTO incoming_msg VALUES"
        "('g',2,20,'g2','sim-A','SM',2,3,4,500,1,'mid<&'),"
        "('g',1,20,'g1','sim-A','SM',1,2,3,400,0,NULL),"
        "('g',1,20,'g1-tie','sim-A','ME',9,1,0,0,1,'tie'),"
        "('a',7,20,'alpha','sim-B','SM',7,0,1,30,0,NULL),"
        "('z',1,10,'old','sim-A','SM',3,0,0,0,0,NULL),"
        "('q',1,30,'new','sim-B','SM',8,0,0,0,0,NULL)"));
    /* Вредоносоподобная строка фильтра вставляется bound value, а не SQL фрагментом. */
    {
        const char *hostile = "sim-\"quoted' OR 1=1 --\\\n";
        sqlite3_stmt *stmt = NULL;
        CHECK(sqlite3_prepare_v2(db, "INSERT INTO incoming_msg VALUES('control',1,5,'odd',?,'SM',0,NULL,NULL,NULL,NULL,NULL)", -1, &stmt, NULL) == SQLITE_OK);
        CHECK(sqlite3_bind_text(stmt, 1, hostile, -1, SQLITE_TRANSIENT) == SQLITE_OK);
        CHECK(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }

    /* Все устройства; порядок: expiration DESC, binary key ASC, seqorder ASC, rowid ASC. */
    CHECK(sms_incoming_paging_count(db, NULL, &total) == 0 && total == 7);
    CHECK(page(db, NULL, 20, 0, &got) == 0 && got.count == 7);
    CHECK(got.rows[0].message[0] == 'n');
    CHECK(strcmp(got.rows[1].key, "a") == 0 && strcmp(got.rows[2].key, "g") == 0);
    CHECK(got.rows[2].seqorder == 1 && strcmp(got.rows[2].message, "g1") == 0);
    CHECK(got.rows[3].seqorder == 1 && strcmp(got.rows[3].message, "g1-tie") == 0);
    CHECK(got.rows[4].seqorder == 2 && strcmp(got.rows[4].message, "g2") == 0);
    CHECK(strcmp(got.rows[5].message, "old") == 0);
    CHECK(strcmp(got.rows[6].message, "odd") == 0);
    CHECK(got.rows[2].rowid < got.rows[3].rowid);

    /* Проверить поля projection, включая преобразование nullable metadata в zero/empty. */
    CHECK(strcmp(got.rows[4].device, "sim-A") == 0);
    CHECK(strcmp(got.rows[4].storage, "SM") == 0 && got.rows[4].source_index == 2);
    CHECK(strcmp(got.rows[4].key, "g") == 0 && got.rows[4].expiration == 20);
    CHECK(got.rows[4].delete_status == 3 && got.rows[4].delete_attempts == 4);
    CHECK(got.rows[4].retry_at == 500 && got.rows[4].mt_queued == 1);
    CHECK(strcmp(got.rows[4].mt_mid, "mid<&") == 0);
    CHECK(got.rows[2].delete_status == 2 && got.rows[2].delete_attempts == 3);
    CHECK(got.rows[2].retry_at == 400 && got.rows[2].mt_queued == 0 && got.rows[2].mt_mid[0] == '\0');
    CHECK(got.rows[6].delete_status == 0 && got.rows[6].delete_attempts == 0);
    CHECK(got.rows[6].retry_at == 0 && got.rows[6].mt_queued == 0 && got.rows[6].mt_mid[0] == '\0');

    /* Проверить изоляцию устройства, включая кавычки, SQL-подобный ввод и control chars. */
    CHECK(sms_incoming_paging_count(db, "sim-A", &total) == 0 && total == 4);
    CHECK(page(db, "sim-A", 20, 0, &got) == 0 && got.count == 4);
    for (int i = 0; i < got.count; ++i) CHECK(strcmp(got.rows[i].device, "sim-A") == 0);
    CHECK(sms_incoming_paging_count(db, "sim-B", &total) == 0 && total == 2);
    CHECK(page(db, "sim-B", 20, 0, &got) == 0 && got.count == 2);
    CHECK(sms_incoming_paging_count(db, "sim-\"quoted' OR 1=1 --\\\n", &total) == 0 && total == 1);
    CHECK(page(db, "sim-\"quoted' OR 1=1 --\\\n", 10, 0, &got) == 0 && got.count == 1);
    CHECK(strcmp(got.rows[0].device, "sim-\"quoted' OR 1=1 --\\\n") == 0);
    CHECK(sms_incoming_paging_count(db, "sim-A' OR 1=1 --", &total) == 0 && total == 0);
    CHECK(page(db, "sim-A' OR 1=1 --", 10, 0, &got) == 0 && got.count == 0);
    /* Пустая строка имеет документированную семантику all devices. */
    CHECK(sms_incoming_paging_count(db, "", &total) == 0 && total == 7);

    /* Первая, средняя, последняя и beyond-end страницы с устойчивыми границами. */
    CHECK(page(db, NULL, 2, 0, &got) == 0 && got.count == 2 && got.rows[0].expiration == 30);
    CHECK(page(db, NULL, 2, 2, &got) == 0 && got.count == 2 && got.rows[0].seqorder == 1 && got.rows[1].seqorder == 1);
    CHECK(page(db, NULL, 2, 6, &got) == 0 && got.count == 1 && strcmp(got.rows[0].message, "odd") == 0);
    CHECK(page(db, NULL, 2, 7, &got) == 0 && got.count == 0);
    CHECK(page(db, NULL, 2, INT_MAX, &got) == 0 && got.count == 0);
    CHECK(page(db, NULL, INT_MAX, INT_MAX, &got) == 0 && got.count == 0);
    CHECK(page(db, NULL, 1, 0, &got) == 0 && got.count == 1);
    reset(&got);
    CHECK(sms_incoming_paging_page(db, NULL, 10, 0, &got, stop_after_one) == 0 && got.count == 1);

    /* Согласованность count/page и пустые filtered/device/table выборки. */
    CHECK(sms_incoming_paging_count(db, NULL, &total) == 0 && total == 7);
    int seen = 0;
    for (int offset = 0; offset < total; offset += 3) {
        CHECK(page(db, NULL, 3, offset, &got) == 0);
        seen += got.count;
    }
    CHECK(seen == total);
    CHECK(sms_incoming_paging_count(db, "missing-device", &total) == 0 && total == 0);
    CHECK(page(db, "missing-device", 10, 0, &got) == 0 && got.count == 0);
    CHECK(exec_ok(db, "DELETE FROM incoming_msg"));
    CHECK(sms_incoming_paging_count(db, NULL, &total) == 0 && total == 0);
    CHECK(page(db, NULL, 10, 0, &got) == 0 && got.count == 0);

    /* Проверить error contracts и отсутствие мутаций от pager операций. */
    CHECK(sms_incoming_paging_count(db, NULL, NULL) == -1);
    CHECK(sms_incoming_paging_page(NULL, NULL, 1, 0, &got, collect) == -1);
    CHECK(sms_incoming_paging_page(db, NULL, 1, 0, &got, NULL) == -1);
    CHECK(sms_incoming_paging_page(db, NULL, 0, 0, &got, collect) == -1);
    CHECK(sms_incoming_paging_page(db, NULL, -1, 0, &got, collect) == -1);
    CHECK(sms_incoming_paging_page(db, NULL, 1, -1, &got, collect) == -1);
    CHECK(scalar(db, "SELECT COUNT(*) FROM incoming_msg", &before_rows));
    before_total_changes = sqlite3_total_changes(db);
    CHECK(sms_incoming_paging_count(db, NULL, &total) == 0);
    CHECK(page(db, NULL, 10, 0, &got) == 0);
    CHECK(scalar(db, "SELECT COUNT(*) FROM incoming_msg", &after_rows));
    CHECK(before_rows == after_rows && sqlite3_total_changes(db) == before_total_changes);

    sqlite3_close(db);

    /* Отсутствующая/неполная schema и настоящие SQLite execution errors. */
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    CHECK(sms_incoming_paging_count(db, NULL, &total) == -1);
    CHECK(sms_incoming_paging_page(db, NULL, 10, 0, &got, collect) == -1);
    CHECK(exec_ok(db, "CREATE TABLE incoming_msg(key TEXT)"));
    CHECK(sms_incoming_paging_count(db, NULL, &total) == 0 && total == 0);
    CHECK(sms_incoming_paging_page(db, NULL, 10, 0, &got, collect) == -1);
    CHECK(exec_ok(db, "DROP TABLE incoming_msg; CREATE TABLE incoming_msg(key TEXT,seqorder INTEGER,expiration INTEGER,message TEXT,source_device TEXT,source_storage TEXT,source_index INTEGER,delete_status INTEGER,delete_attempts INTEGER,retry_at INTEGER,mt_queued INTEGER,mt_mid TEXT);"));
    CHECK(exec_ok(db, "CREATE TRIGGER deny_insert BEFORE INSERT ON incoming_msg BEGIN SELECT RAISE(FAIL,'denied'); END;"));
    CHECK(sqlite3_exec(db, "INSERT INTO incoming_msg(key) VALUES('x')", NULL, NULL, NULL) == SQLITE_CONSTRAINT);
    CHECK(sms_incoming_paging_count(db, NULL, &total) == 0 && total == 0);
    CHECK(page(db, NULL, 10, 0, &got) == 0 && got.count == 0);
    /* Запретить SQLite чтение через authorizer и проверить передачу SQL error наружу. */
    CHECK(sqlite3_set_authorizer(db, deny_incoming_read, NULL) == SQLITE_OK);
    CHECK(sms_incoming_paging_count(db, NULL, &total) == -1);
    CHECK(page(db, NULL, 10, 0, &got) == -1);
    CHECK(sqlite3_set_authorizer(db, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);

    printf("incoming paging: %d checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
