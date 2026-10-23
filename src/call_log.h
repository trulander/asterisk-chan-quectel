#ifndef CHAN_QUECTEL_CALL_LOG_H_INCLUDED
#define CHAN_QUECTEL_CALL_LOG_H_INCLUDED

#include <sqlite3.h>
#include <stddef.h>

/* Размеры буферов ID и текста уведомления; значения включают завершающий ноль. */
#define CALL_LOG_ID_LEN 64
#define CALL_LOG_TEXT_LEN 512

/** Направление исходного GSM вызова; missed возникает только из incoming без ответа. */
typedef enum {
    CALL_LOG_INCOMING = 1,
    CALL_LOG_OUTGOING = 2
} call_log_direction_t;

/** Результат сопоставления release-кандидата с CLCC snapshot. */
typedef enum {
    CALL_LOG_SNAPSHOT_OLDER = 0,
    CALL_LOG_SNAPSHOT_PRESENT = 1,
    CALL_LOG_SNAPSHOT_ABSENT = 2,
    CALL_LOG_SNAPSHOT_INVALID = 3
} call_log_snapshot_result_t;

/** Создать таблицу журнала и применить безопасную первичную инициализацию.
 * db должен быть открытым соединением SQLite; функция не закрывает его.
 * Возвращает 0 после успешного SQL/COMMIT, -1 после ошибки с ROLLBACK.
 */
int call_log_schema_ensure(sqlite3* db);

/** Сверить CLCC direction с направлением существующего cpvt до записи/перехода. */
int call_log_direction_matches(unsigned int event_direction, unsigned int cpvt_direction);
/** Сопоставить release-кандидат с CLCC ответом.
 * Сначала пропускаются ответы запросов, поставленных до события; malformed snapshot
 * никогда не подтверждает отсутствие и требует новой полной проверки.
 */
call_log_snapshot_result_t call_log_release_snapshot_decide(unsigned int* older_snapshots, int slot_present, int snapshot_valid);
/** Сбросить невалидный/ошибочный snapshot и решить, нужна ли новая постановка CLCC.
 * Старый ответ только уменьшает barrier; ошибка актуальной проверки снимает queued flag.
 */
int call_log_release_snapshot_retry(unsigned int* older_snapshots, unsigned int* verification_queued, unsigned int* slot_seen);

/** Записать новый вызов или найти ещё активный входящий по SIM+индексу.
 * Для исходящего всегда создаётся новая строка; для входящего повторный CLCC
 * возвращает ID открытой строки. call_idx служит только активной корреляцией,
 * долговечный ID формируется из AUTOINCREMENT rowid. timestamp — время начала.
 * При ошибке SQLite возвращает -1, иначе 0 и записывает ID в caller buffer.
 */
int call_log_start(sqlite3* db, const char* device, int call_idx, call_log_direction_t direction,
                   const char* number, const char* timestamp, char* call_id, size_t call_id_len);
/** Записать новое поколение incoming несмотря на ещё открытую строку с тем же modem index.
 * Используется только для подтверждённого unsolicited incoming при ожидающем release;
 * обычный повторный CLCC должен оставаться дедуплицированным через call_log_start().
 */
int call_log_start_new_generation(sqlite3* db, const char* device, int call_idx, const char* number,
                                  const char* timestamp, char* call_id, size_t call_id_len);
/** Нормализованный источник и состояние GSM события для общей correlation decision. */
typedef enum {
    CALL_LOG_EVENT_CLCC = 0,
    CALL_LOG_EVENT_UNSOLICITED = 1
} call_log_event_origin_t;
typedef enum {
    CALL_LOG_EVENT_STATE_OTHER = 0,
    CALL_LOG_EVENT_STATE_INCOMING = 1,
    CALL_LOG_EVENT_STATE_WAITING = 2,
    CALL_LOG_EVENT_STATE_ACTIVE = 3
} call_log_event_state_t;
typedef enum {
    CALL_LOG_GENERATION_DEDUPLICATE = 0,
    CALL_LOG_GENERATION_NEW = 1,
    CALL_LOG_GENERATION_USE_SUCCESSOR = 2
} call_log_generation_decision_t;
/** Единственная production decision для incoming generation: источник, направление,
 * состояние, pending release и successor определяют dedup/new/reuse.
 */
call_log_generation_decision_t call_log_incoming_generation_decide(call_log_event_origin_t origin,
                                                                     int incoming_direction,
                                                                     call_log_event_state_t state,
                                                                     int release_pending,
                                                                     int successor_known);
/** Завершать старый release ID при отсутствии slot либо при busy slot с известным successor. */
int call_log_release_should_finish(int slot_present, int successor_known);

/** Связать исходящую строку с присвоенным модемом индексом.
 * ID выбирает вечную запись, call_idx не используется как её ключ; -1 означает
 * что канал не получил пригодного индекса. Возвращает 0 при COMMIT, -1 при ошибке.
 */
int call_log_set_index(sqlite3* db, const char* call_id, int call_idx);

/** Найти открытый входящий вызов по текущему SIM/index, если PBX-канала нет.
 * Выход ID принадлежит caller; 1 найден, 0 отсутствует, -1 SQL/buffer error.
 */
int call_log_active_id(sqlite3* db, const char* device, int call_idx, char* call_id, size_t call_id_len);

/** Зафиксировать факт ответа входящего вызова; повторные события идемпотентны.
 * Завершённая строка не меняется; ошибка SQL возвращает -1.
 */
int call_log_answer(sqlite3* db, const char* call_id);

/** Отметить входящий ответ либо завершить вызов атомарно с missed MT queue.
 * answered=1 оставляет тип incoming; answered=0 переводит только неотвеченный
 * входящий в missed и добавляет deterministic-mid уведомление в mt_queue в той
 * же транзакции. Исходящие всегда остаются outgoing. Возвращает -1 без частичной
 * записи при любой ошибке SQL/COMMIT.
 */
int call_log_finish(sqlite3* db, const char* call_id, int answered, const char* timestamp);

/** Подтвердить DSCI/CCINFO release только по следующему актуальному CLCC snapshot.
 * slot_present=1 сохраняет запись, потому что этот индекс всё ещё занят; 0 атомарно
 * завершает указанную устойчивую строку. Возвращает 0 при успешном решении, -1 при DB error.
 */
int call_log_release_confirm(sqlite3* db, const char* call_id, int slot_present, const char* timestamp);

/** Закрыть незавершённые вызовы после аварийного завершения процесса.
 * Каждая запись обрабатывается отдельной атомарной finish-транзакцией, чтобы
 * сбой посреди восстановления можно было безопасно продолжить при следующем старте.
 * Неотвеченные входящие становятся missed; answered и outgoing сохраняют тип.
 */
int call_log_recover(sqlite3* db, const char* timestamp);

#endif
