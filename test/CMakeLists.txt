# Unit tests для helper-модулей без Asterisk. Каждый target компилирует production source,
# который покрывает тест; production код не зависит от тестовых файлов. CTest включён корневым CMakeLists.

# web_util: RSSI bars, строгий integer parsing, clamping страниц, pagination и query parameters.
add_executable(ctq_web_util_test
    web_util_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/web_util.c
)
target_include_directories(ctq_web_util_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(ctq_web_util_test PRIVATE -Wall -Wextra)
add_test(NAME web-util COMMAND ctq_web_util_test)

# sms_paging: unified MT+MO history, per-SIM filter, newest-first order и count на настоящей in-memory SQLite.
add_executable(ctq_sms_paging_test
    sms_paging_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_paging.c
)
target_include_directories(ctq_sms_paging_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(ctq_sms_paging_test PRIVATE SQLite::SQLite3)
target_compile_options(ctq_sms_paging_test PRIVATE -Wall -Wextra)
add_test(NAME sms-paging COMMAND ctq_sms_paging_test)

# incoming_msg pager: production SQL на настоящей SQLite, filter/order/page/errors/no-mutation.
add_executable(ctq_sms_incoming_paging_test
    sms_incoming_paging_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_incoming_paging.c
)
target_include_directories(ctq_sms_incoming_paging_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(ctq_sms_incoming_paging_test PRIVATE SQLite::SQLite3)
target_compile_options(ctq_sms_incoming_paging_test PRIVATE -Wall -Wextra)
add_test(NAME sms-incoming-paging COMMAND ctq_sms_incoming_paging_test)

# Контрактные incoming-parts helpers, которые напрямую вызываются mt_task.c: route/escaping/errors/URL.
add_executable(ctq_web_incoming_parts_test
    web_incoming_parts_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/web_incoming_parts.c
)
target_include_directories(ctq_web_incoming_parts_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(ctq_web_incoming_parts_test PRIVATE -Wall -Wextra)
add_test(NAME web-incoming-parts COMMAND ctq_web_incoming_parts_test)

# AT mini-web: production-used разборщик, HTTP reader, сборщик ответа и submit seam; проверки framing, escaping и ошибок.
add_executable(ctq_web_at_command_test web_at_command_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../src/web_at_command.c)
target_include_directories(ctq_web_at_command_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(ctq_web_at_command_test PRIVATE -Wall -Wextra)
add_test(NAME web-at-command COMMAND ctq_web_at_command_test)

# sms_multipart: production SQLite helper из smsdb_put(); out-of-order assembly/queue на in-memory DB.
# Multipart/schema regression: migration, rowid/source identity, 1,3,2 assembly и COMMIT failure.
add_executable(ctq_sms_multipart_test
    sms_multipart_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_multipart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_incoming_schema.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_tx.c
)
target_include_directories(ctq_sms_multipart_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(ctq_sms_multipart_test PRIVATE SQLite::SQLite3 Threads::Threads)
target_compile_options(ctq_sms_multipart_test PRIVATE -Wall -Wextra)
add_test(NAME sms-multipart COMMAND ctq_sms_multipart_test)

# sms_delete_state: production state transitions удаления/retry, включая лимит и recovery.
# Таблица persisted state transitions, recovery и лимит подтверждённых ошибок CMGD.
add_executable(ctq_sms_delete_state_test
    sms_delete_state_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_delete_state.c
)
target_include_directories(ctq_sms_delete_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(ctq_sms_delete_state_test PRIVATE -Wall -Wextra)
add_test(NAME sms-delete-state COMMAND ctq_sms_delete_state_test)

# sms_delete_flow: production policy порядка read/save/queue/delete на callbacks.
add_executable(ctq_sms_delete_flow_test
    sms_delete_flow_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_delete_flow.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_delete_state.c
)
target_include_directories(ctq_sms_delete_flow_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(ctq_sms_delete_flow_test PRIVATE -Wall -Wextra)
add_test(NAME sms-delete-flow COMMAND ctq_sms_delete_flow_test)

# SQLite-backed deletion flow: production schema/helpers, queue marker, restart picker и confirmed-absence GC.
add_executable(ctq_sms_delete_flow_sqlite_test
    sms_delete_flow_sqlite_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_delete_flow.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_delete_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_incoming_schema.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_multipart.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_tx.c
)
target_include_directories(ctq_sms_delete_flow_sqlite_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(ctq_sms_delete_flow_sqlite_test PRIVATE SQLite::SQLite3 Threads::Threads)
target_compile_options(ctq_sms_delete_flow_sqlite_test PRIVATE -Wall -Wextra)
add_test(NAME sms-delete-flow-sqlite COMMAND ctq_sms_delete_flow_sqlite_test)

# Журнал звонков: production-транзакции SQLite и существующий MT queue helper.
# Покрыты типы, идемпотентность, SIM, повтор индекса, рестарт и откат COMMIT.
add_executable(ctq_call_log_test
    call_log_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/call_log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_multipart.c
)
target_include_directories(ctq_call_log_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(ctq_call_log_test PRIVATE SQLite::SQLite3)
target_compile_options(ctq_call_log_test PRIVATE -Wall -Wextra)
add_test(NAME call-log COMMAND ctq_call_log_test)

# Call history production SQL pager: exact SIM, durable payload/order/pagination/error на in-memory SQLite.
add_executable(ctq_call_paging_test call_paging_test.c ${CMAKE_CURRENT_SOURCE_DIR}/../src/call_paging.c ${CMAKE_CURRENT_SOURCE_DIR}/../src/call_log.c ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_multipart.c ${CMAKE_CURRENT_SOURCE_DIR}/../src/web_util.c)
target_include_directories(ctq_call_paging_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(ctq_call_paging_test PRIVATE SQLite::SQLite3)
target_compile_options(ctq_call_paging_test PRIVATE -Wall -Wextra)
add_test(NAME call-paging COMMAND ctq_call_paging_test)

# Строгий production CLCC parser и связанный snapshot-invalid/retry/finish flow на SQLite.
add_executable(ctq_clcc_parser_test
    clcc_parser_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/clcc_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/call_log.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_multipart.c
)
target_include_directories(ctq_clcc_parser_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(ctq_clcc_parser_test PRIVATE SQLite::SQLite3)
target_compile_options(ctq_clcc_parser_test PRIVATE -Wall -Wextra)
add_test(NAME clcc-parser COMMAND ctq_clcc_parser_test)

# Per-SIM home metrics и HTML-фрагмент: production SQL/renderer на in-memory SQLite.
add_executable(ctq_sim_metrics_test
    sim_metrics_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sim_metrics.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_paging.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/web_sim_metrics.c
)
target_include_directories(ctq_sim_metrics_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(ctq_sim_metrics_test PRIVATE SQLite::SQLite3)
target_compile_options(ctq_sim_metrics_test PRIVATE -Wall -Wextra)
add_test(NAME sim-metrics COMMAND ctq_sim_metrics_test)

# sms_tx: независимые SQLite transaction primitives src/sms_tx.[ch]. Проверяются nested-BEGIN,
# параллельные begin/commit одного connection, failed COMMIT→ROLLBACK, failed BEGIN,
# SQLITE_BUSY без утечки transaction, повтор seqorder и NULL SRR mapping.
add_executable(ctq_sms_tx_test
    sms_tx_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_tx.c
)
target_include_directories(ctq_sms_tx_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(ctq_sms_tx_test PRIVATE SQLite::SQLite3 Threads::Threads)
target_compile_options(ctq_sms_tx_test PRIVATE -Wall -Wextra)
add_test(NAME sms-tx COMMAND ctq_sms_tx_test)