# Unit tests for the pure (non-Asterisk) helper modules. Each test compiles
# the production source it covers directly, so nothing production-side depends
# on test-only code. CTest is enabled from the top-level CMakeLists.

# web_util: RSSI bars / int parsing / page clamping / pagination / query params
add_executable(ctq_web_util_test
    web_util_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/web_util.c
)
target_include_directories(ctq_web_util_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(ctq_web_util_test PRIVATE -Wall -Wextra)
add_test(NAME web-util COMMAND ctq_web_util_test)

# sms_paging: unified SMS history pagination (MT+MO merge, per-SIM filter,
# newest-first order, count metadata) against a real in-memory SQLite DB.
add_executable(ctq_sms_paging_test
    sms_paging_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_paging.c
)
target_include_directories(ctq_sms_paging_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(ctq_sms_paging_test PRIVATE SQLite::SQLite3)
target_compile_options(ctq_sms_paging_test PRIVATE -Wall -Wextra)
add_test(NAME sms-paging COMMAND ctq_sms_paging_test)

# sms_multipart: the same production SQLite helper used by smsdb_put(), with
# actual out-of-order assembly and queue insertion against an in-memory DB.
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

# sms_delete_state: production-used deletion/retry state transitions.
add_executable(ctq_sms_delete_state_test
    sms_delete_state_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_delete_state.c
)
target_include_directories(ctq_sms_delete_state_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(ctq_sms_delete_state_test PRIVATE -Wall -Wextra)
add_test(NAME sms-delete-state COMMAND ctq_sms_delete_state_test)

# sms_delete_flow: production orchestration of read/save/queue/delete policy.
add_executable(ctq_sms_delete_flow_test
    sms_delete_flow_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_delete_flow.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_delete_state.c
)
target_include_directories(ctq_sms_delete_flow_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(ctq_sms_delete_flow_test PRIVATE -Wall -Wextra)
add_test(NAME sms-delete-flow COMMAND ctq_sms_delete_flow_test)

# End-to-end deletion flow with production SQLite schema, multipart helpers,
# durable queue markers, restart picker, and confirmed-absence group GC.
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

# sms_tx: dependency-free SQLite transaction primitives (src/sms_tx.[ch]).
# Exercises the nested-BEGIN fix directly against an in-memory database:
# concurrent begin/commit on one connection, failed COMMIT -> ROLLBACK (no
# half-open connection), failed BEGIN, COMMIT contention/SQLITE_BUSY -> no
# leak, multipart out-of-order/duplicate seqorder, and SRR-NULL rendering.
add_executable(ctq_sms_tx_test
    sms_tx_test.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sms_tx.c
)
target_include_directories(ctq_sms_tx_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(ctq_sms_tx_test PRIVATE SQLite::SQLite3 Threads::Threads)
target_compile_options(ctq_sms_tx_test PRIVATE -Wall -Wextra)
add_test(NAME sms-tx COMMAND ctq_sms_tx_test)