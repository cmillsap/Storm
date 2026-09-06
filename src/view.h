// Storm - one output. A View is a window, a swap chain, and the rectangle of a
// shared render target that it shows.
//
// This is the whole multi-monitor decision, expressed as a type. A View owns no
// scene state: it holds a camera and a pointer to the target it presents.
// Mirroring is N views sharing one target and one camera; independent cameras
// are the same loop with the sharing removed. Nothing above this layer knows
// which mode is in effect, so the choice stays a policy rather than an
// assumption baked through the renderer.
#pragma once

#include "gpu.h"

struct Camera
{
    // Framed on the storm, not on the cumulus it starts as. Phase 03 grew the
    // domain to 14.4 km deep, and a cumulonimbus filling it subtends 44 degrees
    // vertically from 18 km out - which is why the camera stands that far back
    // rather than the 7 km Phase 02 used. The pitch splits the difference
    // between the base at 900 m and an anvil at 11 km.
    //
    // Fixed. The camera spline that pushes in for the cumulus act and pulls
    // back for the anvil is Phase 05's; until then one framing has to hold the
    // whole arc, and it is the mature storm that sets it.
    float position[3] = { 0.0f, 60.0f, 0.0f };
    // The slow oscillation the frame loop applies is added to this rather than
    // replacing it, so a camera that has been aimed somewhere stays aimed.
    float baseYaw = 0.0f;
    float yaw   = 0.0f;
    float pitch = 0.26f;
    float fovDegrees = 55.0f;
};

// A render target shared by one or more views.
struct RenderTarget
{
    ComPtr<ID3D12Resource> texture;
    UINT width = 0, height = 0;
    UINT uavIndex = 0;        // slot in Gpu::srvHeap
    UINT srvIndex = 0;
    Camera camera;            // the camera this target was rendered with

    float aspect() const { return (float)width / (float)height; }
};

static const UINT kBackBufferCount = 2;

struct View
{
    HWND hwnd = nullptr;
    bool ownsWindow = false;          // false in /p preview, where Windows owns it
    UINT width = 0, height = 0;

    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12Resource>  backBuffers[kBackBufferCount];
    UINT rtvIndex[kBackBufferCount] = {};

    const RenderTarget* source = nullptr;   // not owned

    bool create(Gpu& gpu, HWND window, UINT w, UINT h);
    void release();

    float aspect() const { return (float)width / (float)height; }

    // Crop-to-fill mapping from the shared target into this view.
    //
    // Displays disagree about shape, and the same frame shown on a differently
    // proportioned panel forces a choice. Letterboxing looks broken on a
    // screensaver and stretching distorts a sky that has a horizon in it, so
    // the view samples the largest sub-rectangle of the target that matches its
    // own aspect. Writes uv scale and offset.
    void cropToFill(float outScale[2], float outOffset[2]) const;
};
