#pragma once

#include <chrono>

namespace swift_disk
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

} // namespace swift_disk
