#ifndef CHAN_QUECTEL_CALL_LOG_ASYNC_H_INCLUDED
#define CHAN_QUECTEL_CALL_LOG_ASYNC_H_INCLUDED

#include "call_log.h"

#include <sqlite3.h>
#include <stddef.h>

#define CALL_LOG_ASYNC_QUEUE_CAPACITY 256
#define CALL_LOG_ASYNC_DEVICE_LEN 64
#define CALL_LOG_ASYNC_NUMBER_LEN 256
#define CALL_LOG_ASYNC_TIME_LEN 64

typedef struct call_log_async call_log_async_t;

/** Прочитать безопасный следующий reserved rowid до запуска producer-ов.
 * Учитываются и сохранённая AUTOINCREMENT sqlite_sequence, и текущий MAX(id), чтобы
 * удаление последней строки не приводило к повторному call-<rowid> после restart.
 * Возвращает 0 при успехе, -1 при ошибке SQLite или некорректной схеме.
 */
int call_log_async_seed_id(sqlite3* db, sqlite3_int64* next_id);

/** Создать очередь и единственный SQLite writer.
 * db должен оставаться открытым до call_log_async_stop(); next_id получен seed_id до
 * запуска worker. Возвращает NULL при старте thread error.
 */
call_log_async_t* call_log_async_start(sqlite3* db, sqlite3_int64 next_id);

/** Остановить producer, дописать принятые события и join writer; допустимо только shutdown path. */
void call_log_async_stop(call_log_async_t** queue);

/** Поставить incoming/outgoing START без DB I/O и вернуть стабильный call-<rowid> ID.
 * Повтор incoming того же device/index дедуплицируется в RAM; force_new_generation
 * создаёт новое ID для подтверждённого unsolicited successor. Ошибка trylock/full/OOM — -1.
 */
int call_log_async_start_call(call_log_async_t* queue, const char* device, int call_idx,
                              call_log_direction_t direction, const char* number,
                              const char* timestamp, int force_new_generation,
                              char* call_id, size_t call_id_len);

/** Поставить ANSWER в FIFO по точному `call-<rowid>`; 0 enqueue, -1 busy/full/invalid ID. */
int call_log_async_answer(call_log_async_t* queue, const char* call_id);
/** Поставить SET_INDEX в FIFO; call_idx может быть -1 до первого modem assignment. */
int call_log_async_set_index(call_log_async_t* queue, const char* call_id, int call_idx);
/** Поставить FINISH с UTC timestamp; answered=0/1, missed+MT row остаются одной DB transaction. */
int call_log_async_finish(call_log_async_t* queue, const char* call_id, int answered, const char* timestamp);

/** Найти текущий активный incoming из RAM map, не обращаясь к SQLite. */
int call_log_async_active_id(call_log_async_t* queue, const char* device, int call_idx,
                             char* call_id, size_t call_id_len);

/** Ожидать завершения всех уже поставленных событий; предназначено только тестам/diagnostics. */
int call_log_async_flush(call_log_async_t* queue);

#endif
