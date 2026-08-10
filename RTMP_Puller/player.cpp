extern "C"{
    #include <libswresample/swresample.h>
    #include <libavutil/hwcontext.h>
    #include <libavutil/pixdesc.h>
    #include <libswscale/swscale.h>
}

#include "common.h"
#include "packet_queue.h"
#include "frame_queue.h"
#include "video_player.h"
#include "audio_player.h"
#include "ring_buffer.h"

#include <SDL2/SDL.h>

#include <thread>
#include <atomic>
#include <iostream>
#include <string>

/*
    快进后退
*/
static const double TIME_INTERVAL = 5.0;
std::atomic<bool> seek_request(false);
std::atomic<bool> seek_done(false);
std::atomic<bool> seek_pending(false);
int64_t seek_pos = 0;
std::atomic<bool> seek_disjunctor(true);

/*
    音视频同步
*/
//用于平滑修正
double audio_sync_offset = 0.0;
//上一次校验时间
Uint32 last_sync_check_ticks = 0;
//同步校验间隔
const Uint32 SYNC_CHECK_INTERVAL = 100;
//最大修正幅度
const double MAX_STEP_ADJUST = 0.002;
//丢包阈值
const double BIG_DIFF_TIMESOUT = 0.06;


/*
    屏幕宽高
*/
static const double SCREEN_W = 2048.0;
static const double SCREEN_H = 1536.0;


//文件大小
double file_duration;

static const int SDL_samples = 2048;


static std::atomic<bool> g_running(true);
std::atomic<bool> quit(false);

struct PlayerState {
    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext *vCtx = nullptr;
    AVCodecContext *aCtx = nullptr;

    SwrContext *swrCtx = nullptr;
    SwsContext *swsCtx = nullptr;

    SDL_AudioDeviceID audioDev = 0;

    Packet_queue vpq;
    Packet_queue apq;
    Frame_queue vfq;

    int vIdx = -1;
    int aIdx = -1;

    AVRational vTimeBase;
    AVRational aTimeBase;

    std::atomic<double> audio_clock{0.0};

    int max_samples = 0;

    int bytes_per_sample = 0;

    std::atomic<bool> eof;

};

enum Player_Status{
    PAUSE,
    PLAY
};

std::atomic<enum Player_Status> Status(PLAY);

Ring_buffer rbuf = {};

int video_seek(PlayerState *st);

