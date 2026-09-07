#include "app.h"

#include "../resource.h"

#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

App* App::s_instance = nullptr;

static const wchar_t* kWindowClass = L"StormScreenSaver";
extern const wchar_t* kExitEventName;   // defined in main.cpp

// -------------------------------------------------------------- window class

LRESULT CALLBACK App::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    App* app = s_instance;

    switch (msg)
    {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (app) app->onInputActivity(false);
        return 0;

    case WM_MOUSEMOVE:
        if (app && app->m_mode == Mode::FullScreen)
        {
            // Screen coordinates, so movement is tracked consistently no matter
            // which monitor's window reports it.
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &p);
            app->onMouseMove(p);
        }
        return 0;

    case WM_ACTIVATE:
        // Exit when focus leaves for something that is not ours. Storm creates
        // one window per monitor, so during startup they hand activation back
        // and forth; treating every WA_INACTIVE as user activity would make a
        // multi-monitor machine exit instantly.
        if (app && app->m_mode == Mode::FullScreen && LOWORD(wp) == WA_INACTIVE)
        {
            HWND other = (HWND)lp;
            if (!app->isOwnWindow(other)) app->onInputActivity(true);
        }
        return 0;

    case WM_SETCURSOR:
        SetCursor(nullptr);
        return TRUE;

    case WM_CLOSE:
    case WM_DESTROY:
        if (app) app->m_running = false;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL CALLBACK App::MonitorCallback(HMONITOR, HDC, LPRECT rect, LPARAM param)
{
    auto* rects = reinterpret_cast<std::vector<RECT>*>(param);
    rects->push_back(*rect);
    return TRUE;
}

bool App::isOwnWindow(HWND hwnd) const
{
    if (!hwnd) return false;
    for (const View& v : m_views)
        if (v.hwnd == hwnd) return true;
    return false;
}

void App::onInputActivity(bool force)
{
    // Windows sends spurious activation and mouse traffic while the windows are
    // being created. Without a grace period the screensaver exits before the
    // first frame reaches the screen.
    if (!force && GetTickCount() - m_startTick < kInputGraceMs) return;
    m_running = false;
}

void App::onMouseMove(POINT p)
{
    if (!m_mouseSeen)
    {
        m_firstMouse = p;
        m_mouseSeen = true;
        return;
    }
    const int dx = p.x - m_firstMouse.x;
    const int dy = p.y - m_firstMouse.y;
    if (dx * dx + dy * dy > kMouseThreshold * kMouseThreshold) onInputActivity(false);
}

// --------------------------------------------------------------------- setup

bool App::createFullScreenViews()
{
    m_monitorRects.clear();
    EnumDisplayMonitors(nullptr, nullptr, MonitorCallback, (LPARAM)&m_monitorRects);
    if (m_monitorRects.empty())
    {
        RECT all = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        m_monitorRects.push_back(all);
    }

    m_views.resize(m_monitorRects.size());
    for (size_t i = 0; i < m_monitorRects.size(); ++i)
    {
        const RECT& r = m_monitorRects[i];
        const int w = r.right - r.left;
        const int h = r.bottom - r.top;

        HWND hwnd = CreateWindowExW(
            WS_EX_TOPMOST, kWindowClass, L"Storm",
            WS_POPUP | WS_VISIBLE,
            r.left, r.top, w, h,
            nullptr, nullptr, m_instance, nullptr);
        if (!hwnd) return false;

        m_views[i].ownsWindow = true;
        if (!m_views[i].create(m_gpu, hwnd, (UINT)w, (UINT)h)) return false;
    }

    // Focus the first window so keyboard input arrives somewhere.
    if (!m_views.empty())
    {
        SetForegroundWindow(m_views[0].hwnd);
        SetFocus(m_views[0].hwnd);
    }
    ShowCursor(FALSE);
    return true;
}

