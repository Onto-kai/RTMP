#pragma once

extern "C"{
    #include <libavutil/avutil.h>
    #include <libavformat/avformat.h>
}

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

class Frame_queue{
public:
    explicit Frame_queue(int max = 60);
    void push(AVFrame *frame);
    AVFrame *pop(std::atomic<bool> &running);
    AVFrame *try_pop();
    void flush();

private:
    std::mutex mtx;
    std::condition_variable cv;
    std::queue<AVFrame*> queue;
    int max_size;
};
