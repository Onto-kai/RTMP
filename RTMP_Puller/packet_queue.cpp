#include "packet_queue.h"

Packet_queue::Packet_queue(int max) : max_size(max)
{
}

void Packet_queue::push(AVPacket *pkt)
{
    {
        std::unique_lock<std::mutex> lock(mutex);

        if(!pkt){
            queue.push(nullptr);
        }else{
            cond.wait_for(lock,std::chrono::milliseconds(20),[&]{
                return queue.size() < max_size;
            });
            AVPacket *copy = av_packet_alloc();
            if(!copy) return;
            av_packet_ref(copy, pkt);
            queue.push(copy);
        }
    }
    cond.notify_one();
}

AVPacket* Packet_queue::pop(std::atomic<bool> &running)
{
    std::unique_lock<std::mutex> lock(mutex);
    while(running || !queue.empty()){
        if(!queue.empty()){
            AVPacket *pkt = queue.front();
            queue.pop();
            return pkt;
        }
        cond.wait_for(lock, std::chrono::milliseconds(5));
    }
    cond.notify_one();
    return nullptr;
}

void Packet_queue::flush()
{
    std::lock_guard<std::mutex> lock(mutex);
    while(!queue.empty()){
        AVPacket *pkt = queue.front();
        queue.pop();
        av_packet_free(&pkt);
    }
    cond.notify_all();
}
