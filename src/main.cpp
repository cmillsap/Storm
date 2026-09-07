// Storm - entry point and the .scr command-line contract.
//
// Windows launches a screensaver with /s to run, /c to configure and /p with a
// window handle to draw the little preview in the Settings dialog. Both /c and
// /p can arrive with the handle attached by a colon.

#include "app.h"
#include "settings.h"

#include <tlhelp32.h>
#include <algorithm>
#include <string>
#include <vector>

#include "../resource.h"

// A running /p preview polls this and stands down when a new instance signals
// it, so the newcomer can create a swap chain on the same window. Without the
// handshake the second instance fails and the Settings preview goes black.
const wchar_t* kExitEventName = L"Local\\StormExitPreview";

static void KillOtherInstances()
{
    const DWORD self = GetCurrentProcessId();

    HANDLE exitEvent = CreateEventW(nullptr, TRUE, FALSE, kExitEventName);
    if (exitEvent) SetEvent(exitEvent);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        if (exitEvent) { ResetEvent(exitEvent); CloseHandle(exitEvent); }
        return;
    }

    std::vector<HANDLE> others;
    PROCESSENTRY32W entry = { sizeof(entry) };
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (_wcsicmp(entry.szExeFile, L"Storm.scr") == 0 && entry.th32ProcessID != self)
            {
                HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
                if (p) others.push_back(p);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    if (!others.empty())
    {
        // Give them a second to exit cleanly so DXGI destructors run, then
        // insist.
        const DWORD deadline = GetTickCount() + 1000;
        bool forced = false;
        for (HANDLE p : others)
        {
            const DWORD now = GetTickCount();
            const DWORD remaining = (deadline > now) ? (deadline - now) : 0;
            if (WaitForSingleObject(p, remaining) == WAIT_TIMEOUT)
            {
                TerminateProcess(p, 0);
                forced = true;
            }
            CloseHandle(p);
        }
        if (forced) Sleep(200);
    }

    // Reset so the incoming instance's own loop does not see it signalled.
    if (exitEvent) { ResetEvent(exitEvent); CloseHandle(exitEvent); }
}

