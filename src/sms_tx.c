/*
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Dependency-free SQLite transaction primitives (see sms_tx.h).
 *
 * Pure SQLite: no Asterisk dependency, so the exact same code is exercised by
 * the unit test against a real in-memory database.
 */

#include "sms_tx.h"

sqlite3_mutex* sms_tx_lock(sqlite3* db)
{
    if (!db) {
        return NULL;
    }

    /* sqlite3_db_mutex() is the recursive per-connection mutex. Holding it
     * across BEGIN..COMMIT serializes transactions on the shared connection and
     * prevents the nested-BEGIN race. */
    sqlite3_mutex* const mtx = sqlite3_db_mutex(db);
    if (mtx) {
        sqlite3_mutex_enter(mtx);
    }

    return mtx;
}

void sms_tx_unlock(sqlite3_mutex* const mtx)
{
    if (!mtx) {
        return;
    }
    sqlite3_mutex_leave(mtx);
}

int sms_tx_begin(sqlite3* db)
{
    char* errmsg = NULL;
    const int res = sqlite3_exec(db, "BEGIN TRANSACTION", NULL, NULL, &errmsg);

    if (res != SQLITE_OK) {
        /* Nothing was opened on failure, so there is no half-open transaction to
         * clean up; just drop the error message. */
        sqlite3_free(errmsg);
    }

    return res;
}

int sms_tx_commit(sqlite3* db, int begin_res)
{
    if (begin_res != SQLITE_OK) {
        /* BEGIN never succeeded: nothing to commit and nothing to roll back. */
        return begin_res;
    }

    char* errmsg = NULL;
    const int res = sqlite3_exec(db, "COMMIT TRANSACTION", NULL, NULL, &errmsg);

    if (res != SQLITE_OK) {
        /* COMMIT failed: roll back so the connection is not left in an open
         * transaction (which would make the next BEGIN fail with a nested-BEGIN
         * error - exactly the symptom seen in the field). The rollback error, if
         * any, is intentionally ignored: the connection is back to a clean state
         * either way and the caller already got the COMMIT error code. */
        sqlite3_exec(db, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
        sqlite3_free(errmsg);
    }

    return res;
}

int sms_tx_column_int_or_null(sqlite3_stmt* const stmt, int col, int null_sentinel)
{
    if (sqlite3_column_type(stmt, col) == SQLITE_NULL) {
        return null_sentinel;
    }

    return sqlite3_column_int(stmt, col);
}
