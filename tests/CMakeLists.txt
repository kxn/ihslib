set(TEST_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../include ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_SOURCE_DIR})

function(ihs_add_test NAME SOURCES)
    set(TEST_EXE "ihstest_${NAME}")
    add_executable(${TEST_EXE} ${SOURCES})
    add_test(${NAME} ${TEST_EXE})
    target_include_directories(${TEST_EXE} PRIVATE ${TEST_INCLUDES})
    target_link_libraries(${TEST_EXE} PRIVATE ihslib)
    # Tests use `assert(call(...) == expected)` heavily, which silently drops the
    # function call under NDEBUG (Release). Force assertions on for every test
    # binary regardless of the project build type.
    target_compile_options(${TEST_EXE} PRIVATE -UNDEBUG)
    set_tests_properties(${NAME} PROPERTIES SKIP_RETURN_CODE 127)
    set(IHSTEST_TARGET "${TEST_EXE}" PARENT_SCOPE)
endfunction()

add_subdirectory(common)

ihs_add_test(arraylist test_arraylist.c)
ihs_add_test(enumeration test_enumeration.c)
ihs_add_test(crypto test_crypto.c)
ihs_add_test(authorization test_authorization.c)
ihs_add_test(udp_broadcast test_udp_broadcast.c)
# Calls into ihs-platforms directly, which needs ihslib again after it.
target_link_libraries(${IHSTEST_TARGET} PRIVATE ihs-platforms ihslib)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(ihs-udp-receive-oracle udp_receive_oracle.c)
    target_include_directories(ihs-udp-receive-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-udp-receive-oracle PRIVATE ihslib ihs-platforms ihslib)
    target_compile_options(ihs-udp-receive-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-udp-receive-oracle PRIVATE -Wl,--wrap=select)
    add_executable(ihs-thread-clock-oracle thread_clock_oracle.c)
    target_include_directories(ihs-thread-clock-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-thread-clock-oracle PRIVATE ihs-platforms)
    target_compile_options(ihs-thread-clock-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-thread-clock-oracle PRIVATE -Wl,--wrap=pthread_condattr_init
        -Wl,--wrap=pthread_condattr_setclock -Wl,--wrap=pthread_condattr_destroy
        -Wl,--wrap=pthread_cond_init -Wl,--wrap=pthread_cond_destroy
        -Wl,--wrap=clock_gettime -Wl,--wrap=pthread_cond_timedwait)
    ihs_add_test(base_log_lock test_base_log_lock.c)
    set_tests_properties(base_log_lock PROPERTIES TIMEOUT 10)
    ihs_add_test(udp_send test_udp_send.c)
    target_link_libraries(${IHSTEST_TARGET} PRIVATE ihs-platforms ihslib)
    target_link_options(${IHSTEST_TARGET} PRIVATE -Wl,--wrap=sendto)
endif()
ihs_add_test(hid_report_batch test_hid_report_batch.c)
ihs_add_test(hid_report_chain test_hid_report_chain.c)
ihs_add_test(hid_report_replace test_hid_report_replace.c)

add_subdirectory(hid)
add_subdirectory(session)
