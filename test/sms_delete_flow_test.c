#include "sms_delete_flow.h"
#include "sms_delete_state.h"

#include <stdio.h>
#include <string.h>

typedef struct fake_flow {
    char sequence[32]; /*!< Упорядоченная callback trace с завершающим NUL после каждого append. */
    int sequence_len; /*!< Число записанных байтов trace. */
    int save_fail; /*!< Инъецировать ошибку до подтверждения fake durable save. */
    int queue_fail; /*!< Инъецировать ошибку постановки complete message в queue. */
    int delete_count; /*!< Число имитаций CMGD enqueue; реальный модем не затрагивается. */
    int queued_count; /*!< Успешные fake операции постановки полной MT записи. */
    int saved_count; /*!< Успешные fake durable writes сегмента. */
    int retry_count; /*!< Отложенные временные ошибки, не считающиеся попытками удаления. */
    int absent_count; /*!< Число вызовов durable absent callback. */
    int verifying; /*!< Включает fresh-present state event внутри delete callback. */
    sms_delete_state_t state; /*!< In-memory копия persisted transition state. */
    const char* expected_device; /*!< Ожидаемая source device identity. */
    const char* expected_storage; /*!< Ожидаемый source storage. */
    int expected_index; /*!< Ожидаемый индекс SIM ячейки. */
    const sms_flow_request_t* request; /*!< Borrowed request, проверяемый callbacks без передачи владения. */
} fake_flow_t;

static int failures;
/* CHECK увеличивает общий счётчик fail и продолжает тест, чтобы за один запуск увидеть несколько нарушений. */
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); ++failures; } } while (0)

/** Записать символ шага в bounded sequence trace для проверки callback order. */
static void mark(fake_flow_t* flow, char event)
{
    if (flow->sequence_len < (int)sizeof(flow->sequence) - 1) {
        flow->sequence[flow->sequence_len++] = event;
        flow->sequence[flow->sequence_len] = '\0';
    }
}

/** Fake durable-save callback: проверяет device/storage/index и возвращает completion/rowid. */
static int save_cell(void* opaque, sms_flow_result_t* result)
{
    fake_flow_t* flow = opaque;
    mark(flow, 'S');
    if (flow->save_fail) return -1;
    CHECK(!strcmp(flow->request->device, flow->expected_device) && !strcmp(flow->request->storage, flow->expected_storage) &&
              flow->request->index == flow->expected_index,
          "save is correlated to the exact source device/storage/index");
    ++flow->saved_count;
    result->complete = flow->request->complete;
    result->durable_rowid = 11;
    return 0;
}

/** Fake queue callback для наблюдения порядка и инъекции ошибки queue commit. */
static int queue_sms(void* opaque, const sms_flow_result_t* result)
{
    fake_flow_t* flow = opaque;
    (void)result;
    mark(flow, 'Q');
    if (flow->queue_fail) return -1;
    ++flow->queued_count;
    return 0;
}

/** Fake CMGD handoff: требует предшествующий save/queue и моделирует transition state. */
static int delete_cell(void* opaque, const sms_flow_result_t* result)
{
    fake_flow_t* flow = opaque;
    (void)result;
    mark(flow, 'D');
    CHECK(flow->saved_count > 0, "delete is after durable segment save");
    CHECK(!flow->request->complete || flow->queued_count > 0, "complete SMS is queued before SIM deletion");
    CHECK(!strcmp(flow->request->storage, flow->expected_storage) && flow->request->index == flow->expected_index,
          "delete targets the same SIM storage/index as the durable source");
    if (flow->verifying) {
        CHECK(sms_delete_state_apply(&flow->state, SMS_DELETE_EVENT_VERIFY_PRESENT_SAVED) == 0, "persist present verification result before retry");
        if (flow->state.status == SMS_DELETE_STOPPED) return 0;
    }
    if (flow->state.status == SMS_DELETE_PENDING) {
        CHECK(sms_delete_state_apply(&flow->state, SMS_DELETE_EVENT_DELETE_QUEUED) == 0, "persist delete queue before issuing CMGD");
    }
    ++flow->delete_count;
    return 0;
}

/** Fake durable absence callback: фиксирует завершение после fresh empty read. */
static int mark_absent(void* opaque)
{
    fake_flow_t* flow = opaque;
    mark(flow, 'A');
    ++flow->absent_count;
    if (flow->state.status == SMS_DELETE_VERIFYING || flow->state.status == SMS_DELETE_VERIFYING_UNCOUNTED)
        return sms_delete_state_apply(&flow->state, SMS_DELETE_EVENT_VERIFY_ABSENT);
    return 0;
}

