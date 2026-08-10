#pragma once

extern "C"{
    #include <libavutil/avutil.h>
}

#include <string>

std::string avError(int ret);
