#include "call_log.h"
#include "clcc_parser.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Разобрать строку отдельной копией: production parser изменяет номерный буфер. */
static int parse_copy(const char* source, unsigned* idx, unsigned* dir, unsigned* state,
                      unsigned* mode, unsigned* mpty, char** number, unsigned* toa)
{
    char* copy = strdup(source);
    assert(copy);
    const int result = clcc_parse_line(copy, idx, dir, state, mode, mpty, number, toa);
    if (!result) {
        /* Номер расположен внутри copy и проверяется до освобождения исходного буфера. */
        assert(number && *number);
        assert(strlen(*number) < 64);
        free(copy);
        return 0;
    }
    free(copy);
    return -1;
}

/** Строго отклонить malformed, лишние, кавычечные и переполненные CLCC поля. */
static void test_strict_clcc_field_parsing(void)
{
    unsigned idx = 0, dir = 0, state = 0, mode = 0, mpty = 0, toa = 0;
    char* number = NULL;
    char valid[] = "+CLCC: 7,1,0,0,0,\"+123\",145";
    assert(clcc_parse_line(valid, &idx, &dir, &state, &mode, &mpty, &number, &toa) == 0);
    assert(idx == 7 && dir == 1 && state == 0 && mode == 0 && mpty == 0 && toa == 145);
    assert(strcmp(number, "+123") == 0);
    char empty_number[] = "+CLCC: 7,1,0,0,0,,145";
    assert(clcc_parse_line(empty_number, &idx, &dir, &state, &mode, &mpty, &number, &toa) == 0);
    assert(number[0] == '\0' && toa == 145);

    assert(parse_copy("+CLCC: 7,1,0,0,0,\"+1\",145junk", &idx, &dir, &state, &mode, &mpty, &number, &toa) == -1);
    assert(parse_copy("+CLCC: 7,1,0,0,0,\"+1,145", &idx, &dir, &state, &mode, &mpty, &number, &toa) == -1);
    assert(parse_copy("+CLCC: 7,1,0,0,0,\"+1\",145,extra", &idx, &dir, &state, &mode, &mpty, &number, &toa) == -1);
    assert(parse_copy("+CLCC: 4294967296,1,0,0,0,\"+1\",145", &idx, &dir, &state, &mode, &mpty, &number, &toa) == -1);
    assert(parse_copy("+CLCC: 7,2,0,0,0,\"+1\",145", &idx, &dir, &state, &mode, &mpty, &number, &toa) == -1);
    assert(parse_copy("+CLCC: 7,1,9,0,0,\"+1\",145", &idx, &dir, &state, &mode, &mpty, &number, &toa) == -1);
}

/** Проверить partial state=6 identity и неизменность строгого обычного CLCC parser.
 * Данные моделируют оба направления и строки, оборванные до/внутри номера; только
 * известный vendor release prefix принимается для постановки барьера, прочий мусор нет.
 */
static void test_partial_release_prefix_for_both_directions(void)
{
    unsigned idx = 0, dir = 0, mode = 0, mpty = 0;
    char strict_rejects_state_six[] = "+CLCC: 1,1,6,0,0,\"+79958528857";
    char* strict_number = NULL;
    unsigned strict_state = 0, strict_toa = 0;
    assert(clcc_parse_line(strict_rejects_state_six, &idx, &dir, &strict_state, &mode, &mpty,
                           &strict_number, &strict_toa) == -1);

    char incoming[] = "+CLCC: 1,1,6,0,0,\"+79958528857";
    const int incoming_result = clcc_parse_release_prefix(incoming, &idx, &dir, &mode, &mpty);
    assert(incoming_result == 0);
    assert(clcc_parse_failure_is_error(incoming_result) == 0); /* fallback accepted => WARNING */
    assert(idx == 1 && dir == 1 && mode == 0 && mpty == 0);

    char outgoing[] = "+CLCC: 1,0,6,0,0,\"+79958528857";
    assert(clcc_parse_release_prefix(outgoing, &idx, &dir, &mode, &mpty) == 0);
    assert(idx == 1 && dir == 0 && mode == 0 && mpty == 0);

    char wrong_state[] = "+CLCC: 1,1,5,0,0,\"+799";
    const int rejected_result = clcc_parse_release_prefix(wrong_state, &idx, &dir, &mode, &mpty);
    assert(rejected_result == -1);
    assert(clcc_parse_failure_is_error(rejected_result) == 1); /* fallback отказал => ERROR */
    char wrong_type[] = "+CLCC: 1,1,6,1,0,\"+799";
    const int wrong_type_result = clcc_parse_release_prefix(wrong_type, &idx, &dir, &mode, &mpty);
    assert(wrong_type_result == -1 && clcc_parse_failure_is_error(wrong_type_result) == 1);
    char malformed_identity[] = "+CLCC: 1junk,1,6,0,0,\"+799";
    assert(clcc_parse_release_prefix(malformed_identity, &idx, &dir, &mode, &mpty) == -1);
    char zero_idx[] = "+CLCC: 0,1,6,0,0,\"+799";
    assert(clcc_parse_release_prefix(zero_idx, &idx, &dir, &mode, &mpty) == -1);
    char invalid_dir[] = "+CLCC: 1,2,6,0,0,\"+799";
    assert(clcc_parse_release_prefix(invalid_dir, &idx, &dir, &mode, &mpty) == -1);
    char invalid_mpty[] = "+CLCC: 1,1,6,0,2,\"+799";
    assert(clcc_parse_release_prefix(invalid_mpty, &idx, &dir, &mode, &mpty) == -1);
    char invalid_state[] = "+CLCC: 1,1,6junk,0,0,\"+799";
    assert(clcc_parse_release_prefix(invalid_state, &idx, &dir, &mode, &mpty) == -1);
    char overflow_idx[] = "+CLCC: 4294967296,1,6,0,0,\"+799";
    assert(clcc_parse_release_prefix(overflow_idx, &idx, &dir, &mode, &mpty) == -1);
}

