#define _GNU_SOURCE
#include "call_log_async.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/** Открыть тестовую SQLite и создать production schema до writer startup. */
static sqlite3* open_db(const char* path)
{
    sqlite3* db = NULL;
    assert(sqlite3_open(path, &db) == SQLITE_OK);
    assert(call_log_schema_ensure(db) == 0);
    assert(sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS mt_queue(device TEXT NOT NULL,mid TEXT PRIMARY KEY,from_num TEXT NOT NULL,msg TEXT NOT NULL,ts TEXT,state TEXT NOT NULL DEFAULT 'new',attempts INTEGER DEFAULT 0,next_retry_at INTEGER DEFAULT 0,last_result TEXT,created_at INTEGER DEFAULT 1,sent_at INTEGER,delivered_at INTEGER)", NULL, NULL, NULL) == SQLITE_OK);
    return db;
}

/** Получить значение поля для стабильного ID без обращения к async producer. */
static void assert_call(sqlite3* db, const char* call_id, const char* direction, int answered, int finished)
{
    sqlite3_stmt* stmt = NULL;
    long long id = 0;
    assert(sscanf(call_id, "call-%lld", &id) == 1 && id > 0);
    assert(sqlite3_prepare_v2(db, "SELECT direction,answered,finished FROM call_log WHERE id=?", -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, id);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 0), direction) == 0);
    assert(sqlite3_column_int(stmt, 1) == answered && sqlite3_column_int(stmt, 2) == finished);
    sqlite3_finalize(stmt);
}

/** Проверить FIFO order, incoming dedupe, answered status, missed+queue exactly-once и restart.
 * Операции копируются в producer queue, затем flush синхронизирует только test thread перед asserts.
 */
static void test_fifo_dedupe_answer_missed_and_restart(void)
{
    char path[] = "/tmp/ctq-call-async-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    sqlite3* db = open_db(path);
    call_log_async_t* q = call_log_async_start(db, 1);
    assert(q);
    char answered[CALL_LOG_ID_LEN], duplicate[CALL_LOG_ID_LEN], missed[CALL_LOG_ID_LEN];
    assert(call_log_async_start_call(q, "sim1", 4, CALL_LOG_INCOMING, "+700", "start-1", 0, answered, sizeof(answered)) == 0);
    assert(strcmp(answered, "call-1") == 0); /* обычная reserved ID выдаётся без коллизии */
    /* Ждать worker только в тесте, чтобы проверять дедуп, а не случайный trylock contention. */
    assert(call_log_async_flush(q) == 0);
    assert(call_log_async_start_call(q, "sim1", 4, CALL_LOG_INCOMING, "+700", "duplicate", 0, duplicate, sizeof(duplicate)) == 0);
    assert(strcmp(answered, duplicate) == 0);
    assert(call_log_async_answer(q, answered) == 0);
    assert(call_log_async_finish(q, answered, 0, "end-1") == 0);

    assert(call_log_async_start_call(q, "sim1", 4, CALL_LOG_INCOMING, "+701", "start-2", 0, missed, sizeof(missed)) == 0);
    assert(strcmp(missed, answered) != 0);
    assert(call_log_async_finish(q, missed, 0, "end-2") == 0);
    assert(call_log_async_flush(q) == 0);
    assert_call(db, answered, "incoming", 1, 1);
    assert_call(db, missed, "missed", 0, 1);
    sqlite3_stmt* stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM mt_queue", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    call_log_async_stop(&q);
    sqlite3_close(db);
    db = open_db(path);
    sqlite3_stmt* seed = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(id),0)+1 FROM call_log", -1, &seed, NULL) == SQLITE_OK);
    assert(sqlite3_step(seed) == SQLITE_ROW);
    const sqlite3_int64 next_id = sqlite3_column_int64(seed, 0);
    sqlite3_finalize(seed);
    q = call_log_async_start(db, next_id);
    assert(q);
    char outgoing[CALL_LOG_ID_LEN];
    assert(call_log_async_start_call(q, "sim2", -1, CALL_LOG_OUTGOING, "+702", "out start", 0, outgoing, sizeof(outgoing)) == 0);
    assert(call_log_async_finish(q, outgoing, 0, "out end") == 0);
    assert(call_log_async_flush(q) == 0);
    assert_call(db, outgoing, "outgoing", 0, 1);
    call_log_async_stop(&q);
    sqlite3_close(db);
    unlink(path);
}