bool App::createWindowedView()
{
    // A normal, ordinary window. Everything else in the frame loop is identical
    // to full screen, which is the point: the development view exercises the
    // real path rather than a parallel one.
    const int w = 1280, h = 536;
    RECT rc = { 0, 0, w, h };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(
        0, kWindowClass, L"Storm - windowed",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, m_instance, nullptr);
    if (!hwnd) return false;

    m_views.resize(1);
    m_views[0].ownsWindow = true;
    return m_views[0].create(m_gpu, hwnd, (UINT)w, (UINT)h);
}

bool App::createPreviewView(HWND previewWindow)
{
    RECT rc = {};
    if (!GetClientRect(previewWindow, &rc)) return false;

    m_views.resize(1);
    m_views[0].ownsWindow = false;
    return m_views[0].create(m_gpu, previewWindow,
                             (UINT)std::max<LONG>(1, rc.right - rc.left),
                             (UINT)std::max<LONG>(1, rc.bottom - rc.top));
}

bool App::initialise(HINSTANCE instance, Mode mode, HWND previewWindow, bool freeRun,
                     float sustainedForcing)
{
    s_instance = this;
    m_instance = instance;
    m_mode = mode;
    m_freeRun = freeRun;
    m_sustainedForcing = sustainedForcing;
    m_startTick = GetTickCount();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = nullptr;
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    if (!m_gpu.initialise(false))
    {
        FailHard("No Direct3D 12 capable graphics adapter was found.\n\n"
                 "Storm needs a GPU supporting feature level 11_0 or better.");
        return false;
    }
    if (!m_renderer.initialise(m_gpu)) return false;

    bool ok = false;
    switch (mode)
    {
    case Mode::Preview:    ok = createPreviewView(previewWindow); break;
    case Mode::Windowed:   ok = createWindowedView();             break;
    case Mode::FullScreen: ok = createFullScreenViews();          break;
    }
    if (!ok) return false;

    if (!m_renderer.createSharedTarget(m_views)) return false;
    m_renderer.generateNoise();

    // Mirroring, stated once: every view presents the same target and shares
    // its camera. Independent cameras would give each view its own target here
    // and nothing else in the frame loop would change.
    for (View& v : m_views) v.source = &m_renderer.targets[0];

    m_exitEvent = OpenEventW(SYNCHRONIZE, FALSE, kExitEventName);
    m_running = true;
    return true;
}

// ---------------------------------------------------------------- frame loop

// The tier is moved on a smoothed frame time, and only after it has been
// wrong for a while. The thresholds are asymmetric on purpose: drop quickly
// when the machine is struggling, restore slowly, because the alternative is a
// picture that visibly breathes.
void App::adaptQuality(float frameMilliseconds)
{
    if (m_settings.quality != Settings::QualityAuto) return;

    m_smoothedFrame = (m_smoothedFrame <= 0.0f)
                    ? frameMilliseconds
                    : m_smoothedFrame * 0.94f + frameMilliseconds * 0.06f;

    const float budget = 1000.0f / (float)m_settings.frameCap;
    m_tierHold += frameMilliseconds * 0.001f;
    if (m_tierHold < 2.0f) return;

    const int was = m_renderer.qualityTier;
    if (m_smoothedFrame > budget * 0.85f && m_renderer.qualityTier < 2)
        ++m_renderer.qualityTier;
    else if (m_smoothedFrame < budget * 0.45f && m_renderer.qualityTier > 0)
        --m_renderer.qualityTier;

    if (m_renderer.qualityTier != was) { m_tierHold = 0.0f; m_smoothedFrame = budget * 0.6f; }
}