/** Fake временный retry callback, не считающий deletion failure. */
static void defer_retry(void* opaque)
{
    fake_flow_t* flow = opaque;
    mark(flow, 'R');
    ++flow->retry_count;
    if (flow->state.status == SMS_DELETE_VERIFYING || flow->state.status == SMS_DELETE_VERIFYING_UNCOUNTED)
        sms_delete_state_apply(&flow->state, SMS_DELETE_EVENT_DB_ERROR);
}

static sms_flow_ops_t ops = {save_cell, queue_sms, delete_cell, mark_absent, defer_retry};

/** Создать request с фиксированным SIM source и запомнить ожидаемую identity callbacks. */
static sms_flow_request_t request(fake_flow_t* fake, int auto_delete, sms_flow_source_t source, const char* storage, int complete)
{
    sms_flow_request_t req = {auto_delete, source, "sim-a", storage, 7, complete};
    fake->expected_device = "sim-a";
    fake->expected_storage = storage;
    fake->expected_index = 7;
    return req;
}

/** Сравнить полную последовательность save→queue→delete и запрет queue для неполной группы. */
static void test_present_flow_order_and_incomplete(void)
{
    fake_flow_t fake = {0};
    sms_flow_request_t req = request(&fake, 1, SMS_FLOW_SIM_CELL, "SM", 1);
    fake.request = &req;
    sms_flow_result_t result;
    CHECK(sms_delete_flow_process(&req, SMS_FLOW_CELL_PRESENT, &ops, &fake, &result) == 0, "complete present cell processed");
    CHECK(!strcmp(fake.sequence, "SQD"), "save, durable MT queue, then delete order");
    CHECK(fake.saved_count == 1 && fake.queued_count == 1 && fake.delete_count == 1, "complete flow performs all durable steps");

    memset(&fake, 0, sizeof(fake));
    req = request(&fake, 1, SMS_FLOW_SIM_CELL, "SM", 0);
    fake.request = &req;
    CHECK(sms_delete_flow_process(&req, SMS_FLOW_CELL_PRESENT, &ops, &fake, &result) == 0, "incomplete part processed");
    CHECK(!strcmp(fake.sequence, "SD") && fake.queued_count == 0, "incomplete part is durable before delete and is not partially delivered");
}

/** Покрыть absent/read/save/queue errors и подтвердить отсутствие опасного delete. */
static void test_absent_read_and_database_failures(void)
{
    fake_flow_t fake = {0};
    sms_flow_request_t req = request(&fake, 1, SMS_FLOW_SIM_CELL, "SM", 1);
    fake.request = &req;
    sms_flow_result_t result;
    fake.state = (sms_delete_state_t){SMS_DELETE_VERIFYING, 2};
    CHECK(sms_delete_flow_process(&req, SMS_FLOW_CELL_ABSENT, &ops, &fake, &result) == 0, "absent cell confirms deletion");
    CHECK(!strcmp(fake.sequence, "A") && fake.delete_count == 0 && fake.retry_count == 0, "absence completes without CMGD retry or failure count");
    CHECK(fake.state.status == SMS_DELETE_COMPLETE && fake.state.attempts == 2, "absence after CMGD error succeeds without consuming an attempt");

    memset(&fake, 0, sizeof(fake));
    req = request(&fake, 1, SMS_FLOW_SIM_CELL, "SM", 1);
    fake.request = &req;
    CHECK(sms_delete_flow_process(&req, SMS_FLOW_READ_ERROR, &ops, &fake, &result) < 0, "read error is deferred");
    CHECK(!strcmp(fake.sequence, "R") && fake.delete_count == 0 && fake.queued_count == 0, "read error causes no queue or deletion");

    memset(&fake, 0, sizeof(fake));
    req = request(&fake, 1, SMS_FLOW_SIM_CELL, "SM", 1);
    fake.request = &req;
    fake.save_fail = 1;
    fake.state = (sms_delete_state_t){SMS_DELETE_VERIFYING, 2};
    CHECK(sms_delete_flow_process(&req, SMS_FLOW_CELL_PRESENT, &ops, &fake, &result) < 0, "durable save/commit failure is deferred");
    CHECK(!strcmp(fake.sequence, "SR") && fake.delete_count == 0 && fake.queued_count == 0, "save failure causes no partial delivery or delete");
    CHECK(fake.state.status == SMS_DELETE_VERIFY && fake.state.attempts == 2, "DB/commit failure remains retryable without consuming failure count");

    memset(&fake, 0, sizeof(fake));
    req = request(&fake, 1, SMS_FLOW_SIM_CELL, "SM", 1);
    fake.request = &req;
    fake.queue_fail = 1;
    CHECK(sms_delete_flow_process(&req, SMS_FLOW_CELL_PRESENT, &ops, &fake, &result) < 0, "MT queue commit failure is deferred");
    CHECK(!strcmp(fake.sequence, "SQR") && fake.delete_count == 0, "MT queue failure does not delete SIM cell");
}

