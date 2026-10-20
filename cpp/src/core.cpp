#include "core.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>

Rng& rng() {
    static Rng instance;
    return instance;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), size);
    return out;
}

std::wstring formatPercent(float p) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%d%%", static_cast<int>(std::lround(clampf(p, 0.f, 1.f) * 100.f)));
    return buf;
}

std::wstring formatFloat(float v, int digits) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.*f", digits, v);
    return buf;
}
