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
    add_executable(ihs-auto-authorization-oracle auto_authorization_oracle.c)
    target_include_directories(ihs-auto-authorization-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-auto-authorization-oracle PRIVATE ihslib)
    target_compile_options(ihs-auto-authorization-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-auto-authorization-oracle PRIVATE -Wl,--wrap=time
        -Wl,--wrap=IHS_CryptoRandomBytes -Wl,--wrap=IHS_CryptoRandomUInt32
        -Wl,--wrap=IHS_CryptoKeyExchange -Wl,--wrap=IHS_CryptoSymmetricEncrypt
        -Wl,--wrap=IHS_CryptoRSAEncrypt -Wl,--wrap=IHS_ClientSend)

    add_executable(ihs-device-token-oracle device_token_oracle.c)
    target_include_directories(ihs-device-token-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-device-token-oracle PRIVATE ihslib)
    target_compile_options(ihs-device-token-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-device-token-oracle PRIVATE -Wl,--wrap=IHS_ClientCreateDeviceToken
        -Wl,--wrap=IHS_CryptoSymmetricEncrypt -Wl,--wrap=IHS_CryptoRSAEncrypt
        -Wl,--wrap=IHS_CryptoRandomUInt32 -Wl,--wrap=clock_gettime -Wl,--wrap=time)
    add_executable(ihs-streaming-pin-oracle streaming_pin_oracle.c)
    target_include_directories(ihs-streaming-pin-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-streaming-pin-oracle PRIVATE ihslib)
    target_compile_options(ihs-streaming-pin-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-streaming-pin-oracle PRIVATE -Wl,--wrap=IHS_CryptoRandomUInt32
        -Wl,--wrap=IHS_CryptoSymmetricEncrypt -Wl,--wrap=clock_gettime -Wl,--wrap=time)
    add_executable(ihs-authorization-exchange-oracle authorization_exchange_oracle.c)
    target_include_directories(ihs-authorization-exchange-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-authorization-exchange-oracle PRIVATE ihslib)
    target_compile_options(ihs-authorization-exchange-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-authorization-exchange-oracle PRIVATE -Wl,--wrap=IHS_CryptoRandomBytes
        -Wl,--wrap=IHS_CryptoRandomUInt32 -Wl,--wrap=IHS_CryptoKeyExchange
        -Wl,--wrap=IHS_CryptoRSAEncrypt -Wl,--wrap=IHS_ClientAuthorizationCallback
        -Wl,--wrap=clock_gettime -Wl,--wrap=time -Wl,--wrap=IHS_ClientSend)
    add_executable(ihs-session-key-oracle session_key_oracle.c)
    target_include_directories(ihs-session-key-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-session-key-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-session-key-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    add_executable(ihs-streaming-transaction-oracle streaming_transaction_oracle.c)
    target_include_directories(ihs-streaming-transaction-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-streaming-transaction-oracle PRIVATE ihslib)
    target_compile_options(ihs-streaming-transaction-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-streaming-transaction-oracle PRIVATE -Wl,--wrap=IHS_CryptoRandomUInt32 -Wl,--wrap=time)
    add_executable(ihs-network-test-connect-oracle network_test_connect_oracle.c)
    target_include_directories(ihs-network-test-connect-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-network-test-connect-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-network-test-connect-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-network-test-connect-oracle PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_BaseSend -Wl,--wrap=IHS_SessionUnreliableGet)
    add_executable(ihs-network-test-discovery-oracle network_test_discovery_oracle.c)
    target_include_directories(ihs-network-test-discovery-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-network-test-discovery-oracle PRIVATE ihslib)
    target_compile_options(ihs-network-test-discovery-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-network-test-discovery-oracle PRIVATE -Wl,--wrap=IHS_CryptoRandomUInt32
        -Wl,--wrap=clock_gettime -Wl,--wrap=time)
    ihs_add_test(network_test_lifetime test_network_test_lifetime.c)
    target_link_libraries(ihstest_network_test_lifetime PRIVATE ihs-test-session)
    target_link_options(ihstest_network_test_lifetime PRIVATE -Wl,--wrap=realloc
        -Wl,--wrap=IHS_SessionChannelQueueFrameResult)
    set_tests_properties(network_test_lifetime PROPERTIES TIMEOUT 20)
    ihs_add_test(network_test_udp test_network_test_udp.c)
    target_link_libraries(ihstest_network_test_udp PRIVATE ihs-test-session)
    set_tests_properties(network_test_udp PROPERTIES TIMEOUT 25)
    add_executable(ihs-network-test-oracle network_test_oracle.c)
    target_include_directories(ihs-network-test-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-network-test-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-network-test-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-network-test-oracle PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_BaseSend -Wl,--wrap=IHS_SessionFrameEncrypt -Wl,--wrap=IHS_SessionFrameDecrypt
        -Wl,--wrap=IHS_SessionChannelQueueFrameResult)
    ihs_add_test(media_network_stats_lifetime test_media_network_stats_lifetime.c)
    target_link_libraries(ihstest_media_network_stats_lifetime PRIVATE ihs-test-session)
    target_link_options(ihstest_media_network_stats_lifetime PRIVATE -Wl,--wrap=realloc
        -Wl,--wrap=IHS_HIDManagerCloseAll -Wl,--wrap=IHS_MediaNetworkStatsStop)
    set_tests_properties(media_network_stats_lifetime PROPERTIES TIMEOUT 20)
    add_executable(ihs-media-network-stats-oracle media_network_stats_oracle.c)
    target_include_directories(ihs-media-network-stats-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-media-network-stats-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-media-network-stats-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    add_executable(ihs-video-subframe-oracle video_subframe_oracle.c)
    target_include_directories(ihs-video-subframe-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-video-subframe-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-video-subframe-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-video-subframe-oracle PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_SessionSendControlMessage -Wl,--wrap=IHS_SessionChannelDataLost)
    add_executable(ihs-video-nal-oracle video_nal_oracle.c)
    target_include_directories(ihs-video-nal-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-video-nal-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-video-nal-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-video-nal-oracle PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_SessionSendControlMessage -Wl,--wrap=IHS_SessionChannelDataLost)
    add_executable(ihs-video-crypto-oracle video_crypto_oracle.c)
    target_include_directories(ihs-video-crypto-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-video-crypto-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-video-crypto-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-video-crypto-oracle PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_SessionSendControlMessage -Wl,--wrap=IHS_SessionChannelDataLost)
    add_executable(ihs-video-content-mode-oracle video_content_mode_oracle.c)
    target_include_directories(ihs-video-content-mode-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-video-content-mode-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-video-content-mode-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-video-content-mode-oracle PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_SessionSendControlMessage -Wl,--wrap=IHS_SessionChannelDataLost)
    add_test(NAME video_content_mode_capacity COMMAND ihs-video-content-mode-oracle --capacity)
    set_tests_properties(video_content_mode_capacity PROPERTIES TIMEOUT 10)
    add_executable(ihs-video-keyframe-oracle video_keyframe_oracle.c)
    target_include_directories(ihs-video-keyframe-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-video-keyframe-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-video-keyframe-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-video-keyframe-oracle PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_SessionSendControlMessage -Wl,--wrap=IHS_SessionChannelDataLost)
    add_executable(ihs-video-stop-phases-oracle video_stop_phases_oracle.c)
    target_include_directories(ihs-video-stop-phases-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-video-stop-phases-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-video-stop-phases-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-video-stop-phases-oracle PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_SessionSendControlMessage -Wl,--wrap=IHS_SessionChannelDataLost)
    add_executable(ihs-video-start-oracle video_start_oracle.c)
    target_include_directories(ihs-video-start-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-video-start-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-video-start-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-video-start-oracle PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_ThreadCreate -Wl,--wrap=IHS_SessionSendControlMessage)
    add_executable(ihs-client-shutdown-oracle client_shutdown_oracle.c)
    target_include_directories(ihs-client-shutdown-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-client-shutdown-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-client-shutdown-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-client-shutdown-oracle PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_BaseSend -Wl,--wrap=IHS_UDPSocketWaitReadable -Wl,--wrap=IHS_UDPSocketReceive
        -Wl,--wrap=IHS_SessionNextConnectionId -Wl,--wrap=IHS_SessionClientTransportClosed
        -Wl,--wrap=IHS_SessionChannelControlUpdateStage -Wl,--wrap=IHS_HIDManagerCloseAll
        -Wl,--wrap=IHS_SessionChannelControlOnMessageReceived)
    add_executable(ihs-control-handoff-oracle control_handoff_oracle.c)
    target_include_directories(ihs-control-handoff-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-control-handoff-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-control-handoff-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-control-handoff-oracle PRIVATE -Wl,--wrap=IHS_SessionChannelControlOnMessageReceived)
    ihs_add_test(microphone_lifetime test_microphone_lifetime.c)
    target_link_libraries(ihstest_microphone_lifetime PRIVATE ihs-test-session)
    set_tests_properties(microphone_lifetime PROPERTIES TIMEOUT 10)
    ihs_add_test(frame_report_lifetime test_frame_report_lifetime.c)
    target_link_libraries(ihstest_frame_report_lifetime PRIVATE ihs-test-session)
    target_link_options(ihstest_frame_report_lifetime PRIVATE -Wl,--wrap=IHS_SessionChannelQueueFrameResult)
    set_tests_properties(frame_report_lifetime PROPERTIES TIMEOUT 20)
    add_executable(ihs-media-input-oracle media_input_oracle.c)
    target_include_directories(ihs-media-input-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-media-input-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-media-input-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-media-input-oracle PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_SessionChannelDataReceiveReliableFrame
        -Wl,--wrap=IHS_SessionRecordNetworkVideo -Wl,--wrap=IHS_SessionChannelQueueFrameResult)
    add_executable(ihs-frame-stats-oracle frame_stats_oracle.c)
    target_include_directories(ihs-frame-stats-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-frame-stats-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-frame-stats-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-frame-stats-oracle PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_SessionChannelQueueFrameResult)
    ihs_add_test(reliable_feedback_worker test_reliable_feedback_worker.c)
    target_link_libraries(ihstest_reliable_feedback_worker PRIVATE ihs-test-session)
    target_link_options(ihstest_reliable_feedback_worker PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_UDPSocketReceive -Wl,--wrap=IHS_UDPSocketWaitReadable
        -Wl,--wrap=IHS_SessionPacketsWindowPoll)
    set_tests_properties(reliable_feedback_worker PROPERTIES TIMEOUT 20)
    ihs_add_test(negotiation_stage_worker test_negotiation_stage_worker.c)
    target_link_libraries(ihstest_negotiation_stage_worker PRIVATE ihs-test-session)
    target_link_options(ihstest_negotiation_stage_worker PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_UDPSocketReceive -Wl,--wrap=IHS_UDPSocketWaitReadable -Wl,--wrap=IHS_SessionPacketTimestamp)
    set_tests_properties(negotiation_stage_worker PROPERTIES TIMEOUT 20)
    add_executable(ihs-frame-events-loop-oracle frame_events_loop_oracle.c)
    add_executable(ihs-network-collectors-oracle network_collectors_oracle.c)
    add_executable(ihs-early-media-worker early_media_worker.c)
    target_include_directories(ihs-early-media-worker PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-early-media-worker PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-early-media-worker PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-early-media-worker PRIVATE -Wl,--wrap=IHS_BaseSend -Wl,--wrap=IHS_SessionPacketTimestamp)
    add_executable(ihs-network-wire-oracle network_wire_oracle.c)
    ihs_add_test(receive_cycle_worker test_receive_cycle_worker.c)
    target_link_libraries(ihstest_receive_cycle_worker PRIVATE ihs-test-session)
    target_link_options(ihstest_receive_cycle_worker PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_UDPSocketWaitReadable -Wl,--wrap=IHS_UDPSocketReceive
        -Wl,--wrap=IHS_SessionPacketParse -Wl,--wrap=IHS_BaseSend -Wl,--wrap=IHS_SessionDeliverReliableFrame)
    target_link_options(ihstest_receive_cycle_worker PRIVATE -Wl,--wrap=IHS_SessionPacketsWindowPoll)
    target_link_options(ihstest_receive_cycle_worker PRIVATE -Wl,--wrap=IHS_SessionChannelControlOnMessageReceived)
    set_tests_properties(receive_cycle_worker PROPERTIES TIMEOUT 20)
    ihs_add_test(video_network_workers test_video_network_workers.c)
    target_link_libraries(ihstest_video_network_workers PRIVATE ihs-test-session)
    target_link_options(ihstest_video_network_workers PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_SessionSendControlMessage)
    set_tests_properties(video_network_workers PROPERTIES TIMEOUT 20)
    ihs_add_test(network_sampling_workers test_network_sampling_workers.c)
    target_link_libraries(ihstest_network_sampling_workers PRIVATE ihs-test-session)
    target_link_options(ihstest_network_sampling_workers PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp)
    set_tests_properties(network_sampling_workers PROPERTIES TIMEOUT 20)
    target_include_directories(ihs-network-wire-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-network-wire-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-network-wire-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-network-wire-oracle PRIVATE -Wl,--wrap=IHS_SessionChannelDataReceiveReliableFrame -Wl,--wrap=IHS_SessionClientTransportClosed
        -Wl,--wrap=IHS_SessionNextConnectionId
        -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_BaseSend -Wl,--wrap=IHS_SessionPacketParse
        -Wl,--wrap=IHS_SessionDeliverReliableFrame
        -Wl,--wrap=IHS_SessionChannelDataInit
        -Wl,--wrap=IHS_UDPSocketReceive -Wl,--wrap=IHS_UDPSocketWaitReadable
        -Wl,--wrap=IHS_SessionChannelQueueFrameResult)
    target_include_directories(ihs-network-collectors-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-network-collectors-oracle PRIVATE ihslib)
    target_compile_options(ihs-network-collectors-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    ihs_add_test(network_collectors_lifetime test_network_collectors_lifetime.c)
    target_link_libraries(ihstest_network_collectors_lifetime PRIVATE ihslib)
    target_link_options(ihstest_network_collectors_lifetime PRIVATE -Wl,--wrap=malloc)
    set_tests_properties(network_collectors_lifetime PROPERTIES TIMEOUT 20)
    target_include_directories(ihs-frame-events-loop-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-frame-events-loop-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-frame-events-loop-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-frame-events-loop-oracle PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_SessionSendFrameEvents -Wl,--wrap=IHS_SessionChannelQueueFrameResult)
    add_executable(ihs-negotiation-stage-oracle negotiation_stage_oracle.c)
    target_include_directories(ihs-negotiation-stage-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-negotiation-stage-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-negotiation-stage-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-negotiation-stage-oracle PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_SessionChannelQueueFrameResult)
    add_executable(ihs-reliable-large-frame-oracle reliable_large_frame_oracle.c)
    target_include_directories(ihs-reliable-large-frame-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-reliable-large-frame-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-reliable-large-frame-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-reliable-large-frame-oracle PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_SessionDeliverReliableFrame)
    add_executable(ihs-negotiation-oracle negotiation_oracle.c)
    target_include_directories(ihs-negotiation-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-negotiation-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-negotiation-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-negotiation-oracle PRIVATE -Wl,--wrap=IHS_BaseSend -Wl,--wrap=IHS_SessionPacketTimestamp)
    add_executable(ihs-reliable-channels-oracle reliable_channels_oracle.c)
    target_include_directories(ihs-reliable-channels-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-reliable-channels-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-reliable-channels-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-reliable-channels-oracle PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_SessionDeliverReliableFrame
        -Wl,--wrap=IHS_SessionChannelDataReceiveReliableFrame)
    add_executable(ihs-reliable-feedback-oracle reliable_feedback_oracle.c)
    target_include_directories(ihs-reliable-feedback-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-reliable-feedback-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-reliable-feedback-oracle PRIVATE -UNDEBUG)
    target_link_options(ihs-reliable-feedback-oracle PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_SessionPacketsWindowPoll)
    add_executable(ihs-initial-send-oracle initial_send_oracle.c)
    target_include_directories(ihs-initial-send-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-initial-send-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-initial-send-oracle PRIVATE -UNDEBUG)
    target_link_options(ihs-initial-send-oracle PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp)
    ihs_add_test(initial_send test_initial_send.c)
    target_link_libraries(ihstest_initial_send PRIVATE ihs-test-session)
    target_link_options(ihstest_initial_send PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_RetransmissionTrack
        -Wl,--wrap=IHS_QueueCreate -Wl,--wrap=IHS_QueueItemObtain)
    ihs_add_test(initial_send_worker test_initial_send_worker.c)
    target_link_libraries(ihstest_initial_send_worker PRIVATE ihs-test-session)
    target_link_options(ihstest_initial_send_worker PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_UDPSocketReceive -Wl,--wrap=IHS_UDPSocketWaitReadable)
    set_tests_properties(initial_send initial_send_worker PROPERTIES TIMEOUT 20)
    ihs_add_test(video_reset_worker test_video_reset_worker.c)
    target_link_libraries(ihstest_video_reset_worker PRIVATE ihs-test-session)
    target_link_options(ihstest_video_reset_worker PRIVATE -Wl,--wrap=IHS_ThreadCreate
        -Wl,--wrap=IHS_ThreadJoin -Wl,--wrap=IHS_SessionPacketsWindowReleaseFrame
        -Wl,--wrap=IHS_SessionChannelControlOnMessageReceived
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_SessionSendControlMessage
        -Wl,--wrap=IHS_SessionChannelDataLost -Wl,--wrap=IHS_BaseSend -Wl,--wrap=IHS_UDPSocketWaitReadable)
    set_tests_properties(video_reset_worker PROPERTIES TIMEOUT 20)
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
        -Wl,--wrap=IHS_TimerTaskStart)
    set_tests_properties(video_start_lifetime PROPERTIES TIMEOUT 20)
endif()

ihs_add_test(control_keyboard test_control_keyboard.c)
target_link_libraries(ihstest_control_keyboard PRIVATE ihs-test-session)

ihs_add_test(frame_stats test_frame_stats.c)
ihs_add_test(frame_stats_tracked test_frame_stats_tracked.c)
set_tests_properties(frame_stats_tracked PROPERTIES TIMEOUT 20)
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
target_link_options(ihstest_launch_transactions PRIVATE -Wl,--wrap=clock_gettime -Wl,--wrap=time)

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
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(ihstest_media_recovery PRIVATE -Wl,--wrap=IHS_BaseSend)
endif()

ihs_add_test(fec_channel test_fec_channel.c)
target_link_libraries(ihstest_fec_channel PRIVATE ihs-test-session)
set_tests_properties(fec_channel PROPERTIES TIMEOUT 10)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    ihs_add_test(send_admission test_send_admission.c)
    target_link_libraries(ihstest_send_admission PRIVATE ihs-test-session)
    target_link_options(ihstest_send_admission PRIVATE -Wl,--wrap=IHS_SessionChannelQueueFrameResult)
    ihs_add_test(send_admission_worker test_send_admission_worker.c)
    target_link_libraries(ihstest_send_admission_worker PRIVATE ihs-test-session)
    target_link_options(ihstest_send_admission_worker PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_UDPSocketReceive -Wl,--wrap=IHS_UDPSocketWaitReadable)
    set_tests_properties(send_admission send_admission_worker PROPERTIES TIMEOUT 20)
    add_executable(ihs-send-admission-oracle send_admission_oracle.c)
    target_include_directories(ihs-send-admission-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-send-admission-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-send-admission-oracle PRIVATE -UNDEBUG)
    target_link_options(ihs-send-admission-oracle PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_RetransmissionTrack
        -Wl,--wrap=IHS_SessionFrameEncrypt)
    add_executable(ihs-control-wire-oracle control_wire_oracle.c)
    target_include_directories(ihs-control-wire-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-control-wire-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-control-wire-oracle PRIVATE -UNDEBUG)
    target_link_options(ihs-control-wire-oracle PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp)
    ihs_add_test(connection_id test_connection_id.c)
    set_tests_properties(connection_id PROPERTIES TIMEOUT 10)
    add_executable(ihs-disconnect-oracle disconnect_oracle.c)
    target_include_directories(ihs-disconnect-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-disconnect-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-disconnect-oracle PRIVATE -UNDEBUG)
    target_link_options(ihs-disconnect-oracle PRIVATE -Wl,--wrap=IHS_SessionClientTransportClosed
        -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_SessionNextConnectionId
        -Wl,--wrap=IHS_CryptoRandomUInt32)
    add_executable(ihs-connect-oracle connect_oracle.c)
    target_include_directories(ihs-connect-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-connect-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-connect-oracle PRIVATE -UNDEBUG)
    target_link_options(ihs-connect-oracle PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_SessionDisconnect
        -Wl,--wrap=IHS_SessionTransportDisconnect)
    ihs_add_test(connect_worker test_connect_worker.c)
    target_link_libraries(ihstest_connect_worker PRIVATE ihs-test-session)
    target_link_options(ihstest_connect_worker PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_UDPSocketReceive -Wl,--wrap=IHS_UDPSocketWaitReadable)
    set_tests_properties(connect_worker PROPERTIES TIMEOUT 10)
    ihs_add_test(disconnect_worker test_disconnect_worker.c)
    target_link_libraries(ihstest_disconnect_worker PRIVATE ihs-test-session)
    target_link_options(ihstest_disconnect_worker PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_UDPSocketReceive -Wl,--wrap=IHS_UDPSocketWaitReadable)
    set_tests_properties(disconnect_worker PROPERTIES TIMEOUT 20)
    ihs_add_test(transport_worker test_transport_worker.c)
    target_link_libraries(ihstest_transport_worker PRIVATE ihs-test-session)
    target_link_options(ihstest_transport_worker PRIVATE
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_UDPSocketReceive -Wl,--wrap=IHS_UDPSocketWaitReadable)
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
        -Wl,--wrap=IHS_SessionUnreliableReceived
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_SessionDisconnect
        -Wl,--wrap=IHS_SessionTransportDisconnect
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
    ihs_add_test(client_shutdown_worker test_client_shutdown_worker.c)
    target_link_libraries(ihstest_client_shutdown_worker PRIVATE ihs-test-session)
    target_link_options(ihstest_client_shutdown_worker PRIVATE -Wl,--wrap=IHS_SessionPacketTimestamp
        -Wl,--wrap=IHS_BaseSend -Wl,--wrap=IHS_UDPSocketWaitReadable -Wl,--wrap=IHS_UDPSocketReceive
        -Wl,--wrap=IHS_SessionReliableUpdateChannelAt)
    set_tests_properties(client_shutdown_worker PROPERTIES TIMEOUT 30)
    ihs_add_test(network_cycle_worker test_network_cycle_worker.c)
    target_link_libraries(ihstest_network_cycle_worker PRIVATE ihs-test-session)
    target_link_options(ihstest_network_cycle_worker PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_UDPSocketReceive
        -Wl,--wrap=IHS_UDPSocketWaitReadable)
    set_tests_properties(network_cycle_worker PROPERTIES TIMEOUT 20)
    ihs_add_test(outgoing_worker test_outgoing_worker.c)
    target_link_libraries(ihstest_outgoing_worker PRIVATE ihs-test-session)
    target_link_options(ihstest_outgoing_worker PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_UDPSocketReceive -Wl,--wrap=IHS_UDPSocketWaitReadable -Wl,--wrap=IHS_SessionPacketTimestamp)
    set_tests_properties(outgoing outgoing_worker PROPERTIES TIMEOUT 20)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(ihs-audio-start-oracle audio_start_oracle.c)
    target_include_directories(ihs-audio-start-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-audio-start-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-audio-start-oracle PRIVATE -UNDEBUG -Wall -Wextra -Werror=format)
    target_link_options(ihs-audio-start-oracle PRIVATE
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_ThreadCreate -Wl,--wrap=IHS_ThreadJoin
        -Wl,--wrap=IHS_SessionChannelDataAudioCreate -Wl,--wrap=IHS_SessionUnreliableSetDelay
        -Wl,--wrap=IHS_SessionDrainPendingData -Wl,--wrap=IHS_SessionChannelDataReceiveReliableFrame)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(ihs-microphone-schedule-oracle microphone_schedule_oracle.c)
    target_include_directories(ihs-microphone-schedule-oracle PRIVATE ${TEST_INCLUDES})
    target_link_libraries(ihs-microphone-schedule-oracle PRIVATE ihs-test-session ihslib)
    target_compile_options(ihs-microphone-schedule-oracle PRIVATE -UNDEBUG)
    target_link_options(ihs-microphone-schedule-oracle PRIVATE -Wl,--wrap=IHS_BaseSend
        -Wl,--wrap=IHS_SessionPacketTimestamp -Wl,--wrap=IHS_SessionFrameEncrypt)
endif()
