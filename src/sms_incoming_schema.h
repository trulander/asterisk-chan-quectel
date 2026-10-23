#ifndef CHAN_QUECTEL_SMS_INCOMING_SCHEMA_H_INCLUDED
#define CHAN_QUECTEL_SMS_INCOMING_SCHEMA_H_INCLUDED

#include <stddef.h>
#include <sqlite3.h>

/** Создать таблицу incoming_msg/index либо мигрировать старую схему.
 * db обязателен; функция не берёт application mutex и не открывает собственную
 * transaction, поэтому production вызывает её внутри SMS_TX_SCOPE. Возвращает 0
 * только при успешном создании/миграции; при SQLite/DDL ошибке возвращает -1,
 * а владелец transaction обязан откатить всю миграцию.
 */
int sms_incoming_schema_ensure(sqlite3* db);

/** Сохранить delete state одной exact durable row.
 * \param db соединение под mutex и внутри caller transaction.
 * \param rowid primary row identity; \param device/storage/index ожидаемые source values.
 * \param status числовое значение sms_delete_status_t; \param attempts число подтверждённых CMGD ошибок.
 * \param retry_at Unix epoch seconds следующего действия.
 * \return 0 только если ровно одна строка совпала по rowid и nullable source identity;
 *         -1 при некорректном аргументе/SQL/mismatch. Никакой commit/unlock не делает.
 * Полный identity guard защищает новую SMS, повторно занявшую тот же индекс.
 */
int sms_incoming_delete_state_update(sqlite3* db, long long rowid, const char* device, const char* storage, int index,
                                    int status, unsigned attempts, int retry_at);

/** Перенести заблокированную due job вперёд без изменения статуса/счётчика.
 * db должен быть сериализован; retry_at — epoch seconds. Возврат 0 означает update;
 * -1 означает SQL/аргументную ошибку. Транзакцией владеет caller.
 */
int sms_incoming_delete_retry_defer(sqlite3* db, long long rowid, int retry_at);

/** Прочитать атомарный group queue marker перед генерацией candidate MID.
 * \param key stable multipart group key; \param queued обязательный output boolean.
 * \param mid output buffer; \param mid_len размер с NUL; при queued=0 MID пустой.
 * Возвращает 0 при успешном SELECT, -1 на ошибке/аргументах; SQL NULL MID становится "".
 * Caller сериализует соединение и должен держать transaction, если связывает read с write.
 */
int sms_incoming_group_queue_state(sqlite3* db, const char* key, int* queued, char* mid, size_t mid_len);
/** Записать mt_queued=1 и существующий committed MID каждой строке группы.
 * Caller держит DB mutex/transaction; 0 означает успех, -1 — SQLite/аргументная ошибка.
 */
int sms_incoming_group_mark_queued(sqlite3* db, const char* key, const char* mid);
/** Атомарно обеспечить одну mt_queue запись для stable group key.
 * Если marker уже существует, effective_mid получает прежний MID и inserted=0;
 * иначе вставляется candidate MID и inserted=1. Все буферы обязательны, effective_mid
 * должен иметь место для MID и NUL. Возвращает 0 на успех, -1 на SQL/bind/размер/ошибку.
 * Caller удерживает mutex и transaction от marker SELECT до queue insert/marker update.
 */
int sms_incoming_mt_queue_group(sqlite3* db, const char* key, const char* device, const char* candidate_mid,
                                const char* from_num, const char* message, const char* timestamp,
                                char* effective_mid, size_t effective_mid_len, int* inserted);
/** Удалить одну multipart группу только при mt_queued=1 и complete delete status всех частей.
 * Caller сериализует SQL и включает delete в caller transaction; 0 означает проверено/GC,
 * -1 — ошибка. Неподходящая группа является корректным no-op.
 */
int sms_incoming_gc_group(sqlite3* db, const char* key);
/** Удалить все группы, удовлетворяющие одновременно queue и delete completion условиям.
 * Предназначено для startup/repair; caller управляет mutex/transaction. 0/-1 success/error.
 */
int sms_incoming_gc_completed_groups(sqlite3* db);
/** Выбрать одну due deletion job по retry_at,rowid.
 * \param now текущее Unix время; outputs rowid/device/index должны иметь валидные размеры.
 * \return 1 найден SIM job, 0 корректно нет due row, -1 ошибка/аргументы.
 * Фильтр исключает NULL/пустое device, storage кроме SM, нечисловой/negative index и STOPPED;
 * caller сериализует соединение. Scheduler обязан defer временно недоступную SIM, чтобы
 * первый due row не блокировал другие устройства.
 */
int sms_incoming_delete_job_pick(sqlite3* db, int now, long long* rowid, char* device, size_t device_len, int* index);

#endif
