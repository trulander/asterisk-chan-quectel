/*
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief SMSdb
 *
 * \author Max von Buelow <max@m9x.de>
 * \author Mark Spencer <markster@digium.com>
 *
 * The original code is from the astdb prat of the Asterisk project.
 */

#include <dirent.h>
#include <signal.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "ast_config.h"

#include <asterisk/app.h>
#include <asterisk/logger.h>

#include "smsdb.h"

#include "sms_tx.h"
#include "sms_delete_state.h"
#include "sms_multipart.h"
#include "sms_incoming_schema.h"
#include "sms_incoming_paging.h"

/* Map the dependency-free transaction logging hook to ast_log so every
 * BEGIN/COMMIT/ROLLBACK failure surfaces in messages with the thread-id and the
 * calling function name (see the SMS_TX_SCOPE macro in sms_tx.h). */
#ifdef SMS_TX_LOG
#undef SMS_TX_LOG
#endif
#define SMS_TX_LOG(fmt, ...) ast_log(LOG_ERROR, fmt, __VA_ARGS__)

#include "chan_quectel.h"

static const size_t DBKEY_DEF_LEN = 32;

#define DEFINE_SQL_STATEMENT(s, sql)      \
    static sqlite3_stmt* s##_stmt = NULL; \
    static const char s##_sql[]   = sql;

#define DEFINE_INTERNAL_SQL_STATEMENT(s, sql) static const char s##_sql[] = sql;

// OPER: incoming_msg
DEFINE_SQL_STATEMENT(get_incoming_delete_state, "SELECT delete_status, delete_attempts, source_device, source_storage, source_index FROM incoming_msg WHERE rowid = ?")
// OPER: outgoing_msg
DEFINE_SQL_STATEMENT(put_outgoingmsg, "INSERT INTO outgoing_msg (dev, dst, message, cnt, expiration, srr) VALUES (?, ?, ?, ?, strftime('%s') + ?, ?)")
DEFINE_SQL_STATEMENT(del_outgoingmsg, "DELETE FROM outgoing_msg WHERE uid = ?")
DEFINE_SQL_STATEMENT(get_outgoingmsg_key, "SELECT dev, dst, srr FROM outgoing_msg WHERE uid = ?")
DEFINE_SQL_STATEMENT(get_outgoingmsg, "SELECT dst, message FROM outgoing_msg WHERE uid = ?")
DEFINE_SQL_STATEMENT(get_outgoingmsg_expired, "SELECT uid, dst, message FROM outgoing_msg WHERE expiration < strftime('%s') LIMIT 1")

// OPER: outgoing_ref
DEFINE_SQL_STATEMENT(put_outgoingref, "INSERT INTO outgoing_ref (key) VALUES (?)")
DEFINE_SQL_STATEMENT(set_outgoingref, "UPDATE outgoing_ref SET refid = (refid + 1) % 256 WHERE key = ?")
DEFINE_SQL_STATEMENT(get_outgoingref, "SELECT refid FROM outgoing_ref WHERE key = ?")

// OPER: outgoing_part
DEFINE_SQL_STATEMENT(put_outgoingpart, "INSERT INTO outgoing_part (key, msg, status) VALUES (?, ?, NULL)")
DEFINE_SQL_STATEMENT(del_outgoingpart, "DELETE FROM outgoing_part WHERE msg = ?")
DEFINE_SQL_STATEMENT(set_outgoingpart, "UPDATE outgoing_part SET status = ? WHERE rowid = ?")
DEFINE_SQL_STATEMENT(get_outgoingpart, "SELECT rowid, msg FROM outgoing_part WHERE key = ?")
DEFINE_SQL_STATEMENT(get_all_status, "SELECT status FROM outgoing_part WHERE msg = ? ORDER BY rowid")

// OPER: outgoing_msg, outgoing_part
DEFINE_SQL_STATEMENT(cnt_outgoingpart,
                     "SELECT m.cnt, (SELECT COUNT(p.rowid) FROM outgoing_part p "
                     "               WHERE p.msg = m.rowid AND "
                     "               (p.status & 64 != 0 OR p.status & 32 = 0)) "
                     "FROM outgoing_msg m WHERE m.rowid = ?")
DEFINE_SQL_STATEMENT(cnt_all_outgoingpart,
                     "SELECT m.cnt, (SELECT COUNT(p.rowid) FROM outgoing_part p "
                     "               WHERE p.msg = m.uid) "
                     "FROM outgoing_msg m "
                     "WHERE m.uid = ?")

// OPER: mt_queue (incoming SIM->SIP)
DEFINE_SQL_STATEMENT(get_mt_queue,
                     "SELECT device, mid, from_num, msg, ts, state, attempts, next_retry_at, last_result, created_at, sent_at, delivered_at "
                     "FROM mt_queue WHERE mid = ?")
DEFINE_SQL_STATEMENT(set_mt_queue,
                     "UPDATE mt_queue SET state = ?, attempts = ?, next_retry_at = ?, last_result = ?, sent_at = ?, delivered_at = ? "
                     "WHERE mid = ?")
DEFINE_SQL_STATEMENT(pick_mt_queue,
                     "SELECT device, mid, from_num, msg, ts, state, attempts, next_retry_at, last_result, created_at, sent_at, delivered_at "
                     "FROM mt_queue WHERE (state = 'new') OR (state = 'retrying' AND next_retry_at <= ? AND attempts < ?) "
                     "ORDER BY created_at LIMIT 1")
DEFINE_SQL_STATEMENT(list_mt_queue,
                     "SELECT device, mid, from_num, msg, ts, state, attempts, next_retry_at, last_result, created_at, sent_at, delivered_at "
                     "FROM mt_queue ORDER BY created_at DESC, ROWID DESC")

// OPER: mt_out (outgoing Android->SIM)
DEFINE_SQL_STATEMENT(add_mt_out,
                     "INSERT INTO mt_out (device, dst, msg, ts, imdn_id, state) VALUES (?, ?, ?, ?, ?, 'submitted_to_modem')")
DEFINE_SQL_STATEMENT(set_mt_out,
                     "UPDATE mt_out SET state = ?, srr_status = ?, updated_at = strftime('%s') WHERE id = ?")
DEFINE_SQL_STATEMENT(set_mt_out_mr,
                     "UPDATE mt_out SET mr = ?, updated_at = strftime('%s') WHERE id = ?")
DEFINE_SQL_STATEMENT(get_mt_out_by_mr,
                     "SELECT id FROM mt_out WHERE device = ? AND dst = ? AND mr = ? AND state IN ('submitted_to_modem', 'smsc_ok') "
                     "ORDER BY id DESC LIMIT 1")
DEFINE_SQL_STATEMENT(get_mt_out_imdn_id, "SELECT imdn_id FROM mt_out WHERE id = ?")
DEFINE_SQL_STATEMENT(list_mt_out,
                     "SELECT id, device, dst, msg, ts, state, srr_status, mr, imdn_id, created_at, updated_at "
                     "FROM mt_out ORDER BY created_at DESC, id DESC")

// OPER: unified SMS history (mt_queue MT + mt_out MO) is implemented by
// src/sms_paging.c (a pure SQLite module shared with the unit test).

static sqlite3* smsdb;

static int set_ast_str(sqlite3_stmt* stmt, int colno, struct ast_str** str)
{
    if (!str || !*str) {
        return -1;
    }

    return ast_str_set(str, 0, "%.*s", sqlite3_column_bytes(stmt, colno), sqlite3_column_text(stmt, colno));
}