int App::run()
{
    m_settings = Settings::load();
    m_renderer.cycleStorms = m_settings.cycleStorms && !m_freeRun;
    m_renderer.freeRun = m_freeRun;
    m_renderer.sustainedForcing = m_sustainedForcing;
    m_renderer.simulation.sustain(m_sustainedForcing);

    // On battery, whatever the settings say: half the frame rate and never the
    // top tier. This is the "power and thermals" risk the plan has carried
    // open since the beginning, and it is the cheap half of the answer.
    int frameCap = m_settings.frameCap;
    SYSTEM_POWER_STATUS power = {};
    const bool onBattery = m_settings.respectBattery
                        && GetSystemPowerStatus(&power)
                        && power.ACLineStatus == 0;
    if (onBattery) frameCap = std::max(15, frameCap / 2);

    m_renderer.qualityTier = (m_settings.quality == Settings::QualityAuto)
                           ? (onBattery ? 1 : 0)
                           : m_settings.quality - 1;
    if (onBattery) m_renderer.qualityTier = std::max(m_renderer.qualityTier, 1);

    // The preview is a thumbnail in the Screen Saver dialog - a couple of
    // hundred pixels across, behind a modal window the user is about to close.
    // It gets the cheap path and a slow frame rate, and never adapts: the
    // adaptation would be measuring a window nobody is looking at.
    if (m_mode == Mode::Preview)
    {
        m_renderer.qualityTier = 2;
        m_settings.quality = Settings::QualityLow;
        frameCap = 15;
    }

    LARGE_INTEGER frequency, start;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    const double frameSeconds = 1.0 / (double)frameCap;
    LARGE_INTEGER nextFrame = start;
    float lastFrameTime = 0.0f;

    MSG msg = {};
    while (m_running)
    {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { m_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!m_running) break;

        // A newly launched instance asks a running preview to stand down, so it
        // can create a swap chain on the same window.
        if (m_exitEvent && WaitForSingleObject(m_exitEvent, 0) == WAIT_OBJECT_0) break;

        // In preview the host window can vanish without notifying us.
        if (m_mode == Mode::Preview && !IsWindow(m_views[0].hwnd)) break;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const float elapsed =
            (float)((double)(now.QuadPart - start.QuadPart) / (double)frequency.QuadPart);

        const float delta = elapsed - lastFrameTime;
        lastFrameTime = elapsed;

        // Stop drawing once the user has plainly gone home. The last frame
        // stays on screen - the swap chain keeps it - and the GPU goes quiet,
        // which is the other half of the power answer and the only part of it
        // that helps at four in the morning.
        const bool goneHome = m_settings.idleMinutes > 0
                           && elapsed > (float)m_settings.idleMinutes * 60.0f;
        if (goneHome)
        {
            Sleep(250);
            continue;
        }

        LARGE_INTEGER frameStart;
        QueryPerformanceCounter(&frameStart);

        m_gpu.beginFrame();
        m_renderer.renderTargets(elapsed, delta);
        for (View& v : m_views) m_renderer.presentView(v);
        m_renderer.finishFrame();
        m_gpu.submitAndWait();

        QueryPerformanceCounter(&now);
        adaptQuality((float)((double)(now.QuadPart - frameStart.QuadPart) * 1000.0
                             / (double)frequency.QuadPart));

        for (View& v : m_views)
            if (v.swapChain) v.swapChain->Present(0, 0);

        // Pace to the frame cap: sleep the bulk of the remaining time, then
        // spin the last millisecond, which keeps the cadence steady without
        // raising the system timer resolution process-wide.
        nextFrame.QuadPart += (LONGLONG)(frameSeconds * (double)frequency.QuadPart);
        QueryPerformanceCounter(&now);
        if (now.QuadPart > nextFrame.QuadPart) nextFrame = now;   // we fell behind
        for (;;)
        {
            QueryPerformanceCounter(&now);
            const double remaining =
                (double)(nextFrame.QuadPart - now.QuadPart) / (double)frequency.QuadPart;
            if (remaining <= 0.0) break;
            if (remaining > 0.002) Sleep(1); else Sleep(0);
        }
    }
    return 0;
}

// ------------------------------------------------------------------ capture

