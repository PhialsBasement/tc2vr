#pragma once

namespace tc2vr::log {

void Open();
void Write(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace tc2vr::log

#define LOG(...) ::tc2vr::log::Write(__VA_ARGS__)