/** Проверить autodelete, SM-only, RES_CMT exclusion и независимую source identity. */
static void test_gates_and_source_correlation(void)
{
    fake_flow_t fake = {0};
    sms_flow_result_t result;
    sms_flow_request_t req = request(&fake, 0, SMS_FLOW_SIM_CELL, "SM", 1);
    CHECK(sms_delete_flow_process(&req, SMS_FLOW_CELL_PRESENT, &ops, &fake, &result) == 1 && !fake.sequence_len,
          "sms_autodelete off bypasses SIM persistence and deletion");
    req = request(&fake, 1, SMS_FLOW_SIM_CELL, "ME", 1);
    fake.request = &req;
    CHECK(sms_delete_flow_process(&req, SMS_FLOW_CELL_PRESENT, &ops, &fake, &result) == 1 && !fake.sequence_len,
          "ME storage is excluded");
    req = request(&fake, 1, SMS_FLOW_RES_CMT, "SM", 1);
    fake.request = &req;
    CHECK(sms_delete_flow_process(&req, SMS_FLOW_CELL_PRESENT, &ops, &fake, &result) == 1 && !fake.sequence_len,
          "RES_CMT without a SIM cell is excluded");
    req = request(&fake, 1, SMS_FLOW_SIM_CELL, "SM", 1);
    fake.request = &req;
    req.device = "sim-b";
    fake.expected_device = "sim-b";
    fake.request = &req;
    CHECK(sms_delete_flow_process(&req, SMS_FLOW_CELL_PRESENT, &ops, &fake, &result) == 0, "different source device can be processed independently");
    CHECK(fake.delete_count == 1, "exact device/index correlation is passed to production callbacks");
}

/** Проверить пять counted verification событий и restart повторное чтение/очередь. */
static void test_retry_ceiling_and_restart(void)
{
    sms_delete_state_t state = {SMS_DELETE_VERIFY, 0};
    unsigned queued_deletions = 0;
    for (unsigned i = 0; i < SMS_DELETE_MAX_ATTEMPTS; ++i) {
        CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_VERIFY_START) == 0, "confirmed failed deletion starts fresh read");
        fake_flow_t fake = {0};
        sms_flow_request_t req = request(&fake, 1, SMS_FLOW_SIM_CELL, "SM", 0);
        fake.request = &req;
        fake.state = state;
        fake.verifying = 1;
        sms_flow_result_t result;
        CHECK(sms_delete_flow_process(&req, SMS_FLOW_CELL_PRESENT, &ops, &fake, &result) == 0, "present cell is durably saved before retry");
        queued_deletions += (unsigned)fake.delete_count;
        state = fake.state;
        if (state.status == SMS_DELETE_CMGD_SENT && i + 1 < SMS_DELETE_MAX_ATTEMPTS) {
            CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_CMGD_ERROR) == 0, "retry CMGD failure enters fresh-read state");
        }
    }
    CHECK(state.status == SMS_DELETE_STOPPED && state.attempts == 5 && queued_deletions == 4,
          "five confirmed present failures stop automatic checks without queuing a sixth CMGD");

    sms_delete_state_t restarted = {SMS_DELETE_VERIFY, 2};
    CHECK(sms_delete_state_apply(&restarted, SMS_DELETE_EVENT_VERIFY_START) == 0, "restart resumes with current-cell verification");
    fake_flow_t fake = {0};
    fake.state = restarted;
    fake.verifying = 1;
    sms_flow_request_t req = request(&fake, 1, SMS_FLOW_SIM_CELL, "SM", 1);
    fake.request = &req;
    sms_flow_result_t result;
    CHECK(sms_delete_flow_process(&req, SMS_FLOW_CELL_PRESENT, &ops, &fake, &result) == 0, "restart present cell is reread and saved before delete");
    CHECK(!strcmp(fake.sequence, "SQD") && fake.state.attempts == 3 && fake.delete_count == 1,
          "restart rereads, queues durably, counts confirmed presence, then retries deletion");
}

/** Запустить policy scenarios и вернуть ненулевой exit status при любом провале CHECK. */
int main(void)
{
    test_present_flow_order_and_incomplete();
    test_absent_read_and_database_failures();
    test_gates_and_source_correlation();
    test_retry_ceiling_and_restart();
    if (failures) {
        fprintf(stderr, "%d sms delete flow test(s) failed\n", failures);
        return 1;
    }
    puts("sms delete flow tests passed");
    return 0;
}
