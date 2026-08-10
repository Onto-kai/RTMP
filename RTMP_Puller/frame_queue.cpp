#include "frame_queue.h"

Frame_queue::Frame_queue(int max) : max_size(max)
{
}

void Frame_queue::push(AVFrame *frame)
{
    AVFrame *copy = av_frame_alloc();
    if(!copy) return;
    av_frame_ref(copy, frame);

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::milliseconds(50), [&]{
            return (int)queue.size() < max_size;
        });
        queue.push(copy);
    }
    cv.notify_one();
}

AVFrame *Frame_queue::pop(std::atomic<bool> &running)
{
    std::unique_lock<std::mutex> lock(mtx);
    while(running || !queue.empty()){
        if(!queue.empty()){
            AVFrame *frame = queue.front();
            queue.pop();
            cv.notify_one();
            return frame;
        }
        cv.wait_for(lock, std::chrono::milliseconds(5));
    }
    return nullptr;
}

AVFrame *Frame_queue::try_pop()
{
    std::lock_guard<std::mutex> lock(mtx);
    if(queue.empty()) return nullptr;

    AVFrame *frame = queue.front();
    queue.pop();
    cv.notify_one();
    return frame;
}

void Frame_queue::flush()
{
    std::lock_guard<std::mutex> lock(mtx);
    while(!queue.empty()){
        AVFrame *frame = queue.front();
        queue.pop();
        av_frame_free(&frame);
    }
}
