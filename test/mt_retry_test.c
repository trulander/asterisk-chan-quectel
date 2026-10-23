#include "mt_retry.h"

#include <assert.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/** Создать минимальную настоящую mt_queue схему для общего production picker SQL. */
static void create_queue(sqlite3* db)
{
    const char* schema = "CREATE TABLE mt_queue(device TEXT,mid TEXT PRIMARY KEY,from_num TEXT,msg TEXT,ts TEXT,state TEXT,"
                         "attempts INTEGER,next_retry_at INTEGER,last_result TEXT,created_at INTEGER,sent_at INTEGER,delivered_at INTEGER);"
                         "CREATE TABLE mt_out(mid TEXT,state TEXT);"
                         "INSERT INTO mt_queue VALUES('sim1','mid-1','+7995','text','ts','retrying',24,2000,'SIP 400',1,NULL,NULL);"
                         "INSERT INTO mt_out VALUES('mo-1','failed');";
    assert(sqlite3_exec(db, schema, NULL, NULL, NULL) == SQLITE_OK);
}

/** Выполнить ровно тот SQL picker, который использует smsdb_mt_pick_retry(). */
static int pick_due(sqlite3* db, int now, int max_attempts)
{
    sqlite3_stmt* stmt = NULL;
    assert(sqlite3_prepare_v2(db, MT_RETRY_PICK_SQL, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int(stmt, 1, now) == SQLITE_OK);
    assert(sqlite3_bind_int(stmt, 2, max_attempts) == SQLITE_OK);
    const int result = sqlite3_step(stmt);
    const int found = result == SQLITE_ROW;
    assert(found || result == SQLITE_DONE);
    if (found) assert(strcmp((const char*)sqlite3_column_text(stmt, 1), "mid-1") == 0);
    sqlite3_finalize(stmt);
    return found;
}

/** Персистировать тестовый callback plan в ту же структуру mt_queue. */
static void store_plan(sqlite3* db, const mt_retry_plan_t* plan, const char* result, int delivered_at)
{
    sqlite3_stmt* stmt = NULL;
    assert(sqlite3_prepare_v2(db, "UPDATE mt_queue SET state=?,attempts=?,next_retry_at=?,last_result=?,delivered_at=? WHERE mid='mid-1'", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(stmt, 1, plan->state, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_int(stmt, 2, plan->attempts) == SQLITE_OK);
    assert(sqlite3_bind_int(stmt, 3, plan->next_retry_at) == SQLITE_OK);
    assert(sqlite3_bind_text(stmt, 4, result, -1, SQLITE_TRANSIENT) == SQLITE_OK);
    if (delivered_at) assert(sqlite3_bind_int(stmt, 5, delivered_at) == SQLITE_OK);
    else assert(sqlite3_bind_null(stmt, 5) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1);
    sqlite3_finalize(stmt);
}

/** Проверить полный SIP 400 → due retry → online 200 → delivered путь через production policy/picker.
 * Файл SQLite закрывается и открывается между ошибкой и следующей попыткой, чтобы доказать,
 * что retry state переживает перезапуск; одинаковый MID доставляется и после 200 больше не due.
 */
static void test_timeout_retry_restart_then_success(void)
{
    char path[] = "/tmp/ctq-mt-retry-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);

    sqlite3* db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    create_queue(db);
    assert(!pick_due(db, 1999, 300));
    assert(pick_due(db, 2000, 300));

    mt_retry_plan_t plan;
    assert(mt_retry_plan_response(400, 24, 10000, 300, 30, 3600, 0, &plan) == 0);
    assert(plan.action == MT_RETRY_ACTION_RETRY && plan.attempts == 25);
    assert(plan.next_retry_at == 13600 && strcmp(plan.state, "retrying") == 0);
    store_plan(db, &plan, "SIP 400", 0);
    sqlite3_close(db);

    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(!pick_due(db, 13599, 300));
    assert(pick_due(db, 13600, 300));

    /* Локальная ошибка подготовки/From (status 0) тоже сохраняет retryable ту же MT row. */
    mt_retry_plan_t local_failure;
    assert(mt_retry_plan_response(0, 25, 13600, 300, 30, 3600, 0, &local_failure) == 0);
    assert(local_failure.action == MT_RETRY_ACTION_RETRY && local_failure.attempts == 26);
    assert(local_failure.next_retry_at == 17200);
    store_plan(db, &local_failure, "invalid MT From identity or domain", 0);
    assert(!pick_due(db, 17199, 300));
    assert(pick_due(db, 17200, 300));
    /* Ошибка не должна создавать/изменять mt_out — это иное направление доставки. */
    sqlite3_stmt* unchanged_mo = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT mid,state FROM mt_out", -1, &unchanged_mo, NULL) == SQLITE_OK);
    assert(sqlite3_step(unchanged_mo) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(unchanged_mo, 0), "mo-1") == 0);
    assert(strcmp((const char*)sqlite3_column_text(unchanged_mo, 1), "failed") == 0);
    sqlite3_finalize(unchanged_mo);

    /* В следующий due проход recipient уже online: его 200 меняет тот же MID в delivered. */
    assert(mt_retry_plan_response(200, local_failure.attempts, 17200, 300, 30, 3600, 0, &plan) == 0);
    assert(plan.action == MT_RETRY_ACTION_DELIVERED && plan.attempts == 26 && plan.next_retry_at == 0);
    store_plan(db, &plan, "200 delivered", 17200);
    assert(!pick_due(db, 17200, 300));

    sqlite3_stmt* stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT state,attempts,delivered_at FROM mt_queue WHERE mid='mid-1'", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 0), "delivered") == 0);
    assert(sqlite3_column_int(stmt, 1) == 26 && sqlite3_column_int(stmt, 2) == 17200);
    sqlite3_finalize(stmt);
    /* Picker и переходы не затрагивают направление mt_out (приложение→SIM). */
    assert(sqlite3_prepare_v2(db, "SELECT mid,state FROM mt_out", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 0), "mo-1") == 0);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 1), "failed") == 0);
    sqlite3_finalize(stmt);
    /* Даже due retry недоступен picker-у на достигнутом лимите; mt_out остаётся отдельно. */
    assert(sqlite3_exec(db, "UPDATE mt_queue SET state='retrying',attempts=300,next_retry_at=1 WHERE mid='mid-1'", NULL, NULL, NULL) == SQLITE_OK);
    assert(!pick_due(db, 999999, 300));
    sqlite3_close(db);
    unlink(path);
}

