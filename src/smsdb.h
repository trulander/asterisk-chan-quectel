/*
   Copyright (C) 2020 Max von Buelow <max@m9x.de>
*/

#ifndef CHAN_QUECTEL_SMSDB_H_INCLUDED
#define CHAN_QUECTEL_SMSDB_H_INCLUDED

#include "dc_config.h" /* DEVNAMELEN */
#include "call_log.h"
#include "call_paging.h"
#include "sms_paging.h" /* struct sms_paging_row / unified SMS history */
#include "sms_incoming_paging.h"
#include "sim_metrics.h"

#define MT_MID_LEN 128
#define MT_STATE_LEN 32
#define MT_RESULT_LEN 256
#define MT_MSG_LEN 4096
#define MT_IMDN_ID_LEN 256

/*! Incoming SMS MT queue row (SIM -> SIP/Flexisip) */
struct smsdb_mt_row {
    char device[DEVNAMELEN];
    char mid[MT_MID_LEN];
    char from_num[64];
    char msg[MT_MSG_LEN];
    char ts[64];
    char state[MT_STATE_LEN];
    int attempts;
    int next_retry_at;
    char last_result[MT_RESULT_LEN];
    long created_at;
    long sent_at;
    long delivered_at;
};

/*! Outgoing SMS row (Android -> SIM)
    MT = "Message Transfer" naming shared with mt_queue for the web UI */
struct smsdb_mt_out_row {
    int id;
    char device[DEVNAMELEN];
    char dst[64];
    char msg[MT_MSG_LEN];
    char ts[64];
    char state[MT_STATE_LEN];
    int srr_status;
    int mr; /*!< modem message reference (refid) stamped at send time */
    char imdn_id[MT_IMDN_ID_LEN]; /*!< IMDN id of the MO message (SIP Call-ID), for delivery receipts */
    long created_at;
    long updated_at;
};

int smsdb_init();
void smsdb_atexit();
/** Создать/найти durable запись звонка; ID возвращается caller-owned буферу.
 * direction: 1 входящий, 2 исходящий; call_idx только текущая корреляция.
 */
int smsdb_call_start(const char* device, int call_idx, int direction, const char* number, char* call_id, size_t call_id_len);
/** Создать отдельный incoming ID для нового unsolicited поколения после pending RELEASED. */
int smsdb_call_start_new_generation(const char* device, int call_idx, const char* number, char* call_id, size_t call_id_len);
/** Зафиксировать факт ответа по call ID; 0 success, -1 DB failure. */
int smsdb_call_answer(const char* call_id);
/** Обновить временный индекс по устойчивому ID. */
int smsdb_call_set_index(const char* call_id, int call_idx);
/** Завершить по устойчивому ID: входящий пропуск и MT уведомление атомарны. */
int smsdb_call_finish(const char* call_id, int answered);
/** Найти открытый входящий по текущему device/index для звонка без PBX-канала. */
int smsdb_call_active_id(const char* device, int call_idx, char* call_id, size_t call_id_len);
/** Завершить кандидат только после CLCC-снимка; занятый index сохраняет строку. */
int smsdb_call_release_confirm(const char* call_id, int slot_present);
int smsdb_put(const char* id, const char* addr, int ref, int parts, int order, const char* msg, struct ast_str** out);
/** Сохранить segment из SIM в incoming_msg с exact SM/device/index identity.
 * При вызове удерживает общий smsdb mutex и транзакционно сохраняет сегмент/assembly;
 * retain lifetime продолжается до queue marker и подтверждения пустоты всех исходных ячеек.
 * storage должен быть "SM"; out — caller-owned ast_str output либо NULL; rowid получает
 * точный persisted row. Возвращает legacy count/result либо отрицательное значение при DB error;
 * не выдаёт право CMGD при неуспешном commit.
 */
int smsdb_put_sim(const char* id, const char* addr, int ref, int parts, int order, const char* msg, const char* storage, int index, struct ast_str** out, long long* rowid);
/** Транзакционно применить один delete event к rowid и сохранённому source identity.
 * rowid — durable row; event — sms_delete_event_t; retry_at — epoch seconds; status/attempts
 * необязательные outputs. Mutex удерживается от state read через update/GC до COMMIT/ROLLBACK.
 * Возвращает 0 при commit, -1 при несовпадении row/DB error.
 */
int smsdb_incoming_delete_apply(long long rowid, int event, int retry_at, int* status, unsigned* attempts);
/** Проверить stable group marker перед созданием candidate MID.
 * 1 означает queued и заполняет mid; 0 означает marker пока отсутствует; -1 — DB/buffer error.
 * storage/index входят в состав key. Выходной mid buffer принадлежит caller.
 */
int smsdb_incoming_group_mt_mid(const char* id, const char* addr, int ref, int parts, const char* storage, int index, char* mid, size_t mid_len);
/** Атомарно создать incoming MT row и group marker либо вернуть прежний MID.
 * Кандидат MID создаётся caller-ом; effective_mid/inserted сообщают результат и принадлежат caller.
 * Общий DB mutex удерживается от BEGIN до COMMIT/ROLLBACK, включая eligible group GC.
 * Возвращает 0 на success, -1 на DB/commit error; после ошибки caller не ставит CMGD.
 */
