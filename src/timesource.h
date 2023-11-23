#pragma once

#include <stdint.h>

class TTimeSource {
public:
    uint64_t now();
};
