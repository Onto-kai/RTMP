#pragma once

extern "C"{
    #include <libavformat/avformat.h>
    #include <libavutil/avutil.h>
    #include <libavutil/log.h>
}

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

class Packet_queue
{
public:
    explicit Packet_queue(int max = 300);
    void push(AVPacket *pkt);
    AVPacket* pop(std::atomic<bool> &running);
    void flush();

private:
    std::queue<AVPacket*> queue;
    std::mutex mutex;
    std::condition_variable cond;
    int max_size;
};
