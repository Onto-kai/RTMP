#pragma once

extern "C"{
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/avutil.h>
    #include <libavutil/log.h>
}

int Vdecode(AVCodecContext *ctx, AVFrame *frame, AVPacket *pkt);
