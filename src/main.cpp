// Storm - entry point and the .scr command-line contract.
//
// Windows launches a screensaver with /s to run, /c to configure and /p with a
// window handle to draw the little preview in the Settings dialog. Both /c and
// /p can arrive with the handle attached by a colon.

#include "app.h"

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

static void ShowConfigDialog(HWND parent)
{
    MessageBoxW(parent,
        L"Storm " STORM_VERSION_TEXT L"\n"
        L"A volumetric supercell, as a screensaver.\n\n"
        L"Phase 00: shell and skeleton. The sky is a real Rayleigh/Mie "
        L"atmosphere with a sun on a five-minute arc; clouds, the storm and "
        L"the tornado arrive in later phases.\n\n"
        L"Settings will live here once there is something worth setting.",
        L"Storm", MB_OK | MB_ICONINFORMATION);
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

    std::wstring command = tokens.empty() ? L"" : tokens[0];
    std::transform(command.begin(), command.end(), command.begin(), ::towlower);

    if (command == L"/capture" || command == L"-capture")
    {
        // /capture <file.bmp> [width height] [seconds]
        if (tokens.size() < 2) return 1;
        const UINT  w = (tokens.size() > 2) ? (UINT)_wtoi(tokens[2].c_str()) : 1720u;
        const UINT  h = (tokens.size() > 3) ? (UINT)_wtoi(tokens[3].c_str()) : 720u;
        const float t = (tokens.size() > 4) ? (float)_wtof(tokens[4].c_str()) : 0.0f;
        return App::captureFrame(w, h, t, tokens[1].c_str()) ? 0 : 1;
    }

    if (command == L"/bench" || command == L"-bench")
    {
        // /bench <file.txt> [width height] [frames] [seconds to warm up to]
        const wchar_t* out = (tokens.size() > 1) ? tokens[1].c_str() : L"storm-bench.txt";
        const UINT w = (tokens.size() > 2) ? (UINT)_wtoi(tokens[2].c_str()) : 3440u;
        const UINT h = (tokens.size() > 3) ? (UINT)_wtoi(tokens[3].c_str()) : 1440u;
        const int  n = (tokens.size() > 4) ? _wtoi(tokens[4].c_str()) : 120;
        const float t = (tokens.size() > 5) ? (float)_wtof(tokens[5].c_str()) : 0.0f;
        return App::benchmark(w, h, n, out, t) ? 0 : 1;
    }

    if (command == L"/slice" || command == L"-slice")
    {
        // /slice <file.bmp> [width height] [seconds] - the fields, not the sky
        if (tokens.size() < 2) return 1;
        const UINT  w = (tokens.size() > 2) ? (UINT)_wtoi(tokens[2].c_str()) : 1280u;
        const UINT  h = (tokens.size() > 3) ? (UINT)_wtoi(tokens[3].c_str()) : 720u;
        const float t = (tokens.size() > 4) ? (float)_wtof(tokens[4].c_str()) : 0.0f;
        return App::captureFrame(w, h, t, tokens[1].c_str(), true) ? 0 : 1;
    }

    if (command == L"/arc" || command == L"-arc")
    {
        // /arc <file.csv> [storm seconds] [sample interval] [equilibrium m]
        const wchar_t* out = (tokens.size() > 1) ? tokens[1].c_str() : L"storm-arc.csv";
        const float total = (tokens.size() > 2) ? (float)_wtof(tokens[2].c_str()) : 900.0f;
        const float every = (tokens.size() > 3) ? (float)_wtof(tokens[3].c_str()) : 10.0f;
        const float el    = (tokens.size() > 4) ? (float)_wtof(tokens[4].c_str()) : 0.0f;
        return App::arcReport(out, total, every, el) ? 0 : 1;
    }

    if (command == L"/probe" || command == L"-probe")
    {
        const wchar_t* out = (tokens.size() > 1) ? tokens[1].c_str() : L"storm-probe.txt";
        return WriteProbeReport(out) ? 0 : 1;
    }

    std::wstring flag;
    if (!line.empty() && (line[0] == L'/' || line[0] == L'-'))
    {
        flag = line.substr(0, 2);
        std::transform(flag.begin(), flag.end(), flag.begin(), ::towlower);
    }

    if (flag == L"/s" || flag == L"-s")
    {
        KillOtherInstances();
        App app;
        if (!app.initialise(instance, Mode::FullScreen, nullptr)) { app.shutdown(); return 1; }
        const int result = app.run();
        app.shutdown();
        return result;
    }

    if (flag == L"/p" || flag == L"-p")
    {
        HWND preview = ParseWindowHandle(line.substr(2));
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
        if (!app.initialise(instance, Mode::Windowed, nullptr)) { app.shutdown(); return 1; }
        const int result = app.run();
        app.shutdown();
        return result;
    }

    // /c, /c:HWND, or no arguments at all - Windows uses the bare form when the
    // user picks Settings.
    HWND parent = nullptr;
    if (line.length() > 2 && line[2] == L':') parent = ParseWindowHandle(line.substr(2));
    ShowConfigDialog(parent);
    return 0;
}
