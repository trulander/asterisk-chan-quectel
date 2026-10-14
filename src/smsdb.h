/*
   Copyright (C) 2020 Max von Buelow <max@m9x.de>
*/

#ifndef CHAN_QUECTEL_SMSDB_H_INCLUDED
#define CHAN_QUECTEL_SMSDB_H_INCLUDED

#include "dc_config.h" /* DEVNAMELEN */
#include "sms_paging.h" /* struct sms_paging_row / unified SMS history */

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
int smsdb_put(const char* id, const char* addr, int ref, int parts, int order, const char* msg, struct ast_str** out);
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

#endif
