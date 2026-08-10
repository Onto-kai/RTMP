extern "C"{
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
}

//#include "packet_queue.hpp"

#include <iostream>
#include <string>
#include <functional>

std::string Error_code_to_string(int ret){
    std::string buf(AV_ERROR_MAX_STRING_SIZE,0);
    av_strerror(ret,buf.data(),buf.size());
    buf.resize(strlen(buf.data()));
    return buf;
}


class Transcoder{
public:

    static AVCodec *find_hw_decoder(enum AVCodecID codec_id){
        switch(codec_id){

            case AV_CODEC_ID_H264:
                return avcodec_find_decoder_by_name("h264_cuvid");

            case AV_CODEC_ID_HEVC:
                return avcodec_find_decoder_by_name("hevc_cuvid");

            case AV_CODEC_ID_AV1:
                return avcodec_find_decoder_by_name("av1_cuvid");

            case AV_CODEC_ID_VP9:
                return avcodec_find_decoder_by_name("vp9_cuvid");

            case AV_CODEC_ID_VP8:
                return avcodec_find_decoder_by_name("vp8_cuvid");

            case AV_CODEC_ID_MPEG2VIDEO:
                return avcodec_find_decoder_by_name("mpeg2_cuvid");

            case AV_CODEC_ID_MPEG4:
                return avcodec_find_decoder_by_name("mpeg4_cuvid");

            case AV_CODEC_ID_MJPEG:
                return avcodec_find_decoder_by_name("mjpeg_cuvid");

            case AV_CODEC_ID_VC1:
                return avcodec_find_decoder_by_name("vc1_cuvid");

            default:
                std::cerr << "not supper hardware decode" << std::endl;
                return avcodec_find_decoder(codec_id);
        }
    }

