#pragma once

extern "C"{
    #include <SDL2/SDL.h>
}

#include <vector>
#include <mutex>
#include <condition_variable>

class Ring_buffer{
public:
    Ring_buffer(int size = 65536);
    ~Ring_buffer();

    bool resize(int size);
    int RB_push(uint8_t *data, ssize_t size);
    int RB_cat(Uint8 *stream, ssize_t len);
    ssize_t get_size();
    bool flush();

private:
    std::vector<uint8_t> buf;
    ssize_t data_size;
    ssize_t read_pos;
    ssize_t write_pos;

    std::mutex mtx;
    std::condition_variable cv;
};
