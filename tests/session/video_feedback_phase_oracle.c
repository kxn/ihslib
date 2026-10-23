/* Original-client phase vectors enter the production decoder-feedback consumer
 * and the distinct transport-level sender. Real protobuf/packet construction;
 * final queue admission is the observation boundary. No workers or sockets. */
#ifdef IHS_FEEDBACK_PHASE_SOURCE
#include IHS_FEEDBACK_PHASE_SOURCE
#else
#include "../../src/session/channels/video/ch_data_video.c"
#endif
#include <stdio.h>

static IHS_Session fixture;
static unsigned count;

bool __wrap_IHS_SessionChannelQueuePacket(IHS_SessionChannel *ch,
                                          IHS_SessionPacket *packet, bool retransmit) {
    assert(ch->session == &fixture && !retransmit);
    assert(packet->header.channelId == 3 &&
           packet->header.type == IHS_SessionPacketTypeUnreliable);
    assert(packet->body.size == 1 && *IHS_BufferPointer(&packet->body) == k_EStreamDataLost);
    ++count;
    return true;
}

int main(int argc, char **argv) {
    bool verify = argc == 2 && !strcmp(argv[1], "--verify");
    assert(argc == 1 || verify);
    IHS_SessionChannelVideo video = {0};
    video.base.base.session = &fixture;
    video.base.base.id = 3;
    for (unsigned phase = 0; phase <= 7; ++phase) {
        fixture.state.connectionState = phase;
        count = 0;
        assert(ApplyDecodeFeedback(&video, IHS_StreamVideoSubmitReportLost));
        unsigned consumer = count;
        /* A late ReportLost must not suppress a separately requested reset.
         * This catches an incorrect return from the whole feedback consumer. */
        atomic_store(&video.resetRequested, false);
        count = 0;
        assert(ApplyDecodeFeedback(&video, IHS_StreamVideoSubmitReportLost |
                                            IHS_StreamVideoSubmitReset));
        assert(atomic_load(&video.resetRequested) && count == consumer);
        count = 0;
        /* The connection's bounded-window recovery is below Client phase and
         * deliberately keeps its own existing packet construction contract. */
        IHS_SessionChannelDataLost(&video.base.base);
        if (verify) {
            /* Fixed original Linux Client phase0..7 vectors: only6 emits02.
             * The independent transport-level sender still emits in all8. */
            assert(consumer == (phase == IHS_SessionConnectionStateConnected));
            assert(count == 1);
        }
        printf("{\"phase\":%u,\"client_feedback\":%u,\"transport_feedback\":%u}\n",
               phase, consumer, count);
    }
    return 0;
}
