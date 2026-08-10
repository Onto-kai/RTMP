#include "audio_player.h"
#include "common.h"

int Adecode(AVCodecContext *ctx, AVFrame *frame, AVPacket *pkt)
{
    if(pkt){
        int ret = avcodec_send_packet(ctx, pkt);
        if(ret < 0){
            av_log(nullptr, AV_LOG_ERROR, "Audio send packet failed %s\n", avError(ret).c_str());
            return -1;
        }
    }

    int ret = avcodec_receive_frame(ctx, frame);
    if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
        return 0;
    }
    if(ret < 0){
        av_log(nullptr, AV_LOG_ERROR, "Audio receive frame failed %s\n", avError(ret).c_str());
        return -1;
    }

    return 1;
}