    static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts){
        const enum AVPixelFormat *p = nullptr;
        for(p = pix_fmts; *p != AV_PIX_FMT_NONE; p++){
            if(*p == AV_PIX_FMT_CUDA){
                return *p;
            }
        }
        return AV_PIX_FMT_NONE;
    }

    int video_transcoder_init(AVFormatContext *fmtCtx, int Idx){
        VS_stream = fmtCtx->streams[Idx];

        if(av_hwdevice_ctx_create(&hw_device_ctx,AV_HWDEVICE_TYPE_CUDA,nullptr,nullptr,0) < 0){
            av_buffer_unref(&hw_device_ctx);
            return -1;
        }

        AVCodec *vdeCoder = find_hw_decoder(VS_stream->codecpar->codec_id);
        if(!vdeCoder){
            std::cerr << "Decoder not found " << std::endl;
            av_buffer_unref(&hw_device_ctx);
            return -1;
        }

        AVCodec *venCoder = avcodec_find_encoder_by_name("h264_nvenc");
        if(!venCoder){
            std::cerr << "Encoder not found " << std::endl;
            av_buffer_unref(&hw_device_ctx);
            return -1;
        }

        vdeCoder_Ctx = avcodec_alloc_context3(vdeCoder);
        avcodec_parameters_to_context(vdeCoder_Ctx,fmtCtx->streams[Idx]->codecpar);
    

        venCoder_Ctx = avcodec_alloc_context3(venCoder);
        venCoder_Ctx->width = VS_stream->codecpar->width;
        venCoder_Ctx->height = VS_stream->codecpar->height;
        venCoder_Ctx->bit_rate = VS_stream->codecpar->bit_rate;

        venCoder_Ctx->time_base = VS_stream->time_base;
        venCoder_Ctx->framerate = VS_stream->avg_frame_rate;

        venCoder_Ctx->gop_size = av_q2d(VS_stream->avg_frame_rate) * 2;

        venCoder_Ctx->max_b_frames = 0;
        venCoder_Ctx->pix_fmt = AV_PIX_FMT_CUDA;


        vdeCoder_Ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
        venCoder_Ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

        vdeCoder_Ctx->get_format = get_hw_format;

        if(avcodec_open2(vdeCoder_Ctx,vdeCoder,nullptr) < 0){
            std::cerr << "Decoder bind Decoder Context failed " << std::endl;
            av_buffer_unref(&hw_device_ctx);
            avcodec_free_context(&vdeCoder_Ctx);
            return -1;
        }

        if(avcodec_open2(venCoder_Ctx,venCoder,nullptr) < 0){
            std::cerr << "Encoder bind Encoder Context failed " << std::endl;
            av_buffer_unref(&hw_device_ctx);
            avcodec_free_context(&vdeCoder_Ctx);
            avcodec_free_context(&venCoder_Ctx);
            return -1;
        }

        return 0;

    }

    int video_transcoder(AVPacket *pkt,std::function<void(AVPacket *)> callback){

        AVPacket *src_pkt = av_packet_alloc();
        AVFrame *src_frame = av_frame_alloc();

        if(!src_pkt){
            std::cerr << "src packet allocate failed" << std::endl;
            av_buffer_unref(&hw_device_ctx);
            avcodec_free_context(&vdeCoder_Ctx);
            avcodec_free_context(&venCoder_Ctx);
            return -1;
        }

        if(!src_frame){
            std::cerr << "src frame allocate failed" << std::endl;
            av_buffer_unref(&hw_device_ctx);
            avcodec_free_context(&vdeCoder_Ctx);
            avcodec_free_context(&venCoder_Ctx);
            return -1;
        }

        if(avcodec_send_packet(vdeCoder_Ctx,pkt) < 0){
            std::cerr << "send packet to decoder failed" << std::endl;
            av_buffer_unref(&hw_device_ctx);
            avcodec_free_context(&vdeCoder_Ctx);
            avcodec_free_context(&venCoder_Ctx);
            return -1;
        }

        while(true){
            int ret = avcodec_receive_frame(vdeCoder_Ctx,src_frame);
            if(ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
            if(ret < 0){
                std::cerr << "[avcodec_recevice_frame] : " << Error_code_to_string(ret).data() << std::endl;
                break;
            }

            if(avcodec_send_frame(venCoder_Ctx,src_frame) < 0){
                std::cerr << "send frame to encoder failed" << std::endl;
                av_buffer_unref(&hw_device_ctx);
                avcodec_free_context(&vdeCoder_Ctx);
                avcodec_free_context(&venCoder_Ctx);
                return -1;
            }

            while(true){
                int ret = avcodec_receive_packet(venCoder_Ctx,src_pkt);
                if(ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
                if(ret < 0){
                    std::cerr << "[avcodec_recevice_packet] : " << Error_code_to_string(ret).data() << std::endl;
                    break;
                }

                callback(src_pkt);

                av_packet_unref(src_pkt);
            }
        }
        if(src_pkt){
            av_packet_free(&src_pkt);
            src_pkt = nullptr;
        }

        if(src_frame){
            av_frame_free(&src_frame);
            src_frame = nullptr;
        }
        return 0;
    }


    void video_transcoder_free(){
         if(hw_device_ctx){
            av_buffer_unref(&hw_device_ctx);
        }

        if(vdeCoder_Ctx){
            avcodec_free_context(&vdeCoder_Ctx);
            vdeCoder_Ctx = nullptr;
        }

        if(venCoder_Ctx){
            avcodec_free_context(&venCoder_Ctx);
            venCoder_Ctx = nullptr;
        }
    }

    int audio_transcoder_init(AVFormatContext *fmtCtx, int Idx){
        AS_stream = fmtCtx->streams[Idx];

        AVCodec *adeCoder = avcodec_find_decoder(AS_stream->codecpar->codec_id);
        if(!adeCoder){
            std::cerr << "Decoder not found " << std::endl;
            return -1;
        }

        AVCodec *aenCoder = avcodec_find_encoder_by_name("aac");
        if(!aenCoder){
            std::cerr << "Encoder not found " << std::endl;
            return -1;
        }

        adeCoder_Ctx = avcodec_alloc_context3(adeCoder);
        avcodec_parameters_to_context(adeCoder_Ctx,fmtCtx->streams[Idx]->codecpar);
    

        venCoder_Ctx = avcodec_alloc_context3(aenCoder);


        if(avcodec_open2(adeCoder_Ctx,adeCoder,nullptr) < 0){
            std::cerr << "Decoder bind Decoder Context failed " << std::endl;
            avcodec_free_context(&adeCoder_Ctx);
            return -1;
        }


        aenCoder_Ctx->sample_rate = AS_stream->codecpar->sample_rate;
        aenCoder_Ctx->channel_layout = AS_stream->codecpar->channel_layout;
        aenCoder_Ctx->channels = AS_stream->codecpar->channels;
        aenCoder_Ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        aenCoder_Ctx->time_base = AS_stream->time_base;
        aenCoder_Ctx->bit_rate = AS_stream->codecpar->bit_rate;

        if(avcodec_open2(aenCoder_Ctx,aenCoder,nullptr) < 0){
            std::cerr << "Encoder bind Encoder Context failed " << std::endl;
            avcodec_free_context(&adeCoder_Ctx);
            avcodec_free_context(&aenCoder_Ctx);
            return -1;
        }

        return 0;
    }

    int audio_transcoder(AVPacket *pkt, std::function<void(AVPacket *)>callback){

        AVPacket *src_pkt = av_packet_alloc();
        AVFrame *src_frame = av_frame_alloc();

        if(!src_pkt){
            std::cerr << "src packet allocate failed" << std::endl;
            avcodec_free_context(&adeCoder_Ctx);
            avcodec_free_context(&aenCoder_Ctx);
            return -1;
        }

        if(!src_frame){
            std::cerr << "src frame allocate failed" << std::endl;
            avcodec_free_context(&adeCoder_Ctx);
            avcodec_free_context(&aenCoder_Ctx);
            return -1;
        }

        if(avcodec_send_packet(adeCoder_Ctx,pkt) < 0){
            std::cerr << "send packet to decoder failed" << std::endl;
            avcodec_free_context(&adeCoder_Ctx);
            avcodec_free_context(&aenCoder_Ctx);
            return -1;
        }

        while(true){
            int ret = avcodec_receive_frame(adeCoder_Ctx,src_frame);
            if(ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
            if(ret < 0){
                std::cerr << "[audio avcodec_recevice_frame] : " << Error_code_to_string(ret).data() << std::endl;
                break;
            }

            if(avcodec_send_frame(aenCoder_Ctx,src_frame) < 0){
                std::cerr << "send frame to encoder failed" << std::endl;
                avcodec_free_context(&adeCoder_Ctx);
                avcodec_free_context(&aenCoder_Ctx);
                return -1;
            }

            while(true){
                int ret = avcodec_receive_packet(aenCoder_Ctx,src_pkt);
                if(ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
                if(ret < 0){
                    std::cerr << "[audio avcodec_recevice_packet] : " << Error_code_to_string(ret).data() << std::endl;
                    break;
                }
                callback(src_pkt);
                av_packet_unref(src_pkt);
            }
        }

       if(src_pkt){
            av_packet_free(&src_pkt);
            src_pkt = nullptr;
        }

        if(src_frame){
            av_frame_free(&src_frame);
            src_frame = nullptr;
        }
        return 0;
    }

    void audio_transcoder_free(){
        if(adeCoder_Ctx){
            avcodec_free_context(&adeCoder_Ctx);
            adeCoder_Ctx = nullptr;
        }

        if(aenCoder_Ctx){
            avcodec_free_context(&aenCoder_Ctx);
            aenCoder_Ctx = nullptr;
        }
    }

private:
    AVBufferRef *hw_device_ctx = nullptr;

    AVCodecContext *vdeCoder_Ctx = nullptr;
    AVCodecContext *venCoder_Ctx = nullptr;
    AVCodecContext *adeCoder_Ctx = nullptr;
    AVCodecContext *aenCoder_Ctx = nullptr;


    AVStream *VS_stream = nullptr;
    AVStream *AS_stream = nullptr;
};




// AVCodec* find_hw_decoder(enum AVCodecID codec_id)
// {
//     switch(codec_id)
//     {
//         case AV_CODEC_ID_H264:
//             return avcodec_find_decoder_by_name("h264_cuvid");

//         case AV_CODEC_ID_HEVC:
//             return avcodec_find_decoder_by_name("hevc_cuvid");

//         case AV_CODEC_ID_AV1:
//             return avcodec_find_decoder_by_name("av1_cuvid");

//         case AV_CODEC_ID_VP9:
//             return avcodec_find_decoder_by_name("vp9_cuvid");

//         case AV_CODEC_ID_VP8:
//             return avcodec_find_decoder_by_name("vp8_cuvid");

//         case AV_CODEC_ID_MPEG2VIDEO:
//             return avcodec_find_decoder_by_name("mpeg2_cuvid");

//         case AV_CODEC_ID_MPEG4:
//             return avcodec_find_decoder_by_name("mpeg4_cuvid");

//         case AV_CODEC_ID_MJPEG:
//             return avcodec_find_decoder_by_name("mjpeg_cuvid");

//         case AV_CODEC_ID_VC1:
//             return avcodec_find_decoder_by_name("vc1_cuvid");

//         default:
//             std::cerr << "not supper hardware decode" << std::endl;
//             return avcodec_find_decoder(codec_id);
//     }
// }



// int video_transcoder(AVFormatContext *fmtCtx,Packet_queue *vpq,AVPacket *pkt, int Idx){
//     AVBufferRef *hw_device_ctx = nullptr;

//     AVCodecContext *vdeCoder_Ctx = nullptr;
//     AVCodec *vdeCoder = nullptr;

//     AVCodecContext *venCoder_Ctx = nullptr;
//     AVCodec *venCoder = nullptr;

//     int vIdx = Idx;

//     AVStream *S_stream = fmtCtx->streams[vIdx];

//     if(av_hwdevice_ctx_create(&hw_device_ctx,AV_HWDEVICE_TYPE_CUDA,nullptr,nullptr,0) < 0){
//         av_buffer_unref(&hw_device_ctx);
//         return -1;
//     }

//     vdeCoder = find_hw_decoder(S_stream->codecpar->codec_id);
//     if(!vdeCoder){
//         std::cerr << "Decoder not found " << std::endl;
//         av_buffer_unref(&hw_device_ctx);
//         return -1;
//     }

//     venCoder = avcodec_find_encoder_by_name("h264_nvenc");
//     if(!venCoder){
//         std::cerr << "Encoder not found " << std::endl;
//         av_buffer_unref(&hw_device_ctx);
//         return -1;
//     }

//     vdeCoder_Ctx = avcodec_alloc_context3(vdeCoder);
//     avcodec_parameters_to_context(vdeCoder_Ctx,fmtCtx->streams[vIdx]->codecpar);
    

//     venCoder_Ctx = avcodec_alloc_context3(venCoder);
//     venCoder_Ctx->width = S_stream->codecpar->width;
//     venCoder_Ctx->height = S_stream->codecpar->height;
//     venCoder_Ctx->bit_rate = S_stream->codecpar->bit_rate;

//     venCoder_Ctx->time_base = S_stream->time_base;
//     venCoder_Ctx->framerate = S_stream->avg_frame_rate;

//     venCoder_Ctx->gop_size = av_q2d(S_stream->avg_frame_rate) * 2;

//     venCoder_Ctx->max_b_frames = 0;
//     venCoder_Ctx->pix_fmt = AV_PIX_FMT_CUDA;


//     vdeCoder_Ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
//     venCoder_Ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

//     if(avcodec_open2(vdeCoder_Ctx,vdeCoder,nullptr) < 0){
//         std::cerr << "Decoder bind Decoder Context failed " << std::endl;
//         av_buffer_unref(&hw_device_ctx);
//         avcodec_free_context(&vdeCoder_Ctx);
//         return -1;
//     }

//     if(avcodec_open2(venCoder_Ctx,venCoder,nullptr) < 0){
//         std::cerr << "Encoder bind Encoder Context failed " << std::endl;
//         av_buffer_unref(&hw_device_ctx);
//         avcodec_free_context(&venCoder_Ctx);
//         return -1;
//     }
//     vdeCoder_Ctx->get_format = get_hw_format;

//     AVPacket *src_pkt = av_packet_alloc();
//     AVFrame *src_frame = av_frame_alloc();

//     if(!src_pkt){
//         std::cerr << "src packet allocate failed" << std::endl;
//         av_buffer_unref(&hw_device_ctx);
//         avcodec_free_context(&vdeCoder_Ctx);
//         avcodec_free_context(&venCoder_Ctx);
//         return -1;
//     }

//     if(!src_frame){
//         std::cerr << "src frame allocate failed" << std::endl;
//         av_buffer_unref(&hw_device_ctx);
//         avcodec_free_context(&vdeCoder_Ctx);
//         avcodec_free_context(&venCoder_Ctx);
//         return -1;
//     }

//     if(avcodec_send_packet(vdeCoder_Ctx,pkt) < 0){
//         std::cerr << "send packet to decoder failed" << std::endl;
//         av_buffer_unref(&hw_device_ctx);
//         avcodec_free_context(&vdeCoder_Ctx);
//         avcodec_free_context(&venCoder_Ctx);
//         return -1;
//     }

//     while(true){
//         int ret = avcodec_receive_frame(vdeCoder_Ctx,src_frame);
//         if(ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
//         if(ret < 0){
//             std::cerr << "[avcodec_recevice_frame] : " << Error_code_to_string(ret).data() << std::endl;
//             break;
//         }

//         if(avcodec_send_frame(venCoder_Ctx,src_frame) < 0){
//             std::cerr << "send frame to encoder failed" << std::endl;
//             av_buffer_unref(&hw_device_ctx);
//             avcodec_free_context(&vdeCoder_Ctx);
//             avcodec_free_context(&venCoder_Ctx);
//             return -1;
//         }

//         while(true){
//             int ret = avcodec_receive_packet(venCoder_Ctx,src_pkt);
//             if(ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
//             if(ret < 0){
//                 std::cerr << "[avcodec_recevice_packet] : " << Error_code_to_string(ret).data() << std::endl;
//                 break;
//             }
//             vpq->pkt_push(src_pkt);
//             av_packet_unref(src_pkt);
//         }
//     }


//     if(hw_device_ctx){
//         av_buffer_unref(&hw_device_ctx);
//     }

//     if(vdeCoder_Ctx){
//         avcodec_free_context(&vdeCoder_Ctx);
//         vdeCoder_Ctx = nullptr;
//     }

//     if(venCoder_Ctx){
//         avcodec_free_context(&venCoder_Ctx);
//         venCoder_Ctx = nullptr;
//     }

//     if(src_pkt){
//         av_packet_free(&src_pkt);
//         src_pkt = nullptr;
//     }

//     if(src_frame){
//         av_frame_free(&src_frame);
//         src_frame = nullptr;
//     }
//     return 0;
// }