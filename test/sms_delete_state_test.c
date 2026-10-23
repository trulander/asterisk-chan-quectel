#include <stdio.h>
#include "sms_delete_state.h"

static int failures;

/* CHECK агрегирует ошибки теста, не прекращая проверку последующих переходов. */
#define CHECK(test, msg) do { \
    if (!(test)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } \
} while (0)

/** Проверить CMGD OK и fresh absent как завершение без расхода failure attempts. */
static void test_success_and_absent_do_not_consume_attempt(void)
{
    sms_delete_state_t state = {SMS_DELETE_PENDING, 0};
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_DELETE_QUEUED) == 0, "CMGD queued");
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_CMGD_ERROR) == 0, "CMGD error transition accepted");
    CHECK(state.status == SMS_DELETE_VERIFY && state.attempts == 0, "CMGD error schedules verification without attempt");
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_VERIFY_START) == 0, "fresh verification starts");
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_VERIFY_ABSENT) == 0, "absent verification accepted");
    CHECK(state.status == SMS_DELETE_COMPLETE && state.attempts == 0, "absent cell is success without attempt");

    state.status = SMS_DELETE_PENDING;
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_DELETE_QUEUED) == 0, "second CMGD queued");
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_CMGD_OK) == 0, "CMGD OK transition accepted");
    CHECK(state.status == SMS_DELETE_COMPLETE && state.attempts == 0, "CMGD OK completes without attempt");
}

/** Проверить только durable present после CMGD failure считается; пятый переход останавливает scheduler. */
static void test_only_confirmed_present_counts_and_stops_at_five(void)
{
    sms_delete_state_t state = {SMS_DELETE_PENDING, 0};
    for (unsigned i = 1; i <= SMS_DELETE_MAX_ATTEMPTS; ++i) {
        CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_DELETE_QUEUED) == 0, "CMGD queued");
        CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_CMGD_TIMEOUT) == 0, "timeout requests fresh verification");
        CHECK(state.status == SMS_DELETE_VERIFY, "timeout does not itself count as failed deletion");
        CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_VERIFY_START) == 0, "fresh read starts");
        CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_VERIFY_PRESENT_SAVED) == 0, "saved present cell confirms failed deletion");
        CHECK(state.attempts == i, "confirmed failed deletion increments exactly once");
        CHECK(state.status == (i == SMS_DELETE_MAX_ATTEMPTS ? SMS_DELETE_STOPPED : SMS_DELETE_PENDING),
              "fifth confirmed failure stops automatic attempts");
    }
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_SAVED) < 0, "stopped entry cannot be restarted by ordinary save");
}

/** Проверить, что persisted in-flight/interrupted verification после restart читает ячейку свежо. */
static void test_restart_recovery_uses_fresh_verification(void)
{
    sms_delete_state_t after_queued_command = {SMS_DELETE_CMGD_SENT, 2};
    CHECK(sms_delete_state_apply(&after_queued_command, SMS_DELETE_EVENT_VERIFY_START) == 0, "restart recovers in-flight CMGD via fresh read");
    CHECK(after_queued_command.status == SMS_DELETE_VERIFYING_UNCOUNTED && after_queued_command.attempts == 2,
          "restart uses fresh verification without treating an unresolved command as a confirmed failure");
    CHECK(sms_delete_state_apply(&after_queued_command, SMS_DELETE_EVENT_VERIFY_PRESENT_SAVED) == 0 &&
              after_queued_command.status == SMS_DELETE_PENDING && after_queued_command.attempts == 2,
          "present content after restart does not consume a confirmed failure");

    sms_delete_state_t after_interrupted_scan = {SMS_DELETE_VERIFYING, 4};
    CHECK(sms_delete_state_apply(&after_interrupted_scan, SMS_DELETE_EVENT_VERIFY_START) == 0, "restart can resume interrupted verification");
    CHECK(after_interrupted_scan.status == SMS_DELETE_VERIFYING && after_interrupted_scan.attempts == 4,
          "restart resumes verification after a confirmed delete error");
    CHECK(sms_delete_state_apply(&after_interrupted_scan, SMS_DELETE_EVENT_VERIFY_PRESENT_SAVED) == 0 &&
              after_interrupted_scan.status == SMS_DELETE_STOPPED && after_interrupted_scan.attempts == 5,
          "present cell after restart consumes the fifth confirmed failure");
}

/** Проверить, что локальный отказ AT enqueue не притворяется выполненным CMGD failure. */
static void test_enqueue_failure_does_not_count(void)
{
    sms_delete_state_t state = {SMS_DELETE_PENDING, 0};
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_DELETE_QUEUED) == 0, "delete queue transition");
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_QUEUE_FAILED) == 0, "enqueue failure returns to pending");
    CHECK(state.status == SMS_DELETE_PENDING && state.attempts == 0, "enqueue failure is not a confirmed deletion attempt");
}

/** Проверить retry для read/DB ошибок без расхода лимита и последующий подтверждённый present. */
static void test_read_and_database_errors_do_not_count(void)
{
    sms_delete_state_t state = {SMS_DELETE_PENDING, 2};
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_DELETE_QUEUED) == 0, "CMGD queued before error");
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_CMGD_ERROR) == 0, "error enters verification");
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_VERIFY_START) == 0, "verification scan starts");
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_VERIFY_READ_ERROR) == 0, "read error remains retryable");
    CHECK(state.status == SMS_DELETE_VERIFY && state.attempts == 2, "read error does not count");
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_DB_ERROR) == 0, "database error remains retryable");
    CHECK(state.status == SMS_DELETE_VERIFY && state.attempts == 2, "database error does not count");
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_VERIFY_START) == 0, "retry scan starts after recoverable error");
    CHECK(sms_delete_state_apply(&state, SMS_DELETE_EVENT_VERIFY_PRESENT_SAVED) == 0, "present is countable only after durable save");
    CHECK(state.status == SMS_DELETE_PENDING && state.attempts == 3, "durably saved present cell increments once");
}

/** Запустить независимые сценарии переходов и сообщить общий результат CTest. */
int main(void)
{
    test_success_and_absent_do_not_consume_attempt();
    test_only_confirmed_present_counts_and_stops_at_five();
    test_restart_recovery_uses_fresh_verification();
    test_enqueue_failure_does_not_count();
    test_read_and_database_errors_do_not_count();
    if (failures) {
        fprintf(stderr, "%d sms delete state test(s) failed\n", failures);
        return 1;
    }
    puts("sms delete state tests passed");
    return 0;
}
