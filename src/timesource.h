#pragma once

#include <stdint.h>

struct ITimeSource {
    virtual ~ITimeSource() = default;
    virtual uint64_t Now() = 0;
};

class TTimeSource: public ITimeSource {
public:
    uint64_t Now();
};
