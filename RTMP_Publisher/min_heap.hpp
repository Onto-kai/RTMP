extern "C"{
    #include <libavformat/avformat.h>
}


#include <queue>
#include <vector>
#include <mutex>
#include <condition_variable>

typedef struct MuxPacket{
    AVPacket *pkt = nullptr;

    int64_t dts;

    uint64_t serial;

    int stream_index;

    MuxPacket& operator=(const MuxPacket&)=delete;

}MuxPacket;

struct MuxPacketCompare{
    bool operator()(const MuxPacket &a, const MuxPacket &b){
        if(a.dts != b.dts){
            return a.dts > b.dts;
        }
        return a.serial > b.serial;
    }
};

class Packet_Min_heap{
public:
    Packet_Min_heap(int size = 60,int i = 0):max_size_(size),i(i){};

    int pkt_push(AVPacket *pkt){
        std::unique_lock<std::mutex> lock(mtx_);
        MuxPacket mux_pkt;
        mux_pkt.pkt = av_packet_alloc();
        cv_.wait_for(lock,std::chrono::milliseconds(5),[&]{
            return Mux_queue_.size() < max_size_;
        });
        mux_pkt.dts = pkt->dts;
        mux_pkt.stream_index = pkt->stream_index;
        mux_pkt.serial = i++;
        av_packet_ref(mux_pkt.pkt,pkt);
        Mux_queue_.push(std::move(mux_pkt));
        av_packet_unref(mux_pkt.pkt);
        return 0;
    }

    AVPacket *pkt_pull(){
        std::lock_guard<std::mutex> lock(mtx_);
        AVPacket *copy;
        while(Mux_queue_.size() > 15){
            copy = Mux_queue_.top().pkt;
            Mux_queue_.pop();
            return copy;
        }
        return nullptr;
        cv_.notify_one();
    }

    AVPacket *pkt_pullAll(){
        std::lock_guard<std::mutex> lock(mtx_);
        AVPacket *copy;
        while(!Mux_queue_.empty()){
            copy = Mux_queue_.top().pkt;
            Mux_queue_.pop();
            return copy;
        }
        return nullptr;
    }

    void flush(){
        std::lock_guard<std::mutex> lock(mtx_);
        AVPacket *pkt = nullptr;
        while(!Mux_queue_.empty()){
            pkt = Mux_queue_.top().pkt;
            Mux_queue_.pop();
            av_packet_free(&pkt);
        }
    }

    ~Packet_Min_heap(){
        flush();
    }

private:
    std::priority_queue<MuxPacket, std::vector<MuxPacket>,MuxPacketCompare> Mux_queue_;

    std::mutex mtx_;

    std::condition_variable cv_;

    int max_size_;

    int i;
};