/** Проверить быстрый enqueue при заблокированной SQLite и bounded queue failure policy.
 * Main thread держит connection mutex, поэтому writer не может выполнить DB insert;
 * producers должны возвращать быстро, а очередь отказывает без ожидания после заполнения/trylock.
 */
static void test_locked_database_nonblocking_and_overflow(void)
{
    sqlite3* db = open_db(":memory:");
    call_log_async_t* q = call_log_async_start(db, 1);
    assert(q);
    sqlite3_mutex* db_mutex = sqlite3_db_mutex(db);
    assert(db_mutex);
    sqlite3_mutex_enter(db_mutex);

    char id[CALL_LOG_ID_LEN];
    struct timespec before, after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    assert(call_log_async_start_call(q, "sim1", 1, CALL_LOG_INCOMING, "+700", "queued", 0, id, sizeof(id)) == 0);
    clock_gettime(CLOCK_MONOTONIC, &after);
    const long elapsed_ns = (after.tv_sec - before.tv_sec) * 1000000000L + after.tv_nsec - before.tv_nsec;
    assert(elapsed_ns < 100000000L);

    int saw_rejection = 0;
    const long overflow_start_ns = after.tv_sec * 1000000000L + after.tv_nsec;
    for (int i = 0; i < CALL_LOG_ASYNC_QUEUE_CAPACITY + 64; ++i) {
        char next[CALL_LOG_ID_LEN];
        if (call_log_async_start_call(q, "sim1", i + 10, CALL_LOG_OUTGOING, "+701", "overflow", 0, next, sizeof(next))) {
            saw_rejection = 1;
            break;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &after);
    const long rejected_elapsed_ns = after.tv_sec * 1000000000L + after.tv_nsec - overflow_start_ns;
    assert(saw_rejection && rejected_elapsed_ns < 100000000L);
    /* Пока writer заблокирован, queued START существует только в RAM; crash здесь
     * законно теряет его согласно принятому RAM FIFO контракту. */
    sqlite3_stmt* pending = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM call_log", -1, &pending, NULL) == SQLITE_OK);
    assert(sqlite3_step(pending) == SQLITE_ROW && sqlite3_column_int(pending, 0) == 0);
    sqlite3_finalize(pending);
    sqlite3_mutex_leave(db_mutex);
    assert(call_log_async_flush(q) == 0);
    call_log_async_stop(&q);
    sqlite3_close(db);
}

/** Проверить, что rejected FINISH при заполненной очереди не удаляет active correlation.
 * DB mutex удерживается тестом, поэтому writer блокируется на первом ANSWER и bounded FIFO
 * заполняется; после освобождения DB тот же вызов можно завершить штатно.
 */
static void test_finish_enqueue_overflow_keeps_active_call(void)
{
    sqlite3* db = open_db(":memory:");
    call_log_async_t* q = call_log_async_start(db, 1);
    assert(q);
    char id[CALL_LOG_ID_LEN], active[CALL_LOG_ID_LEN];
    assert(call_log_async_start_call(q, "sim1", 12, CALL_LOG_INCOMING, "+712", "start", 0,
                                     id, sizeof(id)) == 0);
    assert(call_log_async_flush(q) == 0);
    sqlite3_mutex* db_mutex = sqlite3_db_mutex(db);
    assert(db_mutex);
    sqlite3_mutex_enter(db_mutex);
    assert(call_log_async_answer(q, id) == 0);
    int saw_rejection = 0;
    for (int i = 0; i < CALL_LOG_ASYNC_QUEUE_CAPACITY + 16; ++i) {
        if (call_log_async_answer(q, id)) { saw_rejection = 1; break; }
    }
    assert(saw_rejection);
    const int finish_rc = call_log_async_finish(q, id, 0, "finish during pressure");
    assert(finish_rc == 0 || finish_rc == -1);
    /* Независимо от enqueue результата вызов остаётся доступен до DB completion;
     * повторить lookup только при штатном trylock contention самого producer API. */
    int lookup_rc = -1;
    for (int i = 0; i < 10000 && lookup_rc == -1; ++i) {
        lookup_rc = call_log_async_active_id(q, "sim1", 12, active, sizeof(active));
        if (lookup_rc == -1) sched_yield();
    }
    assert(lookup_rc == 1);
    assert(strcmp(active, id) == 0);
    sqlite3_mutex_leave(db_mutex);
    assert(call_log_async_flush(q) == 0);
    if (finish_rc == -1) {
        assert(call_log_async_active_id(q, "sim1", 12, active, sizeof(active)) == 1);
        assert(call_log_async_finish(q, id, 0, "finish retry") == 0);
        assert(call_log_async_flush(q) == 0);
    }
    assert_call(db, id, "incoming", 1, 1);
    assert(call_log_async_active_id(q, "sim1", 12, active, sizeof(active)) == 0);
    call_log_async_stop(&q);
    sqlite3_close(db);
}

/** Проверить SQLite error без остановки writer и успешное следующее событие.
 * Таблица удаляется после старта queue: первый START ошибочен; восстановленная schema
 * и следующий START должны обработаться FIFO, не завершая поток.
 */
static void test_database_error_does_not_kill_writer(void)
{
    sqlite3* db = open_db(":memory:");
    call_log_async_t* q = call_log_async_start(db, 1);
    assert(q);
    assert(sqlite3_exec(db, "DROP TABLE call_log", NULL, NULL, NULL) == SQLITE_OK);
    char failed[CALL_LOG_ID_LEN];
    assert(call_log_async_start_call(q, "sim1", 1, CALL_LOG_INCOMING, "+700", "db error", 0, failed, sizeof(failed)) == 0);
    assert(call_log_async_flush(q) == 0);
    char stale_id[CALL_LOG_ID_LEN];
    assert(call_log_async_active_id(q, "sim1", 1, stale_id, sizeof(stale_id)) == 0);
    assert(call_log_schema_ensure(db) == 0);
    char later[CALL_LOG_ID_LEN];
    assert(call_log_async_start_call(q, "sim1", 2, CALL_LOG_OUTGOING, "+701", "after error", 0, later, sizeof(later)) == 0);
    assert(call_log_async_flush(q) == 0);
    assert_call(db, later, "outgoing", 0, 0);
    call_log_async_stop(&q);
    sqlite3_close(db);
}

/** Один producer-thread повторяет transient queue contention до принятия всех событий. */
typedef struct producer_args {
    call_log_async_t* queue;
    int producer;
} producer_args_t;

static void* concurrent_producer(void* arg)
{
    producer_args_t* args = arg;
    for (int i = 0; i < 25; ++i) {
        char number[32], call_id[CALL_LOG_ID_LEN];
        snprintf(number, sizeof(number), "+%d%02d", args->producer, i);
        while (call_log_async_start_call(args->queue, "sim-concurrent", -1, CALL_LOG_OUTGOING,
                                         number, "concurrent", 0, call_id, sizeof(call_id))) {
            sched_yield();
        }
    }
    return NULL;
}

/** Проверить четыре producer-а, unique reserved IDs и serial SQLite insert order. */
static void test_concurrent_producers_are_serialized(void)
{
    sqlite3* db = open_db(":memory:");
    call_log_async_t* q = call_log_async_start(db, 1);
    assert(q);
    pthread_t threads[4];
    producer_args_t args[4];
    for (int i = 0; i < 4; ++i) {
        args[i].queue = q;
        args[i].producer = i;
        assert(pthread_create(&threads[i], NULL, concurrent_producer, &args[i]) == 0);
    }
    for (int i = 0; i < 4; ++i) pthread_join(threads[i], NULL);
    assert(call_log_async_flush(q) == 0);
    sqlite3_stmt* stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*),min(id),max(id) FROM call_log", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 100 && sqlite3_column_int64(stmt, 1) == 1 && sqlite3_column_int64(stmt, 2) == 100);
    sqlite3_finalize(stmt);
    call_log_async_stop(&q);
    sqlite3_close(db);
}

