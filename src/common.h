// Storm - shared helpers.
#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

// Fatal errors surface as a message box rather than a silent exit: a
// screensaver has no console, and "nothing happened" is the worst possible
// diagnostic when Windows launches it for you.
void FailHard(const char* what, HRESULT hr = S_OK);

#define STORM_CHECK(expr, what)                                   \
    do {                                                          \
        HRESULT storm_hr_ = (expr);                               \
        if (FAILED(storm_hr_)) FailHard(what, storm_hr_);         \
    } while (0)

std::string Narrow(const wchar_t* w);
std::wstring Widen(const char* s);
