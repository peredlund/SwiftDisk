// SPDX-FileCopyrightText: 2026 Per Edlund
// SPDX-License-Identifier: GPL-3.0-or-later

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
