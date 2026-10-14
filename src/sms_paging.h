/*
   Unified SMS history pagination for the mini-web.

   Pure SQLite module (no Asterisk dependency) so it can be unit-tested
   directly against an in-memory database. Production code (smsdb.c) drives it
   through the module's own (mutex-guarded) connection; the web thread never
   touches the connection without the SMSDB lock.

   The unified view merges the two separate SMS record families:
     - 'mt' (SIM -> SIP, table mt_queue)
     - 'mo' (SIP -> SIM, table mt_out)
   Newest first (created_at DESC, sequence DESC), one row per SMS, with the
   direction explicitly tagged in row->direction.
*/

#ifndef CHAN_QUECTEL_SMS_PAGING_H
#define CHAN_QUECTEL_SMS_PAGING_H

#include <stddef.h>

#include <sqlite3.h>

/*! Buffer sizes for the unified row. They mirror the limits used by the
    smsdb tables/structs (SMS_PAGING_DEV_LEN is a superset of DEVNAMELEN). */
#define SMS_PAGING_DEV_LEN   64
#define SMS_PAGING_DIR_LEN   8
#define SMS_PAGING_REF_LEN   128
#define SMS_PAGING_PEER_LEN  64
#define SMS_PAGING_MSG_LEN   4096
#define SMS_PAGING_TS_LEN    64
#define SMS_PAGING_STATE_LEN 32

/*! Unified SMS history row: one entry per SMS message, with the direction
    explicitly tagged ('mt' = incoming SIM->SIP from mt_queue; 'mo' = outgoing
    SIP->SIM from mt_out). The individual MT and MO records keep their own,
    separate shape (struct smsdb_mt_row / struct smsdb_mt_out_row); this is
    only the merged, direction-tagged view used by the mini-web. */
struct sms_paging_row {
    char direction[SMS_PAGING_DIR_LEN];  /*!< "mt" or "mo" */
    char device[SMS_PAGING_DEV_LEN];
    long id;                             /*!< mt_queue rowid / mt_out.id */
    char ref_id[SMS_PAGING_REF_LEN];     /*!< mt_queue.mid / decimal mt_out.id */
    char peer[SMS_PAGING_PEER_LEN];      /*!< mt_queue.from_num / mt_out.dst */
    char msg[SMS_PAGING_MSG_LEN];
    char ts[SMS_PAGING_TS_LEN];
    char state[SMS_PAGING_STATE_LEN];
    long created_at;
};

typedef int (*sms_paging_row_cb)(void* ctx, const struct sms_paging_row* row);

/*! Total number of SMS records (MT and MO) for \a device, or all devices when
    \a device is NULL or empty.
    \note The caller must serialize access to \a db (the smsdb module holds the
    SQLite mutex while calling these).
    \return 0 on success and stores the count in *total, -1 on error */
int sms_paging_count(sqlite3* db, const char* device, int* total);

/*! List one page of the unified SMS history (MT and MO merged, newest first).
    \param device device name to filter, or NULL/empty for all devices
    \param page_size max items per page (> 0)
    \param offset 0-based row offset (already validated by the caller)
    \param ctx context passed to \a cb
    \param cb row visitor; returning non-zero stops iteration
    \note The caller must serialize access to \a db.
    \return 0 on success, -1 on error */
int sms_paging_page(sqlite3* db, const char* device, int page_size, int offset, void* ctx, sms_paging_row_cb cb);

#endif