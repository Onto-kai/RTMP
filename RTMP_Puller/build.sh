#!/bin/bash

g++ -O2 -std=c++17 \
    common.cpp \
    packet_queue.cpp \
    frame_queue.cpp \
    audio_player.cpp \
    video_player.cpp \
    ring_buffer.cpp \
    player.cpp \
    -o player \
    -D__STDC_CONSTANT_MACROS \
    `pkg-config --cflags --libs libavutil libavformat libavcodec libswresample libswscale sdl2` \
    -lpthread
