include(GNUInstallDirs)

find_program(CLANG_FORMAT_EXEC clang-format)

set(SRC_FILES
    app.c
    at_command.c
    at_parse.c
    at_queue.c
    at_read.c
    at_response.c
    chan_quectel.c
    channel.c
    char_conv.c
    cli.c
    cpvt.c
    dc_config.c
    error.c
    eventfd.c
    helpers.c
    memmem.c
    mixbuffer.c
    monitor_thread.c
    msg_tech.c
    mt_task.c
    pcm.c
    pdu.c
    ringbuffer.c
    smsdb.c
    sms_paging.c
    sms_tx.c
    tty.c
    web_util.c
)

add_library(asterisk-chan-quectel SHARED ${SRC_FILES})

target_link_libraries(asterisk-chan-quectel PRIVATE
    AsteriskModule
    Threads::Threads
    ALSA::ALSA
    SQLite::SQLite3
    Iconv::Iconv
)

# Locate the Asterisk include directory (used to reach the bundled pjproject
# headers needed by res_pjsip.h). The builder sets CMAKE_INCLUDE_PATH to the
# Asterisk source 'include' dir, so find_path() resolves it here.
find_path(MT_AST_INCLUDE_DIR NAMES asterisk/autoconfig.h)

set(MT_PJPROJECT_SOURCE_DIR "${MT_AST_INCLUDE_DIR}/../third-party/pjproject/source")

if(MT_AST_INCLUDE_DIR AND EXISTS "${MT_PJPROJECT_SOURCE_DIR}/pjsip/include/pjsip.h")
    target_include_directories(asterisk-chan-quectel PRIVATE
        ${CMAKE_BINARY_DIR}/generated
        ${MT_PJPROJECT_SOURCE_DIR}/pjsip/include
        ${MT_PJPROJECT_SOURCE_DIR}/pjlib/include
        ${MT_PJPROJECT_SOURCE_DIR}/pjlib-util/include
        ${MT_PJPROJECT_SOURCE_DIR}/pjmedia/include
        ${MT_PJPROJECT_SOURCE_DIR}/pjnath/include
    )
else()
    # mt_task.c includes <asterisk/res_pjsip.h> which needs the bundled
    # pjproject headers; without them that file cannot be built. Still allow
    # the rest of the driver to compile by keeping the generated dir on the
    # include path.
    target_include_directories(asterisk-chan-quectel PRIVATE
        ${CMAKE_BINARY_DIR}/generated
    )
endif()

target_compile_definitions(asterisk-chan-quectel PRIVATE
    _GNU_SOURCE
    HAVE_CONFIG_H
    # Endianness/auto-config macros required by the bundled pjproject headers
    # (pj/config.h refuses to build without them; identical to what Asterisk
    # passes in third-party/pjproject/build.mak).
    PJ_AUTOCONF=1
    PJ_IS_BIG_ENDIAN=0
    PJ_IS_LITTLE_ENDIAN=1
)

set_target_properties(asterisk-chan-quectel PROPERTIES
    LIBRARY_OUTPUT_NAME ${AST_MODULE}
    PREFIX ""
)

install(TARGETS asterisk-chan-quectel
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/asterisk/modules
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/asterisk/modules
)

install(FILES ${CMAKE_SOURCE_DIR}/quectel.conf
    DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/asterisk
)

if(CLANG_FORMAT_EXEC)
    file(GLOB ALL_SRC_FILES *.c *.h)

    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXEC} -i ${ALL_SRC_FILES}
    )

    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT_EXEC} --dry-run --Werror ${ALL_SRC_FILES}
    )
endif()
