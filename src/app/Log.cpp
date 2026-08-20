#include "app/Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace od {

void Logf(const std::string& tag, const char* fmt, ...)
{
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME now;
    GetLocalTime(&now);

    // One formatted buffer, one write call: concurrent senders then interleave
    // by line instead of mid-line. stderr because it is unbuffered — main.cpp
    // points both streams at the same log file anyway.
    fprintf(stderr, "%02d:%02d:%02d.%03d [%s] %s", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, tag.c_str(),
            msg);
}

} // namespace od
