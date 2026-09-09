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
    ihs_add_test(video_start_lifetime test_video_start_lifetime.c)
    target_link_libraries(ihstest_video_start_lifetime PRIVATE ihs-test-session)
    target_link_options(ihstest_video_start_lifetime PRIVATE
        -Wl,--wrap=calloc
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