/** Проверить переиспользование индекса новым поколением и активный lookup по newest ID. */
static void test_reused_index_new_generation_map(void)
{
    sqlite3* db = open_db(":memory:");
    call_log_async_t* q = call_log_async_start(db, 1);
    assert(q);
    char first[CALL_LOG_ID_LEN], second[CALL_LOG_ID_LEN], active[CALL_LOG_ID_LEN];
    assert(call_log_async_start_call(q, "sim1", 7, CALL_LOG_INCOMING, "+700", "first", 0, first, sizeof(first)) == 0);
    assert(call_log_async_flush(q) == 0);
    assert(call_log_async_start_call(q, "sim1", 7, CALL_LOG_INCOMING, "+701", "second", 1, second, sizeof(second)) == 0);
    assert(strcmp(first, second) != 0);
    assert(call_log_async_active_id(q, "sim1", 7, active, sizeof(active)) == 1);
    assert(strcmp(active, second) == 0);
    assert(call_log_async_finish(q, first, 0, "first end") == 0);
    assert(call_log_async_flush(q) == 0);
    assert(call_log_async_active_id(q, "sim1", 7, active, sizeof(active)) == 1);
    assert(strcmp(active, second) == 0);
    assert(call_log_async_flush(q) == 0);
    assert_call(db, first, "missed", 0, 1);
    assert_call(db, second, "incoming", 0, 0);
    assert(call_log_async_finish(q, second, 0, "second end") == 0);
    assert(call_log_async_flush(q) == 0);
    assert(call_log_async_active_id(q, "sim1", 7, active, sizeof(active)) == 0);
    assert_call(db, second, "missed", 0, 1);
    call_log_async_stop(&q);
    sqlite3_close(db);
}

