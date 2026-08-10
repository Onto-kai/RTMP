#!/bin/bash

g++ -O2 -std=c++17 rtmp_publisher.cpp transcoder.hpp -o publisher -D__STDC_CONSTANT_MACROS `pkg-config --cflags --libs libavutil libavformat libavcodec`
