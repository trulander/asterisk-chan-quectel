/* SQLite реализации read-only pager incoming_msg; комментарии к контрактам — в .h. */
#include <stdio.h>
#include <string.h>
#include "sms_incoming_paging.h"

/* Запросы держат device как bind value; сортировка задаёт детерминированные границы страниц. */
static const char *const count_sql[] = {
    "SELECT COUNT(*) FROM incoming_msg",
    "SELECT COUNT(*) FROM incoming_msg WHERE source_device=?"
};
static const char *const page_sql[] = {
    "SELECT rowid,source_device,source_storage,source_index,key,seqorder,message,expiration,delete_status,delete_attempts,retry_at,mt_queued,mt_mid FROM incoming_msg ORDER BY expiration DESC,key COLLATE BINARY ASC,seqorder ASC,rowid ASC LIMIT ? OFFSET ?",
    "SELECT rowid,source_device,source_storage,source_index,key,seqorder,message,expiration,delete_status,delete_attempts,retry_at,mt_queued,mt_mid FROM incoming_msg WHERE source_device=? ORDER BY expiration DESC,key COLLATE BINARY ASC,seqorder ASC,rowid ASC LIMIT ? OFFSET ?"
};

/** Копировать nullable SQL text в inline buffer; SQLite-owned bytes не переживают step. */
static void copy_text(char *dst, size_t dst_size, sqlite3_stmt *stmt, int column)
{
    const unsigned char *value = sqlite3_column_text(stmt, column);
    if (dst_size) snprintf(dst, dst_size, "%s", value ? (const char *)value : "");
}

/** Выполнить COUNT; обе ветви завершают statement, не меняют таблицу и возвращают SQL failure. */
int sms_incoming_paging_count(sqlite3 *db, const char *device, int *total)
{
    sqlite3_stmt *stmt = NULL;
    int filtered = device && *device;
    int rc;
    if (!db || !total) return -1;
    *total = 0;
    if (sqlite3_prepare_v2(db, count_sql[filtered], -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (filtered && sqlite3_bind_text(stmt, 1, device, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) *total = sqlite3_column_int(stmt, 0);
    if (sqlite3_finalize(stmt) != SQLITE_OK || rc != SQLITE_ROW) return -1;
    return 0;
}

/** Выполнить paged SELECT и скопировать каждую строку до следующего sqlite_step.
 * При SQL error callback-ов partial page быть не должно; callback-stop является успехом.
 */
int sms_incoming_paging_page(sqlite3 *db, const char *device, int page_size, int offset,
                             void *ctx, sms_incoming_page_cb cb)
{
    sqlite3_stmt *stmt = NULL;
    int filtered = device && *device;
    int rc, bind = 1;
    if (!db || !cb || page_size <= 0 || offset < 0) return -1;
    if (sqlite3_prepare_v2(db, page_sql[filtered], -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (filtered && sqlite3_bind_text(stmt, bind++, device, -1, SQLITE_TRANSIENT) != SQLITE_OK) goto error;
    if (sqlite3_bind_int(stmt, bind++, page_size) != SQLITE_OK ||
        sqlite3_bind_int(stmt, bind, offset) != SQLITE_OK) goto error;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        struct sms_incoming_page_row row;
        memset(&row, 0, sizeof(row));
        row.rowid = sqlite3_column_int64(stmt, 0);
        copy_text(row.device, sizeof(row.device), stmt, 1);
        copy_text(row.storage, sizeof(row.storage), stmt, 2);
        row.source_index = sqlite3_column_int(stmt, 3);
        copy_text(row.key, sizeof(row.key), stmt, 4);
        row.seqorder = sqlite3_column_int(stmt, 5);
        copy_text(row.message, sizeof(row.message), stmt, 6);
        row.expiration = sqlite3_column_int64(stmt, 7);
        row.delete_status = sqlite3_column_int(stmt, 8);
        row.delete_attempts = sqlite3_column_int(stmt, 9);
        row.retry_at = sqlite3_column_int64(stmt, 10);
        row.mt_queued = sqlite3_column_int(stmt, 11);
        copy_text(row.mt_mid, sizeof(row.mt_mid), stmt, 12);
        if (cb(ctx, &row)) {
            sqlite3_finalize(stmt);
            return 0;
        }
    }
    if (rc != SQLITE_DONE) goto error;
    return sqlite3_finalize(stmt) == SQLITE_OK ? 0 : -1;
error:
    sqlite3_finalize(stmt);
    return -1;
}
