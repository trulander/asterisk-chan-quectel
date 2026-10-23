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

    /* Один корректный занятый slot не превращает snapshot с другой malformed строкой в valid. */
    unsigned int older = 0, queued = 1, seen = 1;
    assert(call_log_release_snapshot_decide(&older, 1, 0) == CALL_LOG_SNAPSHOT_INVALID);
    assert(call_log_release_snapshot_retry(&older, &queued, &seen) == 1 && queued == 0 && seen == 0);
    assert(call_log_release_snapshot_decide(&older, 0, 1) == CALL_LOG_SNAPSHOT_ABSENT);
    assert(call_log_release_confirm(db, call_id, 0, "fresh empty") == 0);
    assert(call_log_release_confirm(db, call_id, 0, "duplicate fresh empty") == 0);

    sqlite3_stmt* stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT direction,finished FROM call_log WHERE id=CAST(substr(?1,6) AS INTEGER)", -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, call_id, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char*)sqlite3_column_text(stmt, 0), "missed") == 0 && sqlite3_column_int(stmt, 1) == 1);
    sqlite3_finalize(stmt);
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM mt_queue", -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1);
    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

/** Запустить strict parser и production snapshot/finish regressions. */
int main(void)
{
    test_strict_clcc_field_parsing();
    test_invalid_snapshot_then_fresh_empty_finishes_once();
    puts("clcc-parser: all checks passed");
    return 0;
}
