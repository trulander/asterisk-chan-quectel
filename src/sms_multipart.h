#ifndef CHAN_QUECTEL_SMS_MULTIPART_H_INCLUDED
#define CHAN_QUECTEL_SMS_MULTIPART_H_INCLUDED

#include <sqlite3.h>

/* Insert one part and assemble on count == parts, ordered by seqorder.
 * Returns the current part count, or -1 on a SQLite/assembly error. The caller
 * owns *assembled and must release it with sqlite3_free(); it is NULL until the group is complete. */
int sms_multipart_put(sqlite3* db, const char* key, int seqorder, int parts, int ttl, const char* message, const char* source_device,
                     const char* source_storage, int source_index, int retain_parts, char** assembled, long long* rowid);
/*! Insert the durable incoming MT queue row using the production schema; caller owns serialization/transaction policy. */
int sms_mt_queue_put(sqlite3* db, const char* device, const char* mid, const char* from_num, const char* message, const char* timestamp);

#endif
