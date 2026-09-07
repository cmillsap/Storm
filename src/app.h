// Storm - the screensaver application: windows, input, and the frame loop.
#pragma once

#include "renderer.h"
#include "settings.h"
#include <vector>

enum class Mode
{
    FullScreen,   // /s
    Preview,      // /p <hwnd>
    Windowed,     // /w - development only, never passed by Windows
};

class App
{
public:
    // freeRun is /free: never reset the storm, and watch what is left of it
    // from an orbiting camera once the arc has run out.
    bool initialise(HINSTANCE instance, Mode mode, HWND previewWindow, bool freeRun = false,
                    float sustainedForcing = 0.0f);
    int  run();
    void shutdown();

    // Renders one frame to a BMP without creating a swap chain. A screensaver
    // takes over the display, which makes it almost impossible to inspect while
    // developing; the spikes established that looking at the image is the only
    // way to catch a renderer that is fast and wrong.
    // distance, when positive, moves the camera that many metres from the
    // storm axis instead of the shipped 18 km and re-aims it. Development
    // only: the camera that ships is Phase 05's, and a tornado is two degrees
    // wide from 18 km, which is honest and useless for looking at one.
    static bool captureFrame(UINT width, UINT height, float atTime, const wchar_t* path,
                             bool crossSection = false, float distance = 0.0f,
                             float aimHeight = 0.0f, bool freeRun = false,
                             float sustainedForcing = 0.0f);

    // Runs the simulation headless and writes one CSV row per sampled interval
    // of storm time: cloud base and top, peak updraft and downdraft, condensate
    // and rain mass, cloud radius. No window, no render passes.
    //
    // The storm arc is a set of numbers before it is a picture - cloud top has
    // to track the prescribed equilibrium level, the acts have to change the
    // storm when they say they do - and a capture cannot measure any of that.
    // This is the instrument the whole phase is calibrated on.
    static bool arcReport(const wchar_t* path, float stormSeconds, float sampleSeconds,
                          float equilibrium = 0.0f, float rotation = -1.0f,
                          float shear = -1.0f, float sustainedForcing = 0.0f);

    // Times the render pipeline with no window and no present. Wall clock
    // around a fully synchronised frame, so it is an upper bound that includes
    // CPU submission - every phase from here on wants this number.
    static bool benchmark(UINT width, UINT height, int frames, const wchar_t* path,
                          float atTime = 0.0f);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static BOOL CALLBACK MonitorCallback(HMONITOR monitor, HDC, LPRECT rect, LPARAM param);

    bool createFullScreenViews();
    bool createPreviewView(HWND previewWindow);
    bool createWindowedView();
    bool isOwnWindow(HWND hwnd) const;
    void onInputActivity(bool force);
    void onMouseMove(POINT screenPosition);

    static App* s_instance;

    HINSTANCE m_instance = nullptr;
    Mode      m_mode = Mode::FullScreen;
    bool      m_freeRun = false;
    float     m_sustainedForcing = 0.0f;
    bool      m_running = false;

    Gpu       m_gpu;
    Renderer  m_renderer;
    std::vector<View> m_views;
    std::vector<RECT> m_monitorRects;   // scratch for enumeration

    // Exit conditions. A screensaver has to be twitchy about real input and
    // completely deaf to its own windows swapping focus during creation.
    POINT m_firstMouse = {};
    bool  m_mouseSeen = false;
    DWORD m_startTick = 0;
    static const int  kMouseThreshold = 5;
    static const DWORD kInputGraceMs = 900;   // ignore input while windows settle

    // A screensaver that pins a GPU overnight is antisocial in a way a game is
    // not, so frames are capped rather than left to run at the refresh rate.
    // Presenting every swap chain with SyncInterval 0 and pacing here also
    // avoids serialising on several monitors' vblanks in turn. The cap is a
    // setting now; this is what it defaults to.
    static const int kTargetFps = 30;

    Settings m_settings;
    // Moves the quality tier when the settings say Automatic. Hysteresis is
    // the whole design: a tier that changes on a single slow frame oscillates,
    // and an oscillating tier is more visible than a low one.
    void adaptQuality(float frameMilliseconds);
    float m_smoothedFrame = 0.0f;
    float m_tierHold = 0.0f;

    // Signalled by a newly launched instance so a running preview releases its
    // swap chain before the new one tries to create another on the same HWND.
    HANDLE m_exitEvent = nullptr;
};
