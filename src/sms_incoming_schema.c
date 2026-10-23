#include "sms_incoming_schema.h"
#include "sms_multipart.h"

#include <string.h>

/** Выполнить один idempotent ALTER; duplicate-column означает уже применённую миграцию. */
static int run_migration(sqlite3* db, const char* sql)
{
    char* errmsg = NULL;
    const int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc == SQLITE_OK || (errmsg && strstr(errmsg, "duplicate column name"))) {
        sqlite3_free(errmsg);
        return 0;
    }
    sqlite3_free(errmsg);
    return -1;
}

/** Обновить только rowid с совпавшим source identity; caller владеет mutex и transaction. */
int sms_incoming_delete_state_update(sqlite3* db, long long rowid, const char* device, const char* storage, int index,
                                    int status, unsigned attempts, int retry_at)
{
    static const char sql[] = "UPDATE incoming_msg SET delete_status=?,delete_attempts=?,retry_at=? WHERE rowid=? AND source_device IS ? AND source_storage IS ? AND source_index IS ?";
    sqlite3_stmt* stmt = NULL;
    int result = -1;
    if (!db || rowid <= 0 || !device || !storage || sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_bind_int(stmt, 1, status) == SQLITE_OK && sqlite3_bind_int(stmt, 2, (int)attempts) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 3, retry_at) == SQLITE_OK && sqlite3_bind_int64(stmt, 4, rowid) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 5, device, -1, SQLITE_TRANSIENT) == SQLITE_OK && sqlite3_bind_text(stmt, 6, storage, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 7, index) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1) result = 0;
    sqlite3_finalize(stmt);
    return result;
}

/** Сдвинуть retry_at для валидной SM job, не меняя status/attempts при fairness defer. */
int sms_incoming_delete_retry_defer(sqlite3* db, long long rowid, int retry_at)
{
    static const char sql[] = "UPDATE incoming_msg SET retry_at=? WHERE rowid=? AND source_storage='SM' AND source_device IS NOT NULL AND source_index IS NOT NULL";
    sqlite3_stmt* stmt = NULL;
    int result = -1;
    if (!db || rowid <= 0 || sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_bind_int(stmt, 1, retry_at) == SQLITE_OK && sqlite3_bind_int64(stmt, 2, rowid) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1) result = 0;
    sqlite3_finalize(stmt);
    return result;
}