bool App::captureFrame(UINT width, UINT height, float atTime, const wchar_t* path,
                       bool crossSection, float distance, float aimHeight, bool freeRun,
                       float sustainedForcing)
{
    Gpu gpu;
    if (!gpu.initialise(false)) { FailHard("No Direct3D 12 capable adapter found."); return false; }

    Renderer renderer;
    if (!renderer.initialise(gpu)) return false;
    if (!renderer.createTarget(width, height)) return false;
    renderer.generateNoise();

    RenderTarget& target = renderer.targets[0];

    renderer.freeRun = freeRun;
    if (freeRun) renderer.cycleStorms = false;
    renderer.sustainedForcing = sustainedForcing;
    renderer.simulation.sustain(sustainedForcing);

    if (distance > 0.0f)
    {
        renderer.directorEnabled = false;
        // Stand off the tornado on the inflow side - upstream of the storm,
        // which is the quadrant a supercell keeps free of rain - and aim at it.
        Simulation& sim = renderer.simulation;
        const float axisX = sim.centre(0) + sim.sounding.forceOffset[0]
                          + sim.sounding.rotationTilt * sim.sounding.rotationBase;
        const float axisZ = sim.centre(2) + sim.sounding.forceOffset[1];

        const float bearing = 0.95f;    // radians, round to the upshear side
        const float ex = -std::sin(bearing) * distance;
        const float ez = -std::cos(bearing) * distance;
        target.camera.position[0] = axisX + ex;
        target.camera.position[2] = axisZ + ez;

        const float aim = (aimHeight > 0.0f) ? aimHeight : 2500.0f;
        target.camera.baseYaw = std::atan2(-ex, -ez);
        target.camera.pitch = std::atan2(aim - target.camera.position[1], distance);
    }

    // The temporal resolve accumulates jittered samples, so a single frame is
    // noisier than what the screensaver actually shows. Run up to the requested
    // moment at the real frame rate rather than holding time still: with a
    // frozen camera the reprojection is an identity transform and a capture
    // would prove nothing about it. Arriving with the camera in motion is what
    // makes ghosting visible if it is there.
    // Run the whole history up to the requested moment at the real frame rate.
    // The simulation has no way to jump to a state - a cloud has to be grown -
    // and holding time still would leave the temporal reprojection an identity
    // transform, so a capture would prove nothing about it either.
    const float step = 1.0f / 30.0f;
    // Generous, because a storm is about 145 seconds and the interesting
    // question is often what the second or third one looks like.
    const int frames = std::max(30, std::min(24000, (int)(atTime / step)));
    for (int i = 0; i < frames; ++i)
    {
        gpu.beginFrame();
        renderer.renderTargets((float)i * step, step);
        renderer.finishFrame();
        gpu.submitAndWait();
    }

    D3D12_RESOURCE_DESC desc = target.texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0; UINT64 rowBytes = 0, totalBytes = 0;
    gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = totalBytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES readbackHeap = {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    readbackHeap.CreationNodeMask = 1;
    readbackHeap.VisibleNodeMask = 1;

    ComPtr<ID3D12Resource> readback;
    STORM_CHECK(gpu.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&readback)), "capture readback");

    gpu.beginFrame();
    // Both paths leave the target readable by a shader.
    if (crossSection)
    {
        renderer.simulation.advance(1.0f / 30.0f);
        renderer.renderCrossSection(atTime);
    }
    else
    {
        renderer.renderTargets(atTime, 1.0f / 30.0f);
    }
    auto toCopy = Gpu::transition(target.texture.Get(),
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu.cmd->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = target.texture.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    gpu.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    gpu.submitAndWait();

    const uint8_t* pixels = nullptr;
    D3D12_RANGE readRange = { 0, (SIZE_T)totalBytes };
    STORM_CHECK(readback->Map(0, &readRange, (void**)&pixels), "map capture readback");

    const int w = (int)target.width, h = (int)target.height;
    const int rowPadded = ((w * 3) + 3) & ~3;
    std::vector<uint8_t> image((size_t)rowPadded * h, 0);
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* s = pixels + (size_t)y * footprint.Footprint.RowPitch;
        uint8_t* d = image.data() + (size_t)(h - 1 - y) * rowPadded;   // BMP is bottom-up
        for (int x = 0; x < w; ++x)
        {
            d[x * 3 + 0] = s[x * 4 + 2];
            d[x * 3 + 1] = s[x * 4 + 1];
            d[x * 3 + 2] = s[x * 4 + 0];
        }
    }
    D3D12_RANGE noWrite = { 0, 0 };
    readback->Unmap(0, &noWrite);

