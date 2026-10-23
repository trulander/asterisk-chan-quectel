#ifndef CHAN_QUECTEL_SMS_MULTIPART_H_INCLUDED
#define CHAN_QUECTEL_SMS_MULTIPART_H_INCLUDED

#include <sqlite3.h>

/** Сохранить один сегмент и при count == parts собрать complete payload.
 * \param db caller-owned SQLite connection; caller держит mutex/transaction на всём вызове.
 * \param key стабильный key multipart group; \param seqorder порядок части; \param parts всего частей.
 * \param ttl срок хранения строки в секундах; \param message текст данной части.
 * \param source_device/storage/index nullable source identity для durable SIM deletion.
 * \param retain_parts ненулевое значение удерживает rows после assembly до delete/queue GC.
 * \param assembled output для heap payload: NULL пока группа неполна; при сборке caller
 *        освобождает его sqlite3_free(), так как helper использует SQLite allocator.
 * \param rowid output rowid только что записанного сегмента, если указатель передан.
 * \return текущий count группы, -1 при SQL/assembly/аргументной ошибке. Assembly выполняется
 * лишь при count==parts и сортирует части ORDER BY seqorder; дополнительные sequence-policy
 * не вводятся. При retain_parts=false legacy поведение очищает rows после сборки.
 */
int sms_multipart_put(sqlite3* db, const char* key, int seqorder, int parts, int ttl, const char* message, const char* source_device,
                     const char* source_storage, int source_index, int retain_parts, char** assembled, long long* rowid);

/** Вставить incoming SIM→SIP MT row в production mt_queue schema.
 * Caller owns mutex/transaction; strings borrowed until return. 0 = inserted, -1 = SQL error.
 */
int sms_mt_queue_put(sqlite3* db, const char* device, const char* mid, const char* from_num, const char* message, const char* timestamp);

#endif
