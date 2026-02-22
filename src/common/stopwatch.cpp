#include "common/stopwatch.h"

namespace space_scanner {

/**
 Constructs the stopwatch and starts timing immediately.
 **/
Stopwatch::Stopwatch() {
    Reset();
}

/**
 Resets the start timestamp to the current monotonic clock tick.
 **/
void Stopwatch::Reset() {
    m_start = std::chrono::steady_clock::now();
}

/**
 Returns elapsed seconds since construction or last reset.
 
 Returns: Elapsed time in seconds.
 **/
double Stopwatch::ElapsedSeconds() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - m_start);
    return elapsed.count();
}

} // namespace space_scanner