int readThread(PlayerState *st)
{
    AVPacket *pkt = av_packet_alloc();
    if(!pkt) return -1;

    while(g_running){

        if(Status == PAUSE){
            SDL_Delay(4);
            continue;
        }

        if(seek_request && seek_disjunctor){
            video_seek(st);
            seek_request = false;
            seek_done = true;
        }

        int ret = av_read_frame(st->fmtCtx, pkt);
        
        if(ret < 0) {
            if(ret == AVERROR_EOF){
                st->eof = true;
                continue;
            }
            break;
        }

        if(pkt->stream_index == st->vIdx){
            st->vpq.push(pkt);
        }else if(pkt->stream_index == st->aIdx){
            st->apq.push(pkt);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    st->vpq.push(nullptr);
    st->apq.push(nullptr);
    return 0;
}

int videoThread(PlayerState *st)
{
    AVFrame *frame = av_frame_alloc();
    if(!frame) return -1;
    double vpts = 0;

    while(g_running){

        if(Status == PAUSE){
            SDL_Delay(4);
            continue;
        }

        AVPacket *pkt = st->vpq.pop(g_running);
        if(!pkt) break;

        if(!pkt->data){ av_packet_free(&pkt); break; }

        int ret = avcodec_send_packet(st->vCtx, pkt);
        av_packet_free(&pkt);
        if(ret < 0) continue;

        while(true){
            ret = avcodec_receive_frame(st->vCtx, frame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if(ret < 0) break;

            //+
            if(frame->pts != AV_NOPTS_VALUE && seek_disjunctor){
                vpts = frame->pts * av_q2d(st->vTimeBase);
                if(seek_pending && vpts < seek_pos){
                    av_frame_unref(frame);
                    continue;
                }
            }

            if(frame->hw_frames_ctx){
                AVFrame *sw_frame = av_frame_alloc();

                av_hwframe_transfer_data(sw_frame,frame,0);

                AVFrame *yuv_frame = av_frame_alloc();

                yuv_frame->format = AV_PIX_FMT_YUV420P;
                yuv_frame->width = sw_frame->width;
                yuv_frame->height = sw_frame->height;

                av_frame_get_buffer(yuv_frame,0);

                sws_scale(st->swsCtx,sw_frame->data,sw_frame->linesize,0,st->vCtx->height,yuv_frame->data,yuv_frame->linesize);

                st->vfq.push(yuv_frame);
                av_frame_free(&sw_frame);
                av_frame_free(&yuv_frame);
            }else{
                st->vfq.push(frame);
                av_frame_unref(frame);
            }
       }
    }

    avcodec_send_packet(st->vCtx, nullptr);
    while(Vdecode(st->vCtx, frame, nullptr) == 1){
        if(frame->hw_frames_ctx){
            AVFrame *sw_frame = av_frame_alloc();

            av_hwframe_transfer_data(sw_frame,frame,0);

            AVFrame *yuv_frame = av_frame_alloc();

            yuv_frame->format = AV_PIX_FMT_YUV420P;
            yuv_frame->width = sw_frame->width;
            yuv_frame->height = sw_frame->height;

            av_frame_get_buffer(yuv_frame,0);

            sws_scale(st->swsCtx,sw_frame->data,sw_frame->linesize,0,sw_frame->height,yuv_frame->data,yuv_frame->linesize);

            st->vfq.push(yuv_frame);
            av_frame_free(&sw_frame);
        }else{
            st->vfq.push(frame);
            av_frame_unref(frame);
        }
    }

    av_frame_free(&frame);
    return 0;
}

int audioThread(PlayerState *st)
{
    AVFrame *frame = av_frame_alloc();
    if(!frame){ return -1; }

    double apts = 0;

    uint8_t *buf = nullptr;
    st->bytes_per_sample = av_get_bytes_per_sample(st->aCtx->sample_fmt);

    int ringbuf_size = 4 * SDL_samples * st->aCtx->channels * st->bytes_per_sample;
    //rbuf.resize(ringbuf_size);

    while(g_running){

        if(Status == PAUSE){
            SDL_Delay(4);
            continue;
        }

        AVPacket *pkt = st->apq.pop(g_running);
        if(!pkt) break;

        if(!pkt->data){ av_packet_free(&pkt); break; }

        int ret = avcodec_send_packet(st->aCtx, pkt);
        av_packet_free(&pkt);
        if(ret < 0) continue;

        while(true){
            ret = avcodec_receive_frame(st->aCtx, frame);
            if(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if(ret < 0) break;

            if(frame->pts != AV_NOPTS_VALUE && seek_disjunctor){
                apts = frame->pts * av_q2d(st->aTimeBase);

                if(seek_pending && apts < seek_pos){
                    av_frame_unref(frame);
                    continue;
                }
            }

            st->max_samples = av_rescale_rnd(
                swr_get_delay(st->swrCtx,st->aCtx->sample_rate) + frame->nb_samples,
                st->aCtx->sample_rate,
                st->aCtx->sample_rate,
                AV_ROUND_UP
            );

            buf = (uint8_t*)av_malloc(st->max_samples * st->aCtx->channels * st->bytes_per_sample);
            if(!buf) return -1;

            int out = swr_convert(st->swrCtx, &buf, st->max_samples,
                        (const uint8_t **)frame->data, frame->nb_samples);
                    
            // if(out > 0){
            //     SDL_QueueAudio(st->audioDev, buf, out * st->aCtx->channels * st->bytes_per_sample);
            //     if(frame->pts != AV_NOPTS_VALUE){
            //         st->audio_clock.store(frame->pts * av_q2d(st->aTimeBase));
            //     }
            // }

            if(out > 0){
                ssize_t audio_byte_len = out * st->aCtx->channels * st->bytes_per_sample;
                rbuf.RB_push(buf,audio_byte_len);
            }

            if(seek_pending){
                seek_pending = false;
            }
            
            av_free(buf);
            av_frame_unref(frame);
        }
    }

    st->max_samples = av_rescale_rnd(
        swr_get_delay(st->swrCtx,st->aCtx->sample_rate) + frame->nb_samples,
        st->aCtx->sample_rate,
        st->aCtx->sample_rate,
        AV_ROUND_UP
    );

    buf = (uint8_t *)av_malloc(st->max_samples * st->aCtx->channels * st->bytes_per_sample);
    if(!buf){ return -1; }

    avcodec_send_packet(st->aCtx, nullptr);
    while(Adecode(st->aCtx, frame, nullptr) == 1){
        int out = swr_convert(st->swrCtx, &buf, st->max_samples,
                    (const uint8_t **)frame->data, frame->nb_samples);
        // if(out > 0){
        //     SDL_QueueAudio(st->audioDev, buf, out * 2 * sizeof(float));
        // }

        if(out > 0){
            ssize_t audio_byte_len = out * st->aCtx->channels * st->bytes_per_sample;
            rbuf.RB_push(buf,audio_byte_len);
        }

        av_frame_unref(frame);
    }

    av_frame_free(&frame);
    if(buf){
        av_free(buf);
    }
    return 0;
}

double getAudioClock(PlayerState *st)
{
    return st->audio_clock;
}

enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts){
    const enum AVPixelFormat *p;
    for(p = pix_fmts; *p != AV_PIX_FMT_NONE; p++){
        if(*p == AV_PIX_FMT_CUDA){
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

void audio_callback(void *userdata, Uint8 *stream, int len){

    PlayerState *st = (PlayerState*)userdata;

    SDL_memset(stream,0,len);
    int read = rbuf.RB_cat(stream,len);

    if(read > 0){
        double seconds = read / (double)(st->aCtx->sample_rate * st->aCtx->channels * st->bytes_per_sample);
        st->audio_clock = st->audio_clock + seconds;
    }

}


void HandleEvent(PlayerState *st, SDL_Event &event){
    if(event.type == SDL_QUIT) quit = true;

    if(event.type == SDL_KEYDOWN &&
       !event.key.repeat)
    {
        if(event.key.keysym.sym == SDLK_ESCAPE)
        {
            quit = true;
        }


        if(event.key.keysym.sym == SDLK_SPACE)
        {
            if(Status == PLAY)
            {
                Status = PAUSE;
                SDL_PauseAudio(1);
            }
            else
            {
                Status = PLAY;
                SDL_PauseAudio(0);
            }
        }

        if(event.key.keysym.sym == SDLK_LEFT){
            if(seek_disjunctor){
               seek_pos = getAudioClock(st) - TIME_INTERVAL;
                if(seek_pos <= 0){
                    seek_pos = 0;
                }
                std::cerr << "seek pos " << seek_pos << std::endl;
                seek_request = true;
            }
       }

        if(event.key.keysym.sym == SDLK_RIGHT){
            if(seek_disjunctor){
                seek_pos = getAudioClock(st) + TIME_INTERVAL;
                if(seek_pos >= file_duration){
                    seek_pos = file_duration;
                }
                seek_request = true;
                std::cerr << "seek pos " << seek_pos << std::endl;
            }
           //quit = true;
        }
    }
}

int video_seek(PlayerState *st){

    if(!seek_disjunctor){
        return 0;
    }

    Status = PAUSE;

    SDL_PauseAudio(1);

    st->apq.flush();
    st->vpq.flush();
    st->vfq.flush();

    rbuf.flush();

    int64_t us = (int64_t)(seek_pos * AV_TIME_BASE);

    int ret =avformat_seek_file(st->fmtCtx,-1,INT_MIN,us,INT_MAX,AVSEEK_FLAG_BACKWARD);
    if(ret < 0){
        std::cerr << __LINE__ << " seek video failed " << avError(ret) << std::endl;
        return -1;
    }

    avformat_flush(st->fmtCtx);
    st->eof = false;

    if(ret >= 0){
        std::cerr << "function avformat seek file use sucess" << std::endl;
    }

    avcodec_flush_buffers(st->vCtx);
    avcodec_flush_buffers(st->aCtx);

    seek_pending = true;

    audio_sync_offset = 0.0;

    st->audio_clock = seek_pos;

    SDL_PauseAudio(0);

    Status = PLAY;

    return 0;
}


int main(int argc, char *argv[])
{
    AVDictionary *opts = nullptr;

    av_dict_set(&opts,"rtmp_live","live",0);

    SDL_Event event;
    //硬件解码上下文
    AVBufferRef *hw_device_ctx = nullptr;

    if(argc < 2){
        std::cerr << "Usage: player <filename>" << std::endl;
        return -1;
    }

    PlayerState st;

    st.eof = false;

    av_log_set_level(AV_LOG_WARNING);

    if(av_hwdevice_ctx_create(&hw_device_ctx,AV_HWDEVICE_TYPE_CUDA,nullptr,nullptr,0) < 0){
        av_buffer_unref(&hw_device_ctx);
        return -1;    
    }

    if(avformat_open_input(&st.fmtCtx, argv[1], nullptr, &opts) < 0){
        std::cerr << "Cannot open " << argv[1] << std::endl;
        av_dict_free(&opts);
        return -1;
    }
    if(avformat_find_stream_info(st.fmtCtx, nullptr) < 0){
        std::cerr << "Cannot find stream info" << std::endl;
        av_dict_free(&opts);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }

    if(st.fmtCtx->duration != AV_NOPTS_VALUE){
        file_duration = st.fmtCtx->duration / (double)AV_TIME_BASE;
        std::cerr << "file duration : " << file_duration << std::endl;
    }
    if(st.fmtCtx->duration == AV_NOPTS_VALUE){
        seek_disjunctor = false;
        av_log(nullptr,AV_LOG_ERROR,"%d Cannot fast forward or rewind\n",__LINE__);
    }

    for(unsigned i = 0; i < st.fmtCtx->nb_streams; i++){
        if(st.fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && st.vIdx < 0)
            st.vIdx = i;
        if(st.fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && st.aIdx < 0)
            st.aIdx = i;
    }
    if(st.vIdx < 0 || st.aIdx < 0){
        std::cerr << "Need both video and audio stream" << std::endl;
        av_dict_free(&opts);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }

    AVCodec *vCodec = avcodec_find_decoder_by_name("h264_cuvid");
    AVCodec *aCodec = avcodec_find_decoder(st.fmtCtx->streams[st.aIdx]->codecpar->codec_id);
    if(!vCodec || !aCodec){
        std::cerr << "Decoder not found" << std::endl;
        av_dict_free(&opts);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }

    st.vCtx = avcodec_alloc_context3(vCodec);
    st.aCtx = avcodec_alloc_context3(aCodec);
    avcodec_parameters_to_context(st.vCtx, st.fmtCtx->streams[st.vIdx]->codecpar);
    avcodec_parameters_to_context(st.aCtx, st.fmtCtx->streams[st.aIdx]->codecpar);


    st.vCtx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    st.vCtx->get_format = get_hw_format;

    if(avcodec_open2(st.vCtx, vCodec, nullptr) < 0){
        std::cerr << "Cannot open video decoder" << std::endl;
        avcodec_free_context(&st.vCtx);
        avcodec_free_context(&st.aCtx);
        av_dict_free(&opts);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }
    if(avcodec_open2(st.aCtx, aCodec, nullptr) < 0){
        std::cerr << "Cannot open audio decoder" << std::endl;
        avcodec_free_context(&st.vCtx);
        avcodec_free_context(&st.aCtx);
        av_dict_free(&opts);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }

    st.vTimeBase = st.fmtCtx->streams[st.vIdx]->time_base;
    st.aTimeBase = st.fmtCtx->streams[st.aIdx]->time_base;

    int video_w = st.vCtx->width;
    int video_h = st.vCtx->height;

    int vw = st.vCtx->width;
    int vh = st.vCtx->height;

    SDL_Rect dstrect = {};

    std::cout << "video_w = " << video_w << std::endl;
    std::cout << "video_h = " << video_h << std::endl;

    double ratio_v = vw / (double)vh;

    if((double)vh >= SCREEN_H){
        vh = (int)SCREEN_H;
        vw = (int)(vh * ratio_v);
    }
    if((double)vw >= SCREEN_W){
        vw = (int)SCREEN_W;
        vh = (int)(vw / ratio_v);
    }

    std::cout << "scale after vw = " << vw << std::endl;
    std::cout << "scale after vh = " << vh << std::endl;

    dstrect.x = 0;
    dstrect.y = 0;

    dstrect.w = vw;
    dstrect.h = vh;

    st.swrCtx = swr_alloc_set_opts(nullptr,
        AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT, st.aCtx->sample_rate,
        st.aCtx->channel_layout, st.aCtx->sample_fmt, st.aCtx->sample_rate,
        0, nullptr);
    if(!st.swrCtx || swr_init(st.swrCtx) < 0){
        std::cerr << "Cannot init resampler" << std::endl;
        swr_free(&st.swrCtx);
        avcodec_free_context(&st.vCtx);
        avcodec_free_context(&st.aCtx);
        av_dict_free(&opts);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }

    st.swsCtx = sws_getContext(video_w,video_h,AV_PIX_FMT_NV12,video_w,video_h,AV_PIX_FMT_YUV420P,SWS_BICUBIC,nullptr,nullptr,nullptr);
    if(!st.swrCtx){
        std::cerr << "Cannot init resampler" << std::endl;
        swr_free(&st.swrCtx);
        avcodec_free_context(&st.vCtx);
        avcodec_free_context(&st.aCtx);
        av_dict_free(&opts);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0){
        std::cerr << "SDL init failed: " << SDL_GetError() << std::endl;
        swr_free(&st.swrCtx);
        avcodec_free_context(&st.vCtx);
        avcodec_free_context(&st.aCtx);
        av_dict_free(&opts);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }


    SDL_Window *win = SDL_CreateWindow("player", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        vw, vh, SDL_WINDOW_SHOWN);


    if(!win){
        std::cerr << "Cannot create window: " << SDL_GetError() << std::endl;
        SDL_Quit();
        swr_free(&st.swrCtx);
        avcodec_free_context(&st.vCtx);
        avcodec_free_context(&st.aCtx);
        av_dict_free(&opts);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, video_w, video_h);

    SDL_AudioSpec want = {};
    want.freq = st.aCtx->sample_rate;
    want.format = AUDIO_F32LSB;
    want.channels = 2;
    want.samples = SDL_samples;
    want.callback = audio_callback;
    want.userdata = (void*)&st;

    //st.audioDev = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);

    if(SDL_OpenAudio(&want,nullptr) < 0){
        std::cerr << "Audio Device open failed" << std::endl;
        SDL_Quit();
        swr_free(&st.swrCtx);
        avcodec_free_context(&st.vCtx);
        avcodec_free_context(&st.aCtx);
        av_dict_free(&opts);
        avformat_close_input(&st.fmtCtx);
        return -1;
    }


    double avg_framerate = av_q2d(st.fmtCtx->streams[st.vIdx]->avg_frame_rate);
    if(avg_framerate <= 0.0){
        avg_framerate = av_q2d(st.fmtCtx->streams[st.vIdx]->r_frame_rate);
    }
    if(avg_framerate <= 0.0){
        avg_framerate = 30.0;
    }

    std::thread tRead(readThread, &st);
    std::thread tVideo(videoThread, &st);
    std::thread tAudio(audioThread, &st);

    //SDL_PauseAudioDevice(st.audioDev, 0);
    SDL_PauseAudio(0);

    double frame_last_pts = 0.0;

    double now_clock;

    bool paused = false;
    double pause_start;
    double frame_timer = SDL_GetTicks();
    while(!quit){
        while(SDL_PollEvent(&event)){
            HandleEvent(&st,event);
        }

        double pts = 0;

        if(Status == PAUSE){
            if(!paused){
                pause_start = SDL_GetTicks();
                paused=true;
            }
            SDL_Delay(4);
            continue;
        }
        
        if(paused){
            frame_timer += SDL_GetTicks() - pause_start;
            paused = false;
        }



        AVFrame *frame = st.vfq.try_pop();
        if(!frame){
            SDL_Delay(3);
            continue;
        }
        // std::cout << av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)) << std::endl;

        if(frame->pts != AV_NOPTS_VALUE){
            pts = frame->pts * av_q2d(st.vTimeBase);
            if(seek_done){
                frame_last_pts = NAN;
                //st.audio_clock = pts;
                frame_timer = SDL_GetTicks();
                seek_done = false;
            }

        }else if(!std::isnan(frame_last_pts)){
            pts = frame_last_pts + 1.0 / avg_framerate;
        }else{
            pts = getAudioClock(&st);
        }

        

        double delay = 1.0 / avg_framerate;
        if(!std::isnan(frame_last_pts) && pts > frame_last_pts){
            delay = pts - frame_last_pts;
        }
        if(delay <= 0 || delay > 1.0){
            delay = 1.0 / avg_framerate;
        }

        now_clock = SDL_GetTicks();
        if((now_clock - last_sync_check_ticks) > SYNC_CHECK_INTERVAL){
            double diff = pts - getAudioClock(&st);
            if(fabs(diff) > BIG_DIFF_TIMESOUT || std::isnan(diff)){
                if(diff < -BIG_DIFF_TIMESOUT || std::isnan(diff)){
                    av_frame_free(&frame);
                    frame_last_pts = NAN;
                    while(SDL_PollEvent(&event)){
                        HandleEvent(&st,event);
                    }
                    continue;
                }
            }
            else{
                audio_sync_offset = audio_sync_offset + diff * 0.08;
                if(audio_sync_offset > MAX_STEP_ADJUST) audio_sync_offset = MAX_STEP_ADJUST;
                if(audio_sync_offset < -MAX_STEP_ADJUST) audio_sync_offset = -MAX_STEP_ADJUST;
            }
            last_sync_check_ticks = now_clock;
        }

        delay += audio_sync_offset;

        if(delay < 0.005) delay = 0.005;

        Uint32 delay_ms = (Uint32)(delay * 1000);
        Uint32 target = frame_timer + delay_ms;

        if(target > SDL_GetTicks()){
            while(SDL_GetTicks() < target && !quit){
                while(SDL_PollEvent(&event)){
                    HandleEvent(&st,event);
                }
                SDL_Delay(1);
            }
        }

        //std::cout << av_get_pix_fmt_name((AVPixelFormat)frame->format) << std::endl;

        SDL_UpdateYUVTexture(tex, nullptr,
            frame->data[0], frame->linesize[0],
            frame->data[1], frame->linesize[1],
            frame->data[2], frame->linesize[2]);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, &dstrect);
        SDL_RenderPresent(ren);

        frame_last_pts = pts;
        frame_timer = target;

        av_frame_free(&frame);
    }

    g_running = false;


    tRead.join();
    tVideo.join();
    tAudio.join();

    st.vpq.flush();
    st.apq.flush();

    av_buffer_unref(&hw_device_ctx);

    SDL_CloseAudio();
    //SDL_CloseAudioDevice(st.audioDev);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();

    swr_free(&st.swrCtx);
    sws_freeContext(st.swsCtx);
    avcodec_free_context(&st.vCtx);
    avcodec_free_context(&st.aCtx);
    av_dict_free(&opts);
    avformat_close_input(&st.fmtCtx);

    return 0;
}
