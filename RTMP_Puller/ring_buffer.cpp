#include "ring_buffer.h"

Ring_buffer::Ring_buffer(int size)
{
    buf.resize(size);
    write_pos = 0;
    read_pos = 0;
    data_size = 0;
}

Ring_buffer::~Ring_buffer()
{
}

bool Ring_buffer::resize(int size)
{
    if(size <= 65536) size = 65536;
    buf.resize(size);
    return true;
}

int Ring_buffer::RB_push(uint8_t *data, ssize_t size)
{
    std::unique_lock<std::mutex> lock(mtx);
    cv.wait(lock, [&]{
        return data_size < (ssize_t)buf.size();
    });

    ssize_t bytes = size > ((ssize_t)buf.size() - data_size) ? ((ssize_t)buf.size() - data_size) : size;

    ssize_t first_chunk = (ssize_t)buf.size() - write_pos;
    if(first_chunk > bytes) first_chunk = bytes;

    SDL_memcpy(buf.data() + write_pos, data, first_chunk);
    if(first_chunk < bytes){
        SDL_memcpy(buf.data(), data + first_chunk, bytes - first_chunk);
    }

    write_pos = (write_pos + bytes) % (ssize_t)buf.size();
    data_size += bytes;

    cv.notify_one();
    lock.unlock();
    return 0;
}

int Ring_buffer::RB_cat(Uint8 *stream, ssize_t len)
{
    std::unique_lock<std::mutex> lock(mtx);

    ssize_t to_read = len;
    if(to_read > data_size){
        to_read = data_size;
    }

    ssize_t first_chunk = (ssize_t)buf.size() - read_pos;
    if(first_chunk > to_read){
        first_chunk = to_read;
    }

    SDL_memcpy(stream, buf.data() + read_pos, first_chunk);
    if(first_chunk < to_read){
        SDL_memcpy(stream + first_chunk, buf.data(), to_read - first_chunk);
    }

    read_pos = (read_pos + to_read) % (ssize_t)buf.size();
    data_size -= to_read;

    lock.unlock();
    cv.notify_one();
    return first_chunk;
}

ssize_t Ring_buffer::get_size()
{
    std::lock_guard<std::mutex> lock(mtx);
    return data_size;
}

bool Ring_buffer::flush()
{
    std::lock_guard<std::mutex> lock(mtx);
    SDL_memset(buf.data(),0,buf.size()); 
    write_pos = 0;
    read_pos = 0;
    data_size = 0;
    return true;
}
