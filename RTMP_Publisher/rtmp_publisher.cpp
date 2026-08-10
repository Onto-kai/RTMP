extern "C"{
    #include <libavformat/avformat.h>
    #include <libavcodec/avcodec.h>
    #include <libavutil/log.h>
}

#include "transcoder.hpp"
//#include "min_heap.hpp"
#include <iostream>
#include <string>

bool video_transcoder_flag = false;
bool audio_transcoder_flag = false;

typedef struct Player{
    AVFormatContext *srcfmtCtx = nullptr;
    AVFormatContext *dstfmtCtx = nullptr;

    int vIdx = -1;
    int aIdx = -1;

}Player;

int main(int argc, char *argv[]){

    std::cerr << "start " << std::endl;

    Player py;
    Transcoder transcoder;

    if(argc < 3){
        std::cerr << "parameter error" << std::endl;
        return -1;
    }

    std::string srcfile = argv[1];
    std::string url = argv[2]; 

    if(avformat_open_input(&py.srcfmtCtx,srcfile.data(),nullptr,nullptr) < 0){
        std::cerr << "open source file failed" << std::endl;
        return -1;
    }

    if(avformat_find_stream_info(py.srcfmtCtx,nullptr) < 0){
        avformat_close_input(&py.srcfmtCtx);
        std::cerr << "streams information not found" << std::endl;
        return -1;
    }

    for(int i = 0; i < py.srcfmtCtx->nb_streams; i++){
        if(py.srcfmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO){
            py.vIdx = i;
        }
        if(py.srcfmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO){
            py.aIdx = i;
        }
    }

    if(py.vIdx == -1){
        std::cerr << "source file not have video stream" << std::endl;
    }

    if(py.aIdx == -1){
        std::cerr << "source file not have audio stream" << std::endl;
    }

    if(py.srcfmtCtx->streams[py.vIdx]->codecpar->codec_id != AV_CODEC_ID_H264){
        video_transcoder_flag = true;
    }

    if(py.srcfmtCtx->streams[py.aIdx]->codecpar->codec_id != AV_CODEC_ID_AAC){
        audio_transcoder_flag = true;
    }


    if(video_transcoder_flag){
        if(transcoder.video_transcoder_init(py.srcfmtCtx, py.vIdx) < 0){
            std::cerr << "video transcoder init failed" << std::endl;
            avformat_close_input(&py.srcfmtCtx);
            return -1;
        }
    }

    if(audio_transcoder_flag){
        if(transcoder.audio_transcoder_init(py.srcfmtCtx, py.vIdx) < 0){
            std::cerr << "audio transcoder init failed" << std::endl;
            avformat_close_input(&py.srcfmtCtx);
            return -1;
        }
    }

    if(avformat_alloc_output_context2(&py.dstfmtCtx,nullptr,"flv",url.data()) < 0){
        std::cerr << "No memory or other error" << std::endl;
        avformat_close_input(&py.srcfmtCtx);
        if(py.dstfmtCtx){
            avformat_close_input(&py.dstfmtCtx);
        }
        transcoder.audio_transcoder_free();
        transcoder.video_transcoder_free();
        return -1;
    }


    AVStream *Vout = avformat_new_stream(py.dstfmtCtx,nullptr);
    if(Vout == nullptr){
        std::cerr << "video output context streams allocate faile" << std::endl;
    }
    if(avcodec_parameters_copy(Vout->codecpar,py.srcfmtCtx->streams[py.vIdx]->codecpar) < 0){
        std::cerr << "video streams parameters copy failed" << std::endl;
    }
    Vout->time_base = py.srcfmtCtx->streams[py.vIdx]->time_base;

    AVStream *Aout = avformat_new_stream(py.dstfmtCtx,nullptr);
    if(Aout == nullptr){
        std::cerr << "audio output context streams allocate faile" << std::endl;
    }
    if(avcodec_parameters_copy(Aout->codecpar,py.srcfmtCtx->streams[py.aIdx]->codecpar) < 0){
        std::cerr << "audio streams parameters copy failed" << std::endl;
    }
    Aout->time_base = py.srcfmtCtx->streams[py.aIdx]->time_base;

    Vout->codecpar->codec_tag = 0;
    Aout->codecpar->codec_tag = 0;

    if(!(py.dstfmtCtx->oformat->flags & AVFMT_NOFILE))
    {
        avio_open(
            &py.dstfmtCtx->pb,
            url.data(),
            AVIO_FLAG_WRITE
        );
    }

    int ret = avformat_write_header(py.dstfmtCtx,nullptr);
    if(ret < 0){
        std::cerr << "write header failed" << std::endl;
        avformat_close_input(&py.srcfmtCtx);
        avformat_close_input(&py.dstfmtCtx);
        transcoder.video_transcoder_free();
        transcoder.audio_transcoder_free();
        return -1;
    }

    AVPacket *pkt = av_packet_alloc();
    if(pkt == nullptr){
        avformat_close_input(&py.srcfmtCtx);
        avformat_close_input(&py.dstfmtCtx);
        transcoder.video_transcoder_free();
        transcoder.audio_transcoder_free();
        std::cerr << "packet allocate failed" << std::endl;
        return -1;
    }

    AVStream *Istream = nullptr;
    AVStream *Ostream = nullptr;
    std::cerr << "start read packet" << std::endl;

    while(true){
        if(av_read_frame(py.srcfmtCtx,pkt) < 0){
            break;
        }

        if(pkt->stream_index == py.aIdx){
            if(audio_transcoder_flag){
                transcoder.video_transcoder(pkt,[&pkt](AVPacket *src_pkt){
                    av_packet_unref(pkt);
                    av_packet_ref(pkt,src_pkt);
                });
            }
            Istream = py.srcfmtCtx->streams[py.aIdx];
            pkt->stream_index = 1;
            Ostream = py.dstfmtCtx->streams[pkt->stream_index];
        }
        else if(pkt->stream_index == py.vIdx){
            if(video_transcoder_flag){
                transcoder.video_transcoder(pkt,[&pkt](AVPacket *src_pkt){
                    av_packet_unref(pkt);
                    av_packet_ref(pkt,src_pkt);
                });
            }
            Istream = py.srcfmtCtx->streams[py.vIdx];
            pkt->stream_index = 0;
            Ostream = py.dstfmtCtx->streams[pkt->stream_index];
        }
        else{
            av_packet_unref(pkt);
            continue;
        }

        av_packet_rescale_ts(pkt,Istream->time_base,Ostream->time_base);

        av_interleaved_write_frame(py.dstfmtCtx,pkt);
        av_packet_unref(pkt);
   }

    av_write_trailer(py.dstfmtCtx);


    if(py.srcfmtCtx){
        avformat_close_input(&py.srcfmtCtx);
        py.srcfmtCtx = nullptr;
    }

    if(py.dstfmtCtx){
        avio_closep(&py.dstfmtCtx->pb);
        avformat_free_context(py.dstfmtCtx);
        py.dstfmtCtx = nullptr;
    }

    if(pkt){
        av_packet_free(&pkt);
        pkt = nullptr;
    }
 
    std::cout << __FILE__ << std::endl;
 
    return 0;
}