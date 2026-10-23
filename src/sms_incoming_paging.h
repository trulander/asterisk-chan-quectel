/* Чистый SQLite API для постраничного просмотра сохранённых частей incoming_msg. */
#ifndef CHAN_QUECTEL_SMS_INCOMING_PAGING_H
#define CHAN_QUECTEL_SMS_INCOMING_PAGING_H
#include <sqlite3.h>

/** Максимальная длина текстового фрагмента row в байтах, включая завершающий NUL. */
#define SMS_INCOMING_PAGE_TEXT 4096
/** Максимальный размер составного multipart key в байтах, включая NUL. */
#define SMS_INCOMING_PAGE_KEY 256
/** Буфер device; должен вмещать DEVNAMELEN и NUL. */
#define SMS_INCOMING_PAGE_DEVICE 64
/** Буфер storage-тега вроде SM/ME и завершающего NUL. */
#define SMS_INCOMING_PAGE_STORAGE 16

/** Скопированная read-only проекция строки incoming_msg для mini-web callback.
 * Все массивы принадлежат структуре и завершаются NUL; NULL-тексты SQL становятся "".
 * Числовой NULL читается SQLite как 0. Эта структура не содержит SQLite statement.
 */
struct sms_incoming_page_row {
    long long rowid; /*!< SQLite rowid; точная локальная идентичность строки, единицы: целое. */
    char device[SMS_INCOMING_PAGE_DEVICE]; /*!< source_device; пустая строка для SQL NULL. */
    char storage[SMS_INCOMING_PAGE_STORAGE]; /*!< source_storage, обычно SM; пустая строка при NULL. */
    int source_index; /*!< Индекс source_storage в SIM; SQL NULL преобразован в 0. */
    char key[SMS_INCOMING_PAGE_KEY]; /*!< Multipart group key, не внешний URL идентификатор. */
    int seqorder; /*!< Порядок части внутри группы, переданный декодером модема. */
    char message[SMS_INCOMING_PAGE_TEXT]; /*!< Текст только этой части, не собранное сообщение. */
    long long expiration; /*!< Значение expiration из БД (epoch seconds в текущей схеме). */
    int delete_status; /*!< Числовое persisted состояние из sms_delete_state.h. */
    int delete_attempts; /*!< Подтверждённые ошибки CMGD; NULL/отсутствие читается как 0. */
    long long retry_at; /*!< Время следующего durable scheduler action, epoch seconds. */
    int mt_queued; /*!< Ненулевое значение: группа атомарно помещена в mt_queue. */
    char mt_mid[128]; /*!< Стабильный MID очереди группы; пустая строка, если NULL. */
};

/** Посетитель строк страницы; row действителен только на время callback. */
typedef int (*sms_incoming_page_cb)(void *ctx, const struct sms_incoming_page_row *row);

/** Посчитать строки incoming_msg с опциональным exact device filter.
 * \param db открытое SQLite соединение, не принадлежащее pager.
 * \param device имя устройства либо NULL/"" для всех; bind-параметр, не SQL текст.
 * \param total обязательный выходной указатель; при ошибке до выполнения остаётся 0.
 * \return 0 для успешного COUNT, в том числе корректного нуля; -1 при ошибке аргумента/SQL.
 * Caller сериализует соединение (production держит общую smsdb mutex); функция
 * только читает данные и завершает собственный statement до возврата.
 */
int sms_incoming_paging_count(sqlite3 *db, const char *device, int *total);

/** Прочитать один детерминированный page из incoming_msg без изменения БД.
 * \param db открытое SQLite соединение; caller обеспечивает serialization.
 * \param device NULL/"" означает все устройства, иначе точное bound значение.
 * \param page_size строго положительный лимит строк; \param offset неотрицательный 0-based offset.
 * \param ctx непрозрачное значение, передаваемое callback без передачи владения.
 * \param cb обязательный посетитель; ненулевой return callback штатно прекращает обход.
 * \return 0 при успешном statement или callback-stop, -1 при аргументе/prepare/bind/step/finalize error.
 * Порядок фиксирован как expiration DESC, key BINARY ASC, seqorder ASC, rowid ASC:
 * rowid разрешает равные ключ/порядок и делает соседние страницы стабильными. SQL filter
 * и LIMIT/OFFSET bind-ятся отдельно, чтобы входное устройство не могло менять запрос.
 */
int sms_incoming_paging_page(sqlite3 *db, const char *device, int page_size, int offset, void *ctx, sms_incoming_page_cb cb);
#endif
