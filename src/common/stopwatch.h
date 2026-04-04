#pragma once

#include <chrono>

namespace space_scanner
{

/**
 Lightweight monotonic stopwatch for elapsed wall time.
 **/
class Stopwatch
{
  public:
    Stopwatch();

    void Reset();
    double ElapsedSeconds() const;

  private:
    std::chrono::steady_clock::time_point m_start;
};

} // namespace space_scanner
