extern "C"{
    #include <libavformat/avformat.h>
}

#include <queue>
#include <mutex>
#include <condition_variable>

class Packet_queue{
public:
    Packet_queue(ssize_t size = 60):max_size_(size){};

    int pkt_push(AVPacket *pkt){
        std::unique_lock <std::mutex> lock(mtx_);
        cv_.wait_for(lock,std::chrono::milliseconds(5),[&]{
            return queue_.size() < max_size_;
        });
        AVPacket *copy = av_packet_alloc();
        av_packet_ref(copy,pkt);
        queue_.push(copy);
        return 0;
    }

    AVPacket * pkt_pull(){
        std::unique_lock lock(mtx_);
        while(true){
            if(queue_.empty()){
                AVPacket *pkt = queue_.front();
                queue_.pop();
                cv_.notify_one();
                return pkt;
            }else{
                cv_.wait_for(lock,std::chrono::milliseconds(5));
            }
        }
    }

    void flush(){
        std::lock_guard <std::mutex> lock(mtx_);
        while(!queue_.empty()){
            AVPacket *temp_pkt = queue_.front();
            queue_.pop();
            av_packet_free(&temp_pkt); 
        }
    }

private:
    std::queue<AVPacket *> queue_;
    ssize_t max_size_;
    std::mutex mtx_;
    std::condition_variable cv_;   

};