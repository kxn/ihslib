#include "stream.h"

static int Start(IHS_Session *session, const IHS_StreamVideoConfig *config, void *context) {
    return 0;
}


static void Stop(IHS_Session *session, void *context) {
}

static IHS_StreamVideoSubmitResult Submit(IHS_Session *session, uint16_t frameId, IHS_Buffer *data,
                                          IHS_StreamVideoFrameFlag flags, void *context) {

    return IHS_StreamVideoSubmitOK;
}


const IHS_StreamVideoCallbacks VideoCallbacks = {
        .start = Start,
        .submit = Submit,
        .stop = Stop,
};

void VideoInit(int argc, char *argv[]) {
}

void VideoDeinit() {
}