static int bind_ast_str(sqlite3_stmt* stmt, int colno, const struct ast_str* const str)
{
    return sqlite3_bind_text(stmt, colno, ast_str_buffer(str), ast_str_strlen(str), SQLITE_TRANSIENT);
}

#if SQLITE_VERSION_NUMBER >= 3020000

//
// sqlite3_prepare_v3 function have been introduced in version 3.20.0 (2017-08-01) of SQLite
//
static int init_stmt(sqlite3_stmt** stmt, const char* sql, size_t len)
{
    const int res = sqlite3_prepare_v3(smsdb, sql, len, SQLITE_PREPARE_PERSISTENT, stmt, NULL);
    if (res != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't prepare statement '%s': [%d] %s\n", sql, res, sqlite3_errmsg(smsdb));
        return -1;
    }

    return 0;
}

#else

//
// sqlite3_prepare_v2 function have been introduced in version 3.3.9 (2007-01-04) of SQLite
//
static int init_stmt(sqlite3_stmt** stmt, const char* sql, size_t len)
{
    const int res = sqlite3_prepare_v2(smsdb, sql, len, stmt, NULL);
    if (res != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't prepare statement '%s': [%d] %s\n", sql, res, sqlite3_errmsg(smsdb));
        return -1;
    }

    return 0;
}

#endif

#define INIT_STMT(s) init_stmt(&s##_stmt, s##_sql, sizeof(s##_sql))
#define INIT_VSTMT(s) const int s##_res = init_stmt(&s##_stmt, s##_sql, sizeof(s##_sql))

/* We purposely don't lock around the sqlite3 call because the transaction
 * calls will be called with the database lock held. For any other use, make
 * sure to take the dblock yourself. */
static int execute_sql(const char* sql, int (*callback)(void*, int, char**, char**), void* arg)
{
    char* errmsg  = NULL;
    const int res = sqlite3_exec(smsdb, sql, callback, arg, &errmsg);

    if (res != SQLITE_OK) {
        ast_log(LOG_WARNING, "Error executing SQL (%s): [%d] %s\n", sql, res, errmsg);
        sqlite3_free(errmsg);
    }

    return res;
}

static int execute_ast_str(const struct ast_str* const str) { return execute_sql(ast_str_buffer(str), NULL, NULL); }

#define EXECUTE_STMT(s) execute_sql(s##_sql, NULL, NULL)
#define EXECUTE_VSTMT(s) const int s##_res = execute_sql(s##_sql, NULL, NULL)

/*! \internal
 * \brief Clean up the prepared SQLite3 statement
 * \note dblock should already be locked prior to calling this method
 */
static int clean_stmt(sqlite3_stmt** stmt, const char* sql)
{
    const int res = sqlite3_finalize(*stmt);
    if (res != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't finalize statement '%s': [%d] %s\n", sql, res, sqlite3_errmsg(smsdb));
        *stmt = NULL;
        return -1;
    }
    *stmt = NULL;
    return 0;
}

#define CLEAN_STMT(s) clean_stmt(&s##_stmt, s##_sql)

/* Transaction primitives and the RAII SMS_TX_SCOPE macro live in src/sms_tx.h:
 * they hold the recursive per-connection mutex across the whole BEGIN..COMMIT
 * window (fixing the nested-BEGIN race) and roll back on a failed COMMIT instead
 * of silently leaking an open transaction. SCOPED_DB below is kept as a thin
 * wrapper over the same mutex for the many single-statement operations. */

#define SCOPED_DB(d) RAII_VAR(sqlite3_mutex*, d##_mtx, sms_tx_lock(d), sms_tx_unlock)

static sqlite3_stmt* stmt_begin(int level, const char* file, int line, const char* function, sqlite3_stmt* const stmt)
{
    if (!stmt) {
        ast_log(level, file, line, function, "Trying to begin uninitialized statement\n");
        return NULL;
    }

    const int res = sqlite3_clear_bindings(stmt);
    if (res != SQLITE_OK) {
        ast_log(level, file, line, function, "Fail to clear bindings: [%d] %s\n", res, sqlite3_errmsg(smsdb));
    }

    return stmt;
}

static void stmt_end(int level, const char* file, int line, const char* function, sqlite3_stmt* const stmt)
{
    if (!stmt) {
        return;
    }

    const int res = sqlite3_reset(stmt);
    if (res != SQLITE_OK) {
        ast_log(level, file, line, function, "Fail to reset statement: [%d] %s\n", res, sqlite3_errmsg(smsdb));
    }
}

