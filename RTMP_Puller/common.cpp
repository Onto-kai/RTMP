#include "common.h"

std::string avError(int ret){
    std::string ebuff(AV_ERROR_MAX_STRING_SIZE, 0);
    av_strerror(ret, ebuff.data(), AV_ERROR_MAX_STRING_SIZE);
    ebuff.resize(strlen(ebuff.data()));
    return ebuff;
}
