/*
   Pure SQLite implementation of the unified SMS history pagination.
   See sms_paging.h for the contract.

   The statements must stay in sync with the tables created by smsdb_schema in
   smsdb.c (db_init): mt_queue (MT, SIM->SIP) and mt_out (MO, SIP->SIM).
   Since the production module prepares its statements per call through this
   module, the exact same SQL is what both the module's /sms pages and the
   unit test exercise.
*/

#include <stdio.h>
#include <string.h>

#include "sms_paging.h"

/*! Per-device page (MT + MO merged, newest first). */
static const char SMS_PAGE_DEV_SQL[] =
    "SELECT 'mt' AS direction, device, mid AS ref_id, from_num AS peer, msg, ts, state, created_at AS ts_epoch, CAST(rowid AS INTEGER) AS seq "
    "FROM mt_queue WHERE device = ? "
    "UNION ALL "
    "SELECT 'mo', device, CAST(id AS TEXT) AS ref_id, dst, msg, ts, state, created_at AS ts_epoch, id AS seq "
    "FROM mt_out WHERE device = ? "
    "ORDER BY ts_epoch DESC, seq DESC LIMIT ? OFFSET ?";

/*! Per-device count. */
static const char SMS_COUNT_DEV_SQL[] =
    "SELECT (SELECT COUNT(*) FROM mt_queue WHERE device = ?) + (SELECT COUNT(*) FROM mt_out WHERE device = ?)";

/*! All-devices page (MT + MO merged, newest first). */
static const char SMS_PAGE_ALL_SQL[] =
    "SELECT 'mt' AS direction, device, mid AS ref_id, from_num AS peer, msg, ts, state, created_at AS ts_epoch, CAST(rowid AS INTEGER) AS seq "
    "FROM mt_queue "
    "UNION ALL "
    "SELECT 'mo', device, CAST(id AS TEXT) AS ref_id, dst, msg, ts, state, created_at AS ts_epoch, id AS seq "
    "FROM mt_out "
    "ORDER BY ts_epoch DESC, seq DESC LIMIT ? OFFSET ?";

/*! All-devices count. */
static const char SMS_COUNT_ALL_SQL[] =
    "SELECT (SELECT COUNT(*) FROM mt_queue) + (SELECT COUNT(*) FROM mt_out)";

static void row_copy_text(char* dst, size_t dst_len, sqlite3_stmt* stmt, int col, const char* dflt)
{
    const char* text = (const char*)sqlite3_column_text(stmt, col);

    if (!text) {
        text = dflt ? dflt : "";
    }
    if (dst_len) {
        snprintf(dst, dst_len, "%s", text);
    }
}

static void sms_row_fill(struct sms_paging_row* row, sqlite3_stmt* stmt)
{
    memset(row, 0, sizeof(*row));
    row_copy_text(row->direction, sizeof(row->direction), stmt, 0, "mt");
    row_copy_text(row->device, sizeof(row->device), stmt, 1, "");
    row_copy_text(row->ref_id, sizeof(row->ref_id), stmt, 2, "");
    row_copy_text(row->peer, sizeof(row->peer), stmt, 3, "");
    row_copy_text(row->msg, sizeof(row->msg), stmt, 4, "");
    row_copy_text(row->ts, sizeof(row->ts), stmt, 5, "");
    row_copy_text(row->state, sizeof(row->state), stmt, 6, "");
    row->created_at = (long)sqlite3_column_int64(stmt, 7);
    row->id         = (long)sqlite3_column_int64(stmt, 8);
}

int sms_paging_count(sqlite3* db, const char* device, int* total)
{
    sqlite3_stmt* stmt = NULL;
    const int filtered = device && device[0] != '\0';
    const char* const sql = filtered ? SMS_COUNT_DEV_SQL : SMS_COUNT_ALL_SQL;
    int res;

    if (!db || !total) {
        return -1;
    }
    *total = 0;

    if ((res = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) != SQLITE_OK) {
        return -1;
    }
    if (filtered) {
        if ((res = sqlite3_bind_text(stmt, 1, device, -1, SQLITE_TRANSIENT)) != SQLITE_OK ||
            (res = sqlite3_bind_text(stmt, 2, device, -1, SQLITE_TRANSIENT)) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return -1;
        }
    }

    res = sqlite3_step(stmt);
    if (res != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }
    *total = sqlite3_column_int(stmt, 0);
    res = sqlite3_finalize(stmt);

    return (res == SQLITE_OK) ? 0 : -1;
}

int sms_paging_page(sqlite3* db, const char* device, int page_size, int offset, void* ctx, sms_paging_row_cb cb)
{
    sqlite3_stmt* stmt = NULL;
    const int filtered = device && device[0] != '\0';
    const char* const sql = filtered ? SMS_PAGE_DEV_SQL : SMS_PAGE_ALL_SQL;
    int res;

    if (!db || !cb || page_size <= 0 || offset < 0) {
        return -1;
    }

    if ((res = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) != SQLITE_OK) {
        return -1;
    }
    if (filtered) {
        if ((res = sqlite3_bind_text(stmt, 1, device, -1, SQLITE_TRANSIENT)) != SQLITE_OK ||
            (res = sqlite3_bind_text(stmt, 2, device, -1, SQLITE_TRANSIENT)) != SQLITE_OK ||
            (res = sqlite3_bind_int(stmt, 3, page_size)) != SQLITE_OK || (res = sqlite3_bind_int(stmt, 4, offset)) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            return -1;
        }
    } else if ((res = sqlite3_bind_int(stmt, 1, page_size)) != SQLITE_OK || (res = sqlite3_bind_int(stmt, 2, offset)) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }

    while ((res = sqlite3_step(stmt)) == SQLITE_ROW) {
        struct sms_paging_row row;

        sms_row_fill(&row, stmt);
        if (cb(ctx, &row)) {
            sqlite3_finalize(stmt);
            return 0;
        }
    }
    res = sqlite3_finalize(stmt);

    return (res == SQLITE_OK) ? 0 : -1;
}