#include "call_log.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Создать временную in-memory схему очереди и журнала для проверки production SQL.
 * Тестовые callbacks не изображают модем/Asterisk: проверяются реальные транзакционные функции.
 */
static sqlite3* test_db(void)
{
    sqlite3* db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "CREATE TABLE mt_queue(device TEXT NOT NULL,mid TEXT PRIMARY KEY,from_num TEXT NOT NULL,msg TEXT NOT NULL,ts TEXT,state TEXT NOT NULL DEFAULT 'new',attempts INTEGER DEFAULT 0,next_retry_at INTEGER DEFAULT 0,last_result TEXT,created_at INTEGER DEFAULT 1,sent_at INTEGER,delivered_at INTEGER)", NULL, NULL, NULL) == SQLITE_OK);
    assert(call_log_schema_ensure(db) == 0);
    assert(call_log_schema_ensure(db) == 0);
    return db;
}

/** Сценарий: исходящий завершился ошибкой, входящий answered, пропущенный повторно завершён.
 * Настройка — SQLite в памяти; ожидается три строки верного типа и ровно одна MT row
 * для caller-abandon. Предотвращает ошибочное уведомление об исходящем/answered вызове.
 */
static void test_directions_and_idempotent_missed(void)
{
    sqlite3* db = test_db();
    char out[CALL_LOG_ID_LEN], answered[CALL_LOG_ID_LEN], missed[CALL_LOG_ID_LEN];
    assert(call_log_start(db, "sim1", -1, CALL_LOG_OUTGOING, "+700", "2025-01-01 10:00:00 UTC", out, sizeof(out)) == 0);
    assert(call_log_finish(db, out, 0, "2025-01-01 10:00:10 UTC") == 0);
    assert(call_log_start(db, "sim1", 4, CALL_LOG_INCOMING, "+711", "2025-01-01 11:00:00 UTC", answered, sizeof(answered)) == 0);
    assert(call_log_answer(db, answered) == 0);
    assert(call_log_finish(db, answered, 0, "2025-01-01 11:01:00 UTC") == 0);
    assert(call_log_start(db, "sim2", 4, CALL_LOG_INCOMING, "+722", "2025-01-01 12:00:00 UTC", missed, sizeof(missed)) == 0);
    char duplicate[CALL_LOG_ID_LEN];
    assert(call_log_start(db, "sim2", 4, CALL_LOG_INCOMING, "+722", "different", duplicate, sizeof(duplicate)) == 0);
    assert(strcmp(missed, duplicate) == 0);
    assert(call_log_finish(db, missed, 0, "2025-01-01 12:00:15 UTC") == 0);
    assert(call_log_finish(db, missed, 0, "2025-01-01 12:00:16 UTC") == 0);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT direction,device,number,started_at FROM call_log WHERE id=CAST(substr(?1,6) AS INTEGER)", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, missed, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(st, 0), "missed") == 0);
    assert(strcmp((const char*)sqlite3_column_text(st, 1), "sim2") == 0);
    assert(strcmp((const char*)sqlite3_column_text(st, 2), "+722") == 0);
    assert(strcmp((const char*)sqlite3_column_text(st, 3), "2025-01-01 12:00:00 UTC") == 0);
    sqlite3_finalize(st);
    assert(sqlite3_prepare_v2(db, "SELECT count(*),sum(direction='outgoing'),sum(direction='incoming'),sum(direction='missed') FROM call_log", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 3 && sqlite3_column_int(st, 1) == 1 && sqlite3_column_int(st, 2) == 1 && sqlite3_column_int(st, 3) == 1);
    sqlite3_finalize(st);
    assert(sqlite3_prepare_v2(db, "SELECT count(*),min(from_num),min(msg) FROM mt_queue", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 1);
    assert(strcmp((const char*)sqlite3_column_text(st, 1), "missed-call") == 0);
    assert(strstr((const char*)sqlite3_column_text(st, 2), "SIM sim2, номер +722, время начала 2025-01-01 12:00:00 UTC") != NULL);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

/** Сценарий: несовпадающее направление отбрасывается до durable start.
 * Существующая outgoing row занимает cpvt; incoming event с тем же индексом не должен
 * создать phantom incoming или пропущенное уведомление при восстановлении.
 */
static void test_direction_mismatch_has_no_phantom_row(void)
{
    sqlite3* db = test_db();
    char outgoing[CALL_LOG_ID_LEN];
    assert(call_log_start(db, "sim1", 6, CALL_LOG_OUTGOING, "+706", "outgoing", outgoing, sizeof(outgoing)) == 0);
    assert(!call_log_direction_matches(1u, 0u)); /* входящее событие против исходящего cpvt */
    if (call_log_direction_matches(1u, 0u)) {
        char phantom[CALL_LOG_ID_LEN];
        assert(call_log_start(db, "sim1", 6, CALL_LOG_INCOMING, "+799", "wrong event", phantom, sizeof(phantom)) == 0);
    }
    assert(call_log_recover(db, "recovery") == 0);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM call_log", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
    sqlite3_finalize(st);
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM mt_queue", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

/** Сценарий: DSCI RELEASED опоздал после reuse индекса, но свежий CLCC видит новый звонок.
 * Кандидат по текущему активному ID должен остаться incoming; регрессия — неверный missed.
 */
static void test_late_release_does_not_finish_reused_slot(void)
{
    sqlite3* db = test_db();
    char old_id[CALL_LOG_ID_LEN], current_id[CALL_LOG_ID_LEN], candidate[CALL_LOG_ID_LEN];
    assert(call_log_start(db, "sim1", 2, CALL_LOG_INCOMING, "+702", "old call", old_id, sizeof(old_id)) == 0);
    assert(call_log_finish(db, old_id, 0, "old release") == 0);
    assert(call_log_start(db, "sim1", 2, CALL_LOG_INCOMING, "+703", "new call", current_id, sizeof(current_id)) == 0);
    assert(call_log_active_id(db, "sim1", 2, candidate, sizeof(candidate)) == 1);
    assert(strcmp(candidate, current_id) == 0);
    unsigned int older = 1;
    assert(call_log_release_snapshot_decide(&older, 1, 1) == CALL_LOG_SNAPSHOT_OLDER && older == 0);
    assert(call_log_release_snapshot_decide(&older, 1, 1) == CALL_LOG_SNAPSHOT_PRESENT);
    assert(call_log_release_confirm(db, candidate, 1, "stale release verified present") == 0);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT direction,finished FROM call_log WHERE id=CAST(substr(?1,6) AS INTEGER)", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, current_id, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(st, 0), "incoming") == 0 && sqlite3_column_int(st, 1) == 0);
    sqlite3_finalize(st);
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM mt_queue", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

/** Проверить RELEASED(A) → DSCI ACTIVE(B) первым событием → busy snapshot.
 * Одна production decision классифицирует source/state/barrier; active-first создаёт B,
 * повторный DSCI использует successor, а обычный CLCC poll не создаёт новое поколение.
 * Busy snapshot обязан завершить A как missed и оставить B отдельной answered строкой.
 */
static void test_active_first_unsolicited_generation_before_snapshot(void)
{
    sqlite3* db = test_db();
    char old_id[CALL_LOG_ID_LEN], successor[CALL_LOG_ID_LEN], duplicate[CALL_LOG_ID_LEN];
    assert(call_log_start(db, "sim5", 14, CALL_LOG_INCOMING, "+714", "call A", old_id, sizeof(old_id)) == 0);
    /* RELEASED(A) фиксирует устойчивый ID; проверка ещё ожидает, слот пока занят новым B. */
    assert(call_log_incoming_generation_decide(CALL_LOG_EVENT_UNSOLICITED, 1, CALL_LOG_EVENT_STATE_ACTIVE, 1, 0) ==
           CALL_LOG_GENERATION_NEW);
    assert(call_log_start_new_generation(db, "sim5", 14, "+715", "call B", successor, sizeof(successor)) == 0);
    assert(strcmp(old_id, successor) != 0);
    /* Повторный ACTIVE DSCI адресуется B, а CLCC ACTIVE использует successor, не создавая C. */
    assert(call_log_incoming_generation_decide(CALL_LOG_EVENT_UNSOLICITED, 1, CALL_LOG_EVENT_STATE_ACTIVE, 0, 1) ==
           CALL_LOG_GENERATION_USE_SUCCESSOR);
    /* Если следующее поколение уже само RELEASED, его pending barrier важнее старого successor. */
    assert(call_log_incoming_generation_decide(CALL_LOG_EVENT_UNSOLICITED, 1, CALL_LOG_EVENT_STATE_ACTIVE, 1, 1) ==
           CALL_LOG_GENERATION_NEW);
    assert(call_log_incoming_generation_decide(CALL_LOG_EVENT_CLCC, 1, CALL_LOG_EVENT_STATE_ACTIVE, 0, 1) ==
           CALL_LOG_GENERATION_USE_SUCCESSOR);
    assert(call_log_start(db, "sim5", 14, CALL_LOG_INCOMING, "+715", "CLCC B", duplicate, sizeof(duplicate)) == 0);
    assert(strcmp(successor, duplicate) == 0);
    assert(call_log_answer(db, successor) == 0);
    /* Проверка видит индекс занятым: это B, но известный RELEASED(A) всё равно завершается. */
    unsigned int older = 0;
    assert(call_log_release_snapshot_decide(&older, 1, 1) == CALL_LOG_SNAPSHOT_PRESENT);
    assert(call_log_release_should_finish(1, 1));
    assert(!call_log_release_should_finish(1, 0));
    assert(call_log_release_should_finish(0, 0));
    assert(call_log_finish(db, old_id, 0, "A ended") == 0);
    assert(call_log_finish(db, old_id, 0, "duplicate A release") == 0);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT direction,answered,finished FROM call_log WHERE id=CAST(substr(?1,6) AS INTEGER)", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, old_id, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(st, 0), "missed") == 0 && sqlite3_column_int(st, 1) == 0 && sqlite3_column_int(st, 2) == 1);
    sqlite3_finalize(st);
    assert(sqlite3_prepare_v2(db, "SELECT direction,answered,finished FROM call_log WHERE id=CAST(substr(?1,6) AS INTEGER)", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, successor, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(st, 0), "incoming") == 0 && sqlite3_column_int(st, 1) == 1 && sqlite3_column_int(st, 2) == 0);
    sqlite3_finalize(st);
    assert(sqlite3_prepare_v2(db, "SELECT count(*),sum(direction='missed'),sum(direction='incoming') FROM call_log", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 2 &&
           sqlite3_column_int(st, 1) == 1 && sqlite3_column_int(st, 2) == 1);
    sqlite3_finalize(st);
    char active[CALL_LOG_ID_LEN];
    assert(call_log_active_id(db, "sim5", 14, active, sizeof(active)) == 1 && strcmp(active, successor) == 0);
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM mt_queue", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

/** Сценарий: у входящего без PBX приходит RELEASED, а последующий CLCC slot пуст.
 * Confirmed absence завершает исходную строку и создаёт ровно одну missed row.
 */
static void test_no_channel_release_confirmed_absent(void)
{
    sqlite3* db = test_db();
    char call_id[CALL_LOG_ID_LEN], candidate[CALL_LOG_ID_LEN];
    assert(call_log_start(db, "sim2", 9, CALL_LOG_INCOMING, "+709", "ringing", call_id, sizeof(call_id)) == 0);
    assert(call_log_active_id(db, "sim2", 9, candidate, sizeof(candidate)) == 1);
    unsigned int older = 0;
    assert(call_log_release_snapshot_decide(&older, 0, 1) == CALL_LOG_SNAPSHOT_ABSENT);
    assert(call_log_release_confirm(db, candidate, 0, "confirmed absent") == 0);
    assert(call_log_release_confirm(db, candidate, 0, "duplicate absent") == 0);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT direction,finished FROM call_log WHERE id=CAST(substr(?1,6) AS INTEGER)", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, call_id, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(st, 0), "missed") == 0 && sqlite3_column_int(st, 1) == 1);
    sqlite3_finalize(st);
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM mt_queue", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

/** Сценарий: malformed CLCC с последующим OK не считается пустым snapshot.
 * Пока snapshot неполон, устойчивый ID остаётся открытым и MT очередь не меняется;
 * следующий корректный полный пустой snapshot подтверждает caller-abandon ровно один раз.
 */
static void test_malformed_snapshot_retries_before_absence(void)
{
    sqlite3* db = test_db();
    char call_id[CALL_LOG_ID_LEN];
    assert(call_log_start(db, "sim4", 12, CALL_LOG_INCOMING, "+712", "ringing", call_id, sizeof(call_id)) == 0);
    unsigned int older = 0;
    /* Даже валидный занятый slot не спасает целостность всего повреждённого snapshot. */
    assert(call_log_release_snapshot_decide(&older, 1, 0) == CALL_LOG_SNAPSHOT_INVALID);
    unsigned int queued = 1, seen = 0;
    assert(call_log_release_snapshot_retry(&older, &queued, &seen) == 1 && queued == 0);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT direction,finished FROM call_log WHERE id=CAST(substr(?1,6) AS INTEGER)", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, call_id, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(st, 0), "incoming") == 0 && sqlite3_column_int(st, 1) == 0);
    sqlite3_finalize(st);
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM mt_queue", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0);
    sqlite3_finalize(st);
    assert(call_log_release_snapshot_decide(&older, 0, 1) == CALL_LOG_SNAPSHOT_ABSENT);
    assert(call_log_release_confirm(db, call_id, 0, "valid retry empty") == 0);
    assert(sqlite3_prepare_v2(db, "SELECT direction,finished FROM call_log WHERE id=CAST(substr(?1,6) AS INTEGER)", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, call_id, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(st, 0), "missed") == 0 && sqlite3_column_int(st, 1) == 1);
    sqlite3_finalize(st);
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM mt_queue", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

/** Проверить повтор при ошибочном CLCC до и после release barrier.
 * Ошибка старого ответа уменьшает barrier, не отменяя уже queued свежую проверку;
 * ERROR/TIMEOUT самой свежей проверки очищает состояние и просит taskproc повторить.
 */
static void test_release_snapshot_error_retry_cleanup(void)
{
    unsigned int older = 1, queued = 1, seen = 1;
    assert(call_log_release_snapshot_retry(&older, &queued, &seen) == 0);
    assert(older == 0 && queued == 1 && seen == 0);
    assert(call_log_release_snapshot_retry(&older, &queued, &seen) == 1);
    assert(older == 0 && queued == 0 && seen == 0);
    queued = 1;
    assert(call_log_release_snapshot_retry(&older, &queued, &seen) == 1);
    assert(queued == 0);
}

/** Сценарий: CLCC WAITING входящий классифицируется тем же incoming контрактом.
 * Два WAITING poll events должны вернуть одну открытую строку; регрессия — потерять
 * ожидающий вызов или выдать для него дубликат missed уведомления.
 */
static void test_waiting_is_incoming(void)
{
    sqlite3* db = test_db();
    char first[CALL_LOG_ID_LEN], repeated[CALL_LOG_ID_LEN];
    assert(call_log_start(db, "sim1", 5, CALL_LOG_INCOMING, "+705", "waiting start", first, sizeof(first)) == 0);
    assert(call_log_start(db, "sim1", 5, CALL_LOG_INCOMING, "+705", "waiting repeat", repeated, sizeof(repeated)) == 0);
    assert(strcmp(first, repeated) == 0);
    assert(call_log_finish(db, first, 0, "waiting release") == 0);
    sqlite3_close(db);
}

/** Сценарий: завершить один вызов и повторно занять прежний modem call_idx.
 * Ожидаются разные вечные IDs; предотвращает склейку нового поколения с прежним.
 */
static void test_reused_index_gets_new_id(void)
{
    sqlite3* db = test_db();
    char first[CALL_LOG_ID_LEN], second[CALL_LOG_ID_LEN];
    assert(call_log_start(db, "sim1", 2, CALL_LOG_INCOMING, "+701", "start1", first, sizeof(first)) == 0);
    assert(call_log_finish(db, first, 0, "end1") == 0);
    assert(call_log_start(db, "sim1", 2, CALL_LOG_INCOMING, "+702", "start2", second, sizeof(second)) == 0);
    assert(strcmp(first, second) != 0);
    sqlite3_close(db);
}

/** Сценарий: временный индекс изолирован по SIM и не заменяет устойчивый ID.
 * Одинаковые индексы двух устройств должны разрешаться в свои строки; привязка
 * outgoing к новому индексу сохраняет исходный call ID и не раскрывается как incoming.
 */
static void test_active_id_is_sim_scoped_and_index_is_temporary(void)
{
    sqlite3* db = test_db();
    char sim1_id[CALL_LOG_ID_LEN], sim2_id[CALL_LOG_ID_LEN], outgoing[CALL_LOG_ID_LEN], found[CALL_LOG_ID_LEN];
    assert(call_log_start(db, "sim1", 3, CALL_LOG_INCOMING, "+731", "start one", sim1_id, sizeof(sim1_id)) == 0);
    assert(call_log_start(db, "sim2", 3, CALL_LOG_INCOMING, "+732", "start two", sim2_id, sizeof(sim2_id)) == 0);
    assert(strcmp(sim1_id, sim2_id) != 0);
    assert(call_log_active_id(db, "sim1", 3, found, sizeof(found)) == 1 && strcmp(found, sim1_id) == 0);
    assert(call_log_active_id(db, "sim2", 3, found, sizeof(found)) == 1 && strcmp(found, sim2_id) == 0);
    assert(call_log_active_id(db, "sim3", 3, found, sizeof(found)) == 0);
    assert(call_log_start(db, "sim1", -1, CALL_LOG_OUTGOING, "+740", "outgoing", outgoing, sizeof(outgoing)) == 0);
    assert(call_log_set_index(db, outgoing, 3) == 0);
    assert(call_log_active_id(db, "sim1", 3, found, sizeof(found)) == 1 && strcmp(found, sim1_id) == 0);
    assert(call_log_answer(db, sim1_id) == 0);
    assert(call_log_finish(db, sim1_id, 0, "finish") == 0);
    assert(call_log_active_id(db, "sim1", 3, found, sizeof(found)) == 0);
    sqlite3_close(db);
}

/** Сценарий: восстановление после рестарта и отказ вставки очереди.
 * Вторая проверка ставит триггерную DB ошибку и требует полного rollback статуса missed;
 * предотвращает потерю уведомления либо частичное завершение вызова.
 */
static void test_restart_and_queue_failure_rollback(void)
{
    sqlite3* db = test_db();
    char call_id[CALL_LOG_ID_LEN];
    assert(call_log_start(db, "sim3", 8, CALL_LOG_INCOMING, "+703", "before restart", call_id, sizeof(call_id)) == 0);
    assert(call_log_recover(db, "restart") == 0);
    assert(call_log_recover(db, "restart again") == 0);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM mt_queue", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
    sqlite3_finalize(st);
    assert(call_log_start(db, "sim3", 9, CALL_LOG_INCOMING, "+704", "failed commit", call_id, sizeof(call_id)) == 0);
    assert(sqlite3_exec(db, "PRAGMA foreign_keys=ON; CREATE TABLE commit_parent(id INTEGER PRIMARY KEY); CREATE TABLE commit_child(id INTEGER REFERENCES commit_parent(id) DEFERRABLE INITIALLY DEFERRED); CREATE TRIGGER reject_missed AFTER INSERT ON mt_queue BEGIN INSERT INTO commit_child VALUES(99); END", NULL, NULL, NULL) == SQLITE_OK);
    assert(call_log_finish(db, call_id, 0, "end") == -1);
    assert(sqlite3_exec(db, "DROP TRIGGER reject_missed; DROP TABLE commit_child; DROP TABLE commit_parent", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_prepare_v2(db, "SELECT direction,finished FROM call_log WHERE id=CAST(substr(?1,6) AS INTEGER)", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, call_id, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(st, 0), "incoming") == 0);
    assert(sqlite3_column_int(st, 1) == 0);
    sqlite3_finalize(st);
    sqlite3_close(db);
}

/** Запустить SQLite тесты сценариев направления, дублей, поколения индекса и восстановления. */
int main(void)
{
    test_directions_and_idempotent_missed();
    test_direction_mismatch_has_no_phantom_row();
    test_late_release_does_not_finish_reused_slot();
    test_active_first_unsolicited_generation_before_snapshot();
    test_no_channel_release_confirmed_absent();
    test_malformed_snapshot_retries_before_absence();
    test_release_snapshot_error_retry_cleanup();
    test_waiting_is_incoming();
    test_reused_index_gets_new_id();
    test_active_id_is_sim_scoped_and_index_is_temporary();
    test_restart_and_queue_failure_rollback();
    puts("call-log: all checks passed");
    return 0;
}
