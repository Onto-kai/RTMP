#pragma once

extern "C"{
    #include <libavcodec/avcodec.h>
    #include <libavformat/avformat.h>
    #include <libavutil/avutil.h>
    #include <libavutil/log.h>
}

int Adecode(AVCodecContext *ctx, AVFrame *frame, AVPacket *pkt);
