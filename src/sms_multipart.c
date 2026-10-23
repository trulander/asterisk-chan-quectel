#include "sms_multipart.h"

#include <string.h>

/** Сохранить одну входящую MT queue row; строки caller-owned, SQL statement локальный. */
int sms_mt_queue_put(sqlite3* db, const char* device, const char* mid, const char* from_num, const char* message, const char* timestamp)
{
    static const char sql[] = "INSERT OR IGNORE INTO mt_queue(device,mid,from_num,msg,ts,state) VALUES(?,?,?,?,?,'new')";
    sqlite3_stmt* stmt = NULL;
    int result = -1;
    if (!db || !device || !mid || !*mid || sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_bind_text(stmt, 1, device, -1, SQLITE_TRANSIENT) == SQLITE_OK && sqlite3_bind_text(stmt, 2, mid, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 3, from_num ? from_num : "", -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 4, message ? message : "", -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 5, timestamp ? timestamp : "", -1, SQLITE_TRANSIENT) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_DONE) result = 0;
    sqlite3_finalize(stmt);
    return result;
}

/** Связать NULL/пустой optional source как SQL NULL, остальное как transient copy. */
static int bind_nullable_text(sqlite3_stmt* stmt, int column, const char* value)
{
    return value && *value ? sqlite3_bind_text(stmt, column, value, -1, SQLITE_TRANSIENT) : sqlite3_bind_null(stmt, column);
}

/** Вставить/обновить часть, определить count, собрать ordered payload и при legacy режиме GC. */
int sms_multipart_put(sqlite3* db, const char* key, int seqorder, int parts, int ttl, const char* message, const char* source_device,
                     const char* source_storage, int source_index, int retain_parts, char** assembled, long long* rowid)
{
    static const char put_sql[] =
        "INSERT INTO incoming_msg (key,seqorder,expiration,message,source_device,source_storage,source_index,delete_status,delete_attempts,retry_at,mt_queued,mt_mid) "
        "VALUES (?, ?, strftime('%s') + ?, ?, ?, ?, ?, 0, 0, 0, 0, NULL) "
        "ON CONFLICT(key,seqorder) DO UPDATE SET expiration=excluded.expiration,message=excluded.message,"
        "delete_status=CASE WHEN incoming_msg.source_device IS excluded.source_device AND incoming_msg.source_storage IS excluded.source_storage AND incoming_msg.source_index IS excluded.source_index THEN incoming_msg.delete_status ELSE 0 END,"
        "delete_attempts=CASE WHEN incoming_msg.source_device IS excluded.source_device AND incoming_msg.source_storage IS excluded.source_storage AND incoming_msg.source_index IS excluded.source_index THEN incoming_msg.delete_attempts ELSE 0 END,"
        "retry_at=CASE WHEN incoming_msg.source_device IS excluded.source_device AND incoming_msg.source_storage IS excluded.source_storage AND incoming_msg.source_index IS excluded.source_index THEN incoming_msg.retry_at ELSE 0 END,"
        "mt_queued=CASE WHEN incoming_msg.source_device IS excluded.source_device AND incoming_msg.source_storage IS excluded.source_storage AND incoming_msg.source_index IS excluded.source_index THEN incoming_msg.mt_queued ELSE 0 END,"
        "mt_mid=CASE WHEN incoming_msg.source_device IS excluded.source_device AND incoming_msg.source_storage IS excluded.source_storage AND incoming_msg.source_index IS excluded.source_index THEN incoming_msg.mt_mid ELSE NULL END,"
        "source_device=excluded.source_device,source_storage=excluded.source_storage,source_index=excluded.source_index";
    sqlite3_stmt* stmt = NULL;
    int rc;
    int count = -1;
    if (!db || !key || !message || parts < 1 || seqorder < 1 || !assembled) return -1;
    *assembled = NULL;
    if (rowid) *rowid = 0;
    if (sqlite3_prepare_v2(db, put_sql, -1, &stmt, NULL) != SQLITE_OK) goto done;
    if (sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT) != SQLITE_OK || sqlite3_bind_int(stmt, 2, seqorder) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 3, ttl) != SQLITE_OK || sqlite3_bind_text(stmt, 4, message, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        bind_nullable_text(stmt, 5, source_device) != SQLITE_OK || bind_nullable_text(stmt, 6, source_storage) != SQLITE_OK ||
        (source_index < 0 ? sqlite3_bind_null(stmt, 7) : sqlite3_bind_int(stmt, 7, source_index)) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_DONE) goto done;
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (rowid) {
        if (sqlite3_prepare_v2(db, "SELECT rowid FROM incoming_msg WHERE key=? AND seqorder=?", -1, &stmt, NULL) != SQLITE_OK ||
            sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT) != SQLITE_OK || sqlite3_bind_int(stmt, 2, seqorder) != SQLITE_OK ||
            sqlite3_step(stmt) != SQLITE_ROW) goto done;
        *rowid = (long long)sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    if (sqlite3_prepare_v2(db, "SELECT COUNT(seqorder) FROM incoming_msg WHERE key=?", -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT) != SQLITE_OK || sqlite3_step(stmt) != SQLITE_ROW) goto done;
    count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (count == parts) {
        size_t bytes = 1;
        if (sqlite3_prepare_v2(db, "SELECT message FROM incoming_msg WHERE key=? ORDER BY seqorder", -1, &stmt, NULL) != SQLITE_OK ||
            sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT) != SQLITE_OK) goto done;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) bytes += (size_t)sqlite3_column_bytes(stmt, 0);
        if (rc != SQLITE_DONE) goto done;
        sqlite3_finalize(stmt);
        stmt = NULL;
        *assembled = sqlite3_malloc64(bytes);
        if (!*assembled) goto done;
        (*assembled)[0] = '\0';
        if (sqlite3_prepare_v2(db, "SELECT message FROM incoming_msg WHERE key=? ORDER BY seqorder", -1, &stmt, NULL) != SQLITE_OK ||
            sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT) != SQLITE_OK) goto done;
        size_t used = 0;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            const size_t n = (size_t)sqlite3_column_bytes(stmt, 0);
            memcpy(*assembled + used, sqlite3_column_text(stmt, 0), n);
            used += n;
        }
        if (rc != SQLITE_DONE) goto done;
        (*assembled)[used] = '\0';
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (!retain_parts && sqlite3_prepare_v2(db, "DELETE FROM incoming_msg WHERE key=?", -1, &stmt, NULL) == SQLITE_OK &&
            sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_DONE) {
            sqlite3_finalize(stmt);
            stmt = NULL;
        } else if (!retain_parts) {
            goto done;
        }
    }
    return count;

done:
    if (stmt) sqlite3_finalize(stmt);
    sqlite3_free(*assembled);
    *assembled = NULL;
    return -1;
}
