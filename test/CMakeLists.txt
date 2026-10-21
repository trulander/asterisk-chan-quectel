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