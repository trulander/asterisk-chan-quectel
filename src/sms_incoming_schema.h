#ifndef CHAN_QUECTEL_SMS_INCOMING_SCHEMA_H_INCLUDED
#define CHAN_QUECTEL_SMS_INCOMING_SCHEMA_H_INCLUDED

#include <stddef.h>
#include <sqlite3.h>

/* Create or migrate incoming_msg with durable SIM-cell deletion metadata. */
int sms_incoming_schema_ensure(sqlite3* db);

/* Apply a deletion transition only if rowid still names the expected SIM cell.
 * Caller must hold the DB mutex and an open transaction. Returns 0 only when
 * exactly one row matched the complete source identity. */
int sms_incoming_delete_state_update(sqlite3* db, long long rowid, const char* device, const char* storage, int index,
                                    int status, unsigned attempts, int retry_at);
/*! Move a blocked due job to a future scan without changing its status or attempts. */
int sms_incoming_delete_retry_defer(sqlite3* db, long long rowid, int retry_at);

/* These SQLite helpers are shared with production and integration tests. Unless
 * noted otherwise, callers serialize use of db and include mutations in their
 * surrounding transaction. */
/*! Read stable group marker/MID before generating another candidate MID. */
int sms_incoming_group_queue_state(sqlite3* db, const char* key, int* queued, char* mid, size_t mid_len);
/*! Store the committed MT MID as the group idempotency marker. */
int sms_incoming_group_mark_queued(sqlite3* db, const char* key, const char* mid);
/*! In the caller's transaction, insert MT row + marker atomically or return the already committed MID. */
int sms_incoming_mt_queue_group(sqlite3* db, const char* key, const char* device, const char* candidate_mid,
                                const char* from_num, const char* message, const char* timestamp,
                                char* effective_mid, size_t effective_mid_len, int* inserted);
int sms_incoming_gc_group(sqlite3* db, const char* key);
int sms_incoming_gc_completed_groups(sqlite3* db);
/* Due-job contract: 1 = job returned, 0 = none, -1 = DB/argument error. */
int sms_incoming_delete_job_pick(sqlite3* db, int now, long long* rowid, char* device, size_t device_len, int* index);

#endif