// Per-monitor v2 keeps window sizes in real pixels on mixed-DPI desks. Resolved
// dynamically so no manifest is needed and older systems simply skip it.
static void EnableDpiAwareness()
{
    using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
    {
        auto setContext = (SetContextFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setContext && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    SetProcessDPIAware();
}

// ------------------------------------------------------------------- probing

static std::vector<RECT> g_probeMonitors;

static BOOL CALLBACK ProbeMonitorCallback(HMONITOR, HDC, LPRECT rect, LPARAM)
{
    g_probeMonitors.push_back(*rect);
    return TRUE;
}

static void DescribeLayout(std::string& report, const char* title,
                           const std::vector<std::pair<UINT, UINT>>& sizes)
{
    char line[256];

    std::vector<View> views(sizes.size());
    for (size_t i = 0; i < sizes.size(); ++i)
    {
        views[i].width = sizes[i].first;
        views[i].height = sizes[i].second;
    }

    UINT tw = 0, th = 0;
    ComputeSharedTargetSize(views, tw, th);

    RenderTarget target;
    target.width = tw;
    target.height = th;

    std::snprintf(line, sizeof(line), "%s\n  shared target %ux%u (aspect %.3f)\n",
                  title, tw, th, (float)tw / (float)th);
    report += line;

    for (View& v : views)
    {
        v.source = &target;
        float scale[2], offset[2];
        v.cropToFill(scale, offset);

        // What fraction of the view's own pixels the crop actually supplies.
        const float suppliedWidth = (float)tw * scale[0];
        std::snprintf(line, sizeof(line),
                      "  view %4ux%-4u aspect %5.3f  crop scale (%.3f, %.3f) offset (%.3f, %.3f)"
                      "  source px %.0fx%.0f -> %.0f%% of native\n",
                      v.width, v.height, v.aspect(), scale[0], scale[1], offset[0], offset[1],
                      suppliedWidth, (float)th * scale[1],
                      100.0f * suppliedWidth / (float)v.width);
        report += line;
    }
    report += "\n";
}

static bool WriteProbeReport(const wchar_t* path)
{
    std::string report = "Storm " STORM_VERSION_ASCII " - multi-monitor probe\n"
                         "=========================================\n\n";
    char line[256];

    g_probeMonitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, ProbeMonitorCallback, 0);

    std::snprintf(line, sizeof(line), "This machine reports %zu monitor(s):\n",
                  g_probeMonitors.size());
    report += line;

    std::vector<std::pair<UINT, UINT>> actual;
    for (const RECT& r : g_probeMonitors)
    {
        const UINT w = (UINT)(r.right - r.left), h = (UINT)(r.bottom - r.top);
        std::snprintf(line, sizeof(line), "  %ux%u at (%ld, %ld)\n", w, h, r.left, r.top);
        report += line;
        actual.emplace_back(w, h);
    }
    report += "\n";

    if (!actual.empty()) DescribeLayout(report, "Actual layout", actual);

    // Arrangements worth proving the arithmetic against. A crop supplying 100%
    // of a view's native width means mirroring costs it no resolution.
    DescribeLayout(report, "Hypothetical: ultrawide + two 1080p",
                   { {3440,1440}, {1920,1080}, {1920,1080} });
    DescribeLayout(report, "Hypothetical: ultrawide + portrait",
                   { {3440,1440}, {1080,1920} });
    DescribeLayout(report, "Hypothetical: three identical 1440p",
                   { {2560,1440}, {2560,1440}, {2560,1440} });
    DescribeLayout(report, "Hypothetical: 4K + 720p laptop",
                   { {3840,2160}, {1366,768} });

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(file, report.data(), (DWORD)report.size(), &written, nullptr);
    CloseHandle(file);
    return true;
}

static void ShowConfigDialog(HINSTANCE instance, HWND parent)
{
    Settings settings = Settings::load();
    ShowSettingsDialog(instance, parent, settings);
}

// Accepts "/p 1234", "/p:1234" and "-p1234" alike, since the exact spelling
// varies between Windows versions and third-party preview hosts.
static HWND ParseWindowHandle(const std::wstring& tail)
{
    const size_t start = tail.find_first_not_of(L" \t:");
    if (start == std::wstring::npos) return nullptr;
    return (HWND)(uintptr_t)_wcstoui64(tail.c_str() + start, nullptr, 10);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR commandLine, int)
{
    EnableDpiAwareness();

    std::wstring line(commandLine ? commandLine : L"");
    const size_t begin = line.find_first_not_of(L" \t");
    line = (begin == std::wstring::npos) ? L"" : line.substr(begin);

    // Split into tokens up front. The whole-word development commands have to
    // be matched before the single-letter screensaver flags, because "/probe"
    // and "/capture" begin with "/p" and "/c" and would otherwise be swallowed
    // by the preview and configure branches.
    std::vector<std::wstring> tokens;
    {
        std::wstring current;
        for (wchar_t ch : line + L" ")
        {
            if (ch == L' ' || ch == L'\t')
            {
                if (!current.empty()) { tokens.push_back(current); current.clear(); }
            }
            else current += ch;
        }
    }

    // Modifiers may appear anywhere, in any order, with or without a mode.
    //
    // They used to be looked for only after the mode, and the mode itself was
    // taken from the first two characters of the raw command line - so
    // "/free /forced" read as the flag "/f", matched no mode, and fell through
    // to the settings dialog. Silently opening the wrong thing is a poor answer
    // to a command line that is asking for something perfectly clear.
    //
    // They are pulled out of the list entirely rather than skipped in place,
    // because everything downstream reads positionally: /capture takes its
    // width from the third token, and a modifier left sitting anywhere before
    // it would quietly shift them all along by one.
    bool  freeRun = false;
    float sustainedForcing = 0.0f;
    std::vector<std::wstring> args;

    for (const std::wstring& token : tokens)
    {
        std::wstring modifier = token;
        std::transform(modifier.begin(), modifier.end(), modifier.begin(), ::towlower);

        if (modifier == L"/free" || modifier == L"-free")
        {
            freeRun = true;
            continue;
        }

        // /forced, or /forced:2.5 to say how hard. It keeps the storm running
        // too - forcing a domain that is reset five seconds later is pointless
        // - so it implies /free.
        if (modifier.rfind(L"/forced", 0) == 0 || modifier.rfind(L"-forced", 0) == 0)
        {
            freeRun = true;
            const size_t colon = modifier.find_first_of(L":=");
            // A multiple of the arc's peak forcing, not a fraction of it, and
            // it has to be: a storm from rest also gets a two-kelvin bubble in
            // its initial condition, and a rate has to exceed what the arc ever
            // asks for to stand in for one. Below about 2 nothing condenses at
            // all - the thermals rise to the condensation level and stop on it.
            sustainedForcing = (colon == std::wstring::npos)
                             ? 3.5f
                             : (float)_wtof(modifier.c_str() + colon + 1);
            sustainedForcing = std::min(std::max(sustainedForcing, 0.0f), 6.0f);
            continue;
        }

        args.push_back(token);
    }

    std::wstring command = args.empty() ? L"" : args[0];
    std::transform(command.begin(), command.end(), command.begin(), ::towlower);

    if (command == L"/capture" || command == L"-capture")
    {
        // /capture <file.bmp> [w h] [seconds] [camera distance m] [aim height m]
        // ... and /free, to capture past where the storm would have reset.
        if (args.size() < 2) return 1;
        const UINT  w = (args.size() > 2) ? (UINT)_wtoi(args[2].c_str()) : 1720u;
        const UINT  h = (args.size() > 3) ? (UINT)_wtoi(args[3].c_str()) : 720u;
        const float t = (args.size() > 4) ? (float)_wtof(args[4].c_str()) : 0.0f;
        const float d = (args.size() > 5) ? (float)_wtof(args[5].c_str()) : 0.0f;
        const float a = (args.size() > 6) ? (float)_wtof(args[6].c_str()) : 0.0f;
        return App::captureFrame(w, h, t, args[1].c_str(), false, d, a, freeRun,
                                 sustainedForcing) ? 0 : 1;
    }

    if (command == L"/bench" || command == L"-bench")
    {
        // /bench <file.txt> [width height] [frames] [seconds to warm up to]
        const wchar_t* out = (args.size() > 1) ? args[1].c_str() : L"storm-bench.txt";
        const UINT w = (args.size() > 2) ? (UINT)_wtoi(args[2].c_str()) : 3440u;
        const UINT h = (args.size() > 3) ? (UINT)_wtoi(args[3].c_str()) : 1440u;
        const int  n = (args.size() > 4) ? _wtoi(args[4].c_str()) : 120;
        const float t = (args.size() > 5) ? (float)_wtof(args[5].c_str()) : 0.0f;
        return App::benchmark(w, h, n, out, t) ? 0 : 1;
    }

    if (command == L"/slice" || command == L"-slice")
    {
        // /slice <file.bmp> [width height] [seconds] - the fields, not the sky
        if (args.size() < 2) return 1;
        const UINT  w = (args.size() > 2) ? (UINT)_wtoi(args[2].c_str()) : 1280u;
        const UINT  h = (args.size() > 3) ? (UINT)_wtoi(args[3].c_str()) : 720u;
        const float t = (args.size() > 4) ? (float)_wtof(args[4].c_str()) : 0.0f;
        return App::captureFrame(w, h, t, args[1].c_str(), true, 0.0f, 0.0f,
                                 freeRun, sustainedForcing) ? 0 : 1;
    }

    if (command == L"/arc" || command == L"-arc")
    {
        // /arc <file.csv> [storm s] [interval] [EL m] [rotation m/s] [shear m/s/km]
        const wchar_t* out = (args.size() > 1) ? args[1].c_str() : L"storm-arc.csv";
        const float total = (args.size() > 2) ? (float)_wtof(args[2].c_str()) : 900.0f;
        const float every = (args.size() > 3) ? (float)_wtof(args[3].c_str()) : 10.0f;
        const float el    = (args.size() > 4) ? (float)_wtof(args[4].c_str()) : 0.0f;
        const float rot   = (args.size() > 5) ? (float)_wtof(args[5].c_str()) : -1.0f;
        const float shear = (args.size() > 6) ? (float)_wtof(args[6].c_str()) : -1.0f;
        return App::arcReport(out, total, every, el, rot, shear, sustainedForcing) ? 0 : 1;
    }

    if (command == L"/probe" || command == L"-probe")
    {
        const wchar_t* out = (args.size() > 1) ? args[1].c_str() : L"storm-probe.txt";
        return WriteProbeReport(out) ? 0 : 1;
    }

    // The mode is the first token that is not a modifier. Taking it from the
    // raw line is what broke "/free /forced"; taking it from the tokens also
    // means /p can be read out of its own token rather than out of a substring
    // that a modifier might have shifted.
    const std::wstring modeToken = args.empty() ? L"" : args[0];
    std::wstring flag;
    if (modeToken.length() >= 2 && (modeToken[0] == L'/' || modeToken[0] == L'-'))
    {
        flag = modeToken.substr(0, 2);
        std::transform(flag.begin(), flag.end(), flag.begin(), ::towlower);
    }

    // Modifiers with no mode at all. Windows never does this, so it is someone
    // at a prompt who wants to watch the thing: give them a window rather than
    // taking over the display, and leave full screen one keystroke away as
    // "/s /free". A completely bare command line still means Settings, because
    // that is the form Windows uses when the user picks it.
    if (flag.empty() && args.empty() && !tokens.empty())
    {
        App app;
        if (!app.initialise(instance, Mode::Windowed, nullptr, freeRun, sustainedForcing))
        { app.shutdown(); return 1; }
        const int result = app.run();
        app.shutdown();
        return result;
    }

    if (flag == L"/s" || flag == L"-s")
    {
        KillOtherInstances();
        App app;
        if (!app.initialise(instance, Mode::FullScreen, nullptr, freeRun, sustainedForcing))
        { app.shutdown(); return 1; }
        const int result = app.run();
        app.shutdown();
        return result;
    }

    if (flag == L"/p" || flag == L"-p")
    {
        std::wstring tail = (modeToken.length() > 2) ? modeToken.substr(2) : L"";
        if (tail.find_first_of(L"0123456789") == std::wstring::npos && args.size() > 1)
            tail = args[1];
        HWND preview = ParseWindowHandle(tail);
        if (!preview || !IsWindow(preview)) return 0;

        KillOtherInstances();
        App app;
        if (!app.initialise(instance, Mode::Preview, preview)) { app.shutdown(); return 1; }
        const int result = app.run();
        app.shutdown();
        return result;
    }

    // Development-only switches. Windows never passes these, so they cannot
    // collide with the screensaver contract.
    if (flag == L"/w" || flag == L"-w")
    {
        App app;
        if (!app.initialise(instance, Mode::Windowed, nullptr, freeRun, sustainedForcing))
        { app.shutdown(); return 1; }
        const int result = app.run();
        app.shutdown();
        return result;
    }

    // /c, /c:HWND, or no arguments at all - Windows uses the bare form when the
    // user picks Settings.
    HWND parent = nullptr;
    if (modeToken.length() > 2 && modeToken[2] == L':')
        parent = ParseWindowHandle(modeToken.substr(2));
    ShowConfigDialog(instance, parent);
    return 0;
}
