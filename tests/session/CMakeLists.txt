ihs_add_test(packet_conn_read_write packet_conn_read_write.c)
ihs_add_test(packet_ping_resp packet_ping_resp.c)
ihs_add_test(packet_generation packet_generation.c)
ihs_add_test(ip_address test_ip_address.c)

ihs_add_test(timer test_timer.c)
ihs_add_test(disconnect_destroy test_disconnect_destroy.c)
target_link_libraries(ihstest_disconnect_destroy PRIVATE ihs-test-session)

ihs_add_test(video_control test_video_control.c)
target_link_libraries(ihstest_video_control PRIVATE ihs-test-session)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    ihs_add_test(video_tracked test_video_tracked.c)
    target_link_libraries(ihstest_video_tracked PRIVATE ihs-test-session)
    target_link_options(ihstest_video_tracked PRIVATE -Wl,--wrap=IHS_SessionSendControlMessage)
    set_tests_properties(video_tracked PROPERTIES TIMEOUT 20)
    ihs_add_test(video_start_lifetime test_video_start_lifetime.c)
    target_link_libraries(ihstest_video_start_lifetime PRIVATE ihs-test-session)
    target_link_options(ihstest_video_start_lifetime PRIVATE
        -Wl,--wrap=calloc
        -Wl,--wrap=malloc
        -Wl,--wrap=IHS_SessionSendControlMessage
        -Wl,--wrap=IHS_TimerTaskStart
        -Wl,--wrap=IHS_TimerTaskStopImmediate)
    set_tests_properties(video_start_lifetime PROPERTIES TIMEOUT 20)
endif()

ihs_add_test(control_keyboard test_control_keyboard.c)
target_link_libraries(ihstest_control_keyboard PRIVATE ihs-test-session)

ihs_add_test(frame_stats test_frame_stats.c)
ihs_add_test(window_orphan_frags window_orphan_frags.c)
ihs_add_test(window_head_gap window_head_gap.c)
ihs_add_test(window_join_midmessage window_join_midmessage.c)
ihs_add_test(retransmission_state_machine retransmission_state_machine.c)
ihs_add_test(control_hid_admission control_hid_admission.c)
target_link_libraries(ihstest_control_hid_admission PRIVATE ihs-test-session)

add_subdirectory(video)

ihs_add_test(protocol_evidence test_protocol_evidence.c)
target_link_libraries(ihstest_protocol_evidence PRIVATE ihs-test-session)

ihs_add_test(hid_send_order test_hid_send_order.c)
target_link_libraries(ihstest_hid_send_order PRIVATE ihs-test-session)

ihs_add_test(game_activity test_game_activity.c)
target_link_libraries(ihstest_game_activity PRIVATE ihs-test-session)

ihs_add_test(launch_transactions test_launch_transactions.c)
target_link_libraries(ihstest_launch_transactions PRIVATE ihs-test-session)

ihs_add_test(frame_tracker test_frame_tracker.c)
set_tests_properties(frame_tracker PROPERTIES TIMEOUT 20)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(ihstest_frame_tracker PRIVATE IHS_TEST_ALLOC_FAILURE)
    target_link_options(ihstest_frame_tracker PRIVATE -Wl,--wrap=calloc)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    ihs_add_test(video_report test_video_report.c)
    target_link_libraries(ihstest_video_report PRIVATE ihs-test-session)
    target_link_options(ihstest_video_report PRIVATE
        -Wl,--wrap=IHS_SessionSendControlMessage
        -Wl,--wrap=IHS_TimerTaskStart
        -Wl,--wrap=IHS_SessionChannelStatsSend)
    set_tests_properties(video_report PROPERTIES TIMEOUT 20)
endif()

ihs_add_test(client_capabilities test_client_capabilities.c)
target_link_libraries(ihstest_client_capabilities PRIVATE ihs-test-session)