#define SCOPED_STMT(s)                                                       \
    auto void _stmt_dtor_##s(sqlite3_stmt* const*);                          \
    void _stmt_dtor_##s(sqlite3_stmt* const* s) { stmt_end(LOG_ERROR, *s); } \
    sqlite3_stmt* const s __attribute__((cleanup(_stmt_dtor_##s))) = stmt_begin(LOG_WARNING, s##_stmt)

/** Открыть одну schema transaction под SMS_TX_SCOPE; любая DDL/migration ошибка откатывает scope. */
static int db_create(void)
{
    // TABLE: outgoing_msg(KEY: IMSI/DEST_ADDR)
    DEFINE_INTERNAL_SQL_STATEMENT(create_outgoingmsg,
                                  "CREATE TABLE IF NOT EXISTS outgoing_msg (uid INTEGER PRIMARY KEY AUTOINCREMENT,"
                                  "dev VARCHAR(256), dst VARCHAR(256), message VARCHAR(256), cnt INTEGER, expiration TIMESTAMP, srr BOOLEAN)")

    // TABLE: outgoing_ref
    DEFINE_INTERNAL_SQL_STATEMENT(create_outgoingref, "CREATE TABLE IF NOT EXISTS outgoing_ref (key VARCHAR(256), refid INTEGER DEFAULT 0, PRIMARY KEY(key))")

    // TABLE: outgoing_part(KEY: IMSI/DEST_ADDR/MR)
    DEFINE_INTERNAL_SQL_STATEMENT(create_outgoingpart,
                                  "CREATE TABLE IF NOT EXISTS outgoing_part (key VARCHAR(256), msg INTEGER, status INTEGER, PRIMARY KEY(key))")
    DEFINE_INTERNAL_SQL_STATEMENT(create_outgoingpart_index, "CREATE INDEX IF NOT EXISTS outgoing_part_msg ON outgoing_part(msg)")

    // TABLE: mt_queue(device, mid) - incoming SIM->SIP delivery queue + history
    DEFINE_INTERNAL_SQL_STATEMENT(create_mt_queue,
                                  "CREATE TABLE IF NOT EXISTS mt_queue (device TEXT NOT NULL, mid TEXT PRIMARY KEY, from_num TEXT NOT NULL, msg TEXT NOT NULL,"
                                  "ts TEXT, state TEXT NOT NULL DEFAULT 'new', attempts INTEGER NOT NULL DEFAULT 0, next_retry_at INTEGER NOT NULL DEFAULT 0,"
                                  "last_result TEXT, created_at INTEGER NOT NULL DEFAULT (strftime('%s')), sent_at INTEGER, delivered_at INTEGER)")
    DEFINE_INTERNAL_SQL_STATEMENT(create_mt_queue_index, "CREATE INDEX IF NOT EXISTS mt_queue_pending ON mt_queue(state, next_retry_at)")

    // TABLE: mt_out(device, created_at) - outgoing Android->SIM delivery status
    DEFINE_INTERNAL_SQL_STATEMENT(create_mt_out,
                                  "CREATE TABLE IF NOT EXISTS mt_out (id INTEGER PRIMARY KEY AUTOINCREMENT, device TEXT NOT NULL, dst TEXT NOT NULL,"
                                  "msg TEXT NOT NULL, ts TEXT, state TEXT NOT NULL DEFAULT 'submitted_to_modem', srr_status INTEGER, mr INTEGER,"
                                  "imdn_id TEXT, created_at INTEGER NOT NULL DEFAULT (strftime('%s')), updated_at INTEGER)")
    DEFINE_INTERNAL_SQL_STATEMENT(create_mt_out_index, "CREATE INDEX IF NOT EXISTS mt_out_dev ON mt_out(device, created_at)")
    DEFINE_INTERNAL_SQL_STATEMENT(alter_mt_out_imdn_id, "ALTER TABLE mt_out ADD COLUMN imdn_id TEXT")

    SCOPED_DB(smsdb);
    SMS_TX_SCOPE(dbtrans, smsdb, "schema");

    if (sms_incoming_schema_ensure(smsdb)) {
        ast_log(LOG_ERROR, "smsdb: incoming_msg schema creation/migration failed\n");
        sqlite3_exec(smsdb, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
        dbtrans.res = SQLITE_ERROR;
        return -1;
    }
    EXECUTE_VSTMT(create_outgoingmsg);
    EXECUTE_VSTMT(create_outgoingref);
    EXECUTE_VSTMT(create_outgoingpart);
    EXECUTE_VSTMT(create_outgoingpart_index);
    EXECUTE_VSTMT(create_mt_queue);
    EXECUTE_VSTMT(create_mt_queue_index);
    EXECUTE_VSTMT(create_mt_out);
    EXECUTE_VSTMT(create_mt_out_index);

    /* Best-effort migration for databases created before imdn_id existed:
     * the ALTER fails with "duplicate column name" on already-migrated
     * tables - ignore that case and continue initialization either way. */
    {
        char* errmsg = NULL;
        const int mig = sqlite3_exec(smsdb, alter_mt_out_imdn_id_sql, NULL, NULL, &errmsg);
        if (mig != SQLITE_OK) {
            if (errmsg && strstr(errmsg, "duplicate column")) {
                ast_log(LOG_DEBUG, "smsdb: mt_out.imdn_id already present (%s)\n", errmsg);
            } else {
                ast_log(LOG_NOTICE, "smsdb: could not add mt_out.imdn_id (continuing): %s\n", S_OR(errmsg, "?"));
            }
            sqlite3_free(errmsg);
        }
    }

    const int create_result = create_outgoingmsg_res || create_outgoingref_res || create_outgoingpart_res ||
                              create_outgoingpart_index_res || create_mt_queue_res || create_mt_queue_index_res || create_mt_out_res || create_mt_out_index_res;
    if (create_result) {
        sqlite3_exec(smsdb, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
        dbtrans.res = SQLITE_ERROR;
    }
    return create_result;
}

static int db_init_statements(void)
{
    /* Don't initialize create_smsdb_statement here as the smsdb table needs to exist
     * brefore these statements can be initialized */

    SCOPED_DB(smsdb);

    INIT_VSTMT(get_incoming_delete_state);
    INIT_VSTMT(put_outgoingref);
    INIT_VSTMT(set_outgoingref);
    INIT_VSTMT(get_outgoingref);
    INIT_VSTMT(put_outgoingmsg);
    INIT_VSTMT(put_outgoingpart);
    INIT_VSTMT(del_outgoingmsg);
    INIT_VSTMT(del_outgoingpart);
    INIT_VSTMT(get_outgoingmsg_key);
    INIT_VSTMT(get_outgoingpart);
    INIT_VSTMT(set_outgoingpart);
    INIT_VSTMT(cnt_outgoingpart);
    INIT_VSTMT(cnt_all_outgoingpart);
    INIT_VSTMT(get_outgoingmsg);
    INIT_VSTMT(get_all_status);
    INIT_VSTMT(get_outgoingmsg_expired);
    INIT_VSTMT(get_mt_queue);
    INIT_VSTMT(set_mt_queue);
    INIT_VSTMT(pick_mt_queue);
    INIT_VSTMT(list_mt_queue);
    INIT_VSTMT(add_mt_out);
    INIT_VSTMT(set_mt_out);
    INIT_VSTMT(set_mt_out_mr);
    INIT_VSTMT(get_mt_out_by_mr);
    INIT_VSTMT(get_mt_out_imdn_id);
    INIT_VSTMT(list_mt_out);

    return get_incoming_delete_state_res ||
           put_outgoingref_res || set_outgoingref_res ||
           get_outgoingref_res || put_outgoingmsg_res || put_outgoingpart_res || del_outgoingmsg_res || del_outgoingpart_res || get_outgoingmsg_key_res ||
           get_outgoingpart_res || set_outgoingpart_res || cnt_outgoingpart_res || cnt_all_outgoingpart_res || get_outgoingmsg_res || get_all_status_res ||
           get_outgoingmsg_expired_res || get_mt_queue_res || set_mt_queue_res || pick_mt_queue_res || list_mt_queue_res || add_mt_out_res ||
           set_mt_out_res || set_mt_out_mr_res || get_mt_out_by_mr_res || get_mt_out_imdn_id_res || list_mt_out_res;
}

static void db_clean_statements(void)
{
    SCOPED_DB(smsdb);

    CLEAN_STMT(get_incoming_delete_state);
    CLEAN_STMT(put_outgoingref);
    CLEAN_STMT(set_outgoingref);
    CLEAN_STMT(get_outgoingref);
    CLEAN_STMT(put_outgoingmsg);
    CLEAN_STMT(put_outgoingpart);
    CLEAN_STMT(del_outgoingmsg);
    CLEAN_STMT(del_outgoingpart);
    CLEAN_STMT(get_outgoingmsg_key);
    CLEAN_STMT(set_outgoingpart);
    CLEAN_STMT(get_outgoingpart);
    CLEAN_STMT(cnt_outgoingpart);
    CLEAN_STMT(cnt_all_outgoingpart);
    CLEAN_STMT(get_outgoingmsg);
    CLEAN_STMT(get_all_status);
    CLEAN_STMT(get_outgoingmsg_expired);
    CLEAN_STMT(get_mt_queue);
    CLEAN_STMT(set_mt_queue);
    CLEAN_STMT(pick_mt_queue);
    CLEAN_STMT(list_mt_queue);
    CLEAN_STMT(add_mt_out);
    CLEAN_STMT(set_mt_out);
    CLEAN_STMT(set_mt_out_mr);
    CLEAN_STMT(get_mt_out_by_mr);
    CLEAN_STMT(get_mt_out_imdn_id);
    CLEAN_STMT(list_mt_out);
}

static int db_name_in_memory(const char* db)
{
    static const char SQLITE_IN_MEMORY_SPECIAL_NAME[] = ":memory:";

    if (!db) {
        return 0;
    }
    return !strncmp(db, SQLITE_IN_MEMORY_SPECIAL_NAME, STRLEN(SQLITE_IN_MEMORY_SPECIAL_NAME));
}

static int db_name_temporary(const char* db)
{
    static const char SQLITE_TMP_SPECIAL_NAME[] = ":temporary:";

    if (!db) {
        return 0;
    }
    return !strncmp(db, SQLITE_TMP_SPECIAL_NAME, STRLEN(SQLITE_TMP_SPECIAL_NAME));
}

static int db_open_url(const char* url)
{
    //
    // sqlite3_open_v2 function have been introduced in version 3.5.0 (2007-09-04) of SQLite
    //
    const int res = sqlite3_open_v2(url, &smsdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (res != SQLITE_OK) {
        ast_log(LOG_WARNING, "Unable to open database '%s': [%d] %s\n", url, res, sqlite3_errmsg(smsdb));
        sqlite3_close(smsdb);
        return -1;
    }

    return 0;
}

static int db_open(void)
{
    static const size_t DBNAME_DEF_LEN = 32;

    if (db_name_in_memory(CONF_GLOBAL(sms_db))) {
        return db_open_url(CONF_GLOBAL(sms_db));
    } else if (db_name_temporary(CONF_GLOBAL(sms_db))) {
        return db_open_url("");
    } else {
        RAII_VAR(struct ast_str*, dbname, ast_str_create(DBNAME_DEF_LEN), ast_free);
        ast_str_set(&dbname, 0, "%s.sqlite3", CONF_GLOBAL(sms_db));
        return db_open_url(ast_str_buffer(dbname));
    }
}

static int db_init()
{
    if (smsdb) {
        return 0;
    }

    if (db_open() || db_create() || db_init_statements()) {
        return -1;
    }

    return 0;
}


/*!
 * \brief Adds a message part into the DB and returns the whole message into 'out' when the message is complete.
 * \param id -- Some ID for the device or so, e.g. the IMSI
 * \param addr -- The sender address
 * \param ref -- The reference ID
 * \param parts -- The total number of messages
 * \param order -- The current message number
 * \param msg -- The current message part
 * \param out -- Output: Only written if parts == cnt
 * \retval <=0 Error
 * \retval >0 Current number of messages in the DB
 */
/** Основная put transaction: сохранить SIM-сегмент, собрать лишь при ожидаемом count,
 * при необходимости удерживать source rows. Mutex принадлежит операции от BEGIN до
 * COMMIT/ROLLBACK; SQLite-буфер assembly освобождается на каждом failure пути.
 */
static int smsdb_put_internal(const char* id, const char* addr, int ref, int parts, int order, const char* msg, const char* source_device,
                              const char* storage, int index, int retain_parts, struct ast_str** out, long long* rowid)
{
    RAII_VAR(struct ast_str*, fullkey, ast_str_create(DBKEY_DEF_LEN), ast_free);
    const int key_result = (parts == 1 && !ast_strlen_zero(storage) && index >= 0)
                               ? ast_str_set(&fullkey, 0, "%s/%s/%d/%d/%s/%d", id, addr, ref, parts, storage, index)
                               : ast_str_set(&fullkey, 0, "%s/%s/%d/%d", id, addr, ref, parts);
    if (key_result < 0) {
        ast_log(LOG_ERROR, "Fail to create key\n");
        return -1;
    }

    int res;
    /* Hold the shared connection mutex across BEGIN, helper statements, and the
     * explicit COMMIT/ROLLBACK below (the previous tx fix's serialization rule). */
    sqlite3_mutex* const db_mtx __attribute__((cleanup(_sms_tx_mtx_close))) = sms_tx_lock(smsdb);
    const int begin_res = sms_tx_begin(smsdb);
    if (begin_res != SQLITE_OK) {
        SMS_TX_LOG("[tid=%lu] %s [%s]: BEGIN failed [%d] %s\n", (unsigned long)pthread_self(), __func__, addr, begin_res,
                   sqlite3_errmsg(smsdb));
        return -1;
    }

    char* assembled = NULL;
    res = sms_multipart_put(smsdb, ast_str_buffer(fullkey), order, parts, CONF_GLOBAL(sms_ttl), msg, source_device, storage, index, retain_parts,
                           &assembled, rowid);
    if (res == parts && assembled && ast_str_set(out, 0, "%s", assembled) < 0) {
        res = -1;
    }
    sqlite3_free(assembled);

    if (res <= 0) {
        sqlite3_exec(smsdb, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
        return -1;
    }

    const int commit_res = sms_tx_commit(smsdb, begin_res);
    if (commit_res != SQLITE_OK) {
        SMS_TX_LOG("[tid=%lu] %s [%s]: COMMIT failed [%d] %s; rolled back\n", (unsigned long)pthread_self(), __func__, addr, commit_res,
                   sqlite3_errmsg(smsdb));
        return -1;
    }

    return res;
}

/** Legacy receive API: non-SIM callers use the same transaction without a deletable source cell. */
int smsdb_put(const char* id, const char* addr, int ref, int parts, int order, const char* msg, struct ast_str** out)
{
    return smsdb_put_internal(id, addr, ref, parts, order, msg, NULL, NULL, -1, 0, out, NULL);
}

/** SIM-aware wrapper: отклоняет storage кроме SM и выдаёт durable rowid для delete flow. */
int smsdb_put_sim(const char* id, const char* addr, int ref, int parts, int order, const char* msg, const char* storage, int index,
                  struct ast_str** out, long long* rowid)
{
    if (ast_strlen_zero(storage) || strcmp(storage, "SM") || index < 0) {
        return -1;
    }
    return smsdb_put_internal(id, addr, ref, parts, order, msg, id, storage, index, 1, out, rowid);
}

/** Построить stable multipart key из modem/SMS полей; ast_str остаётся во владении caller. */
static int smsdb_group_key(struct ast_str** key, const char* id, const char* addr, int ref, int parts, const char* storage, int index)
{
    if (parts == 1 && !ast_strlen_zero(storage) && index >= 0) {
        return ast_str_set(key, 0, "%s/%s/%d/%d/%s/%d", id, addr, ref, parts, storage, index) < 0 ? -1 : 0;
    }
    return ast_str_set(key, 0, "%s/%s/%d/%d", id, addr, ref, parts) < 0 ? -1 : 0;
}

/** Под общей DB transaction выполнить sweep только для queued и полностью complete групп. */
static int smsdb_gc_completed_groups(void)
{
    return sms_incoming_gc_completed_groups(smsdb);
}

/** Проверить eligibility и условно GC одну stable group; вызывается при удержанном mutex. */
static int smsdb_maybe_gc_group(const struct ast_str* key)
{
    return sms_incoming_gc_group(smsdb, ast_str_buffer(key));
}

/** Сформировать group key и получить прежний committed MID до генерации candidate identity. */
int smsdb_incoming_group_mt_mid(const char* id, const char* addr, int ref, int parts, const char* storage, int index, char* mid, size_t mid_len)
{
    RAII_VAR(struct ast_str*, key, ast_str_create(DBKEY_DEF_LEN), ast_free);
    int queued = 0;
    if (!mid || !mid_len || smsdb_group_key(&key, id, addr, ref, parts, storage, index)) return -1;
    mid[0] = '\0';
    SCOPED_DB(smsdb);
    if (sms_incoming_group_queue_state(smsdb, ast_str_buffer(key), &queued, mid, mid_len)) return -1;
    return queued ? 1 : 0;
}

/** Удерживать mutex DB от BEGIN через marker lookup, MT insert, marker, GC до commit/rollback. */
int smsdb_mt_queue_incoming_group(const char* id, const char* addr, int ref, int parts, const char* storage, int index,
                                  const char* candidate_mid, const char* from_num, const char* message, const char* timestamp,
                                  char* effective_mid, size_t effective_mid_len, int* inserted)
{
    RAII_VAR(struct ast_str*, key, ast_str_create(DBKEY_DEF_LEN), ast_free);
    char committed_mid[MT_MID_LEN];
    int did_insert = 0;
    if (!candidate_mid || ast_strlen_zero(candidate_mid) || !effective_mid || !effective_mid_len || !inserted ||
        smsdb_group_key(&key, id, addr, ref, parts, storage, index)) return -1;
    sqlite3_mutex* const db_mtx __attribute__((cleanup(_sms_tx_mtx_close))) = sms_tx_lock(smsdb);
    const int begin_res = sms_tx_begin(smsdb);
    if (begin_res != SQLITE_OK) return -1;
    if (sms_incoming_mt_queue_group(smsdb, ast_str_buffer(key), id, candidate_mid, from_num, message, timestamp,
                                    committed_mid, sizeof(committed_mid), &did_insert)) {
        sqlite3_exec(smsdb, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
        return -1;
    }
    if (sms_tx_commit(smsdb, begin_res) != SQLITE_OK) return -1;
    if (strlen(committed_mid) >= effective_mid_len) return -1;
    ast_copy_string(effective_mid, committed_mid, effective_mid_len);
    *inserted = did_insert;
    return 0;
}

/** Сохранить один state transition и подходящий group GC в общей mutex-held transaction. */
int smsdb_incoming_delete_apply(long long rowid, int event, int retry_at, int* status, unsigned* attempts)
{
    int res;
    char source_device[DEVNAMELEN], source_storage[8];
    int source_index;
    int current_status, next_status;
    unsigned current_attempts, next_attempts;
    /* Read-transition-update-GC must be atomic against other DB users. The
     * mutex remains held until explicit commit/rollback returns. */
    sqlite3_mutex* const db_mtx __attribute__((cleanup(_sms_tx_mtx_close))) = sms_tx_lock(smsdb);
    const int begin_res = sms_tx_begin(smsdb);
    if (begin_res != SQLITE_OK) {
        return -1;
    }
    {
        SCOPED_STMT(get_incoming_delete_state);
        if ((res = sqlite3_bind_int64(get_incoming_delete_state, 1, rowid)) != SQLITE_OK || sqlite3_step(get_incoming_delete_state) != SQLITE_ROW) {
            sqlite3_exec(smsdb, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
            return -1;
        }
        current_status = sqlite3_column_int(get_incoming_delete_state, 0);
        current_attempts = (unsigned)sqlite3_column_int(get_incoming_delete_state, 1);
        ast_copy_string(source_device, (const char*)sqlite3_column_text(get_incoming_delete_state, 2), sizeof(source_device));
        ast_copy_string(source_storage, (const char*)sqlite3_column_text(get_incoming_delete_state, 3), sizeof(source_storage));
        source_index = sqlite3_column_int(get_incoming_delete_state, 4);
    }
    sms_delete_state_t next = {(sms_delete_status_t)current_status, current_attempts};
    if (sms_delete_state_apply(&next, (sms_delete_event_t)event)) {
        sqlite3_exec(smsdb, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
        return -1;
    }
    next_status = (int)next.status;
    next_attempts = next.attempts;
    if (sms_incoming_delete_state_update(smsdb, rowid, source_device, source_storage, source_index,
                                        next_status, next_attempts, retry_at)) {
        sqlite3_exec(smsdb, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
        return -1;
    }
    if (next.status == SMS_DELETE_COMPLETE && smsdb_gc_completed_groups()) {
        sqlite3_exec(smsdb, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
        return -1;
    }
    if (sms_tx_commit(smsdb, begin_res) != SQLITE_OK) {
        return -1;
    }
    if (status) {
        *status = next_status;
    }
    if (attempts) {
        *attempts = next_attempts;
    }
    return 0;
}

/** Кратко захватить mutex для выбора due SM row; modem/AT действия под lock не выполняются. */
int smsdb_incoming_delete_job(long long* rowid, char* device, size_t device_len, int* index)
{
    if (!rowid || !device || !device_len || !index) {
        return -1;
    }
    SCOPED_DB(smsdb);
    return sms_incoming_delete_job_pick(smsdb, (int)time(NULL), rowid, device, device_len, index);
}

/** Сохранить только retry_at fairness defer под DB mutex, не меняя status/attempts. */
int smsdb_incoming_delete_defer(long long rowid, int retry_at)
{
    sqlite3_mutex* const db_mtx __attribute__((cleanup(_sms_tx_mtx_close))) = sms_tx_lock(smsdb);
    const int begin_res = sms_tx_begin(smsdb);
    if (begin_res != SQLITE_OK) return -1;
    if (sms_incoming_delete_retry_defer(smsdb, rowid, retry_at)) {
        sqlite3_exec(smsdb, "ROLLBACK TRANSACTION", NULL, NULL, NULL);
        return -1;
    }
    return sms_tx_commit(smsdb, begin_res) == SQLITE_OK ? 0 : -1;
}

static int get_refid(const struct ast_str* const fullkey)
{
    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(get_outgoingref);

    if ((res = bind_ast_str(get_outgoingref, 1, fullkey)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind key to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    switch (res = sqlite3_step(get_outgoingref)) {
        case SQLITE_ROW:
            return sqlite3_column_int(get_outgoingref, 0);

        case SQLITE_DONE:
            return -2;

        default:
            return -1;
    }
}

static int inc_refid(const struct ast_str* const fullkey, int use_insert)
{
    int res;

    sqlite3_stmt* const outgoingref_stmt = use_insert ? put_outgoingref_stmt : set_outgoingref_stmt;

    SCOPED_DB(smsdb);
    SCOPED_STMT(outgoingref);

    if ((res = bind_ast_str(outgoingref, 1, fullkey)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind key to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(outgoingref)) != SQLITE_DONE) {
        return -1;
    }

    return 0;
}

int smsdb_get_refid(const char* id, const char* addr)
{
    RAII_VAR(struct ast_str*, fullkey, ast_str_create(DBKEY_DEF_LEN), ast_free);
    if (ast_str_set(&fullkey, 0, "%s/%s", id, addr) < 0) {
        ast_log(LOG_ERROR, "Fail to create key\n");
        return -1;
    }

    int res, use_insert = 0;

    SMS_TX_SCOPE(dbtrans, smsdb, addr);

    if ((res = get_refid(fullkey)) == -2) {
        res        = 0;
        use_insert = 1;
    }

    if (res < 0) {
        return res;
    }

    inc_refid(fullkey, use_insert);
    return (res + 1) % 256;
}

int smsdb_outgoing_add(const char* id, const char* addr, const char* msg, int cnt, int ttl, int srr)
{
    int res;

    SCOPED_DB(smsdb);
    SMS_TX_SCOPE(dbtrans, smsdb, id);
    SCOPED_STMT(put_outgoingmsg);

    if ((res = sqlite3_bind_text(put_outgoingmsg, 1, id, strlen(id), SQLITE_TRANSIENT)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind dev to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_bind_text(put_outgoingmsg, 2, addr, strlen(addr), SQLITE_TRANSIENT)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind destination address to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_bind_text(put_outgoingmsg, 3, msg, strlen(msg), SQLITE_TRANSIENT)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind message to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_bind_int(put_outgoingmsg, 4, cnt)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind count to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_bind_int(put_outgoingmsg, 5, ttl)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind TTL to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_bind_int(put_outgoingmsg, 6, srr)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind SRR to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(put_outgoingmsg)) != SQLITE_DONE) {
        return -1;
    }

    return sqlite3_last_insert_rowid(smsdb);
}

static int get_outmsg(int uid, struct ast_str** dst, struct ast_str** msg)
{
    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(get_outgoingmsg);

    if ((res = sqlite3_bind_int(get_outgoingmsg, 1, uid)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind key to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(get_outgoingmsg)) != SQLITE_ROW) {
        return -1;
    }

    res = set_ast_str(get_outgoingmsg, 0, dst);
    res = set_ast_str(get_outgoingmsg, 1, msg);

    return 0;
}

static int del_outmsg(int uid)
{
    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(del_outgoingmsg);

    if ((res = sqlite3_bind_int(del_outgoingmsg, 1, uid)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind UID to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(del_outgoingmsg)) != SQLITE_DONE) {
        return -1;
    }

    return 0;
}

static int del_outpart(int uid)
{
    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(del_outgoingpart);

    if ((res = sqlite3_bind_int(del_outgoingpart, 1, uid)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind UID to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(del_outgoingpart)) != SQLITE_DONE) {
        return -1;
    }

    return 0;
}

static int smsdb_outgoing_clear_nolock(int uid)
{
    if (del_outmsg(uid)) {
        return -1;
    }

    if (del_outpart(uid)) {
        return -1;
    }

    return 0;
}

int smsdb_outgoing_clear(int uid, struct ast_str** dst, struct ast_str** msg)
{
    SMS_TX_SCOPE(dbtrans, smsdb, "clear");

    if (get_outmsg(uid, dst, msg)) {
        return -1;
    }

    if (smsdb_outgoing_clear_nolock(uid)) {
        return -1;
    }

    return 0;
}

static int get_outmsg_key(int uid, int refid, struct ast_str** fullkey, int* srr)
{
    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(get_outgoingmsg_key);

    if ((res = sqlite3_bind_int(get_outgoingmsg_key, 1, uid)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind UID to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(get_outgoingmsg_key)) != SQLITE_ROW) {
        return -2;
    }

    const char* dev = (const char*)sqlite3_column_text(get_outgoingmsg_key, 0);
    const char* dst = (const char*)sqlite3_column_text(get_outgoingmsg_key, 1);
    *srr            = sqlite3_column_int(get_outgoingmsg_key, 2);

    if (ast_str_set(fullkey, 0, "%s/%s/%d", dev, dst, refid) < 0) {
        ast_log(LOG_ERROR, "Fail to create key\n");
        return -3;
    }

    return 0;
}

static int put_outpart(const struct ast_str* const fullkey, int uid)
{
    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(put_outgoingpart);

    if ((res = bind_ast_str(put_outgoingpart, 1, fullkey)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind key to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_bind_int(put_outgoingpart, 2, uid)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind UID to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(put_outgoingpart)) != SQLITE_DONE) {
        return -1;
    }

    return 0;
}

static int cnt_all_outpart(int uid)
{
    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(cnt_all_outgoingpart);

    if ((res = sqlite3_bind_int(cnt_all_outgoingpart, 1, uid)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind key to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(cnt_all_outgoingpart)) != SQLITE_ROW) {
        return -1;
    }

    const int cur = sqlite3_column_int(cnt_all_outgoingpart, 0);
    const int cnt = sqlite3_column_int(cnt_all_outgoingpart, 1);

    if (cur != cnt) {
        return -2;
    }

    return 0;
}

int smsdb_outgoing_part_put(int uid, int refid, struct ast_str** dst, struct ast_str** msg)
{
    int srr = -1;

    RAII_VAR(struct ast_str*, fullkey, ast_str_create(DBKEY_DEF_LEN), ast_free);

    SMS_TX_SCOPE(dbtrans, smsdb, "part_put");

    if (get_outmsg_key(uid, refid, &fullkey, &srr)) {
        return -1;
    }

    if (put_outpart(fullkey, uid)) {
        return -1;
    }

    if (!srr) {
        return -2;
    }

    if (cnt_all_outpart(uid)) {
        return -1;
    }

    if (get_outmsg(uid, dst, msg)) {
        return -1;
    }

    // clear if everything is finished
    if (smsdb_outgoing_clear_nolock(uid)) {
        return -1;
    }

    return 0;
}

static int get_outpart(const struct ast_str* const fullkey, int* partid, int* uid)
{
    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(get_outgoingpart);

    if ((res = bind_ast_str(get_outgoingpart, 1, fullkey)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind key to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(get_outgoingpart)) != SQLITE_ROW) {
        return -1;
    }

    *partid = sqlite3_column_int(get_outgoingpart, 0);
    *uid    = sqlite3_column_int(get_outgoingpart, 1);
    return 0;
}

static int set_outpart(int st, int partid)
{
    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(set_outgoingpart);

    if ((res = sqlite3_bind_int(set_outgoingpart, 1, st)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind status to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_bind_int(set_outgoingpart, 2, partid)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind ID to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(set_outgoingpart)) != SQLITE_DONE) {
        return -1;
    }

    return 0;
}

static int cnt_outpart(int uid)
{
    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(cnt_outgoingpart);

    if ((res = sqlite3_bind_int(cnt_outgoingpart, 1, uid)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind key to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(cnt_outgoingpart)) != SQLITE_ROW) {
        return -1;
    }

    const int cur = sqlite3_column_int(cnt_outgoingpart, 0);
    const int cnt = sqlite3_column_int(cnt_outgoingpart, 1);
    if (cur != cnt) {
        return -2;
    }

    return 0;
}

static int get_all_stats(int uid, int* status_all)
{
    int res, i = 0;

    SCOPED_DB(smsdb);
    SCOPED_STMT(get_all_status);

    if ((res = sqlite3_bind_int(get_all_status, 1, uid)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind key to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    while ((res = sqlite3_step(get_all_status)) == SQLITE_ROW) {
        status_all[i++] = sqlite3_column_int(get_all_status, 0);
    }
    status_all[i] = -1;
    return (res == SQLITE_DONE) ? 0 : -1;
}

int smsdb_outgoing_part_status(const char* id, const char* addr, int mr, int st, int* status_all)
{
    int partid = -1, uid = -1;

    RAII_VAR(struct ast_str*, fullkey, ast_str_create(DBKEY_DEF_LEN), ast_free);
    if (ast_str_set(&fullkey, 0, "%s/%s/%d", id, addr, mr) < 0) {
        ast_log(LOG_ERROR, "Fail to create key\n");
        return -1;
    }

    SMS_TX_SCOPE(dbtrans, smsdb, addr);

    if (get_outpart(fullkey, &partid, &uid)) {
        return -1;
    }

    if (set_outpart(st, partid)) {
        return -1;
    }

    if (cnt_outpart(uid)) {
        return -1;
    }

    if (get_all_stats(uid, status_all)) {
        return -1;
    }

    // clear if everything is finished
    if (smsdb_outgoing_clear_nolock(uid)) {
        return -1;
    }

    return 0;
}

static int get_outmsg_expired(int* uid, struct ast_str** dst, struct ast_str** msg)
{
    int res;

    SCOPED_STMT(get_outgoingmsg_expired);

    if ((res = sqlite3_step(get_outgoingmsg_expired)) != SQLITE_ROW) {
        return -1;
    }

    *uid = sqlite3_column_int(get_outgoingmsg_expired, 0);
    res  = set_ast_str(get_outgoingmsg_expired, 1, dst);
    res  = set_ast_str(get_outgoingmsg_expired, 2, msg);
    return 0;
}

int smsdb_outgoing_purge_one(int* uid, struct ast_str** dst, struct ast_str** msg)
{
    SMS_TX_SCOPE(dbtrans, smsdb, "purge");

    if (get_outmsg_expired(uid, dst, msg)) {
        return -1;
    }

    if (smsdb_outgoing_clear_nolock(*uid)) {
        return -1;
    }

    return 0;
}

// ================================================================
// MT helpers (incoming SIM->SIP queue and outgoing Android->SIM status)
// ================================================================

static void mt_row_fill(struct smsdb_mt_row* row, sqlite3_stmt* stmt)
{
    const char* const device     = (const char*)sqlite3_column_text(stmt, 0);
    const char* const mid        = (const char*)sqlite3_column_text(stmt, 1);
    const char* const from_num   = (const char*)sqlite3_column_text(stmt, 2);
    const char* const msg        = (const char*)sqlite3_column_text(stmt, 3);
    const char* const ts         = (const char*)sqlite3_column_text(stmt, 4);
    const char* const state      = (const char*)sqlite3_column_text(stmt, 5);
    const char* const last_result = (const char*)sqlite3_column_text(stmt, 8);

    ast_copy_string(row->device, S_OR(device, ""), sizeof(row->device));
    ast_copy_string(row->mid, S_OR(mid, ""), sizeof(row->mid));
    ast_copy_string(row->from_num, S_OR(from_num, ""), sizeof(row->from_num));
    ast_copy_string(row->msg, S_OR(msg, ""), sizeof(row->msg));
    ast_copy_string(row->ts, S_OR(ts, ""), sizeof(row->ts));
    ast_copy_string(row->state, S_OR(state, ""), sizeof(row->state));
    ast_copy_string(row->last_result, S_OR(last_result, ""), sizeof(row->last_result));
    row->attempts       = sqlite3_column_int(stmt, 6);
    row->next_retry_at  = sqlite3_column_int(stmt, 7);
    row->created_at     = (long)sqlite3_column_int64(stmt, 9);
    row->sent_at        = (long)sqlite3_column_int64(stmt, 10);
    row->delivered_at   = (long)sqlite3_column_int64(stmt, 11);
}

static void mt_out_row_fill(struct smsdb_mt_out_row* row, sqlite3_stmt* stmt)
{
    const char* const device = (const char*)sqlite3_column_text(stmt, 1);
    const char* const dst    = (const char*)sqlite3_column_text(stmt, 2);
    const char* const msg    = (const char*)sqlite3_column_text(stmt, 3);
    const char* const ts     = (const char*)sqlite3_column_text(stmt, 4);
    const char* const state  = (const char*)sqlite3_column_text(stmt, 5);
    const char* const imdn_id = (const char*)sqlite3_column_text(stmt, 8);

    row->id         = sqlite3_column_int(stmt, 0);
    ast_copy_string(row->device, S_OR(device, ""), sizeof(row->device));
    ast_copy_string(row->dst, S_OR(dst, ""), sizeof(row->dst));
    ast_copy_string(row->msg, S_OR(msg, ""), sizeof(row->msg));
    ast_copy_string(row->ts, S_OR(ts, ""), sizeof(row->ts));
    ast_copy_string(row->state, S_OR(state, ""), sizeof(row->state));
    ast_copy_string(row->imdn_id, S_OR(imdn_id, ""), sizeof(row->imdn_id));
    row->srr_status = sms_tx_column_int_or_null(stmt, 6, -1);
    row->mr         = sms_tx_column_int_or_null(stmt, 7, -1);
    row->created_at = (long)sqlite3_column_int64(stmt, 9);
    row->updated_at = (long)sqlite3_column_int64(stmt, 10);
}

static int bind_text_or_null(sqlite3_stmt* stmt, int colno, const char* text)
{
    if (ast_strlen_zero(text)) {
        return sqlite3_bind_null(stmt, colno);
    }
    return sqlite3_bind_text(stmt, colno, text, -1, SQLITE_TRANSIENT);
}

static int bind_int_or_null(sqlite3_stmt* stmt, int colno, int val)
{
    if (val < 0) {
        return sqlite3_bind_null(stmt, colno);
    }
    return sqlite3_bind_int(stmt, colno, val);
}

int smsdb_mt_put(const char* device, const char* mid, const char* from_num, const char* msg, const char* ts)
{
    if (ast_strlen_zero(device) || ast_strlen_zero(mid)) {
        return -1;
    }
    SCOPED_DB(smsdb);
    return sms_mt_queue_put(smsdb, device, mid, from_num, msg, ts);
}

int smsdb_mt_get(const char* mid, struct smsdb_mt_row* row)
{
    if (ast_strlen_zero(mid) || !row) {
        return -1;
    }

    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(get_mt_queue);

    if ((res = sqlite3_bind_text(get_mt_queue, 1, mid, -1, SQLITE_TRANSIENT)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind mid to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(get_mt_queue)) != SQLITE_ROW) {
        return -1;
    }

    mt_row_fill(row, get_mt_queue);
    return 0;
}

int smsdb_mt_set_result(const char* mid, const char* state, int attempts, int next_retry_at, const char* last_result, int sent_at, int delivered_at)
{
    if (ast_strlen_zero(mid) || ast_strlen_zero(state)) {
        return -1;
    }

    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(set_mt_queue);

    if ((res = sqlite3_bind_text(set_mt_queue, 1, state, -1, SQLITE_TRANSIENT)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind state to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = sqlite3_bind_int(set_mt_queue, 2, attempts)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind attempts to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = sqlite3_bind_int(set_mt_queue, 3, next_retry_at)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind next_retry_at to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = bind_text_or_null(set_mt_queue, 4, last_result)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind last_result to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = bind_int_or_null(set_mt_queue, 5, sent_at)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind sent_at to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = bind_int_or_null(set_mt_queue, 6, delivered_at)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind delivered_at to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = sqlite3_bind_text(set_mt_queue, 7, mid, -1, SQLITE_TRANSIENT)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind mid to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(set_mt_queue)) != SQLITE_DONE) {
        ast_log(LOG_WARNING, "Couldn't execute statement: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    return 0;
}

int smsdb_mt_pick_retry(long now, int max_attempts, struct smsdb_mt_row* row)
{
    if (!row || max_attempts < 0) {
        return -1;
    }

    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(pick_mt_queue);

    if ((res = sqlite3_bind_int64(pick_mt_queue, 1, now)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind now to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = sqlite3_bind_int(pick_mt_queue, 2, max_attempts)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind max_attempts to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    res = sqlite3_step(pick_mt_queue);
    if (res == SQLITE_ROW) {
        mt_row_fill(row, pick_mt_queue);
        return 1;
    }

    return (res == SQLITE_DONE) ? 0 : -1;
}

int smsdb_mt_list(void* ctx, smsdb_mt_list_cb cb)
{
    if (!cb) {
        return -1;
    }

    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(list_mt_queue);

    while ((res = sqlite3_step(list_mt_queue)) == SQLITE_ROW) {
        struct smsdb_mt_row row;
        mt_row_fill(&row, list_mt_queue);
        if (cb(ctx, &row)) {
            return 0;
        }
    }

    return (res == SQLITE_DONE) ? 0 : -1;
}

int smsdb_mt_out_add(const char* device, const char* dst, const char* msg, const char* ts, const char* imdn_id)
{
    if (ast_strlen_zero(device) || ast_strlen_zero(dst)) {
        return -1;
    }

    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(add_mt_out);

    if ((res = sqlite3_bind_text(add_mt_out, 1, device, -1, SQLITE_TRANSIENT)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind device to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = sqlite3_bind_text(add_mt_out, 2, dst, -1, SQLITE_TRANSIENT)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind dst to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = bind_text_or_null(add_mt_out, 3, msg)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind msg to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = bind_text_or_null(add_mt_out, 4, ts)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind ts to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = bind_text_or_null(add_mt_out, 5, imdn_id)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind imdn_id to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(add_mt_out)) != SQLITE_DONE) {
        ast_log(LOG_WARNING, "Couldn't execute statement: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    return (int)sqlite3_last_insert_rowid(smsdb);
}

int smsdb_mt_out_set_state(int id, const char* state, int srr_status)
{
    if (id <= 0 || ast_strlen_zero(state)) {
        return -1;
    }

    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(set_mt_out);

    if ((res = sqlite3_bind_text(set_mt_out, 1, state, -1, SQLITE_TRANSIENT)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind state to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = bind_int_or_null(set_mt_out, 2, srr_status)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind srr_status to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = sqlite3_bind_int(set_mt_out, 3, id)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind id to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(set_mt_out)) != SQLITE_DONE) {
        ast_log(LOG_WARNING, "Couldn't execute statement: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    return 0;
}

int smsdb_mt_out_set_mr(int id, int mr)
{
    if (id <= 0 || mr < 0) {
        return -1;
    }

    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(set_mt_out_mr);

    if ((res = sqlite3_bind_int(set_mt_out_mr, 1, mr)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind mr to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = sqlite3_bind_int(set_mt_out_mr, 2, id)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind id to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    if ((res = sqlite3_step(set_mt_out_mr)) != SQLITE_DONE) {
        ast_log(LOG_WARNING, "Couldn't execute statement: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    return 0;
}

int smsdb_mt_out_find_by_mr(const char* device, const char* dst, int mr, int* id)
{
    /* The mt_out row is stamped with the modem MR (refid) at send time, so the
     * status report is matched precisely by (device, dst, mr). */
    if (ast_strlen_zero(device) || ast_strlen_zero(dst) || mr < 0 || !id) {
        return -1;
    }

    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(get_mt_out_by_mr);

    if ((res = sqlite3_bind_text(get_mt_out_by_mr, 1, device, -1, SQLITE_TRANSIENT)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind device to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = sqlite3_bind_text(get_mt_out_by_mr, 2, dst, -1, SQLITE_TRANSIENT)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind dst to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }
    if ((res = sqlite3_bind_int(get_mt_out_by_mr, 3, mr)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind mr to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    res = sqlite3_step(get_mt_out_by_mr);
    if (res != SQLITE_ROW) {
        return -1;
    }

    *id = sqlite3_column_int(get_mt_out_by_mr, 0);
    return 0;
}

int smsdb_mt_out_get_imdn_id(int id, char* out, size_t len)
{
    if (id <= 0 || !out || len == 0) {
        return -1;
    }

    out[0] = '\0';

    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(get_mt_out_imdn_id);

    if ((res = sqlite3_bind_int(get_mt_out_imdn_id, 1, id)) != SQLITE_OK) {
        ast_log(LOG_WARNING, "Couldn't bind id to stmt: [%d] %s\n", res, sqlite3_errmsg(smsdb));
        return -1;
    }

    res = sqlite3_step(get_mt_out_imdn_id);
    if (res != SQLITE_ROW) {
        return -1;
    }

    const char* const imdn_id = (const char*)sqlite3_column_text(get_mt_out_imdn_id, 0);
    ast_copy_string(out, S_OR(imdn_id, ""), len);
    return 0;
}

int smsdb_mt_out_list(void* ctx, smsdb_mt_out_list_cb cb)
{
    if (!cb) {
        return -1;
    }

    int res;

    SCOPED_DB(smsdb);
    SCOPED_STMT(list_mt_out);

    while ((res = sqlite3_step(list_mt_out)) == SQLITE_ROW) {
        struct smsdb_mt_out_row row;
        mt_out_row_fill(&row, list_mt_out);
        if (cb(ctx, &row)) {
            return 0;
        }
    }

    return (res == SQLITE_DONE) ? 0 : -1;
}

// ================================================================
// Unified SMS history (mini-web): MT (incoming SIM->SIP) + MO (outgoing SIP->SIM)
// The SQL/bind/row-read logic lives in src/sms_paging.c (pure SQLite module
// shared with the unit test); these wrappers run it on the module's
// (mutex-protected) connection and log failures for the mini-web HTTP layer.
// ================================================================

int smsdb_sms_page(const char* device, int page_size, int offset, void* ctx, smsdb_sms_list_cb cb)
{
    int res;

    SCOPED_DB(smsdb);

    res = sms_paging_page(smsdb, device, page_size, offset, ctx, cb);
    if (res) {
        ast_log(LOG_ERROR, "smsdb: unified SMS page failed (device=%s): [%d] %s\n",
                (device && !ast_strlen_zero(device)) ? device : "(all)", sqlite3_errcode(smsdb), sqlite3_errmsg(smsdb));
    }
    return res;
}

int smsdb_sms_count(const char* device, int* total)
{
    int res;

    SCOPED_DB(smsdb);

    res = sms_paging_count(smsdb, device, total);
    if (res) {
        ast_log(LOG_ERROR, "smsdb: unified SMS count failed (device=%s): [%d] %s\n",
                (device && !ast_strlen_zero(device)) ? device : "(all)", sqlite3_errcode(smsdb), sqlite3_errmsg(smsdb));
    }
    return res;
}

/** Сериализовать read-only pager на общем connection; callback вызывается пока mutex удержан. */
int smsdb_incoming_parts_page(const char* device, int page_size, int offset, void* ctx, sms_incoming_page_cb cb)
{
    SCOPED_DB(smsdb);
    return sms_incoming_paging_page(smsdb, device, page_size, offset, ctx, cb);
}

/** Сериализовать COUNT incoming_msg и освободить общий DB mutex до возврата. */
int smsdb_incoming_parts_count(const char* device, int* total)
{
    SCOPED_DB(smsdb);
    return sms_incoming_paging_count(smsdb, device, total);
}

int smsdb_vacuum_into(const char* backup_file)
{
    static const size_t SQLSTMT_DEF_LEN = 64;

    if (ast_file_is_readable(backup_file)) {
        remove(backup_file);
    }

    RAII_VAR(struct ast_str*, sqlstmt, ast_str_create(SQLSTMT_DEF_LEN), ast_free);
    ast_str_set(&sqlstmt, 0, "VACUUM INTO \"%s\"", backup_file);

    return execute_ast_str(sqlstmt);
}

/*!
 * \internal
 * \brief Clean up resources on Asterisk shutdown
 */
void smsdb_atexit()
{
    db_clean_statements();
    if (sqlite3_close(smsdb) == SQLITE_OK) {
        smsdb = NULL;
    }
}

int smsdb_init()
{
    if (db_init()) {
        return -1;
    }

    return 0;
}
