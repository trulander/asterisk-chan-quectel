/*
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief Dependency-free SQLite transaction primitives and RAII scope.
 *
 * The chan_quectel SMS layer keeps a single sqlite3 connection (the module
 * global `smsdb`) shared by every thread. A transaction on that connection is
 * only safe when the recursive per-connection mutex (sqlite3_db_mutex) is held
 * across the whole BEGIN..COMMIT window; otherwise concurrent operations
 * interleave and a second BEGIN fails with
 * "cannot start a transaction within a transaction" (the nested-BEGIN bug that
 * shows up as pairs of different thread-ids in messages).
 *
 * These helpers are intentionally free of any Asterisk dependency so the exact
 * same code drives production (smsdb.c, via SMS_TX_SCOPE) and the unit test
 * (test/sms_tx_test.c, via its own SMS_TX_LOG mapping).
 */

#ifndef CHAN_QUECTEL_SMS_TX_H_INCLUDED
#define CHAN_QUECTEL_SMS_TX_H_INCLUDED

#include <pthread.h>
#include <sqlite3.h>

/*! Logging hook used by the transaction scope. Production maps it to ast_log
 * (see smsdb.c) so every BEGIN/COMMIT/ROLLBACK failure is visible in messages
 * with the thread-id and the calling function name; the unit test maps it to
 * printf. TU must #define this before including this header. */
#ifndef SMS_TX_LOG
#define SMS_TX_LOG(fmt, ...) do {} while (0)
#endif

/*! Acquire the recursive per-connection database mutex. Returns NULL when the
 * connection exposes no mutex (then the caller proceeds without serialization). */
sqlite3_mutex* sms_tx_lock(sqlite3* db);

/*! Release a mutex previously returned by sms_tx_lock(NULL-safe). */
void sms_tx_unlock(sqlite3_mutex* mtx);

/*! BEGIN a transaction. Returns SQLITE_OK or an sqlite error code; on failure
 * the connection is left with no open transaction (nothing half-open). */
int sms_tx_begin(sqlite3* db);

/*! COMMIT an open transaction. begin_res must be SQLITE_OK (a successful BEGIN).
 * On COMMIT failure the transaction is rolled back so the connection is never
 * left half-open, and the COMMIT error code is returned (never silently
 * swallowed). Returns SQLITE_OK on success, begin_res when nothing was opened. */
int sms_tx_commit(sqlite3* db, int begin_res);

/*! sqlite3_column_int that returns null_sentinel for a NULL column instead of 0.
 * Used to render "no delivery report yet" truthfully (-1) rather than as a real
 * delivered code (0). */
int sms_tx_column_int_or_null(sqlite3_stmt* stmt, int col, int null_sentinel);

/*! State carried across one transaction scope: the connection, the BEGIN result
 * and the context (calling function name + thread-id + a short context tag such
 * as the address/refid) for error logging so races are pinpointable in messages. */
struct sms_tx_scope {
    sqlite3* db;
    int res;
    const char* func;
    const char* ctx;
    pthread_t tid;
};

/*! Commit helper for SMS_TX_SCOPE. Commits the open transaction and, on COMMIT
 * failure, rolls it back so the connection is never left half-open. The commit
 * error is logged (never swallowed) but, as a cleanup function, cannot propagate
 * it to the enclosing function. Defined once as static inline (not per macro
 * expansion) so repeated SMS_TX_SCOPE uses in one TU do not redefine it. */
static inline void _sms_tx_close(struct sms_tx_scope* const s)
{
    if (!s || s->res != SQLITE_OK) {
        return;
    }

    const int _c = sms_tx_commit(s->db, s->res);
    if (_c != SQLITE_OK) {
        SMS_TX_LOG("[tid=%lu] %s [%s]: COMMIT failed [%d] %s; rolled back\n", \
            (unsigned long)s->tid, s->func, s->ctx, _c, sqlite3_errmsg(s->db)); \
    }
}

/*! Mutex release helper for SMS_TX_SCOPE. NULL-safe: releasing a NULL mutex is a
 * no-op, so a connection with no per-connection mutex is handled cleanly. Defined
 * once as static inline for the same reason as _sms_tx_close above. */
static inline void _sms_tx_mtx_close(sqlite3_mutex* const* const m)
{
    sms_tx_unlock(*m);
}

/*! RAII transaction scope.
 *
 *   SMS_TX_SCOPE(dbtrans, smsdb);
 *   SMS_TX_SCOPE(dbtrans, smsdb, addr);   // optional short context tag
 *
 * Holds the recursive per-connection mutex across the whole BEGIN..COMMIT window
 * (acquire before BEGIN, release after COMMIT) and commits at scope exit,
 * rolling back on commit failure. Mirrors Asterisk's RAII_VAR cleanup idiom but
 * is dependency-free, so the very same macro drives production and the test.
 *
 * On a failed BEGIN the scope logs the error (thread-id + function name +
 * context tag + sqlite message) and the enclosing function returns -1; the
 * mutex is released either way. Declared variables are cleaned up in reverse
 * order, so COMMIT runs while the mutex is still held. The context tag is
 * optional (defaults to "?"). The macro only attaches the shared static inline
 * cleanup helpers above to its own local variables through the cleanup attribute. */
#define SMS_TX_SCOPE(varname, dbname, ...) SMS_TX_SCOPE_(varname, dbname, "?", ##__VA_ARGS__)
#define SMS_TX_SCOPE_(varname, dbname, ctxtag, ...)                                \
    sqlite3_mutex* const _sms_tx_mtx_##varname __attribute__((cleanup(_sms_tx_mtx_close))) \
        = sms_tx_lock(dbname);                                                \
    struct sms_tx_scope varname __attribute__((cleanup(_sms_tx_close))) \
        = { dbname, sms_tx_begin(dbname), __func__, ctxtag, pthread_self() };    \
    if (varname.res != SQLITE_OK) {                                           \
        SMS_TX_LOG("[tid=%lu] %s [%s]: BEGIN failed [%d] %s\n",               \
            (unsigned long)varname.tid, varname.func, varname.ctx, varname.res, \
            sqlite3_errmsg(varname.db));                                      \
        return -1;                                                            \
    }

#endif /* CHAN_QUECTEL_SMS_TX_H_INCLUDED */
