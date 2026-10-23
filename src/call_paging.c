#include "call_paging.h"
#include <stdio.h>
#include <string.h>

/** Копирует SQLite TEXT в caller-owned ограниченный буфер, NULL становится пустой строкой. */
static void copy_text(char* dst, size_t size, sqlite3_stmt* st, int col)
{
    const unsigned char* s = sqlite3_column_text(st, col);
    snprintf(dst, size, "%s", s ? (const char*)s : "");
}

/** SQL count имеет точное равенство device: чужая SIM никогда не попадает в результат. */
int call_paging_count(sqlite3* db, const char* device, int* total)
{
    sqlite3_stmt* st = NULL;
    int rc;
    /* Точное равенство device фильтрует SIM; отсутствующая SIM даёт успешный 0, ошибка SQL — -1. */
    if (!db || !device || !total || sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM call_log WHERE device=?", -1, &st, NULL) != SQLITE_OK) return -1;
    if (sqlite3_bind_text(st, 1, device, -1, SQLITE_TRANSIENT) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return -1; }
    *total = sqlite3_column_int(st, 0);
    rc = sqlite3_finalize(st);
    return rc == SQLITE_OK ? 0 : -1;
}

/** SQL page сортирует durable rows новыми первыми с ID tie-break; stmt освобождается и любая SQL ошибка видна caller. */
int call_paging_page(sqlite3* db, const char* device, int limit, int offset, void* ctx, call_page_cb cb)
{
    sqlite3_stmt* st = NULL;
    int rc;
    /* LIMIT/OFFSET ограничивают страницу; durable id DESC разрешает совпавшие timestamp детерминированно. */
    if (!db || !device || !cb || limit <= 0 || offset < 0 || sqlite3_prepare_v2(db,
        "SELECT id,device,number,started_at,direction,answered,finished,ended_at FROM call_log WHERE device=? ORDER BY started_at DESC,id DESC LIMIT ? OFFSET ?", -1, &st, NULL) != SQLITE_OK) return -1;
    if (sqlite3_bind_text(st,1,device,-1,SQLITE_TRANSIENT)!=SQLITE_OK || sqlite3_bind_int(st,2,limit)!=SQLITE_OK || sqlite3_bind_int(st,3,offset)!=SQLITE_OK) { sqlite3_finalize(st); return -1; }
    while ((rc=sqlite3_step(st))==SQLITE_ROW) {
        struct call_page_row row;
        memset(&row,0,sizeof(row)); row.id=sqlite3_column_int64(st,0); copy_text(row.device,sizeof(row.device),st,1); copy_text(row.number,sizeof(row.number),st,2); copy_text(row.started_at,sizeof(row.started_at),st,3); copy_text(row.direction,sizeof(row.direction),st,4); row.answered=sqlite3_column_int(st,5); row.finished=sqlite3_column_int(st,6); copy_text(row.ended_at,sizeof(row.ended_at),st,7);
        if (cb(ctx,&row)) { sqlite3_finalize(st); return 0; }
    }
    if (rc != SQLITE_DONE) { sqlite3_finalize(st); return -1; }
    return sqlite3_finalize(st)==SQLITE_OK ? 0 : -1;
}
