#include "app.h"

#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <algorithm>
#include <cmath>

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

bool App::initialise(HINSTANCE instance, Mode mode, HWND previewWindow)
{
    s_instance = this;
    m_instance = instance;
    m_mode = mode;
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

    // Mirroring, stated once: every view presents the same target and shares
    // its camera. Independent cameras would give each view its own target here
    // and nothing else in the frame loop would change.
    for (View& v : m_views) v.source = &m_renderer.targets[0];

    m_exitEvent = OpenEventW(SYNCHRONIZE, FALSE, kExitEventName);
    m_running = true;
    return true;
}

// ---------------------------------------------------------------- frame loop

int App::run()
{
    LARGE_INTEGER frequency, start;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);

    const double frameSeconds = 1.0 / (double)kTargetFps;
    LARGE_INTEGER nextFrame = start;

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

        m_gpu.beginFrame();
        m_renderer.renderTargets(elapsed);
        for (View& v : m_views) m_renderer.presentView(v, elapsed);
        m_renderer.finishFrame();
        m_gpu.submitAndWait();

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

bool App::captureFrame(UINT width, UINT height, float atTime, const wchar_t* path)
{
    Gpu gpu;
    if (!gpu.initialise(false)) { FailHard("No Direct3D 12 capable adapter found."); return false; }

    Renderer renderer;
    if (!renderer.initialise(gpu)) return false;
    if (!renderer.createTarget(width, height)) return false;

    RenderTarget& target = renderer.targets[0];

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
    renderer.renderTargets(atTime);      // leaves the target readable by a shader
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
