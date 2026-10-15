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

# at_pause: queue-native pause deadline arming (arm exactly once; later
# URC/response re-entries must not re-base the absolute deadline). The logic
# is a header-only pure inline (src/at_pause.h), so no production .c is needed.
add_executable(ctq_at_pause_test
    at_pause_test.c
)
target_include_directories(ctq_at_pause_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(ctq_at_pause_test PRIVATE -Wall -Wextra)
add_test(NAME at-pause COMMAND ctq_at_pause_test)