/** Проверить invalid whole snapshot, retry и однократный finish после валидного empty. */
static void test_invalid_snapshot_then_fresh_empty_finishes_once(void)
{
    sqlite3* db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "CREATE TABLE mt_queue(device TEXT NOT NULL,mid TEXT PRIMARY KEY,from_num TEXT NOT NULL,msg TEXT NOT NULL,ts TEXT,state TEXT NOT NULL DEFAULT 'new',attempts INTEGER DEFAULT 0,next_retry_at INTEGER DEFAULT 0,last_result TEXT,created_at INTEGER DEFAULT 1,sent_at INTEGER,delivered_at INTEGER)", NULL, NULL, NULL) == SQLITE_OK);
    assert(call_log_schema_ensure(db) == 0);

    char call_id[CALL_LOG_ID_LEN];
    assert(call_log_start(db, "sim6", 7, CALL_LOG_INCOMING, "+700", "start", call_id, sizeof(call_id)) == 0);

    unsigned idx, dir, state, mode, mpty, toa;
    char valid_present[] = "+CLCC: 7,1,0,0,0,\"+700\",145";
    char malformed[] = "+CLCC: 8,1,0,0,0,\"+800\",145junk";
    char* number;
    assert(clcc_parse_line(valid_present, &idx, &dir, &state, &mode, &mpty, &number, &toa) == 0);
    assert(clcc_parse_line(malformed, &idx, &dir, &state, &mode, &mpty, &number, &toa) == -1);

    /* State=6 только маркирует release candidate: до валидного snapshot slot-present
     * вызов остаётся открытым; invalid snapshot не подтверждает отсутствие; лишь fresh
     * empty завершает его и создаёт ровно одно missed уведомление. */
    char release[] = "+CLCC: 7,1,6,0,0,\"+700";
    assert(clcc_parse_release_prefix(release, &idx, &dir, &mode, &mpty) == 0);
    assert(idx == 7 && dir == 1);
    unsigned int barrier = 0;
    assert(call_log_release_snapshot_decide(&barrier, 1, 1) == CALL_LOG_SNAPSHOT_PRESENT);
    sqlite3_stmt* stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT finished FROM call_log WHERE id=CAST(substr(?1,6) AS INTEGER)", -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, call_id, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 0);
    sqlite3_finalize(stmt);

    /* Один корректный занятый slot не превращает snapshot с другой malformed строкой в valid. */
    unsigned int older = 0, queued = 1, seen = 1;
    assert(call_log_release_snapshot_decide(&older, 1, 0) == CALL_LOG_SNAPSHOT_INVALID);
    assert(call_log_release_snapshot_retry(&older, &queued, &seen) == 1 && queued == 0 && seen == 0);
    assert(call_log_release_snapshot_decide(&older, 0, 1) == CALL_LOG_SNAPSHOT_ABSENT);
    assert(call_log_release_confirm(db, call_id, 0, "fresh empty") == 0);
    assert(call_log_release_confirm(db, call_id, 0, "duplicate fresh empty") == 0);

    assert(sqlite3_prepare_v2(db, "SELECT direction,finished FROM call_log WHERE id=CAST(substr(?1,6) AS INTEGER)", -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, call_id, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 0), "missed") == 0 && sqlite3_column_int(stmt, 1) == 1);
    sqlite3_finalize(stmt);
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM mt_queue", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);

    /* Подтверждённый release исходящего также ждёт valid snapshot, но не становится missed. */
    char outgoing_id[CALL_LOG_ID_LEN];
    assert(call_log_start(db, "sim6", 7, CALL_LOG_OUTGOING, "+701", "outgoing start", outgoing_id, sizeof(outgoing_id)) == 0);
    assert(call_log_release_snapshot_decide(&older, 0, 0) == CALL_LOG_SNAPSHOT_INVALID);
    assert(call_log_release_confirm(db, outgoing_id, 0, "outgoing fresh empty") == 0);
    assert(sqlite3_prepare_v2(db, "SELECT direction,finished,answered FROM call_log WHERE id=CAST(substr(?1,6) AS INTEGER)", -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, outgoing_id, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 0), "outgoing") == 0);
    assert(sqlite3_column_int(stmt, 1) == 1 && sqlite3_column_int(stmt, 2) == 0);
    sqlite3_finalize(stmt);
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM mt_queue", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

/** Запустить strict parser, vendor release-prefix и snapshot/finish regressions. */
int main(void)
{
    test_strict_clcc_field_parsing();
    test_partial_release_prefix_for_both_directions();
    test_invalid_snapshot_then_fresh_empty_finishes_once();
    puts("clcc-parser: all checks passed");
    return 0;
}
