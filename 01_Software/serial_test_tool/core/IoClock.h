#pragma once

#include <chrono>

#include <QtGlobal>

// 进程内统一的单调时间源。数值只用于同一进程内的相减，不与日历时间混用。
inline qint64 ioMonotonicUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
