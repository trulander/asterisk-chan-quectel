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