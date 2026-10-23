#include "sim_metrics.h"

#include <sqlite3.h>
#include <stddef.h>

/* Запрос считает направления независимо: mt_queue delivered означает доставку SIP,
 * а mt_out delivered_to_handset — подтверждённую доставку абонентскому устройству.
 * NULL SRR не классифицируется отдельно: источником состояния остаётся поле state. */
static const char METRICS_SQL[] =
    "SELECT "
    "(SELECT COUNT(*) FROM mt_queue WHERE device=?1 AND state<>'delivered') + "
    "(SELECT COUNT(*) FROM mt_out WHERE device=?1 AND state<>'delivered_to_handset'), "
    "(SELECT COUNT(*) FROM call_log WHERE device=?1)";

/** Посчитать недоставленные SMS и все вызовы для одной SIM одним read-only SQL statement.
 * Общий mutex удерживает вызывающий smsdb wrapper; локальный statement принадлежит функции.
 * Любая SQL ошибка возвращается как -1 и не подменяется нулевыми счётчиками.
 */
int sim_metrics_read(sqlite3 *db, const char *device, struct sim_metrics *out)
{
    sqlite3_stmt *stmt = NULL;
    int rc;
    if (!db || !device || !*device || !out) return -1;
    if (sqlite3_prepare_v2(db, METRICS_SQL, -1, &stmt, NULL) != SQLITE_OK) return -1;
    if (sqlite3_bind_text(stmt, 1, device, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        out->sms_undelivered = sqlite3_column_int(stmt, 0);
        out->calls_total = sqlite3_column_int(stmt, 1);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK) return -1;
    return rc == SQLITE_ROW ? 0 : -1;
}