/**
 * Зафиксировать rowid allocator collision после удаления последней строки и restart.
 * AUTOINCREMENT sequence остаётся выше MAX(id); production seed обязан учитывать оба значения,
 * чтобы старый внешний call-<rowid> не адресовал новое поколение.
 */
static void test_restart_allocator_does_not_reuse_deleted_rowid(void)
{
    char path[] = "/tmp/ctq-call-id-reuse-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    sqlite3* db = open_db(path);
    assert(sqlite3_exec(db,
        "INSERT INTO call_log(id,device,number,started_at,direction,call_idx) VALUES(1,'sim1','+1','t1','incoming',1);"
        "INSERT INTO call_log(id,device,number,started_at,direction,call_idx) VALUES(2,'sim1','+2','t2','incoming',2);"
        "DELETE FROM call_log WHERE id=2;", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_stmt* stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT seq FROM sqlite_sequence WHERE name='call_log'", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int64(stmt, 0) == 2);
    sqlite3_finalize(stmt);
    /* Невозможность прочитать sequence/table обязана вернуть ошибку, не выдавая seed. */
    sqlite3_int64 unsafe_seed = 77;
    assert(call_log_async_seed_id(NULL, &unsafe_seed) == -1 && unsafe_seed == 77);
    sqlite3* broken_db = NULL;
    assert(sqlite3_open(":memory:", &broken_db) == SQLITE_OK);
    assert(call_log_async_seed_id(broken_db, &unsafe_seed) == -1 && unsafe_seed == 77);
    /* Схема без AUTOINCREMENT не создаёт sqlite_sequence: не разрешать потенциально
     * коллизионный fallback seed даже при существующей таблице call_log. */
    assert(sqlite3_exec(broken_db, "CREATE TABLE call_log(id INTEGER PRIMARY KEY, device TEXT, number TEXT, started_at TEXT, direction TEXT, call_idx INTEGER)", NULL, NULL, NULL) == SQLITE_OK);
    assert(call_log_async_seed_id(broken_db, &unsafe_seed) == -1 && unsafe_seed == 77);
    sqlite3_close(broken_db);
    /* Обратная сторона: повреждённый/устаревший sequence ниже MAX(id) не должен
     * дать повторный ID даже при сохранённой строке с более высоким rowid. */
    assert(sqlite3_exec(db, "INSERT INTO call_log(id,device,number,started_at,direction,call_idx) VALUES(9,'sim1','+9','t9','incoming',9); UPDATE sqlite_sequence SET seq=1 WHERE name='call_log';", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_int64 max_seed = 0;
    assert(call_log_async_seed_id(db, &max_seed) == 0 && max_seed == 10);
    sqlite3_close(db);
    /* Настоящее reconnect моделирует restart; production и regression используют тот же seed helper. */
    db = open_db(path);
    sqlite3_int64 next_id = 0;
    assert(call_log_async_seed_id(db, &next_id) == 0);
    assert(next_id == 10);
    call_log_async_t* q = call_log_async_start(db, next_id);
    assert(q);
    char replacement[CALL_LOG_ID_LEN];
    assert(call_log_async_start_call(q, "sim1", 3, CALL_LOG_INCOMING, "+3", "after restart", 0,
                                     replacement, sizeof(replacement)) == 0);
    assert(strcmp(replacement, "call-10") == 0);
    assert(call_log_async_flush(q) == 0);
    assert_call(db, replacement, "incoming", 0, 0);
    call_log_async_stop(&q);
    sqlite3_close(db);
    unlink(path);
}

/**
 * Зафиксировать потерю принятого FINISH при SQLite write error.
 * FINISH принимается до write failure: correlation остаётся доступна, повторный вызов
 * получает отдельное поколение, а recovery завершает только старую row без второго notice.
 */
static void test_finish_db_error_remains_recoverable(void)
{
    sqlite3* db = open_db(":memory:");
    call_log_async_t* q = call_log_async_start(db, 1);
    assert(q);
    char id[CALL_LOG_ID_LEN], active[CALL_LOG_ID_LEN];
    assert(call_log_async_start_call(q, "sim1", 9, CALL_LOG_INCOMING, "+700", "start", 0, id, sizeof(id)) == 0);
    assert(call_log_async_flush(q) == 0);
    assert(sqlite3_exec(db, "CREATE TRIGGER reject_finish BEFORE UPDATE OF finished ON call_log BEGIN SELECT RAISE(ABORT,'injected finish failure'); END", NULL, NULL, NULL) == SQLITE_OK);

    /* Заблокировать writer на SQLite, чтобы проверить pending FINISH до commit/failure. */
    sqlite3_mutex* db_mutex = sqlite3_db_mutex(db);
    assert(db_mutex);
    sqlite3_mutex_enter(db_mutex);
    assert(call_log_async_finish(q, id, 0, "finish fails") == 0);
    assert(call_log_async_active_id(q, "sim1", 9, active, sizeof(active)) == 1);
    assert(strcmp(active, id) == 0);
    char successor[CALL_LOG_ID_LEN];
    assert(call_log_async_start_call(q, "sim1", 9, CALL_LOG_INCOMING, "+701", "pending successor", 0,
                                     successor, sizeof(successor)) == 0);
    assert(strcmp(successor, id) != 0); /* pending FINISH не дедуплицирует новое поколение */
    sqlite3_mutex_leave(db_mutex);
    assert(call_log_async_flush(q) == 0);

    /* Ошибка SQLite не удаляет старую correlation и не ломает новую generation. */
    assert(call_log_async_active_id(q, "sim1", 9, active, sizeof(active)) == 1);
    assert(strcmp(active, successor) == 0); /* lookup предпочитает newest generation */
    assert(sqlite3_exec(db, "DROP TRIGGER reject_finish", NULL, NULL, NULL) == SQLITE_OK);
    assert(call_log_async_finish(q, id, 0, "retry after recovery") == 0);
    assert(call_log_async_flush(q) == 0);
    assert_call(db, id, "missed", 0, 1);
    assert_call(db, successor, "incoming", 0, 0);
    assert(call_log_async_active_id(q, "sim1", 9, active, sizeof(active)) == 1);
    assert(strcmp(active, successor) == 0);

    /* Успешный повторный FINISH для уже закрытой строки идемпотентен: notice ровно один. */
    assert(call_log_async_finish(q, id, 0, "duplicate finish") == 0);
    assert(call_log_async_flush(q) == 0);
    sqlite3_stmt* count = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM mt_queue", -1, &count, NULL) == SQLITE_OK);
    assert(sqlite3_step(count) == SQLITE_ROW && sqlite3_column_int(count, 0) == 1);
    sqlite3_finalize(count);
    assert(call_log_async_finish(q, successor, 0, "successor finish") == 0);
    assert(call_log_async_flush(q) == 0);
    assert(call_log_async_active_id(q, "sim1", 9, active, sizeof(active)) == 0);
    assert_call(db, successor, "missed", 0, 1);

    /* После successful commit тот же modem index снова может создать обычное новое
     * поколение; dedupe не возвращает уже закрытый stable ID. */
    char after_success[CALL_LOG_ID_LEN];
    assert(call_log_async_start_call(q, "sim1", 9, CALL_LOG_INCOMING, "+702", "after success", 0,
                                     after_success, sizeof(after_success)) == 0);
    assert(strcmp(after_success, successor) != 0);
    assert(call_log_async_active_id(q, "sim1", 9, active, sizeof(active)) == 1);
    assert(strcmp(active, after_success) == 0);
    assert(call_log_async_finish(q, after_success, 1, "answered finish") == 0);
    assert(call_log_async_flush(q) == 0);
    assert(call_log_async_active_id(q, "sim1", 9, active, sizeof(active)) == 0);
    assert_call(db, after_success, "incoming", 1, 1);
    sqlite3_stmt* final_count = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM mt_queue", -1, &final_count, NULL) == SQLITE_OK);
    assert(sqlite3_step(final_count) == SQLITE_ROW && sqlite3_column_int(final_count, 0) == 2); /* Только два missed; answered не добавил notice. */
    sqlite3_finalize(final_count);
    call_log_async_stop(&q);
    sqlite3_close(db);
}

/** Запустить queue lifecycle, nonblocking/overflow, concurrency и DB-error regressions. */
int main(int argc, char** argv)
{
    if (argc == 2 && strcmp(argv[1], "rowid-reuse") == 0) {
        test_restart_allocator_does_not_reuse_deleted_rowid();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "finish-db-error") == 0) {
        test_finish_db_error_remains_recoverable();
        return 0;
    }
    test_fifo_dedupe_answer_missed_and_restart();
    test_locked_database_nonblocking_and_overflow();
    test_finish_enqueue_overflow_keeps_active_call();
    test_concurrent_producers_are_serialized();
    test_database_error_does_not_kill_writer();
    test_reused_index_new_generation_map();
    test_restart_allocator_does_not_reuse_deleted_rowid();
    test_finish_db_error_remains_recoverable();
    puts("call-log-async: all checks passed");
    return 0;
}
