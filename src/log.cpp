#include "log.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace tc2vr::log {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;

}  // namespace

void Open() {
    std::lock_guard lock(g_mutex);
    if (g_file) {
        return;
    }
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = path + len;
    while (slash > path && slash[-1] != '\\' && slash[-1] != '/') {
        --slash;
    }
    snprintf(slash, MAX_PATH - (slash - path), "tc2vr.log");
    g_file = fopen(path, "w");
}

void Write(const char* fmt, ...) {
    std::lock_guard lock(g_mutex);
    if (!g_file) {
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute,
            t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);
    fputc('\n', g_file);
    fflush(g_file);
}

}  // namespace tc2vr::log
