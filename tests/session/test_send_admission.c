/* Admission rejection must preserve IDs/buffer ownership and live transport.
 * Local bound/resource/crypto guards have separate, explicit semantics. */
#include "ihs_buffer.h"
#include "ihs_buffer_ext.h"
#include "session/channels/ch_control.h"
#include "session/session_pri.h"
#include "test_session.h"
#include <assert.h>
static bool rejectFrame;
IHS_SendAdmission __real_IHS_SessionChannelQueueFrameResult(IHS_SessionChannel *,
                                                            IHS_SessionFrame *, bool);
IHS_SendAdmission __wrap_IHS_SessionChannelQueueFrameResult(IHS_SessionChannel *c,
                                                            IHS_SessionFrame *f, bool retry) {
    return rejectFrame ? IHS_SendAdmissionTooLarge
                       : __real_IHS_SessionChannelQueueFrameResult(c, f, retry);
}
static void empty_queue(IHS_Session *s) {
    typedef struct {
        IHS_SessionPacket packet;
        bool reliable;
    } Queued;
    Queued *q;
    while ((q = (void *)IHS_QueuePoll(s->sendQueue))) {
        IHS_SessionPacketClear(&q->packet, true);
        IHS_QueueItemFree((void *)q);
    }
}
int main(void) {
    IHS_Init();
    for (unsigned state = 0; state < 4; ++state) {
        IHS_Session *s = IHS_TestSessionCreate();
        IHS_TimerTaskStopImmediate(s->retransmission.timer);
        IHS_SessionChannelControl *c = (void *)s->channels[1];
        s->state.connectionState =
            state ? IHS_SessionConnectionStateHandshaking : IHS_SessionConnectionStateConnecting;
        atomic_store(&s->transport.shutdown, state > 1 ? state - 1 : 0);
        IHS_SessionPacket packet;
        IHS_SessionChannelInitializePacket(&c->base, &packet, IHS_SessionPacketTypeUnreliable,
                                           false, IHS_PACKET_ID_NEXT);
        IHS_BufferAppendUInt8(&packet.body, 0xaa);
        IHS_SendAdmission result = IHS_SessionQueuePacketResult(s, &packet, false);
        assert(result == (state == 1 ? IHS_SendAdmissionAccepted : IHS_SendAdmissionNotConnected));
        assert(s->outgoing.unreliableId[1] == (state == 1 ? 1 : 0));
        if (state != 1)
            assert(packet.body.size == 1 && *IHS_BufferPointer(&packet.body) == 0xaa);
        IHS_SessionPacketClear(&packet, true);
        empty_queue(s);
        /* Unconnected discovery is a separate connection-independent path. */
        IHS_SessionChannelInitializePacket(&c->base, &packet, IHS_SessionPacketTypeUnconnected,
                                           false, 0);
        assert(IHS_SessionQueuePacket(s, &packet, false));
        IHS_SessionPacketClear(&packet, true);
        empty_queue(s);
        CKeepAliveMsg message = CKEEP_ALIVE_MSG__INIT;
        c->sendEncryptSequence = 7;
        if (state != 1) {
            assert(!IHS_SessionSendControlMessage(s, k_EStreamControlKeepAlive, &message.base));
            assert(c->sendEncryptSequence == 8 && !s->base.interrupted);
            assert(s->outgoing.reliableId[1] == 0);
        } else {
            rejectFrame = true;
            assert(!IHS_SessionSendControlMessage(s, k_EStreamControlKeepAlive, &message.base));
            rejectFrame = false;
            assert(c->sendEncryptSequence == 8 &&
                   IHS_SessionTransportState(s) == IHS_TransportConnected);
            assert(!s->base.interrupted && s->outgoing.reliableId[1] == 0);
            IHS_SessionFrame frame;
            IHS_SessionChannelInitializeFrame(&c->base, &frame, IHS_SessionPacketTypeReliable,
                                              false, IHS_PACKET_ID_NEXT);
            s->state.mtu = 512;
            frame.body.maxCapacity = 0;
            IHS_BufferFillMem(&frame.body, 0, 0x5a, (512u - IHS_PACKET_HEADER_SIZE) * 32768u);
            assert(__real_IHS_SessionChannelQueueFrameResult(&c->base, &frame, true) ==
                   IHS_SendAdmissionTooLarge);
            assert(s->outgoing.reliableId[1] == 0);
            IHS_SessionFrameClear(&frame, true);
        }
        assert(!IHS_QueuePoll(s->sendQueue));
        IHS_SessionDestroy(s);
    }
    IHS_Quit();
    return 0;
}
