#include "common/stopwatch.h"

namespace space_scanner {

Stopwatch::Stopwatch() {
    Reset();
}

void Stopwatch::Reset() {
    start_ = std::chrono::steady_clock::now();
}

double Stopwatch::ElapsedSeconds() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - start_);
    return elapsed.count();
}

} // namespace space_scanner