#pragma pack(push, 1)
    struct FileHeader { uint16_t type; uint32_t size; uint16_t r1, r2; uint32_t offset; };
    struct InfoHeader { uint32_t size; int32_t w, h; uint16_t planes, bits;
                        uint32_t compression, imageSize; int32_t xppm, yppm;
                        uint32_t used, important; };
#pragma pack(pop)

    const uint32_t dataBytes = (uint32_t)image.size();
    FileHeader fh = { 0x4D42, 54 + dataBytes, 0, 0, 54 };
    InfoHeader ih = { 40, w, h, 1, 24, 0, dataBytes, 2835, 2835, 0, 0 };

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { FailHard("Could not open the capture file for writing."); return false; }
    DWORD written = 0;
    WriteFile(file, &fh, sizeof(fh), &written, nullptr);
    WriteFile(file, &ih, sizeof(ih), &written, nullptr);
    WriteFile(file, image.data(), (DWORD)image.size(), &written, nullptr);
    CloseHandle(file);

    gpu.shutdown();
    renderer.shutdown();
    return true;
}

bool App::arcReport(const wchar_t* path, float stormSeconds, float sampleSeconds,
                    float equilibrium, float rotation, float shear, float sustainedForcing)
{
    Gpu gpu;
    if (!gpu.initialise(false)) { FailHard("No Direct3D 12 capable adapter found."); return false; }

    // The renderer is created for its root signature and its noise volumes -
    // the forcing pass samples the base noise - but nothing is ever rendered
    // and no target is allocated.
    Renderer renderer;
    if (!renderer.initialise(gpu)) return false;
    renderer.generateNoise();

    Simulation& sim = renderer.simulation;
    // Overriding the equilibrium level from the command line is what makes the
    // cloud-top calibration curve a sweep rather than a rebuild per point.
    if (equilibrium > 0.0f) sim.sounding.equilibrium = equilibrium;
    if (rotation >= 0.0f)   sim.sounding.rotationSpeed = rotation;
    if (shear >= 0.0f)      sim.sounding.shear[0] = shear;
    sim.sustain(sustainedForcing);
    const float interval = 1.0f / (float)sim.stepsPerSecond;
    const int   steps = (int)(stormSeconds / sim.stepSeconds);
    const int   every = std::max(1, (int)(sampleSeconds / sim.stepSeconds));

    std::string csv = "time_s,base_m,top_m,updraft_ms,downdraft_ms,condensate,rain,radius_m,cells,wzeta_corr,peak_zeta\n";

    // Band centres as kilometres, so a column header says what height it is.
    std::string profile = "time_s";
    for (int b = 0; b < SimStats::kBands; ++b)
    {
        char header[32];
        std::snprintf(header, sizeof(header), ",%.1fkm",
                      (sim.origin[1] + ((float)b + 0.5f) * sim.extent(1) / SimStats::kBands) * 0.001f);
        profile += header;
    }
    profile += "\n";

    char row[256];

    for (int i = 0; i <= steps; ++i)
    {
        const bool sample = (i % every) == 0;

        gpu.beginFrame();
        sim.advance(interval);
        if (sample) sim.gatherStats();
        gpu.submitAndWait();

        if (!sample) continue;

        const SimStats s = sim.fetchStats();
        const int written = std::snprintf(row, sizeof(row),
            "%.0f,%.0f,%.0f,%.2f,%.2f,%.3f,%.3f,%.0f,%.0f,%.3f,%.4f\n",
            s.stormTime, s.cloudBase, s.cloudTop, s.updraftMax, s.downdraftMax,
            s.condensate, s.rain, s.radius, s.cloudyCells,
            s.updraftVorticityCorrelation, s.peakVorticity);
        if (written > 0) csv.append(row, (size_t)written);

        std::snprintf(row, sizeof(row), "%.0f", s.stormTime);
        profile += row;
        for (int b = 0; b < SimStats::kBands; ++b)
        {
            std::snprintf(row, sizeof(row), ",%.5f/%.0f", s.peakByBand[b], s.cellsByBand[b]);
            profile += row;
        }
        profile += "\n";
    }

    auto write = [](const wchar_t* to, const std::string& text) -> bool
    {
        HANDLE file = CreateFileW(to, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD bytes = 0;
        WriteFile(file, text.data(), (DWORD)text.size(), &bytes, nullptr);
        CloseHandle(file);
        return true;
    };

    // The profile goes beside the summary rather than into it: 32 more columns
    // would make the file the summary is useful for unreadable.
    std::wstring profilePath(path);
    const size_t dot = profilePath.find_last_of(L'.');
    profilePath.insert(dot == std::wstring::npos ? profilePath.size() : dot, L"-profile");

    const bool ok = write(path, csv) && write(profilePath.c_str(), profile);

    gpu.shutdown();
    renderer.shutdown();
    return ok;
}

bool App::benchmark(UINT width, UINT height, int frames, const wchar_t* path, float atTime)
{
    Gpu gpu;
    if (!gpu.initialise(false)) { FailHard("No Direct3D 12 capable adapter found."); return false; }

    Renderer renderer;
    if (!renderer.initialise(gpu)) return false;
    if (!renderer.createTarget(width, height)) return false;
    renderer.generateNoise();

    // Run the storm up to the requested moment before timing anything. An
    // empty sky is not the case the budget has to survive: a mature storm
    // fills the volume the march crosses, and the frame it costs is the only
    // one worth quoting.
    const float step = 1.0f / 30.0f;
    float clock = 0.0f;
    for (int i = 0; i < (int)(atTime / step); ++i, clock += step)
    {
        gpu.beginFrame();
        renderer.renderTargets(clock, step);
        renderer.finishFrame();
        gpu.submitAndWait();
    }

    auto timeFrames = [&](int count) -> double
    {
        LARGE_INTEGER frequency, start, end;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        for (int i = 0; i < count; ++i)
        {
            gpu.beginFrame();
            renderer.renderTargets(clock + (float)i * step, step);
            renderer.finishFrame();
            gpu.submitAndWait();
        }
        QueryPerformanceCounter(&end);
        return (double)(end.QuadPart - start.QuadPart) * 1000.0
             / (double)frequency.QuadPart / (double)count;
    };

    timeFrames(12);                       // warm up
    const double ms = timeFrames(frames);

    char report[1024];
    const int written = std::snprintf(report, sizeof(report),
        "Storm " STORM_VERSION_ASCII " - render benchmark\n"
        "======================================\n\n"
        "device        %s\n"
        "output        %ux%u\n"
        "cloud march   %ux%u (half resolution)\n"
        "frames        %d\n"
        "warm-up       %.0f s of display time before timing\n\n"
        "frame         %.2f ms  (%.0f fps uncapped)\n"
        "budget        %.0f%% of a 30 fps frame, %.0f%% of a 60 fps frame\n\n"
        "Includes the simulation step, the light volume rebuild, the cloud\n"
        "march, the temporal resolve and the full-resolution composite.\n"
        "Excludes present. The simulation keeps its own 20 Hz, so a frame\n"
        "carries 0.67 of a step at 30 fps and proportionally less above it.\n",
        Narrow(gpu.adapterName.c_str()).c_str(),
        width, height, renderer.halfWidth, renderer.halfHeight, frames, atTime,
        ms, 1000.0 / ms, ms / 33.3 * 100.0, ms / 16.7 * 100.0);

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD bytes = 0;
    WriteFile(file, report, (DWORD)written, &bytes, nullptr);
    CloseHandle(file);

    gpu.shutdown();
    renderer.shutdown();
    return true;
}

void App::shutdown()
{
    m_gpu.shutdown();
    for (View& v : m_views) v.release();
    m_views.clear();
    m_renderer.shutdown();

    if (m_exitEvent) { CloseHandle(m_exitEvent); m_exitEvent = nullptr; }
    if (m_mode == Mode::FullScreen) ShowCursor(TRUE);
    s_instance = nullptr;
}