/** Проверить сохранение terminal accepted/delivered и конечный лимит попыток. */
static void test_response_classification_and_attempt_limit(void)
{
    mt_retry_plan_t plan;
    assert(mt_retry_plan_response(180, 3, 100, 10, 30, 3600, 0, &plan) == 0);
    assert(plan.action == MT_RETRY_ACTION_QUEUED && !strcmp(plan.state, "queued") && plan.attempts == 3);
    assert(mt_retry_plan_response(202, 3, 100, 10, 30, 3600, 0, &plan) == 0);
    assert(plan.action == MT_RETRY_ACTION_QUEUED);
    assert(mt_retry_plan_response(200, 3, 100, 10, 30, 3600, 0, &plan) == 0);
    assert(plan.action == MT_RETRY_ACTION_DELIVERED);
    assert(mt_retry_plan_response(408, 9, 100, 10, 30, 3600, 0, &plan) == 0);
    assert(plan.action == MT_RETRY_ACTION_EXHAUSTED && plan.attempts == 10 && plan.next_retry_at == 0);
    assert(mt_retry_plan_response(408, -1, 100, 10, 30, 3600, 0, &plan) == -1);
}

/** Проверить соседние SIP/transport ошибки, backoff cap+jitter и ненарастающие queued ответы. */
static void test_error_statuses_and_backoff_boundaries(void)
{
    mt_retry_plan_t plan;
    const int errors[] = {0, 302, 400, 408, 500};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        assert(mt_retry_plan_response(errors[i], 0, 100, 10, 30, 3600, 7, &plan) == 0);
        assert(plan.action == MT_RETRY_ACTION_RETRY && plan.attempts == 1);
        assert(plan.next_retry_at == 167); /* 30*2^1 + jitter 7 */
    }

    /* После exponential cap jitter всё ещё добавляется, а attempt остаётся ограничен. */
    assert(mt_retry_plan_response(503, 7, 1000, 20, 30, 1000, 10, &plan) == 0);
    assert(plan.action == MT_RETRY_ACTION_RETRY && plan.attempts == 8);
    assert(plan.next_retry_at == 2010);

    /* Accepted response не расходует retry budget и не задаёт due timestamp. */
    assert(mt_retry_plan_response(202, 9, 100, 10, 30, 3600, 10, &plan) == 0);
    assert(plan.action == MT_RETRY_ACTION_QUEUED && plan.attempts == 9 && plan.next_retry_at == 0);
    assert(mt_retry_plan_response(200, 9, 100, 10, 30, 3600, 10, &plan) == 0);
    assert(plan.action == MT_RETRY_ACTION_DELIVERED && plan.attempts == 9 && plan.next_retry_at == 0);

    /* Недопустимые арифметические параметры не публикуют retry plan. */
    assert(mt_retry_plan_response(408, 0, -1, 10, 30, 3600, 0, &plan) == -1);
    assert(mt_retry_plan_response(408, 0, 100, 10, 30, 3600, -1, &plan) == -1);
}

/** Запустить MT retry policy и SQLite restart regressions. */
int main(void)
{
    test_timeout_retry_restart_then_success();
    test_response_classification_and_attempt_limit();
    test_error_statuses_and_backoff_boundaries();
    puts("mt-retry: all checks passed");
    return 0;
}
