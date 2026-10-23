#include "call_log.h"
#include "sms_multipart.h"

#include <stdio.h>
#include <string.h>

/** Выполнить транзакционную SQL-команду без результата строк.
 * db остаётся владельцем соединения; 0 означает SQLITE_OK, иначе -1.
 */
static int exec_sql(sqlite3* db, const char* sql)
{
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

/** Проверить направление event до записи; параметры используют CALL_DIR значения драйвера.
 * Входящий равен 1, исходящий равен 0; это не call_log_direction_t mapping.
 */
int call_log_direction_matches(unsigned int event_direction, unsigned int cpvt_direction)
{
    return event_direction == cpvt_direction;
}

/** Отделить устаревший ответ от snapshot, пригодного для release correlation.
 * Счётчик caller-owned; older decrement идёт ровно один раз на уже поставленный ответ.
 */
call_log_snapshot_result_t call_log_release_snapshot_decide(unsigned int* older_snapshots, int slot_present, int snapshot_valid)
{
    if (older_snapshots && *older_snapshots) {
        --*older_snapshots;
        return CALL_LOG_SNAPSHOT_OLDER;
    }
    if (!snapshot_valid) return CALL_LOG_SNAPSHOT_INVALID;
    return slot_present ? CALL_LOG_SNAPSHOT_PRESENT : CALL_LOG_SNAPSHOT_ABSENT;
}

/** Сохранить release candidate и запросить повтор только если ошибся его свежий snapshot. */
int call_log_release_snapshot_retry(unsigned int* older_snapshots, unsigned int* verification_queued, unsigned int* slot_seen)
{
    if (older_snapshots && *older_snapshots) {
        --*older_snapshots;
    } else if (verification_queued) {
        *verification_queued = 0;
    }
    if (slot_seen) *slot_seen = 0;
    return (!older_snapshots || !*older_snapshots) && verification_queued && !*verification_queued;
}

/** Создать durable schema звонков и индекс открытых входящих.
 * CREATE IF NOT EXISTS допускает повторную загрузку модуля; соединением владеет smsdb.
 */
int call_log_schema_ensure(sqlite3* db)
{
    static const char schema[] =
        "CREATE TABLE IF NOT EXISTS call_log (id INTEGER PRIMARY KEY AUTOINCREMENT, device TEXT NOT NULL,"
        "number TEXT NOT NULL, started_at TEXT NOT NULL, direction TEXT NOT NULL CHECK(direction IN ('incoming','outgoing','missed')) ,"
        "call_idx INTEGER NOT NULL DEFAULT -1, answered INTEGER NOT NULL DEFAULT 0, finished INTEGER NOT NULL DEFAULT 0,"
        "ended_at TEXT)";
    if (!db || exec_sql(db, "BEGIN IMMEDIATE") || exec_sql(db, schema) ||
        exec_sql(db, "CREATE INDEX IF NOT EXISTS call_log_active_idx ON call_log(device,call_idx,finished)")) {
        if (db) exec_sql(db, "ROLLBACK");
        return -1;
    }
    if (exec_sql(db, "COMMIT")) { exec_sql(db, "ROLLBACK"); return -1; }
    return 0;
}

/** Создать запись или дедуплицировать CLCC повтор открытого входящего.
 * Локальная транзакция атомарно проверяет активность и вставляет; call_id выдаётся
 * только после COMMIT. ID имеет пространство call-<AUTOINCREMENT rowid> и не
 * зависит от краткоживущего modem call_idx.
 */
static int call_log_start_internal(sqlite3* db, const char* device, int call_idx, call_log_direction_t direction,
                                   const char* number, const char* timestamp, char* call_id, size_t call_id_len,
                                   int force_new_generation)
{
    if (!db || !device || !call_id || !call_id_len || !timestamp || (direction != CALL_LOG_INCOMING && direction != CALL_LOG_OUTGOING)) return -1;
    sqlite3_mutex* mutex = sqlite3_db_mutex(db);
    if (mutex) sqlite3_mutex_enter(mutex);
    int rc = -1;
    if (exec_sql(db, "BEGIN IMMEDIATE")) goto done;
    sqlite3_int64 id = 0;
    if (!force_new_generation && direction == CALL_LOG_INCOMING && call_idx >= 0) {
        sqlite3_stmt* find = NULL;
        if (sqlite3_prepare_v2(db, "SELECT id FROM call_log WHERE device=? AND call_idx=? AND finished=0 AND direction='incoming' ORDER BY id DESC LIMIT 1", -1, &find, NULL) != SQLITE_OK) goto rollback;
        sqlite3_bind_text(find, 1, device, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(find, 2, call_idx);
        if (sqlite3_step(find) == SQLITE_ROW) id = sqlite3_column_int64(find, 0);
        sqlite3_finalize(find);
        if (id && number && *number) {
            sqlite3_stmt* update_number = NULL;
            if (sqlite3_prepare_v2(db, "UPDATE call_log SET number=? WHERE id=? AND number=''", -1, &update_number, NULL) != SQLITE_OK) goto rollback;
            sqlite3_bind_text(update_number, 1, number, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(update_number, 2, id);
            int update_step = sqlite3_step(update_number);
            sqlite3_finalize(update_number);
            if (update_step != SQLITE_DONE) goto rollback;
        }
    }
    if (!id) {
        sqlite3_stmt* insert = NULL;
        if (sqlite3_prepare_v2(db, "INSERT INTO call_log(device,number,started_at,direction,call_idx) VALUES(?,?,?,?,?)", -1, &insert, NULL) != SQLITE_OK) goto rollback;
        sqlite3_bind_text(insert, 1, device, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 2, number ? number : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 3, timestamp, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insert, 4, direction == CALL_LOG_INCOMING ? "incoming" : "outgoing", -1, SQLITE_STATIC);
        sqlite3_bind_int(insert, 5, call_idx);
        int step = sqlite3_step(insert);
        sqlite3_finalize(insert);
        if (step != SQLITE_DONE) goto rollback;
        id = sqlite3_last_insert_rowid(db);
    }
    if (exec_sql(db, "COMMIT")) goto rollback;
    if (snprintf(call_id, call_id_len, "call-%lld", (long long)id) >= (int)call_id_len) goto done;
    rc = 0;
    goto done;
rollback:
    exec_sql(db, "ROLLBACK");
done:
    if (mutex) sqlite3_mutex_leave(mutex);
    return rc;
}

/** Дедуплицировать повтор CLCC входящего события либо создать исходящую запись. */
int call_log_start(sqlite3* db, const char* device, int call_idx, call_log_direction_t direction,
                   const char* number, const char* timestamp, char* call_id, size_t call_id_len)
{
    return call_log_start_internal(db, device, call_idx, direction, number, timestamp, call_id, call_id_len, 0);
}

/** Сохранить START с заранее зарезервированным ID из async producer.
 * Writer вставляет явный AUTOINCREMENT rowid; транзакция удерживает SQLite mutex до commit,
 * чтобы последующие answer/finish из FIFO видели существующую строку. Ошибка откатывает insert.
 */
int call_log_start_reserved(sqlite3* db, sqlite3_int64 id, const char* device, int call_idx,
                            call_log_direction_t direction, const char* number, const char* timestamp)
{
    if (!db || id <= 0 || !device || !timestamp ||
        (direction != CALL_LOG_INCOMING && direction != CALL_LOG_OUTGOING)) return -1;
    sqlite3_mutex* mutex = sqlite3_db_mutex(db);
    if (mutex) sqlite3_mutex_enter(mutex);
    int rc = -1;
    sqlite3_stmt* stmt = NULL;
    if (exec_sql(db, "BEGIN IMMEDIATE")) goto done;
    if (sqlite3_prepare_v2(db, "INSERT INTO call_log(id,device,number,started_at,direction,call_idx) VALUES(?,?,?,?,?,?)", -1, &stmt, NULL) != SQLITE_OK) goto rollback;
    if (sqlite3_bind_int64(stmt, 1, id) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 2, device, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 3, number ? number : "", -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 4, timestamp, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 5, direction == CALL_LOG_INCOMING ? "incoming" : "outgoing", -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 6, call_idx) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_DONE) goto rollback;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (exec_sql(db, "COMMIT")) goto rollback;
    rc = 0;
    goto done;
rollback:
    if (stmt) sqlite3_finalize(stmt);
    exec_sql(db, "ROLLBACK");
done:
    if (mutex) sqlite3_mutex_leave(mutex);
    return rc;
}

/** Создать новое поколение входящего вызова при явном unsolicited событии. */
int call_log_start_new_generation(sqlite3* db, const char* device, int call_idx, const char* number,
                                  const char* timestamp, char* call_id, size_t call_id_len)
{
    return call_log_start_internal(db, device, call_idx, CALL_LOG_INCOMING, number, timestamp, call_id, call_id_len, 1);
}

/** Классифицировать source/state/barrier: DSCI active-first также создаёт поколение. */
call_log_generation_decision_t call_log_incoming_generation_decide(call_log_event_origin_t origin,
                                                                     int incoming_direction,
                                                                     call_log_event_state_t state,
                                                                     int release_pending,
                                                                     int successor_known)
{
    if (!incoming_direction || state == CALL_LOG_EVENT_STATE_OTHER) return CALL_LOG_GENERATION_DEDUPLICATE;
    if (origin == CALL_LOG_EVENT_UNSOLICITED && release_pending) return CALL_LOG_GENERATION_NEW;
    if (successor_known) return CALL_LOG_GENERATION_USE_SUCCESSOR;
    return CALL_LOG_GENERATION_DEDUPLICATE;
}

/** Busy snapshot отменяет старый release только если не наблюдалось successor-поколение. */
int call_log_release_should_finish(int slot_present, int successor_known)
{
    return !slot_present || successor_known;
}

/** Обновить эфемерный индекс исходящей записи после CLCC correlation.
 * Условие по durable ID исключает повреждение другой строки при повторном индексе.
 */
int call_log_set_index(sqlite3* db, const char* call_id, int call_idx)
{
    if (!db || !call_id) return -1;
    long long id = 0;
    if (sscanf(call_id, "call-%lld", &id) != 1 || id <= 0) return -1;
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE call_log SET call_idx=? WHERE id=? AND finished=0", -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, call_idx);
    sqlite3_bind_int64(stmt, 2, id);
    int ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok ? 0 : -1;
}

/** Найти ID активного входящего вызова без PBX-канала.
 * Выходной буфер принадлежит caller; возвращает 1 найден, 0 отсутствует, -1 SQL/buffer error.
 */
int call_log_active_id(sqlite3* db, const char* device, int call_idx, char* call_id, size_t call_id_len)
{
    if (!db || !device || !call_id || !call_id_len) return -1;
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id FROM call_log WHERE device=? AND call_idx=? AND finished=0 AND direction='incoming' ORDER BY id DESC LIMIT 1", -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, device, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, call_idx);
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        int n = snprintf(call_id, call_id_len, "call-%lld", (long long)sqlite3_column_int64(stmt, 0));
        sqlite3_finalize(stmt);
        return n >= 0 && (size_t)n < call_id_len ? 1 : -1;
    }
    sqlite3_finalize(stmt);
    return step == SQLITE_DONE ? 0 : -1;
}

/** Зафиксировать ответ только для ещё открытого вызова по устойчивому ID.
 * UPDATE не создаёт строк и не влияет на другие звонки при повторном событии.
 */
int call_log_answer(sqlite3* db, const char* call_id)
{
    if (!db || !call_id) return -1;
    long long id = 0;
    if (sscanf(call_id, "call-%lld", &id) != 1 || id <= 0) return -1;
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE call_log SET answered=1 WHERE id=? AND finished=0 AND direction='incoming'", -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, id);
    int ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok ? 0 : -1;
}

/** Завершить вызов и при пропуске вставить ровно одну MT запись.
 * BEGIN IMMEDIATE удерживает статус и очередь атомарно; повторный вызов видит
 * finished=1 и не вставляет заново. Текст строится из сохранённых device/number/start.
 */
int call_log_finish(sqlite3* db, const char* call_id, int answered, const char* timestamp)
{
    if (!db || !call_id || !timestamp) return -1;
    long long id = 0;
    if (sscanf(call_id, "call-%lld", &id) != 1 || id <= 0) return -1;
    sqlite3_mutex* mutex = sqlite3_db_mutex(db);
    if (mutex) sqlite3_mutex_enter(mutex);
    int rc = -1;
    if (exec_sql(db, "BEGIN IMMEDIATE")) goto done;
    sqlite3_stmt* find = NULL;
    if (sqlite3_prepare_v2(db, "SELECT device,number,started_at,direction,answered,finished FROM call_log WHERE id=?", -1, &find, NULL) != SQLITE_OK) goto rollback;
    sqlite3_bind_int64(find, 1, id);
    if (sqlite3_step(find) != SQLITE_ROW) { sqlite3_finalize(find); goto rollback; }
    char device[128], number[128], started[64], direction[16];
    snprintf(device, sizeof(device), "%s", sqlite3_column_text(find, 0));
    snprintf(number, sizeof(number), "%s", sqlite3_column_text(find, 1));
    snprintf(started, sizeof(started), "%s", sqlite3_column_text(find, 2));
    snprintf(direction, sizeof(direction), "%s", sqlite3_column_text(find, 3));
    int was_answered = sqlite3_column_int(find, 4), finished = sqlite3_column_int(find, 5);
    sqlite3_finalize(find);
    if (finished) {
        if (!exec_sql(db, "COMMIT")) rc = 0;
        else exec_sql(db, "ROLLBACK");
        goto done;
    }
    const int missed = !strcmp(direction, "incoming") && !answered && !was_answered;
    sqlite3_stmt* update = NULL;
    const char* new_direction = missed ? "missed" : direction;
    if (sqlite3_prepare_v2(db, "UPDATE call_log SET direction=?,answered=MAX(answered,?),finished=1,ended_at=? WHERE id=? AND finished=0", -1, &update, NULL) != SQLITE_OK) goto rollback;
    sqlite3_bind_text(update, 1, new_direction, -1, SQLITE_STATIC);
    sqlite3_bind_int(update, 2, answered != 0);
    sqlite3_bind_text(update, 3, timestamp, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(update, 4, id);
    int step = sqlite3_step(update); sqlite3_finalize(update);
    if (step != SQLITE_DONE) goto rollback;
    if (missed) {
        char mid[CALL_LOG_ID_LEN], message[CALL_LOG_TEXT_LEN];
        snprintf(mid, sizeof(mid), "missed-%lld", id);
        snprintf(message, sizeof(message), "Пропущенный вызов: SIM %s, номер %s, время начала %s", device, number, started);
        if (sms_mt_queue_put(db, device, mid, "missed-call", message, started)) goto rollback;
    }
    if (exec_sql(db, "COMMIT")) goto rollback;
    rc = 0;
    goto done;
rollback:
    exec_sql(db, "ROLLBACK");
done:
    if (mutex) sqlite3_mutex_leave(mutex);
    return rc;
}

/** Завершить кандидат только если свежий CLCC не показывает занятый индекс.
 * При наличии строки запись остаётся неизменной; absence использует тот же атомарный
 * missed+queue commit, что и обычное подтверждённое завершение.
 */
int call_log_release_confirm(sqlite3* db, const char* call_id, int slot_present, const char* timestamp)
{
    if (slot_present) return 0;
    return call_log_finish(db, call_id, 0, timestamp);
}

/** Восстановить незавершённые строки после аварийного завершения процесса.
 * Каждая строка проходит ту же атомарную finish-операцию под общей транзакцией;
 * answered-флаг сохраняет answered incoming, а outgoing не становится missed.
 */
int call_log_recover(sqlite3* db, const char* timestamp)
{
    if (!db || !timestamp) return -1;
    sqlite3_mutex* mutex = sqlite3_db_mutex(db);
    for (;;) {
        if (mutex) sqlite3_mutex_enter(mutex);
        sqlite3_stmt* row = NULL;
        if (sqlite3_prepare_v2(db, "SELECT id,answered FROM call_log WHERE finished=0 ORDER BY id LIMIT 1", -1, &row, NULL) != SQLITE_OK) {
            if (mutex) sqlite3_mutex_leave(mutex);
            return -1;
        }
        int step = sqlite3_step(row);
        char id[CALL_LOG_ID_LEN] = "";
        int answered = 0;
        if (step == SQLITE_ROW) {
            snprintf(id, sizeof(id), "call-%lld", (long long)sqlite3_column_int64(row, 0));
            answered = sqlite3_column_int(row, 1);
        }
        sqlite3_finalize(row);
        if (mutex) sqlite3_mutex_leave(mutex);
        if (step == SQLITE_DONE) return 0;
        if (step != SQLITE_ROW || call_log_finish(db, id, answered, timestamp)) return -1;
    }
}
