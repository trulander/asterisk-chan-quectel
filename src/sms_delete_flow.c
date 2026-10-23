#include "sms_delete_flow.h"

#include <string.h>

/** Применить production policy к одному результату чтения SIM.
 * Проверяет gate/source/storage прежде callbacks; каждый побочный шаг вызывается
 * только после успеха предыдущего, а временный сбой передаётся defer_retry.
 * Аргументы и коды результата подробно описаны в sms_delete_flow.h; функция
 * не удерживает SQLite lock и сама не отправляет AT-команду.
 */
int sms_delete_flow_process(const sms_flow_request_t* request, sms_flow_read_result_t read_result,
                            const sms_flow_ops_t* ops, void* context, sms_flow_result_t* result)
{
    if (!request || !ops || !result) return -1;
    if (!request->sms_autodelete || request->source != SMS_FLOW_SIM_CELL || !request->device || !*request->device ||
        !request->storage || strcmp(request->storage, "SM") || request->index < 0) return 1;
    if (read_result == SMS_FLOW_READ_ERROR) {
        if (ops->defer_retry) ops->defer_retry(context);
        return -1;
    }
    if (read_result == SMS_FLOW_CELL_ABSENT) {
        if (ops->mark_absent && ops->mark_absent(context)) {
            if (ops->defer_retry) ops->defer_retry(context);
            return -1;
        }
        return 0;
    }
    result->complete = request->complete;
    result->durable_rowid = 0;
    if (read_result != SMS_FLOW_CELL_PRESENT || !ops->save_cell || ops->save_cell(context, result)) {
        if (ops->defer_retry) ops->defer_retry(context);
        return -1;
    }
    if (result->complete && (!ops->queue_complete_sms || ops->queue_complete_sms(context, result))) {
        if (ops->defer_retry) ops->defer_retry(context);
        return -1;
    }
    if (!ops->delete_cell || ops->delete_cell(context, result)) {
        if (ops->defer_retry) ops->defer_retry(context);
        return -1;
    }
    return 0;
}