int smsdb_mt_queue_incoming_group(const char* id, const char* addr, int ref, int parts, const char* storage, int index,
                                  const char* candidate_mid, const char* from_num, const char* message, const char* timestamp,
                                  char* effective_mid, size_t effective_mid_len, int* inserted);
/** Выбрать due SM job; CMGD_SENT включён для обязательной fresh-read recovery.
 * Возвращает 1 с rowid/device/index, 0 если due jobs нет, -1 при ошибке; caller владеет outputs.
 */
int smsdb_incoming_delete_job(long long* rowid, char* device, size_t device_len, int* index);
/** Сдвинуть retry_at временно недоступной/disabled SIM без изменения status/attempts.
 * rowid должен быть точной job, retry_at задан в epoch seconds. Mutation выполняется
 * под smsdb mutex; возвращает 0 на update/commit, -1 при DB error.
 */
int smsdb_incoming_delete_defer(long long rowid, int retry_at);
int smsdb_get_refid(const char* id, const char* addr);
int smsdb_outgoing_add(const char* id, const char* addr, const char* msg, int cnt, int ttl, int srr);
int smsdb_outgoing_clear(int uid, struct ast_str** dst, struct ast_str** msg);
int smsdb_outgoing_part_put(int uid, int refid, struct ast_str** dst, struct ast_str** msg);
int smsdb_outgoing_part_status(const char* id, const char* addr, int mr, int st, int* status_all);
int smsdb_outgoing_purge_one(int* uid, struct ast_str** dst, struct ast_str** msg);
int smsdb_vacuum_into(const char* backup_file);

/* MT queue (incoming SIM -> SIP) */
int smsdb_mt_put(const char* device, const char* mid, const char* from_num, const char* msg, const char* ts);
int smsdb_mt_get(const char* mid, struct smsdb_mt_row* row);
int smsdb_mt_set_result(const char* mid, const char* state, int attempts, int next_retry_at, const char* last_result, int sent_at, int delivered_at);
int smsdb_mt_pick_retry(long now, int max_attempts, struct smsdb_mt_row* row);
typedef int (*smsdb_mt_list_cb)(void* ctx, const struct smsdb_mt_row* row);
int smsdb_mt_list(void* ctx, smsdb_mt_list_cb cb);

/* MT out (outgoing Android -> SIM) */
int smsdb_mt_out_add(const char* device, const char* dst, const char* msg, const char* ts, const char* imdn_id);
int smsdb_mt_out_set_state(int id, const char* state, int srr_status);
int smsdb_mt_out_set_mr(int id, int mr);
int smsdb_mt_out_find_by_mr(const char* device, const char* dst, int mr, int* id);
int smsdb_mt_out_get_imdn_id(int id, char* out, size_t len);
typedef int (*smsdb_mt_out_list_cb)(void* ctx, const struct smsdb_mt_out_row* row);
int smsdb_mt_out_list(void* ctx, smsdb_mt_out_list_cb cb);

/*! Unified SMS history visitor: one entry per SMS, with the direction
    explicitly tagged ('mt' = incoming SIM->SIP from mt_queue; 'mo' = outgoing
    SIP->SIM from mt_out). The individual MT and MO records keep their own,
    separate record shape (see struct smsdb_mt_row / smsdb_mt_out_row); the
    unified paging view is defined by sms_paging.h. */
typedef int (*smsdb_sms_list_cb)(void* ctx, const struct sms_paging_row* row);

/*! List one page of the unified SMS history (MT and MO merged, newest first).
    \param device device name to filter, or NULL for all devices
    \param page_size max items per page (> 0)
    \param offset 0-based row offset (already validated by the caller)
    \param ctx context passed to \a cb
    \param cb row visitor; returning non-zero stops iteration
    \return 0 on success, -1 on error */
int smsdb_sms_page(const char* device, int page_size, int offset, void* ctx, smsdb_sms_list_cb cb);

/*! Total number of SMS records (MT and MO) for a device, or all devices when
    \a device is NULL.
    \return 0 on success and stores the count in *total, -1 on error */
int smsdb_sms_count(const char* device, int* total);
/** Читать durable call_log страницу/total под mutex общего SQLite connection; ошибки возвращают -1. */
int smsdb_call_page(const char* device, int limit, int offset, void* ctx, call_page_cb cb);
int smsdb_call_count(const char* device, int* total);
/** Прочитать per-SIM home-card счётчики под mutex общего SQLite connection.
 * Ошибка любой требуемой таблицы/SQL возвращает -1; успешный нулевой результат остаётся нулём.
 */
int smsdb_sim_metrics(const char* device, struct sim_metrics* out);
/** Прочитать страницу incoming_msg под общей SMS DB mutex; см. pager контракт в заголовке.
 * device NULL/empty означает все SIM; callback получает копию row только на время вызова.
 * Возвращает 0 при успехе, -1 при SQL/DB error; DB не меняет.
 */
int smsdb_incoming_parts_page(const char* device, int page_size, int offset, void* ctx, sms_incoming_page_cb cb);
/** Посчитать incoming_msg строки под mutex; нулевой count — корректный результат, -1 — ошибка. */
int smsdb_incoming_parts_count(const char* device, int* total);

#endif