ihs_add_test(media_recovery test_media_recovery.c)
target_link_libraries(ihstest_media_recovery PRIVATE ihs-test-session)
set_tests_properties(media_recovery PROPERTIES TIMEOUT 10)

ihs_add_test(fec_channel test_fec_channel.c)
target_link_libraries(ihstest_fec_channel PRIVATE ihs-test-session)
set_tests_properties(fec_channel PROPERTIES TIMEOUT 10)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    ihs_add_test(transport_worker test_transport_worker.c)
    target_link_libraries(ihstest_transport_worker PRIVATE ihs-test-session)
    target_link_options(ihstest_transport_worker PRIVATE
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_UDPSocketReceive)
    set_tests_properties(transport_worker PROPERTIES TIMEOUT 10)
    ihs_add_test(client_config test_client_config.c)
    target_link_libraries(ihstest_client_config PRIVATE ihs-test-session)
    set_tests_properties(client_config PROPERTIES TIMEOUT 20)
    add_executable(ihs-client-config-oracle client_config_oracle.c)
    target_include_directories(ihs-client-config-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-client-config-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-client-config-oracle PRIVATE -UNDEBUG)
    add_executable(ihs-transport-oracle transport_oracle.c)
    target_include_directories(ihs-transport-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-transport-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-transport-oracle PRIVATE -UNDEBUG)
    target_link_options(ihs-transport-oracle PRIVATE
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_SessionDisconnect
        -Wl,--wrap=IHS_BaseSend)
    add_executable(ihs-fec-oracle fec_oracle.c)
    target_include_directories(ihs-fec-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-fec-oracle PRIVATE ihslib)
    target_compile_options(ihs-fec-oracle PRIVATE -UNDEBUG)
    add_executable(ihs-media-oracle media_oracle.c)
    target_include_directories(ihs-media-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-media-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-media-oracle PRIVATE -UNDEBUG)
    target_link_options(ihs-media-oracle PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME media_recovery_oracle COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/check_recovery_oracle.py"
        --media $<TARGET_FILE:ihs-media-oracle>
        --video $<TARGET_FILE:ihstest_decoder_loss>
        --fixture "${CMAKE_CURRENT_SOURCE_DIR}/recovery_oracle.json")
    set_tests_properties(media_recovery_oracle PROPERTIES TIMEOUT 60)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(ihs-retry-sender-oracle retry_sender_oracle.c)
    target_include_directories(ihs-retry-sender-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-retry-sender-oracle PRIVATE ihslib ihs-test-session)
    target_compile_options(ihs-retry-sender-oracle PRIVATE -UNDEBUG)
    target_link_options(ihs-retry-sender-oracle PRIVATE
        -Wl,--wrap=IHS_BaseSend -Wl,--wrap=IHS_SessionPacketTimestamp)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    ihs_add_test(retry_sender test_retry_sender.c)
    target_link_libraries(ihstest_retry_sender PRIVATE ihs-test-session)
    set_tests_properties(retry_sender PROPERTIES TIMEOUT 20)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(ihs-outgoing-oracle outgoing_oracle.c)
    target_include_directories(ihs-outgoing-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-outgoing-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-outgoing-oracle PRIVATE -UNDEBUG)
    target_link_options(ihs-outgoing-oracle PRIVATE -Wl,--wrap=IHS_BaseSend -Wl,--wrap=IHS_SessionPacketTimestamp)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    ihs_add_test(outgoing test_outgoing.c)
    target_link_libraries(ihstest_outgoing PRIVATE ihs-test-session)
    ihs_add_test(outgoing_worker test_outgoing_worker.c)
    target_link_libraries(ihstest_outgoing_worker PRIVATE ihs-test-session)
    target_link_options(ihstest_outgoing_worker PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_UDPSocketReceive -Wl,--wrap=IHS_SessionPacketTimestamp)
    set_tests_properties(outgoing outgoing_worker PROPERTIES TIMEOUT 20)
endif()
