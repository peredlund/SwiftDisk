#pragma once

#include <chrono>

namespace space_scanner {

class Stopwatch {
public:
    Stopwatch();

    void Reset();
    double ElapsedSeconds() const;

private:
    std::chrono::steady_clock::time_point start_;
};

} // namespace space_scanner

