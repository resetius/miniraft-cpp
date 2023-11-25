#pragma once

#include <chrono>

struct ITimeSource {
    using Time = std::chrono::time_point<std::chrono::steady_clock>;
    virtual ~ITimeSource() = default;
    virtual Time Now() = 0;
};

class TTimeSource: public ITimeSource {
public:
    Time Now() override {
        return std::chrono::steady_clock::now();
    }
};