/** Прочитать aggregate marker стабильной multipart группы; отсутствие group — ошибка, не empty marker. */
int sms_incoming_group_queue_state(sqlite3* db, const char* key, int* queued, char* mid, size_t mid_len)
{
    static const char sql[] = "SELECT COUNT(*),MIN(mt_queued),MIN(mt_mid) FROM incoming_msg WHERE key=?";
    sqlite3_stmt* stmt = NULL;
    int result = -1;
    if (!db || !key || !queued || !mid || !mid_len || sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) > 0) {
        const unsigned char* const stored_mid = sqlite3_column_text(stmt, 2);
        const int bytes = stored_mid ? sqlite3_column_bytes(stmt, 2) : 0;
        if ((size_t)bytes < mid_len) {
            *queued = sqlite3_column_int(stmt, 1) == 1;
            if (bytes) memcpy(mid, stored_mid, bytes);
            mid[bytes] = '\0';
            result = 0;
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

/** Пометить все segment rows группы одним MID внутри внешней transaction. */
int sms_incoming_group_mark_queued(sqlite3* db, const char* key, const char* mid)
{
    static const char sql[] = "UPDATE incoming_msg SET mt_queued=1,mt_mid=? WHERE key=?";
    sqlite3_stmt* stmt = NULL;
    int result = -1;
    if (!db || !key || !mid || !*mid || sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_bind_text(stmt, 1, mid, -1, SQLITE_TRANSIENT) == SQLITE_OK && sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) result = 0;
    sqlite3_finalize(stmt);
    return result;
}

/** Сначала reuse marker, иначе вставить MT row и marker в общей caller transaction. */
int sms_incoming_mt_queue_group(sqlite3* db, const char* key, const char* device, const char* candidate_mid,
                                const char* from_num, const char* message, const char* timestamp,
                                char* effective_mid, size_t effective_mid_len, int* inserted)
{
    char existing_mid[128];
    int queued = 0;
    if (!db || !key || !device || !candidate_mid || !*candidate_mid || !effective_mid || !effective_mid_len || !inserted ||
        sms_incoming_group_queue_state(db, key, &queued, existing_mid, sizeof(existing_mid))) return -1;
    if (queued) {
        const size_t length = strlen(existing_mid);
        if (length >= effective_mid_len) return -1;
        memcpy(effective_mid, existing_mid, length + 1);
        *inserted = 0;
        return 0;
    }
    if (sms_mt_queue_put(db, device, candidate_mid, from_num, message, timestamp) != 0 || sqlite3_changes(db) != 1 ||
        sms_incoming_group_mark_queued(db, key, candidate_mid) || sms_incoming_gc_group(db, key)) return -1;
    const size_t length = strlen(candidate_mid);
    if (length >= effective_mid_len) return -1;
    memcpy(effective_mid, candidate_mid, length + 1);
    *inserted = 1;
    return 0;
}

/** Удалить выбранную группу только если queued и каждая строка complete. */
int sms_incoming_gc_group(sqlite3* db, const char* key)
{
    static const char select_sql[] = "SELECT COUNT(*),SUM(CASE WHEN delete_status=2 THEN 1 ELSE 0 END),MIN(mt_queued) FROM incoming_msg WHERE key=?";
    static const char delete_sql[] = "DELETE FROM incoming_msg WHERE key=?";
    sqlite3_stmt* stmt = NULL;
    int total, empty, queued, result = -1;
    if (!db || !key || sqlite3_prepare_v2(db, select_sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW) goto done;
    total = sqlite3_column_int(stmt, 0);
    empty = sqlite3_column_int(stmt, 1);
    queued = sqlite3_column_int(stmt, 2);
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (total <= 0 || total != empty || !queued) return 0;
    if (sqlite3_prepare_v2(db, delete_sql, -1, &stmt, NULL) != SQLITE_OK || sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_DONE) goto done;
    result = 0;
done:
    sqlite3_finalize(stmt);
    return result;
}

/** Startup/repair sweep для групп, удовлетворяющих тем же queue/delete invariant. */
int sms_incoming_gc_completed_groups(sqlite3* db)
{
    static const char sql[] =
        "DELETE FROM incoming_msg WHERE key IN (SELECT key FROM incoming_msg GROUP BY key "
        "HAVING MIN(mt_queued)=1 AND SUM(CASE WHEN delete_status=2 THEN 1 ELSE 0 END)=COUNT(*))";
    return db && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

/** Выбрать первую due SM job по retry_at,rowid; WHERE отсекает неадресуемые строки. */
int sms_incoming_delete_job_pick(sqlite3* db, int now, long long* rowid, char* device, size_t device_len, int* index)
{
    /* CMGD_SENT включён: потерянная/ошибочная фиксация ответа OK оставляет результат
     * неоднозначным, поэтому restart обязан свежо прочитать ячейку, а не повторять CMGD. */
    static const char sql[] = "SELECT rowid,source_device,source_index FROM incoming_msg WHERE source_storage='SM' AND source_device IS NOT NULL AND length(trim(source_device))>0 AND typeof(source_index)='integer' AND source_index>=0 AND delete_status IN (0,1,4,5,6) AND retry_at<=? ORDER BY retry_at,rowid LIMIT 1";
    sqlite3_stmt* stmt = NULL;
    int rc, result = -1;
    if (!db || !rowid || !device || !device_len || !index || sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_bind_int(stmt, 1, now) != SQLITE_OK) goto done;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        result = 0;
        goto done;
    }
    if (rc != SQLITE_ROW) goto done;
    const unsigned char* const source = sqlite3_column_text(stmt, 1);
    if (!source || sqlite3_column_bytes(stmt, 1) >= (int)device_len) goto done;
    memcpy(device, source, sqlite3_column_bytes(stmt, 1));
    device[sqlite3_column_bytes(stmt, 1)] = '\0';
    *rowid = (long long)sqlite3_column_int64(stmt, 0);
    *index = sqlite3_column_int(stmt, 2);
    result = 1;
done:
    sqlite3_finalize(stmt);
    return result;
}

/** Создать актуальную таблицу, применить совместимые миграции и индекс идемпотентно. */
int sms_incoming_schema_ensure(sqlite3* db)
{
    static const char* const migrations[] = {
        "ALTER TABLE incoming_msg ADD COLUMN source_device TEXT",
        "ALTER TABLE incoming_msg ADD COLUMN source_storage TEXT",
        "ALTER TABLE incoming_msg ADD COLUMN source_index INTEGER",
        "ALTER TABLE incoming_msg ADD COLUMN delete_status INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE incoming_msg ADD COLUMN delete_attempts INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE incoming_msg ADD COLUMN retry_at INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE incoming_msg ADD COLUMN mt_queued INTEGER NOT NULL DEFAULT 0",
        "ALTER TABLE incoming_msg ADD COLUMN mt_mid TEXT",
    };
    static const char create_sql[] =
        "CREATE TABLE IF NOT EXISTS incoming_msg (key VARCHAR(256), seqorder INTEGER, expiration TIMESTAMP DEFAULT (strftime('%s'))," 
        "message VARCHAR(256), source_device TEXT, source_storage TEXT, source_index INTEGER, delete_status INTEGER NOT NULL DEFAULT 0," 
        "delete_attempts INTEGER NOT NULL DEFAULT 0, retry_at INTEGER NOT NULL DEFAULT 0, mt_queued INTEGER NOT NULL DEFAULT 0, mt_mid TEXT, PRIMARY KEY(key,seqorder))";
    if (!db || sqlite3_exec(db, create_sql, NULL, NULL, NULL) != SQLITE_OK) return -1;
    for (unsigned i = 0; i < sizeof(migrations) / sizeof(migrations[0]); ++i) {
        if (run_migration(db, migrations[i])) return -1;
    }
    return sqlite3_exec(db, "CREATE INDEX IF NOT EXISTS incoming_key ON incoming_msg(key)", NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}
