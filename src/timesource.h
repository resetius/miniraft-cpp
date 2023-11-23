#pragma once

#include <stdint.h>

struct ITimeSource {
    virtual ~ITimeSource() = default;
    virtual uint64_t now() = 0;
};

class TTimeSource: public ITimeSource {
public:
    uint64_t now();
};
